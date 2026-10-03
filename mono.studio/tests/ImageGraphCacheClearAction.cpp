#include "ImageGraphCacheClearAction.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_cache_clear_action")
TEST_DEPENDS("engine.imagegraph.cache_results_replay")
using namespace engine::imagegraph;
TEST_CASE(
	"Composer Clear targets selected runtime cache and preserves unrelated preview frames",
	"[studio][cache_results_clear]"
) {
	Document d;
	d.FormatVersion = 9;
	d.Nodes = {
		{"source",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{12, 24, 36, 255}}}},
		{"cache", "pc.cache_results", "", {}, {{"amount", int64_t{1}}}},
		{"select", "pc.sequence_anim", "", {}, {{"overflow", EnumValue{0}}}}
	};
	d.Links = {
		{"source", "image", "cache", "surface_in"}, {"cache", "cache_surfaces", "select", "surface_in"}
	};
	d.Outputs = {{"image", "select", "surface_out"}, {"unchanged", "source", "image"}};
	Plan p;
	Diagnostic diagnostic;
	REQUIRE(Compile(d, p, diagnostic) == Status::Ok);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	REQUIRE(host.Prepare(d, p, 7, 11, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	const auto original = d;
	const auto retained = *request.DataReplay;
	studio::ImageGraphPreviewCache previews;
	const Image pixel = *host.Output("image");
	REQUIRE(previews.Store(7, 0, 0, pixel));
	REQUIRE(previews.Store(7, 0, 1, pixel));
	REQUIRE(previews.Store(7, 1, 0, pixel));
	REQUIRE(previews.Store(7, 1, 1, pixel));
	CHECK_FALSE(
		studio::detail::ApplyImageGraphCacheResultsClear(
			d, p, host, previews, "cache", "select", 7, 11, diagnostic
		)
	);
	CHECK(diagnostic.Code == Status::InvalidValue);
	CHECK(*request.DataReplay == retained);
	CHECK(previews.Find(7, 0, 0));
	CHECK_FALSE(
		studio::detail::ApplyImageGraphCacheResultsClear(
			d, p, host, previews, "cache", "cache", 7, 11, diagnostic, 1
		)
	);
	CHECK(*request.DataReplay == retained);
	CHECK(previews.Find(7, 0, 0));
	REQUIRE(
		studio::detail::ApplyImageGraphCacheResultsClear(
			d, p, host, previews, "cache", "cache", 7, 11, diagnostic
		)
	);
	CHECK(d == original);
	CHECK(previews.Find(7, 0, 0) == nullptr);
	CHECK(previews.Find(7, 0, 1) == nullptr);
	REQUIRE(previews.Find(7, 1, 0));
	CHECK(*previews.Find(7, 1, 0) == pixel);
	REQUIRE(previews.Find(7, 1, 1));
	CHECK(*previews.Find(7, 1, 1) == pixel);
	CHECK(host.Output("image") == nullptr);
	CHECK_FALSE(host.Prepare(d, p, 7, 11, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	request.Tick = 1;
	REQUIRE(host.Prepare(d, p, 7, 11, request, diagnostic, Limits::MaximumEvaluationBytes, "image"));
	REQUIRE(host.Output("image"));
	CHECK(*host.Output("image") == pixel);
}
