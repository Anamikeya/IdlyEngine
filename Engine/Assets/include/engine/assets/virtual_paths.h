#pragma once

// Long package names and redirectors, after Unreal Engine's (Milestone 5, slice 7).
//
// A package name is a mount point and an asset's path under that mount's root, without the file's extension:
// "/Game/Textures/Brick" names Textures/Brick.png in the project's Assets folder. Names compare without regard to ASCII
// case on every platform, so "/game/textures/brick" finds the same asset on Windows and on Linux, while the file keeps
// its own case on disk. Two assets in one folder whose names differ only by extension or case cannot share a name: the
// bare name is reported as a conflict, and the name with the extension ("/Game/Textures/Brick.png") still finds each.
//
// A redirector is what an asset leaves behind when the editor moves it: a small "<old path>.redirect" file naming where
// it went. Resolving a name that lands on one follows it, and every redirector after it, to the asset.
//
// References in scenes and materials are not package names. They keep a stable id and a relative path, and the id wins
// (asset_types.h). Package names are how people, tools and scripts name assets.

#include "engine/assets/asset_types.h"

#include <engine/core/Result.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// The placeholder a moved asset leaves at its old path.
struct Redirector {
	// Its own path under the root: the moved asset's old path, extension included ("Textures/Brick.png"). The file on
	// disk is this plus redirector_extension.
	std::string path;
	// Where the asset went, under the same root ("Textures/Walls/Brick.png"). It may be another redirector's path.
	std::string target;
	// The asset that moved; the null id when the file does not say.
	AssetId id;

	friend bool operator==(const Redirector&, const Redirector&) = default;
};

inline constexpr std::string_view redirector_extension = ".redirect";

// A redirector file is text, like a .meta:
//   # GameEngine redirector: ...
//   version = 1
//   target = Textures/Walls/Brick.png
//   id = 0123456789abcdef
// `path` is where the file was found, without redirector_extension. ParseError for anything else, and InvalidArgument
// for a path or target that is not a usable asset path, or a target of another asset type.
Result<Redirector> parse_redirector(std::string_view text, std::string path);
[[nodiscard]] std::string write_redirector(const Redirector& redirector);

// ASCII letters to lower case. Every other byte, UTF-8 included, is unchanged.
[[nodiscard]] std::string fold_case(std::string_view text);
// "Textures/Brick.png" -> "Textures/Brick": an asset path without its file's extension.
[[nodiscard]] std::string package_path_of(std::string_view asset_path);
// "/Game" and "Textures/Brick.png" -> "/Game/Textures/Brick".
[[nodiscard]] std::string package_name(std::string_view mount_point, std::string_view asset_path);

struct PackageResolution {
	std::string mount_point;    // As mounted: "/Game".
	std::string asset_path;     // Under the mount's root, as catalogued: "Textures/Walls/Brick.png".
	std::filesystem::path file; // The mount's root joined with asset_path.
	AssetId id;
	AssetType type = AssetType::Unknown;
	// The names of the redirectors followed, in order. Empty when the name found the asset directly.
	std::vector<std::string> redirects;
};

// Mount points and what each holds. Every member is safe to call from any thread: resolving takes a shared lock, and
// mounting or indexing an exclusive one.
class VirtualFileSystem {
public:
	static constexpr std::size_t max_redirects = 32;

	// `mount_point` is a slash and 1 to 64 letters, digits, '_' or '-': "/Game". It replaces a mount of the same name in
	// any case, with an empty index. The root need not exist yet.
	Result<void> mount(std::string_view mount_point, std::filesystem::path root);
	void unmount(std::string_view mount_point);
	[[nodiscard]] std::vector<std::string> mount_points() const;

	// Replaces what a mount holds with a catalogue's assets and redirectors. Returns what could not be indexed as it
	// stands: bare names two assets share, and redirectors hidden by an asset at their path or name (the asset wins).
	// NotFound when nothing is mounted there.
	Result<std::vector<std::string>> index(std::string_view mount_point, std::span<const AssetMetadata> assets,
		std::span<const Redirector> redirectors);

	// Finds the asset a name means, following redirectors: "/Game/Textures/Brick", "/game/textures/brick" or
	// "/Game/Textures/Brick.png". InvalidArgument for a malformed name, Conflict for a bare name two assets share,
	// NotFound when nothing has the name (or a redirector points at nothing), Cycle for a redirector loop, and
	// InvalidState for more than max_redirects redirectors in a row.
	[[nodiscard]] Result<PackageResolution> resolve(std::string_view name) const;

	// The name to show for an asset: its bare package name, or the name with its extension when the bare one is a
	// conflict. Nullopt when no mount holds it.
	[[nodiscard]] std::optional<std::string> name_of(AssetId id) const;

private:
	struct Entry {
		std::string asset_path; // As catalogued.
		AssetId id;
		AssetType type = AssetType::Unknown;
		bool redirector = false;
		std::string target; // A redirector's.
	};
	struct Mount {
		std::string name;   // As given: "/Game".
		std::string folded; // "/game".
		std::filesystem::path root;
		std::map<std::string, std::vector<Entry>> by_package; // Folded package path; more than one entry is a conflict.
		std::map<std::string, Entry> by_file;                 // Folded asset path, extension included.
		std::map<AssetId, std::string> names;                 // What name_of() returns.
	};
	const Mount* find_mount(std::string_view folded_name) const;

	mutable std::shared_mutex mutex_;
	std::vector<Mount> mounts_;
};

} // namespace engine::assets
