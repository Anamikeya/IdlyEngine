#include "engine/assets/asset_database.h"

#include "engine/assets/importers.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string_view>
#include <system_error>

namespace engine::assets {
namespace fs = std::filesystem;

namespace {

std::string utf8(const fs::path& path)
{
	const auto u8 = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path from_utf8(const std::string& text)
{
	return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string lowercase(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(),
		[](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
	return text;
}

bool ends_with_meta(const std::string& path)
{
	return path.size() > 5 && lowercase(path.substr(path.size() - 5)) == ".meta";
}

bool inside(const fs::path& root, const fs::path& candidate)
{
	const fs::path relative = candidate.lexically_relative(root);
	return !relative.empty() && *relative.begin() != ".." && !relative.is_absolute();
}

bool has_extension(const std::string& path, std::string_view extension)
{
	return path.size() >= extension.size() && lowercase(path.substr(path.size() - extension.size())) == extension;
}

} // namespace

const char* to_string(AssetState state) noexcept
{
	switch (state) {
	case AssetState::Unknown:
		return "unknown";
	case AssetState::Unloaded:
		return "unloaded";
	case AssetState::Loaded:
		return "loaded";
	case AssetState::Failed:
		return "failed";
	case AssetState::Missing:
		return "missing";
	}
	return "unknown";
}

struct AssetDatabase::Impl {
	struct Entry {
		AssetMetadata meta;
		AssetState state = AssetState::Unloaded;
		std::uint32_t revision = 1;
		std::shared_ptr<const MeshData> mesh;
		std::shared_ptr<const TextureData> texture;

		[[nodiscard]] bool cached() const noexcept { return mesh || texture; }
		void drop() noexcept
		{
			mesh.reset();
			texture.reset();
		}
	};

	fs::path root;
	ImportLimits limits;
	std::map<AssetId, Entry> by_id;
	std::map<std::string, AssetId> by_path;
	std::vector<AssetIssue> issues;

	fs::path source_of(const AssetMetadata& meta) const { return root / from_utf8(meta.path); }

	// All reads go through here: the canonical target must stay under the canonical root (no symlink escapes).
	Result<std::vector<std::uint8_t>> read(const fs::path& path, std::uint64_t max_bytes) const
	{
		std::error_code ec;
		if (!fs::is_regular_file(path, ec)) {
			return fail(ErrorCode::NotFound, "source file is missing: " + utf8(path));
		}
		const fs::path canonical = fs::weakly_canonical(path, ec);
		if (ec) {
			return fail(ErrorCode::NotFound, "source file cannot be resolved: " + utf8(path));
		}
		if (!inside(root, canonical)) {
			return fail(ErrorCode::PermissionDenied, "source file resolves outside the asset root");
		}
		const std::uintmax_t size = fs::file_size(canonical, ec);
		if (ec) {
			return fail(ErrorCode::NotFound, "source file cannot be read: " + utf8(path));
		}
		if (size > max_bytes) {
			return fail(ErrorCode::InvalidArgument, "source file exceeds the size limit");
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		std::ifstream in(canonical, std::ios::binary);
		if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
			return fail(ErrorCode::NotFound, "source file cannot be read: " + utf8(path));
		}
		return bytes;
	}

	Result<std::shared_ptr<const MeshData>> import_mesh(const AssetMetadata& meta) const
	{
		auto bytes = read(source_of(meta), limits.max_source_bytes);
		if (!bytes) {
			return bytes.error();
		}
		const auto& b = bytes.value();
		auto mesh = has_extension(meta.path, ".glb")
			? import_glb(b, meta.settings, limits)
			: import_obj(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), meta.settings, limits);
		if (!mesh) {
			return mesh.error();
		}
		return std::make_shared<const MeshData>(std::move(mesh.value()));
	}

	Result<std::shared_ptr<const TextureData>> import_texture(const AssetMetadata& meta) const
	{
		auto bytes = read(source_of(meta), limits.max_source_bytes);
		if (!bytes) {
			return bytes.error();
		}
		auto texture = has_extension(meta.path, ".ppm")   ? import_ppm(bytes.value(), meta.settings, limits)
			: has_extension(meta.path, ".tga") ? import_tga(bytes.value(), meta.settings, limits)
											   : import_image(bytes.value(), meta.settings, limits);
		if (!texture) {
			return texture.error();
		}
		return std::make_shared<const TextureData>(std::move(texture.value()));
	}

	template <typename T, typename Import>
	Result<LoadedAsset<T>> load(AssetId id, AssetType type, std::shared_ptr<const T> Entry::*slot, Import import)
	{
		const auto it = by_id.find(id);
		if (it == by_id.end()) {
			return fail(ErrorCode::NotFound, "unknown asset id " + to_string(id));
		}
		Entry& e = it->second;
		if (e.meta.type != type) {
			return fail(ErrorCode::InvalidArgument,
				"asset " + e.meta.path + " is a " + assets::to_string(e.meta.type) + ", not a " + assets::to_string(type));
		}
		if (e.*slot) {
			return LoadedAsset<T>{id, e.revision, e.*slot};
		}
		auto data = import(e.meta);
		if (!data) {
			e.state = data.error().code == ErrorCode::NotFound ? AssetState::Missing : AssetState::Failed;
			Error error = data.error();
			error.message = e.meta.path + ": " + error.message;
			return error;
		}
		e.*slot = std::move(data.value());
		e.state = AssetState::Loaded;
		return LoadedAsset<T>{id, e.revision, e.*slot};
	}
};

AssetDatabase::AssetDatabase(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AssetDatabase::~AssetDatabase() = default;
AssetDatabase::AssetDatabase(AssetDatabase&& other) noexcept = default;
AssetDatabase& AssetDatabase::operator=(AssetDatabase&& other) noexcept = default;

Result<AssetDatabase> AssetDatabase::open(const fs::path& root, const ImportLimits& limits)
{
	std::error_code ec;
	if (!fs::is_directory(root, ec)) {
		return fail(ErrorCode::NotFound, "asset root is not a directory: " + utf8(root));
	}
	auto impl = std::make_unique<Impl>();
	impl->root = fs::canonical(root, ec);
	if (ec) {
		return fail(ErrorCode::NotFound, "asset root cannot be resolved: " + utf8(root));
	}
	impl->limits = limits;
	return AssetDatabase(std::move(impl));
}

Result<std::size_t> AssetDatabase::scan()
{
	Impl& db = *impl_;
	std::vector<AssetIssue> issues;
	std::set<std::string> metas;      // Relative paths of sidecars, without ".meta".
	std::vector<AssetMetadata> found; // Supported assets with a valid path.

	std::error_code ec;
	fs::recursive_directory_iterator it(db.root, fs::directory_options::skip_permission_denied, ec);
	if (ec) {
		return fail(ErrorCode::NotFound, "asset root cannot be listed: " + ec.message());
	}
	for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
		if (ec) {
			return fail(ErrorCode::Internal, "asset scan failed: " + ec.message());
		}
		if (!it->is_regular_file(ec)) {
			continue;
		}
		const std::string relative = utf8(it->path().lexically_relative(db.root));
		if (ends_with_meta(relative)) {
			metas.insert(relative.substr(0, relative.size() - 5));
			continue;
		}
		auto path = normalize_asset_path(relative);
		if (!path) {
			issues.push_back({relative, path.error().message});
			continue;
		}
		AssetMetadata meta;
		meta.path = std::move(path.value());
		meta.type = asset_type_for(meta.path);
		if (meta.type == AssetType::Unknown) {
			issues.push_back({meta.path, "unsupported format (supported: .glb, .obj, .png, .jpg, .jpeg, .tga, .ppm)"});
			continue;
		}
		meta.source_bytes = it->file_size(ec);
		found.push_back(std::move(meta));
	}

	std::set<std::string> asset_paths;
	std::vector<AssetMetadata> valid;
	for (AssetMetadata& meta : found) {
		asset_paths.insert(meta.path);
		meta.id = asset_id_from_path(meta.path);
		if (metas.contains(meta.path)) {
			auto text = db.read(db.root / from_utf8(meta.path + ".meta"), 64 * 1024);
			if (!text) {
				issues.push_back({meta.path + ".meta", text.error().message});
				continue;
			}
			const auto& b = text.value();
			auto parsed = parse_meta(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
			if (!parsed) {
				issues.push_back({meta.path + ".meta", parsed.error().message});
				continue;
			}
			meta.has_meta = true;
			meta.settings = parsed.value().settings;
			if (parsed.value().id) {
				meta.id = *parsed.value().id;
			}
		}
		valid.push_back(std::move(meta));
	}
	for (const std::string& meta : metas) {
		if (!asset_paths.contains(meta)) {
			issues.push_back({meta + ".meta", "sidecar has no matching asset"});
		}
	}

	// Paths differing only by case collide on case-insensitive filesystems; ids must be unique.
	std::map<std::string, std::size_t> by_case;
	std::map<AssetId, std::size_t> by_id;
	for (const AssetMetadata& meta : valid) {
		++by_case[lowercase(meta.path)];
		++by_id[meta.id];
	}

	std::map<AssetId, Impl::Entry> entries;
	std::map<std::string, AssetId> paths;
	for (AssetMetadata& meta : valid) {
		if (by_case[lowercase(meta.path)] > 1) {
			issues.push_back({meta.path, "path differs from another asset only by case"});
			continue;
		}
		if (by_id[meta.id] > 1) {
			issues.push_back({meta.path, "asset id " + to_string(meta.id) + " is used by more than one asset"});
			continue;
		}
		Impl::Entry entry;
		const auto previous = db.by_id.find(meta.id);
		if (previous != db.by_id.end() && previous->second.meta.path == meta.path &&
			previous->second.meta.settings == meta.settings) {
			entry = std::move(previous->second);
			if (entry.state == AssetState::Missing) {
				entry.state = entry.cached() ? AssetState::Loaded : AssetState::Unloaded;
			}
		}
		entry.meta = meta;
		paths.emplace(meta.path, meta.id);
		entries.emplace(meta.id, std::move(entry));
	}

	std::sort(issues.begin(), issues.end(), [](const AssetIssue& a, const AssetIssue& b) { return a.path < b.path; });
	db.by_id = std::move(entries);
	db.by_path = std::move(paths);
	db.issues = std::move(issues);
	return db.by_id.size();
}

std::span<const AssetIssue> AssetDatabase::issues() const noexcept
{
	return impl_->issues;
}

std::vector<AssetMetadata> AssetDatabase::assets() const
{
	std::vector<AssetMetadata> out;
	out.reserve(impl_->by_path.size());
	for (const auto& [path, id] : impl_->by_path) {
		(void)path;
		out.push_back(impl_->by_id.at(id).meta);
	}
	return out;
}

std::optional<AssetMetadata> AssetDatabase::find(AssetId id) const
{
	const auto it = impl_->by_id.find(id);
	return it == impl_->by_id.end() ? std::nullopt : std::optional<AssetMetadata>(it->second.meta);
}

std::optional<AssetMetadata> AssetDatabase::find(std::string_view path) const
{
	auto normalized = normalize_asset_path(path);
	if (!normalized) {
		return std::nullopt;
	}
	const auto it = impl_->by_path.find(normalized.value());
	return it == impl_->by_path.end() ? std::nullopt : find(it->second);
}

Result<MeshAsset> AssetDatabase::load_mesh(AssetId id)
{
	return impl_->load<MeshData>(id, AssetType::Mesh, &Impl::Entry::mesh,
		[this](const AssetMetadata& meta) { return impl_->import_mesh(meta); });
}

Result<TextureAsset> AssetDatabase::load_texture(AssetId id)
{
	return impl_->load<TextureData>(id, AssetType::Texture, &Impl::Entry::texture,
		[this](const AssetMetadata& meta) { return impl_->import_texture(meta); });
}

Result<AssetId> AssetDatabase::import_file(const fs::path& source)
{
	std::error_code ec;
	if (!fs::is_regular_file(source, ec)) {
		return fail(ErrorCode::NotFound, "file not found: " + utf8(source));
	}
	const std::string name = utf8(source.filename());
	const AssetType type = asset_type_for(name);
	if (type == AssetType::Unknown) {
		const std::string extension = utf8(source.extension());
		return fail(ErrorCode::InvalidArgument, "unsupported file type '" + extension + "'" +
			(lowercase(extension) == ".gltf" ? " (re-export the model as a self-contained .glb)" : "") +
			"; supported: .glb, .obj, .png, .jpg, .jpeg, .tga, .ppm");
	}
	const auto load = [&](AssetId id) -> Result<void> {
		if (type == AssetType::Mesh) {
			auto mesh = load_mesh(id);
			return mesh ? Result<void>{} : Result<void>(mesh.error());
		}
		auto texture = load_texture(id);
		return texture ? Result<void>{} : Result<void>(texture.error());
	};
	// Already in the asset folder: nothing to copy.
	const fs::path canonical = fs::weakly_canonical(source, ec);
	if (!ec && inside(impl_->root, canonical)) {
		if (auto scanned = scan(); !scanned) {
			return scanned.error();
		}
		const auto existing = find(utf8(canonical.lexically_relative(impl_->root)));
		if (!existing) {
			return fail(ErrorCode::InvalidArgument, name + " is in the asset folder but could not be catalogued");
		}
		if (auto loaded = load(existing->id); !loaded) {
			return loaded.error();
		}
		return existing->id;
	}
	const std::uintmax_t size = fs::file_size(source, ec);
	if (ec) {
		return fail(ErrorCode::NotFound, "file cannot be read: " + utf8(source));
	}
	if (size > impl_->limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, name + " exceeds the size limit");
	}
	auto normalized = normalize_asset_path(name);
	if (!normalized) {
		return fail(ErrorCode::InvalidArgument, "the file name cannot be used for an asset: " + normalized.error().message);
	}
	// A free name: "model.glb", then "model_1.glb", ...
	const fs::path stem = from_utf8(normalized.value()).stem(), extension = from_utf8(normalized.value()).extension();
	fs::path target = impl_->root / from_utf8(normalized.value());
	for (int i = 1; fs::exists(target, ec) || fs::exists(fs::path(target) += ".meta", ec); ++i) {
		target = impl_->root / (fs::path(stem) += "_" + std::to_string(i));
		target += extension;
	}
	fs::path meta_path = target;
	meta_path += ".meta";
	const auto undo = [&] {
		std::error_code ignored;
		fs::remove(target, ignored);
		fs::remove(meta_path, ignored);
		(void)scan();
	};
	if (!fs::copy_file(source, target, ec) || ec) {
		return fail(ErrorCode::InvalidState, "the file could not be copied into the asset folder: " + ec.message());
	}
	std::random_device random;
	AssetId id;
	do {
		id.value = (static_cast<std::uint64_t>(random()) << 32) | random();
	} while (id.is_null() || impl_->by_id.contains(id));
	{
		std::ofstream out(meta_path, std::ios::binary);
		out << write_meta(MetaFile{id, {}});
		if (!out) {
			undo();
			return fail(ErrorCode::InvalidState, "the asset's .meta file could not be written");
		}
	}
	if (auto scanned = scan(); !scanned) {
		undo();
		return scanned.error();
	}
	if (!find(id)) {
		const std::string relative = utf8(target.lexically_relative(impl_->root));
		std::string reason = "it could not be catalogued";
		for (const AssetIssue& issue : impl_->issues) {
			if (issue.path == relative) {
				reason = issue.message;
			}
		}
		undo();
		return fail(ErrorCode::InvalidArgument, name + ": " + reason);
	}
	if (auto loaded = load(id); !loaded) {
		const Error error = loaded.error();
		undo();
		return error;
	}
	return id;
}

std::vector<AssetIssue> AssetDatabase::validate_all()
{
	std::vector<AssetIssue> failures;
	for (const auto& [path, id] : impl_->by_path) {
		const AssetMetadata& meta = impl_->by_id.at(id).meta;
		std::optional<Error> error;
		if (meta.type == AssetType::Mesh) {
			if (auto r = impl_->import_mesh(meta); !r) {
				error = r.error();
			}
		} else if (auto r = impl_->import_texture(meta); !r) {
			error = r.error();
		}
		if (error) {
			failures.push_back({path, error->message});
		}
	}
	return failures;
}

AssetState AssetDatabase::state(AssetId id) const
{
	const auto it = impl_->by_id.find(id);
	return it == impl_->by_id.end() ? AssetState::Unknown : it->second.state;
}

std::vector<AssetId> AssetDatabase::find_missing()
{
	std::vector<AssetId> missing;
	for (auto& [id, e] : impl_->by_id) {
		std::error_code ec;
		if (!fs::is_regular_file(impl_->source_of(e.meta), ec)) {
			e.state = AssetState::Missing;
			missing.push_back(id);
		} else if (e.state == AssetState::Missing) {
			e.state = e.cached() ? AssetState::Loaded : AssetState::Unloaded;
		}
	}
	return missing;
}

Result<void> AssetDatabase::reload(AssetId id)
{
	const auto it = impl_->by_id.find(id);
	if (it == impl_->by_id.end()) {
		return fail(ErrorCode::NotFound, "unknown asset id " + to_string(id));
	}
	it->second.drop();
	++it->second.revision;
	it->second.state = AssetState::Unloaded;
	return {};
}

std::size_t AssetDatabase::unload_unused()
{
	std::size_t freed = 0;
	for (auto& [id, e] : impl_->by_id) {
		(void)id;
		const bool mesh_unused = e.mesh && e.mesh.use_count() == 1;
		const bool texture_unused = e.texture && e.texture.use_count() == 1;
		if (mesh_unused || texture_unused) {
			e.drop();
			if (e.state == AssetState::Loaded) {
				e.state = AssetState::Unloaded;
			}
			++freed;
		}
	}
	return freed;
}

std::size_t AssetDatabase::cached_count() const noexcept
{
	return static_cast<std::size_t>(
		std::count_if(impl_->by_id.begin(), impl_->by_id.end(), [](const auto& entry) { return entry.second.cached(); }));
}

const fs::path& AssetDatabase::root() const noexcept
{
	return impl_->root;
}

} // namespace engine::assets
