#include "engine/assets/virtual_paths.h"

#include "engine/assets/importers.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace engine::assets {
namespace {

constexpr std::size_t max_redirector_bytes = 64 * 1024;
constexpr std::size_t max_mount_name = 64;

bool mount_character(char c) noexcept
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

// "/Game" -> "/game", or nothing for a malformed mount point.
std::optional<std::string> folded_mount(std::string_view mount_point)
{
	if (mount_point.size() < 2 || mount_point.size() > max_mount_name + 1 || mount_point.front() != '/') {
		return std::nullopt;
	}
	if (!std::all_of(mount_point.begin() + 1, mount_point.end(), mount_character)) {
		return std::nullopt;
	}
	return fold_case(mount_point);
}

// The trimmed, space-separated words of a line; "key = value" has three.
std::vector<std::string_view> words(std::string_view line)
{
	std::vector<std::string_view> out;
	std::size_t at = 0;
	while (at < line.size()) {
		while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) {
			++at;
		}
		const std::size_t start = at;
		while (at < line.size() && line[at] != ' ' && line[at] != '\t') {
			++at;
		}
		if (at > start) {
			out.push_back(line.substr(start, at - start));
		}
	}
	return out;
}

Error parse_failure(std::size_t line, const std::string& message)
{
	return fail(ErrorCode::ParseError, "redirector line " + std::to_string(line) + ": " + message);
}

} // namespace

std::string fold_case(std::string_view text)
{
	std::string out(text);
	for (char& c : out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return out;
}

std::string package_path_of(std::string_view asset_path)
{
	const std::size_t slash = asset_path.rfind('/');
	const std::size_t dot = asset_path.rfind('.');
	// A dot in a folder's name, or a file named ".hidden", is not an extension.
	if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) || dot == (slash == std::string_view::npos ? 0 : slash + 1)) {
		return std::string(asset_path);
	}
	return std::string(asset_path.substr(0, dot));
}

std::string package_name(std::string_view mount_point, std::string_view asset_path)
{
	return std::string(mount_point) + "/" + package_path_of(asset_path);
}

Result<Redirector> parse_redirector(std::string_view text, std::string path)
{
	if (text.size() > max_redirector_bytes) {
		return fail(ErrorCode::ParseError, "redirector file exceeds 64 KiB");
	}
	auto own = normalize_asset_path(path);
	if (!own || asset_type_for(own.value()) == AssetType::Unknown) {
		return fail(ErrorCode::InvalidArgument, "a redirector must stand at an asset's path: " + path);
	}
	Redirector redirector;
	redirector.path = std::move(own.value());
	bool has_version = false;
	bool has_target = false;
	bool has_id = false;
	std::size_t line_number = 0;
	std::string_view rest = text;
	while (!rest.empty()) {
		++line_number;
		const std::size_t newline = rest.find('\n');
		std::string_view line = rest.substr(0, newline);
		rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		const auto w = words(line);
		if (w.empty() || w[0].front() == '#') {
			continue;
		}
		if (w.size() != 3 || w[1] != "=") {
			return parse_failure(line_number, "expected 'key = value'");
		}
		const auto once = [](bool& seen) { return !std::exchange(seen, true); };
		if (w[0] == "version") {
			if (!once(has_version) || w[2] != "1") {
				return parse_failure(line_number, "unsupported or duplicate version");
			}
		} else if (w[0] == "target") {
			if (!once(has_target)) {
				return parse_failure(line_number, "target appears twice");
			}
			auto target = normalize_asset_path(w[2]);
			if (!target) {
				return fail(ErrorCode::InvalidArgument, "redirector " + redirector.path + " has an unusable target: " + target.error().message);
			}
			redirector.target = std::move(target.value());
		} else if (w[0] == "id") {
			const auto id = parse_asset_id(w[2]);
			if (!once(has_id) || !id) {
				return parse_failure(line_number, "id must be 16 hex digits, nonzero, and appear once");
			}
			redirector.id = *id;
		} else {
			return parse_failure(line_number, "unknown key '" + std::string(w[0]) + "'");
		}
	}
	if (!has_version) {
		return fail(ErrorCode::ParseError, "redirector " + redirector.path + " has no version");
	}
	if (!has_target) {
		return fail(ErrorCode::ParseError, "redirector " + redirector.path + " has no target");
	}
	if (asset_type_for(redirector.target) != asset_type_for(redirector.path)) {
		return fail(ErrorCode::InvalidArgument, "redirector " + redirector.path + " points at another type of asset: " + redirector.target);
	}
	if (fold_case(redirector.target) == fold_case(redirector.path)) {
		return fail(ErrorCode::InvalidArgument, "redirector " + redirector.path + " points at itself");
	}
	return redirector;
}

