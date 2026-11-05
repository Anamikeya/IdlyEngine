// Long package names, redirectors and the asset database's side of them (Milestone 5, slice 7).

#include "engine/assets/asset_database.h"
#include "engine/assets/virtual_paths.h"
#include "engine/test/test.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace engine::assets;
using engine::ErrorCode;
namespace fs = std::filesystem;

namespace {

AssetMetadata asset(std::string path, std::uint64_t id)
{
	AssetMetadata meta;
	meta.path = std::move(path);
	meta.id = AssetId{id};
	meta.type = asset_type_for(meta.path);
	return meta;
}

// A mutex inside makes it immovable, so it comes on the heap.
std::unique_ptr<VirtualFileSystem> mounted(const std::vector<AssetMetadata>& assets, const std::vector<Redirector>& redirectors = {})
{
	auto vfs = std::make_unique<VirtualFileSystem>();
	ENGINE_REQUIRE(static_cast<bool>(vfs->mount("/Game", fs::path("C:/Project/Assets"))));
	ENGINE_REQUIRE(static_cast<bool>(vfs->index("/Game", assets, redirectors)));
	return vfs;
}

fs::path scratch(const char* name)
{
	const fs::path dir = fs::current_path() / "virtual_path_tests_tmp" / name;
	fs::remove_all(dir);
	fs::create_directories(dir);
	return dir;
}

void write_text(const fs::path& path, std::string_view text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

ENGINE_TEST(package_names_drop_the_extension_only)
{
	ENGINE_CHECK_EQ(package_path_of("Textures/Brick.png"), std::string("Textures/Brick"));
	ENGINE_CHECK_EQ(package_path_of("Brick.material"), std::string("Brick"));
	ENGINE_CHECK_EQ(package_path_of("v1.2/Brick"), std::string("v1.2/Brick"));
	ENGINE_CHECK_EQ(package_path_of("Textures/.hidden"), std::string("Textures/.hidden"));
	ENGINE_CHECK_EQ(package_path_of("a.b/c.d.png"), std::string("a.b/c.d"));
	ENGINE_CHECK_EQ(package_name("/Game", "Textures/Brick.png"), std::string("/Game/Textures/Brick"));
	ENGINE_CHECK_EQ(fold_case("/Game/Textures/BRICK.png"), std::string("/game/textures/brick.png"));
	// Only ASCII is folded: UTF-8 bytes pass through.
	ENGINE_CHECK_EQ(fold_case("Ziegel/\xc3\x84pfel"), std::string("ziegel/\xc3\x84pfel"));
}

ENGINE_TEST(names_resolve_whatever_their_case)
{
	const auto vfs = mounted({asset("Textures/Brick.png", 11), asset("Materials/Wall.material", 12)});
	for (const char* name : {"/Game/Textures/Brick", "/game/textures/brick", "/GAME/TEXTURES/BRICK", "/Game/Textures/Brick.png",
			 "/game/textures/brick.PNG", "/Game\\Textures\\Brick"}) {
		const auto found = vfs->resolve(name);
		ENGINE_REQUIRE(static_cast<bool>(found));
		ENGINE_CHECK_EQ(found.value().asset_path, std::string("Textures/Brick.png"));
		ENGINE_CHECK_EQ(found.value().mount_point, std::string("/Game"));
		ENGINE_CHECK(found.value().id == AssetId{11});
		ENGINE_CHECK(found.value().type == AssetType::Texture);
		ENGINE_CHECK(found.value().redirects.empty());
		ENGINE_CHECK(found.value().file == fs::path("C:/Project/Assets") / "Textures/Brick.png");
	}
	const auto name = vfs->name_of(AssetId{12});
	ENGINE_REQUIRE(name.has_value());
	ENGINE_CHECK_EQ(*name, std::string("/Game/Materials/Wall"));
	ENGINE_CHECK(!vfs->name_of(AssetId{99}).has_value());
}

ENGINE_TEST(a_bare_name_two_assets_share_is_a_conflict)
{
	VirtualFileSystem vfs;
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/Game", "root")));
	const std::vector<AssetMetadata> assets{asset("Textures/Brick.png", 1), asset("Textures/Brick.material", 2), asset("Textures/Stone.png", 3)};
	const auto conflicts = vfs.index("/Game", assets, {});
	ENGINE_REQUIRE(static_cast<bool>(conflicts));
	ENGINE_REQUIRE(conflicts.value().size() == 1u);
	ENGINE_CHECK(conflicts.value().front().find("Textures/Brick.png") != std::string::npos);
	ENGINE_CHECK(conflicts.value().front().find("Textures/Brick.material") != std::string::npos);

	const auto bare = vfs.resolve("/Game/Textures/Brick");
	ENGINE_REQUIRE(!bare);
	ENGINE_CHECK(bare.error().code == ErrorCode::Conflict);
	ENGINE_CHECK(bare.error().message.find("add the extension") != std::string::npos);
	// The extension still names each, and so does what name_of() shows for them.
	const auto png = vfs.resolve("/Game/Textures/Brick.png"), material = vfs.resolve("/Game/textures/brick.material");
	ENGINE_REQUIRE(png && material);
	ENGINE_CHECK(png.value().id == AssetId{1});
	ENGINE_CHECK(material.value().id == AssetId{2});
	const auto shown = vfs.name_of(AssetId{2}), plain = vfs.name_of(AssetId{3});
	ENGINE_REQUIRE(shown && plain);
	ENGINE_CHECK_EQ(*shown, std::string("/Game/Textures/Brick.material"));
	ENGINE_CHECK_EQ(*plain, std::string("/Game/Textures/Stone"));
}

ENGINE_TEST(malformed_and_unknown_names_say_why)
{
	const auto vfs = mounted({asset("Textures/Brick.png", 1)});
	for (const char* name : {"", "Game/Textures/Brick", "/", "/Game", "/Game/", "/Ga me/Brick", "/Game/../Brick", "/Game/a:b"}) {
		const auto r = vfs->resolve(name);
		ENGINE_CHECK(!r);
		if (!r) {
			ENGINE_CHECK(r.error().code == ErrorCode::InvalidArgument);
		}
	}
	const auto unmounted = vfs->resolve("/Engine/Textures/Brick");
	ENGINE_REQUIRE(!unmounted);
	ENGINE_CHECK(unmounted.error().code == ErrorCode::NotFound);
	ENGINE_CHECK(unmounted.error().message.find("/Engine") != std::string::npos);
	const auto missing = vfs->resolve("/Game/Textures/Marble");
	ENGINE_REQUIRE(!missing);
	ENGINE_CHECK(missing.error().code == ErrorCode::NotFound);
}

ENGINE_TEST(redirectors_are_followed_to_the_asset)
{
	// Brick moved to Walls/Brick, then to Walls/Old/Brick: two redirectors in a row.
	const auto vfs = mounted({asset("Textures/Walls/Old/Brick.png", 7)},
		{{"Textures/Brick.png", "Textures/Walls/Brick.png", AssetId{7}}, {"Textures/Walls/Brick.png", "Textures/Walls/Old/Brick.png", AssetId{7}}});
	const auto found = vfs->resolve("/game/textures/brick");
	ENGINE_REQUIRE(static_cast<bool>(found));
	ENGINE_CHECK_EQ(found.value().asset_path, std::string("Textures/Walls/Old/Brick.png"));
	ENGINE_CHECK(found.value().id == AssetId{7});
	ENGINE_REQUIRE(found.value().redirects.size() == 2u);
	ENGINE_CHECK_EQ(found.value().redirects[0], std::string("/Game/Textures/Brick"));
	ENGINE_CHECK_EQ(found.value().redirects[1], std::string("/Game/Textures/Walls/Brick"));
	// Starting halfway goes through one.
	const auto half = vfs->resolve("/Game/Textures/Walls/Brick.png");
	ENGINE_REQUIRE(static_cast<bool>(half));
	ENGINE_CHECK(half.value().redirects.size() == 1u);
	// A redirector whose target went away names it.
	const auto dangling = mounted({}, {{"Textures/Brick.png", "Textures/Gone.png", AssetId{}}})->resolve("/Game/Textures/Brick");
	ENGINE_REQUIRE(!dangling);
	ENGINE_CHECK(dangling.error().code == ErrorCode::NotFound);
	ENGINE_CHECK(dangling.error().message.find("Textures/Gone.png") != std::string::npos);
}

ENGINE_TEST(a_redirector_loop_is_named_not_followed_forever)
{
	const auto vfs = mounted({}, {{"A.png", "B.png", AssetId{}}, {"B.png", "C.png", AssetId{}}, {"C.png", "A.png", AssetId{}}});
	const auto r = vfs->resolve("/Game/A");
	ENGINE_REQUIRE(!r);
	ENGINE_CHECK(r.error().code == ErrorCode::Cycle);
	ENGINE_CHECK(r.error().message.find("/Game/A -> /Game/B -> /Game/C -> /Game/A") != std::string::npos);
}

ENGINE_TEST(redirector_chains_stop_at_the_limit)
{
	const auto chain = [](std::size_t redirects) {
		std::vector<Redirector> rs;
		for (std::size_t i = 0; i < redirects; ++i) {
			rs.push_back({"R" + std::to_string(i) + ".png", "R" + std::to_string(i + 1) + ".png", AssetId{}});
		}
		return mounted({asset("R" + std::to_string(redirects) + ".png", 5)}, rs)->resolve("/Game/R0");
	};
	const auto longest = chain(VirtualFileSystem::max_redirects);
	ENGINE_REQUIRE(static_cast<bool>(longest));
	ENGINE_CHECK(longest.value().redirects.size() == VirtualFileSystem::max_redirects);
	const auto too_long = chain(VirtualFileSystem::max_redirects + 1);
	ENGINE_REQUIRE(!too_long);
	ENGINE_CHECK(too_long.error().code == ErrorCode::InvalidState);
}

ENGINE_TEST(an_asset_wins_over_a_redirector_with_its_name)
{
	VirtualFileSystem vfs;
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/Game", "root")));
	const std::vector<AssetMetadata> assets{asset("Textures/Brick.png", 1), asset("Textures/Walls/Brick.png", 2)};
	const std::vector<Redirector> redirectors{{"Textures/Brick.png", "Textures/Walls/Brick.png", AssetId{2}}};
	const auto conflicts = vfs.index("/Game", assets, redirectors);
	ENGINE_REQUIRE(static_cast<bool>(conflicts));
	ENGINE_REQUIRE(conflicts.value().size() == 1u);
	ENGINE_CHECK(conflicts.value().front().find("hidden") != std::string::npos);
	const auto found = vfs.resolve("/Game/Textures/Brick");
	ENGINE_REQUIRE(static_cast<bool>(found));
	ENGINE_CHECK(found.value().id == AssetId{1});
	ENGINE_CHECK(found.value().redirects.empty());
}

ENGINE_TEST(mount_points_are_checked_and_replaced)
{
	VirtualFileSystem vfs;
	for (const char* bad : {"", "/", "Game", "/Game/Sub", "/Ga me", "/G\xc3\xa4me"}) {
		const auto r = vfs.mount(bad, "root");
		ENGINE_CHECK(!r);
	}
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/Game", "one")));
	ENGINE_REQUIRE(static_cast<bool>(vfs.index("/Game", std::vector<AssetMetadata>{asset("A.png", 1)}, {})));
	// The same name in another case replaces it, empty.
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/game", "two")));
	ENGINE_CHECK(vfs.mount_points() == std::vector<std::string>{"/game"});
	ENGINE_CHECK(!vfs.resolve("/Game/A"));
	ENGINE_REQUIRE(static_cast<bool>(vfs.index("/GAME", std::vector<AssetMetadata>{asset("A.png", 1)}, {})));
	const auto found = vfs.resolve("/Game/A");
	ENGINE_REQUIRE(static_cast<bool>(found));
	ENGINE_CHECK(found.value().file == fs::path("two") / "A.png");
	ENGINE_CHECK_EQ(found.value().mount_point, std::string("/game"));
	vfs.unmount("/Game");
	ENGINE_CHECK(vfs.mount_points().empty());
	const auto indexed = vfs.index("/Game", std::vector<AssetMetadata>{}, {});
	ENGINE_REQUIRE(!indexed);
	ENGINE_CHECK(indexed.error().code == ErrorCode::NotFound);
}

ENGINE_TEST(names_resolve_from_other_threads_while_the_index_changes)
{
	VirtualFileSystem vfs;
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/Game", "root")));
	const std::vector<AssetMetadata> before{asset("Textures/Brick.png", 1)}, after{asset("Textures/Walls/Brick.png", 1)};
	const std::vector<Redirector> moved{{"Textures/Brick.png", "Textures/Walls/Brick.png", AssetId{1}}};
	ENGINE_REQUIRE(static_cast<bool>(vfs.index("/Game", before, {})));
	std::atomic<bool> done = false;
	std::atomic<int> resolved = 0, wrong = 0;
	std::vector<std::thread> readers;
	for (int t = 0; t < 4; ++t) {
		readers.emplace_back([&] {
			while (!done) {
				// Before the move it is found directly, after it through the redirector: the same asset either way.
				const auto r = vfs.resolve("/game/textures/brick");
				if (!r || r.value().id != AssetId{1}) {
					++wrong;
				}
				++resolved;
			}
		});
	}
	// Re-index until the readers have done plenty while it happened, so the two really overlap; capped at 10 s.
	const auto start = std::chrono::steady_clock::now();
	int swaps = 0;
	while ((swaps < 400 || resolved.load() < 2000) && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
		const bool odd = swaps++ % 2 == 1;
		const auto indexed = vfs.index("/Game", odd ? after : before, odd ? moved : std::vector<Redirector>{});
		ENGINE_CHECK(static_cast<bool>(indexed));
	}
	done = true;
	for (auto& reader : readers) {
		reader.join();
	}
	ENGINE_CHECK(resolved.load() >= 2000);
	ENGINE_CHECK(swaps >= 400);
	ENGINE_CHECK_EQ(wrong.load(), 0);
}

ENGINE_TEST(redirector_files_round_trip_and_reject_nonsense)
{
	const Redirector r{"Textures/Brick.png", "Textures/Walls/Brick.png", AssetId{0x1234}};
	const auto parsed = parse_redirector(write_redirector(r), "Textures/Brick.png");
	ENGINE_REQUIRE(static_cast<bool>(parsed));
	ENGINE_CHECK(parsed.value() == r);
	// Without an id, and with Windows line endings.
	const auto plain = parse_redirector("version = 1\r\ntarget = Textures\\Walls\\Brick.png\r\n", "Textures/Brick.png");
	ENGINE_REQUIRE(static_cast<bool>(plain));
	ENGINE_CHECK_EQ(plain.value().target, std::string("Textures/Walls/Brick.png"));
	ENGINE_CHECK(plain.value().id.is_null());
	const std::vector<std::pair<std::string, std::string>> bad{
		{"target = B.png\n", "A.png"},                         // No version.
		{"version = 1\n", "A.png"},                            // No target.
		{"version = 2\ntarget = B.png\n", "A.png"},            // Unknown version.
		{"version = 1\ntarget = B.material\n", "A.png"},       // Another type.
		{"version = 1\ntarget = a.PNG\n", "A.png"},            // Itself.
		{"version = 1\ntarget = ../B.png\n", "A.png"},         // Outside the root.
		{"version = 1\ntarget = B.png\nid = 12\n", "A.png"},   // Short id.
		{"version = 1\ntarget = B.png\ncolor = red\n", "A.png"}, // Unknown key.
		{"version = 1\ntarget = B.png\n", "A.txt"},            // Not at an asset's path.
	};
	for (const auto& [text, path] : bad) {
		ENGINE_CHECK(!parse_redirector(text, path));
	}
}

ENGINE_TEST(moving_an_asset_can_leave_a_redirector)
{
	const fs::path root = scratch("move");
	write_text(root / "Textures" / "Brick.png", "not decoded by a scan");
	auto opened = AssetDatabase::open(root);
	ENGINE_REQUIRE(static_cast<bool>(opened));
	auto& db = opened.value();
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	const auto brick = db.find(std::string_view("Textures/Brick.png"));
	ENGINE_REQUIRE(brick.has_value());
	const auto scans = db.scans();

	ENGINE_REQUIRE(static_cast<bool>(db.move(brick->id, "Textures/Walls/Brick.png", true)));
	ENGINE_CHECK(db.scans() > scans);
	ENGINE_CHECK(fs::exists(root / "Textures" / "Brick.png.redirect"));
	ENGINE_CHECK(db.issues().empty());
	ENGINE_REQUIRE(db.redirectors().size() == 1u);
	const auto left = db.redirectors().front();
	ENGINE_CHECK_EQ(left.path, std::string("Textures/Brick.png"));
	ENGINE_CHECK_EQ(left.target, std::string("Textures/Walls/Brick.png"));
	ENGINE_CHECK(left.id == brick->id);
	ENGINE_CHECK(!db.find(std::string_view("Textures/Brick.png")).has_value());

	// Moved again: a chain, which the VFS follows from the first name.
	ENGINE_REQUIRE(static_cast<bool>(db.move(brick->id, "Old/Brick.png", true)));
	VirtualFileSystem vfs;
	ENGINE_REQUIRE(static_cast<bool>(vfs.mount("/Game", root)));
	const auto everything = db.assets();
	ENGINE_REQUIRE(static_cast<bool>(vfs.index("/Game", everything, db.redirectors())));
	const auto found = vfs.resolve("/Game/Textures/Brick");
	ENGINE_REQUIRE(static_cast<bool>(found));
	ENGINE_CHECK_EQ(found.value().asset_path, std::string("Old/Brick.png"));
	ENGINE_CHECK(found.value().redirects.size() == 2u);
	ENGINE_CHECK(fs::exists(found.value().file));

	// Moving back onto a redirector's name takes the name from it.
	ENGINE_REQUIRE(static_cast<bool>(db.move(brick->id, "Textures/Brick.png", true)));
	ENGINE_CHECK(!fs::exists(root / "Textures" / "Brick.png.redirect"));
	ENGINE_CHECK(db.issues().empty());
	ENGINE_CHECK(db.find(std::string_view("Textures/Brick.png")).has_value());
	// Without the flag, nothing is left.
	const auto before = db.redirectors().size();
	ENGINE_REQUIRE(static_cast<bool>(db.move(brick->id, "Textures/Plain.png")));
	ENGINE_CHECK(db.redirectors().size() == before);
	ENGINE_CHECK(!fs::exists(root / "Textures" / "Brick.png.redirect"));
}

ENGINE_TEST(scans_catalogue_redirectors_and_report_broken_ones)
{
	const fs::path root = scratch("scan");
	write_text(root / "Walls" / "Brick.png", "x");
	write_text(root / "Brick.png.redirect", "version = 1\ntarget = Walls/Brick.png\n");
	write_text(root / "Broken.png.redirect", "version = 1\n");
	write_text(root / "Odd.txt.redirect", "version = 1\ntarget = Walls/Brick.png\n");
	auto opened = AssetDatabase::open(root);
	ENGINE_REQUIRE(static_cast<bool>(opened));
	auto& db = opened.value();
	const auto count = db.scan();
	ENGINE_REQUIRE(static_cast<bool>(count));
	ENGINE_CHECK_EQ(count.value(), std::size_t{1});
	ENGINE_REQUIRE(db.redirectors().size() == 1u);
	ENGINE_CHECK_EQ(db.redirectors().front().path, std::string("Brick.png"));
	// The broken ones are issues; none is called an unsupported format.
	ENGINE_CHECK(db.issues().size() == 2u);
	for (const auto& issue : db.issues()) {
		ENGINE_CHECK(issue.message.find("unsupported format") == std::string::npos);
	}

	const std::vector<std::string> gone{"Brick.png", "Nothing.png"};
	const auto removed = db.remove_redirectors(gone);
	ENGINE_REQUIRE(static_cast<bool>(removed));
	ENGINE_CHECK_EQ(removed.value(), std::size_t{1});
	ENGINE_CHECK(db.redirectors().empty());
	ENGINE_CHECK(!fs::exists(root / "Brick.png.redirect"));
	ENGINE_CHECK(read_text(root / "Walls" / "Brick.png") == "x");
}
