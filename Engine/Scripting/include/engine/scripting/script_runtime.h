#pragma once

// One Lua state for one run of a game (Milestone 6, D2 to D4). Scripts are classes: a script returns a table whose
// `properties` declare what Details shows and whose functions the engine calls on each entity's instance, `self`.
//
// The sandbox (D3): scripts get the base functions without dofile, loadfile or bytecode, and math, string, table,
// utf8 and coroutine. No io, os, debug or package: `require "/Game/Scripts/util"` loads another script through the
// loader. Each script has its own globals, which fall back to the shared libraries.
//
// Budgets: a call that runs more than `instructions_per_call` Lua instructions is stopped with an error, and stays
// stopped even if the script catches that error. The state as a whole may not hold more than `memory_bytes`.
//
// Errors (D4): a failing call disables only that instance and is reported once, with the script, line and traceback.
// Everything is deterministic: no clock reaches a script, math.random is seeded by the host, string hashing has a
// fixed seed, and instances run in the order of their entity IDs.

#include <engine/scripting/script_host.h>
#include <engine/scripting/script_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::scripting {

struct RuntimeLimits {
	std::size_t memory_bytes = std::size_t{256} << 20;
	// A safety net, not a frame budget: 36 to 42 ms of a plain Lua loop on a Ryzen 7 5800X, in Release and CI builds.
	std::uint64_t instructions_per_call = 10'000'000;
};

using PropertyOverride = std::pair<std::string, Value>;

class ScriptRuntime {
public:
	// `host` is the world scripts act on, which must outlive the runtime; without one, what needs a world raises an error
	// in the script that asked (a script's top level, read by Details, has none).
	explicit ScriptRuntime(ScriptLoader loader, RuntimeLimits limits = {}, std::uint64_t random_seed = 1, ScriptHost* host = nullptr);
	~ScriptRuntime();
	ScriptRuntime(const ScriptRuntime&) = delete;
	ScriptRuntime& operator=(const ScriptRuntime&) = delete;

	// Loads a script and what it requires, or gives the one already loaded under its canonical name. A script that
	// fails to load is reported in take_errors() too, and loading it again tries again.
	Result<ScriptInfo> load(std::string_view name);

	// The scripts and modules loaded, by the names their loader gave them, ascending.
	[[nodiscard]] std::vector<std::string> loaded() const;
	// Hot reload (D8): loads a script again from its loader. One that runs and returns a class replaces the old class
	// under every instance: each keeps its `self`, gains the defaults of properties that are new, and calls the new
	// functions from then on. One that fails keeps the old code running, and its error is in take_errors() too. A module
	// is run again when next required, and every script is reloaded so that each requires the new one. Not from inside
	// a call.
	Result<void> reload(std::string_view name);

	// Gives an entity an instance of a script: `self` holds each property, the override when there is one and
	// otherwise the default, and `self.entity`. An override of the wrong type is reported and the default used; one
	// the script does not declare is ignored. Replaces the entity's instance if it has one. Nothing is called: start
	// is the host's call.
	Result<void> attach(EntityId entity, std::string_view script, std::span<const PropertyOverride> overrides = {});
	void detach(EntityId entity);

	[[nodiscard]] bool has_instance(EntityId entity) const;
	// False once a call on it failed.
	[[nodiscard]] bool enabled(EntityId entity) const;
	[[nodiscard]] std::vector<EntityId> instances() const; // Ascending.

	// Calls `callback` on every enabled instance whose script defines it, in entity order, with `args` after self.
	// Instances attached meanwhile are not called. Returns how many calls succeeded. Never detach() from inside a call.
	std::size_t call(std::string_view callback, std::span<const Value> args = {});
	// Calls it on one instance. False when it has no such instance or function, is disabled, or the call failed.
	bool call(EntityId entity, std::string_view callback, std::span<const Value> args = {});

	// A field of an instance's `self`, when it holds a value of one of the plain types.
	[[nodiscard]] std::optional<Value> field(EntityId entity, std::string_view name) const;

	// Errors and printed lines since the last take, oldest first.
	std::vector<ScriptError> take_errors();
	std::vector<ScriptOutput> take_output();

	[[nodiscard]] std::size_t memory_used() const noexcept;
	[[nodiscard]] const RuntimeLimits& limits() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// What a script declares, read without a game: its top level runs in a throwaway runtime with small budgets.
Result<ScriptInfo> inspect_script(std::string_view name, const ScriptLoader& loader);

// In the script's text, the order its properties are first named in, for Details. Names the text never names in
// `name =` form keep their relative order after the others.
void order_by_source(std::vector<PropertyDecl>& properties, std::string_view text);

} // namespace engine::scripting
