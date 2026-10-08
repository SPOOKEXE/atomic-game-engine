#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.source_frame_cache_region_lifecycle")
TEST_DEPENDS("engine.imagegraph.source_common_runtime")
TEST_DEPENDS("engine.imagegraph.frame_cache_replay")
using namespace engine::imagegraph;
namespace {
	Document RegionScene(bool array) {
		Document document;
		document.FormatVersion = 11;
		document.Timeline = TimelineSettings{4, 0, 3, "loop", 24};
		Node node{"cache", array ? "pc.cache_array" : "pc.cache", "", {}, {}};
		node.Values =
			array
				? std::vector<
					  AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				: std::vector<AuthoredValue>{{"animated", false}};
		node.SourceProperties = {{"cache", std::string{"saved"}}};
		document.Nodes.push_back(std::move(node));
		document.Nodes.push_back(
			{"producer",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{99, 0, 0, 255}}}}
		);
		document.Links = {{"producer", "image", "cache", "surface_in"}};
		document.Nodes.front().SourceProperties.push_back(
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"producer"}}}}
		);
		document.Outputs = {{"out", "cache", array ? "cache_array" : "cache_surface"}};
		SourceCommonOwnerRecord owner;
		owner.SourceOwnerId = "cache";
		owner.SourceType = array ? "Node_Cache_Array" : "Node_Cache";
		owner.NativeOwnerId = "cache";
		owner.OutMeta = true;
		owner.UpdateAnimatorOwnerId = "cache";
		owner.UpdateAnimatorPort = "native:animator:1";
		document.SourceCommonOwners.push_back(std::move(owner));
		document.SourceAnimators.emplace();
		DetachedSourceAnimator animator;
		animator.OwnerId = "cache";
		animator.Id = "native:animator:1";
		animator.OriginalPort = "pxcx.update_in_trigger";
		animator.Type = ValueType::Boolean;
		animator.Writer = GroupSubtypeAnimator::Static;
		document.SourceAnimators->Detached.push_back(std::move(animator));
		GroupSubtypeOverlay value;
		value.NodeId = "cache";
		value.Port = "native:animator:1";
		value.Fixed = false;
		document.SourceAnimators->DetachedValues.push_back(std::move(value));
		return document;
	}
	Document NativeRegionScene(bool array) {
		auto document = RegionScene(array);
		DataReplayEntry inventory;
		inventory.NodeId = "cache";
		inventory.Initialized = true;
		inventory.PreviousValue = 1;
		inventory.LoadedCacheData = "saved";
		inventory.SourceFrameCacheSerializedSlots = 4;
		inventory.Values = {
			{0, document.Nodes.front().Type},
			{1, array ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}}}
		};
		for (uint64_t slot = 0; slot < 4; ++slot) {
			Image image{1, 1, {uint8_t(11 + slot * 11), 0, 0, 255}};
			image.Hash = SurfaceHash(image);
			inventory.Values.push_back({slot + 2, SurfaceValue{std::move(image)}});
		}
		ArrayValue chunks;
		Diagnostic error;
		REQUIRE(EncodeSourceFrameCacheReceipt(inventory, chunks, error) == Status::Ok);
		document.Nodes.front().SourceProperties.push_back(
			{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), inventory.LoadedCacheData}
		);
		document.Nodes.front().SourceProperties.push_back(
			{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(chunks)}
		);
		return document;
	}
	const CacheGroupReplayNode &Activity(const DataReplayState &state, std::string_view id) {
		const auto found = std::find_if(
			state.CacheGroups.Nodes.begin(), state.CacheGroups.Nodes.end(), [&](const auto &node) {
				return node.NodeId == id;
			}
		);
		REQUIRE(found != state.CacheGroups.Nodes.end());
		return *found;
	}
}
TEST_CASE(
	"Selected fractional endpoints leave progressive project loading independent of group render activity "
	"and Clear",
	"[imagegraph][source_frame_cache][frame_cache_loading][cache_group][source_cache_region_lifecycle]"
) {
	for (bool array : {false, true}) {
		auto document = NativeRegionScene(array);
		const auto saved = Write(document);
		for (bool reopened : {false, true}) {
			CAPTURE(array, reopened);
			Diagnostic error;
			if (reopened) {
				Document restored;
				REQUIRE(Read(saved, restored, error) == Status::Ok);
				REQUIRE(restored == document);
				document = std::move(restored);
			}
			Plan plan;
			REQUIRE(CompileSourceCommonRuntime(document, plan, error) == Status::Ok);
			DataReplayState initial;
			REQUIRE(
				InitializeAuthoredCacheGroupReplay(
					document, {}, initial.CacheGroups, Limits::MaximumEvaluationBytes, error
				) == Status::Ok
			);
			for (auto &node : initial.CacheGroups.Nodes)
				node.RenderActive = false;
			const auto frozenInitial = initial;
			EvaluationRequest request;
			request.Tick = 1;
			request.Subframe = .5;
			request.DataReplay = &initial;
			request.SourceCachePlayback = SourceCachePlaybackObservation{
				false, SourceCacheSampling::ObservedFrame, true, SourceCacheLoadMode::SourceStepLoading
			};
			request.SourceCacheProject =
				SourceFrameCacheProjectObservation{{1, .5, false}, 1.5, false, false};
			GroupRenderSession session;
			REQUIRE(
				InitializeNativeSourceCommonRuntime(
					document, plan, request, SourceNodeInitialState::Loaded, session, error
				) == Status::Ok
			);
			request.DataReplay = nullptr;
			REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, error) == Status::Ok);
			REQUIRE(session.Replay.Data.Entries.size() == 1);
			REQUIRE(session.Replay.Data.Entries.front().SourceFrameCacheLoading);
			CHECK(session.Replay.Data.Entries.front().SourceFrameCacheLoading->NextSlot == 1);
			CHECK_FALSE(Activity(session.Replay.Data, "producer").RenderActive);
			CHECK_FALSE(Activity(session.Replay.Data, "cache").RenderActive);
			const auto beforeClear = session.Replay.Data;
			DataReplayState cleared;
			REQUIRE(
				ClearSourceFrameCacheButtonReplay(
					document.Nodes.front(), beforeClear, cleared, request.SourceCacheProject, error
				) == Status::Ok
			);
			CHECK(beforeClear == session.Replay.Data);
			CHECK(
				cleared.Entries.front().SourceFrameCacheLoading ==
				beforeClear.Entries.front().SourceFrameCacheLoading
			);
			CHECK(cleared.Entries.front().Values.size() == 2);
			CHECK(Activity(cleared, "producer").RenderActive);
			CHECK_FALSE(Activity(cleared, "cache").RenderActive);
			session.Replay.Data = std::move(cleared);
			for (uint64_t next = 2; next <= 4; ++next) {
				REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, error) == Status::Ok);
				const auto &row = session.Replay.Data.Entries.front();
				REQUIRE(row.SourceFrameCacheLoading);
				CHECK(row.SourceFrameCacheLoading->NextSlot == next);
				CHECK(row.SourceFrameCacheLoading->Loading == (next != 4));
			}
			const auto &complete = session.Replay.Data.Entries.front();
			CHECK(complete.SourceFrameCacheSerializedSlots == 4);
			CHECK(complete.Values.size() == 5);
			CHECK(complete.Values.back().Frame == 5);
			CHECK(std::get<SurfaceValue>(complete.Values.back().Data).Data.Pixels.front() == 44);
			const auto *output = SourceFrameCacheLastOutput(complete);
			REQUIRE(output);
			if (array)
				CHECK(std::get<ArrayValue>(*output).Elements.empty());
			else
				CHECK(std::get<SurfaceValue>(*output).Data.Pixels.front() == 22);
			const auto finished = session.Replay.Data;
			REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, error) == Status::Ok);
			CHECK(session.Replay.Data == finished);
			CHECK(initial == frozenInitial);
			CHECK(Write(document) == saved);
			CHECK(request.Tick == 1);
			CHECK(request.Subframe == .5);
		}
	}
}
