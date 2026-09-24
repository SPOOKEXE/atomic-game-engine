// Studio MCP mode contracts, observed through the staged program.

#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("studio.mcpcontract")

namespace {
	using nlohmann::json;
	using Clock = std::chrono::steady_clock;

	std::filesystem::path Program() {
		return engine::core::Paths::Base().parent_path() / "studio" / engine::core::Paths::Program("studio");
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

	std::optional<std::string> ReadLine(asio::ip::tcp::socket &socket, std::chrono::milliseconds timeout) {
		std::string line;
		std::array<char, 4096> bytes{};
		std::error_code failure;
		socket.non_blocking(true, failure);
		if (failure) return {};
		const Clock::time_point deadline = Clock::now() + timeout;
		while (Clock::now() < deadline) {
			const size_t read = socket.read_some(asio::buffer(bytes), failure);
			if (!failure) {
				line.append(bytes.data(), read);
				if (const size_t end = line.find('\n'); end != std::string::npos) return line.substr(0, end);
				continue;
			}
			if (failure != asio::error::would_block && failure != asio::error::try_again) return {};
			failure.clear();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return {};
	}

	std::optional<json>
	Ask(asio::ip::tcp::socket &socket, int id, std::string_view method, json parameters = {}) {
		std::error_code failure;
		socket.non_blocking(false, failure);
		if (failure) return {};
		const std::string request =
			json{
				{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(parameters)}
			}.dump() +
			"\n";
		asio::write(socket, asio::buffer(request), failure);
		if (failure) return {};
		const std::optional<std::string> reply = ReadLine(socket, std::chrono::seconds(5));
		if (!reply) return {};
		try {
			return json::parse(*reply);
		} catch (const json::parse_error &) {
			return {};
		}
	}

	bool Connect(asio::ip::tcp::socket &socket, uint16_t port) {
		std::error_code failure;
		for (int attempt = 0; attempt < 200; ++attempt) {
			socket.connect({asio::ip::address_v4::loopback(), port}, failure);
			if (!failure) return true;
			std::error_code ignored;
			socket.close(ignored);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	bool Contains(const json &tools, std::string_view name) {
		return std::any_of(tools.begin(), tools.end(), [name](const json &tool) {
			return tool.value("name", "") == name;
		});
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

	bool Capture(std::string_view mode, const json &observed) {
		const char *directory = std::getenv("ATOMIC_MCP_CONTRACT_CAPTURE_DIR");
		if (directory == nullptr || *directory == '\0') return false;
		const std::filesystem::path root(directory);
		if (!std::filesystem::is_directory(root)) {
			throw std::runtime_error("ATOMIC_MCP_CONTRACT_CAPTURE_DIR must name an existing directory");
		}
		std::ofstream output(root / ("studio-" + std::string(mode) + ".json"));
		REQUIRE(output.good());
		output << observed.dump(2) << '\n';
		return true;
	}

	void Check(std::string_view mode, bool factory) {
		if (!std::filesystem::exists(Program())) SKIP("the Studio program is not built into this preset");
		const json expected = Fixture()[std::string(mode)];
		const uint16_t port = FreePort();
		engine::parallel::Process child;
		std::vector<std::string> arguments{
			"--headless", "--frames", "3000", "--mcp-port", std::to_string(port)
		};
		if (factory) arguments.push_back("--data-factory");
		REQUIRE(child.Start(Program(), arguments));

		asio::io_context context;
		asio::ip::tcp::socket socket(context);
		REQUIRE(Connect(socket, port));
		const auto opened = Ask(socket, 1, "initialize");
		REQUIRE(opened);
		CHECK((*opened)["result"]["serverInfo"]["name"] == expected["server"]);
		CHECK((*opened)["result"]["capabilities"]["tools"]["listChanged"] == false);

		const auto listed = Ask(socket, 2, "tools/list");
		REQUIRE(listed);
		const json &tools = (*listed)["result"]["tools"];
		for (const json &tool : tools)
			CHECK(tool.contains("inputSchema"));
		const json toolRows = RowsBy(tools, "name");
		for (const json &entry : expected["required_tools"])
			CHECK(Contains(tools, entry.get<std::string>()));
		for (const json &entry : expected["forbidden_tools"])
			CHECK_FALSE(Contains(tools, entry.get<std::string>()));
		const auto resources = Ask(socket, 3, "resources/list");
		REQUIRE(resources);
		CHECK_FALSE((*resources)["result"]["resources"].empty());
		const auto prompts = Ask(socket, 4, "prompts/list");
		REQUIRE(prompts);
		CHECK_FALSE((*prompts)["result"]["prompts"].empty());

		const auto negotiated =
			Ask(socket,
				5,
				"tools/call",
				{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
		REQUIRE(negotiated);
		const json discovery = json::parse((*negotiated)["result"]["content"][0]["text"].get<std::string>());
		CHECK(discovery["contract_version"] == "1");
		CHECK(discovery.contains("hooks"));
		if (factory) CHECK_FALSE(discovery["hooks"].empty());
		if (!factory) {
			const auto worlds = Ask(socket, 6, "tools/call", {{"name", "world_list"}, {"arguments", {}}});
			REQUIRE(worlds);
			CHECK_FALSE((*worlds)["result"].value("isError", false));
			CHECK_FALSE(json::parse((*worlds)["result"]["content"][0]["text"].get<std::string>()).empty());
			const auto components =
				Ask(socket, 7, "tools/call", {{"name", "component_list"}, {"arguments", {}}});
			REQUIRE(components);
			CHECK_FALSE((*components)["result"].value("isError", false));
			CHECK(
				json::parse((*components)["result"]["content"][0]["text"].get<std::string>())
					.contains("components")
			);
			const auto click = Ask(
				socket, 8, "tools/call", {{"name", "emulate_click"}, {"arguments", {{"x", 5.0}, {"y", 5.0}}}}
			);
			REQUIRE(click);
			CHECK_FALSE((*click)["result"].value("isError", false));
			CHECK(json::parse((*click)["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
			const auto key =
				Ask(socket, 9, "tools/call", {{"name", "emulate_key"}, {"arguments", {{"key", "F5"}}}});
			REQUIRE(key);
			CHECK_FALSE((*key)["result"].value("isError", false));
			CHECK(json::parse((*key)["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
			const auto text =
				Ask(socket,
					10,
					"tools/call",
					{{"name", "emulate_text"}, {"arguments", {{"text", "headless control"}}}});
			REQUIRE(text);
			CHECK_FALSE((*text)["result"].value("isError", false));
			CHECK(json::parse((*text)["result"]["content"][0]["text"].get<std::string>())["queued"] == true);
			const auto behavior =
				Ask(socket, 11, "tools/call", {{"name", "viewport_behavior_diagnostics"}, {"arguments", {}}});
			REQUIRE(behavior);
			CHECK_FALSE((*behavior)["result"].value("isError", false));
			const json behaviorData =
				json::parse((*behavior)["result"]["content"][0]["text"].get<std::string>());
			CHECK(behaviorData.contains("behavior_pose"));
			CHECK(behaviorData.contains("culling"));
			CHECK(behaviorData.contains("lod"));
			CHECK(behaviorData.contains("lighting"));
			CHECK(behaviorData.contains("particles"));
			CHECK(behaviorData.contains("portal_demand"));
		}
		const json observed{
			{"server", (*opened)["result"]["serverInfo"]["name"]},
			{"initialize", StableInitialize((*opened)["result"])},
			{"tool_rows", toolRows},
			{"resources", RowsBy((*resources)["result"]["resources"], "uri")},
			{"prompts", RowsBy((*prompts)["result"]["prompts"], "name")},
			{"negotiate", StableNegotiation(discovery)},
		};
		if (Capture(mode, observed)) return;
		CHECK(StableInitialize((*opened)["result"]) == expected["initialize"]);
		CHECK(toolRows == expected["tool_rows"]);
		CHECK(RowsBy((*resources)["result"]["resources"], "uri") == expected["resources"]);
		CHECK(RowsBy((*prompts)["result"]["prompts"], "name") == expected["prompts"]);
		CHECK(StableNegotiation(discovery) == expected["negotiate"]);
	}
}

TEST_CASE("Studio normal MCP manifest is available", "[studio][mcp]") {
	Check("normal", false);
}
TEST_CASE("Studio factory MCP manifest is available", "[studio][mcp][data-factory]") {
	Check("factory", true);
}
