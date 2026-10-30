#pragma once
#include <engine/core/Result.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {
// What a user can trigger, as opposed to engine::commands::Command, which is what changes the scene. No ImGui or GLFW
// here: the UI turns device events into chords and draws menus from this registry.
enum class Key : std::uint8_t {
    None,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Left, Right, Up, Down,
    Grave, LeftBracket, RightBracket, Minus, Equal, Semicolon, Apostrophe, Comma, Period, Slash, Backslash,
    Count,
};
namespace modifier {
inline constexpr std::uint8_t none = 0, ctrl = 1, shift = 2, alt = 4;
}
struct KeyChord {
    Key key = Key::None;
    std::uint8_t modifiers = modifier::none;
    bool empty() const noexcept { return key == Key::None; }
    bool operator==(const KeyChord &) const = default;
};
// "Ctrl+Shift+S". Modifiers always in that order, so equal chords have equal text.
std::string to_string(KeyChord);
// Any modifier order, any letter case. Empty for text that is not one chord.
std::optional<KeyChord> parse_chord(std::string_view);

namespace context {
inline constexpr std::string_view global = "Global", viewport = "Viewport", outliner = "Outliner", details = "Details", asset_browser = "AssetBrowser";
// Keys held while flying with RMB. Never dispatched: the camera reads them, so the same keys can mean something else otherwise.
inline constexpr std::string_view flying = "Flying";
}
// The description of a command, with no behavior: what the shortcut documentation and tests read.
struct CommandInfo {
    std::string id;       // "Viewport.FocusSelected"
    std::string label;    // "Focus Selected"
    std::string tooltip;  // One sentence, shown after the label and shortcut.
    std::string category; // Groups it in menus and the Keyboard Shortcuts panel.
    std::string context = std::string(context::global);
    std::vector<KeyChord> default_chords;
    bool repeats = false; // Fires again while the key is held.
    std::string icon;     // Optional; empty for none.
};
struct EditorCommand {
    CommandInfo info;
    std::vector<KeyChord> chords; // The current bindings: defaults with the user's overrides on top.
    std::function<void()> execute;
    std::function<bool()> can_execute; // Unset: always. False greys it out and a shortcut passes it by.
    std::function<bool()> is_checked;  // Unset: not a toggle.
};
// Why a shortcut must not fire this frame.
struct InputBlock {
    bool text_input = false;  // A text field has the keyboard.
    bool modal = false;       // A dialog or popup is open.
    bool captured = false;    // The running game holds the mouse and keyboard.
    bool interacting = false; // A drag is in progress: a gizmo or a field.
    bool rebinding = false;   // The Keyboard Shortcuts panel is waiting for keys.
    bool any() const noexcept { return text_input || modal || captured || interacting || rebinding; }
};
struct KeymapReport {
    std::size_t applied = 0;
    std::vector<std::string> unknown; // Commands the file names that this build does not have.
};
class CommandRegistry {
    std::vector<EditorCommand> commands_; // In registration order.
    std::map<std::string, std::size_t, std::less<>> index_;
    EditorCommand *mutable_find(std::string_view);
public:
    Result<void> add(CommandInfo);
    // Gives a registered command its behavior.
    Result<void> bind(std::string_view id, std::function<void()> execute, std::function<bool()> can_execute = {}, std::function<bool()> is_checked = {});
    const std::vector<EditorCommand> &commands() const noexcept { return commands_; }
    const EditorCommand *find(std::string_view id) const;
    // Commands with no behavior yet; a host checks this is empty at startup.
    std::vector<std::string> unbound() const;
    bool enabled(std::string_view id) const;
    bool checked(std::string_view id) const;
    // Runs it if it can run. True when it ran.
    bool run(std::string_view id) const;
    // The first current binding as text, empty if none: what a menu shows.
    std::string shortcut(std::string_view id) const;
    // "Focus Selected (F)" and the description on a second line, for a tooltip.
    std::string tooltip(std::string_view id) const;
    Result<void> set_chords(std::string_view id, std::vector<KeyChord>);
    Result<void> reset_chords(std::string_view id);
    void reset_all_chords();
    bool overridden(std::string_view id) const;
    // Other commands that the chord would also trigger, in `id`'s context or one that overlaps it.
    std::vector<const EditorCommand *> conflicts(std::string_view id, KeyChord) const;
    // The command the chord means in these contexts, searched in order and then Global. A command that cannot run
    // now is passed over, so a panel's disabled binding does not swallow a global one. Null: none.
    const EditorCommand *resolve(KeyChord, std::span<const std::string_view> contexts) const;
    // Resolves and runs. False, and nothing happens, when `block` says shortcuts must not fire.
    bool dispatch(KeyChord, std::span<const std::string_view> contexts, const InputBlock &block = {}) const;
    // Text of the user's overrides, one "id = chord, chord" a line.
    std::string save_keymap() const;
    // Replaces all overrides with the text's. A malformed line fails and changes nothing; an unknown command is reported.
    Result<KeymapReport> load_keymap(std::string_view text);
};
// Every command the editor has, as data. The UI binds behavior to these by ID.
std::vector<CommandInfo> default_command_table();
// Mouse gestures, which belong to the camera and gizmo state machines rather than to chords, so they cannot be rebound yet.
struct MouseGesture { std::string_view input, action; };
std::span<const MouseGesture> mouse_gestures();
// The commands and their default bindings as Markdown: docs/EDITOR_SHORTCUTS.md.
std::string shortcuts_markdown(std::span<const CommandInfo>);
} // namespace engine::editor
