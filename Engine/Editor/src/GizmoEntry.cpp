#include <engine/editor/GizmoEntry.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <charconv>
#include <cmath>

namespace engine::editor {
namespace {
constexpr std::size_t longest_number = 24;
WorldTransform copy(const glm::dmat4 &m) {
    WorldTransform out;
    const auto *values = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i) out.matrix[std::size_t(i)] = values[i];
    return out;
}
}
bool GizmoEntry::feed(char32_t character) {
    if (character == U'x' || character == U'X') { axis_ = 0; return true; }
    if (character == U'y' || character == U'Y') { axis_ = 1; return true; }
    if (character == U'z' || character == U'Z') { axis_ = 2; return true; }
    if (number_.size() >= longest_number) return false;
    if (character == U'-') {
        if (!number_.empty()) return false;
        number_ = "-";
        return true;
    }
    if (character == U'.') {
        if (number_.find('.') != std::string::npos) return false;
        number_ += '.';
        return true;
    }
    if (character < U'0' || character > U'9') return false;
    number_ += char(character);
    return true;
}
void GizmoEntry::backspace() {
    if (!number_.empty()) number_.pop_back();
    else axis_ = -1;
}
void GizmoEntry::clear() {
    number_.clear();
    axis_ = -1;
}
std::optional<double> GizmoEntry::value() const {
    double value = 0;
    const auto [end, error] = std::from_chars(number_.data(), number_.data() + number_.size(), value);
    if (number_.empty() || error != std::errc{} || end != number_.data() + number_.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}
std::string GizmoEntry::text() const {
    constexpr const char *letters[] = {"x", "y", "z"};
    return (axis_ >= 0 ? letters[axis_] : "") + number_;
}
int gizmo_handle_axis(GizmoOperation operation, int handle) noexcept {
    if (handle >= 0 && handle <= 2) return handle;
    return operation == GizmoOperation::Scale && handle == 3 ? gizmo_all_axes : -1;
}
std::optional<WorldTransform> gizmo_typed_change(GizmoOperation operation, const WorldTransform &start, bool local, int axis, double value) {
    if (axis < 0 || axis > gizmo_all_axes || !std::isfinite(value)) return std::nullopt;
    if (axis == gizmo_all_axes && operation != GizmoOperation::Scale) return std::nullopt;
    const auto from = glm::make_mat4(start.matrix.data());
    if (operation == GizmoOperation::Scale) {
        if (value <= 0) return std::nullopt;
        glm::dvec3 factors(1.0);
        if (axis == gizmo_all_axes) factors = glm::dvec3(value);
        else factors[axis] = value;
        return copy(from * glm::scale(glm::dmat4(1.0), factors) * glm::inverse(from));
    }
    glm::dvec3 direction(0.0);
    direction[axis] = 1;
    if (local) {
        const glm::dvec3 column(from[axis]);
        const auto length = glm::length(column);
        if (length <= 0) return std::nullopt;
        direction = column / length;
    }
    if (operation == GizmoOperation::Move) return copy(glm::translate(glm::dmat4(1.0), direction * value));
    const glm::dvec3 pivot(from[3]);
    return copy(glm::translate(glm::dmat4(1.0), pivot) * glm::rotate(glm::dmat4(1.0), glm::radians(value), direction) * glm::translate(glm::dmat4(1.0), -pivot));
}
} // namespace engine::editor