std::string write_redirector(const Redirector& redirector)
{
	std::string out = "# GameEngine redirector: the asset that was here moved. Fix Up Redirectors in the editor\n"
					  "# removes this file once nothing names the old path.\nversion = 1\ntarget = " +
		redirector.target + "\n";
	if (!redirector.id.is_null()) {
		out += "id = " + to_string(redirector.id) + "\n";
	}
	return out;
}

Result<void> VirtualFileSystem::mount(std::string_view mount_point, std::filesystem::path root)
{
	const auto folded = folded_mount(mount_point);
	if (!folded) {
		return fail(ErrorCode::InvalidArgument, "a mount point is a slash and 1 to 64 letters, digits, '_' or '-': " + std::string(mount_point));
	}
	std::unique_lock lock(mutex_);
	std::erase_if(mounts_, [&](const Mount& m) { return m.folded == *folded; });
	Mount added;
	added.name = std::string(mount_point);
	added.folded = *folded;
	added.root = std::move(root);
	mounts_.push_back(std::move(added));
	return {};
}

void VirtualFileSystem::unmount(std::string_view mount_point)
{
	const std::string folded = fold_case(mount_point);
	std::unique_lock lock(mutex_);
	std::erase_if(mounts_, [&](const Mount& m) { return m.folded == folded; });
}

std::vector<std::string> VirtualFileSystem::mount_points() const
{
	std::shared_lock lock(mutex_);
	std::vector<std::string> out;
	for (const Mount& m : mounts_) {
		out.push_back(m.name);
	}
	return out;
}

const VirtualFileSystem::Mount* VirtualFileSystem::find_mount(std::string_view folded_name) const
{
	const auto found = std::find_if(mounts_.begin(), mounts_.end(), [&](const Mount& m) { return m.folded == folded_name; });
	return found == mounts_.end() ? nullptr : &*found;
}

Result<std::vector<std::string>> VirtualFileSystem::index(std::string_view mount_point, std::span<const AssetMetadata> assets,
	std::span<const Redirector> redirectors)
{
	// Built outside the lock, so resolving goes on meanwhile; only the swap below is exclusive.
	Mount built;
	built.folded = fold_case(mount_point);
	std::vector<std::string> conflicts;
	for (const AssetMetadata& meta : assets) {
		const Entry entry{meta.path, meta.id, meta.type, false, {}};
		built.by_file[fold_case(meta.path)] = entry;
		built.by_package[fold_case(package_path_of(meta.path))].push_back(entry);
	}
	for (const auto& [key, entries] : built.by_package) {
		if (entries.size() < 2) {
			continue;
		}
		std::string names;
		for (const Entry& e : entries) {
			names += (names.empty() ? "" : " and ") + e.asset_path;
		}
		conflicts.push_back(std::string(mount_point) + "/" + package_path_of(entries.front().asset_path) + " names both " + names +
			"; add the extension to name one");
	}
	for (const Redirector& r : redirectors) {
		const std::string file = fold_case(r.path), package = fold_case(package_path_of(r.path));
		if (built.by_file.contains(file) || built.by_package.contains(package)) {
			// An asset has the name now; it wins, and the redirector is only left for Fix Up Redirectors to remove.
			const auto& owner = built.by_file.contains(file) ? built.by_file.at(file) : built.by_package.at(package).front();
			if (owner.redirector) {
				conflicts.push_back("redirectors " + owner.asset_path + " and " + r.path + " share a name");
			} else {
				conflicts.push_back("redirector " + r.path + " is hidden by the asset " + owner.asset_path);
			}
			continue;
		}
		const Entry entry{r.path, r.id, asset_type_for(r.path), true, r.target};
		built.by_file[file] = entry;
		built.by_package[package].push_back(entry);
	}
	for (const AssetMetadata& meta : assets) {
		const bool shared = built.by_package.at(fold_case(package_path_of(meta.path))).size() > 1;
		built.names[meta.id] = std::string(mount_point) + "/" + (shared ? meta.path : package_path_of(meta.path));
	}

	std::unique_lock lock(mutex_);
	const auto target = std::find_if(mounts_.begin(), mounts_.end(), [&](const Mount& m) { return m.folded == built.folded; });
	if (target == mounts_.end()) {
		return fail(ErrorCode::NotFound, "nothing is mounted at " + std::string(mount_point));
	}
	// Names keep the mount's own spelling.
	for (auto& [id, name] : built.names) {
		name = target->name + name.substr(mount_point.size());
	}
	target->by_package = std::move(built.by_package);
	target->by_file = std::move(built.by_file);
	target->names = std::move(built.names);
	return conflicts;
}

