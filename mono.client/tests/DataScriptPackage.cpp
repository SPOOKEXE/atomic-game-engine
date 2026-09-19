#include <engine/assets/ContentHash.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/DataFactory.hpp>
#include <engine/core/Name.hpp>
#include <engine/script/DataScriptPackage.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/DataFactory.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/DataScriptPackage.hpp>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <utility>

TEST_SUITE_ID("client.data-script-package")

namespace {
	using nlohmann::json;

	std::string SourceHash(std::string_view source) {
		return engine::assets::Hasher::Of(std::as_bytes(std::span(source.data(), source.size()))).ToHex();
	}

	const engine::control::Tool &PackageTool(engine::control::Surface &surface) {
		for (const engine::control::Tool &tool : surface.Registered())
			if (tool.Name == "run_script_package") return tool;
		throw std::runtime_error("package tool was not installed");
	}

	std::string Manifest(std::string_view source, uint64_t sourceBytes, uint64_t assetBytes = 0);

	json Request(std::string operation = "operation-1") {
		return {
			{"instance_id", "client.world"},
			{"expected_tick", 7},
			{"expected_world_epoch", 2},
			{"expected_world_version", 4},
			{"operation_id", std::move(operation)},
			{"language", "luau"},
			{"manifest", Manifest("return", 1024)},
			{"source_base64", "cmV0dXJu"},
			{"assets", json::array()},
		};
	}

	std::string Manifest(std::string_view source, uint64_t sourceBytes, uint64_t assetBytes) {
		const std::string hash = SourceHash(source);
		return json{
			{"format", "atomic.data-script.v1"},
			{"entry", "package.luau"},
			{"source_hash", hash},
			{"assets", json::array()},
			{"parameters", json::array()},
			{"capabilities", json::array()},
			{"budget",
			 json{
				 {"source_bytes", sourceBytes}, {"asset_bytes", assetBytes}, {"assets", 0}, {"parameters", 0}
			 }},
			{"seed", 0},
		}
			.dump();
	}

	json SurfaceReply(engine::control::Surface &surface, const json &arguments, bool &isError) {
		const json envelope = json::parse(surface.Answer(
			json{
				{"jsonrpc", "2.0"},
				{"id", 1},
				{"method", "tools/call"},
				{"params", {{"name", "run_script_package"}, {"arguments", arguments}}}
			}.dump()
		));
		isError = envelope["result"].value("isError", true);
		return json::parse(envelope["result"]["content"][0]["text"].get<std::string>());
	}
}

TEST_CASE(
	"client package tool returns terminal lifecycle details and replays an operation", "[client][mcp]"
) {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &request) {
		calls++;
		engine::script::DataScriptResult result;
		result.Ran = true;
		result.Atomic = true;
		result.Lifecycle.InstanceId = request.InstanceId;
		result.Lifecycle.Clock.Tick = request.ExpectedTick;
		result.Lifecycle.WorldEpoch = request.ExpectedEpoch;
		result.Lifecycle.WorldVersion = request.ExpectedVersion + 1;
		return result;
	});

	std::string failure;
	const json first = PackageTool(surface).Call(Request(), failure);
	REQUIRE(failure.empty());
	CHECK(first["terminal"] == "completed");
	CHECK(first["atomic"] == true);
	CHECK(first["language"] == "luau");
	CHECK(first["world_version"] == 5);
	const json replay = PackageTool(surface).Call(Request(), failure);
	CHECK(failure.empty());
	CHECK(replay == first);
	CHECK(calls == 1);

	json conflict = Request();
	conflict["expected_world_version"] = 5;
	CHECK(PackageTool(surface).Call(conflict, failure).is_null());
	CHECK(failure.starts_with("operation_id_conflict:"));
}

