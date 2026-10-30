// Product-level MCP contracts, observed through the actual server binary.
// The probes request one cube because the benchmark field's population is
// unrelated to the listener contract and delays its bind.

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("server.mcpcontract")

namespace {
	using nlohmann::json;

	std::filesystem::path Program() {
		return engine::core::Paths::Base().parent_path() / "server" / engine::core::Paths::Program("server");
	}

	uint16_t FreePort() {
		asio::io_context context;
		asio::ip::tcp::acceptor probe(context, {asio::ip::address_v4::loopback(), 0});
		return probe.local_endpoint().port();
	}

	json Fixture() {
		std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "McpContracts.json");
		REQUIRE(input.good());
		return json::parse(input);
	}

	json
	Ask(asio::ip::tcp::socket &socket, int id, std::string_view method, json parameters = json::object()) {
		const std::string request =
			json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}}.dump() + "\n";
		asio::write(socket, asio::buffer(request));
		asio::streambuf reply;
		asio::read_until(socket, reply, '\n');
		std::istream input(&reply);
		std::string line;
		std::getline(input, line);
		return json::parse(line);
	}

	json RowsBy(const json &rows, std::string_view field) {
		json indexed = json::object();
		for (const json &row : rows) {
			const std::string key = row.at(std::string(field)).get<std::string>();
			const auto [ignored, inserted] = indexed.emplace(key, row);
			(void)ignored;
			REQUIRE(inserted);
		}
		return indexed;
	}

	json StableInitialize(json value) {
		value["serverInfo"].erase("version");
		return value;
	}

	json StableNegotiation(json value) {
		value.erase("control_generation");
		value.erase("engine_version");
		return value;
	}

	void Check(std::string_view mode, bool factory) {
		if (!std::filesystem::exists(Program())) SKIP("the server program is not built into this preset");
		const json expected = Fixture()[std::string(mode)];
		const uint16_t port = FreePort();
		engine::parallel::Process child;
		std::vector<std::string> arguments{
			"--mcp-port", std::to_string(port), "--unpaced", "--seconds", "30", "--entities", "1"
		};
		if (factory) arguments.push_back("--data-factory");
		REQUIRE(child.Start(Program(), arguments));

		asio::io_context context;
		asio::ip::tcp::socket socket(context);
		std::error_code failure;
		for (int attempt = 0; attempt < 200; ++attempt) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) break;
			socket.close();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE_FALSE(failure);

		const json opened = Ask(socket, 1, "initialize");
		CHECK(StableInitialize(opened["result"]) == expected["initialize"]);
		const json tools = Ask(socket, 2, "tools/list");
		CHECK(RowsBy(tools["result"]["tools"], "name") == expected["tools"]);
		const json resources = Ask(socket, 3, "resources/list");
		CHECK(RowsBy(resources["result"]["resources"], "uri") == expected["resources"]);
		const json promptRows = Ask(socket, 4, "prompts/list");
		CHECK(RowsBy(promptRows["result"]["prompts"], "name") == expected["prompts"]);
		const json negotiated =
			Ask(socket,
				5,
				"tools/call",
				{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
		const json discovery = json::parse(negotiated["result"]["content"][0]["text"].get<std::string>());
		CHECK(StableNegotiation(discovery) == expected["negotiate"]);
		if (!factory) {
			const json worlds = Ask(socket, 6, "tools/call", {{"name", "world_list"}, {"arguments", {}}});
			CHECK_FALSE(worlds["result"].value("isError", false));
			CHECK_FALSE(json::parse(worlds["result"]["content"][0]["text"].get<std::string>()).empty());
			const json components =
				Ask(socket, 7, "tools/call", {{"name", "component_list"}, {"arguments", {}}});
			CHECK_FALSE(components["result"].value("isError", false));
			CHECK(
				json::parse(components["result"]["content"][0]["text"].get<std::string>())
					.contains("components")
			);
			const json created =
				Ask(socket,
					8,
					"tools/call",
					{{"name", "entity_create"}, {"arguments", {{"name", "mcp-contract-probe"}}}});
			CHECK_FALSE(created["result"].value("isError", false));
			const uint64_t createdId = json::parse(created["result"]["content"][0]["text"].get<std::string>())
										   .at("id")
										   .get<uint64_t>();
			const json read =
				Ask(socket, 9, "tools/call", {{"name", "instance_get"}, {"arguments", {{"id", createdId}}}});
			CHECK_FALSE(read["result"].value("isError", false));
			CHECK(json::parse(read["result"]["content"][0]["text"].get<std::string>()).at("id") == createdId);
			const json removed = Ask(
				socket, 10, "tools/call", {{"name", "entity_destroy"}, {"arguments", {{"id", createdId}}}}
			);
			CHECK_FALSE(removed["result"].value("isError", false));
			CHECK(json::parse(removed["result"]["content"][0]["text"].get<std::string>())["ok"] == true);
		}
	}
}

