// The byte pump, driven through the real binary.
//
// **`Schemas.cpp` proves the protocol and this proves it survives the pipe.**
// The two failures the bridge can have are invisible to a test of `Surface`: a
// reply that never leaves because stdout was block-buffered, and a reply lost
// because closing stdin closed the whole socket instead of half of it. Both were
// real - the second is written up in `app/main.cpp` - and both look like a
// client that hangs rather than like an error.
//
// **A file for stdin rather than a pipe**, which is what keeps this suite off
// `fork` and `CreateProcess` and platform sources. `parallel::Capture` runs a
// program and keeps what it printed, leaving stdin inherited; so the suite
// points its own stdin at a scripted conversation, and the child reads it.
// JSON-RPC carries an id per request, so a pipelined script is answered in
// order and every reply is identifiable.
//
// **The surface is pumped on a second thread**, because `Capture` blocks until
// the child exits and the child is waiting for answers. That is safe here and
// nowhere else: the tools registered below touch no world, and `Universe::Enter`
// is what makes the single-threaded rule a rule.

#include <engine/control/Server.hpp>
#include <engine/control/Surface.hpp>
#include <engine/core/Paths.hpp>
#include <engine/parallel/Capture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("tools.mcpbridge.bridge")
TEST_DEPENDS("engine.control.surface")

using engine::control::Resource;
using engine::control::Surface;
using engine::control::Tool;
using nlohmann::json;

namespace {
	// Where the bridge is staged, from where the suite is.
	//
	// `<stage>/tests/test_mcpbridge` and `<stage>/tools/mcpbridge`, which is the
	// same relationship the editor's control panel spells out when it prints the
	// command a client should run.
	std::filesystem::path BridgePath() {
		return engine::core::Paths::Base().parent_path() / "tools" /
			   engine::core::Paths::Program("mcpbridge");
	}

	std::string Request(int id, const std::string &method, const json &parameters = json::object()) {
		return json{
				   {"jsonrpc", "2.0"},
				   {"id", id},
				   {"method", method},
				   {"params", parameters},
			   }
				   .dump() +
			   "\n";
	}

	// Runs the whole conversation and returns every JSON-RPC reply, in order.
	//
	// **Non-JSON lines are dropped rather than asserted about.** `Capture` merges
	// stdout and stderr, and the bridge writes "connected" and "closed" to
	// stderr on purpose - stdout is the protocol, which is the one thing that
	// file may never carry a log line on.
	std::vector<json> Converse(Surface &surface, const std::string &script, int &exitCode) {
		engine::control::Server listener;
		REQUIRE(listener.Start(0));
		REQUIRE(listener.IsRunning());

		const std::filesystem::path input =
			std::filesystem::temp_directory_path() /
			("atomic-mcpbridge-" + std::to_string(listener.Port()) + ".jsonl");
		{
			std::ofstream out(input, std::ios::binary | std::ios::trunc);
			REQUIRE(out.good());
			out << script;
		}

		// The suite's own stdin, for the child to inherit. Catch2 reads none of
		// its own, so leaving it pointed at a file at EOF costs nothing.
		REQUIRE(std::freopen(input.string().c_str(), "rb", stdin) != nullptr);

		std::atomic<bool> pumping{true};
		std::thread pump([&] {
			while (pumping.load()) {
				listener.Pump([&surface](const std::string &line) { return surface.Answer(line); });
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});

		const engine::parallel::CaptureResult ran = engine::parallel::Capture({
			BridgePath().string(),
			"--port",
			std::to_string(listener.Port()),
		});

		pumping.store(false);
		pump.join();
		listener.Stop();
		std::filesystem::remove(input);

		REQUIRE(ran.Started);
		exitCode = ran.ExitCode;

		std::vector<json> replies;
		std::istringstream lines(ran.Output);
		std::string line;
		while (std::getline(lines, line)) {
			json parsed = json::parse(line, nullptr, false);
			if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("jsonrpc")) {
				replies.push_back(std::move(parsed));
			}
		}

		INFO(ran.Output);
		return replies;
	}