TEST_CASE("client package tool shares lifecycle and capture operation ids", "[client][mcp]") {
	engine::world::Universe universe;
	engine::world::WorldSettings settings;
	settings.Name = engine::core::Name("script-package-policy");
	universe.Create(settings);
	engine::world::DataFactorySession session(universe);
	session.SetPauseParticipant(
		[](engine::world::WorldId, engine::world::DataFactoryPauseScope, bool, std::string &) { return true; }
	);
	engine::control::Surface surface("test", "test");
	surface.Enable(std::array{engine::control::features::DataFactory(session)});
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &request) {
		calls++;
		engine::script::DataScriptResult result;
		result.Ran = true;
		result.Atomic = true;
		result.Lifecycle.InstanceId = request.InstanceId;
		result.Lifecycle.Clock.Tick = request.ExpectedTick;
		result.Lifecycle.WorldEpoch = request.ExpectedEpoch;
		result.Lifecycle.WorldVersion = request.ExpectedVersion + 1;
		return result;
	});

	auto lifecycle = session.Inspect("script-package-policy");
	std::string failure;
	bool paused = false;
	for (const engine::control::Tool &tool : surface.Registered()) {
		if (tool.Name != "pause") continue;
		const json reply = tool.Call(
			json{
				{"instance_id", "script-package-policy"},
				{"expected_tick", lifecycle.Clock.Tick},
				{"expected_world_epoch", lifecycle.WorldEpoch},
				{"expected_world_version", lifecycle.WorldVersion},
				{"operation_id", "pause-id"}
			},
			failure
		);
		REQUIRE(reply["status"] == "ok");
		paused = true;
		break;
	}
	REQUIRE(paused);
	CHECK(failure.empty());
	CHECK(PackageTool(surface).Call(Request("pause-id"), failure).is_null());
	CHECK(failure.starts_with("operation_id_conflict:"));
	CHECK(calls == 0);

	surface.DataFactoryOperations()->Store(
		"capture", "capture-id", "capture", json{{"status", "queued"}}, ""
	);
	CHECK(PackageTool(surface).Call(Request("capture-id"), failure).is_null());
	CHECK(failure.starts_with("operation_id_conflict:"));
	CHECK(calls == 0);

	CHECK(PackageTool(surface).Call(Request("script-id"), failure)["status"] == "completed");
	CHECK(failure.empty());
	CHECK(calls == 1);
	bool audited = false;
	failure.clear();
	for (const engine::control::Tool &tool : surface.Registered()) {
		if (tool.Name != "data_factory_operation_audit") continue;
		const json audit = tool.Call(json{{"limit", 1u}}, failure);
		REQUIRE(failure.empty());
		CHECK(audit["entries"][0]["tool"] == "run_script_package");
		CHECK(audit["entries"][0]["operation_id"] == "script-id");
		audited = true;
		break;
	}
	REQUIRE(audited);
}

TEST_CASE("client package tool bounds hostile bytes and declares Luau only", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	client::AddDataScriptPackageTool(surface, [](const engine::script::DataScriptRequest &) {
		return engine::script::DataScriptResult{};
	});
	std::string failure;
	json unsupported = Request();
	unsupported["language"] = "javascript";
	CHECK(PackageTool(surface).Call(unsupported, failure).is_null());
	CHECK(failure.starts_with("capability_unsupported:"));

	json invalidByte = Request("operation-2");
	invalidByte["assets"] = json::array({{{"path", "asset.bin"}, {"base64", "?"}}});
	CHECK(PackageTool(surface).Call(invalidByte, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));

	json invalidManifest = Request("operation-3");
	invalidManifest["manifest"] = "{";
	CHECK(PackageTool(surface).Call(invalidManifest, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));
}

TEST_CASE("client package tool preserves unsigned lifecycle values and canonical base64", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &) {
		calls++;
		engine::script::DataScriptResult result;
		result.Ran = true;
		return result;
	});

	std::string failure;
	json exact = Request("uint64");
	exact["expected_tick"] = UINT64_MAX;
	CHECK(PackageTool(surface).Call(exact, failure)["ran"] == true);
	CHECK(failure.empty());

	json noncanonical = Request("noncanonical");
	noncanonical["source_base64"] = "Zh==";
	CHECK(PackageTool(surface).Call(noncanonical, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));
	CHECK(calls == 1);
}

TEST_CASE("client package tool applies manifest byte budgets before execution", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &) {
		calls++;
		return engine::script::DataScriptResult{};
	});

	std::string failure;
	json source = Request("source-budget");
	source["manifest"] = Manifest("return", 5);
	CHECK(PackageTool(surface).Call(source, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));

	json asset = Request("asset-budget");
	asset["manifest"] = Manifest("return", 6, 1);
	asset["assets"] = json::array({{{"path", "asset.bin"}, {"base64", "YWI="}}});
	CHECK(PackageTool(surface).Call(asset, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));
	CHECK(calls == 0);
}

