#pragma once
#include <engine/gameplay/game.h>
#include <imgui.h>

namespace engine::editor {
enum class HudAction { None, Start, Resume, Restart, Stop };
// Draws the game's state over the viewport, from plain data. Gameplay knows nothing of this; another host can draw
// the same HudState its own way. `captured`: the mouse is held by the game, so nothing here can be clicked.
HudAction draw_game_hud(const gameplay::HudState &, ImVec2 origin, ImVec2 size, bool captured);
} // namespace engine::editor
