#include <engine/editor/EditorCommands.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

namespace engine::editor {
namespace {
constexpr std::size_t key_count = std::size_t(Key::Count);
constexpr std::array<std::string_view, key_count> key_names{
    "", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Space", "Enter", "Esc", "Tab", "Backspace", "Delete", "Insert", "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down",
    "`", "[", "]", "-", "=", ";", "'", ",", ".", "/", "\\"};
bool same_text(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
bool overlap(std::string_view a, std::string_view b) { return a == b || a == context::global || b == context::global; }
std::string chord_list(const std::vector<KeyChord> &chords) {
    std::string text;
    for (const auto chord : chords) text += (text.empty() ? "" : ", ") + to_string(chord);
    return text;
}
}
std::string to_string(KeyChord chord) {
    std::string text;
    if (chord.modifiers & modifier::ctrl) text += "Ctrl+";
    if (chord.modifiers & modifier::shift) text += "Shift+";
    if (chord.modifiers & modifier::alt) text += "Alt+";
    return text + std::string(key_names[std::size_t(chord.key)]);
}
std::optional<KeyChord> parse_chord(std::string_view text) {
    KeyChord chord;
    text = trim(text);
    while (!text.empty()) {
        const auto plus = text.find('+');
        const auto part = trim(text.substr(0, plus));
        if (plus == std::string_view::npos) {
            for (std::size_t i = 1; i < key_count; ++i) if (same_text(part, key_names[i])) chord.key = Key(i);
            return chord.empty() ? std::nullopt : std::optional(chord);
        }
        if (same_text(part, "ctrl")) chord.modifiers |= modifier::ctrl;
        else if (same_text(part, "shift")) chord.modifiers |= modifier::shift;
        else if (same_text(part, "alt")) chord.modifiers |= modifier::alt;
        else return std::nullopt;
        text.remove_prefix(plus + 1);
    }
    return std::nullopt;
}
EditorCommand *CommandRegistry::mutable_find(std::string_view id) {
    const auto found = index_.find(id);
    return found == index_.end() ? nullptr : &commands_[found->second];
}
const EditorCommand *CommandRegistry::find(std::string_view id) const {
    const auto found = index_.find(id);
    return found == index_.end() ? nullptr : &commands_[found->second];
}
Result<void> CommandRegistry::add(CommandInfo info) {
    if (info.id.empty() || info.label.empty() || info.context.empty()) return fail(ErrorCode::InvalidArgument, "A command needs an id, a label and a context");
    if (index_.contains(info.id)) return fail(ErrorCode::InvalidArgument, "Command already registered: " + info.id);
    index_.emplace(info.id, commands_.size());
    auto chords = info.default_chords;
    commands_.push_back(EditorCommand{std::move(info), std::move(chords), {}, {}, {}});
    return {};
}
Result<void> CommandRegistry::bind(std::string_view id, std::function<void()> execute, std::function<bool()> can_execute, std::function<bool()> is_checked) {
    auto *command = mutable_find(id);
    if (!command) return fail(ErrorCode::NotFound, "No such command: " + std::string(id));
    command->execute = std::move(execute);
    command->can_execute = std::move(can_execute);
    command->is_checked = std::move(is_checked);
    return {};
}
std::vector<std::string> CommandRegistry::unbound() const {
    std::vector<std::string> ids;
    for (const auto &command : commands_) if (!command.execute) ids.push_back(command.info.id);
    return ids;
}
bool CommandRegistry::enabled(std::string_view id) const {
    const auto *command = find(id);
    return command && command->execute && (!command->can_execute || command->can_execute());
}
bool CommandRegistry::checked(std::string_view id) const {
    const auto *command = find(id);
    return command && command->is_checked && command->is_checked();
}
bool CommandRegistry::run(std::string_view id) const {
    if (!enabled(id)) return false;
    find(id)->execute();
    return true;
}
std::string CommandRegistry::shortcut(std::string_view id) const {
    const auto *command = find(id);
    return command && !command->chords.empty() ? to_string(command->chords.front()) : std::string{};
}
std::string CommandRegistry::tooltip(std::string_view id) const {
    const auto *command = find(id);
    if (!command) return {};
    auto text = command->info.label;
    if (const auto keys = shortcut(id); !keys.empty()) text += " (" + keys + ")";
    if (!command->info.tooltip.empty()) text += "\n" + command->info.tooltip;
    return text;
}
Result<void> CommandRegistry::set_chords(std::string_view id, std::vector<KeyChord> chords) {
    auto *command = mutable_find(id);
    if (!command) return fail(ErrorCode::NotFound, "No such command: " + std::string(id));
    if (std::ranges::any_of(chords, [](KeyChord chord) { return chord.empty(); })) return fail(ErrorCode::InvalidArgument, "A binding needs a key");
    command->chords = std::move(chords);
    return {};
}
Result<void> CommandRegistry::reset_chords(std::string_view id) {
    auto *command = mutable_find(id);
    if (!command) return fail(ErrorCode::NotFound, "No such command: " + std::string(id));
    command->chords = command->info.default_chords;
    return {};
}
void CommandRegistry::reset_all_chords() {
    for (auto &command : commands_) command.chords = command.info.default_chords;
}
bool CommandRegistry::overridden(std::string_view id) const {
    const auto *command = find(id);
    return command && command->chords != command->info.default_chords;
}
std::vector<const EditorCommand *> CommandRegistry::conflicts(std::string_view id, KeyChord chord) const {
    std::vector<const EditorCommand *> found;
    const auto *self = find(id);
    if (!self || chord.empty()) return found;
    for (const auto &command : commands_)
        if (command.info.id != id && overlap(command.info.context, self->info.context) && std::ranges::find(command.chords, chord) != command.chords.end())
            found.push_back(&command);
    return found;
}
const EditorCommand *CommandRegistry::resolve(KeyChord chord, std::span<const std::string_view> contexts) const {
    if (chord.empty()) return nullptr;
    const auto in_context = [&](std::string_view context) -> const EditorCommand * {
        for (const auto &command : commands_)
            if (command.info.context == context && std::ranges::find(command.chords, chord) != command.chords.end() && enabled(command.info.id))
                return &command;
        return nullptr;
    };
    for (const auto context : contexts)
        if (const auto *command = in_context(context)) return command;
    return in_context(context::global);
}
std::vector<CheatSheetGroup> CommandRegistry::cheat_sheet(std::span<const std::string_view> contexts, bool with_global) const {
    std::vector<std::string_view> asked;
    for (const auto wanted : contexts) if (std::ranges::find(asked, wanted) == asked.end()) asked.push_back(wanted);
    if (with_global && std::ranges::find(asked, context::global) == asked.end()) asked.push_back(context::global);
    std::vector<KeyChord> taken;
    std::vector<CheatSheetGroup> groups;
    for (const auto wanted : asked) {
        CheatSheetGroup group{std::string(wanted), {}};
        std::vector<KeyChord> taken_here;
        for (const auto &command : commands_) {
            if (command.info.context != wanted) continue;
            const bool can_run = !command.can_execute || command.can_execute();
            std::string keys;
            for (const auto chord : command.chords) {
                if (std::ranges::find(taken, chord) != taken.end()) continue;
                keys += (keys.empty() ? "" : ", ") + to_string(chord);
                // Like resolve(): one that cannot run now does not take the key from the contexts further out.
                if (can_run) taken_here.push_back(chord);
            }
            if (!keys.empty()) group.rows.push_back({std::move(keys), command.info.label, can_run});
        }
        taken.insert(taken.end(), taken_here.begin(), taken_here.end());
        if (!group.rows.empty()) groups.push_back(std::move(group));
    }
    return groups;
}
bool CommandRegistry::dispatch(KeyChord chord, std::span<const std::string_view> contexts, const InputBlock &block) const {
    const auto *command = resolve(chord, contexts);
    if (!command || !block.allows(command->info)) return false;
    command->execute();
    return true;
}
std::string CommandRegistry::save_keymap() const {
    std::string text = "# Editor keymap: only what differs from the defaults. An empty value unbinds a command.\n";
    for (const auto &command : commands_)
        if (command.chords != command.info.default_chords) text += command.info.id + " = " + chord_list(command.chords) + "\n";
    return text;
}
Result<KeymapReport> CommandRegistry::load_keymap(std::string_view text) {
    std::vector<std::pair<std::string, std::vector<KeyChord>>> parsed;
    KeymapReport report;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const auto end = text.find('\n');
        const auto line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        const auto malformed = [&](const std::string &why) { return fail(ErrorCode::ParseError, "Keymap line " + std::to_string(number) + ": " + why); };
        if (equals == std::string_view::npos) return malformed("expected 'command = keys'");
        const auto id = std::string(trim(line.substr(0, equals)));
        auto keys = trim(line.substr(equals + 1));
        std::vector<KeyChord> chords;
        // "=" and "," are keys as well as separators, so a chord ending in one is taken before splitting on commas.
        while (!keys.empty()) {
            auto comma = keys.find(',', keys[0] == ',' ? 1 : 0);
            while (comma != std::string_view::npos && comma > 0 && keys[comma - 1] == '+') comma = keys.find(',', comma + 1);
            const auto chord = parse_chord(keys.substr(0, comma));
            if (!chord) return malformed("'" + std::string(trim(keys.substr(0, comma))) + "' is not a key");
            chords.push_back(*chord);
            if (comma == std::string_view::npos) break;
            keys = trim(keys.substr(comma + 1));
        }
        parsed.emplace_back(id, std::move(chords));
    }
    reset_all_chords();
    for (auto &[id, chords] : parsed) {
        if (auto *command = mutable_find(id)) { command->chords = std::move(chords); ++report.applied; }
        else report.unknown.push_back(id);
    }
    return report;
}
std::vector<CommandInfo> default_command_table() {
    using namespace modifier;
    const auto chord = [](Key key, std::uint8_t modifiers = none) { return KeyChord{key, modifiers}; };
    const std::string global(context::global), viewport(context::viewport), outliner(context::outliner), flying(context::flying), playing(context::playing), held(context::held);
    std::vector<CommandInfo> table{
        {"File.NewScene", "New Scene", "Start an empty scene.", "File", global, {chord(Key::N, ctrl)}},
        {"File.OpenScene", "Open Scene...", "Choose a scene file and load it.", "File", global, {chord(Key::O, ctrl)}},
        {"File.LoadFromPath", "Load", "Load the scene named in the Scene file field.", "File", global, {}},
        {"File.Save", "Save", "Save the scene to the Scene file field's path.", "File", global, {chord(Key::S, ctrl)}},
        {"File.SaveAs", "Save As...", "Choose a file and save the scene there.", "File", global, {}},
        {"File.SaveAll", "Save All", "Save the scene. Materials save themselves when a control is released.", "File", global, {chord(Key::S, ctrl | shift)}},
        {"File.OpenSample", "Open Physics Shooter Sample", "Open the sample project and its scene.", "File", global, {}},
        {"File.NewProject", "New Empty Project...", "Create a project folder with an empty scene.", "File", global, {}},
        {"File.OpenProject", "Open Project...", "Open a project folder.", "File", global, {}},
        {"File.Quit", "Exit", "Close the editor, asking first if there are unsaved changes.", "File", global, {}},

        {"Edit.Undo", "Undo", "Undo the latest edit.", "Edit", global, {chord(Key::Z, ctrl)}, true},
        {"Edit.Redo", "Redo", "Redo the edit just undone.", "Edit", global, {chord(Key::Y, ctrl), chord(Key::Z, ctrl | shift)}, true},
        {"Edit.Duplicate", "Duplicate", "Copy the selection and its children.", "Edit", global, {chord(Key::D, ctrl)}},
        {"Edit.Copy", "Copy", "Copy the selection and its children to the clipboard.", "Edit", global, {chord(Key::C, ctrl)}},
        {"Edit.Paste", "Paste", "Add what was copied, at the root.", "Edit", global, {chord(Key::V, ctrl)}},
        {"Edit.Group", "Group", "Put the selection under a new empty entity.", "Edit", global, {chord(Key::G, ctrl)}},
        {"Edit.Rename", "Rename", "Rename the selected entity or folder in the hierarchy.", "Edit", global, {chord(Key::F2)}},
        {"Edit.Delete", "Delete", "Delete the selection and its children.", "Edit", global, {chord(Key::Delete)}},
        {"Edit.MoveToRoot", "Move to Root", "Take the selection out of its parent, keeping its place in the world.", "Edit", global, {}},
        {"Edit.SelectAll", "Select All", "Select every entity.", "Edit", global, {chord(Key::A, ctrl)}},
        {"Edit.Deselect", "Deselect", "Clear the selection.", "Edit", global, {chord(Key::Escape)}},
        {"Edit.SnapToFloor", "Snap to Floor", "Drop the selection onto the surface below it.", "Edit", global, {chord(Key::End)}},
        {"Edit.Hide", "Hide Selected", "Hide the selection in the editor. Play mode and saved content are not affected.", "Edit", global, {chord(Key::H)}},
        {"Edit.UnhideAll", "Unhide All", "Show everything hidden in the editor.", "Edit", global, {chord(Key::H, ctrl)}},

        {"Outliner.NewFolder", "New Folder", "Add a folder to the hierarchy. Folders only organize the editor's view.", "Edit", global, {}},

        {"Assets.ToggleDrawer", "Asset Drawer", "Slide the Asset Browser up over the editor, or away again.", "Assets", global, {chord(Key::Space, ctrl)}},
        {"Assets.CloseDrawer", "Close Asset Drawer", "Slide the drawer away.", "Assets", std::string(context::asset_browser), {chord(Key::Escape)}},
        {"Assets.FindInBrowser", "Find in Asset Browser", "Show the selected entity's asset.", "Assets", global, {chord(Key::B, ctrl)}},
        {"Assets.EditAsset", "Edit Asset", "Open the selected entity's material in the Material Editor.", "Assets", global, {chord(Key::E, ctrl)}},
        {"Asset.Assign", "Assign to Selection", "Give the selected asset to the selected entity.", "Assets", std::string(context::asset_browser), {}},
        {"Asset.AddToScene", "Add to Scene", "Place the selected model in the scene.", "Assets", std::string(context::asset_browser), {}},
        {"Asset.Edit", "Edit", "Open the selected material in the Material Editor.", "Assets", std::string(context::asset_browser), {}},
        {"Asset.Reload", "Reload", "Read the selected asset from disk again.", "Assets", std::string(context::asset_browser), {}},
        {"Asset.Rename", "Rename or Move", "Change the selected asset's path, keeping its references.", "Assets", std::string(context::asset_browser), {chord(Key::F2)}},
        {"Asset.ToggleFavorite", "Favorite", "Keep the selected asset in Favorites.", "Assets", std::string(context::asset_browser), {}},

        {"Create.Cube", "Cube", "Add a cube.", "Create", global, {}},
        {"Create.Sphere", "Sphere", "Add a sphere.", "Create", global, {}},
        {"Create.Plane", "Plane", "Add a 10 m plane.", "Create", global, {}},
        {"Create.Empty", "Empty", "Add an entity with no components.", "Create", global, {}},
        {"Create.DirectionalLight", "Directional Light", "Add a directional light.", "Create", global, {}},
        {"Create.PointLight", "Point Light", "Add a point light sized for the scene's exposure.", "Create", global, {}},
        {"Create.SpotLight", "Spot Light", "Add a spot light that casts shadows.", "Create", global, {}},
        {"Create.Camera", "Camera", "Add a camera. Play mode looks through the first primary one.", "Create", global, {}},
        {"Create.FallingCube", "Falling Cube", "Add a cube with a dynamic body and a collider.", "Create", global, {}},
        {"Create.FallingSphere", "Falling Sphere", "Add a sphere with a dynamic body and a collider.", "Create", global, {}},
        {"Create.Floor", "Floor", "Add a plane with a static collider.", "Create", global, {}},
        {"Create.ChildEmpty", "Child Empty", "Add an empty entity under the selection.", "Create", outliner, {}},
        {"Create.ChildCube", "Child Cube", "Add a cube under the selection.", "Create", outliner, {}},
        {"Create.ChildSphere", "Child Sphere", "Add a sphere under the selection.", "Create", outliner, {}},
        {"Create.ChildPointLight", "Child Point Light", "Add a point light under the selection.", "Create", outliner, {}},

        {"Viewport.FocusSelected", "Focus Selected", "Frame the selection in the viewport.", "Viewport", viewport, {chord(Key::F)}},
        {"Viewport.ToggleColliders", "Show Colliders", "Draw collider outlines over the view.", "Viewport", viewport, {}},
        {"Viewport.ShowGrid", "Show Grid", "Draw the grid.", "Viewport", viewport, {}},
        {"Viewport.ShowMarkers", "Show Light and Camera Markers", "Draw the clickable markers for lights and cameras.", "Viewport", viewport, {}},
        {"Viewport.ShowSkybox", "Show Skybox", "Draw the environment behind the scene.", "Viewport", viewport, {}},
        {"Viewport.ShowShadows", "Show Shadows", "Draw shadows.", "Viewport", viewport, {}},
        {"Viewport.ShowAmbientOcclusion", "Show Ambient Occlusion", "Darken creases and contact points.", "Viewport", viewport, {}},
        {"Viewport.ShowBloom", "Show Bloom", "Let bright areas glow.", "Viewport", viewport, {}},
        {"Viewport.GameView", "Game View", "Hide the gizmo, markers, grid and outlines, to see what the player sees.", "Viewport", viewport, {chord(Key::G)}},
        {"Viewport.Maximize", "Maximize Viewport", "Let the viewport fill the window.", "Viewport", viewport, {chord(Key::F11)}},
        {"Viewport.Perspective", "Perspective", "A perspective view.", "Viewport", viewport, {chord(Key::G, alt)}},
        {"Viewport.Front", "Front", "An orthographic view from the front, along -Z.", "Viewport", viewport, {chord(Key::H, alt)}},
        {"Viewport.Top", "Top", "An orthographic view from above.", "Viewport", viewport, {chord(Key::J, alt)}},
        {"Viewport.Side", "Side", "An orthographic view from the side, along -X.", "Viewport", viewport, {chord(Key::K, alt)}},
        {"Viewport.Wireframe", "Wireframe", "Not available yet: the renderer has no wireframe mode.", "Viewport", viewport, {chord(Key::Num2, alt)}},
        {"Viewport.Unlit", "Unlit", "Not available yet: the renderer has no unlit mode.", "Viewport", viewport, {chord(Key::Num3, alt)}},
        {"Viewport.Lit", "Lit", "The normal, fully lit view.", "Viewport", viewport, {chord(Key::Num4, alt)}},

        {"Gizmo.Move", "Move", "The move gizmo.", "Transform", viewport, {chord(Key::W)}},
        {"Gizmo.Rotate", "Rotate", "The rotate gizmo.", "Transform", viewport, {chord(Key::E)}},
        {"Gizmo.Scale", "Scale", "The scale gizmo.", "Transform", viewport, {chord(Key::R)}},
        {"Gizmo.Cycle", "Next Gizmo", "Cycle through move, rotate and scale.", "Transform", viewport, {chord(Key::Space)}},
        {"Gizmo.ToggleSpace", "World or Local", "Switch the gizmo between world and local axes.", "Transform", viewport, {chord(Key::Grave, ctrl)}},
        {"Snap.ToggleGrid", "Snap to Grid", "Moves go in whole grid steps.", "Transform", viewport, {}},
        {"Snap.ToggleRotation", "Snap Rotation", "Rotations go in whole steps.", "Transform", viewport, {}},
        {"Snap.ToggleScale", "Snap Scale", "Scaling goes in whole steps.", "Transform", viewport, {}},
        {"Snap.GridSmaller", "Smaller Grid", "The next smaller grid size.", "Transform", viewport, {chord(Key::LeftBracket)}},
        {"Snap.GridLarger", "Larger Grid", "The next larger grid size.", "Transform", viewport, {chord(Key::RightBracket)}},

        {"Camera.FlyForward", "Fly Forward", "While holding the right mouse button.", "Camera", flying, {chord(Key::W)}},
        {"Camera.FlyBack", "Fly Back", "While holding the right mouse button.", "Camera", flying, {chord(Key::S)}},
        {"Camera.FlyLeft", "Fly Left", "While holding the right mouse button.", "Camera", flying, {chord(Key::A)}},
        {"Camera.FlyRight", "Fly Right", "While holding the right mouse button.", "Camera", flying, {chord(Key::D)}},
        {"Camera.FlyDown", "Fly Down", "While holding the right mouse button.", "Camera", flying, {chord(Key::Q)}},
        {"Camera.FlyUp", "Fly Up", "While holding the right mouse button.", "Camera", flying, {chord(Key::E)}},
        {"Camera.FovOut", "Widen View", "While holding the right mouse button. It returns when the button comes up.", "Camera", flying, {chord(Key::Z)}},
        {"Camera.FovIn", "Narrow View", "While holding the right mouse button. It returns when the button comes up.", "Camera", flying, {chord(Key::C)}},

        {"Play.Play", "Play", "Run a copy of the scene, or carry on a paused run. Changes are discarded on Stop.", "Play", global, {chord(Key::P, alt)}},
        {"Play.Simulate", "Simulate", "Run the scene without possessing the player, and look around while it runs.", "Play", global, {chord(Key::S, alt)}},
        {"Play.Eject", "Eject or Possess", "Detach to the editor camera while the game runs, or go back to the player.", "Play", playing, {chord(Key::F8)}, false, true},
        {"Play.FreeMouse", "Free the Mouse", "Give the mouse back without pausing. A click on the view takes it again.", "Play", playing, {chord(Key::F1, shift)}, false, true},
        {"Play.Escape", "Pause or Stop", "With the mouse held, free it and pause. Otherwise stop.", "Play", playing, {chord(Key::Escape)}, false, true},
        {"Play.Resume", "Resume", "Continue the paused run.", "Play", global, {}},
        {"Play.Pause", "Pause", "Freeze the run.", "Play", global, {}},
        {"Play.Step", "Step", "Advance the paused run by one tick.", "Play", global, {}},
        {"Play.Stop", "Stop", "Discard the run and return to the editor scene.", "Play", global, {}},

        {"Palette.Commands", "Command Palette", "Find and run any command by typing.", "Find", global, {chord(Key::P, ctrl | shift)}, false, false, true},
        {"Palette.QuickOpen", "Quick Open", "Find any entity or asset by typing.", "Find", global, {chord(Key::P, ctrl)}, false, false, true},
        {"Console.Toggle", "Console", "Type commands and variables.", "Find", global, {chord(Key::Grave)}},

        {"Help.CheatSheet", "Shortcut Cheat Sheet", "Hold to see the shortcuts that work where you are.", "Help", held, {chord(Key::F1), chord(Key::Slash, shift)}},

        {"Window.UndoHistory", "Undo History", "Show every edit and jump to any of them.", "Window", global, {}},
        {"Window.KeyboardShortcuts", "Keyboard Shortcuts", "Search and change every shortcut.", "Window", global, {}},
        {"Window.Toolbar", "Toolbar", "Show or hide this panel.", "Window", global, {}},
        {"Window.Hierarchy", "Scene Hierarchy", "Show or hide this panel.", "Window", global, {}},
        {"Window.Inspector", "Inspector", "Show or hide this panel.", "Window", global, {}},
        {"Window.Lighting", "Lighting", "Show or hide this panel.", "Window", global, {}},
        {"Window.MaterialEditor", "Material Editor", "Show or hide this panel.", "Window", global, {}},
        {"Window.AssetBrowser", "Asset Browser", "Show or hide this panel.", "Window", global, {}},
        {"Window.Console", "Console", "Show or hide this panel.", "Window", global, {}},
        {"Window.MessageLog", "Message Log", "Show or hide this panel.", "Window", global, {}},
        {"Window.SaveLayout", "Save Layout...", "Keep the arrangement of panels under a name.", "Window", global, {}},
        {"Window.ResetLayout", "Reset Layout", "Put every panel back where it starts.", "Window", global, {}},
    };
    for (int slot = 0; slot < 10; ++slot) {
        const auto number = std::to_string(slot);
        const auto key = Key(int(Key::Num0) + slot);
        table.push_back({"Bookmark.Set" + number, "Set Bookmark " + number, "Remember this view in the scene's editor file.", "Bookmarks", viewport, {chord(key, ctrl)}});
        table.push_back({"Bookmark.Go" + number, "Go to Bookmark " + number, "Ease back to the remembered view.", "Bookmarks", viewport, {chord(key)}});
    }
    return table;
}
std::span<const MouseGesture> mouse_gestures() {
    static constexpr MouseGesture gestures[] = {
        {"RMB drag", "Look around"},
        {"RMB + Wheel", "Change camera speed"},
        {"Shift while flying", "Move three times as fast"},
        {"LMB drag", "Move forward and back, and turn"},
        {"LMB + RMB drag", "Move up, down and sideways"},
        {"MMB drag", "Pan"},
        {"Wheel", "Zoom"},
        {"Alt + LMB drag", "Orbit around the selection"},
        {"Alt + RMB drag", "Dolly"},
        {"Alt + MMB drag", "Pan"},
        {"Click", "Select"},
        {"Ctrl + Click", "Add to or take from the selection"},
        {"Alt + drag a gizmo", "Copy the selection and move the copies"},
        {"Shift + drag a gizmo", "The camera follows what moves"},
        {"Hold V while dragging a gizmo", "Snap to the nearest vertex"},
        {"Esc while dragging a gizmo", "Cancel the drag"},
    };
    return gestures;
}
std::string shortcuts_markdown(std::span<const CommandInfo> commands) {
    std::ostringstream out;
    out << "# Editor shortcuts\n\nGenerated by `engine_editor --dump-shortcuts docs/EDITOR_SHORTCUTS.md`; a test fails when this file is out of date.\n"
           "Every key can be changed in Window > Keyboard Shortcuts. A shortcut works in its context, and Global ones work everywhere.\n"
           "None fire while a dialog is open or a drag is in progress. While the game holds the mouse, only the ways out fire: Esc, F8 and Shift+F1.\n"
           "While a text field has the keyboard, only Ctrl+P and Ctrl+Shift+P fire.\n";
    // A code span holding a backtick needs longer fences around it.
    const auto code = [](const std::string &text) { return text.find('`') == std::string::npos ? "`" + text + "`" : "`` " + text + " ``"; };
    std::vector<std::string> categories;
    for (const auto &command : commands)
        if (std::ranges::find(categories, command.category) == categories.end()) categories.push_back(command.category);
    for (const auto &category : categories) {
        out << "\n## " << category << "\n\n| Command | Keys | Context | ID |\n|---|---|---|---|\n";
        for (const auto &command : commands) {
            if (command.category != category) continue;
            std::string keys;
            for (const auto chord : command.default_chords) keys += (keys.empty() ? "" : ", ") + code(to_string(chord));
            out << "| " << command.label << " | " << keys << " | " << command.context << " | `" << command.id << "` |\n";
        }
    }
    out << "\n## Mouse\n\nThese cannot be changed yet.\n\n| Input | Action |\n|---|---|\n";
    for (const auto &gesture : mouse_gestures()) out << "| " << gesture.input << " | " << gesture.action << " |\n";
    return out.str();
}
} // namespace engine::editor
