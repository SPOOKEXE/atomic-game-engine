#include "Fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <scriptcheck/Check.hpp>
#include <scriptcheck/Server.hpp>
#include <sstream>

TEST_SUITE_ID("tools.scriptcheck.server")
TEST_TIER("fast")

namespace {
	using Json = nlohmann::json;

	std::string Frame(const Json &message) {
		std::ostringstream out;
		scriptcheck::WriteMessage(out, message.dump());
		return out.str();
	}

	std::vector<Json> Replies(const std::string &stream) {
		std::istringstream in(stream);
		std::vector<Json> out;
		while (const auto message = scriptcheck::ReadMessage(in)) {
			out.push_back(Json::parse(*message));
		}
		return out;
	}
}

TEST_CASE("a session over framed stdio checks, rechecks, hovers and exits cleanly") {
	const fixture::Plugin plugin;
	scriptcheck::Checker checker;
	std::vector<scriptcheck::Diagnostic> failures;
	REQUIRE(checker.Load(fixture::DECLARATIONS, failures));

	const std::string uri = scriptcheck::UriOfPath(plugin.At("plugin/src/main.luau"));
	const std::string broken =
		"--!strict\nlocal Add = require(\"./core/Add\")\nlocal total: number = Add.add(\"one\", 2)\n";
	const std::string fixed =
		"--!strict\nlocal Add = require(\"./core/Add\")\nlocal total: number = Add.add(1, 2)\n";

	std::string input;
	input += Frame(
		{{"jsonrpc", "2.0"},
		 {"id", 1},
		 {"method", "initialize"},
		 {"params", {{"rootUri", scriptcheck::UriOfPath(plugin.Folder)}}}}
	);
	input += Frame({{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", Json::object()}});
	input += Frame(
		{{"jsonrpc", "2.0"},
		 {"method", "textDocument/didOpen"},
		 {"params",
		  {{"textDocument", {{"uri", uri}, {"languageId", "luau"}, {"version", 1}, {"text", broken}}}}}}
	);
	input += Frame(
		{{"jsonrpc", "2.0"},
		 {"method", "textDocument/didChange"},
		 {"params",
		  {{"textDocument", {{"uri", uri}, {"version", 2}}}, {"contentChanges", {{{"text", fixed}}}}}}}
	);
	input += Frame(
		{{"jsonrpc", "2.0"},
		 {"id", 2},
		 {"method", "textDocument/hover"},
		 {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 2}, {"character", 7}}}}}}
	);
	input += Frame(
		{{"jsonrpc", "2.0"},
		 {"id", 3},
		 {"method", "textDocument/completion"},
		 {"params", {{"textDocument", {{"uri", uri}}}, {"position", {{"line", 2}, {"character", 26}}}}}}
	);
	input +=
		Frame({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "workspace/symbol"}, {"params", Json::object()}});
	input += Frame({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "shutdown"}});
	input += Frame({{"jsonrpc", "2.0"}, {"method", "exit"}});

	std::istringstream in(input);
	std::ostringstream out;
	scriptcheck::Server server(checker);
	CHECK(server.Run(in, out) == 0);
	CHECK(server.Done());

	const std::vector<Json> replies = Replies(out.str());
	REQUIRE(replies.size() == 7);

	CHECK(replies[0]["id"] == 1);
	CHECK(replies[0]["result"]["capabilities"]["hoverProvider"] == true);

	CHECK(replies[1]["method"] == "textDocument/publishDiagnostics");
	CHECK(replies[1]["params"]["uri"] == uri);
	REQUIRE(replies[1]["params"]["diagnostics"].size() == 1);
	CHECK(replies[1]["params"]["diagnostics"][0]["range"]["start"]["line"] == 2);

	CHECK(replies[2]["params"]["diagnostics"].empty());

	CHECK(replies[3]["id"] == 2);
	CHECK(replies[3]["result"]["contents"]["value"].get<std::string>().find("number") != std::string::npos);

	CHECK(replies[4]["id"] == 3);
	bool offersAdd = false;
	for (const Json &item : replies[4]["result"]["items"]) {
		offersAdd = offersAdd || item["label"] == "add";
	}
	CHECK(offersAdd);

	CHECK(replies[5]["error"]["code"] == -32601);
	CHECK(replies[6]["id"] == 5);
}

TEST_CASE("file URIs decode and encode") {
	CHECK(scriptcheck::PathOfUri("file:///a%20b/c.luau") == "/a b/c.luau");
	CHECK(scriptcheck::PathOfUri("file:///C:/x/y.luau") == "C:/x/y.luau");
	CHECK(scriptcheck::UriOfPath("/a b/c.luau") == "file:///a%20b/c.luau");
}
