#pragma once
#include <engine/scene/Scene.hpp>
#include <optional>

namespace engine::editor {
// The editor's view. Always a pivot, two angles and a distance, so orbiting, flying and framing share one state:
// flying moves the pivot along with the eye.
struct OrbitCamera {
    Vec3 target{0, 0.5, 0};
    double yaw = 0.7, pitch = 0.4, distance = 8;
    double fov_degrees = 60; // Vertical.
    bool orthographic = false; // Seen along the view direction; `half_height()` meters from the middle to the top.
    // Turns around the pivot.
    void orbit(double dx, double dy);
    // Turns around any point, keeping what is on screen where it is relative to the rotation.
    void orbit_around(const Vec3 &pivot, double dx, double dy);
    // Turns the view around the eye, as a person turning their head.
    void look(double dx, double dy);
    void pan(double dx, double dy, double viewport_height);
    void zoom(double steps);
    void move(const Vec3 &offset);
    Vec3 eye() const;
    Vec3 forward() const;
    Vec3 right() const;
    double half_height() const;
    WorldTransform world() const;
};
enum class CameraMode : std::uint8_t {
    Idle,
    ClickPending, // LMB down, not moved far enough to be a drag yet.
    Walk,         // LMB drag: move forward and back, and turn.
    Fly,          // RMB: look, WASD to move.
    Vertical,     // LMB and RMB: up, down and sideways.
    Orbit,        // Alt+LMB.
    Dolly,        // Alt+RMB.
    Pan,          // MMB or Alt+MMB. RMB too in an orthographic view, which cannot be looked around.
};
// One frame of what the devices are doing.
struct CameraInput {
    double dt = 0;
    double mouse_dx = 0, mouse_dy = 0, wheel = 0;
    double viewport_height = 1;
    bool lmb = false, rmb = false, mmb = false, alt = false, shift = false;
    bool can_start = false; // The mouse is over the view and not over a gizmo or an overlay: a press starts navigating.
    bool forward = false, back = false, left = false, right = false, up = false, down = false, fov_out = false, fov_in = false;
    std::optional<Vec3> pivot; // What Alt+LMB turns around; the camera's own pivot when empty.
};
// The state machine behind viewport navigation. The state comes from the held buttons and modifier, and it holds until
// the buttons that started it are released, wherever the mouse has gone. No ImGui or GLFW here.
class CameraController {
    CameraMode mode_ = CameraMode::Idle;
    bool last_lmb_ = false, last_rmb_ = false, last_mmb_ = false;
    bool clicked_ = false;
    double moved_ = 0;
    double speed_ = 4; // Meters a second while flying.
    double speed_shown_ = 0; // Seconds left to show it.
    double vx_ = 0, vy_ = 0, vz_ = 0;
    double fov_offset_ = 0;
    double base_fov_ = 60;
    Vec3 pivot_;
    std::optional<OrbitCamera> from_, to_;
    double animation_ = 0, animation_length_ = 0;
public:
    static constexpr double min_speed = 0.05, max_speed = 1000, click_slop = 4;
    void update(OrbitCamera &, const CameraInput &);
    CameraMode mode() const noexcept { return mode_; }
    // The mouse is held for navigating: hide it, and put it back where it was afterwards.
    bool holds_cursor() const noexcept { return mode_ != CameraMode::Idle && mode_ != CameraMode::ClickPending; }
    // LMB came up this frame without having dragged: the host treats it as a click.
    bool clicked() const noexcept { return clicked_; }
    double speed() const noexcept { return speed_; }
    void set_speed(double);
    // Above zero for a moment after the speed changes, so an overlay can show it.
    double speed_shown() const noexcept { return speed_shown_; }
    // Eases the camera to `goal`. Starting to navigate cancels it.
    void animate_to(const OrbitCamera &current, const OrbitCamera &goal, double seconds = 0.2);
    bool animating() const noexcept { return to_.has_value(); }
    // Back to idle at once, as when the window loses focus.
    void reset();
};
} // namespace engine::editor