TEST_CASE("client package tool fences new operation ids when its ledger is full", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &) {
		calls++;
		engine::script::DataScriptResult result;
		result.Ran = true;
		result.Atomic = true;
		result.Lifecycle.InstanceId = "client.world";
		result.Lifecycle.Clock.Tick = 7;
		result.Lifecycle.WorldEpoch = 2;
		result.Lifecycle.WorldVersion = 5;
		return result;
	});

	std::string failure;
	const json completed = PackageTool(surface).Call(Request("fixture"), failure);
	CHECK(failure.empty());
	CHECK(
		completed == json{
						 {"schema_version", "atomic.data-script.v1"},
						 {"status", "completed"},
						 {"terminal", "completed"},
						 {"ran", true},
						 {"atomic", true},
						 {"language", "luau"},
						 {"error", ""},
						 {"instance_id", "client.world"},
						 {"tick", 7},
						 {"world_epoch", 2},
						 {"world_version", 5},
						 {"source_hash", SourceHash("return")}
					 }
	);

	for (size_t index = 0; index < engine::control::DataFactoryOperationLedger::MAXIMUM_ENTRIES - 1;
		 index++) {
		const json reply = PackageTool(surface).Call(Request("evict-" + std::to_string(index)), failure);
		REQUIRE(failure.empty());
		REQUIRE(reply["terminal"] == "completed");
	}
	CHECK(PackageTool(surface).Call(Request("evict-new"), failure).is_null());
	CHECK(failure == "operation_id_capacity: operation ledger is full; retry an existing operation_id");
	const json replay = PackageTool(surface).Call(Request("fixture"), failure);
	CHECK(failure.empty());
	CHECK(replay["terminal"] == "completed");
	CHECK(calls == engine::control::DataFactoryOperationLedger::MAXIMUM_ENTRIES);
}

TEST_CASE("client package tool returns domain failures as strict value replies", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &) {
		calls++;
		engine::script::DataScriptResult result;
		result.Error = "active_script_runtime_unsupported";
		return result;
	});

	std::string failure;
	const json first = PackageTool(surface).Call(Request("refused"), failure);
	CHECK(failure.empty());
	const json replay = PackageTool(surface).Call(Request("refused"), failure);
	CHECK(first == replay);
	CHECK(failure.empty());
	CHECK(first["terminal"] == "failed");
	CHECK(first["error"] == "active_script_runtime_unsupported");
	CHECK(first["instance_id"] == "client.world");
	CHECK(first["source_hash"] == SourceHash("return"));
	CHECK(calls == 1);
}

TEST_CASE("client package tool keeps failed package replies out of MCP errors", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	unsigned calls = 0;
	client::AddDataScriptPackageTool(surface, [&calls](const engine::script::DataScriptRequest &) {
		calls++;
		engine::script::DataScriptResult result;
		result.Error = "active_script_runtime_unsupported";
		return result;
	});

	bool isError = true;
	const json first = SurfaceReply(surface, Request("surface-failed"), isError);
	CHECK_FALSE(isError);
	CHECK(
		first == json{
					 {"schema_version", "atomic.data-script.v1"},
					 {"status", "failed"},
					 {"terminal", "failed"},
					 {"ran", false},
					 {"atomic", false},
					 {"language", "luau"},
					 {"error", "active_script_runtime_unsupported"},
					 {"instance_id", "client.world"},
					 {"tick", 0},
					 {"world_epoch", 0},
					 {"world_version", 0},
					 {"source_hash", SourceHash("return")}
				 }
	);
	const json replay = SurfaceReply(surface, Request("surface-failed"), isError);
	CHECK_FALSE(isError);
	CHECK(replay == first);
	CHECK(calls == 1);
}

TEST_CASE("client package tool returns caught executor exceptions as value replies", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	client::AddDataScriptPackageTool(
		surface, [](const engine::script::DataScriptRequest &) -> engine::script::DataScriptResult {
			throw std::runtime_error("deliberate executor exception");
		}
	);

	bool isError = true;
	const json reply = SurfaceReply(surface, Request("surface-exception"), isError);
	CHECK_FALSE(isError);
	CHECK(reply["terminal"] == "failed");
	CHECK(reply["error"] == "package execution failed: deliberate executor exception");
	CHECK(reply["instance_id"] == "client.world");
	CHECK(reply["source_hash"] == SourceHash("return"));
}

TEST_CASE("client package tool keeps exact final base64 bytes and bounds asset rows", "[client][mcp]") {
	engine::control::Surface surface("test", "test");
	std::string source;
	client::AddDataScriptPackageTool(surface, [&source](const engine::script::DataScriptRequest &request) {
		source = request.Source;
		engine::script::DataScriptResult result;
		result.Ran = true;
		return result;
	});

	std::string failure;
	json exact = Request("last-byte");
	exact["source_base64"] = "YQ==";
	exact["manifest"] = Manifest("a", 1024);
	CHECK(PackageTool(surface).Call(exact, failure)["ran"] == true);
	CHECK(failure.empty());
	CHECK(source == "a");

	json tooMany = Request("too-many-assets");
	tooMany["assets"] = json::array();
	for (unsigned index = 0; index < 129; index++) {
		tooMany["assets"].push_back({{"path", "asset-" + std::to_string(index)}, {"base64", "YQ=="}});
	}
	CHECK(PackageTool(surface).Call(tooMany, failure).is_null());
	CHECK(failure.starts_with("validation_failed:"));
}
