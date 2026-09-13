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
#include <engine/control/features/DataCapture.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/core/Version.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
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
using engine::ecs::FieldPacking;
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

	const json *Named(const json &values, const char *name) {
		for (const json &value : values) {
			if (value.value("name", "") == name) {
				return &value;
			}
		}
		return nullptr;
	}

	class FakeCapture final : public engine::script::DataCaptureBridge {
	  public:
		engine::script::DataCaptureBridgeCapabilities Capabilities() const override {
			return {.Available = true, .Channels = {"rgb_linear_hdr"}, .Detail = "ready"};
		}
		bool Queue(
			std::string_view instance,
			const engine::script::DataCaptureBridgeRequest &request,
			uint64_t &ticket,
			std::string &
		) override {
			Instance = instance;
			Snapshot = request.SnapshotId;
			ticket = 1;
			Queued = true;
			return true;
		}
		bool Poll(
			std::string_view instance,
			uint64_t ticket,
			engine::script::DataCaptureBridgePoll &poll,
			std::string &detail
		) override {
			if (!Queued || instance != Instance || ticket != 1) {
				detail = "unknown ticket";
				return false;
			}
			poll.Status = "ready";
			poll.SnapshotId = Snapshot;
			poll.HasCamera = true;
			poll.CropLeft = 0.125;
			poll.CropTop = 0.25;
			poll.CropWidth = 0.5;
			poll.CropHeight = 0.75;
			poll.Planes = {
				{.Channel = "rgb_linear_hdr",
				 .Status = "ready",
				 .Resource = "capture/1/rgb_linear_hdr",
				 .SourceResource = "colour",
				 .HashAlgorithm = "blake3-256",
				 .Hash = "abcd",
				 .Width = 2,
				 .Height = 1,
				 .RowStride = 6,
				 .Scalar = "float16",
				 .ColourSpace = "linear",
				 .Origin = "top_left",
				 .Packing = {}}
			};
			return true;
		}
		bool ReadPlane(
			std::string_view instance,
			uint64_t ticket,
			std::string_view resource,
			size_t offset,
			size_t maximum,
			std::vector<std::byte> &bytes,
			std::string &detail
		) override {
			if (!Queued || instance != Instance || ticket != 1 || resource != "capture/1/rgb_linear_hdr" ||
				offset > 3) {
				detail = "unknown resource";
				return false;
			}
			const std::array raw{std::byte{1}, std::byte{2}, std::byte{3}};
			const size_t count = std::min(maximum, raw.size() - offset);
			bytes.assign(
				raw.begin() + static_cast<ptrdiff_t>(offset),
				raw.begin() + static_cast<ptrdiff_t>(offset + count)
			);
			return true;
		}
		bool Release(std::string_view instance, uint64_t ticket, std::string &detail) override {
			if (!Queued || instance != Instance || ticket != 1) {
				detail = "unknown ticket";
				return false;
			}
			Queued = false;
			return true;
		}
		void Cancel(std::string_view, uint64_t) override {}

	  private:
		std::string Instance;
		std::string Snapshot;
		bool Queued = false;
	};
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

TEST_CASE("a refused tool retains its structured recovery payload", "[control]") {
	Surface surface("test", "a suite");
	surface.Add(
		Tool{
			"guarded_write",
			"Refuses a stale write with the current versions.",
			nullptr,
			[](const json &, std::string &failure) {
				failure = "version_conflict";
				return json{{"status", "version_conflict"}, {"current_versions", json{{"scene", 7}}}};
			},
		}
	);

	bool failed = false;
	const json reply = Called(surface, "guarded_write", json::object(), failed);
	CHECK(failed);
	CHECK(reply["error"] == "version_conflict");
	CHECK(reply["status"] == "version_conflict");
	CHECK(reply["current_versions"]["scene"] == 7);
}

