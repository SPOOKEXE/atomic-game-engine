// Every row of every table, exercised once.
//
// **A schema that has never been called is not a tool.** `tools/list` builds its
// `inputSchema` from a lambda that nothing else runs, so a schema naming a
// required argument the handler never reads, or declaring `required` as a string
// rather than an array, is invisible until a client refuses to call it - and the
// client that refuses is usually somebody else's. The same is true of a resource
// whose reader throws and a prompt whose renderer returns nothing.
//
// So this walks the whole surface a program registers and asserts the shape of
// every row, rather than picking the interesting ones. Adding a tool that does
// not fit fails here rather than in a transcript.
//
// **`mcpbridge` had no suite at all until v0.19**, which the root `AGENTS.md`
// calls a gap rather than a convention. The bridge is a byte pump and the thing
// worth testing is the protocol it carries, so half of that lives here and the
// other half - the pump itself, driven through the real binary - lives in
// `Bridge.cpp`.

#include <engine/control/Architecture.hpp>
#include <engine/control/Features.hpp>
#include <engine/control/Surface.hpp>
#include <engine/control/features/Script.hpp>
#include <engine/control/features/Universe.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <string>

TEST_SUITE_ID("tools.mcpbridge.schemas")

using engine::control::Graph;
using engine::control::GraphModule;
using engine::control::LinkVerdict;
using engine::control::Prompt;
using engine::control::Resource;
using engine::control::Surface;
using engine::control::Tool;
using engine::core::Name;
using engine::world::Universe;
using engine::world::WorldSettings;
using nlohmann::json;

namespace {
	// A surface with everything a program with worlds registers.
	//
	// The universe outlives the surface because the caller holds both, which is
	// what the universe feature documents: it keeps the reference.
	void Fill(Surface &surface, Universe &universe) {
		WorldSettings settings;
		settings.Name = Name("mcpbridge-schemas");
		universe.Create(settings);

		const std::array features{
			engine::control::features::Universe(universe),
			engine::control::features::Architecture(),
			engine::control::features::Script(),
			engine::control::features::Diagnostics(),
			engine::control::features::Build(),
			engine::control::features::Resources(),
			engine::control::features::Prompts(),
		};
		surface.Enable(features);
	}

	json Ask(Surface &surface, const std::string &method, const json &parameters = json::object()) {
		const json request{
			{"jsonrpc", "2.0"},
			{"id", 7},
			{"method", method},
			{"params", parameters},
		};

		const std::string reply = surface.Answer(request.dump());
		INFO(method << " -> " << reply);
		REQUIRE_FALSE(reply.empty());
		return json::parse(reply);
	}

	// Whether every character is one a tool name may be spelled with.
	bool LowerSnake(const std::string &name) {
		for (const char letter : name) {
			if ((letter < 'a' || letter > 'z') && letter != '_') {
				return false;
			}
		}
		return !name.empty();
	}
}

// --- the tables ---------------------------------------------------------------

TEST_CASE("every tool's schema is one a client could build a call from", "[mcpbridge]") {
	Universe universe;
	Surface surface("test", "a suite");
	Fill(surface, universe);

	REQUIRE(surface.Count() > 0);

	const json listed = Ask(surface, "tools/list");
	REQUIRE(listed["result"]["tools"].size() == surface.Count());

	for (const json &tool : listed["result"]["tools"]) {
		const std::string name = tool.at("name").get<std::string>();
		INFO("tool " << name);

		CHECK(LowerSnake(name));

		// **The description is the only documentation a client gets**, and a
		// one-word one is what "lists worlds" looked like before the module's
		// AGENTS.md said not to. The bound is deliberately low: it catches an
		// empty or placeholder row without becoming a style rule.
		REQUIRE(tool.contains("description"));
		CHECK(tool.at("description").get<std::string>().size() > 40);

		REQUIRE(tool.contains("inputSchema"));
		const json &schema = tool.at("inputSchema");
		REQUIRE(schema.is_object());
		CHECK(schema.value("type", std::string()) == "object");

		if (schema.contains("properties")) {
			CHECK(schema.at("properties").is_object());
			for (const auto &[property, described] : schema.at("properties").items()) {
				INFO("property " << property);
				CHECK(described.is_object());
			}
		}

		if (!schema.contains("required")) {
			continue;
		}

		// A `required` naming something `properties` does not declare is a call
		// a strict client cannot construct and a lax one gets wrong.
		REQUIRE(schema.at("required").is_array());
		for (const json &required : schema.at("required")) {
			REQUIRE(required.is_string());
			const std::string field = required.get<std::string>();
			INFO("required " << field);
			REQUIRE(schema.contains("properties"));
			CHECK(schema.at("properties").contains(field));
		}
	}
}

