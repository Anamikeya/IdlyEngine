#include "engine/animation/animation.h"

#include <ozz/animation/offline/additive_animation_builder.h>
#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/maths/transform.h>
#include <ozz/base/maths/vec_float.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace engine::animation {
namespace {

namespace offline = ozz::animation::offline;

constexpr double max_step = 10;
// ozz blends rotation keys with nlerp, which lags glTF's slerp between keys far apart (0.9 degrees a quarter of the
// way through a quarter turn). Keys are added so no segment turns more than this, where nlerp is within 0.001 degrees.
constexpr double max_segment_turn = 0.2;
constexpr std::size_t max_queued_events = 1024;
constexpr std::size_t max_crossed = 64;

bool finite(std::span<const float> values)
{
	return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

ozz::math::Transform transform_of(const assets::Joint& joint)
{
	ozz::math::Transform t;
	t.translation = ozz::math::Float3(joint.translation[0], joint.translation[1], joint.translation[2]);
	t.rotation = ozz::math::Normalize(
		ozz::math::Quaternion(joint.rotation[0], joint.rotation[1], joint.rotation[2], joint.rotation[3]));
	t.scale = ozz::math::Float3(joint.scale[0], joint.scale[1], joint.scale[2]);
	return t;
}

JointMatrix joint_matrix(const ozz::math::Float4x4& m)
{
	JointMatrix out{};
	for (int c = 0; c < 4; ++c) {
		ozz::math::StorePtrU(m.cols[c], out.data() + 4 * c);
	}
	return out;
}

Matrix multiply(const Matrix& a, const JointMatrix& b)
{
	Matrix out{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			double sum = 0;
			for (int k = 0; k < 4; ++k) {
				sum += a[static_cast<std::size_t>(k * 4 + r)] * static_cast<double>(b[static_cast<std::size_t>(c * 4 + k)]);
			}
			out[static_cast<std::size_t>(c * 4 + r)] = sum;
		}
	}
	return out;
}

// Inverse of an affine matrix; nullopt when it is singular.
std::optional<Matrix> inverse_affine(const Matrix& m)
{
	const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (!std::isfinite(det) || std::abs(det) < 1e-12) {
		return std::nullopt;
	}
	const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
		(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
		(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
	Matrix out = identity;
	// inv is row-major: inv[r * 3 + col].
	for (int r = 0; r < 3; ++r) {
		for (int col = 0; col < 3; ++col) {
			out[static_cast<std::size_t>(col * 4 + r)] = inv[r * 3 + col];
		}
	}
	for (int r = 0; r < 3; ++r) {
		out[static_cast<std::size_t>(12 + r)] = -(out[static_cast<std::size_t>(r)] * m[12] +
			out[static_cast<std::size_t>(4 + r)] * m[13] + out[static_cast<std::size_t>(8 + r)] * m[14]);
	}
	return out;
}

Point transform(const Matrix& m, const Point& p, double w)
{
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] * w, m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] * w,
		m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] * w};
}

using Quat = std::array<double, 4>;

Quat slerp(Quat a, Quat b, double t)
{
	double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	if (dot < 0) {
		dot = -dot;
		for (auto& v : b) {
			v = -v;
		}
	}
	double wa = 1 - t, wb = t;
	if (dot < 0.9999) {
		const double angle = std::acos(std::min(dot, 1.0));
		wa = std::sin((1 - t) * angle) / std::sin(angle);
		wb = std::sin(t * angle) / std::sin(angle);
	}
	Quat out{};
	double length = 0;
	for (std::size_t i = 0; i < 4; ++i) {
		out[i] = wa * a[i] + wb * b[i];
		length += out[i] * out[i];
	}
	length = std::sqrt(length);
	for (auto& v : out) {
		v /= length;
	}
	return out;
}

// Rotation keys as ozz takes them, with keys added between ones far apart so nlerp follows slerp.
void add_rotations(const std::vector<assets::RotationKey>& keys, ozz::vector<offline::RawAnimation::RotationKey>& out)
{
	const auto quat = [](const assets::RotationKey& k) { return Quat{k.value[0], k.value[1], k.value[2], k.value[3]}; };
	const auto push = [&](float time, const Quat& q) {
		out.push_back({time, ozz::math::Normalize(ozz::math::Quaternion(static_cast<float>(q[0]), static_cast<float>(q[1]),
			static_cast<float>(q[2]), static_cast<float>(q[3])))});
	};
	for (std::size_t k = 0; k < keys.size(); ++k) {
		if (k > 0) {
			const Quat a = quat(keys[k - 1]), b = quat(keys[k]);
			const double dot = std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) /
				std::sqrt((a[0] * a[0] + a[1] * a[1] + a[2] * a[2] + a[3] * a[3]) * (b[0] * b[0] + b[1] * b[1] + b[2] * b[2] + b[3] * b[3]));
			const double turn = 2 * std::acos(std::min(dot, 1.0));
			const double t0 = keys[k - 1].time, t1 = keys[k].time;
			const int parts = static_cast<int>(std::ceil(turn / max_segment_turn));
			// A held STEP key (1 ms apart) jumps; keys closer than float time can tell apart are not split either.
			if (parts > 1 && (t1 - t0) / parts > 1e-4 * std::max(1.0, t1)) {
				for (int p = 1; p < parts; ++p) {
					const double f = static_cast<double>(p) / parts;
					push(static_cast<float>(t0 + (t1 - t0) * f), slerp(a, b, f));
				}
			}
		}
		push(keys[k].time, quat(keys[k]));
	}
}