	// A surface that touches no world, so it can be answered from the pump
	// thread.
	void Fill(Surface &surface) {
		surface.AddArchitectureTools();

		surface.Add(
			Tool{
				"echo",
				"Returns whatever it was given, so the suite can prove an argument survives the pipe "
				"unchanged in both directions.",
				[] {
					return json{
						{"type", "object"},
						{"properties", json{{"text", json{{"type", "string"}}}}},
						{"required", json::array({"text"})},
					};
				},
				[](const json &arguments, std::string &failure) -> json {
					if (!arguments.contains("text")) {
						failure = "echo needs `text`";
						return nullptr;
					}
					return json{{"text", arguments.at("text")}};
				},
			}
		);

		surface.Add(
			Tool{
				"explode",
				"Throws, so the suite can prove an escaping exception arrives as a tool error rather "
				"than as a closed connection.",
				nullptr,
				[](const json &, std::string &) -> json { throw std::runtime_error("boom"); },
			}
		);
	}

	const json *Reply(const std::vector<json> &replies, int id) {
		for (const json &reply : replies) {
			if (reply.contains("id") && reply["id"].is_number() && reply["id"].get<int>() == id) {
				return &reply;
			}
		}
		return nullptr;
	}
}

TEST_CASE("the bridge carries a whole conversation between stdio and the port", "[mcpbridge]") {
	Surface surface("bridge-suite", "the suite's own surface");
	Fill(surface);

	std::string script;
	script += Request(1, "initialize");
	// A notification, which must produce no reply at all. It sits in the middle
	// on purpose: a server that answered one would put a message on the wire
	// nobody is reading and every later reply would be read against the wrong
	// request.
	script += R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
			  "\n";
	script += Request(2, "ping");
	script += Request(3, "tools/list");
	script +=
		Request(4, "tools/call", json{{"name", "echo"}, {"arguments", json{{"text", "over the pipe"}}}});
	script += Request(5, "tools/call", json{{"name", "explode"}});
	script += Request(6, "tools/call", json{{"name", "no_such_tool"}});
	script += Request(7, "resources/list");
	script += Request(8, "prompts/list");
	script += Request(9, "nonsense/list");
	script += "{ not json\n";
	script += Request(
		11,
		"tools/call",
		json{{"name", "module_may_link"}, {"arguments", json{{"from", "ecs"}, {"to", "control"}}}}
	);

	int exitCode = -1;
	const std::vector<json> replies = Converse(surface, script, exitCode);

	// It exits when either side closes, which is what makes a client's own
	// lifecycle management work.
	CHECK(exitCode == 0);

	// Eleven things that expect an answer, plus one notification that must not
	// get one. A server that answered the notification would put a twelfth
	// message on the wire and every later reply would be read against the wrong
	// request, which is why the count is asserted rather than only the contents.
	REQUIRE(replies.size() == 11);

	const json *opened = Reply(replies, 1);
	REQUIRE(opened != nullptr);
	CHECK((*opened)["result"].at("protocolVersion") == "2025-06-18");
	CHECK((*opened)["result"]["serverInfo"].at("name") == "bridge-suite");

	REQUIRE(Reply(replies, 2) != nullptr);
	CHECK((*Reply(replies, 2))["result"].empty());

	const json *listed = Reply(replies, 3);
	REQUIRE(listed != nullptr);
	CHECK((*listed)["result"]["tools"].size() == surface.Count());

	// The argument survived the pipe unchanged, which is the whole contract.
	const json *echoed = Reply(replies, 4);
	REQUIRE(echoed != nullptr);
	CHECK((*echoed)["result"].at("isError") == false);
	CHECK(
		(*echoed)["result"]["content"][0].at("text").get<std::string>().find("over the pipe") !=
		std::string::npos
	);

	const json *thrown = Reply(replies, 5);
	REQUIRE(thrown != nullptr);
	CHECK((*thrown)["result"].at("isError") == true);
	CHECK((*thrown)["result"]["content"][0].at("text").get<std::string>().find("boom") != std::string::npos);

	const json *missing = Reply(replies, 6);
	REQUIRE(missing != nullptr);
	CHECK((*missing)["result"].at("isError") == true);

	// No architecture tools were registered as resources or prompts on this
	// surface, so both tables are empty and both still answer.
	REQUIRE(Reply(replies, 7) != nullptr);
	CHECK((*Reply(replies, 7))["result"].at("resources").empty());
	REQUIRE(Reply(replies, 8) != nullptr);
	CHECK((*Reply(replies, 8))["result"].at("prompts").empty());

	const json *unknown = Reply(replies, 9);
	REQUIRE(unknown != nullptr);
	CHECK((*unknown)["error"].at("code") == -32601);

	// The malformed line is answered against the null id JSON-RPC reserves for
	// it, which is why it is looked up by shape rather than by number.
	bool sawParseError = false;
	for (const json &reply : replies) {
		if (reply.contains("error") && reply["error"].at("code") == -32700) {
			sawParseError = true;
			CHECK(reply.at("id").is_null());
		}
	}
	CHECK(sawParseError);

	// And the conversation carried on afterwards, which is the part that makes
	// a parse error recoverable rather than the end of the session.
	const json *verdict = Reply(replies, 11);
	REQUIRE(verdict != nullptr);
	const json body = json::parse((*verdict)["result"]["content"][0].at("text").get<std::string>());
	CHECK(body.at("allowed") == false);
}