TEST_CASE("every resource has an address and reads", "[mcpbridge]") {
	Universe universe;
	Surface surface("test", "a suite");
	Fill(surface, universe);

	const json listed = Ask(surface, "resources/list");
	REQUIRE(listed["result"].contains("resources"));
	REQUIRE(listed["result"]["resources"].size() == surface.Readable().size());

	// The three that are compiled in or read out of this process are there
	// whatever directory the suite runs from; the file-backed ones are not, and
	// that is what makes them conditional.
	REQUIRE(surface.Readable().size() >= 3);

	for (const json &resource : listed["result"]["resources"]) {
		const std::string uri = resource.at("uri").get<std::string>();
		INFO("resource " << uri);

		CHECK(uri.rfind("atomic://", 0) == 0);
		CHECK_FALSE(resource.at("name").get<std::string>().empty());
		CHECK_FALSE(resource.at("description").get<std::string>().empty());
		CHECK_FALSE(resource.at("mimeType").get<std::string>().empty());

		const json read = Ask(surface, "resources/read", json{{"uri", uri}});
		REQUIRE(read.contains("result"));
		REQUIRE(read["result"]["contents"].size() == 1);
		CHECK_FALSE(read["result"]["contents"][0].at("text").get<std::string>().empty());
	}
}

TEST_CASE("every prompt renders", "[mcpbridge]") {
	Universe universe;
	Surface surface("test", "a suite");
	Fill(surface, universe);

	const json listed = Ask(surface, "prompts/list");
	REQUIRE(listed["result"].contains("prompts"));
	REQUIRE(listed["result"]["prompts"].size() == surface.Prompted().size());

	// One is written in C++ and has no file, so it is there whatever directory
	// the suite runs from.
	REQUIRE(surface.Prompted().size() >= 1);

	for (const json &prompt : listed["result"]["prompts"]) {
		const std::string name = prompt.at("name").get<std::string>();
		INFO("prompt " << name);

		CHECK_FALSE(prompt.at("description").get<std::string>().empty());
		REQUIRE(prompt.at("arguments").is_array());

		const json got = Ask(surface, "prompts/get", json{{"name", name}});
		REQUIRE(got.contains("result"));
		REQUIRE(got["result"]["messages"].size() == 1);
		CHECK(got["result"]["messages"][0].at("role") == "user");
		CHECK(got["result"]["messages"][0]["content"].at("text").get<std::string>().size() > 40);
	}
}

// --- the module graph ----------------------------------------------------------

