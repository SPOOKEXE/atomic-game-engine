// Product-level MCP mode contracts, observed through the staged client binary.

#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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
		for (int attempt = 0; attempt < 200; ++attempt) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) break;
			socket.close();
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
		CHECK(StableNegotiation(discovery) == expected["negotiate"]);
	}
}

TEST_CASE("client normal MCP surface is available", "[client][mcp]") {
	Check(false);
}
TEST_CASE("client factory MCP surface is available", "[client][mcp][data-factory]") {
	Check(true);
}
