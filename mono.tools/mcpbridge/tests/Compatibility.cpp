// The fixed direct transcript for the MCP wire contract.
//
// `Bridge.cpp` sends the same family of requests through the real stdio
// adapter. This suite keeps the transport-independent contract close to its
// reviewed fixture, so a change in dispatch does not need a socket failure to
// be noticed.

#include <engine/control/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("tools.mcpbridge.compatibility")
TEST_DEPENDS("engine.control.surface")

using engine::control::Prompt;
using engine::control::PromptArgument;
using engine::control::Resource;
using engine::control::Surface;
using engine::control::Tool;
using nlohmann::json;

namespace {

	json Fixture() {
		const std::filesystem::path path =
			std::filesystem::path(__FILE__).parent_path() / "fixtures" / "Compatibility.json";
		std::ifstream input(path);
		REQUIRE(input.good());
		return json::parse(input);
	}

	json Ask(Surface &surface, int id, const std::string &method, const json &parameters = json::object()) {
		const json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}};
		const std::string reply = surface.Answer(request.dump());
		INFO(method << " -> " << reply);
		REQUIRE_FALSE(reply.empty());
		return json::parse(reply);
	}

	void MakeSurface(Surface &surface, const json &fixture) {
		surface.Add(
			Tool{
				fixture["tool"]["name"],
				"Returns its text argument so a compatibility transcript can prove request and result "
				"envelopes.",
				[] {
					return json{
						{"type", "object"},
						{"properties", json{{"text", json{{"type", "string"}}}}},
						{"required", json::array({"text"})}
					};
				},
				[](const json &arguments, std::string &failure) -> json {
					if (!arguments.contains("text") || !arguments["text"].is_string()) {
						failure = "echo needs text";
						return nullptr;
					}
					return json{{"text", arguments["text"]}};
				},
			}
		);
		surface.Add(
			Tool{
				fixture["refusedTool"]["name"],
				"Refuses predictably so the fixture preserves MCP tool-error envelopes.",
				nullptr,
				[reason = fixture["refusedTool"]["reason"].get<std::string>()](
					const json &, std::string &failure
				) -> json {
					failure = reason;
					return nullptr;
				},
			}
		);
		surface.AddResource(
			Resource{
				fixture["resource"]["uri"],
				"Compatibility baseline",
				"A fixed readable resource for the baseline MCP transcript.",
				"text/plain",
				[text = fixture["resource"]["text"].get<std::string>()](std::string &) { return text; },
			}
		);
		surface.AddPrompt(
			Prompt{
				fixture["prompt"]["name"],
				"Renders one fixed baseline prompt.",
				{PromptArgument{"subject", "A subject for the fixture prompt.", true}},
				[](const json &arguments, std::string &failure) -> std::string {
					if (!arguments.contains("subject") || !arguments["subject"].is_string()) {
						failure = "baseline_prompt needs subject";
						return {};
					}
					return "baseline prompt for " + arguments["subject"].get<std::string>();
				},
			}
		);
		surface.AddDiscoveryTools();
	}
}

TEST_CASE("the direct compatibility transcript preserves MCP envelopes", "[mcpbridge][compatibility]") {
	const json fixture = Fixture();
	Surface surface(fixture["server"]["name"], fixture["server"]["purpose"]);
	MakeSurface(surface, fixture);

	const json opened = Ask(surface, 1, "initialize");
	CHECK(opened["result"]["serverInfo"]["name"] == fixture["server"]["name"]);
	CHECK(opened["result"]["capabilities"].contains("tools"));
	CHECK(opened["result"]["capabilities"].contains("resources"));
	CHECK(opened["result"]["capabilities"].contains("prompts"));
	CHECK(surface.Answer(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());

	const json tools = Ask(surface, 2, "tools/list");
	CHECK(tools["result"]["tools"][0]["inputSchema"] == fixture["tool"]["inputSchema"]);

	const json called =
		Ask(surface,
			3,
			"tools/call",
			json{{"name", fixture["tool"]["name"]}, {"arguments", fixture["tool"]["arguments"]}});
	CHECK(called["result"]["isError"] == false);
	CHECK(
		json::parse(called["result"]["content"][0]["text"].get<std::string>()) == fixture["tool"]["result"]
	);

	const json refused = Ask(surface, 4, "tools/call", json{{"name", fixture["refusedTool"]["name"]}});
	CHECK(refused["result"]["isError"] == true);
	CHECK(
		refused["result"]["content"][0]["text"].get<std::string>().find(
			fixture["refusedTool"]["reason"].get<std::string>()
		) != std::string::npos
	);

	const json missing = Ask(surface, 5, "tools/call", json{{"name", "unknown_tool"}});
	CHECK(missing["result"]["isError"] == true);

	const json resource = Ask(surface, 6, "resources/read", json{{"uri", fixture["resource"]["uri"]}});
	CHECK(resource["result"]["contents"][0]["text"] == fixture["resource"]["text"]);

	const json prompt =
		Ask(surface,
			7,
			"prompts/get",
			json{
				{"name", fixture["prompt"]["name"]},
				{"arguments",
				 json{{fixture["prompt"]["argument"]["name"], fixture["prompt"]["argument"]["value"]}}}
			});
	CHECK(prompt["result"]["messages"][0]["content"]["text"] == fixture["prompt"]["text"]);

	const json negotiated =
		Ask(surface,
			8,
			"tools/call",
			json{{"name", "negotiate"}, {"arguments", fixture["negotiate"]["arguments"]}});
	REQUIRE(negotiated["result"]["isError"] == false);
	const json negotiation = json::parse(negotiated["result"]["content"][0]["text"].get<std::string>());
	CHECK(negotiation["contract_version"] == fixture["negotiate"]["contract_version"]);
	CHECK(negotiation["schema_version"] == fixture["negotiate"]["schema_version"]);
	CHECK(negotiation["control_generation"] == fixture["negotiate"]["control_generation"]);
	CHECK(negotiation["hooks"] == fixture["negotiate"]["hooks"]);
	CHECK(negotiation["requested_channels"] == fixture["negotiate"]["requested_channels"]);
	CHECK(negotiation["headless_cpu"] == fixture["negotiate"]["headless_cpu"]);
	CHECK(negotiation["offscreen_gpu"] == fixture["negotiate"]["offscreen_gpu"]);

	const json unknown = Ask(surface, 9, "unknown/method");
	CHECK(unknown["error"]["code"] == -32601);
}