TEST_CASE("the layer table is the one the architecture check enforces", "[mcpbridge]") {
	// Compiled in from `expected_graph.json` at configure time, so a suite that
	// finds it empty is a build whose embedding broke rather than a graph that
	// shrank.
	REQUIRE(Graph::Modules().size() > 20);
	REQUIRE(Graph::Programs().size() > 3);
	REQUIRE(Graph::Height() >= 10);

	// **Heights are asserted against each other rather than against numbers.**
	// The stack grows: a module added below `control` moves it, and a suite
	// that named 13 would fail on a change that broke nothing. What has to stay
	// true is the ordering the dependency rule is about.
	const GraphModule *control = Graph::Find("control");
	const GraphModule *world = Graph::Find("world");
	const GraphModule *ecs = Graph::Find("ecs");
	REQUIRE(control != nullptr);
	REQUIRE(world != nullptr);
	REQUIRE(ecs != nullptr);
	CHECK(control->Tier == "shared");
	CHECK(control->Layer > world->Layer);
	CHECK(world->Layer > ecs->Layer);
	CHECK_FALSE(control->Program);

	// Every module this one is expected to link is a module, and every edge
	// runs downward or is named lateral - which is the whole rule, checked here
	// against the same data the CMake check reads.
	for (const GraphModule &row : Graph::Modules()) {
		INFO("module " << row.Name);
		for (const std::string &link : row.Links) {
			INFO("links " << link);
			const LinkVerdict verdict = Graph::MayLink(row.Name, link);
			CHECK(verdict.Allowed);
			CHECK_FALSE(verdict.Reason.empty());
		}
	}
}

TEST_CASE("module_may_link refuses an edge for the reason the build would", "[mcpbridge]") {
	Universe universe;
	Surface surface("test", "a suite");
	Fill(surface, universe);

	const auto verdict = [&surface](const char *from, const char *to) {
		const json reply =
			Ask(surface,
				"tools/call",
				json{{"name", "module_may_link"}, {"arguments", json{{"from", from}, {"to", to}}}});
		REQUIRE(reply["result"].at("isError") == false);
		return json::parse(reply["result"]["content"][0].at("text").get<std::string>());
	};

	// Downward, and it already exists.
	const json down = verdict("control", "parallel");
	CHECK(down.at("allowed") == true);
	CHECK(down.at("existing") == true);

	// Upward. `ecs` is L3 and `control` is L13.
	const json up = verdict("ecs", "control");
	CHECK(up.at("allowed") == false);
	CHECK(up.at("reason").get<std::string>().find("none above it") != std::string::npos);

	// Sideways and not named. `assets` and `physics` sit at the same height and
	// neither names the other, which is the ordinary case: siblings do not
	// include each other.
	REQUIRE(Graph::Find("assets")->Layer == Graph::Find("physics")->Layer);
	const json sideways = verdict("assets", "physics");
	CHECK(sideways.at("allowed") == false);
	CHECK(sideways.at("reason").get<std::string>().find("lateral") != std::string::npos);

	// Sideways and named, which is how the deliberate ones are allowed: `ui`
	// implements `render::FrameOverlayHook` and says so in its `lateral` array.
	REQUIRE(Graph::Find("ui")->Layer == Graph::Find("render")->Layer);
	const json named = verdict("ui", "render");
	CHECK(named.at("allowed") == true);

	// A `shared` module may not link a `client` one, whatever the layers say -
	// and the tier check is the one that refuses first. `control` does not link
	// `render`, so there is no escape to excuse it.
	const json tier = verdict("control", "render");
	CHECK(tier.at("allowed") == false);
	CHECK(tier.at("reason").get<std::string>().find("ALLOW_TIER_ESCAPE") != std::string::npos);

	// A module may not link the program band, which is any row with no layer -
	// the programs, the tools, and each program's own library. `cdn` is shared
	// tier, so this is the layer rule refusing and not the tier table.
	const json band = verdict("control", "cdn");
	CHECK(band.at("allowed") == false);
	CHECK(band.at("reason").get<std::string>().find("program band") != std::string::npos);
}

// --- the failure modes ---------------------------------------------------------

