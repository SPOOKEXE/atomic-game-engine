// The control surface, driven the way a client drives it.
//
// **Through `Answer` rather than by calling a tool's lambda**, because the half
// that can be silently wrong is the protocol: a tool registered but not listed,
// a refusal reported as a transport error, a schema that says a field is
// required and a handler that does not check it. Calling the lambda directly
// would test the body and skip all of that.
//
// The module had no suite until v0.12, which is a gap rather than a convention -
// `AGENTS.md` says so in those words. These cases open with the storage tools
// this version added, and cover the shared table around them.

#include <engine/control/Features.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.control.surface")
TEST_DEPENDS("engine.ecs.schema")

using engine::control::Feature;
using engine::control::Surface;
using engine::control::Tool;
using engine::core::Name;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::FieldSpec;
using engine::ecs::PropertyType;
using engine::ecs::Schema;
using engine::ecs::Schemas;
using engine::ecs::Store;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	// The component table is process-wide and nothing unregisters, so a case
	// naming a component another suite in this binary also names would be
	// agreeing with it rather than declaring its own.
	std::string Unique(const char *what) {
		static int counter = 0;
		return std::string("engine.control.surface.test.") + what + "." + std::to_string(counter++);
	}

	// One request in, one parsed reply out.
	json Ask(Surface &surface, const std::string &method, const json &parameters = json::object()) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 1},
			{"method", method},
			{"params", parameters},
		};

		const std::string reply = surface.Answer(request.dump());
		INFO(method << " -> " << reply);
		REQUIRE_FALSE(reply.empty());
		return json::parse(reply);
	}

	// A tool's payload, parsed back out of the text block it travels in.
	//
	// **Text rather than structured content is the surface's own decision** -
	// see `Surface.cpp` - so a test reading a result has to undo it, and this is
	// the one place that knows how.
	json Called(Surface &surface, const std::string &tool, const json &arguments, bool &failed) {
		const json reply = Ask(surface, "tools/call", json{{"name", tool}, {"arguments", arguments}});

		REQUIRE(reply.contains("result"));
		const json &result = reply["result"];

		failed = result.value("isError", false);
		REQUIRE(result.contains("content"));
		REQUIRE(result["content"].is_array());
		REQUIRE_FALSE(result["content"].empty());

		return json::parse(result["content"][0]["text"].get<std::string>(), nullptr, false);
	}

	json Called(Surface &surface, const std::string &tool, const json &arguments) {
		bool failed = false;
		const json payload = Called(surface, tool, arguments, failed);
		INFO(tool << " refused: " << payload.dump());
		REQUIRE_FALSE(failed);
		return payload;
	}

	// A universe with one named world, which is what every tool defaults to.
	WorldId MakeWorld(Universe &universe, const char *name) {
		WorldSettings settings;
		settings.Name = Name(name);
		return universe.Create(settings);
	}
}

// --- the protocol -------------------------------------------------------------

