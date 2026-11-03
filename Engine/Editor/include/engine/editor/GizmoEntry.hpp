#pragma once
#include <engine/scene/Scene.hpp>
#include <optional>
#include <string>

// Typing during a gizmo drag: a number, with an axis letter if wanted, sets how far the drag goes. No ImGui or GLFW.
namespace engine::editor {
enum class GizmoOperation { Move, Rotate, Scale };
inline constexpr int gizmo_all_axes = 3; // Scaling by the center handle.
class GizmoEntry {
    std::string number_;
    int axis_ = -1;
public:
    // A character from the keyboard. False when it means nothing here, which leaves everything as it was.
    bool feed(char32_t character);
    void backspace();
    void clear();
    bool active() const noexcept { return axis_ >= 0 || !number_.empty(); }
    int axis() const noexcept { return axis_; } // 0, 1 or 2 when a letter was typed, otherwise -1.
    const std::string &number() const noexcept { return number_; }
    std::optional<double> value() const; // Empty until what was typed is a number.
    std::string text() const;            // "x-2.5", for the log.
};
// The axis a handle stands for: which axis, gizmo_all_axes for uniform scale, or -1 for a plane or screen handle, which
// needs a letter. `handle` counts from the first handle of its kind: X, Y, Z, then whatever else the kind has.
int gizmo_handle_axis(GizmoOperation operation, int handle) noexcept;
// The world-space change, applied before each entity's start transform, that moves, turns or scales by exactly `value`
// (meters, degrees, times) from `start`. `local` measures along start's own axes; scale always does, as the gizmo does.
// Empty when the values do not make sense: no axis, or a scale of zero or less.
std::optional<WorldTransform> gizmo_typed_change(GizmoOperation operation, const WorldTransform &start, bool local, int axis, double value);
} // namespace engine::editor
