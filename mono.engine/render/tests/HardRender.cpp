// CPU contracts for authored trace settings and unavailable history inputs.

#include "HardRender.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.hardrender")

using engine::core::Name;
using engine::graph::Node;
using engine::render::IsTraceNode;
using engine::render::TraceOptionsFor;

TEST_CASE("hard render options preserve authored trace settings", "[render][hard-render]") {
	Node illumination;
	illumination.Kind = Name("global-illumination");
	illumination.Parameters = {
		{Name("rays-per-pixel"), "7"},
		{Name("max-distance"), "42"},
	};
	const auto gi = TraceOptionsFor(illumination, false);
	CHECK(gi.Trace.x == 7.0f);
	CHECK(gi.Trace.y == 42.0f);
	CHECK(gi.Inputs.x == 0u);

	Node pathtrace;
	pathtrace.Kind = Name("pathtrace");
	pathtrace.Parameters = {
		{Name("max-bounces"), "5"},
		{Name("samples-per-frame"), "3"},
	};
	const auto trace = TraceOptionsFor(pathtrace, true);
	CHECK(trace.Trace.x == 5.0f);
	CHECK(trace.Trace.w == 3.0f);
	CHECK(trace.Inputs.x == 1u);
}

TEST_CASE("only trace nodes allocate the trace uniform slot", "[render][hard-render]") {
	CHECK(IsTraceNode(Name("global-illumination")));
	CHECK(IsTraceNode(Name("raytrace")));
	CHECK(IsTraceNode(Name("pathtrace")));
	CHECK_FALSE(IsTraceNode(Name("dispatch")));
}
