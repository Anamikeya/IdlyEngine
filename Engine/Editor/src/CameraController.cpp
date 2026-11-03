#include <engine/editor/CameraController.hpp>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace engine::editor {
namespace {
constexpr double pi = 3.141592653589793;
constexpr double turn_per_pixel = 0.008, look_per_pixel = 0.003, max_pitch = 1.5;
glm::dvec3 to_glm(const Vec3 &v) { return {v.x, v.y, v.z}; }
Vec3 to_vec(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
// The direction from the pivot to the eye.
glm::dvec3 back_vector(double yaw, double pitch) { return {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)}; }
glm::dquat orientation(double yaw, double pitch) {
    return glm::angleAxis(yaw, glm::dvec3(0, 1, 0)) * glm::angleAxis(-pitch, glm::dvec3(1, 0, 0));
}
}
void OrbitCamera::orbit(double dx, double dy) {
    yaw = std::remainder(yaw - dx * turn_per_pixel, 2 * pi);
    pitch = std::clamp(pitch + dy * turn_per_pixel, -max_pitch, max_pitch);
}
void OrbitCamera::orbit_around(const Vec3 &pivot, double dx, double dy) {
    const auto old_turn = orientation(yaw, pitch);
    const auto old_eye = to_glm(eye());
    orbit(dx, dy);
    const auto new_eye = to_glm(pivot) + orientation(yaw, pitch) * glm::inverse(old_turn) * (old_eye - to_glm(pivot));
    target = to_vec(new_eye - distance * back_vector(yaw, pitch));
}
void OrbitCamera::look(double dx, double dy) {
    const auto at = to_glm(eye());
    yaw = std::remainder(yaw - dx * look_per_pixel, 2 * pi);
    pitch = std::clamp(pitch + dy * look_per_pixel, -max_pitch, max_pitch);
    target = to_vec(at - distance * back_vector(yaw, pitch));
}
void OrbitCamera::pan(double dx, double dy, double height) {
    const auto m = glm::make_mat4(world().matrix.data());
    const auto shift = (-dx * glm::dvec3(m[0]) + dy * glm::dvec3(m[1])) * (2 * half_height() / std::max(height, 1.0));
    target = {target.x + shift.x, target.y + shift.y, target.z + shift.z};
}
void OrbitCamera::zoom(double steps) { distance = std::clamp(distance * std::exp(-steps * 0.15), 0.2, 500.0); }
void OrbitCamera::move(const Vec3 &offset) { target = {target.x + offset.x, target.y + offset.y, target.z + offset.z}; }
Vec3 OrbitCamera::eye() const { return to_vec(to_glm(target) + distance * back_vector(yaw, pitch)); }
Vec3 OrbitCamera::forward() const { return to_vec(-back_vector(yaw, pitch)); }
Vec3 OrbitCamera::right() const { return {std::cos(yaw), 0, -std::sin(yaw)}; }
double OrbitCamera::half_height() const { return distance * std::tan(glm::radians(fov_degrees) / 2); }
WorldTransform OrbitCamera::world() const {
    const auto m = glm::inverse(glm::lookAtRH(to_glm(eye()), to_glm(target), glm::dvec3(0, 1, 0)));
    WorldTransform result;
    std::copy_n(glm::value_ptr(m), 16, result.matrix.begin());
    return result;
}
void CameraController::set_speed(double speed) { speed_ = std::clamp(speed, min_speed, max_speed); }
void CameraController::animate_to(const OrbitCamera &current, const OrbitCamera &goal, double seconds) {
    from_ = current; to_ = goal;
    animation_ = 0; animation_length_ = std::max(seconds, 1e-3);
}
void CameraController::reset() {
    mode_ = CameraMode::Idle;
    clicked_ = false;
    vx_ = vy_ = vz_ = 0;
}
void CameraController::update(OrbitCamera &c, const CameraInput &in) {
    const bool lmb_pressed = in.lmb && !last_lmb_, rmb_pressed = in.rmb && !last_rmb_, mmb_pressed = in.mmb && !last_mmb_;
    last_lmb_ = in.lmb; last_rmb_ = in.rmb; last_mmb_ = in.mmb;
    clicked_ = false;
    // ---- Which state ----
    const bool flat = c.orthographic;
    switch (mode_) {
    case CameraMode::Idle:
        if (!in.can_start) break;
        if (in.alt) {
            if (lmb_pressed && !flat) { mode_ = CameraMode::Orbit; pivot_ = in.pivot.value_or(c.target); }
            else if (rmb_pressed) mode_ = CameraMode::Dolly;
            else if (mmb_pressed) mode_ = CameraMode::Pan;
        } else if (rmb_pressed) mode_ = flat ? CameraMode::Pan : CameraMode::Fly;
        else if (mmb_pressed) mode_ = CameraMode::Pan;
        else if (lmb_pressed) { mode_ = CameraMode::ClickPending; moved_ = 0; }
        break;
    case CameraMode::ClickPending:
        moved_ += std::abs(in.mouse_dx) + std::abs(in.mouse_dy);
        if (!in.lmb) { clicked_ = moved_ <= click_slop || flat; mode_ = CameraMode::Idle; }
        else if (in.rmb && !flat) mode_ = CameraMode::Vertical;
        else if (moved_ > click_slop && !flat) mode_ = CameraMode::Walk;
        break;
    case CameraMode::Walk:
        if (!in.lmb) mode_ = CameraMode::Idle;
        else if (in.rmb) mode_ = CameraMode::Vertical;
        break;
    case CameraMode::Fly:
        if (!in.rmb) mode_ = CameraMode::Idle;
        else if (in.lmb) mode_ = CameraMode::Vertical;
        break;
    case CameraMode::Vertical:
        if (!in.lmb && !in.rmb) mode_ = CameraMode::Idle;
        else if (!in.lmb) mode_ = CameraMode::Fly;
        else if (!in.rmb) mode_ = CameraMode::Walk;
        break;
    case CameraMode::Orbit:
        if (!in.lmb) mode_ = CameraMode::Idle;
        break;
    case CameraMode::Dolly:
        if (!in.rmb) mode_ = CameraMode::Idle;
        break;
    case CameraMode::Pan:
        if (!in.mmb && !in.rmb) mode_ = CameraMode::Idle;
        break;
    }
    if (holds_cursor()) { to_.reset(); from_.reset(); }
    // ---- What it does ----
    const double step_speed = speed_ * (in.shift ? 3 : 1);
    switch (mode_) {
    case CameraMode::Walk: {
        c.look(in.mouse_dx, 0);
        const double along = -in.mouse_dy * step_speed * 0.008;
        c.move({-std::sin(c.yaw) * along, 0, -std::cos(c.yaw) * along});
        break;
    }
    case CameraMode::Vertical: {
        const auto right = c.right();
        const double across = in.mouse_dx * step_speed * 0.008;
        c.move({right.x * across, -in.mouse_dy * step_speed * 0.008, right.z * across});
        break;
    }
    case CameraMode::Fly: {
        c.look(in.mouse_dx, in.mouse_dy);
        if (in.wheel != 0) { set_speed(speed_ * std::pow(1.25, in.wheel)); speed_shown_ = 1.5; }
        const auto forward = c.forward(), right = c.right();
        const double ahead = (in.forward ? 1 : 0) - (in.back ? 1 : 0), side = (in.right ? 1 : 0) - (in.left ? 1 : 0), rise = (in.up ? 1 : 0) - (in.down ? 1 : 0);
        const double want_x = (forward.x * ahead + right.x * side) * step_speed, want_y = (forward.y * ahead + rise) * step_speed, want_z = (forward.z * ahead + right.z * side) * step_speed;
        // Eases toward the wanted velocity, reaching most of it in about a fifth of a second.
        const double blend = 1 - std::exp(-in.dt * 12);
        vx_ += (want_x - vx_) * blend; vy_ += (want_y - vy_) * blend; vz_ += (want_z - vz_) * blend;
        c.move({vx_ * in.dt, vy_ * in.dt, vz_ * in.dt});
        fov_offset_ = std::clamp(fov_offset_ + ((in.fov_out ? 1 : 0) - (in.fov_in ? 1 : 0)) * 40 * in.dt, 5 - base_fov_, 170 - base_fov_);
        break;
    }
    case CameraMode::Orbit: c.orbit_around(pivot_, in.mouse_dx, in.mouse_dy); break;
    case CameraMode::Dolly: c.zoom((in.mouse_dx - in.mouse_dy) * 0.05); break;
    case CameraMode::Pan: c.pan(in.mouse_dx, in.mouse_dy, in.viewport_height); break;
    default:
        if (in.can_start && in.wheel != 0) c.zoom(in.wheel);
        break;
    }
    if (mode_ != CameraMode::Fly) {
        vx_ = vy_ = vz_ = 0;
        // The field of view comes back on its own once the button is up.
        fov_offset_ *= std::exp(-in.dt * 10);
        if (std::abs(fov_offset_) < 0.01) fov_offset_ = 0;
    }
    c.fov_degrees = base_fov_ + fov_offset_;
    speed_shown_ = std::max(speed_shown_ - in.dt, 0.0);
    // ---- Eased moves ----
    if (to_) {
        animation_ += in.dt;
        const double t = std::min(animation_ / animation_length_, 1.0), eased = t * t * (3 - 2 * t);
        const auto mix = [&](double a, double b) { return a + (b - a) * eased; };
        c.target = {mix(from_->target.x, to_->target.x), mix(from_->target.y, to_->target.y), mix(from_->target.z, to_->target.z)};
        c.distance = mix(from_->distance, to_->distance);
        c.pitch = mix(from_->pitch, to_->pitch);
        c.yaw = from_->yaw + std::remainder(to_->yaw - from_->yaw, 2 * pi) * eased; // The short way around.
        c.orthographic = to_->orthographic;
        if (t >= 1) { c.yaw = to_->yaw; to_.reset(); from_.reset(); }
    }
}
} // namespace engine::editor
