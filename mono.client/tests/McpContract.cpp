// Product-level MCP mode contracts, observed through the staged client binary.

#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("client.mcpcontract")

namespace {
	using nlohmann::json;

	std::filesystem::path Program() {
		return engine::core::Paths::Base().parent_path() / "client" / engine::core::Paths::Program("client");
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

	json RowsBy(const json &rows, std::string_view field) {
		json indexed = json::object();
		for (const json &row : rows) {
			const auto [ignored, inserted] =
				indexed.emplace(row.at(std::string(field)).get<std::string>(), row);
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

	bool Capture(bool factory, const json &observed) {
		const char *directory = std::getenv("ATOMIC_MCP_CONTRACT_CAPTURE_DIR");
		if (directory == nullptr || *directory == '\0') return false;
		const std::filesystem::path root(directory);
		if (!std::filesystem::is_directory(root)) {
			throw std::runtime_error("ATOMIC_MCP_CONTRACT_CAPTURE_DIR must name an existing directory");
		}
		std::ofstream output(root / (factory ? "client-factory.json" : "client-normal.json"));
		REQUIRE(output.good());
		output << observed.dump(2) << '\n';
		return true;
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

	void Check(bool factory) {
		if (!std::filesystem::exists(Program())) SKIP("the client program is not built into this preset");
		const json expected = Fixture()[factory ? "factory" : "normal"];
		const uint16_t port = FreePort();
		engine::parallel::Process child;
		std::vector<std::string> arguments{"--headless", "--mcp-port", std::to_string(port)};
		if (factory)
			arguments.push_back("--data-factory");
		else
			arguments.insert(arguments.end(), {"--frames", "3000"});
		REQUIRE(child.Start(Program(), arguments));

		asio::io_context context;
		asio::ip::tcp::socket socket(context);
		std::error_code failure;
		// Vulkan and asset startup can exceed two seconds while the full suite runs.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) break;
			socket.close();
			if (std::chrono::steady_clock::now() >= deadline) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE_FALSE(failure);

		const json opened = Ask(socket, 1, "initialize");
		CHECK(StableInitialize(opened["result"]) == expected["initialize"]);
		const json tools = Ask(socket, 2, "tools/list")["result"]["tools"];
		CHECK(RowsBy(tools, "name") == expected["tools"]);
		const json resources = Ask(socket, 3, "resources/list")["result"]["resources"];
		const json prompts = Ask(socket, 4, "prompts/list")["result"]["prompts"];
		CHECK(RowsBy(resources, "uri") == expected["resources"]);
		CHECK(RowsBy(prompts, "name") == expected["prompts"]);
		const json negotiated =
			Ask(socket,
				5,
				"tools/call",
				{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
		const json discovery = json::parse(negotiated["result"]["content"][0]["text"].get<std::string>());
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
			const json click = Ask(
				socket, 8, "tools/call", {{"name", "emulate_click"}, {"arguments", {{"x", 5.0}, {"y", 5.0}}}}
			);
			CHECK_FALSE(click["result"].value("isError", false));
			CHECK(json::parse(click["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
			const json key =
				Ask(socket, 9, "tools/call", {{"name", "emulate_key"}, {"arguments", {{"key", "F5"}}}});
			CHECK_FALSE(key["result"].value("isError", false));
			CHECK(json::parse(key["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
			const json text =
				Ask(socket,
					10,
					"tools/call",
					{{"name", "emulate_text"}, {"arguments", {{"text", "headless control"}}}});
			CHECK_FALSE(text["result"].value("isError", false));
			CHECK(json::parse(text["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
		}
		const json observed{
			{"initialize", StableInitialize(opened["result"])},
			{"tools", RowsBy(tools, "name")},
			{"resources", RowsBy(resources, "uri")},
			{"prompts", RowsBy(prompts, "name")},
			{"negotiate", StableNegotiation(discovery)},
		};
		if (Capture(factory, observed)) return;
		CHECK(observed["initialize"] == expected["initialize"]);
		CHECK(observed["tools"] == expected["tools"]);
		CHECK(observed["resources"] == expected["resources"]);
		CHECK(observed["prompts"] == expected["prompts"]);
		CHECK(observed["negotiate"] == expected["negotiate"]);
	}
}

TEST_CASE("client normal MCP surface is available", "[client][mcp]") {
	Check(false);
}
TEST_CASE("client factory MCP surface is available", "[client][mcp][data-factory]") {
	Check(true);
}

TEST_CASE("client MCP listener restarts with its reviewed normal manifest", "[client][mcp]") {
	if (!std::filesystem::exists(Program())) SKIP("the client program is not built into this preset");
	const json expected = Fixture()["normal"];
	const uint16_t port = FreePort();
	const auto run = [&](engine::parallel::Process &child) {
		REQUIRE(
			child.Start(Program(), {"--headless", "--mcp-port", std::to_string(port), "--frames", "600"})
		);

		asio::io_context context;
		asio::ip::tcp::socket socket(context);
		std::error_code failure;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) break;
			socket.close();
			if (std::chrono::steady_clock::now() >= deadline) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		REQUIRE_FALSE(failure);

		const json opened = Ask(socket, 1, "initialize");
		const json tools = Ask(socket, 2, "tools/list")["result"]["tools"];
		const json resources = Ask(socket, 3, "resources/list")["result"]["resources"];
		const json prompts = Ask(socket, 4, "prompts/list")["result"]["prompts"];
		const json negotiated =
			Ask(socket,
				5,
				"tools/call",
				{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
		return json{
			{"initialize", StableInitialize(opened["result"])},
			{"tools", RowsBy(tools, "name")},
			{"resources", RowsBy(resources, "uri")},
			{"prompts", RowsBy(prompts, "name")},
			{"negotiate",
			 StableNegotiation(json::parse(negotiated["result"]["content"][0]["text"].get<std::string>()))},
		};
	};
	const auto waitForExit = [](engine::parallel::Process &child) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		engine::parallel::ProcessStatus status = child.Poll();
		while (status.Alive() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			status = child.Poll();
		}
		if (status.Alive()) {
			child.Kill();
			FAIL("client did not exit after its frame budget");
		}
		return status;
	};

	engine::parallel::Process first;
	const json firstManifest = run(first);
	CHECK(firstManifest == expected);
	const engine::parallel::ProcessStatus firstExit = waitForExit(first);
	REQUIRE(firstExit.Reason == engine::parallel::ExitReason::Exited);
	CHECK(firstExit.Code == 0);

	engine::parallel::Process second;
	const json secondManifest = run(second);
	CHECK(secondManifest == firstManifest);
	const engine::parallel::ProcessStatus secondExit = waitForExit(second);
	REQUIRE(secondExit.Reason == engine::parallel::ExitReason::Exited);
	CHECK(secondExit.Code == 0);
}
