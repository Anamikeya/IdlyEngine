// ozz-animation as the engine builds it: C++20, the DLL runtime, runtime and offline libraries only.
// A two-joint arm and a one-second clip are built in code, as the importer will build them from a GLB.
#include "engine/test/test.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/animation/runtime/sampling_job.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>

#include <cmath>
#include <string>

namespace {

namespace offline = ozz::animation::offline;
constexpr float quarter_turn = 1.57079632679f;
// ozz stores rotation keys as three 16-bit components, so a sampled pose is within about 0.5 mm per metre of
// exact (0.45 mm measured at the end of this clip). Tests compare poses at this precision.
constexpr double key_precision = 1e-3;

ozz::unique_ptr<ozz::animation::Skeleton> make_arm()
{
	offline::RawSkeleton raw;
	raw.roots.resize(1);
	auto& shoulder = raw.roots[0];
	shoulder.name = "shoulder";
	shoulder.transform = ozz::math::Transform::identity();
	shoulder.children.resize(1);
	auto& elbow = shoulder.children[0];
	elbow.name = "elbow";
	elbow.transform = ozz::math::Transform::identity();
	elbow.transform.translation = ozz::math::Float3(0.0f, 1.0f, 0.0f);
	ENGINE_REQUIRE(raw.Validate());
	return offline::SkeletonBuilder()(raw);
}

// The shoulder turns from rest to a quarter turn about +Z over one second.
ozz::unique_ptr<ozz::animation::Animation> make_raise()
{
	offline::RawAnimation raw;
	raw.duration = 1.0f;
	raw.tracks.resize(2);
	raw.tracks[0].rotations.push_back({0.0f, ozz::math::Quaternion::identity()});
	raw.tracks[0].rotations.push_back(
		{1.0f, ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3::z_axis(), quarter_turn)});
	raw.tracks[1].translations.push_back({0.0f, ozz::math::Float3(0.0f, 1.0f, 0.0f)});
	ENGINE_REQUIRE(raw.Validate());
	return offline::AnimationBuilder()(raw);
}

struct Pose {
	ozz::vector<ozz::math::SoaTransform> locals;
	ozz::vector<ozz::math::Float4x4> models;
};

// Model-space position of a joint after sampling the clip at a ratio in [0, 1].
Pose sample(const ozz::animation::Skeleton& skeleton, const ozz::animation::Animation& clip, float ratio)
{
	Pose pose;
	pose.locals.resize(skeleton.num_soa_joints());
	pose.models.resize(skeleton.num_joints());
	ozz::animation::SamplingJob::Context context(skeleton.num_joints());
	ozz::animation::SamplingJob sampling;
	sampling.animation = &clip;
	sampling.context = &context;
	sampling.ratio = ratio;
	sampling.output = ozz::make_span(pose.locals);
	ENGINE_REQUIRE(sampling.Run());
	ozz::animation::LocalToModelJob to_model;
	to_model.skeleton = &skeleton;
	to_model.input = ozz::make_span(pose.locals);
	to_model.output = ozz::make_span(pose.models);
	ENGINE_REQUIRE(to_model.Run());
	return pose;
}

float x_of(const ozz::math::Float4x4& m) { return ozz::math::GetX(m.cols[3]); }
float y_of(const ozz::math::Float4x4& m) { return ozz::math::GetY(m.cols[3]); }

} // namespace

ENGINE_TEST(builds_a_skeleton_from_raw_joints)
{
	const auto skeleton = make_arm();
	ENGINE_REQUIRE(skeleton != nullptr);
	ENGINE_CHECK_EQ(skeleton->num_joints(), 2);
	ENGINE_CHECK_EQ(std::string(skeleton->joint_names()[1]), std::string("elbow"));
	ENGINE_CHECK_EQ(static_cast<int>(skeleton->joint_parents()[1]), 0);
}

