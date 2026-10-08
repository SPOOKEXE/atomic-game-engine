#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_frame_cache_loading_common")
TEST_DEPENDS("engine.imagegraph.source_common_runtime")
TEST_DEPENDS("engine.imagegraph.frame_cache_replay")

using namespace engine::imagegraph;
namespace {
	Document LoadingScene(bool array) {
		Document document;
		document.FormatVersion = 11;
		document.Timeline = TimelineSettings{2, 0, 1, "loop", 24};
		Node node{"cache", array ? "pc.cache_array" : "pc.cache", "", {}, {}};
		node.Values =
			array
				? std::vector<
					  AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				: std::vector<AuthoredValue>{{"animated", false}};
		node.SourceProperties = {{"cache", std::string{"saved"}}};
		document.Nodes.push_back(std::move(node));
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
	DataReplayState Inventory(const Document &document) {
		DataReplayEntry row;
		row.NodeId = "cache";
		row.Initialized = true;
		row.PreviousValue = 1;
		row.LoadedCacheData = "saved";
		row.SourceFrameCacheSerializedSlots = 2;
		Image first{1, 1, {13, 0, 0, 255}};
		first.Hash = SurfaceHash(first);
		Image second{1, 1, {27, 0, 0, 255}};
		second.Hash = SurfaceHash(second);
		row.Values = {
			{0, document.Nodes.front().Type},
			{1, int64_t{-4}},
			{2, SurfaceValue{std::move(first)}},
			{3, SurfaceValue{std::move(second)}}
		};
		if (document.Nodes.front().Type == "pc.cache_array")
			row.Values[1].Data = ArrayValue{ValueType::Any, {}};
		return {{std::move(row)}, {}};
	}
}

TEST_CASE(
	"Cold source cache Serialize false leaves saved inventory unloaded",
	"[imagegraph][source_frame_cache_loading][frame_cache_loading]"
) {
	for (bool array : {false, true}) {
		auto document = LoadingScene(array);
		document.Nodes.front().SourceProperties.push_back({"serialize", false});
		const auto loads = Inventory(document);
		CHECK(SourceFrameCacheSavedText(document.Nodes.front()).empty());
		Plan plan;
		Diagnostic diagnostic;
		const auto compile = CompileSourceCommonRuntime(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compile == Status::Ok);
		EvaluationRequest request;
		request.SourceFrameCacheLoads = &loads;
		request.SourceCachePlayback = SourceCachePlaybackObservation{
			false, SourceCacheSampling::ObservedFrame, true, SourceCacheLoadMode::SourceStepLoading
		};
		GroupRenderSession session;
		REQUIRE(
			InitializeNativeSourceCommonRuntime(
				document, plan, request, SourceNodeInitialState::Loaded, session, diagnostic
			) == Status::Ok
		);
		const auto replay = session.Replay.Data;
		const auto common = session.Common;
		const auto outputs = session.Outputs;
		REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
		CHECK(session.Replay.Data == replay);
		CHECK(session.Common == common);
		CHECK(session.Outputs == outputs);
		CHECK(session.Replay.Data.Entries.empty());
	}
}

TEST_CASE(
	"Cache override loads one slot while paused without inherited metadata or Updated",
	"[imagegraph][source_frame_cache_loading][frame_cache_loading]"
) {
	for (bool array : {false, true}) {
		const auto document = LoadingScene(array);
		const auto loads = Inventory(document);
		Plan plan;
		Diagnostic diagnostic;
		const auto compile = CompileSourceCommonRuntime(document, plan, diagnostic);
		INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
		REQUIRE(compile == Status::Ok);
		EvaluationRequest request;
		request.SourceFrameCacheLoads = &loads;
		request.SourceCachePlayback = SourceCachePlaybackObservation{
			false, SourceCacheSampling::ObservedFrame, true, SourceCacheLoadMode::SourceStepLoading
		};
		GroupRenderSession session;
		INFO(diagnostic.Message);
		REQUIRE(
			InitializeNativeSourceCommonRuntime(
				document, plan, request, SourceNodeInitialState::Loaded, session, diagnostic
			) == Status::Ok
		);
		REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
		REQUIRE(session.Replay.Data.Entries.size() == 1);
		const auto &first = session.Replay.Data.Entries.front();
		REQUIRE(first.SourceFrameCacheLoading);
		CHECK(first.SourceFrameCacheLoading->NextSlot == 1);
		CHECK(first.SourceFrameCacheLoading->Loading);
		CHECK(first.Values.size() == 3);
		CHECK_FALSE(session.Common.Owners.front().Updated);
		REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
		const auto &complete = session.Replay.Data.Entries.front();
		REQUIRE(complete.SourceFrameCacheLoading);
		CHECK(complete.SourceFrameCacheLoading->NextSlot == 2);
		CHECK_FALSE(complete.SourceFrameCacheLoading->Loading);
		CHECK_FALSE(session.Common.Owners.front().Updated);
		const auto *output = SourceFrameCacheLastOutput(complete);
		REQUIRE(output);
		if (array)
			CHECK(std::get<ArrayValue>(*output).Elements.empty());
		else
			CHECK(std::get<SurfaceValue>(*output).Data.Pixels.front() == 13);
		const auto replay = session.Replay.Data;
		REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
		CHECK(session.Replay.Data == replay);
	}
}

TEST_CASE(
	"Cache loading refusal retains common session and progress",
	"[imagegraph][source_frame_cache_loading][frame_cache_loading]"
) {
	const auto document = LoadingScene(false);
	auto loads = Inventory(document);
	loads.Entries.front().SourceFrameCacheSerializedSlots = 1;
	loads.Entries.front().Values.pop_back();
	Plan plan;
	Diagnostic diagnostic;
	const auto compile = CompileSourceCommonRuntime(document, plan, diagnostic);
	INFO(diagnostic.Message << " node=" << diagnostic.NodeId << " port=" << diagnostic.Port);
	REQUIRE(compile == Status::Ok);
	EvaluationRequest request;
	request.SourceFrameCacheLoads = &loads;
	request.SourceCachePlayback = SourceCachePlaybackObservation{
		false, SourceCacheSampling::ObservedFrame, true, SourceCacheLoadMode::SourceStepLoading
	};
	GroupRenderSession session;
	REQUIRE(
		InitializeNativeSourceCommonRuntime(
			document, plan, request, SourceNodeInitialState::Loaded, session, diagnostic
		) == Status::Ok
	);
	REQUIRE(NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) == Status::Ok);
	const auto before = session;
	CHECK(
		NativeSourceStepBounded(document, plan, request, {}, session, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(session.Replay.Data == before.Replay.Data);
	CHECK(session.Common == before.Common);
}
