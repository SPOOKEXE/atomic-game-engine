// Product-level MCP contracts, observed through the actual server binary.

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
			"--mcp-port", std::to_string(port), "--unpaced", "--seconds", "30"
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

TEST_CASE("server replication observation MCP hook follows listener availability", "[server][mcp]") {
	if (!std::filesystem::exists(Program())) SKIP("the server program is not built into this preset");
	const uint16_t port = FreePort();
	engine::parallel::Process child;
	REQUIRE(child.Start(
		Program(),
		{"--mcp-port", std::to_string(port), "--listen", "--listen-port", "0", "--unpaced", "--seconds", "30"}
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
		REQUIRE(
			child.Start(Program(), {"--mcp-port", std::to_string(port), "--unpaced", "--seconds", "0.25"})
		);

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