ENGINE_TEST(samples_the_start_middle_and_end_of_a_clip)
{
	const auto skeleton = make_arm();
	const auto clip = make_raise();
	ENGINE_REQUIRE(skeleton && clip);
	ENGINE_CHECK_NEAR(clip->duration(), 1.0, 1e-6);

	const auto start = sample(*skeleton, *clip, 0.0f);
	ENGINE_CHECK_NEAR(x_of(start.models[1]), 0.0, key_precision);
	ENGINE_CHECK_NEAR(y_of(start.models[1]), 1.0, key_precision);

	// Halfway the shoulder has turned 45 degrees, so the elbow is at (-sin 45, cos 45).
	const auto middle = sample(*skeleton, *clip, 0.5f);
	ENGINE_CHECK_NEAR(x_of(middle.models[1]), -std::sqrt(0.5), 1e-3);
	ENGINE_CHECK_NEAR(y_of(middle.models[1]), std::sqrt(0.5), 1e-3);

	const auto end = sample(*skeleton, *clip, 1.0f);
	ENGINE_CHECK_NEAR(x_of(end.models[1]), -1.0, key_precision);
	ENGINE_CHECK_NEAR(y_of(end.models[1]), 0.0, key_precision);
}

ENGINE_TEST(blends_two_poses_by_weight)
{
	const auto skeleton = make_arm();
	const auto clip = make_raise();
	ENGINE_REQUIRE(skeleton && clip);
	const auto rest = sample(*skeleton, *clip, 0.0f);
	const auto raised = sample(*skeleton, *clip, 1.0f);

	ozz::animation::BlendingJob::Layer layers[2];
	layers[0].transform = ozz::make_span(rest.locals);
	layers[0].weight = 0.5f;
	layers[1].transform = ozz::make_span(raised.locals);
	layers[1].weight = 0.5f;
	ozz::vector<ozz::math::SoaTransform> blended(skeleton->num_soa_joints());
	ozz::animation::BlendingJob blending;
	blending.layers = layers;
	blending.rest_pose = skeleton->joint_rest_poses();
	blending.output = ozz::make_span(blended);
	ENGINE_REQUIRE(blending.Run());

	ozz::vector<ozz::math::Float4x4> models(skeleton->num_joints());
	ozz::animation::LocalToModelJob to_model;
	to_model.skeleton = skeleton.get();
	to_model.input = ozz::make_span(blended);
	to_model.output = ozz::make_span(models);
	ENGINE_REQUIRE(to_model.Run());
	// Normalized quaternion blending of rest and a quarter turn gives the 45 degree pose.
	ENGINE_CHECK_NEAR(x_of(models[1]), -std::sqrt(0.5), 1e-3);
	ENGINE_CHECK_NEAR(y_of(models[1]), std::sqrt(0.5), 1e-3);
}

ENGINE_TEST(skeleton_and_clip_survive_the_archive)
{
	const auto skeleton = make_arm();
	const auto clip = make_raise();
	ENGINE_REQUIRE(skeleton && clip);
	ozz::io::MemoryStream stream;
	{
		ozz::io::OArchive out(&stream);
		out << *skeleton;
		out << *clip;
	}
	ENGINE_REQUIRE(stream.Seek(0, ozz::io::Stream::kSet) == 0);
	ozz::animation::Skeleton loaded_skeleton;
	ozz::animation::Animation loaded_clip;
	ozz::io::IArchive in(&stream);
	ENGINE_REQUIRE(in.TestTag<ozz::animation::Skeleton>());
	in >> loaded_skeleton;
	ENGINE_REQUIRE(in.TestTag<ozz::animation::Animation>());
	in >> loaded_clip;
	ENGINE_CHECK_EQ(loaded_skeleton.num_joints(), 2);
	const auto end = sample(loaded_skeleton, loaded_clip, 1.0f);
	ENGINE_CHECK_NEAR(x_of(end.models[1]), -1.0, key_precision);
}

ENGINE_TEST(refuses_an_invalid_clip)
{
	offline::RawAnimation raw;
	raw.duration = 1.0f;
	raw.tracks.resize(1);
	// Keys out of order.
	raw.tracks[0].translations.push_back({0.8f, ozz::math::Float3::zero()});
	raw.tracks[0].translations.push_back({0.2f, ozz::math::Float3::one()});
	ENGINE_CHECK(!raw.Validate());
	ENGINE_CHECK(offline::AnimationBuilder()(raw) == nullptr);
}
