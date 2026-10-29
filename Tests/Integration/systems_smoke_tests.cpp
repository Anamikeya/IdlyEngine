// End-to-end headless smoke test across module boundaries, using only public APIs:
//   asset (OBJ) -> collider sized from mesh bounds -> physics on a scene entity -> render frame extraction.
// This is also the reference integration example for the editor host (docs/integration/m2-contracts.md).

#include "engine/assets/asset_database.h"
#include "engine/physics/scene_physics.h"
#include "engine/test/test.h"

#include <engine/renderer/RenderSnapshot.hpp>

#include <filesystem>
#include <vector>

namespace commands = engine::commands;
namespace physics = engine::physics;
namespace assets = engine::assets;
namespace renderer = engine::renderer;

namespace {

constexpr std::uint32_t all_grants =
	static_cast<std::uint32_t>(commands::Capability::Inspect) | static_cast<std::uint32_t>(commands::Capability::Edit);

engine::Transform at(double x, double y, double z)
{
	engine::Transform t;
	t.position = {x, y, z};
	return t;
}

} // namespace

ENGINE_TEST(asset_physics_and_render_snapshot_work_together)
{
	// Host-side services.
	auto db_result = assets::AssetDatabase::open(std::filesystem::path(GAMEENGINE_TEST_ASSETS_DIR) / "editor");
	ENGINE_REQUIRE(static_cast<bool>(db_result));
	assets::AssetDatabase& db = db_result.value();
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	const auto cube_meta = db.find("meshes/cube.obj");
	ENGINE_REQUIRE(cube_meta.has_value());
	auto cube = db.load_mesh(cube_meta->id);
	ENGINE_REQUIRE(static_cast<bool>(cube));

	engine::Scene scene;
	commands::CommandBus editor(scene);
	commands::CommandBus runtime(scene, 0); // History-free bus for physics poses (C-1).
	const commands::Context editor_ctx{"editor", all_grants};
	const commands::Context physics_ctx{"physics", all_grants};
	auto physics_result = physics::ScenePhysics::create();
	ENGINE_REQUIRE(static_cast<bool>(physics_result));
	physics::ScenePhysics& world = physics_result.value();

	const auto create = [&](const char* name, engine::Transform t) {
		auto receipt = editor.execute(editor_ctx, editor.revision(), commands::CreateEntity{name, t, 0, 0});
		ENGINE_REQUIRE(static_cast<bool>(receipt));
		return receipt.value().created.front();
	};
	const engine::EntityId camera = create("camera", at(0, 2, 10));
	const engine::EntityId ground = create("ground", at(0, -0.5, 0));
	const engine::EntityId crate = create("crate", at(0, 3, 0));

	ENGINE_REQUIRE(static_cast<bool>(world.set_rigid_body(scene, ground, {physics::BodyType::Static})));
	physics::Collider floor;
	floor.shape = physics::BoxShape{{20.0f, 0.5f, 20.0f}, {}};
	ENGINE_REQUIRE(static_cast<bool>(world.add_collider(ground, floor)));

	// Box collider fitted to the mesh's bounds: assets feed physics without either knowing the other.
	const assets::Bounds& b = cube.value().data->bounds;
	physics::Collider crate_box;
	crate_box.shape = physics::BoxShape{{(b.max[0] - b.min[0]) * 0.5f, (b.max[1] - b.min[1]) * 0.5f, (b.max[2] - b.min[2]) * 0.5f}, {}};
	ENGINE_REQUIRE(static_cast<bool>(world.set_rigid_body(scene, crate, {})));
	ENGINE_REQUIRE(static_cast<bool>(world.add_collider(crate, crate_box)));

	world.set_simulation_enabled(true);
	for (int i = 0; i < 180; ++i) {
		ENGINE_REQUIRE(static_cast<bool>(world.fixed_update(world.config().fixed_timestep, scene, runtime, physics_ctx)));
	}
	const auto crate_world = scene.world_transform(crate);
	ENGINE_REQUIRE(crate_world.has_value());
	ENGINE_CHECK_NEAR(crate_world->matrix[13], 0.5, 0.02); // Resting on the ground.
	ENGINE_CHECK_EQ(world.debug_geometry().lines.size(), std::size_t{24}); // Two boxes, 12 edges each.
	ENGINE_CHECK(world.diagnostics().issues.empty());

	// The renderer would map (AssetId, revision) to its own handle (C-5); a placeholder handle stands in here.
	const renderer::MeshHandle mesh_handle{1, cube.value().revision};
	const std::vector<renderer::MeshBinding> meshes{{crate, mesh_handle, renderer::MaterialHandle{1, 1}, true}};
	renderer::PerspectiveCamera lens;
	lens.entity = camera;
	auto frame = renderer::extract_frame(scene, renderer::FrameStamp{1, runtime.revision(), 1}, lens, meshes, {});
	ENGINE_REQUIRE(static_cast<bool>(frame));
	ENGINE_REQUIRE(frame.value()->objects.size() == 1);
	ENGINE_CHECK_EQ(frame.value()->objects[0].entity, crate);
	// The frame carries the physics-driven pose published into the scene.
	ENGINE_CHECK(frame.value()->objects[0].world.matrix == crate_world->matrix);
}
