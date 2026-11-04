#pragma once

// Skeletal animation on ozz-animation (Milestone 5, D1, D2, D4 to D6). It turns a model's skeleton and clips into
// poses: model-space joint transforms a renderer skins with. Poses live here, never in the scene, its history or its
// file. No scene, renderer, physics or editor dependency, and ozz stays behind this header. One thread: the caller's.

#include <engine/assets/asset_types.h>
#include <engine/core/Result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::animation {

using Matrix = std::array<double, 16>;     // Column-major world transform, like engine::WorldTransform.
using JointMatrix = std::array<float, 16>; // Column-major model-space joint transform.
// One matrix per joint of assets::MeshSkin::joints, in that order: what renderer::Pose holds.
using Pose = std::shared_ptr<const std::vector<JointMatrix>>;
using Point = std::array<double, 3>;
inline constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// A model's skeleton, ready to animate. Immutable once built, so animators share it.
class Rig {
public:
	// Fails for an empty skeleton, more than assets::max_skeleton_joints joints, a parent that does not come before its
	// child, a repeated name or a non-finite rest transform.
	static Result<std::shared_ptr<const Rig>> build(const assets::MeshSkin& skin);
	~Rig();
	Rig(const Rig&) = delete;
	Rig& operator=(const Rig&) = delete;

	[[nodiscard]] std::size_t joint_count() const noexcept;
	[[nodiscard]] const std::string& joint_name(std::size_t joint) const;
	[[nodiscard]] int joint_parent(std::size_t joint) const; // -1 for a root.
	[[nodiscard]] std::optional<std::size_t> find_joint(std::string_view name) const;
	// Model-space transforms as the skeleton rests.
	[[nodiscard]] const Pose& rest_pose() const noexcept;