// Multiplies one joint's local rotation, inside its SoA lane, by a correction.
void correct_rotation(ozz::span<ozz::math::SoaTransform> locals, int joint, const ozz::math::SimdQuaternion& correction)
{
	auto& soa = locals[static_cast<std::size_t>(joint / 4)];
	const int lane = joint % 4;
	float x[4], y[4], z[4], w[4], c[4];
	ozz::math::StorePtrU(soa.rotation.x, x);
	ozz::math::StorePtrU(soa.rotation.y, y);
	ozz::math::StorePtrU(soa.rotation.z, z);
	ozz::math::StorePtrU(soa.rotation.w, w);
	ozz::math::StorePtrU(correction.xyzw, c);
	const auto turned = ozz::math::Normalize(
		ozz::math::Quaternion(x[lane], y[lane], z[lane], w[lane]) * ozz::math::Quaternion(c[0], c[1], c[2], c[3]));
	x[lane] = turned.x;
	y[lane] = turned.y;
	z[lane] = turned.z;
	w[lane] = turned.w;
	soa.rotation.x = ozz::math::simd_float4::LoadPtrU(x);
	soa.rotation.y = ozz::math::simd_float4::LoadPtrU(y);
	soa.rotation.z = ozz::math::simd_float4::LoadPtrU(z);
	soa.rotation.w = ozz::math::simd_float4::LoadPtrU(w);
}

double wrap(double time, double duration, bool loop)
{
	if (!(duration > 0)) {
		return 0;
	}
	if (!loop) {
		return std::clamp(time, 0.0, duration);
	}
	double t = std::fmod(time, duration);
	if (t < 0) {
		t += duration;
	}
	return t >= duration ? 0 : t;
}

} // namespace

// ---- Rig ----

struct Rig::Impl {
	ozz::unique_ptr<ozz::animation::Skeleton> skeleton;
	std::vector<assets::Joint> joints; // Ours, in our order.
	std::vector<int> ozz_of;           // Our joint -> ozz joint.
	std::vector<int> ours_of;          // ozz joint -> our joint.
	std::map<std::string, std::size_t, std::less<>> by_name;
	Pose rest;
};