TEST_CASE("discovery reads each surface's installed capture readiness", "[control][discovery]") {
	Surface supported("supported", "a suite");
	supported.SetDataCaptureAvailabilityProvider([] {
		return engine::control::DataCaptureAvailability{
			.Available = true,
			.Channels = {"rgb_linear_hdr", "linear_depth"},
			.Detail = "renderer readback is ready",
		};
	});
	supported.Enable(std::array{engine::control::features::Discovery()});

	const json available =
		Called(supported, "negotiate", json{{"requested_channels", {"rgb_linear_hdr", "shading_normal"}}});
	CHECK(available["requested_channels"][0]["supported"]);
	CHECK_FALSE(available["requested_channels"][1]["supported"]);
	CHECK(available["offscreen_gpu"]["supported"]);

	Surface unavailable("unavailable", "a suite");
	unavailable.SetDataCaptureAvailabilityProvider([] {
		return engine::control::DataCaptureAvailability{
			.Available = false,
			.Channels = {},
			.Detail = "renderer is not ready",
		};
	});
	unavailable.Enable(std::array{engine::control::features::Discovery()});
	const json absent = Called(unavailable, "negotiate", json{{"requested_channels", {"rgb_linear_hdr"}}});
	CHECK_FALSE(absent["requested_channels"][0]["supported"]);
	CHECK(absent["requested_channels"][0]["reason"] == "renderer is not ready");
	CHECK_FALSE(absent["offscreen_gpu"]["supported"]);
}

TEST_CASE("capture tools retain metadata and return bounded base64 resources", "[control][data-capture]") {
	Universe universe;
	MakeWorld(universe, "capture-world");
	engine::world::DataFactorySession session(universe);
	auto bridge = std::make_shared<FakeCapture>();
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::DataCapture(session, bridge)});
	const auto current = session.Inspect("capture-world");
	const json capture = Called(
		surface,
		"capture",
		json{
			{"instance_id", "capture-world"},
			{"snapshot_id", "snapshot-1"},
			{"pipeline", "default_pbr"},
			{"capture_node", "capture"},
			{"view_slot", 0},
			{"channels", {"rgb_linear_hdr"}},
			{"temporal_history", "preserve"},
			{"operation_id", "capture-1"},
			{"expected_tick", current.Clock.Tick},
			{"expected_world_epoch", current.WorldEpoch},
			{"expected_world_version", current.WorldVersion}
		}
	);
	CHECK(capture["status"] == "queued");
	const json poll = Called(surface, "poll_capture", json{{"instance_id", "capture-world"}, {"ticket", 1}});
	CHECK(poll["planes"][0]["digest"] == "abcd");
	CHECK(poll["planes"][0]["hash_algorithm"] == "blake3-256");
	CHECK(poll["planes"][0]["shape"] == json::array({1, 2, 4}));
	CHECK(poll["planes"][0]["snapshot_id"] == "snapshot-1");
	CHECK(poll["planes"][0]["dtype"] == "float16");
	CHECK(poll["camera"]["crop"] == json::array({0.125, 0.25, 0.5, 0.75}));
	CHECK(poll["camera"]["crop_convention"] == "normalized_full_view_left_top_width_height");
	CHECK_FALSE(poll["camera"]["lens_distortion_available"]);
	CHECK(poll["camera"]["lens_distortion_reason"] == "unavailable");
	CHECK_FALSE(poll["camera"]["jitter_available"]);
	CHECK(poll["camera"]["jitter_policy"] == "unavailable");
	const json bytes = Called(
		surface,
		"get_resource",
		json{
			{"id", "capture/1/rgb_linear_hdr"},
			{"options", {{"instance_id", "capture-world"}, {"ticket", 1}, {"offset", 0}, {"max_bytes", 3}}}
		}
	);
	CHECK(bytes["encoding"] == "base64");
	CHECK(bytes["data"] == "AQID");
	CHECK(
		Called(
			surface,
			"cancel_capture",
			json{{"instance_id", "capture-world"}, {"ticket", 1}, {"operation_id", "shared-action"}}
		)["status"] == "cancel_requested"
	);
	bool failed = false;
	const json conflict = Called(
		surface,
		"release_capture",
		json{{"instance_id", "capture-world"}, {"ticket", 1}, {"operation_id", "shared-action"}},
		failed
	);
	CHECK(failed);
	CHECK(conflict["error"].get<std::string>().starts_with("operation_id_conflict:"));
	CHECK(
		Called(
			surface,
			"release_capture",
			json{{"instance_id", "capture-world"}, {"ticket", 1}, {"operation_id", "release-1"}}
		)["status"] == "released"
	);
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

