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
		std::shared_ptr<const MaterialData> material;
		std::shared_ptr<const EnvironmentData> environment;
		std::shared_ptr<const EffectData> effect;

		[[nodiscard]] bool cached() const noexcept { return mesh || texture || material || environment || effect; }
		void drop() noexcept
		{
			mesh.reset();
			texture.reset();
			material.reset();
			environment.reset();
			effect.reset();
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

	Result<std::shared_ptr<const MaterialData>> import_material(const AssetMetadata& meta) const
	{
		auto bytes = read(source_of(meta), 1024 * 1024);
		if (!bytes) {
			return bytes.error();
		}
		const auto& b = bytes.value();
		auto material = parse_material(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
		if (!material) {
			return material.error();
		}
		return std::make_shared<const MaterialData>(std::move(material.value()));
	}

	Result<std::shared_ptr<const EffectData>> import_effect(const AssetMetadata& meta) const
	{
		auto bytes = read(source_of(meta), 1024 * 1024);
		if (!bytes) {
			return bytes.error();
		}
		const auto& b = bytes.value();
		auto effect = parse_effect(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
		if (!effect) {
			return effect.error();
		}
		return std::make_shared<const EffectData>(std::move(effect.value()));
	}

	// Writes a new text asset of `type` under a free name based on `path`, with a .meta holding a new random id.
	Result<AssetId> create_text(AssetDatabase& db, std::string_view path, AssetType type, const char* noun,
		const char* extension, const std::string& content)
	{
		auto normalized = normalize_asset_path(path);
		if (!normalized) {
			return fail(ErrorCode::InvalidArgument, std::string("the ") + noun + " name cannot be used: " + normalized.error().message);
		}
		if (asset_type_for(normalized.value()) != type) {
			return fail(ErrorCode::InvalidArgument, std::string("a ") + noun + "'s file name must end in " + extension);
		}
		const fs::path target = free_name(root / from_utf8(normalized.value()));
		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		if (ec) {
			return fail(ErrorCode::InvalidState, "cannot create the folder for " + normalized.value());
		}
		fs::path meta_path = target;
		meta_path += ".meta";
		const AssetId id = new_id();
		const auto undo = [&] {
			std::error_code ignored;
			fs::remove(target, ignored);
			fs::remove(meta_path, ignored);
			(void)db.scan();
		};
		if (auto written = write_atomic(target, content); !written) {
			return written.error();
		}
		if (auto written = write_atomic(meta_path, write_meta(MetaFile{id, {}})); !written) {
			undo();
			return written.error();
		}
		if (auto scanned = db.scan(); !scanned) {
			undo();
			return scanned.error();
		}
		if (!db.find(id)) {
			undo();
			return fail(ErrorCode::InvalidArgument, normalized.value() + " could not be catalogued");
		}
		return id;
	}

	Result<std::shared_ptr<const EnvironmentData>> import_environment(const AssetMetadata& meta) const
	{
		auto bytes = read(source_of(meta), limits.max_source_bytes);
		if (!bytes) {
			return bytes.error();
		}
		auto environment = import_hdr(bytes.value(), limits);
		if (!environment) {
			return environment.error();
		}
		return std::make_shared<const EnvironmentData>(std::move(environment.value()));
	}

	// Writes bytes beside the target and moves them over it, so a failure leaves the old file.
	Result<void> write_atomic(const fs::path& target, std::string_view text) const
	{
		fs::path temporary = target;
		temporary += ".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!out) {
				return fail(ErrorCode::InvalidState, "cannot write " + utf8(temporary));
			}
		}
		std::error_code ec;
		fs::rename(temporary, target, ec);
		if (ec) {
			fs::remove(temporary, ec);
			return fail(ErrorCode::InvalidState, "cannot replace " + utf8(target));
		}
		return {};
	}

	AssetId new_id() const
	{
		std::random_device random;
		AssetId id;
		do {
			id.value = (static_cast<std::uint64_t>(random()) << 32) | random();
		} while (id.is_null() || by_id.contains(id));
		return id;
	}

	// A free name beside `wanted`: "model.glb", then "model_1.glb", ...
	fs::path free_name(const fs::path& wanted) const
	{
		std::error_code ec;
		const fs::path stem = wanted.stem(), extension = wanted.extension(), folder = wanted.parent_path();
		fs::path target = wanted;
		for (int i = 1; fs::exists(target, ec) || fs::exists(fs::path(target) += ".meta", ec); ++i) {
			target = folder / (fs::path(stem) += "_" + std::to_string(i));
			target += extension;
		}
		return target;
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
			issues.push_back({meta.path, "unsupported format (supported: .glb, .obj, .png, .jpg, .jpeg, .tga, .ppm, .material, .effect, .hdr)"});
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

Result<MaterialAsset> AssetDatabase::load_material(AssetId id)
{
	return impl_->load<MaterialData>(id, AssetType::Material, &Impl::Entry::material,
		[this](const AssetMetadata& meta) { return impl_->import_material(meta); });
}

Result<EffectAsset> AssetDatabase::load_effect(AssetId id)
{
	return impl_->load<EffectData>(id, AssetType::Effect, &Impl::Entry::effect,
		[this](const AssetMetadata& meta) { return impl_->import_effect(meta); });
}

Result<EnvironmentAsset> AssetDatabase::load_environment(AssetId id)
{
	return impl_->load<EnvironmentData>(id, AssetType::Environment, &Impl::Entry::environment,
		[this](const AssetMetadata& meta) { return impl_->import_environment(meta); });
}

std::uint32_t AssetDatabase::revision(AssetId id) const
{
	const auto it = impl_->by_id.find(id);
	return it == impl_->by_id.end() ? 0 : it->second.revision;
}

Result<void> AssetDatabase::move(AssetId id, std::string_view new_path)
{
	const auto it = impl_->by_id.find(id);
	if (it == impl_->by_id.end()) {
		return fail(ErrorCode::NotFound, "unknown asset id " + to_string(id));
	}
	const auto meta = it->second.meta;
	auto normalized = normalize_asset_path(new_path);
	if (!normalized) {
		return fail(ErrorCode::InvalidArgument, "that name cannot be used: " + normalized.error().message);
	}
	if (normalized.value() == meta.path) {
		return {};
	}
	if (asset_type_for(normalized.value()) != meta.type) {
		return fail(ErrorCode::InvalidArgument, "the new name must keep the file type of " + meta.path);
	}
	const fs::path source = impl_->source_of(meta), target = impl_->root / from_utf8(normalized.value());
	fs::path source_meta = source, target_meta = target;
	source_meta += ".meta";
	target_meta += ".meta";
	std::error_code ec;
	if (fs::exists(target, ec) || fs::exists(target_meta, ec)) {
		return fail(ErrorCode::InvalidArgument, normalized.value() + " already exists");
	}
	fs::create_directories(target.parent_path(), ec);
	if (ec) {
		return fail(ErrorCode::InvalidState, "cannot create the folder for " + normalized.value());
	}
	// The id goes with the file: a new .meta first, even for an asset whose id came from its old path.
	if (auto written = impl_->write_atomic(target_meta, write_meta(MetaFile{id, meta.settings})); !written) {
		return written.error();
	}
	fs::rename(source, target, ec);
	if (ec) {
		std::error_code ignored;
		fs::remove(target_meta, ignored);
		return fail(ErrorCode::InvalidState, "cannot move " + meta.path + ": " + ec.message());
	}
	fs::remove(source_meta, ec);
	if (auto scanned = scan(); !scanned) {
		return scanned.error();
	}
	return {};
}

Result<AssetId> AssetDatabase::create_material(std::string_view path, const MaterialData& material)
{
	if (auto valid = validate_material(material); !valid) {
		return valid.error();
	}
	return impl_->create_text(*this, path, AssetType::Material, "material", ".material", write_material(material));
}

Result<void> AssetDatabase::save_material(AssetId id, const MaterialData& material)
{
	const auto it = impl_->by_id.find(id);
	if (it == impl_->by_id.end()) {
		return fail(ErrorCode::NotFound, "unknown asset id " + to_string(id));
	}
	if (it->second.meta.type != AssetType::Material) {
		return fail(ErrorCode::InvalidArgument, it->second.meta.path + " is not a material");
	}
	if (auto valid = validate_material(material); !valid) {
		return valid.error();
	}
	if (auto written = impl_->write_atomic(impl_->source_of(it->second.meta), write_material(material)); !written) {
		return written.error();
	}
	it->second.material = std::make_shared<const MaterialData>(material);
	it->second.state = AssetState::Loaded;
	++it->second.revision;
	return {};
}

Result<AssetId> AssetDatabase::create_effect(std::string_view path, const EffectData& effect)
{
	if (auto valid = validate_effect(effect); !valid) {
		return valid.error();
	}
	return impl_->create_text(*this, path, AssetType::Effect, "effect", ".effect", write_effect(effect));
}

Result<void> AssetDatabase::save_effect(AssetId id, const EffectData& effect)
{
	const auto it = impl_->by_id.find(id);
	if (it == impl_->by_id.end()) {
		return fail(ErrorCode::NotFound, "unknown asset id " + to_string(id));
	}
	if (it->second.meta.type != AssetType::Effect) {
		return fail(ErrorCode::InvalidArgument, it->second.meta.path + " is not an effect");
	}
	if (auto valid = validate_effect(effect); !valid) {
		return valid.error();
	}
	if (auto written = impl_->write_atomic(impl_->source_of(it->second.meta), write_effect(effect)); !written) {
		return written.error();
	}
	it->second.effect = std::make_shared<const EffectData>(effect);
	it->second.state = AssetState::Loaded;
	++it->second.revision;
	return {};
}

Result<AssetId> AssetDatabase::import_file(const fs::path& source)
{
	return import_source(source, nullptr);
}

Result<AssetId> AssetDatabase::commit_import(const PreparedImport& prepared)
{
	return import_source(prepared.source, &prepared);
}

Result<AssetId> AssetDatabase::import_source(const fs::path& source, const PreparedImport* prepared)
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
			"; supported: .glb, .obj, .png, .jpg, .jpeg, .tga, .ppm, .material, .effect, .hdr");
	}
	const auto load = [&](AssetId id) -> Result<void> {
		if (prepared && prepared->type == type) {
			// Decoded already; the settings are the defaults a fresh import uses.
			auto& entry = impl_->by_id.at(id);
			if (entry.meta.settings == ImportSettings{}) {
				entry.mesh = prepared->mesh;
				entry.texture = prepared->texture;
				entry.material = prepared->material;
				entry.environment = prepared->environment;
				entry.effect = prepared->effect;
				entry.state = AssetState::Loaded;
				return {};
			}
		}
		const auto done = [](const auto& loaded) { return loaded ? Result<void>{} : Result<void>(loaded.error()); };
		switch (type) {
		case AssetType::Mesh:
			return done(load_mesh(id));
		case AssetType::Material:
			return done(load_material(id));
		case AssetType::Effect:
			return done(load_effect(id));
		case AssetType::Environment:
			return done(load_environment(id));
		default:
			return done(load_texture(id));
		}
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
	const fs::path target = impl_->free_name(impl_->root / from_utf8(normalized.value()));
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
	const AssetId id = impl_->new_id();
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
		const auto check = [&](const auto& r) {
			if (!r) {
				error = r.error();
			}
		};
		switch (meta.type) {
		case AssetType::Mesh:
			check(impl_->import_mesh(meta));
			break;
		case AssetType::Material:
			check(impl_->import_material(meta));
			break;
		case AssetType::Effect:
			check(impl_->import_effect(meta));
			break;
		case AssetType::Environment:
			check(impl_->import_environment(meta));
			break;
		default:
			check(impl_->import_texture(meta));
			break;
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
		const bool material_unused = e.material && e.material.use_count() == 1;
		const bool environment_unused = e.environment && e.environment.use_count() == 1;
		const bool effect_unused = e.effect && e.effect.use_count() == 1;
		if (mesh_unused || texture_unused || material_unused || environment_unused || effect_unused) {
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

Result<PreparedImport> prepare_import(const fs::path& source, const ImportLimits& limits)
{
	std::error_code ec;
	if (!fs::is_regular_file(source, ec)) {
		return fail(ErrorCode::NotFound, "file not found: " + utf8(source));
	}
	const std::string name = utf8(source.filename());
	PreparedImport prepared{source, asset_type_for(name), {}, {}, {}, {}};
	if (prepared.type == AssetType::Unknown) {
		return fail(ErrorCode::InvalidArgument, "unsupported file type '" + utf8(source.extension()) + "'");
	}
	const std::uintmax_t size = fs::file_size(source, ec);
	if (ec) {
		return fail(ErrorCode::NotFound, "file cannot be read: " + utf8(source));
	}
	if (size > limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, name + " exceeds the size limit");
	}
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	std::ifstream in(source, std::ios::binary);
	if (!in || !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
		return fail(ErrorCode::NotFound, "file cannot be read: " + utf8(source));
	}
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	const auto failed = [&](const Error& error) {
		Error named = error;
		named.message = name + ": " + error.message;
		return named;
	};
	switch (prepared.type) {
	case AssetType::Mesh: {
		auto mesh = has_extension(name, ".glb") ? import_glb(bytes, {}, limits) : import_obj(text, {}, limits);
		if (!mesh) {
			return failed(mesh.error());
		}
		prepared.mesh = std::make_shared<const MeshData>(std::move(mesh.value()));
		break;
	}
	case AssetType::Material: {
		auto material = parse_material(text);
		if (!material) {
			return failed(material.error());
		}
		prepared.material = std::make_shared<const MaterialData>(std::move(material.value()));
		break;
	}
	case AssetType::Effect: {
		auto effect = parse_effect(text);
		if (!effect) {
			return failed(effect.error());
		}
		prepared.effect = std::make_shared<const EffectData>(std::move(effect.value()));
		break;
	}
	case AssetType::Environment: {
		auto environment = import_hdr(bytes, limits);
		if (!environment) {
			return failed(environment.error());
		}
		prepared.environment = std::make_shared<const EnvironmentData>(std::move(environment.value()));
		break;
	}
	default: {
		auto texture = has_extension(name, ".ppm")   ? import_ppm(bytes, {}, limits)
			: has_extension(name, ".tga") ? import_tga(bytes, {}, limits)
										  : import_image(bytes, {}, limits);
		if (!texture) {
			return failed(texture.error());
		}
		prepared.texture = std::make_shared<const TextureData>(std::move(texture.value()));
		break;
	}
	}
	return prepared;
}

} // namespace engine::assets