TEST_CASE("a reply larger than one read still arrives whole", "[mcpbridge]") {
	Surface surface("bridge-suite", "the suite's own surface");
	Fill(surface);

	// The pump copies 64 KB at a time and flushes stdout on every write. A
	// payload several times that is what proves a reply is reassembled rather
	// than truncated at a buffer boundary - and a world tree is exactly this
	// shape, which is why the pump has no line buffer of its own.
	const std::string wide(400000, 'x');

	std::string script =
		Request(1, "tools/call", json{{"name", "echo"}, {"arguments", json{{"text", wide}}}});

	int exitCode = -1;
	const std::vector<json> replies = Converse(surface, script, exitCode);

	CHECK(exitCode == 0);
	REQUIRE(replies.size() == 1);

	const json body = json::parse(replies[0]["result"]["content"][0].at("text").get<std::string>());
	CHECK(body.at("text").get<std::string>().size() == wide.size());
}

TEST_CASE("the bridge says which command would have opened the port", "[mcpbridge]") {
	// Nothing is listening on this one: `Start(0)` bound a port, and stopping
	// releases it. The ordinary case rather than a fault - a client launches the
	// bridge when it starts and the editor is started by a person - so the
	// message has to name the command that fixes it.
	uint16_t closed = 0;
	{
		engine::control::Server listener;
		REQUIRE(listener.Start(0));
		closed = listener.Port();
	}

	const engine::parallel::CaptureResult ran = engine::parallel::Capture({
		BridgePath().string(),
		"--port",
		std::to_string(closed),
	});

	REQUIRE(ran.Started);
	CHECK(ran.ExitCode == 1);
	INFO(ran.Output);
	CHECK(ran.Output.find("could not reach an editor") != std::string::npos);
	CHECK(ran.Output.find("--mcp-port") != std::string::npos);
}

TEST_CASE("the bridge dials the port the engine's own constant names", "[mcpbridge]") {
	// The help is generated from `DEFAULT_PORT` rather than written, which is
	// what makes `.mcp.json`, the `just mcp` recipe and this program agree. The
	// CMake check next to this suite covers the two that are not C++; this
	// covers the one that is.
	const engine::parallel::CaptureResult ran = engine::parallel::Capture({BridgePath().string(), "--help"});

	REQUIRE(ran.Started);
	CHECK(ran.ExitCode == 0);
	INFO(ran.Output);
	CHECK(ran.Output.find(std::to_string(engine::control::DEFAULT_PORT)) != std::string::npos);
}
