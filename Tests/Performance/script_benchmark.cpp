// What scripts cost, without a window (Milestone 6, slice 6). A tool, not a test: it prints times and checks nothing
// about them. The real scene, command bus, Box3D world and script runtime, ticked as a play session ticks them, with
// cubes like those of the sample's script_stress scene, each running a script, and no game.
//
//   script_benchmark [--entities 500] [--ticks 600] [--script <file>]...
//
// It times, per tick: the scripts' calls before physics (ScriptWorld::pre_physics, applying what they changed
// included), physics (which syncs the scene and steps), and the calls after it (late_tick, which none of these
// scripts has). Five scripts tell the costs apart: one whose tick does nothing, one that does the Mover's arithmetic
// but keeps the result, one that reads its entity's position, one that moves it between its two points with no
// arithmetic, and the sample's own Mover.lua. It also times a scene snapshot and one tick's moves sent through the
// command bus with no script.
#include <engine/scripting/script_loader.h>
#include <engine/scripting/script_world.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::scripting;

namespace {

constexpr std::chrono::nanoseconds fixed_step{16'666'667};
using Clock = std::chrono::steady_clock;
using Sources = std::map<std::string, std::string, std::less<>>;

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

const char* const header = "local S = { properties = { a = Vec3(0, 0, 0), b = Vec3(0, 0, 0), speed = 2 } }\n"
						   "function S:start() self.travelled = 0 end\n";

// Each with the Mover's properties, so the scene's values are the same for all four.
std::string empty_script() { return std::string(header) + "function S:tick(dt) end\nreturn S\n"; }
std::string math_script()
{
	return std::string(header) +
		"function S:tick(dt)\n"
		"  local a, b = self.a, self.b\n"
		"  local length = (b - a):length()\n"
		"  if length <= 0 then return end\n"
		"  self.travelled = math.fmod(self.travelled + math.max(self.speed, 0) * dt, 2 * length)\n"
		"  local along = (self.travelled <= length and self.travelled or 2 * length - self.travelled) / length\n"
		"  self.last = a + (b - a) * along\n"
		"end\nreturn S\n";
}
std::string read_script() { return std::string(header) + "function S:tick(dt) self.last = self.entity.position end\nreturn S\n"; }
std::string write_script()
{
	return std::string(header) + "function S:tick(dt)\n  self.up = not self.up\n  self.entity.position = self.up and self.a or self.b\nend\nreturn S\n";
}

std::string read_file(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) throw std::runtime_error("cannot read " + path);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

struct Times {
	double pre = 0, physics = 0, post = 0, pre_max = 0, first = 0;
	std::size_t instances = 0, errors = 0;
};

void check(bool ok, const char* what)
{
	if (!ok) throw std::runtime_error(what);
}

// The scene: `count` cubes 25 to a row, as in script_stress.scene, each running /Game/Scripts/Bench.
void populate(commands::CommandBus& bus, const commands::Context& context, int count, bool scripted)
{
	std::vector<commands::Command> batch;
	const auto flush = [&] {
		if (batch.empty()) return;
		check(static_cast<bool>(bus.execute(context, bus.revision(), std::span<const commands::Command>(batch))), "creating the cubes failed");
		batch.clear();
	};
	for (int i = 0; i < count; ++i) {
		const int column = i % 25, row = i / 25;
		const double x = -12 + column, z = -12 + row;
		Vec3 low{x, 5.5, z}, high{x, 7.5, z};
		if ((column + row) % 2) std::swap(low, high);
		Transform t;
		t.position = low;
		t.scale = {0.3, 0.3, 0.3};
		Components c;
		c.mesh = MeshComponent{};
		if (scripted)
			c.script = ScriptComponent{engine::AssetRef{0x5c71b7, "Scripts/Bench.lua"},
				{{"a", low}, {"b", high}, {"speed", 1 + double((column * 7 + row * 3) % 10) * 0.2}}};
		batch.emplace_back(commands::CreateEntity{"Script Mover " + std::to_string(i + 1), t, 0, 0, std::move(c)});
		if (batch.size() == commands::CommandBus::max_transaction) flush();
	}
	flush();
}

Times run_scripts(const std::string& text, int count, int ticks)
{
	Sources sources{{"/Game/Scripts/Bench", text}};
	Scene scene;
	commands::CommandBus bus{scene, 0};
	const commands::Context context{"benchmark", 3};
	populate(bus, context, count, true);
	auto created = physics::ScenePhysics::create();
	check(static_cast<bool>(created), "no physics world");
	auto physics = std::make_unique<physics::ScenePhysics>(std::move(created.value()));
	physics->set_simulation_enabled(true);
	check(static_cast<bool>(physics->sync(scene)), "physics sync failed");
	ScriptWorld::Setup setup;
	setup.loader = memory_loader(sources);
	setup.names = [](const engine::AssetRef&) { return std::string("/Game/Scripts/Bench"); };
	auto world = ScriptWorld::create(scene, bus, *physics, nullptr, fixed_step, std::move(setup));
	check(world != nullptr, "no scripts attached");
	Times times;
	times.instances = world->runtime().instances().size();
	for (int tick = 0; tick <= ticks; ++tick) {
		const auto start = Clock::now();
		world->pre_physics();
		const auto before_physics = Clock::now();
		check(static_cast<bool>(physics->fixed_update(fixed_step, scene, bus, context)), "physics failed");
		const auto after_physics = Clock::now();
		world->post_physics({});
		const auto end = Clock::now();
		if (tick == 0) { // start() runs on the first tick: reported apart.
			times.first = ms(before_physics - start);
			continue;
		}
		times.pre += ms(before_physics - start);
		times.pre_max = std::max(times.pre_max, ms(before_physics - start));
		times.physics += ms(after_physics - before_physics);
		times.post += ms(end - after_physics);
	}
	times.errors = world->runtime().take_errors().size();
	times.pre /= ticks;
	times.physics /= ticks;
	times.post /= ticks;
	return times;
}

} // namespace