TEST_CASE("discovery reports the final callable registry and schema", "[control][discovery]") {
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::Discovery()});
	size_t calls = 0;

	// A product row can arrive after the feature list. Discovery reads the
	// registry at call time, so it describes that row instead of a snapshot from
	// installation before the product finished registering its vocabulary.
	surface.Add(
		Tool{
			"host_probe",
			"A host-specific probe.",
			[] {
				return json{{"type", "object"}, {"properties", json{{"label", json{{"type", "string"}}}}}};
			},
			[&calls](const json &, std::string &) {
				calls++;
				return json::object();
			},
		}
	);
	surface.Add(
		Tool{
			"capture",
			"An unrelated host tool named capture.",
			nullptr,
			[&calls](const json &, std::string &) {
				calls++;
				return json::object();
			},
		}
	);

	const json result = Called(surface, "negotiate", json::object());
	CHECK(result["engine_version"] == std::string(engine::core::Version()));
	const json *negotiate = Named(result["operations"], "negotiate");
	REQUIRE(negotiate != nullptr);
	CHECK((*negotiate)["input_schema"]["type"] == "object");
	const json *probe = Named(result["operations"], "host_probe");
	REQUIRE(probe != nullptr);
	CHECK((*probe)["input_schema"]["properties"]["label"]["type"] == "string");
	const json *capture = Named(result["operations"], "capture");
	REQUIRE(capture != nullptr);
	CHECK((*capture)["input_schema"]["type"] == "object");
	CHECK(Named(result["unsupported_operations"], "capture") == nullptr);
	CHECK(Called(surface, "negotiate", json::object()) == result);
	CHECK(calls == 0);
}

TEST_CASE("discovery refuses unknown versions and oversized requests", "[control][discovery]") {
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::Discovery()});

	bool failed = false;
	const json version = Called(surface, "negotiate", json{{"contract_version", "unknown"}}, failed);
	CHECK(failed);
	CHECK(version["error"].get<std::string>().starts_with("capability_unsupported:"));

	std::vector<std::string> channels(65, "rgb");
	const json oversized = Called(surface, "negotiate", json{{"requested_channels", channels}}, failed);
	CHECK(failed);
	CHECK(oversized["error"].get<std::string>().find("64-channel") != std::string::npos);

	const json malformed = Called(surface, "negotiate", json::array(), failed);
	CHECK(failed);
	CHECK(malformed["error"] == "negotiate arguments must be an object");

	const std::array<std::pair<json, std::string_view>, 6> invalid{
		std::pair{json{{"schema_version", "unknown"}}, "schema_version"},
		std::pair{json{{"requested_channels", "rgb"}}, "must be an array"},
		std::pair{json{{"requested_channels", json::array({1})}}, "lowercase ASCII"},
		std::pair{json{{"requested_channels", json::array({""})}}, "lowercase ASCII"},
		std::pair{json{{"requested_channels", json::array({"RGB"})}}, "lowercase ASCII"},
		std::pair{json{{"requested_channels", json::array({std::string(129, 'a')})}}, "lowercase ASCII"},
	};
	for (const auto &[arguments, expected] : invalid) {
		const json refused = Called(surface, "negotiate", arguments, failed);
		CHECK(failed);
		CHECK(refused["error"].get<std::string>().find(expected) != std::string::npos);
	}
}

TEST_CASE(
	"discovery names unavailable channels and limits instead of promising them", "[control][discovery]"
) {
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::Discovery()});

	const json result =
		Called(surface, "negotiate", json{{"requested_channels", json::array({"rgb", "made_up"})}});
	REQUIRE(result["requested_channels"].size() == 2);
	CHECK_FALSE(result["requested_channels"][0]["supported"]);
	CHECK(result["requested_channels"][0]["reason"] == "capture channels are not implemented by this host");
	CHECK_FALSE(result["requested_channels"][1]["supported"]);
	CHECK(result["requested_channels"][1]["reason"] == "capture channels are not implemented by this host");

	const json *channels = Named(result["limits"], "channels");
	REQUIRE(channels != nullptr);
	CHECK_FALSE((*channels)["supported"]);
	CHECK((*channels)["reason"] == "this host does not implement the data-factory operation");
	CHECK_FALSE(result["offscreen_gpu"]["supported"]);
	CHECK_FALSE(result["headless_cpu"]["supported"]);
}

TEST_CASE("discovery accepts its exact requested-channel bounds", "[control][discovery]") {
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::Discovery()});

	const std::vector<std::string> channels(64, std::string(128, 'a'));
	const json result = Called(surface, "negotiate", json{{"requested_channels", channels}});
	CHECK(result["requested_channels"].size() == 64);
	CHECK(result["requested_channels"].front()["name"] == channels.front());
}

// --- the storage underneath ----------------------------------------------------

