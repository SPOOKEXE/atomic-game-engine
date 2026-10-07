#include "ImageGraphCacheClearAction.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph_grouped_cache_clear")
TEST_DEPENDS("engine.imagegraph.frame_cache_replay")
using namespace engine::imagegraph;

namespace {
	Document GroupedDocument(bool array, bool serialize) {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{2, 0, 1, "loop", 24};
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = document.Project->SurfaceHeight = 1;
		document.Nodes = {
			{"producer",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{12, 24, 36, 255}}}},
			{"cache",
			 array ? "pc.cache_array" : "pc.cache",
			 "",
			 {},
			 array
				 ? std::vector<
					   AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				 : std::vector<AuthoredValue>{{"animated", false}}},
			{"overlap", "pc.cache", "", {}, {{"animated", false}}},
			{"consumer", "image.passthrough", "", {}, {}},
			{"other",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{99, 0, 0, 255}}}},
			{"other-cache", "pc.cache", "", {}, {{"animated", false}}}
		};
		const ArrayValue group{ValueType::Text, {ElementValue{std::string{"producer"}}}};
		document.Nodes[1].SourceProperties = {{"cache_group", group}, {"serialize", serialize}};
		document.Nodes[2].SourceProperties = {{"cache_group", group}};
		document.Links = {
			{"producer", "image", "cache", "surface_in"},
			{"producer", "image", "overlap", "surface_in"},
			{"producer", "image", "consumer", "image"},
			{"other", "image", "other-cache", "surface_in"}
		};
		document.Outputs = {
			{"cached", "cache", array ? "cache_array" : "cache_surface"},
			{"overlap", "overlap", "cache_surface"},
			{"downstream", "consumer", "image"},
			{"other", "other-cache", "cache_surface"}
		};
		if (array) {
			document.Nodes.push_back({"select", "pc.sequence_anim", "", {}, {{"overflow", EnumValue{0}}}});
			document.Links.push_back({"cache", "cache_array", "select", "surface_in"});
			document.Outputs[0] = {"cached", "select", "surface_out"};
		}
		// Feedback declarations make the real host retain each preview cone in one frame.
		for (const auto &output : document.Outputs)
			document.Nodes.push_back(
				{"binding-" + output.Id,
				 "image.captured",
				 "",
				 {},
				 {{"source_id", std::string{"feedback:"} + output.Id}}}
			);
		return document;
	}
	const CacheGroupReplayNode &Producer(const DataReplayState &state) {
		const auto found = std::find_if(
			state.CacheGroups.Nodes.begin(), state.CacheGroups.Nodes.end(), [](const auto &node) {
				return node.NodeId == "producer";
			}
		);
		REQUIRE(found != state.CacheGroups.Nodes.end());
		return *found;
	}
	const DataReplayEntry &CacheRow(const DataReplayState &state, std::string_view id) {
		const auto found = std::find_if(state.Entries.begin(), state.Entries.end(), [&](const auto &row) {
			return row.NodeId == id;
		});
		REQUIRE(found != state.Entries.end());
		return *found;
	}
}