Result<PackageResolution> VirtualFileSystem::resolve(std::string_view name) const
{
	std::shared_lock lock(mutex_);
	PackageResolution out;
	std::string current(name);
	std::set<std::string> visited; // Folded names of the redirectors passed.
	for (;;) {
		std::replace(current.begin(), current.end(), '\\', '/');
		if (current.size() < 2 || current.front() != '/') {
			return fail(ErrorCode::InvalidArgument, "a package name starts with a mount point, as in /Game/Textures/Brick: " + current);
		}
		const std::size_t slash = current.find('/', 1);
		if (slash == std::string::npos || slash + 1 >= current.size()) {
			return fail(ErrorCode::InvalidArgument, current + " names a mount point, not an asset");
		}
		const auto folded_name = folded_mount(std::string_view(current).substr(0, slash));
		if (!folded_name) {
			return fail(ErrorCode::InvalidArgument, current + " does not start with a mount point");
		}
		const Mount* mount = find_mount(*folded_name);
		if (!mount) {
			return fail(ErrorCode::NotFound, "nothing is mounted at " + current.substr(0, slash));
		}
		auto relative = normalize_asset_path(std::string_view(current).substr(slash + 1));
		if (!relative) {
			return fail(ErrorCode::InvalidArgument, current + " is not a usable name: " + relative.error().message);
		}
		const std::string key = fold_case(relative.value());
		const Entry* hit = nullptr;
		if (const auto file = mount->by_file.find(key); file != mount->by_file.end()) {
			hit = &file->second;
		} else if (const auto package = mount->by_package.find(key); package != mount->by_package.end()) {
			if (package->second.size() > 1) {
				std::string names;
				for (const Entry& e : package->second) {
					names += (names.empty() ? "" : ", ") + mount->name + "/" + e.asset_path;
				}
				return fail(ErrorCode::Conflict, current + " could be " + names + "; add the extension");
			}
			hit = &package->second.front();
		}
		if (!hit) {
			if (out.redirects.empty()) {
				return fail(ErrorCode::NotFound, current + " names no asset");
			}
			return fail(ErrorCode::NotFound, "redirector " + out.redirects.back() + " points at " + current + ", which names no asset");
		}
		if (!hit->redirector) {
			out.mount_point = mount->name;
			out.asset_path = hit->asset_path;
			out.file = mount->root / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(hit->asset_path.data()), hit->asset_path.size()));
			out.id = hit->id;
			out.type = hit->type;
			return out;
		}
		const std::string here = mount->name + "/" + package_path_of(hit->asset_path);
		if (!visited.insert(mount->folded + "/" + fold_case(hit->asset_path)).second) {
			std::string loop;
			for (const std::string& step : out.redirects) {
				loop += step + " -> ";
			}
			return fail(ErrorCode::Cycle, "redirectors go round in a loop: " + loop + here);
		}
		if (out.redirects.size() >= max_redirects) {
			return fail(ErrorCode::InvalidState, "more than " + std::to_string(max_redirects) + " redirectors in a row from " + std::string(name));
		}
		out.redirects.push_back(here);
		current = mount->name + "/" + hit->target;
	}
}

std::optional<std::string> VirtualFileSystem::name_of(AssetId id) const
{
	std::shared_lock lock(mutex_);
	for (const Mount& m : mounts_) {
		if (const auto found = m.names.find(id); found != m.names.end()) {
			return found->second;
		}
	}
	return std::nullopt;
}

} // namespace engine::assets