TEST_CASE("component_list names what a game declared and how many carry it", "[control]") {
	const std::string component = Unique("health");
	const FieldSpec fields[] = {
		{"Current", PropertyType::Double},
		{"Max", PropertyType::Double},
		{"Small", PropertyType::Int32, {}, FieldPacking::Int4},
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
		CHECK(entry["fields"]["Small"] == "int4");
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
		{"Small", PropertyType::Int32, {}, FieldPacking::Int4},
		{"Unit", PropertyType::Float, {}, FieldPacking::UFloat8},
		{"Flag", PropertyType::Bool},
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
				 {"Small", 20},
				 {"Unit", 2.0},
				 {"Flag", true},
			 }},
		}
	);

	const json held = Called(surface, "component_get", json{{"id", entity.Id}, {"component", component}});
	REQUIRE(held.contains("fields"));
	CHECK(held["fields"]["Current"] == 30.0);
	CHECK(held["fields"]["Max"] == 100.0);
	CHECK(held["fields"]["Label"] == "a score that changes");
	CHECK(held["fields"]["Where"]["Y"] == 2.0);
	CHECK(held["fields"]["Small"] == 7);
	CHECK(held["fields"]["Unit"] == 1.0);
	CHECK(held["fields"]["Flag"] == true);

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
	CHECK(after["fields"]["Small"] == 7);
	CHECK(after["fields"]["Flag"] == true);
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

