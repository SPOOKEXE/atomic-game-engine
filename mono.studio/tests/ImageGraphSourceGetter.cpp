#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <studio/ImageGraph.hpp>

TEST_SUITE_ID("studio.imagegraph_source_getter")
using namespace engine::imagegraph;
TEST_CASE(
	"Inspector preserves proven raw source getter values without widening legacy properties",
	"[studio][imagegraph][source_getter]"
) {
	Document document;
	document.FormatVersion = 8;
	document.Nodes = {
		{"window", "pc.audio_window", "", {}, {}, {}},
		{"vector", "pc.vector2", "", {}, {}, {}},
		{"legacy",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{4}}, {"height", int64_t{4}}, {"colour", Colour{255, 0, 0, 255}}},
		 {}}
	};
	Diagnostic diagnostic;
	REQUIRE(studio::SetImageGraphValue(document, "window", "width", 6.5, diagnostic));
	REQUIRE(studio::SetImageGraphValue(document, "vector", "integer", .25, diagnostic));
	CHECK(document.Nodes[0].Values.front().Data == Value{6.5});
	CHECK(document.Nodes[1].Values.front().Data == Value{.25});
	const auto sentinel = document;
	CHECK_FALSE(studio::SetImageGraphValue(document, "legacy", "width", 6.5, diagnostic));
	CHECK_FALSE(studio::SetImageGraphValue(document, "legacy", "empty", .25, diagnostic));
	CHECK_FALSE(
		studio::SetImageGraphValue(
			document, "window", "width", std::numeric_limits<double>::infinity(), diagnostic
		)
	);
	CHECK(document == sentinel);
	document.Outputs = {{"out", "vector", "x"}};
	Plan plan;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	INFO(document.FormatVersion);
	REQUIRE(compiled == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
}
