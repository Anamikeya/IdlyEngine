// Asset pipeline tests against the committed fixtures in Tests/Assets (real files, real filesystem).
// Cases that mutate files work on a copy under the test working directory.

#include "engine/assets/asset_database.h"
#include "engine/assets/importers.h"
#include "engine/test/test.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine::assets;
using engine::ErrorCode;
namespace fs = std::filesystem;

namespace {

const fs::path fixtures = fs::path(GAMEENGINE_TEST_ASSETS_DIR);
const fs::path editor_root = fixtures / "editor";
const fs::path invalid_root = fixtures / "invalid";

std::vector<std::uint8_t> read_bytes(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	ENGINE_REQUIRE(static_cast<bool>(in));
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const fs::path& path)
{
	const auto bytes = read_bytes(path);
	return {bytes.begin(), bytes.end()};
}

void write_text(const fs::path& path, std::string_view text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

// Fresh per-case scratch directory under the CTest working directory.
fs::path scratch(const char* name)
{
	const fs::path dir = fs::current_path() / "asset_tests_tmp" / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

AssetDatabase open_scanned(const fs::path& root)
{
	auto db = AssetDatabase::open(root);
	ENGINE_REQUIRE(static_cast<bool>(db));
	ENGINE_REQUIRE(static_cast<bool>(db.value().scan()));
	return std::move(db.value());
}

std::array<std::uint8_t, 4> pixel(const TextureData& t, std::uint32_t x, std::uint32_t y)
{
	const std::size_t i = (static_cast<std::size_t>(y) * t.width + x) * 4;
	return {t.pixels[i], t.pixels[i + 1], t.pixels[i + 2], t.pixels[i + 3]};
}

using Rgba = std::array<std::uint8_t, 4>;
constexpr Rgba white{255, 255, 255, 255};

const char* const triangle_obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

} // namespace

// ---- Paths and identifiers ---------------------------------------------------------------------

ENGINE_TEST(asset_paths_normalize)
{
	const auto ok = [](std::string_view raw, std::string_view expected) {
		auto r = normalize_asset_path(raw);
		ENGINE_REQUIRE(static_cast<bool>(r));
		ENGINE_CHECK_EQ(r.value(), std::string(expected));
	};
	ok("meshes/cube.obj", "meshes/cube.obj");
	ok("meshes\\cube.obj", "meshes/cube.obj");
	ok("./meshes//cube.obj", "meshes/cube.obj");
	ok("meshes/old/../cube.obj", "meshes/cube.obj");
	ok("Meshes/Cube.OBJ", "Meshes/Cube.OBJ"); // Case preserved.
	ok("textures/con_table.tga", "textures/con_table.tga");
}

ENGINE_TEST(asset_paths_reject_escapes_and_reserved_names)
{
	for (const std::string_view bad : {"", "/etc/passwd", "\\\\server\\share\\a.obj", "C:/a.obj", "c:a.obj", "../a.obj",
			 "meshes/../../a.obj", ".", "a\x01.obj", "a?.obj", "a|b.obj", "CON", "textures/nul.tga",
			 "textures/com1.tga", "trailing./a.obj", "space /a.obj"}) {
		const auto r = normalize_asset_path(bad);
		ENGINE_CHECK(!r);
		if (!r) {
			ENGINE_CHECK(r.error().code == ErrorCode::InvalidArgument);
		}
	}
	ENGINE_CHECK(!normalize_asset_path(std::string(1025, 'a')));
}

ENGINE_TEST(asset_ids_are_stable_and_round_trip)
{
	// FNV-1a 64 of "meshes/cube.obj"; changing the hash would silently re-identify every asset.
	ENGINE_CHECK_EQ(to_string(asset_id_from_path("meshes/cube.obj")), std::string("e8c36ec6d5803027"));
	ENGINE_CHECK(asset_id_from_path("a.obj") != asset_id_from_path("A.obj"));
	const AssetId id{0x6a1f0c3e9b2d4a01ull};
	ENGINE_CHECK_EQ(to_string(id), std::string("6a1f0c3e9b2d4a01"));
	ENGINE_CHECK(parse_asset_id("6A1F0C3E9B2D4A01") == id);
	ENGINE_CHECK(!parse_asset_id("0000000000000000"));
	ENGINE_CHECK(!parse_asset_id("6a1f0c3e9b2d4a0"));
	ENGINE_CHECK(!parse_asset_id("6a1f0c3e9b2d4a0g"));
}

ENGINE_TEST(asset_types_follow_extensions)
{
	ENGINE_CHECK(asset_type_for("a/b.OBJ") == AssetType::Mesh);
	ENGINE_CHECK(asset_type_for("b.tga") == AssetType::Texture);
	ENGINE_CHECK(asset_type_for("b.ppm") == AssetType::Texture);
	ENGINE_CHECK(asset_type_for("b.png") == AssetType::Unknown);
	ENGINE_CHECK(asset_type_for("dir.obj/file") == AssetType::Unknown);
}

// ---- OBJ ---------------------------------------------------------------------------------------

ENGINE_TEST(obj_cube_imports_with_consistent_winding)
{
	auto mesh = import_obj(read_text(editor_root / "meshes/cube.obj"));
	ENGINE_REQUIRE(static_cast<bool>(mesh));
	const MeshData& m = mesh.value();
	ENGINE_CHECK_EQ(m.vertices.size(), std::size_t{24});
	ENGINE_CHECK_EQ(m.indices.size(), std::size_t{36});
	ENGINE_CHECK(!m.generated_normals);
	ENGINE_CHECK(!m.missing_uvs);
	for (std::size_t k = 0; k < 3; ++k) {
		ENGINE_CHECK_EQ(m.bounds.min[k], -0.5f);
		ENGINE_CHECK_EQ(m.bounds.max[k], 0.5f);
	}
	// Counter-clockwise front faces: each triangle's geometric normal agrees with its vertex normals.
	for (std::size_t i = 0; i < m.indices.size(); i += 3) {
		const auto& a = m.vertices[m.indices[i]];
		const auto& b = m.vertices[m.indices[i + 1]];
		const auto& c = m.vertices[m.indices[i + 2]];
		const std::array<float, 3> e1{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
		const std::array<float, 3> e2{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
		const std::array<float, 3> n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
		ENGINE_CHECK(n[0] * a.normal[0] + n[1] * a.normal[1] + n[2] * a.normal[2] > 0.0f);
	}
	// vt (0, 0) is the bottom-left in OBJ; output UVs are top-left origin.
	const auto first = std::find_if(m.vertices.begin(), m.vertices.end(),
		[](const MeshVertex& v) { return v.position == std::array<float, 3>{-0.5f, -0.5f, 0.5f} && v.normal[2] == 1.0f; });
	ENGINE_REQUIRE(first != m.vertices.end());
	ENGINE_CHECK(first->uv == (std::array<float, 2>{0.0f, 1.0f}));
}

ENGINE_TEST(obj_without_normals_generates_them)
{
	auto mesh = import_obj(read_text(editor_root / "meshes/triangle.obj"));
	ENGINE_REQUIRE(static_cast<bool>(mesh));
	ENGINE_CHECK(mesh.value().generated_normals);
	ENGINE_CHECK(mesh.value().missing_uvs);
	ENGINE_CHECK_EQ(mesh.value().vertices.size(), std::size_t{3});
	for (const MeshVertex& v : mesh.value().vertices) {
		ENGINE_CHECK(v.normal == (std::array<float, 3>{0.0f, 0.0f, 1.0f}));
	}
}

ENGINE_TEST(obj_fan_triangulates_and_scales)
{
	ImportSettings settings;
	settings.mesh_scale = 0.01f;
	auto mesh = import_obj("v 0 0 0\nv 100 0 0\nv 100 100 0\nv 50 150 0\nv 0 100 0\nf 1 2 3 4 5\n", settings);
	ENGINE_REQUIRE(static_cast<bool>(mesh));
	ENGINE_CHECK(mesh.value().indices == (std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3, 0, 3, 4}));
	ENGINE_CHECK_NEAR(mesh.value().bounds.max[1], 1.5, 1e-6);
}

ENGINE_TEST(obj_rejects_invalid_content_with_line_numbers)
{
	const auto error = [](std::string_view text) {
		auto r = import_obj(text);
		ENGINE_REQUIRE(!r);
		return r.error();
	};
	const auto bad_index = error(read_text(invalid_root / "bad_index.obj"));
	ENGINE_CHECK(bad_index.code == ErrorCode::ParseError);
	ENGINE_CHECK(bad_index.message.find("line 4") != std::string::npos);
	const auto unsupported = error(read_text(invalid_root / "unsupported.obj"));
	ENGINE_CHECK(unsupported.message.find("line 4") != std::string::npos);
	ENGINE_CHECK(unsupported.message.find("cstype") != std::string::npos);

	ENGINE_CHECK(error("v 0 0\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 nan\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 1e999\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 0\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/ 2 3\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1 2 3\n").code == ErrorCode::ParseError); // No vt defined.
	ENGINE_CHECK(error("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1//1/1 2 3\n").code == ErrorCode::ParseError);
	ENGINE_CHECK(error("# only a comment\nv 0 0 0\n").message.find("no faces") != std::string::npos);

	ImportLimits tiny;
	tiny.max_vertices = 2;
	ENGINE_CHECK(import_obj(triangle_obj, {}, tiny).error().code == ErrorCode::InvalidArgument);
	tiny = {};
	tiny.max_source_bytes = 4;
	ENGINE_CHECK(import_obj(triangle_obj, {}, tiny).error().code == ErrorCode::InvalidArgument);
	ImportSettings bad_scale;
	bad_scale.mesh_scale = 0.0f;
	ENGINE_CHECK(import_obj(triangle_obj, bad_scale).error().code == ErrorCode::InvalidArgument);
}

ENGINE_TEST(obj_accepts_crlf_comments_and_ignored_statements)
{
	auto mesh = import_obj("mtllib a.mtl\r\no tri\r\ng group\r\nusemtl red\r\ns 1\r\nv 0 0 0 # origin\r\nv 1 0 0\r\nv 0 1 0\r\nf 1 2 3\r\n");
	ENGINE_REQUIRE(static_cast<bool>(mesh));
	ENGINE_CHECK_EQ(mesh.value().indices.size(), std::size_t{3});
}

// ---- Textures ----------------------------------------------------------------------------------

ENGINE_TEST(tga_uncompressed_bottom_left_is_flipped_to_top_left)
{
	auto tex = import_tga(read_bytes(editor_root / "textures/checker.tga"));
	ENGINE_REQUIRE(static_cast<bool>(tex));
	const TextureData& t = tex.value();
	ENGINE_CHECK_EQ(t.width, 8u);
	ENGINE_CHECK_EQ(t.height, 8u);
	ENGINE_CHECK_EQ(t.pixels.size(), std::size_t{8 * 8 * 4});
	ENGINE_CHECK(t.srgb);
	// 2x2 cells: top-left cell white, its right and lower neighbours red with alpha 128.
	ENGINE_CHECK(pixel(t, 0, 0) == white);
	ENGINE_CHECK(pixel(t, 2, 0) == (Rgba{255, 0, 0, 128}));
	ENGINE_CHECK(pixel(t, 0, 2) == (Rgba{255, 0, 0, 128}));
	ENGINE_CHECK(pixel(t, 7, 7) == white);
}

ENGINE_TEST(tga_rle_top_left_decodes)
{
	auto tex = import_tga(read_bytes(editor_root / "textures/checker_rle.tga"));
	ENGINE_REQUIRE(static_cast<bool>(tex));
	const TextureData& t = tex.value();
	ENGINE_CHECK(pixel(t, 0, 0) == white);
	ENGINE_CHECK(pixel(t, 3, 3) == white);
	ENGINE_CHECK(pixel(t, 4, 0) == (Rgba{255, 0, 0, 255}));
	ENGINE_CHECK(pixel(t, 0, 4) == (Rgba{255, 0, 0, 255}));
	ENGINE_CHECK(pixel(t, 4, 4) == white);
}

ENGINE_TEST(tga_grayscale_expands_to_rgba)
{
	// Type 3, 2x1, 8 bpp, top-left origin.
	const std::vector<std::uint8_t> bytes{0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 1, 0, 8, 0x20, 10, 200};
	auto tex = import_tga(bytes);
	ENGINE_REQUIRE(static_cast<bool>(tex));
	ENGINE_CHECK(pixel(tex.value(), 0, 0) == (Rgba{10, 10, 10, 255}));
	ENGINE_CHECK(pixel(tex.value(), 1, 0) == (Rgba{200, 200, 200, 255}));
}

ENGINE_TEST(tga_rejects_malformed_files)
{
	ENGINE_CHECK(import_tga(read_bytes(invalid_root / "truncated.tga")).error().code == ErrorCode::ParseError);
	ENGINE_CHECK(!import_tga(std::vector<std::uint8_t>(10, 0)));
	std::vector<std::uint8_t> header{0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0x20};
	auto mapped = header;
	mapped[1] = 1;
	ENGINE_CHECK(!import_tga(mapped));
	auto paletted = header;
	paletted[2] = 1;
	ENGINE_CHECK(!import_tga(paletted));
	auto deep = header;
	deep[16] = 16;
	ENGINE_CHECK(!import_tga(deep));
	auto mirrored = header;
	mirrored[17] = 0x30;
	ENGINE_CHECK(!import_tga(mirrored));
	auto empty = header;
	empty[12] = 0;
	ENGINE_CHECK(!import_tga(empty));
	// RLE run of 3 pixels in a 1x1 image.
	auto overrun = header;
	overrun[2] = 10;
	overrun.insert(overrun.end(), {0x82, 1, 2, 3});
	ENGINE_CHECK(import_tga(overrun).error().message.find("overruns") != std::string::npos);
	ImportLimits small;
	small.max_texture_dimension = 4;
	ENGINE_CHECK(import_tga(read_bytes(editor_root / "textures/checker.tga"), {}, small).error().code == ErrorCode::InvalidArgument);
}

ENGINE_TEST(ppm_decodes_and_rejects_bad_headers)
{
	ImportSettings linear;
	linear.texture_srgb = false;
	auto tex = import_ppm(read_bytes(editor_root / "textures/gradient.ppm"), linear);
	ENGINE_REQUIRE(static_cast<bool>(tex));
	const TextureData& t = tex.value();
	ENGINE_CHECK_EQ(t.width, 4u);
	ENGINE_CHECK_EQ(t.height, 2u);
	ENGINE_CHECK(!t.srgb);
	ENGINE_CHECK(pixel(t, 0, 0) == (Rgba{0, 0, 64, 255}));
	ENGINE_CHECK(pixel(t, 3, 0) == (Rgba{255, 0, 64, 255}));
	ENGINE_CHECK(pixel(t, 0, 1) == (Rgba{0, 255, 64, 255}));

	ENGINE_CHECK(!import_ppm(read_bytes(invalid_root / "bad_maxval.ppm")));
	const auto bytes = [](std::string_view s) { return std::vector<std::uint8_t>(s.begin(), s.end()); };
	ENGINE_CHECK(!import_ppm(bytes("P3\n1 1\n255\n0 0 0\n")));
	ENGINE_CHECK(!import_ppm(bytes("P6\n1 1\n255\n\x01\x02")));
	ENGINE_CHECK(!import_ppm(bytes("P6\n1 1\n255\n\x01\x02\x03\x04")));
	ENGINE_CHECK(!import_ppm(bytes("P6\n1 x\n255\n\x01\x02\x03")));
	ENGINE_CHECK(static_cast<bool>(import_ppm(bytes("P6 # c\n1 1 255\n\x01\x02\x03"))));
}

// ---- Metadata ----------------------------------------------------------------------------------

ENGINE_TEST(meta_files_parse_strictly_and_round_trip)
{
	auto meta = parse_meta("# c\nversion = 1\nid = 6a1f0c3e9b2d4a01\nmesh.scale = 0.01\ntexture.srgb = false\n");
	ENGINE_REQUIRE(static_cast<bool>(meta));
	ENGINE_CHECK(meta.value().id == AssetId{0x6a1f0c3e9b2d4a01ull});
	ENGINE_CHECK_EQ(meta.value().settings.mesh_scale, 0.01f);
	ENGINE_CHECK(!meta.value().settings.texture_srgb);

	auto again = parse_meta(write_meta(meta.value()));
	ENGINE_REQUIRE(static_cast<bool>(again));
	ENGINE_CHECK(again.value().id == meta.value().id);
	ENGINE_CHECK(again.value().settings == meta.value().settings);

	for (const std::string_view bad : {"", "id = 6a1f0c3e9b2d4a01\n", "version = 2\n", "version = 1\nversion = 1\n",
			 "version = 1\ncolour = red\n", "version = 1\nid = 0000000000000000\n", "version = 1\nmesh.scale = -1\n",
			 "version = 1\ntexture.srgb = yes\n", "version 1\n"}) {
		ENGINE_CHECK(!parse_meta(bad));
	}
}

// ---- Database ----------------------------------------------------------------------------------

ENGINE_TEST(database_catalogues_editor_test_assets)
{
	AssetDatabase db = open_scanned(editor_root);
	ENGINE_CHECK(db.issues().empty());
	const auto all = db.assets();
	ENGINE_REQUIRE(all.size() == 5);
	ENGINE_CHECK_EQ(all[0].path, std::string("meshes/cube.obj"));
	ENGINE_CHECK_EQ(all[4].path, std::string("textures/gradient.ppm"));

	const auto cube = db.find("meshes/cube.obj");
	ENGINE_REQUIRE(cube.has_value());
	ENGINE_CHECK(cube->id == AssetId{0x6a1f0c3e9b2d4a01ull}); // From the sidecar.
	ENGINE_CHECK(cube->has_meta);
	ENGINE_CHECK(cube->type == AssetType::Mesh);
	const auto triangle = db.find("meshes\\triangle.obj");
	ENGINE_REQUIRE(triangle.has_value());
	ENGINE_CHECK(triangle->id == asset_id_from_path("meshes/triangle.obj")); // Path-derived.
	ENGINE_CHECK(!triangle->has_meta);
	ENGINE_CHECK(!db.find("./textures/gradient.ppm")->settings.texture_srgb);
	ENGINE_CHECK(db.find(AssetId{0x3c9d2e7f10a45b02ull})->path == "textures/checker.tga");
	ENGINE_CHECK(!db.find("../editor/meshes/cube.obj").has_value());
	ENGINE_CHECK(db.validate_all().empty());
}

ENGINE_TEST(database_caches_reloads_and_unloads)
{
	AssetDatabase db = open_scanned(editor_root);
	const AssetId cube{0x6a1f0c3e9b2d4a01ull};
	ENGINE_CHECK(db.state(cube) == AssetState::Unloaded);

	auto first = db.load_mesh(cube);
	ENGINE_REQUIRE(static_cast<bool>(first));
	ENGINE_CHECK(db.state(cube) == AssetState::Loaded);
	ENGINE_CHECK_EQ(first.value().revision, 1u);
	auto second = db.load_mesh(cube);
	ENGINE_REQUIRE(static_cast<bool>(second));
	ENGINE_CHECK(first.value().data == second.value().data); // Cached, not re-imported.
	ENGINE_CHECK_EQ(db.cached_count(), std::size_t{1});

	// Referenced from outside: not freed.
	ENGINE_CHECK_EQ(db.unload_unused(), std::size_t{0});
	ENGINE_REQUIRE(static_cast<bool>(db.reload(cube)));
	auto reloaded = db.load_mesh(cube);
	ENGINE_REQUIRE(static_cast<bool>(reloaded));
	ENGINE_CHECK_EQ(reloaded.value().revision, 2u);
	ENGINE_CHECK(reloaded.value().data != first.value().data);
	ENGINE_CHECK_EQ(first.value().data->vertices.size(), std::size_t{24}); // Old data stays valid for its holders.

	first = engine::fail(ErrorCode::Internal, "released");
	second = engine::fail(ErrorCode::Internal, "released");
	reloaded = engine::fail(ErrorCode::Internal, "released");
	ENGINE_CHECK_EQ(db.unload_unused(), std::size_t{1});
	ENGINE_CHECK_EQ(db.cached_count(), std::size_t{0});
	ENGINE_CHECK(db.state(cube) == AssetState::Unloaded);

	auto texture = db.load_texture(AssetId{0x3c9d2e7f10a45b02ull});
	ENGINE_REQUIRE(static_cast<bool>(texture));
	ENGINE_CHECK_EQ(texture.value().data->width, 8u);
}

ENGINE_TEST(database_reports_type_mismatch_and_unknown_ids)
{
	AssetDatabase db = open_scanned(editor_root);
	ENGINE_CHECK(db.load_texture(AssetId{0x6a1f0c3e9b2d4a01ull}).error().code == ErrorCode::InvalidArgument);
	ENGINE_CHECK(db.load_mesh(AssetId{0x1234ull}).error().code == ErrorCode::NotFound);
	ENGINE_CHECK(db.state(AssetId{0x1234ull}) == AssetState::Unknown);
	ENGINE_CHECK(db.reload(AssetId{0x1234ull}).error().code == ErrorCode::NotFound);
	ENGINE_CHECK(AssetDatabase::open(editor_root / "meshes/cube.obj").error().code == ErrorCode::NotFound);
	ENGINE_CHECK(AssetDatabase::open(fixtures / "does-not-exist").error().code == ErrorCode::NotFound);
}

ENGINE_TEST(database_detects_missing_assets)
{
	const fs::path root = scratch("missing");
	fs::copy(editor_root, root, fs::copy_options::recursive);
	AssetDatabase db = open_scanned(root);
	const AssetId cube{0x6a1f0c3e9b2d4a01ull};
	ENGINE_CHECK(db.find_missing().empty());

	fs::rename(root / "meshes/cube.obj", root / "cube.obj.bak");
	ENGINE_CHECK(db.find_missing() == std::vector<AssetId>{cube});
	ENGINE_CHECK(db.state(cube) == AssetState::Missing);
	const auto load = db.load_mesh(cube);
	ENGINE_REQUIRE(!load);
	ENGINE_CHECK(load.error().code == ErrorCode::NotFound);
	ENGINE_CHECK(load.error().message.find("meshes/cube.obj") != std::string::npos);

	fs::rename(root / "cube.obj.bak", root / "meshes/cube.obj");
	ENGINE_CHECK(db.find_missing().empty());
	ENGINE_CHECK(db.state(cube) == AssetState::Unloaded);
	ENGINE_CHECK(static_cast<bool>(db.load_mesh(cube)));
}

ENGINE_TEST(database_failed_import_recovers_after_fix)
{
	const fs::path root = scratch("failed");
	write_text(root / "broken.obj", "v 0 0 0\nf 1 2 3\n");
	AssetDatabase db = open_scanned(root);
	const AssetId id = db.find("broken.obj")->id;
	ENGINE_CHECK(db.load_mesh(id).error().code == ErrorCode::ParseError);
	ENGINE_CHECK(db.state(id) == AssetState::Failed);
	const auto failures = db.validate_all();
	ENGINE_REQUIRE(failures.size() == 1);
	ENGINE_CHECK_EQ(failures[0].path, std::string("broken.obj"));

	write_text(root / "broken.obj", triangle_obj);
	ENGINE_CHECK(static_cast<bool>(db.load_mesh(id)));
	ENGINE_CHECK(db.state(id) == AssetState::Loaded);
}

ENGINE_TEST(database_scan_reports_catalogue_problems)
{
	const fs::path root = scratch("issues");
	write_text(root / "a.obj", triangle_obj);
	write_text(root / "a.obj.meta", "version = 1\nid = 00000000000000aa\n");
	write_text(root / "b.obj", triangle_obj);
	write_text(root / "b.obj.meta", "version = 1\nid = 00000000000000aa\n"); // Same id as a.obj.
	write_text(root / "c.obj", triangle_obj);
	write_text(root / "c.obj.meta", "version = 1\nbogus = 1\n");
	write_text(root / "orphan.tga.meta", "version = 1\n");
	write_text(root / "picture.png", "not supported");
	write_text(root / "good/d.obj", triangle_obj);

	AssetDatabase db = open_scanned(root);
	const auto all = db.assets();
	ENGINE_REQUIRE(all.size() == 1);
	ENGINE_CHECK_EQ(all[0].path, std::string("good/d.obj"));

	const auto issues = db.issues();
	const auto has = [&](std::string_view path, std::string_view text) {
		return std::any_of(issues.begin(), issues.end(), [&](const AssetIssue& i) {
			return i.path == path && i.message.find(text) != std::string::npos;
		});
	};
	ENGINE_CHECK(has("a.obj", "more than one asset"));
	ENGINE_CHECK(has("b.obj", "more than one asset"));
	ENGINE_CHECK(has("c.obj.meta", "unknown key"));
	ENGINE_CHECK(has("orphan.tga.meta", "no matching asset"));
	ENGINE_CHECK(has("picture.png", "unsupported format"));
	ENGINE_CHECK_EQ(issues.size(), std::size_t{5});

	// Fixing the collision on disk and rescanning brings both assets back.
	write_text(root / "b.obj.meta", "version = 1\nid = 00000000000000bb\n");
	fs::remove(root / "c.obj.meta");
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	ENGINE_CHECK_EQ(db.assets().size(), std::size_t{4});
	ENGINE_CHECK_EQ(db.issues().size(), std::size_t{2});
}

ENGINE_TEST(database_rescan_keeps_cache_for_unchanged_assets)
{
	const fs::path root = scratch("rescan");
	write_text(root / "keep.obj", triangle_obj);
	write_text(root / "settings.obj", triangle_obj);
	AssetDatabase db = open_scanned(root);
	const AssetId keep = db.find("keep.obj")->id;
	const AssetId settings = db.find("settings.obj")->id;
	auto kept = db.load_mesh(keep);
	ENGINE_REQUIRE(static_cast<bool>(db.load_mesh(settings)));

	write_text(root / "settings.obj.meta", "version = 1\nmesh.scale = 2\n");
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	ENGINE_CHECK(db.state(keep) == AssetState::Loaded);
	ENGINE_CHECK(db.load_mesh(keep).value().data == kept.value().data);
	// Import settings changed: cached data is discarded and re-imported with the new scale.
	ENGINE_CHECK(db.state(settings) == AssetState::Unloaded);
	ENGINE_CHECK_EQ(db.load_mesh(settings).value().data->bounds.max[0], 2.0f);
}

ENGINE_TEST(database_detects_case_only_collisions_where_possible)
{
	const fs::path root = scratch("case");
	write_text(root / "Rock.obj", triangle_obj);
	// On case-insensitive filesystems (default Windows/macOS) the second name aliases the first,
	// so the collision cannot exist on disk and the scan must see a single asset.
	write_text(root / "rock.obj", triangle_obj);
	std::size_t files = 0;
	for (const auto& entry : fs::directory_iterator(root)) {
		(void)entry;
		++files;
	}
	AssetDatabase db = open_scanned(root);
	if (files == 1) {
		ENGINE_CHECK_EQ(db.assets().size(), std::size_t{1});
		ENGINE_CHECK(db.issues().empty());
	} else {
		ENGINE_CHECK(db.assets().empty());
		ENGINE_CHECK_EQ(db.issues().size(), std::size_t{2});
	}
}