Rig::Rig(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Rig::~Rig() = default;

Result<std::shared_ptr<const Rig>> Rig::build(const assets::MeshSkin& skin)
{
	const auto& joints = skin.joints;
	if (joints.empty()) {
		return fail(ErrorCode::InvalidArgument, "the model has no skeleton");
	}
	if (joints.size() > assets::max_skeleton_joints) {
		return fail(ErrorCode::InvalidArgument, "the skeleton has more than " + std::to_string(assets::max_skeleton_joints) + " joints");
	}
	auto impl = std::make_unique<Impl>();
	std::vector<std::vector<std::size_t>> children(joints.size());
	std::vector<std::size_t> roots;
	for (std::size_t i = 0; i < joints.size(); ++i) {
		const auto& joint = joints[i];
		if (joint.parent < -1 || joint.parent >= static_cast<int>(i)) {
			return fail(ErrorCode::InvalidArgument, "joint '" + joint.name + "' has a parent that does not come before it");
		}
		if (!impl->by_name.emplace(joint.name, i).second) {
			return fail(ErrorCode::InvalidArgument, "two joints are called '" + joint.name + "'");
		}
		const auto& q = joint.rotation;
		const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		if (!finite(joint.translation) || !finite(joint.rotation) || !finite(joint.scale) || !(length > 1e-6f)) {
			return fail(ErrorCode::InvalidArgument, "joint '" + joint.name + "' has a rest transform that is not finite");
		}
		(joint.parent < 0 ? roots : children[static_cast<std::size_t>(joint.parent)]).push_back(i);
	}
	offline::RawSkeleton raw;
	const auto fill = [&](auto& self, offline::RawSkeleton::Joint& out, std::size_t index) -> void {
		out.name = joints[index].name.c_str();
		out.transform = transform_of(joints[index]);
		out.children.resize(children[index].size());
		for (std::size_t c = 0; c < children[index].size(); ++c) {
			self(self, out.children[c], children[index][c]);
		}
	};
	raw.roots.resize(roots.size());
	for (std::size_t r = 0; r < roots.size(); ++r) {
		fill(fill, raw.roots[r], roots[r]);
	}
	if (!raw.Validate()) {
		return fail(ErrorCode::InvalidArgument, "the skeleton cannot be built");
	}
	impl->skeleton = offline::SkeletonBuilder()(raw);
	if (!impl->skeleton || impl->skeleton->num_joints() != static_cast<int>(joints.size())) {
		return fail(ErrorCode::InvalidArgument, "the skeleton cannot be built");
	}
	impl->joints = joints;
	impl->ozz_of.assign(joints.size(), -1);
	impl->ours_of.assign(joints.size(), -1);
	const auto names = impl->skeleton->joint_names();
	for (std::size_t o = 0; o < names.size(); ++o) {
		const std::size_t ours = impl->by_name.at(std::string(names[o]));
		impl->ozz_of[ours] = static_cast<int>(o);
		impl->ours_of[o] = static_cast<int>(ours);
	}
	ozz::vector<ozz::math::Float4x4> models(joints.size());
	ozz::animation::LocalToModelJob job;
	job.skeleton = impl->skeleton.get();
	job.input = impl->skeleton->joint_rest_poses();
	job.output = ozz::make_span(models);
	if (!job.Run()) {
		return fail(ErrorCode::InvalidArgument, "the skeleton's rest pose cannot be computed");
	}
	auto rest = std::make_shared<std::vector<JointMatrix>>(joints.size());
	for (std::size_t i = 0; i < joints.size(); ++i) {
		(*rest)[i] = joint_matrix(models[static_cast<std::size_t>(impl->ozz_of[i])]);
	}
	impl->rest = std::move(rest);
	return std::shared_ptr<const Rig>(new Rig(std::move(impl)));
}

std::size_t Rig::joint_count() const noexcept { return impl_->joints.size(); }
const std::string& Rig::joint_name(std::size_t joint) const { return impl_->joints.at(joint).name; }
int Rig::joint_parent(std::size_t joint) const { return impl_->joints.at(joint).parent; }
std::optional<std::size_t> Rig::find_joint(std::string_view name) const
{
	const auto found = impl_->by_name.find(name);
	return found == impl_->by_name.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}
const Pose& Rig::rest_pose() const noexcept { return impl_->rest; }

// ---- ClipSet ----

struct ClipSet::Impl {
	struct Clip {
		std::string name;
		double duration = 0;
		ozz::unique_ptr<ozz::animation::Animation> animation, additive;
	};
	std::shared_ptr<const Rig> rig;
	std::vector<Clip> clips;
	std::map<std::string, std::size_t, std::less<>> by_name;
	std::size_t matched = 0;
};

ClipSet::ClipSet(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ClipSet::~ClipSet() = default;

Result<std::shared_ptr<const ClipSet>> ClipSet::build(std::shared_ptr<const Rig> rig, const assets::MeshSkin& source,
	std::span<const assets::AnimationClip> clips)
{
	if (!rig) {
		return fail(ErrorCode::InvalidArgument, "clips need a skeleton to play on");
	}
	const auto& r = rig->impl();
	const std::size_t count = r.joints.size();
	std::map<std::string_view, std::size_t> source_index;
	for (std::size_t j = 0; j < source.joints.size(); ++j) {
		source_index.emplace(source.joints[j].name, j);
	}
	// For each ozz joint, the source joint of the same name, or -1.
	std::vector<int> source_of(count, -1);
	auto impl = std::make_unique<Impl>();
	for (std::size_t o = 0; o < count; ++o) {
		const auto found = source_index.find(r.joints[static_cast<std::size_t>(r.ours_of[o])].name);
		if (found != source_index.end()) {
			source_of[o] = static_cast<int>(found->second);
			++impl->matched;
		}
	}
	if (impl->matched == 0) {
		return fail(ErrorCode::InvalidArgument, "no joint of the clips' model has the name of a joint of this skeleton");
	}
	impl->rig = rig;
	for (const auto& clip : clips) {
		if (!std::isfinite(clip.duration) || clip.duration < 0) {
			return fail(ErrorCode::InvalidArgument, "clip '" + clip.name + "' has no valid duration");
		}
		offline::RawAnimation raw;
		raw.name = clip.name.c_str();
		// ozz needs a duration above zero; a single pose has all its keys at 0.
		raw.duration = std::max(clip.duration, 1e-3f);
		raw.tracks.resize(count);
		for (std::size_t o = 0; o < count; ++o) {
			auto& track = raw.tracks[o];
			const auto rest = transform_of(r.joints[static_cast<std::size_t>(r.ours_of[o])]);
			const int s = source_of[o];
			const assets::JointTrack* keys =
				s >= 0 && static_cast<std::size_t>(s) < clip.tracks.size() ? &clip.tracks[static_cast<std::size_t>(s)] : nullptr;
			// An empty ozz track means identity, not rest, so a channel without keys gets one key holding the rest value.
			if (keys && !keys->translation.empty()) {
				for (const auto& k : keys->translation) {
					track.translations.push_back({k.time, ozz::math::Float3(k.value[0], k.value[1], k.value[2])});
				}
			} else {
				track.translations.push_back({0.0f, rest.translation});
			}
			if (keys && !keys->rotation.empty()) {
				add_rotations(keys->rotation, track.rotations);
			} else {
				track.rotations.push_back({0.0f, rest.rotation});
			}
			if (keys && !keys->scale.empty()) {
				for (const auto& k : keys->scale) {
					track.scales.push_back({k.time, ozz::math::Float3(k.value[0], k.value[1], k.value[2])});
				}
			} else {
				track.scales.push_back({0.0f, rest.scale});
			}
		}
		if (!raw.Validate()) {
			return fail(ErrorCode::InvalidArgument, "clip '" + clip.name + "' has keys out of order or outside its duration");
		}
		offline::RawAnimation delta;
		Impl::Clip built;
		built.name = clip.name;
		built.duration = clip.duration;
		built.animation = offline::AnimationBuilder()(raw);
		if (!built.animation || !offline::AdditiveAnimationBuilder()(raw, &delta) ||
			!(built.additive = offline::AnimationBuilder()(delta))) {
			return fail(ErrorCode::InvalidArgument, "clip '" + clip.name + "' cannot be built");
		}
		if (!impl->by_name.emplace(clip.name, impl->clips.size()).second) {
			return fail(ErrorCode::InvalidArgument, "two clips are called '" + clip.name + "'");
		}
		impl->clips.push_back(std::move(built));
	}
	return std::shared_ptr<const ClipSet>(new ClipSet(std::move(impl)));
}

std::size_t ClipSet::size() const noexcept { return impl_->clips.size(); }
const std::string& ClipSet::name(std::size_t clip) const { return impl_->clips.at(clip).name; }
double ClipSet::duration(std::size_t clip) const { return impl_->clips.at(clip).duration; }
std::optional<std::size_t> ClipSet::find(std::string_view name) const
{
	const auto found = impl_->by_name.find(name);
	return found == impl_->by_name.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}
std::size_t ClipSet::matched_joints() const noexcept { return impl_->matched; }
const std::shared_ptr<const Rig>& ClipSet::rig() const noexcept { return impl_->rig; }

// ---- Events ----

std::vector<std::size_t> crossed_events(std::span<const double> times, double from, double delta, double duration, bool loop,
	bool inclusive)
{
	std::vector<std::size_t> out;
	const auto take = [&](std::size_t i) {
		if (out.size() < max_crossed) {
			out.push_back(i);
		}
	};
	if (!(duration > 0) || !std::isfinite(from) || !std::isfinite(delta)) {
		// A single pose: what is at its start fires when it starts.
		if (inclusive) {
			for (std::size_t i = 0; i < times.size(); ++i) {
				if (times[i] <= 0) {
					take(i);
				}
			}
		}
		return out;
	}
	const auto after = [&](double u) { return inclusive ? u >= from : u > from; };
	const auto before = [&](double u) { return inclusive ? u <= from : u < from; };
	if (!loop) {
		const double to = std::clamp(from + delta, 0.0, duration);
		if (delta >= 0) {
			for (std::size_t i = 0; i < times.size(); ++i) {
				if (after(times[i]) && times[i] <= to) {
					take(i);
				}
			}
		} else {
			for (std::size_t i = times.size(); i-- > 0;) {
				if (before(times[i]) && times[i] >= to) {
					take(i);
				}
			}
		}
		return out;
	}
	const double to = from + delta;
	if (delta >= 0) {
		for (double lap = 0; lap <= to && out.size() < max_crossed; lap += duration) {
			for (std::size_t i = 0; i < times.size(); ++i) {
				const double u = times[i] + lap;
				if (after(u) && u <= to) {
					take(i);
				}
			}
		}
	} else {
		for (double lap = 0; lap + duration >= to && out.size() < max_crossed; lap -= duration) {
			for (std::size_t i = times.size(); i-- > 0;) {
				const double u = times[i] + lap;
				if (before(u) && u >= to) {
					take(i);
				}
			}
		}
	}
	return out;
}

// ---- AnimationSystem ----

namespace {

struct Layer {
	int clip = -1;
	double time = 0;
	bool loop = true;
	bool fresh = false; // Started and not advanced yet: an event at its start time still fires.
};

struct Layered {
	int clip = -1;
	double time = 0, weight = 1, blend = 0.1;
	bool fresh = false;
};

struct Chain {
	TwoBoneIK spec;
	int start = 0, middle = 0, end = 0; // ozz joints.
};

struct Buffers {
	ozz::vector<ozz::math::SoaTransform> current, previous, layered, blended;
	ozz::vector<ozz::math::Float4x4> models;
	ozz::animation::SamplingJob::Context current_context, previous_context, layered_context;
	explicit Buffers(const ozz::animation::Skeleton& skeleton)
		: current(static_cast<std::size_t>(skeleton.num_soa_joints())), previous(current.size()), layered(current.size()),
		  blended(current.size()), models(static_cast<std::size_t>(skeleton.num_joints())),
		  current_context(skeleton.num_joints()), previous_context(skeleton.num_joints()), layered_context(skeleton.num_joints())
	{
	}
};

struct Instance {
	std::shared_ptr<const ClipSet> clips;
	std::string authored_clip;
	bool authored_loop = true;
	double authored_start = 0;
	double speed = 1;
	bool playing = true, paused = false;
	Matrix world = identity;
	std::vector<assets::ClipEvent> events;
	// Per clip of the set: event times (ascending) and names.
	std::vector<std::vector<double>> event_times;
	std::vector<std::vector<std::string>> event_names;
	Layer current, previous;
	double fade = 0, fade_elapsed = 0; // A crossfade from `previous` while fade > 0.
	Layered layered;
	std::vector<Chain> chains;
	bool ik_reached = true;
	std::unique_ptr<Buffers> buffers;
	Pose pose;
	Pose previous_pose; // Before the last update, for how fast the pose moves (D7).
	bool dirty = true;
};

const ClipSet::Impl& set_of(const Instance& inst) { return inst.clips->impl(); }
const Rig::Impl& rig_of(const Instance& inst) { return set_of(inst).rig->impl(); }

double clip_duration(const Instance& inst, int clip)
{
	return clip < 0 ? 0 : set_of(inst).clips[static_cast<std::size_t>(clip)].duration;
}

void index_events(Instance& inst)
{
	const auto& set = set_of(inst);
	inst.event_times.assign(set.clips.size(), {});
	inst.event_names.assign(set.clips.size(), {});
	for (const auto& event : inst.events) {
		const auto found = set.by_name.find(event.clip);
		// An event past its clip's end could never be reached.
		if (found == set.by_name.end() || event.time > set.clips[found->second].duration + 1e-4) {
			continue;
		}
		// The asset database keeps events sorted by clip, then time.
		inst.event_times[found->second].push_back(event.time);
		inst.event_names[found->second].push_back(event.name);
	}
}

void reset(Instance& inst)
{
	const auto found = inst.clips->find(inst.authored_clip);
	inst.current = Layer{};
	if (found) {
		inst.current.clip = static_cast<int>(*found);
		inst.current.loop = inst.authored_loop;
		inst.current.time = wrap(inst.authored_start, clip_duration(inst, inst.current.clip), inst.authored_loop);
		inst.current.fresh = true;
	}
	inst.previous = Layer{};
	inst.fade = inst.fade_elapsed = 0;
	inst.layered = Layered{};
	inst.paused = false;
	inst.dirty = true;
}

double layered_envelope(const Instance& inst, double duration)
{
	const auto& l = inst.layered;
	if (!(l.blend > 0)) {
		return 1;
	}
	return std::clamp(std::min(l.time / l.blend, (duration - l.time) / l.blend), 0.0, 1.0);
}

} // namespace

struct AnimationSystem::Impl {
	std::map<std::uint64_t, Instance> instances;
	std::vector<AnimationEvent> events;

	void queue(std::uint64_t entity, const Instance& inst, int clip, const std::vector<std::size_t>& crossed)
	{
		for (const std::size_t i : crossed) {
			const auto c = static_cast<std::size_t>(clip);
			events.push_back({entity, set_of(inst).clips[c].name, inst.event_names[c][i], inst.event_times[c][i]});
		}
		if (events.size() > max_queued_events) {
			events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(events.size() - max_queued_events));
		}
	}

	void advance(std::uint64_t entity, Instance& inst, double dt)
	{
		const double step = dt * inst.speed;
		if (auto& c = inst.current; c.clip >= 0) {
			const double duration = clip_duration(inst, c.clip);
			queue(entity, inst, c.clip,
				crossed_events(inst.event_times[static_cast<std::size_t>(c.clip)], c.time, step, duration, c.loop, c.fresh));
			c.time = wrap(c.time + step, duration, c.loop);
			c.fresh = false;
		}
		if (inst.fade > 0) {
			auto& p = inst.previous;
			p.time = wrap(p.time + step, clip_duration(inst, p.clip), p.loop);
			inst.fade_elapsed += dt;
			if (inst.fade_elapsed >= inst.fade || p.clip < 0) {
				inst.previous = Layer{};
				inst.fade = inst.fade_elapsed = 0;
			}
		}
		if (auto& l = inst.layered; l.clip >= 0) {
			// A layered clip plays forwards at its own pace, whatever the base clip's speed.
			const double duration = clip_duration(inst, l.clip);
			queue(entity, inst, l.clip,
				crossed_events(inst.event_times[static_cast<std::size_t>(l.clip)], l.time, dt, duration, false, l.fresh));
			l.time += dt;
			l.fresh = false;
			if (l.time >= duration) {
				l = Layered{};
			}
		}
		inst.dirty = true;
	}

	static void sample(const Instance& inst, int clip, double time, bool additive,
		ozz::animation::SamplingJob::Context& context, ozz::vector<ozz::math::SoaTransform>& out)
	{
		const auto& c = set_of(inst).clips[static_cast<std::size_t>(clip)];
		ozz::animation::SamplingJob job;
		job.animation = additive ? c.additive.get() : c.animation.get();
		job.context = &context;
		job.ratio = c.duration > 0 ? static_cast<float>(std::clamp(time / c.duration, 0.0, 1.0)) : 0.0f;
		job.output = ozz::make_span(out);
		(void)job.Run();
	}

	static void compute(Instance& inst)
	{
		const auto& rig = rig_of(inst);
		const auto& skeleton = *rig.skeleton;
		auto& b = *inst.buffers;
		ozz::animation::BlendingJob::Layer layers[2];
		ozz::animation::BlendingJob::Layer additive[1];
		int count = 0, additive_count = 0;
		const double w = inst.fade > 0 ? std::clamp(inst.fade_elapsed / inst.fade, 0.0, 1.0) : 1.0;
		if (inst.current.clip >= 0) {
			sample(inst, inst.current.clip, inst.current.time, false, b.current_context, b.current);
			layers[count].transform = ozz::make_span(std::as_const(b.current));
		} else {
			layers[count].transform = skeleton.joint_rest_poses();
		}
		layers[count++].weight = static_cast<float>(w);
		if (inst.fade > 0 && inst.previous.clip >= 0) {
			sample(inst, inst.previous.clip, inst.previous.time, false, b.previous_context, b.previous);
			layers[count].transform = ozz::make_span(std::as_const(b.previous));
			layers[count++].weight = static_cast<float>(1 - w);
		}
		if (const auto& l = inst.layered; l.clip >= 0) {
			sample(inst, l.clip, l.time, true, b.layered_context, b.layered);
			additive[0].transform = ozz::make_span(std::as_const(b.layered));
			additive[0].weight = static_cast<float>(l.weight * layered_envelope(inst, clip_duration(inst, l.clip)));
			additive_count = 1;
		}
		ozz::animation::BlendingJob blend;
		blend.layers = ozz::span<const ozz::animation::BlendingJob::Layer>(layers, static_cast<std::size_t>(count));
		blend.additive_layers = ozz::span<const ozz::animation::BlendingJob::Layer>(additive, static_cast<std::size_t>(additive_count));
		blend.rest_pose = skeleton.joint_rest_poses();
		blend.output = ozz::make_span(b.blended);
		(void)blend.Run();
		ozz::animation::LocalToModelJob to_model;
		to_model.skeleton = &skeleton;
		to_model.input = ozz::make_span(std::as_const(b.blended));
		to_model.output = ozz::make_span(b.models);
		(void)to_model.Run();
		inst.ik_reached = true;
		if (!inst.chains.empty()) {
			solve(inst);
		}
		auto pose = std::make_shared<std::vector<JointMatrix>>(rig.joints.size());
		for (std::size_t i = 0; i < rig.joints.size(); ++i) {
			(*pose)[i] = joint_matrix(b.models[static_cast<std::size_t>(rig.ozz_of[i])]);
		}
		inst.pose = std::move(pose);
		inst.dirty = false;
	}

	static void solve(Instance& inst)
	{
		const auto& skeleton = *rig_of(inst).skeleton;
		auto& b = *inst.buffers;
		const auto to_model = inverse_affine(inst.world);
		if (!to_model) {
			inst.ik_reached = false;
			return;
		}
		for (const auto& chain : inst.chains) {
			const auto target = transform(*to_model, chain.spec.target, 1);
			const auto pole = transform(*to_model, chain.spec.pole, 0);
			const auto& axis = chain.spec.bend_axis;
			ozz::animation::IKTwoBoneJob job;
			job.target = ozz::math::simd_float4::Load(static_cast<float>(target[0]), static_cast<float>(target[1]),
				static_cast<float>(target[2]), 1.0f);
			job.pole_vector = ozz::math::simd_float4::Load(static_cast<float>(pole[0]), static_cast<float>(pole[1]),
				static_cast<float>(pole[2]), 0.0f);
			job.mid_axis = ozz::math::simd_float4::Load(axis[0], axis[1], axis[2], 0.0f);
			job.weight = static_cast<float>(chain.spec.weight);
			job.soften = static_cast<float>(chain.spec.soften);
			job.start_joint = &b.models[static_cast<std::size_t>(chain.start)];
			job.mid_joint = &b.models[static_cast<std::size_t>(chain.middle)];
			job.end_joint = &b.models[static_cast<std::size_t>(chain.end)];
			ozz::math::SimdQuaternion start_correction, middle_correction;
			bool reached = false;
			job.start_joint_correction = &start_correction;
			job.mid_joint_correction = &middle_correction;
			job.reached = &reached;
			if (!job.Run()) {
				inst.ik_reached = false;
				continue;
			}
			inst.ik_reached = inst.ik_reached && reached;
			correct_rotation(ozz::make_span(b.blended), chain.start, start_correction);
			correct_rotation(ozz::make_span(b.blended), chain.middle, middle_correction);
			ozz::animation::LocalToModelJob again;
			again.skeleton = &skeleton;
			again.input = ozz::make_span(std::as_const(b.blended));
			again.output = ozz::make_span(b.models);
			again.from = chain.start;
			(void)again.Run();
		}
	}

	Instance* find(std::uint64_t entity)
	{
		const auto found = instances.find(entity);
		return found == instances.end() ? nullptr : &found->second;
	}
	const Instance* find(std::uint64_t entity) const
	{
		const auto found = instances.find(entity);
		return found == instances.end() ? nullptr : &found->second;
	}
};

AnimationSystem::AnimationSystem() : impl_(std::make_unique<Impl>()) {}
AnimationSystem::~AnimationSystem() = default;
AnimationSystem::AnimationSystem(AnimationSystem&&) noexcept = default;
AnimationSystem& AnimationSystem::operator=(AnimationSystem&&) noexcept = default;

void AnimationSystem::set_animators(std::span<const AnimatorInput> animators)
{
	std::map<std::uint64_t, Instance> next;
	for (const auto& input : animators) {
		if (!input.entity || !input.clips || next.contains(input.entity)) {
			continue;
		}
		auto found = impl_->instances.find(input.entity);
		Instance inst;
		if (found != impl_->instances.end()) {
			inst = std::move(found->second);
		}
		const bool new_clips = inst.clips != input.clips;
		const bool restart = new_clips || inst.authored_clip != input.clip || inst.authored_loop != input.loop ||
			inst.authored_start != input.start_time;
		if (new_clips) {
			const bool new_rig = !inst.clips || inst.clips->rig() != input.clips->rig();
			inst.clips = input.clips;
			if (new_rig) {
				inst.buffers = std::make_unique<Buffers>(*input.clips->rig()->impl().skeleton);
				inst.chains.clear();
			}
		}
		inst.authored_clip = input.clip;
		inst.authored_loop = input.loop;
		inst.authored_start = std::isfinite(input.start_time) ? std::max(input.start_time, 0.0) : 0.0;
		if (new_clips || inst.events != input.events) {
			inst.events = input.events;
			index_events(inst);
		}
		if (restart) {
			reset(inst);
		}
		inst.speed = std::isfinite(input.speed) ? std::clamp(input.speed, -10.0, 10.0) : 1.0;
		inst.playing = input.playing;
		if (inst.world != input.world) {
			inst.world = input.world;
			inst.dirty = inst.dirty || !inst.chains.empty();
		}
		if (inst.dirty) {
			Impl::compute(inst);
		}
		// Starting again is a jump, not movement.
		if (restart || !inst.previous_pose) {
			inst.previous_pose = inst.pose;
		}
		next.emplace(input.entity, std::move(inst));
	}
	impl_->instances = std::move(next);
}

void AnimationSystem::update(double seconds)
{
	const double dt = std::isfinite(seconds) ? std::clamp(seconds, 0.0, max_step) : 0.0;
	for (auto& [entity, inst] : impl_->instances) {
		inst.previous_pose = inst.pose;
		if (inst.playing && !inst.paused && dt > 0) {
			impl_->advance(entity, inst, dt);
		}
		if (inst.dirty) {
			Impl::compute(inst);
		}
	}
}

void AnimationSystem::restart()
{
	for (auto& [entity, inst] : impl_->instances) {
		(void)entity;
		reset(inst);
		Impl::compute(inst);
		inst.previous_pose = inst.pose;
	}
	impl_->events.clear();
}

Pose AnimationSystem::pose(std::uint64_t entity) const
{
	const auto* inst = impl_->find(entity);
	return inst ? inst->pose : nullptr;
}

Pose AnimationSystem::previous_pose(std::uint64_t entity) const
{
	const auto* inst = impl_->find(entity);
	return inst ? inst->previous_pose : nullptr;
}

std::optional<JointMatrix> AnimationSystem::joint_model(std::uint64_t entity, std::string_view joint) const
{
	const auto* inst = impl_->find(entity);
	if (!inst || !inst->pose) {
		return std::nullopt;
	}
	const auto index = inst->clips->rig()->find_joint(joint);
	if (!index) {
		return std::nullopt;
	}
	return (*inst->pose)[*index];
}

std::optional<Matrix> AnimationSystem::joint_world(std::uint64_t entity, std::string_view joint) const
{
	const auto model = joint_model(entity, joint);
	if (!model) {
		return std::nullopt;
	}
	return multiply(impl_->find(entity)->world, *model);
}

std::optional<AnimatorStatus> AnimationSystem::status(std::uint64_t entity) const
{
	const auto* inst = impl_->find(entity);
	if (!inst) {
		return std::nullopt;
	}
	const auto& set = *inst->clips;
	AnimatorStatus s;
	if (inst->current.clip >= 0) {
		s.clip = set.name(static_cast<std::size_t>(inst->current.clip));
		s.time = inst->current.time;
		s.duration = set.duration(static_cast<std::size_t>(inst->current.clip));
		s.loop = inst->current.loop;
	}
	s.speed = inst->speed;
	s.playing = inst->playing;
	s.paused = inst->paused;
	if (inst->fade > 0 && inst->previous.clip >= 0) {
		s.fading_from = set.name(static_cast<std::size_t>(inst->previous.clip));
		s.fade = std::clamp(inst->fade_elapsed / inst->fade, 0.0, 1.0);
	}
	if (inst->layered.clip >= 0) {
		s.layered = set.name(static_cast<std::size_t>(inst->layered.clip));
		s.layered_weight = inst->layered.weight * layered_envelope(*inst, set.duration(static_cast<std::size_t>(inst->layered.clip)));
	}
	s.ik_reached = inst->ik_reached;
	return s;
}

Stats AnimationSystem::stats() const
{
	Stats s;
	for (const auto& [entity, inst] : impl_->instances) {
		(void)entity;
		++s.animators;
		s.posed_joints += inst.pose ? inst.pose->size() : 0;
	}
	return s;
}

std::vector<AnimationEvent> AnimationSystem::take_events() { return std::exchange(impl_->events, {}); }

Result<void> AnimationSystem::play(std::uint64_t entity, std::string_view clip, double blend_seconds, bool loop)
{
	auto* inst = impl_->find(entity);
	if (!inst) {
		return fail(ErrorCode::NotFound, "the entity has no animator");
	}
	const auto found = inst->clips->find(clip);
	if (!found) {
		return fail(ErrorCode::NotFound, "clip '" + std::string(clip) + "' is not in the model");
	}
	if (!std::isfinite(blend_seconds) || blend_seconds < 0 || blend_seconds > 10) {
		return fail(ErrorCode::InvalidArgument, "a crossfade lasts 0 to 10 seconds");
	}
	if (blend_seconds > 0 && inst->current.clip >= 0) {
		inst->previous = inst->current;
		inst->fade = blend_seconds;
		inst->fade_elapsed = 0;
	} else {
		inst->previous = Layer{};
		inst->fade = inst->fade_elapsed = 0;
	}
	inst->current = Layer{static_cast<int>(*found), 0, loop, true};
	inst->dirty = true;
	return {};
}

Result<void> AnimationSystem::play_layered(std::uint64_t entity, std::string_view clip, double weight, double blend_seconds)
{
	auto* inst = impl_->find(entity);
	if (!inst) {
		return fail(ErrorCode::NotFound, "the entity has no animator");
	}
	const auto found = inst->clips->find(clip);
	if (!found) {
		return fail(ErrorCode::NotFound, "clip '" + std::string(clip) + "' is not in the model");
	}
	if (!std::isfinite(weight) || weight < 0 || weight > 1 || !std::isfinite(blend_seconds) || blend_seconds < 0 || blend_seconds > 10) {
		return fail(ErrorCode::InvalidArgument, "a layered clip needs a weight within [0, 1] and a blend of 0 to 10 seconds");
	}
	inst->layered = Layered{static_cast<int>(*found), 0, weight, blend_seconds, true};
	inst->dirty = true;
	return {};
}

Result<void> AnimationSystem::set_time(std::uint64_t entity, double seconds)
{
	auto* inst = impl_->find(entity);
	if (!inst) {
		return fail(ErrorCode::NotFound, "the entity has no animator");
	}
	if (inst->current.clip < 0) {
		return fail(ErrorCode::InvalidState, "no clip is playing");
	}
	if (!std::isfinite(seconds)) {
		return fail(ErrorCode::InvalidArgument, "the time must be finite");
	}
	inst->current.time = wrap(seconds, clip_duration(*inst, inst->current.clip), inst->current.loop);
	inst->current.fresh = false;
	inst->previous = Layer{};
	inst->fade = inst->fade_elapsed = 0;
	Impl::compute(*inst);
	return {};
}

Result<void> AnimationSystem::set_paused(std::uint64_t entity, bool paused)
{
	auto* inst = impl_->find(entity);
	if (!inst) {
		return fail(ErrorCode::NotFound, "the entity has no animator");
	}
	inst->paused = paused;
	return {};
}

Result<void> AnimationSystem::set_ik(std::uint64_t entity, std::vector<TwoBoneIK> chains)
{
	auto* inst = impl_->find(entity);
	if (!inst) {
		return fail(ErrorCode::NotFound, "the entity has no animator");
	}
	const auto& rig = rig_of(*inst);
	const auto ozz_joint = [&](const std::string& name) -> int {
		const auto found = rig.by_name.find(name);
		return found == rig.by_name.end() ? -1 : rig.ozz_of[found->second];
	};
	const auto parents = rig.skeleton->joint_parents();
	const auto above = [&](int ancestor, int joint) {
		for (int j = parents[static_cast<std::size_t>(joint)]; j >= 0; j = parents[static_cast<std::size_t>(j)]) {
			if (j == ancestor) {
				return true;
			}
		}
		return false;
	};
	std::vector<Chain> resolved;
	for (auto& spec : chains) {
		Chain chain{spec, ozz_joint(spec.start), ozz_joint(spec.middle), ozz_joint(spec.end)};
		if (chain.start < 0 || chain.middle < 0 || chain.end < 0) {
			return fail(ErrorCode::NotFound, "an IK chain names a joint the skeleton does not have");
		}
		if (!above(chain.start, chain.middle) || !above(chain.middle, chain.end)) {
			return fail(ErrorCode::InvalidArgument, "an IK chain's joints must each be above the next");
		}
		const auto& a = spec.bend_axis;
		const double length = std::hypot(a[0], a[1], a[2]);
		const bool finite_points = std::all_of(spec.target.begin(), spec.target.end(), [](double v) { return std::isfinite(v); }) &&
			std::all_of(spec.pole.begin(), spec.pole.end(), [](double v) { return std::isfinite(v); });
		if (!finite_points || !std::isfinite(length) || length < 1e-6 || !(spec.weight >= 0 && spec.weight <= 1) ||
			!(spec.soften > 0 && spec.soften <= 1)) {
			return fail(ErrorCode::InvalidArgument, "an IK chain needs finite points, a bend axis, a weight within [0, 1] and soften within (0, 1]");
		}
		for (auto& v : chain.spec.bend_axis) {
			v = static_cast<float>(v / length);
		}
		resolved.push_back(std::move(chain));
	}
	inst->chains = std::move(resolved);
	inst->dirty = true;
	return {};
}

Pose place_joints(std::span<const int> parents, std::span<const JointMatrix> base, std::span<const std::optional<JointMatrix>> placed)
{
	const std::size_t n = parents.size();
	if (base.size() != n || placed.size() != n) {
		return nullptr;
	}
	auto out = std::make_shared<std::vector<JointMatrix>>(n);
	auto& m = *out;
	for (std::size_t j = 0; j < n; ++j) {
		const int parent = parents[j];
		if (placed[j]) {
			m[j] = *placed[j];
		} else if (parent >= 0 && static_cast<std::size_t>(parent) < j) {
			// Its transform relative to its parent, as the base pose has it, under where its parent is now.
			const auto p = static_cast<std::size_t>(parent);
			const auto widen = [](const JointMatrix& f) {
				Matrix d{};
				std::copy(f.begin(), f.end(), d.begin());
				return d;
			};
			const auto inverse = inverse_affine(widen(base[p]));
			if (!inverse) {
				m[j] = base[j];
				continue;
			}
			const Matrix local = multiply(*inverse, base[j]);
			JointMatrix narrow{};
			std::transform(local.begin(), local.end(), narrow.begin(), [](double v) { return static_cast<float>(v); });
			const Matrix now = multiply(widen(m[p]), narrow);
			std::transform(now.begin(), now.end(), m[j].begin(), [](double v) { return static_cast<float>(v); });
		} else {
			m[j] = base[j];
		}
	}
	return out;
}

} // namespace engine::animation
