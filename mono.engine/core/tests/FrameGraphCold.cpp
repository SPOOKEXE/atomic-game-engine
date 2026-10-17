// This suite runs in its own process so no control API can warm the graph first.
#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.core.framegraphcold")
TEST_DEPENDS("engine.core.heapprofile")

using engine::core::FrameGraph;
using engine::core::HeapProfile;
using engine::core::ProfileCategory;

namespace {
	struct Collecting {
		Collecting() {
			FrameGraph::SetEnabled(true);
		}
		~Collecting() {
			FrameGraph::SetEnabled(false);
		}
	};
}

TEST_CASE("cold frame graph scopes and reports allocate nothing", "[framegraph][heap]") {
	if (!HeapProfile::IsCompiledIn()) {
		SKIP("allocator hooks are not compiled in");
	}

	const auto before = HeapProfile::Totals();
	{
		FrameGraph::Scope scope("cold scope", ProfileCategory::Engine);
		FrameGraph::CopiedScope copied("fallback", "cold copied scope", ProfileCategory::Engine);
		FrameGraph::CopiedScope fallback("fallback", "", ProfileCategory::Engine);
		FrameGraph::ReportedScope reported("cold reported scope", ProfileCategory::Engine, 1.0f);
		FrameGraph::ReportedScope invalid("cold invalid scope", ProfileCategory::Engine, -1.0f);
		FrameGraph::Report("cold report", ProfileCategory::Engine, 1.0f);
		FrameGraph::ReportNamed("fallback", "cold named report", ProfileCategory::Engine, 1.0f);
		FrameGraph::ReportNamed("fallback", "", ProfileCategory::Engine, 1.0f);
		FrameGraph::RecordNamed("fallback", "cold named recording", ProfileCategory::Engine, 1.0f);
		FrameGraph::RecordNamed("fallback", "", ProfileCategory::Engine, 1.0f);
		FrameGraph::RecordNamed(
			"fallback", "invalid recording", ProfileCategory::Engine, std::numeric_limits<float>::quiet_NaN()
		);
	}
	const auto after = HeapProfile::Totals();

	CHECK(after.TotalBlocks == before.TotalBlocks);
	CHECK(after.TotalBytes == before.TotalBytes);
	CHECK(after.LiveBlocks == before.LiveBlocks);
	CHECK(after.LiveBytes == before.LiveBytes);

	// Control calls must still initialise collection after cold callbacks were ignored.
	Collecting collecting;
	FrameGraph::BeginFrame();
	{
		FrameGraph::Scope scope("active scope", ProfileCategory::Engine);
		FrameGraph::ReportNamed("fallback", "active report", ProfileCategory::Engine, 1.0f);
	}
	FrameGraph::EndFrame();
	const auto &spans = FrameGraph::Spans();
	REQUIRE(spans.size() == 2);
	CHECK(spans[0].Name == "active scope");
	CHECK(spans[1].Name == "active report");
	CHECK(spans[1].Reported);
	CHECK(spans[1].Parent == 0);
}