TEST_CASE("the handshake reports the program and its tools", "[control]") {
	Universe universe;
	MakeWorld(universe, "control-handshake");

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	const json opened = Ask(surface, "initialize");
	REQUIRE(opened.contains("result"));
	CHECK(opened["result"].contains("protocolVersion"));

	// **A notification gets nothing at all**, which is JSON-RPC's rule and is
	// the one part of the handshake a server can get wrong without any client
	// complaining until it hangs.
	CHECK(surface.Answer(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());

	const json listed = Ask(surface, "tools/list");
	REQUIRE(listed["result"].contains("tools"));
	CHECK(listed["result"]["tools"].size() == surface.Count());

	// Every listed tool is callable and every callable tool is listed - one
	// table read twice is the whole reason this is a registry.
	CHECK(surface.Registered().size() == surface.Count());
	for (const Tool &tool : surface.Registered()) {
		CHECK_FALSE(tool.Name.empty());
		CHECK_FALSE(tool.Description.empty());
		CHECK(static_cast<bool>(tool.Call));
	}
}

TEST_CASE("an unknown tool is a refusal rather than a protocol error", "[control]") {
	Universe universe;
	MakeWorld(universe, "control-unknown");

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	bool failed = false;
	Called(surface, "no_such_tool", json::object(), failed);

	// MCP draws that line deliberately: a transport error is for a malformed
	// call, and a tool that refused is something a model reads and reacts to.
	CHECK(failed);
}

TEST_CASE("a later row replaces an earlier one of the same name", "[control]") {
	Surface surface("test", "a suite");

	surface.Add(Tool{"thing", "first", nullptr, [](const json &, std::string &) { return json(1); }});
	REQUIRE(surface.Count() == 1);

	surface.Add(Tool{"thing", "second", nullptr, [](const json &, std::string &) { return json(2); }});
	CHECK(surface.Count() == 1);
	CHECK(surface.Registered().front().Description == "second");
	CHECK(Called(surface, "thing", json::object()) == 2);
}

TEST_CASE("a feature list installs only the named groups and keeps order", "[control]") {
	Surface surface("test", "a suite");

	const std::array features{
		Feature{
			"base",
			[](Surface &registry) {
				registry.Add(Tool{"thing", "shared feature", nullptr, [](const json &, std::string &) {
									  return 1;
								  }});
			},
		},
		engine::control::features::Custom("product", [](Surface &registry) {
			registry.Add(Tool{"thing", "product feature", nullptr, [](const json &, std::string &) {
								  return 2;
							  }});
		}),
	};

	surface.Enable(features);

	REQUIRE(surface.Count() == 1);
	CHECK(surface.Registered().front().Description == "product feature");
	CHECK(Called(surface, "thing", json::object()) == 2);
}

TEST_CASE("omitted engine features publish none of their rows", "[control]") {
	Surface surface("test", "a suite");
	const std::array features{engine::control::features::Architecture()};

	surface.Enable(features);

	bool architecture = false;
	for (const Tool &tool : surface.Registered()) {
		architecture = architecture || tool.Name == "module_get";
		CHECK(tool.Name != "world_list");
		CHECK(tool.Name != "class_list");
		CHECK(tool.Name != "test_run");
	}
	CHECK(architecture);
}

// --- the storage underneath ----------------------------------------------------

TEST_CASE("component_list names what a game declared and how many carry it", "[control]") {
	const std::string component = Unique("health");
	const FieldSpec fields[] = {
		{"Current", PropertyType::Double},
		{"Max", PropertyType::Double},
	};
	REQUIRE(Schemas::Register(component, fields).Why == Schemas::Status::Ok);

	Universe universe;
	const WorldId world = MakeWorld(universe, "control-components");

	const ComponentId id = Components::Find(Name(component));
	const Schema *schema = Schemas::Of(id);

	std::vector<std::byte> value(schema->Size());
	Components::Describe(id).DefaultConstruct(value.data(), 1);

	universe.Enter(world, [&](Store &store) {
		store.SetComponent(store.Create(), id, value.data());
		store.SetComponent(store.Create(), id, value.data());
	});
	Components::Describe(id).Destruct(value.data(), 1);

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	const json listed = Called(surface, "component_list", json::object());
	REQUIRE(listed.contains("components"));

	bool found = false;
	for (const json &entry : listed["components"]) {
		if (entry["name"] != component) {
			continue;
		}
		found = true;
		CHECK(entry["entities"] == 2);
		CHECK(entry["fields"]["Current"] == "double");
		CHECK(entry["fields"]["Max"] == "double");
	}
	CHECK(found);
}

TEST_CASE("entity_query answers with the entities carrying every named component", "[control]") {
	const std::string first = Unique("a");
	const std::string second = Unique("b");
	const FieldSpec fields[] = {{"Value", PropertyType::Float}};

	REQUIRE(Schemas::Register(first, fields).Why == Schemas::Status::Ok);
	REQUIRE(Schemas::Register(second, fields).Why == Schemas::Status::Ok);

	const ComponentId one = Components::Find(Name(first));
	const ComponentId two = Components::Find(Name(second));

	Universe universe;
	const WorldId world = MakeWorld(universe, "control-query");

	std::vector<std::byte> value(Schemas::Of(one)->Size());
	Components::Describe(one).DefaultConstruct(value.data(), 1);

	Entity both;
	universe.Enter(world, [&](Store &store) {
		both = store.Create();
		store.SetComponent(both, one, value.data());
		store.SetComponent(both, two, value.data());

		const Entity onlyOne = store.Create();
		store.SetComponent(onlyOne, one, value.data());
	});
	Components::Describe(one).Destruct(value.data(), 1);

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	const json wide = Called(surface, "entity_query", json{{"components", json::array({first})}});
	CHECK(wide["total"] == 2);
	CHECK(wide["entities"].size() == 2);

	const json narrow = Called(surface, "entity_query", json{{"components", json::array({first, second})}});
	CHECK(narrow["total"] == 1);
	REQUIRE(narrow["entities"].size() == 1);
	CHECK(narrow["entities"][0] == both.Id);

	// **A typo is refused rather than answered with nothing**, because an empty
	// result reads exactly like a world with nothing in it.
	bool failed = false;
	Called(surface, "entity_query", json{{"components", json::array({"nothing.declared"})}}, failed);
	CHECK(failed);

	failed = false;
	Called(surface, "entity_query", json::object(), failed);
	CHECK(failed);
}

TEST_CASE("entity_query says when it truncated", "[control]") {
	const std::string component = Unique("many");
	const FieldSpec fields[] = {{"Value", PropertyType::Float}};
	REQUIRE(Schemas::Register(component, fields).Why == Schemas::Status::Ok);

	const ComponentId id = Components::Find(Name(component));

	Universe universe;
	const WorldId world = MakeWorld(universe, "control-truncate");

	std::vector<std::byte> value(Schemas::Of(id)->Size());
	Components::Describe(id).DefaultConstruct(value.data(), 1);
	universe.Enter(world, [&](Store &store) {
		for (int index = 0; index < 5; index++) {
			store.SetComponent(store.Create(), id, value.data());
		}
	});
	Components::Describe(id).Destruct(value.data(), 1);

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	const json capped =
		Called(surface, "entity_query", json{{"components", json::array({component})}, {"limit", 2}});

	// A caller that sees no `truncated` is entitled to believe it got them all,
	// which is the same contract `world_tree` keeps.
	CHECK(capped["entities"].size() == 2);
	CHECK(capped["total"] == 5);
	CHECK(capped.value("truncated", false));
}

TEST_CASE("component_get and component_set read and write one entity's fields", "[control]") {
	const std::string component = Unique("stats");
	const FieldSpec fields[] = {
		{"Current", PropertyType::Double},
		{"Max", PropertyType::Double},
		{"Label", PropertyType::String},
		{"Where", PropertyType::Vector3},
	};
	REQUIRE(Schemas::Register(component, fields).Why == Schemas::Status::Ok);

	Universe universe;
	const WorldId world = MakeWorld(universe, "control-values");

	Entity entity;
	universe.Enter(world, [&](Store &store) { entity = store.Create(); });

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	// **Null rather than an empty object for a component nobody attached**, so
	// "not carried" and "carried and every field is zero" stay different
	// answers.
	CHECK(Called(surface, "component_get", json{{"id", entity.Id}, {"component", component}}).is_null());

	Called(
		surface,
		"component_set",
		json{
			{"id", entity.Id},
			{"component", component},
			{"fields",
			 json{
				 {"Current", 30.0},
				 {"Max", 100.0},
				 {"Label", "a score that changes"},
				 {"Where", json{{"X", 1.0}, {"Y", 2.0}, {"Z", 3.0}}},
			 }},
		}
	);

	const json held = Called(surface, "component_get", json{{"id", entity.Id}, {"component", component}});
	REQUIRE(held.contains("fields"));
	CHECK(held["fields"]["Current"] == 30.0);
	CHECK(held["fields"]["Max"] == 100.0);
	CHECK(held["fields"]["Label"] == "a score that changes");
	CHECK(held["fields"]["Where"]["Y"] == 2.0);

	// A field left out keeps what it had, which is what anybody writing a
	// partial update means.
	Called(
		surface,
		"component_set",
		json{{"id", entity.Id}, {"component", component}, {"fields", json{{"Current", 12.0}}}}
	);

	const json after = Called(surface, "component_get", json{{"id", entity.Id}, {"component", component}});
	CHECK(after["fields"]["Current"] == 12.0);
	CHECK(after["fields"]["Max"] == 100.0);
	CHECK(after["fields"]["Label"] == "a score that changes");
}

TEST_CASE("a component the engine declares is refused with a reason", "[control]") {
	Universe universe;
	const WorldId world = MakeWorld(universe, "control-refuse");

	Entity entity;
	universe.Enter(world, [&](Store &store) { entity = store.Create(); });

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe);

	// `ecs.Hierarchy` is the storage's own and has no field list at run time.
	// "There is no such component" would send a reader looking for a typo,
	// where the answer is "that one is reached through its properties".
	bool failed = false;
	Called(surface, "component_get", json{{"id", entity.Id}, {"component", "ecs.Hierarchy"}}, failed);
	CHECK(failed);

	failed = false;
	Called(surface, "component_get", json{{"id", entity.Id}, {"component", "no.such.thing"}}, failed);
	CHECK(failed);

	// And an unknown field is refused rather than dropped.
	const std::string component = Unique("strict");
	const FieldSpec fields[] = {{"A", PropertyType::Float}};
	REQUIRE(Schemas::Register(component, fields).Why == Schemas::Status::Ok);

	failed = false;
	Called(
		surface,
		"component_set",
		json{{"id", entity.Id}, {"component", component}, {"fields", json{{"B", 1.0}}}},
		failed
	);
	CHECK(failed);
}

TEST_CASE("a read-only surface offers neither write tool", "[control]") {
	Universe universe;
	MakeWorld(universe, "control-readonly");

	Surface surface("test", "a suite");
	surface.AddUniverseTools(universe, false);

	// A tool that always fails is worse than one that was never listed, which
	// is the rule `instance_set` already followed and the storage pair now
	// follows too.
	for (const Tool &tool : surface.Registered()) {
		CHECK(tool.Name != "instance_set");
		CHECK(tool.Name != "component_set");
	}

	// The read halves are still there.
	bool listed = false;
	for (const Tool &tool : surface.Registered()) {
		listed = listed || tool.Name == "component_list";
	}
	CHECK(listed);
}
