#pragma once

// Input actions. Gameplay reads actions, never devices; a host feeds an InputMap whatever device codes it has.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace engine::gameplay {

enum class Action : std::uint8_t {
	MoveForward,
	MoveBack,
	MoveLeft,
	MoveRight,
	Jump,
	Sprint,
	Fire,
	Reload,
	Count,
};

inline constexpr std::size_t action_count = static_cast<std::size_t>(Action::Count);

// One fixed tick's worth of input.
struct InputFrame {
	std::array<bool, action_count> held{};
	std::array<bool, action_count> pressed{}; // Went down since the previous frame, even if already released.

	[[nodiscard]] bool is_held(Action action) const noexcept { return held[static_cast<std::size_t>(action)]; }
	[[nodiscard]] bool was_pressed(Action action) const noexcept { return pressed[static_cast<std::size_t>(action)]; }
};

// Device codes for the default bindings. They equal GLFW's key codes for the keyboard; mouse buttons follow from 1000.
namespace code {
inline constexpr int space = 32;
inline constexpr int a = 'A', d = 'D', r = 'R', s = 'S', w = 'W';
inline constexpr int left_shift = 340;
inline constexpr int mouse_left = 1000;
} // namespace code

// Turns device state into actions. A press shorter than a tick still reaches the next take().
class InputMap {
public:
	// WASD, mouse button fires, Space jumps, Shift sprints, R reloads.
	static InputMap shooter_defaults();

	void bind(int device_code, Action action);
	// Current state of a device code. Codes nothing is bound to are ignored.
	void set(int device_code, bool down);
	// Releases everything, for when the host stops delivering input (the cursor was released).
	void release_all();
	[[nodiscard]] InputFrame take();

private:
	std::map<int, Action> bindings_;
	std::map<int, bool> down_;
	std::array<bool, action_count> pressed_{};
};

} // namespace engine::gameplay