int main(int argc, char** argv)
{
	try {
		int count = 500, ticks = 600;
		std::vector<std::string> extra; // --script <file>: times a script of your own too, with the Mover's properties.
		for (int i = 1; i < argc; ++i) {
			if (!std::strcmp(argv[i], "--entities") && i + 1 < argc) count = std::atoi(argv[++i]);
			else if (!std::strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = std::atoi(argv[++i]);
			else if (!std::strcmp(argv[i], "--script") && i + 1 < argc) extra.emplace_back(argv[++i]);
			else throw std::runtime_error("usage: script_benchmark [--entities N] [--ticks T] [--script <file>]...");
		}
		if (count < 1 || count > 20000 || ticks < 1) throw std::runtime_error("--entities is 1 to 20000 and --ticks at least 1");
		std::printf("entities %d\nticks %d\n", count, ticks);

		// The command bus alone: a snapshot, and one tick's moves of every cube, with no script.
		{
			Scene scene;
			commands::CommandBus bus{scene, 0};
			const commands::Context context{"benchmark", 3};
			populate(bus, context, count, false);
			const int rounds = std::max(1, ticks / 10);
			auto start = Clock::now();
			std::size_t held = 0;
			for (int r = 0; r < rounds; ++r) held += scene.snapshot().entities.size();
			check(held == std::size_t(count) * std::size_t(rounds), "the snapshot lost entities");
			std::printf("snapshot_ms %.4f\n", ms(Clock::now() - start) / rounds);
			const auto snapshot = scene.snapshot();
			start = Clock::now();
			for (int r = 0; r < rounds; ++r) {
				std::vector<commands::Command> batch;
				batch.reserve(commands::CommandBus::max_transaction);
				for (const auto& e : snapshot.entities) {
					Transform t = e.transform;
					t.position.y += 0.001 * (r + 1);
					batch.emplace_back(commands::SetTransform{e.id, t});
					if (batch.size() == commands::CommandBus::max_transaction) {
						check(static_cast<bool>(bus.execute(context, bus.revision(), std::span<const commands::Command>(batch))), "moving failed");
						batch.clear();
					}
				}
				if (!batch.empty()) check(static_cast<bool>(bus.execute(context, bus.revision(), std::span<const commands::Command>(batch))), "moving failed");
			}
			std::printf("bus_moves_ms %.4f\n", ms(Clock::now() - start) / rounds);
		}

		const std::string mover = read_file(std::string(GAMEENGINE_SAMPLES_DIR) + "/PhysicsShooter/Assets/Scripts/Mover.lua");
		std::vector<std::pair<std::string, std::string>> scripts{
			{"empty", empty_script()}, {"math", math_script()}, {"read", read_script()}, {"write", write_script()}, {"mover", mover}};
		for (const auto& file : extra) scripts.emplace_back(file, read_file(file));
		for (const auto& [name, text] : scripts) {
			const Times t = run_scripts(text, count, ticks);
			std::printf("%-5s instances %zu errors %zu | first tick %.3f ms | scripts before physics %.4f ms mean %.4f max | physics %.4f ms | "
						"after %.4f ms | per instance %.2f us\n",
				name.c_str(), t.instances, t.errors, t.first, t.pre, t.pre_max, t.physics, t.post, 1000.0 * (t.pre + t.post) / double(t.instances));
		}
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "script_benchmark failed: %s\n", e.what());
		return 1;
	}
}