TEST_CASE("a malformed message is a parse error against the null id", "[mcpbridge]") {
	Surface surface("test", "a suite");

	const json reply = json::parse(surface.Answer("{ not json"));
	REQUIRE(reply.contains("error"));
	CHECK(reply["error"].at("code") == -32700);
	CHECK(reply.at("id").is_null());

	// Valid JSON that is not a request. There is nothing to dispatch on, and a
	// server that guessed would answer a message nobody sent.
	const json shaped = json::parse(surface.Answer("[1,2,3]"));
	REQUIRE(shaped.contains("error"));
	CHECK(shaped["error"].at("code") == -32600);
}

TEST_CASE("an unknown method is a protocol error and an unknown tool is not", "[mcpbridge]") {
	Surface surface("test", "a suite");

	const json method = Ask(surface, "nonsense/list");
	REQUIRE(method.contains("error"));
	CHECK(method["error"].at("code") == -32601);

	// The line MCP draws: a method that does not exist is the transport being
	// wrong, and a tool that does not exist is something a model reads and
	// reacts to.
	const json tool = Ask(surface, "tools/call", json{{"name", "no_such_tool"}});
	REQUIRE(tool.contains("result"));
	CHECK(tool["result"].at("isError") == true);
}

TEST_CASE("a tool that throws is a failed tool and not a failed program", "[mcpbridge]") {
	Surface surface("test", "a suite");
	surface.Add(
		Tool{
			"explode",
			"Throws, so the suite can prove an escaping exception does not take the frame loop with it.",
			nullptr,
			[](const json &, std::string &) -> json { throw std::runtime_error("boom"); },
		}
	);

	const json reply = Ask(surface, "tools/call", json{{"name", "explode"}});
	REQUIRE(reply.contains("result"));
	CHECK(reply["result"].at("isError") == true);
	CHECK(reply["result"]["content"][0].at("text").get<std::string>().find("boom") != std::string::npos);
}

TEST_CASE("a resource that cannot be read is a protocol error", "[mcpbridge]") {
	Surface surface("test", "a suite");
	surface.AddResource(
		Resource{
			"atomic://test/missing",
			"missing",
			"A resource whose reader always refuses, so the suite can prove the refusal is not "
			"reported as an empty document.",
			"text/plain",
			[](std::string &failure) {
				failure = "there is nothing there";
				return std::string();
			},
		}
	);

	const json refused = Ask(surface, "resources/read", json{{"uri", "atomic://test/missing"}});
	REQUIRE(refused.contains("error"));
	CHECK(refused["error"].at("code") == -32002);

	const json unknown = Ask(surface, "resources/read", json{{"uri", "atomic://test/nowhere"}});
	REQUIRE(unknown.contains("error"));
	CHECK(unknown["error"].at("code") == -32002);

	// Every address this module serves is a fixed one, so the templates list is
	// declared and empty rather than absent - a client that asks is entitled to
	// an answer.
	const json templates = Ask(surface, "resources/templates/list");
	REQUIRE(templates.contains("result"));
	CHECK(templates["result"].at("resourceTemplates").empty());
}

TEST_CASE("the handshake declares a capability only when there is one", "[mcpbridge]") {
	Surface bare("test", "a suite");
	const json empty = Ask(bare, "initialize");
	REQUIRE(empty["result"].contains("capabilities"));
	CHECK(empty["result"]["capabilities"].contains("tools"));
	CHECK_FALSE(empty["result"]["capabilities"].contains("resources"));
	CHECK_FALSE(empty["result"]["capabilities"].contains("prompts"));

	Universe universe;
	Surface full("test", "a suite");
	Fill(full, universe);

	const json opened = Ask(full, "initialize");
	CHECK(opened["result"]["capabilities"].contains("resources"));
	CHECK(opened["result"]["capabilities"].contains("prompts"));

	// A notification is answered with nothing at all, which is the one part of
	// the handshake a server can get wrong without a client complaining until
	// it hangs.
	CHECK(full.Answer(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());

	// `ping` is the other half of a client's liveness check.
	const json pinged = Ask(full, "ping");
	REQUIRE(pinged.contains("result"));
	CHECK(pinged["result"].empty());
}