TEST_CASE(
	"data-factory lifecycle tools validate and deduplicate through the surface", "[control][data-factory]"
) {
	Universe universe;
	const WorldId world = MakeWorld(universe, "control-data-factory");
	engine::world::DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });

	Surface surface("test", "a suite");
	surface.Enable(
		std::array{engine::control::features::Discovery(), engine::control::features::DataFactory(session)}
	);
	const json negotiated = Called(surface, "negotiate", json::object());
	for (const json &unavailable : negotiated["unsupported_operations"]) {
		CHECK(unavailable["name"] != "pause");
		CHECK(unavailable["name"] != "resume");
		CHECK(unavailable["name"] != "step");
		CHECK(unavailable["name"] != "snapshot");
		CHECK(unavailable["name"] != "checkpoint");
		CHECK(unavailable["name"] != "restore");
	}
	const json inspected =
		Called(surface, "lifecycle_inspect", json{{"instance_id", "control-data-factory"}});
	CHECK(inspected["tick"] == 0);
	CHECK(inspected["world_epoch"] == 1);
	CHECK(inspected["world_version"] == 0);
	const auto before = session.Inspect("control-data-factory");
	const json base{
		{"instance_id", "control-data-factory"},
		{"expected_tick", before.Clock.Tick},
		{"expected_world_epoch", before.WorldEpoch},
		{"expected_world_version", before.WorldVersion}
	};

	// Every malformed value is refused before the pause participant or world can
	// change. Negative, floating, and boolean JSON values must not coerce to an
	// unsigned expectation.
	json negative = base;
	negative["expected_tick"] = -1;
	bool failed = false;
	Called(surface, "pause", negative, failed);
	CHECK(failed);
	json floating = base;
	floating["expected_world_version"] = 1.0;
	failed = false;
	Called(surface, "pause", floating, failed);
	CHECK(failed);
	json boolean = base;
	boolean["expected_world_epoch"] = true;
	failed = false;
	Called(surface, "pause", boolean, failed);
	CHECK(failed);
	json unknown = base;
	unknown["scope"] = "physics";
	failed = false;
	Called(surface, "pause", unknown, failed);
	CHECK(failed);
	json nul = base;
	nul["operation_id"] = std::string("nul\0id", 6);
	failed = false;
	Called(surface, "pause", nul, failed);
	CHECK(failed);
	CHECK(session.Inspect("control-data-factory").WorldVersion == before.WorldVersion);

	json pause = base;
	pause["operation_id"] = "pause-1";
	const json paused = Called(surface, "pause", pause);
	CHECK(paused["world_version"] == 1);

	json step{
		{"instance_id", "control-data-factory"},
		{"expected_tick", 0u},
		{"expected_world_epoch", 1u},
		{"expected_world_version", 1u},
		{"operation_id", "step-1"},
		{"dt_ns", {{"numerator", 1'000'000'000u}, {"denominator", 60u}}},
		{"actions", json::array()}
	};
	const json stepped = Called(surface, "step", step);
	CHECK(stepped["tick"] == 1);
	CHECK(stepped["world_version"] == 2);
	const json replay = Called(surface, "step", step);
	CHECK(replay == stepped);
	CHECK(universe.StatisticsOf(world).Ticks == 1);

	json conflict = step;
	conflict["expected_tick"] = 1u;
	failed = false;
	Called(surface, "step", conflict, failed);
	CHECK(failed);
	json action = step;
	action["operation_id"] = "step-actions";
	action["actions"] = json::array({"jump"});
	failed = false;
	Called(surface, "step", action, failed);
	CHECK(failed);

	const json current{
		{"instance_id", "control-data-factory"},
		{"expected_tick", 1u},
		{"expected_world_epoch", 1u},
		{"expected_world_version", 2u}
	};
	json snapshotArguments = current;
	snapshotArguments["operation_id"] = "capture-1";
	const json snapshot = Called(surface, "snapshot", snapshotArguments);
	CHECK(snapshot.contains("snapshot_id"));
	json checkpoint = current;
	checkpoint["operation_id"] = "capture-1";
	failed = false;
	Called(surface, "checkpoint", checkpoint, failed);
	CHECK(failed);
	checkpoint["operation_id"] = "checkpoint-1";
	const json saved = Called(surface, "checkpoint", checkpoint);
	REQUIRE(saved.contains("checkpoint_id"));
	json restore = current;
	restore["operation_id"] = "restore-1";
	restore["checkpoint_id"] = saved["checkpoint_id"];
	const json restored = Called(surface, "restore", restore);
	CHECK(restored["world_epoch"] == 2);
	const json resumed = Called(
		surface,
		"resume",
		json{
			{"instance_id", "control-data-factory"},
			{"expected_tick", restored["tick"]},
			{"expected_world_epoch", restored["world_epoch"]},
			{"expected_world_version", restored["world_version"]}
		}
	);
	CHECK(resumed["status"] == "ok");

	json missing = base;
	missing["instance_id"] = "no-such-world";
	failed = false;
	Called(surface, "pause", missing, failed);
	CHECK(failed);
}

TEST_CASE("data-factory intervention is guarded, typed, and idempotent", "[control][data-factory]") {
	Universe universe;
	MakeWorld(universe, "control-intervention");
	engine::world::DataFactorySession session(universe);
	session.SetPauseParticipant([](WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) {
		return true;
	});
	session.SetRehydrate([](Universe &, WorldId, std::string &) { return true; });
	std::vector<engine::world::DataFactoryIntervention> received;
	session.SetInterventionExecutor([&](Universe &,
										WorldId,
										std::span<const engine::world::DataFactoryIntervention> edits,
										std::string &) {
		received.assign(edits.begin(), edits.end());
		return true;
	});
	Surface surface("test", "a suite");
	surface.Enable(std::array{engine::control::features::DataFactory(session)});
	const auto current = session.Inspect("control-intervention");
	Called(
		surface,
		"pause",
		json{
			{"instance_id", "control-intervention"},
			{"expected_tick", current.Clock.Tick},
			{"expected_world_epoch", current.WorldEpoch},
			{"expected_world_version", current.WorldVersion}
		}
	);
	const auto paused = session.Inspect("control-intervention");
	const json snapshot = Called(
		surface,
		"snapshot",
		json{
			{"instance_id", "control-intervention"},
			{"expected_tick", paused.Clock.Tick},
			{"expected_world_epoch", paused.WorldEpoch},
			{"expected_world_version", paused.WorldVersion}
		}
	);
	const json request{
		{"instance_id", "control-intervention"},
		{"expected_tick", paused.Clock.Tick},
		{"expected_world_epoch", paused.WorldEpoch},
		{"expected_world_version", paused.WorldVersion},
		{"operation_id", "edit-1"},
		{"base_snapshot_id", snapshot["snapshot_id"]},
		{"changed_causes",
		 json::array(
			 {{{"target_id", "fixture/part"},
			   {"path", "attributes.Label"},
			   {"expected", "old"},
			   {"value", "new"}}}
		 )}
	};
	const json applied = Called(surface, "apply_intervention", request);
	CHECK(applied["world_version"] == paused.WorldVersion + 1);
	REQUIRE(received.size() == 1);
	CHECK(received[0].TargetId == "fixture/part");
	CHECK(received[0].Expected.Type == engine::world::DataFactoryInterventionValue::Kind::String);
	CHECK(received[0].Value.String == "new");
	CHECK(Called(surface, "apply_intervention", request) == applied);
}
