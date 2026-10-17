// The checked CDN MCP contract, observed through the staged program.

#include <engine/assets/Signature.hpp>
#include <engine/core/Paths.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <asio.hpp>
#include <cdn/Publisher.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

TEST_SUITE_ID("cdn.mcpcontract")
TEST_DEPENDS("cdn.publisher")

namespace {
	using nlohmann::json;
	constexpr std::string_view KEY = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
	struct Workspace {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-cdn-mcp-contract";
		Workspace() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
			std::filesystem::create_directories(Root / "content", ignored);
		}
		~Workspace() {
			std::error_code ignored;
			std::filesystem::remove_all(Root, ignored);
		}
	};

	std::filesystem::path Program() {
		return engine::core::Paths::Base().parent_path() / "cdn" / engine::core::Paths::Program("cdn");
	}
	uint16_t FreePort() {
		asio::io_context context;
		asio::ip::tcp::acceptor probe(context, {asio::ip::address_v4::loopback(), 0});
		return probe.local_endpoint().port();
	}
	json Fixture() {
		std::ifstream input(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "McpContract.json");
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
	std::vector<std::string> Names(const json &rows, std::string_view field) {
		std::vector<std::string> names;
		for (const json &row : rows)
			names.push_back(row[std::string(field)].get<std::string>());
		return names;
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
	bool Capture(const json &observed) {
		const char *directory = std::getenv("ATOMIC_MCP_CONTRACT_CAPTURE_DIR");
		if (directory == nullptr || *directory == '\0') return false;
		const std::filesystem::path root(directory);
		if (!std::filesystem::is_directory(root)) {
			throw std::runtime_error("ATOMIC_MCP_CONTRACT_CAPTURE_DIR must name an existing directory");
		}
		std::ofstream output(root / "cdn.json");
		REQUIRE(output.good());
		output << observed.dump(2) << '\n';
		return true;
	}
}

TEST_CASE("CDN MCP manifest is the reviewed contract", "[cdn][mcp]") {
	if (!std::filesystem::exists(Program())) SKIP("the CDN program is not built into this preset");
	Workspace workspace;
	const auto &root = workspace.Root;
	{
		std::ofstream content(root / "content" / "contract.txt");
		content << "contract fixture";
	}
	std::array<std::byte, 32> seed{};
	for (size_t index = 0; index < seed.size(); ++index)
		seed[index] = static_cast<std::byte>(index);
	auto key = engine::assets::SigningKey::FromSeed(seed);
	REQUIRE(key);
	REQUIRE(cdn::Publish(root / "content", root / "store", *key));
	const uint16_t port = FreePort();
	engine::parallel::Process child;
	REQUIRE(child.Start(
		Program(),
		{"--store",
		 (root / "store").string(),
		 "--grant-key",
		 std::string(KEY),
		 "--mcp-port",
		 std::to_string(port),
		 "--frames",
		 "3000"}
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
	const json expected = Fixture();
	const json opened = Ask(socket, 1, "initialize");
	CHECK(opened["result"]["serverInfo"]["name"] == expected["server"]);
	const json tools = Ask(socket, 2, "tools/list");
	CHECK(Names(tools["result"]["tools"], "name") == expected["tools"].get<std::vector<std::string>>());
	for (const json &tool : tools["result"]["tools"])
		CHECK(tool.contains("inputSchema"));
	const json toolRows = RowsBy(tools["result"]["tools"], "name");
	const json resourceReply = Ask(socket, 3, "resources/list");
	const json resourceRows = RowsBy(resourceReply["result"]["resources"], "uri");
	const auto resources = Names(resourceReply["result"]["resources"], "uri");
	for (const json &entry : expected["required_resources"])
		CHECK(std::find(resources.begin(), resources.end(), entry.get<std::string>()) != resources.end());
	const json promptReply = Ask(socket, 4, "prompts/list");
	const json promptRows = RowsBy(promptReply["result"]["prompts"], "name");
	const auto prompts = Names(promptReply["result"]["prompts"], "name");
	for (const json &entry : expected["required_prompts"])
		CHECK(std::find(prompts.begin(), prompts.end(), entry.get<std::string>()) != prompts.end());
	const json negotiated =
		Ask(socket,
			5,
			"tools/call",
			{{"name", "negotiate"}, {"arguments", {{"requested_channels", {"rgb_linear_hdr"}}}}});
	const json discovery = json::parse(negotiated["result"]["content"][0]["text"].get<std::string>());
	CHECK(discovery["contract_version"] == "1");
	CHECK(discovery["requested_channels"][0]["supported"] == false);
	const auto product =
		std::find_if(discovery["hooks"].begin(), discovery["hooks"].end(), [](const json &hook) {
			return hook["id"] == "cdn.product";
		});
	REQUIRE(product != discovery["hooks"].end());
	CHECK((*product)["state"] == "active");
	CHECK((*product)["tools"] == json::array({"engine_info"}));
	const json info = Ask(socket, 6, "tools/call", {{"name", "engine_info"}, {"arguments", {}}});
	CHECK_FALSE(info["result"].value("isError", false));
	CHECK(json::parse(info["result"]["content"][0]["text"].get<std::string>()).contains("control"));
	for (const std::string_view unavailable :
		 {"emulate_click",
		  "emulate_key",
		  "emulate_text",
		  "instance_get",
		  "instance_set",
		  "component_get",
		  "component_set"}) {
		CHECK_FALSE(
			std::any_of(
				tools["result"]["tools"].begin(),
				tools["result"]["tools"].end(),
				[unavailable](const json &tool) { return tool["name"] == unavailable; }
			)
		);
	}
	const json observed{
		{"server", opened["result"]["serverInfo"]["name"]},
		{"initialize", StableInitialize(opened["result"])},
		{"tool_rows", toolRows},
		{"resources", resourceRows},
		{"prompts", promptRows},
		{"negotiate", StableNegotiation(discovery)},
	};
	if (Capture(observed)) return;
	CHECK(StableInitialize(opened["result"]) == expected["initialize"]);
	CHECK(toolRows == expected["tool_rows"]);
	CHECK(resourceRows == expected["resources"]);
	CHECK(promptRows == expected["prompts"]);
	CHECK(StableNegotiation(discovery) == expected["negotiate"]);
}
