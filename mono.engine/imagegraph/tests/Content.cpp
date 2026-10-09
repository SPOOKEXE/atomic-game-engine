#include <engine/imagegraph/Content.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.content")
using namespace engine::imagegraph;

TEST_CASE(
	"verified route caches preserve last good graphs and isolate identical asset names",
	"[imagegraph][content]"
) {
	Content firstSession, secondSession;
	Diagnostic diagnostic;
	Document document{
		.Nodes = {{"source", Source{"__imagegraph_sources/a.atex"}, {}, {}}},
		.Outputs = {{"image", "source"}},
		.Parameters = {},
		.Bindings = {}
	};
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	REQUIRE(firstSession.Admit("panel.aimagegraph", "signed-root-a", encoded, diagnostic));
	const auto revision = firstSession.Find("panel.aimagegraph")->Revision;
	CHECK_FALSE(secondSession.Find("panel.aimagegraph"));
	CHECK_FALSE(firstSession.Admit("panel.aimagegraph", "signed-root-a", encoded, diagnostic));
	CHECK(firstSession.Find("panel.aimagegraph")->Revision == revision);
	CHECK_FALSE(firstSession.Admit("panel.aimagegraph", "signed-root-b", "{}", diagnostic));
	CHECK(firstSession.Find("panel.aimagegraph")->Root == "signed-root-a");
	document.Nodes[0].Value = Source{"unsigned.png"};
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK_FALSE(firstSession.Admit("panel.aimagegraph", "signed-root-b", encoded, diagnostic));
	CHECK(firstSession.Find("panel.aimagegraph")->Revision == revision);
	document.Nodes[0].Value = Solid{2, 2, {0, 255, 0, 255}};
	REQUIRE(Write(document, encoded, diagnostic));
	REQUIRE(secondSession.Admit("panel.aimagegraph", "signed-root-b", encoded, diagnostic));
	CHECK(firstSession.Find("panel.aimagegraph")->Root != secondSession.Find("panel.aimagegraph")->Root);
	REQUIRE(firstSession.Admit("panel.aimagegraph", "signed-root-b", encoded, diagnostic));
	CHECK(firstSession.Find("panel.aimagegraph")->Revision > revision);
}

TEST_CASE("resolved source overrides demand exact signed texture names", "[imagegraph][content]") {
	Document document{
		.Nodes = {{"source", Source{"default.atex"}, {}, {}}},
		.Outputs = {{"image", "source"}},
		.Parameters = {{"texture", std::string("default.atex")}},
		.Bindings = {{"source", "path", "texture"}}
	};
	Diagnostic diagnostic;
	Document resolved;
	const InputOverride replacement{"texture", std::string("__imagegraph_sources/other.atex")};
	REQUIRE(ResolveInputs(document, std::span(&replacement, 1), resolved, diagnostic));
	std::vector<std::string> sources;
	REQUIRE(RuntimeSources(resolved, sources, diagnostic));
	CHECK(sources == std::vector<std::string>{"__imagegraph_sources/other.atex"});
	resolved.Nodes[0].Value = Source{"imagegraph://recursive.aimagegraph#image"};
	CHECK_FALSE(RuntimeSources(resolved, sources, diagnostic));
	CHECK(sources == std::vector<std::string>{"__imagegraph_sources/other.atex"});
}

TEST_CASE("signed graph record admission bounds small documents as well as bytes", "[imagegraph][content]") {
	Content content;
	Diagnostic diagnostic;
	Document document{
		.Nodes = {{"solid", Solid{}, {}, {}}},
		.Outputs = {{"image", "solid"}},
		.Parameters = {},
		.Bindings = {}
	};
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	for (size_t i = 0; i < Content::MaximumRecords; ++i)
		REQUIRE(content.Admit("graph-" + std::to_string(i) + ".aimagegraph", "root", encoded, diagnostic));
	CHECK_FALSE(content.Admit("excess.aimagegraph", "root", encoded, diagnostic));
	CHECK_FALSE(content.Find("excess.aimagegraph"));
	REQUIRE(content.Admit("graph-0.aimagegraph", "new-root", encoded, diagnostic));
}

TEST_CASE(
	"runtime texture demand follows selected output cones without disconnected sources",
	"[imagegraph][content]"
) {
	Document document{
		.Nodes =
			{{"solid", Solid{}, {}, {}},
			 {"a", Source{"a.atex"}, {}, {}},
			 {"b", Source{"b.atex"}, {}, {}},
			 {"unused", Source{"unused.atex"}, {}, {}},
			 {"resize", Resize{2, 2, Sampling::Nearest}, {"a"}, {}}},
		.Outputs = {{"plain", "solid"}, {"first", "resize"}, {"second", "b"}},
		.Parameters = {},
		.Bindings = {}
	};
	Diagnostic diagnostic;
	std::vector<std::string> sources;
	const std::string_view plain = "plain";
	REQUIRE(RuntimeSources(document, sources, diagnostic, std::span(&plain, 1)));
	CHECK(sources.empty());
	const std::string_view both[] = {"first", "second"};
	REQUIRE(RuntimeSources(document, sources, diagnostic, both));
	CHECK(sources == std::vector<std::string>{"a.atex", "b.atex"});
	const std::string_view invalid = "unknown";
	CHECK_FALSE(RuntimeSources(document, sources, diagnostic, std::span(&invalid, 1)));
	CHECK(sources == std::vector<std::string>{"a.atex", "b.atex"});
	REQUIRE(RuntimeSources(document, sources, diagnostic));
	CHECK(sources == std::vector<std::string>{"a.atex", "b.atex", "unused.atex"});
}

TEST_CASE(
	"local editable sources bind runtime controls but cannot enter signed defaults", "[imagegraph][content]"
) {
	Document document{
		.Nodes = {{"source", Source{"default.atex"}, {}, {}}},
		.Outputs = {{"image", "source"}},
		.Parameters = {{"texture", std::string("default.atex")}},
		.Bindings = {{"source", "path", "texture"}}
	};
	Diagnostic diagnostic;
	Content content;
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	REQUIRE(content.Admit("panel.aimagegraph", "signed-root", encoded, diagnostic));
	const auto revision = content.Find("panel.aimagegraph")->Revision;
	const InputOverride input{"texture", std::string("editable-image://42")};
	Document resolved;
	REQUIRE(ResolveInputs(document, std::span(&input, 1), resolved, diagnostic));
	std::vector<std::string> sources;
	REQUIRE(RuntimeSources(resolved, sources, diagnostic));
	CHECK(sources == std::vector<std::string>{"editable-image://42"});
	document.Parameters[0].Default = std::string("editable-image://42");
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK_FALSE(content.Admit("panel.aimagegraph", "changed-root", encoded, diagnostic));
	CHECK(content.Find("panel.aimagegraph")->Revision == revision);
	document.Parameters.clear();
	document.Bindings.clear();
	document.Nodes[0].Value = Source{"editable-image://42"};
	REQUIRE(Write(document, encoded, diagnostic));
	CHECK_FALSE(content.Admit("panel.aimagegraph", "changed-root", encoded, diagnostic));
	CHECK(content.Find("panel.aimagegraph")->Root == "signed-root");
}