	struct Impl;
	[[nodiscard]] const Impl& impl() const noexcept { return *impl_; }

private:
	explicit Rig(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

// The clips of a model bound to a rig: the rig's own model, or another whose joints have the same names (D4; there is
// no retargeting). A joint the clips do not move holds its rest transform. Rotations between keys follow glTF's
// spherical interpolation: keys are added where ozz's normalized lerp would lag it. Immutable once built.
class ClipSet {
public:
	// Fails when no joint name of the source skeleton is in the rig, or a clip cannot be built (keys out of order).
	static Result<std::shared_ptr<const ClipSet>> build(std::shared_ptr<const Rig> rig, const assets::MeshSkin& source,
		std::span<const assets::AnimationClip> clips);
	~ClipSet();
	ClipSet(const ClipSet&) = delete;
	ClipSet& operator=(const ClipSet&) = delete;

	[[nodiscard]] std::size_t size() const noexcept;
	[[nodiscard]] const std::string& name(std::size_t clip) const;
	[[nodiscard]] double duration(std::size_t clip) const; // Seconds; 0 for a single pose.
	[[nodiscard]] std::optional<std::size_t> find(std::string_view name) const;
	// Joints of the rig that the source skeleton has too.
	[[nodiscard]] std::size_t matched_joints() const noexcept;
	[[nodiscard]] const std::shared_ptr<const Rig>& rig() const noexcept;

	struct Impl;
	[[nodiscard]] const Impl& impl() const noexcept { return *impl_; }

private:
	explicit ClipSet(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

// An animated entity as the host reads it from the scene each frame.
struct AnimatorInput {
	std::uint64_t entity = 0;
	std::shared_ptr<const ClipSet> clips; // Null: the entity is dropped. Its rig is the entity's model's.
	std::string clip;                     // Played from start_time. Empty or unknown: the rest pose.
	double speed = 1;                     // Within [-10, 10].
	bool loop = true;
	double start_time = 0;
	bool playing = true;                  // False holds the start pose: the editor with its preview off.
	Matrix world = identity;              // Where the entity is; joint_world() and IK targets use it.
	std::vector<assets::ClipEvent> events; // Of the clip source, for any of its clips.
};

// A clip event a tick crossed, in the order they happened.
struct AnimationEvent {
	std::uint64_t entity = 0;
	std::string clip;
	std::string name;
	double time = 0; // The event's time in its clip.
};

// Turns a chain of three joints (hip, knee, ankle) so its end reaches a world point, as for planting a foot.
struct TwoBoneIK {
	std::string start, middle, end; // Joint names; each an ancestor of the next, not necessarily the parent.
	Point target{};                 // World space.
	Point pole{0, 0, 1};            // World direction the middle joint bends towards.
	std::array<float, 3> bend_axis{0, 0, 1}; // In the middle joint's own space: turning about it opens the chain.
	double weight = 1;              // 0 leaves the chain as animated; within [0, 1].
	double soften = 1;              // Below 1 the chain stops short of straight; within (0, 1].
};

struct AnimatorStatus {
	std::string clip;          // Empty when resting.
	double time = 0, duration = 0, speed = 1;
	bool loop = true, playing = true, paused = false;
	std::string fading_from;   // The clip fading out, during a crossfade.
	double fade = 1;           // Crossfade progress, 1 when there is none.
	std::string layered;       // The additive clip playing on top, if any.
	double layered_weight = 0;
	bool ik_reached = true;    // Every IK chain reached its target on the last update.
};

struct Stats {
	std::size_t animators = 0, posed_joints = 0;
};

class AnimationSystem {
public:
	AnimationSystem();
	~AnimationSystem();
	AnimationSystem(AnimationSystem&&) noexcept;
	AnimationSystem& operator=(AnimationSystem&&) noexcept;

	// The animators that exist now; the rest are dropped. A new one, or one whose clips, clip, loop or start time changed,
	// starts its clip again and has its pose at once. Speed, world, events and `playing` change in place, so what
	// gameplay played keeps playing. Entities must be unique and nonzero.
	void set_animators(std::span<const AnimatorInput> animators);
	// Advances every playing animator by `seconds` (clamped to [0, 10]) and poses it. Clip events crossed are queued.
	void update(double seconds);
	// Starts every animator again from its authored clip and start time, and drops queued events.
	void restart();

	// ---- What gameplay and tools ask for (D2) ----
	[[nodiscard]] Pose pose(std::uint64_t entity) const;
	// The pose before the last update(): with pose(), how fast each joint was moving, which a ragdoll starts with (D7).
	// The same as pose() for an animator just started or not playing.
	[[nodiscard]] Pose previous_pose(std::uint64_t entity) const;
	[[nodiscard]] std::optional<JointMatrix> joint_model(std::uint64_t entity, std::string_view joint) const;
	// The joint's world transform this tick: the entity's world times its model-space transform.
	[[nodiscard]] std::optional<Matrix> joint_world(std::uint64_t entity, std::string_view joint) const;
	[[nodiscard]] std::optional<AnimatorStatus> status(std::uint64_t entity) const;
	[[nodiscard]] Stats stats() const;
	// Events crossed since the last call, oldest first. At most 1,024 are kept.
	std::vector<AnimationEvent> take_events();

	// ---- Control at run time; none of it changes the scene ----
	// Crossfades to a clip over `blend_seconds` (0 cuts), starting it from its beginning.
	Result<void> play(std::uint64_t entity, std::string_view clip, double blend_seconds = 0.2, bool loop = true);
	// Plays a clip once on top of the current one, as the difference from its first frame (a flinch over a walk). It
	// fades in and out over `blend_seconds`; playing it again starts it over. `weight` within [0, 1].
	Result<void> play_layered(std::uint64_t entity, std::string_view clip, double weight = 1, double blend_seconds = 0.1);
	// Jumps the current clip to a time (wrapped when it loops, otherwise clamped) and poses at once. Fires no event and
	// ends a crossfade.
	Result<void> set_time(std::uint64_t entity, double seconds);
	Result<void> set_paused(std::uint64_t entity, bool paused);
	// Replaces the entity's IK chains; empty removes them. Applied after the clips on every update.
	Result<void> set_ik(std::uint64_t entity, std::vector<TwoBoneIK> chains);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// A pose in which something else places some joints, as a ragdoll's bodies do (D7). `placed` holds a model-space
// transform for each joint it moves and nothing for the rest, which keep their transform relative to their parent from
// `base`. All three in the skeleton's order, parents first, as assets::MeshSkin::joints are. Null when the sizes differ.
[[nodiscard]] Pose place_joints(std::span<const int> parents, std::span<const JointMatrix> base,
	std::span<const std::optional<JointMatrix>> placed);

// Which events a clip's time crosses moving from `from` by `delta` seconds (negative plays backwards): indices into
// `event_times` (ascending), in the order crossed. A looping clip wraps and crosses an event once per lap; one that
// does not stops at its ends. `inclusive` also counts an event exactly at `from`, as when a clip starts. At most 64.
[[nodiscard]] std::vector<std::size_t> crossed_events(std::span<const double> event_times, double from, double delta,
	double duration, bool loop, bool inclusive);

} // namespace engine::animation
