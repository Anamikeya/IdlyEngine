#pragma once

// Finding scripts by package name, as scenes and `require` name them.

#include <engine/assets/virtual_paths.h>
#include <engine/scripting/script_types.h>

#include <map>
#include <string>
#include <string_view>

namespace engine::scripting {

// Scripts through the virtual file system: "/Game/Scripts/Gate", "/game/scripts/gate" or "/Game/Scripts/Gate.lua",
// following redirectors. The canonical name is the package name ("/Game/Scripts/Gate"). InvalidArgument for an asset
// that is not a script. The file system must outlive the loader.
[[nodiscard]] ScriptLoader package_loader(const assets::VirtualFileSystem& files);

// Scripts held in memory under their names, for tests and tools. The map must outlive the loader.
[[nodiscard]] ScriptLoader memory_loader(const std::map<std::string, std::string, std::less<>>& scripts);

} // namespace engine::scripting