TEST_CASE("server normal MCP manifest is the reviewed contract", "[server][mcp]") {
	Check("normal", false);
}
TEST_CASE("server factory MCP manifest is the reviewed contract", "[server][mcp][data-factory]") {
	Check("factory", true);
}

TEST_CASE("server factory MCP packages survive reset and retirement", "[server][mcp][data-factory]") {
	if (!std::filesystem::exists(Program())) SKIP("the server program is not built into this preset");
	const uint16_t port = FreePort();
	engine::parallel::Process child;
	REQUIRE(child.Start(
		Program(), {"--data-factory", "--mcp-port", std::to_string(port), "--unpaced", "--seconds", "30"}
	));

	asio::io_context context;
	asio::ip::tcp::socket socket(context);
	std::error_code failure;
	for (int attempt = 0; attempt < 200; ++attempt) {
		socket.connect({asio::ip::address_v4::loopback(), port}, failure);
		if (!failure) break;
		socket.close();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE_FALSE(failure);
	REQUIRE(Ask(socket, 1, "initialize").contains("result"));
	int requestId = 1;
	const auto call = [&](std::string_view name, json arguments) {
		const json reply =
			Ask(socket, ++requestId, "tools/call", {{"name", name}, {"arguments", std::move(arguments)}});
		INFO(reply.dump());
		REQUIRE_FALSE(reply.contains("error"));
		REQUIRE_FALSE(reply["result"].value("isError", false));
		return json::parse(reply["result"]["content"][0]["text"].get<std::string>());
	};
	const auto revision = [](const json &state, std::string_view operation) {
		return json{
			{"instance_id", "server-package-world"},
			{"expected_tick", state.at("tick")},
			{"expected_world_epoch", state.at("world_epoch")},
			{"expected_world_version", state.at("world_version")},
			{"operation_id", operation},
		};
	};
	const std::string source = "return";
	const json manifest{
		{"format", "atomic.data-script.v1"},
		{"entry", "package.luau"},
		{"source_hash", engine::assets::Hasher::Of(std::as_bytes(std::span(source))).ToHex()},
		{"assets", json::array()},
		{"parameters", json::array()},
		{"capabilities", json::array()},
		{"budget", {{"source_bytes", 1024}, {"asset_bytes", 0}, {"assets", 0}, {"parameters", 0}}},
		{"seed", 0},
	};
	const auto package = [&](const json &state, std::string_view operation) {
		json arguments = revision(state, operation);
		arguments["language"] = "luau";
		arguments["manifest"] = manifest.dump();
		arguments["source_base64"] = "cmV0dXJu";
		arguments["assets"] = json::array();
		const json applied = call("run_script_package", std::move(arguments));
		CHECK(applied.at("status") == "completed");
		CHECK(applied.at("ran") == true);
		CHECK(applied.at("atomic") == true);
		CHECK(applied.at("tick") == state.at("tick"));
		return applied;
	};
	json state = call(
		"world_create",
		{{"instance_id", "server-package-world"}, {"seed", 1}, {"tick_rate", 30}, {"operation_id", "create"}}
	);
	state = package(state, "package-first");
	const json tree = call("world_tree", {{"world", "server-package-world"}});
	const auto workspace =
		std::find_if(tree.at("roots").begin(), tree.at("roots").end(), [](const json &root) {
			return root.at("name") == "Workspace";
		});
	REQUIRE(workspace != tree.at("roots").end());
	const json createdPart = call(
		"entity_create",
		{{"world", "server-package-world"},
		 {"class", "Part"},
		 {"name", "GravityProbe"},
		 {"parent", workspace->at("id")}}
	);
	const json partId = createdPart.at("id");
	call(
		"instance_set",
		{{"world", "server-package-world"},
		 {"id", partId},
		 {"property", "Position"},
		 {"value", {{"X", 0}, {"Y", 10}, {"Z", 0}}}}
	);
	call(
		"instance_set",
		{{"world", "server-package-world"}, {"id", partId}, {"property", "Anchored"}, {"value", false}}
	);
	const auto position = [&] {
		return call("instance_get", {{"world", "server-package-world"}, {"id", partId}})
			.at("properties")
			.at("Position")
			.at("Y")
			.get<double>();
	};
	const double before = position();
	for (unsigned index = 0; index < 6; ++index) {
		json arguments = revision(state, "physics-step-" + std::to_string(index));
		arguments["dt_ns"] = {{"numerator", 1'000'000'000u}, {"denominator", 30u}};
		state = call("step", std::move(arguments));
	}
	CHECK(position() < before);
	state = package(state, "package-second");
	json reset = revision(state, "reset");
	reset["seed"] = 2;
	reset["tick_rate"] = 60;
	const json fresh = call("world_reset", std::move(reset));
	CHECK(fresh.at("world_epoch").get<uint64_t>() == state.at("world_epoch").get<uint64_t>() + 1);
	state = package(fresh, "package-after-reset");
	const json retired = call("world_retire", revision(state, "retire"));
	CHECK(retired.at("tombstone") == true);
}

TEST_CASE("server replication observation MCP hook follows listener availability", "[server][mcp]") {
	if (!std::filesystem::exists(Program())) SKIP("the server program is not built into this preset");
	const uint16_t port = FreePort();
	engine::parallel::Process child;
	REQUIRE(child.Start(
		Program(),
		{"--mcp-port",
		 std::to_string(port),
		 "--listen",
		 "--listen-port",
		 "0",
		 "--unpaced",
		 "--seconds",
		 "30",
		 "--entities",
		 "1"}
	));

	asio::io_context context;
	asio::ip::tcp::socket socket(context);
	std::error_code failure;
	for (int attempt = 0; attempt < 200; ++attempt) {
		socket.connect({asio::ip::address_v4::loopback(), port}, failure);
		if (!failure) break;
		socket.close();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	REQUIRE_FALSE(failure);

	const json negotiated =
		Ask(socket,
			1,
			"tools/call",
			{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
	const json discovery = json::parse(negotiated["result"]["content"][0]["text"].get<std::string>());
	const auto hook = std::find_if(discovery["hooks"].begin(), discovery["hooks"].end(), [](const json &row) {
		return row["id"] == "server.replication-observation";
	});
	REQUIRE(hook != discovery["hooks"].end());
	CHECK(*hook == Fixture()["replication_observation"]["hook"]);
}

TEST_CASE("server MCP listener restarts with its reviewed normal manifest", "[server][mcp]") {
	if (!std::filesystem::exists(Program())) SKIP("the server program is not built into this preset");
	const json expected = Fixture()["normal"];
	const uint16_t port = FreePort();
	const auto run = [&](engine::parallel::Process &child) {
		REQUIRE(child.Start(
			Program(),
			{"--mcp-port", std::to_string(port), "--unpaced", "--seconds", "0.25", "--entities", "1"}
		));

		asio::io_context context;
		asio::ip::tcp::socket socket(context);
		std::error_code failure;
		for (int attempt = 0; attempt < 200; ++attempt) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) break;
			socket.close();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE_FALSE(failure);

		const json opened = Ask(socket, 1, "initialize");
		CHECK(StableInitialize(opened["result"]) == expected["initialize"]);
		const json negotiated =
			Ask(socket,
				2,
				"tools/call",
				{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
		return StableNegotiation(json::parse(negotiated["result"]["content"][0]["text"].get<std::string>()));
	};

	engine::parallel::Process first;
	const json firstManifest = run(first);
	CHECK(firstManifest == expected["negotiate"]);
	const engine::parallel::ProcessStatus firstExit = first.Wait();
	REQUIRE(firstExit.Reason == engine::parallel::ExitReason::Exited);
	CHECK(firstExit.Code == 0);

	engine::parallel::Process second;
	const json secondManifest = run(second);
	CHECK(secondManifest == firstManifest);
	const engine::parallel::ProcessStatus secondExit = second.Wait();
	REQUIRE(secondExit.Reason == engine::parallel::ExitReason::Exited);
	CHECK(secondExit.Code == 0);
}
