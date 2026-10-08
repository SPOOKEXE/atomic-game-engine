#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraphphysics.cache_results_transport")
TEST_DEPENDS("engine.imagegraph.cache_results_transport")
using namespace engine::imagegraph;
TEST_CASE(
	"A rigid frame-start refresh redraws the selected slot without resurrecting freed cycles",
	"[rigid][cache_results_transport]"
) {
	auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
	document.Timeline = TimelineSettings{5, 1, 4, "loop", 24};
	document.Nodes.push_back({"cache", "pc.cache_results", "rigid", {}, {{"amount", int64_t{2}}}});
	document.Nodes.push_back({"select", "pc.sequence_anim", "rigid", {}, {{"speed", 0.}}});
	document.Links[0] = {"select", "surface_out", "body", "texture"};
	document.Links.push_back({"texture", "image", "cache", "surface_in"});
	document.Links.push_back({"cache", "cache_surfaces", "select", "surface_in"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	CapturedFeedbackHost host;
	EvaluationRequest clock;
	clock.RigidProvider = &provider;
	const auto prepare = [&] {
		const bool ok =
			host.Prepare(document, plan, 7, 11, clock, diagnostic, Limits::MaximumEvaluationBytes, "image");
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(ok);
	};
	const auto cache = [&]() -> const ArrayValue & {
		for (const auto &row : clock.DataReplay->Entries)
			if (row.NodeId == "cache") return std::get<ArrayValue>(row.Values[0].Data);
		FAIL("missing Cache Results history");
		return std::get<ArrayValue>(clock.DataReplay->Entries.front().Values[0].Data);
	};
	prepare();
	clock.Tick = 1;
	clock.RigidPlaying = clock.RigidFrameProgress = true;
	prepare();
	clock.Tick = 2;
	prepare();
	REQUIRE(cache().Elements.size() == 2);
	const auto before = *clock.RigidReplay;
	REQUIRE(before.Owners.size() == 1);
	REQUIRE(before.Owners[0].History.Frames.size() >= 3);
	REQUIRE(host.ClearSourceCache(document, plan, "cache", 7, 11, diagnostic));
	const auto cleared = *clock.DataReplay;
	clock.Subframe = .5;
	clock.RigidPlaying = clock.RigidFrameProgress = false;
	prepare();
	REQUIRE(cache().Items.size() == 2);
	CHECK(std::get<SurfaceValue>(std::get<ElementValue>(cache().Items[0].Data)).Data.Pixels[0] == 255);
	CHECK(IsFreedCacheResultsSlot(std::get<ElementValue>(cache().Items[1].Data)));
	CHECK(IsFreedCacheResultsSlot(std::get<ArrayValue>(cleared.Entries[0].Values[0].Data).Elements[0]));
	CHECK(clock.RigidReplay->Owners[0].History.Frames[0] == before.Owners[0].History.Frames[0]);
	CHECK(clock.RigidReplay->Owners[0].History.Frames[1] == before.Owners[0].History.Frames[1]);
	REQUIRE(host.Output("image"));
	CHECK(
		std::any_of(
			host.Output("image")->Pixels.begin(), host.Output("image")->Pixels.end(), [](uint8_t byte) {
				return byte != 0;
			}
		)
	);
	const auto retained = *clock.DataReplay;
	const auto refreshed = *clock.RigidReplay;
	const auto pixels = *host.Output("image");
	prepare();
	CHECK(*clock.DataReplay == retained);
	CHECK(*clock.RigidReplay == refreshed);
	CHECK(*host.Output("image") == pixels);
}