TEST_CASE(
	"Selected Clear wakes overlapping frozen groups and retires dependent Studio previews under source gates",
	"[studio][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true})
		for (int gate = 0; gate < 4; ++gate) {
			CAPTURE(array, gate);
			const auto document = GroupedDocument(array, gate != 1);
			const auto authored = document;
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
			CapturedFeedbackHost host;
			EvaluationRequest request;
			request.SourceCachePlayback =
				SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
			// Inspect the last owner through the public input-capture route. Ordinary playback
			// sees an inactive producer after the first owner freezes it and only wakes the
			// overlapping owner; input capture admits its final update and last-frame freeze.
			for (uint64_t tick = 0; tick < 2; ++tick) {
				request.Tick = tick;
				request.SourceCacheProject =
					SourceFrameCacheProjectObservation{{tick, 0, false}, double(tick), false, false};
				REQUIRE(host.Prepare(
					document,
					plan,
					7,
					11,
					request,
					diagnostic,
					Limits::MaximumEvaluationBytes,
					"other",
					"overlap"
				));
				REQUIRE(request.DataReplay);
			}
			request.SourceCacheProject =
				SourceFrameCacheProjectObservation{{1, 0, false}, 1, gate == 2, gate == 3};
			REQUIRE(host.Prepare(
				document, plan, 7, 11, request, diagnostic, Limits::MaximumEvaluationBytes, "other", "overlap"
			));
			REQUIRE_FALSE(Producer(*request.DataReplay).RenderActive);
			CHECK(Producer(*request.DataReplay).OwnerId == "overlap");
			const auto before = *request.DataReplay;
			const auto bytes = host.RetainedBytes();
			studio::ImageGraphPreviewCache previews;
			const Image pixel{1, 1, {12, 24, 36, 255}};
			for (size_t output = 0; output < document.Outputs.size(); ++output)
				for (uint64_t frame = 0; frame < 2; ++frame)
					REQUIRE(previews.Store(7, output, frame, pixel));
			const auto clear = [&](uint64_t cap) {
				return studio::detail::ApplyImageGraphSourceCacheClear(
					document, plan, host, previews, "cache", "cache", 7, 11, diagnostic, cap
				);
			};
			// Gated Cache Array is an admitted input-only no-op before frame-copy admission.
			CHECK(clear(1) == (array && gate != 0));
			CHECK(diagnostic.Code == (array && gate != 0 ? Status::Ok : Status::LimitExceeded));
			CHECK(*request.DataReplay == before);
			CHECK(host.RetainedBytes() == bytes);
			CHECK(host.CacheInvalidatedOutputs().empty());
			for (size_t output = 0; output < document.Outputs.size(); ++output)
				for (uint64_t frame = 0; frame < 2; ++frame)
					REQUIRE(previews.Find(7, output, frame));
			REQUIRE(clear(Limits::MaximumEvaluationBytes));
			CHECK(document == authored);
			CHECK(Producer(*request.DataReplay).RenderActive == (gate == 0));
			CHECK(Producer(*request.DataReplay).OwnerId == "overlap");
			CHECK(Producer(*request.DataReplay).Outputs == Producer(before).Outputs);
			CHECK(CacheRow(*request.DataReplay, "other-cache") == CacheRow(before, "other-cache"));
			CHECK(CacheRow(*request.DataReplay, "overlap") == CacheRow(before, "overlap"));
			const bool clears = !array || gate == 0;
			if (clears) {
				CHECK(CacheRow(*request.DataReplay, "cache").FrameCacheConstructorCleared);
				CHECK(CacheRow(*request.DataReplay, "cache").Values.size() == 2);
			} else
				CHECK(*request.DataReplay == before);
			for (uint64_t frame = 0; frame < 2; ++frame) {
				CHECK((previews.Find(7, 0, frame) == nullptr) == clears);
				CHECK((previews.Find(7, 1, frame) == nullptr) == (gate == 0));
				CHECK((previews.Find(7, 2, frame) == nullptr) == (gate == 0));
				REQUIRE(previews.Find(7, 3, frame));
				CHECK(*previews.Find(7, 3, frame) == pixel);
			}
			REQUIRE(host.Output("other"));
			CHECK(host.Output("other")->Pixels[0] == 99);
			if (gate == 0) {
				// The untouched selected cone permits same-clock refresh from the private checkpoint.
				// Loading suppresses automatic group actions, exposing the Clear's frame-start wake.
				request.SourceCacheProject->ProjectLoading = true;
				REQUIRE(host.Prepare(
					document,
					plan,
					7,
					11,
					request,
					diagnostic,
					Limits::MaximumEvaluationBytes,
					"other",
					"overlap"
				));
				CHECK(Producer(*request.DataReplay).RenderActive);
				CHECK(Producer(*request.DataReplay).OwnerId == "overlap");
				CHECK(CacheRow(*request.DataReplay, "cache").FrameCacheConstructorCleared);
				CHECK(document == authored);
			}
		}
}
