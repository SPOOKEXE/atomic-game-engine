#include "SourceFrameCacheInputs.hpp"

#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_frame_cache_loading_execution")
using namespace engine::imagegraph;
namespace {
	Image SavedImage(uint8_t red) {
		return {1, 1, {red, 20, 30, 255}, 0};
	}
	DataReplayEntry Receipt(bool array = false) {
		DataReplayEntry row;
		row.NodeId = "cache";
		row.Initialized = true;
		row.LoadedCacheData = "[exact source serialized inventory]";
		row.SourceFrameCacheSerializedSlots = 4;
		row.Values = {
			{0, std::string(array ? "pc.cache_array" : "pc.cache")},
			{1, array ? Value{ArrayValue{ValueType::Any, {}}} : Value{SurfaceValue{SavedImage(99)}}},
			{2, SurfaceValue{SavedImage(11)}},
			{3, int64_t{-4}},
			{4, SurfaceValue{SavedImage(22)}}
		};
		return row;
	}
	Document Scene(bool array = false, bool trap = false) {
		Document d;
		d.FormatVersion = 11;
		d.Timeline = TimelineSettings{4, 0, 3, "loop", 24};
		d.Nodes = {
			trap
				? Node{"producer", "pc.lua_compute", "", {}, {{"function_name", std::string{"unread_producer"}}, {"return_type", EnumValue{0}}}}
				: Node{"producer", "image.solid", "", {}, {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{77, 20, 30, 255}}}},
			{"cache",
			 array ? "pc.cache_array" : "pc.cache",
			 "",
			 {},
			 array
				 ? std::vector<
					   AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				 : std::vector<AuthoredValue>{{"animated", true}}}
		};
		d.Nodes[1].SourceProperties = {{"cache", Receipt(array).LoadedCacheData}};
		d.Links = {{"producer", trap ? "return_value" : "image", "cache", "surface_in"}};
		d.Outputs = {{"out", "cache", array ? "cache_array" : "cache_surface"}};
		return d;
	}
	Plan Checked(const Document &d) {
		Plan p;
		Diagnostic e;
		auto code = Compile(d, p, e);
		INFO(e.Message << " " << e.Port);
		REQUIRE(code == Status::Ok);
		return p;
	}
	DataReplayState Begun(const Document &d) {
		DataReplayState state;
		DataReplayEntry row;
		Diagnostic e;
		REQUIRE(
			BeginSourceFrameCacheLoading(d.Nodes[1], Receipt(d.Nodes[1].Type == "pc.cache_array"), row, e) ==
			Status::Ok
		);
		state.Entries.push_back(std::move(row));
		return state;
	}
	void Step(const Document &d, DataReplayState &state, bool expectedCompleted = false) {
		DataReplayEntry next;
		bool completed = false;
		Diagnostic e;
		auto code = StepSourceFrameCacheLoading(d.Nodes[1], 4, state.Entries[0], next, completed, e);
		INFO(e.Message);
		REQUIRE(code == Status::Ok);
		CHECK(completed == expectedCompleted);
		state.Entries[0] = std::move(next);
	}
	EvaluationRequest Request(uint64_t tick, const DataReplayState &state, bool playing = true) {
		EvaluationRequest q;
		q.Tick = tick;
		q.DataReplay = &state;
		q.SourceCachePlayback = SourceCachePlaybackObservation{
			playing, SourceCacheSampling::ObservedFrame, true, SourceCacheLoadMode::SourceStepLoading
		};
		return q;
	}
	StatefulEvaluationResult Run(const Document &d, EvaluationRequest q) {
		StatefulEvaluationResult r;
		Diagnostic e;
		auto code = EvaluateStateful(d, Checked(d), "out", q, r, e);
		INFO(e.Message << " " << e.NodeId << ":" << e.Port);
		REQUIRE(code == Status::Ok);
		return r;
	}
	int64_t Noone(const StatefulEvaluationResult &r) {
		auto value = std::get_if<EvaluatedValue>(&r.Output);
		REQUIRE(value);
		REQUIRE(std::holds_alternative<int64_t>(value->Data));
		return std::get<int64_t>(value->Data);
	}
	int Red(const StatefulEvaluationResult &r) {
		auto image = std::get_if<Image>(&r.Output);
		REQUIRE(image);
		return image->Pixels[0];
	}
}
TEST_CASE(
	"Progressive Cache reads never advance or expose future saved frames",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(false, true);
	auto state = Begun(d);
	const auto original = state;
	auto cold = Run(d, Request(2, state));
	CHECK(Noone(cold) == -4);
	CHECK(cold.Data.Entries[0].SourceFrameCacheLoading == state.Entries[0].SourceFrameCacheLoading);
	CHECK(cold.Data.Entries[0].Values.size() == 2);
	CHECK(state == original);
	auto retry = Run(d, Request(2, cold.Data));
	CHECK(Noone(retry) == -4);
	CHECK(retry.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 0);
	Step(d, state);
	auto first = Run(d, Request(0, state, false));
	CHECK(Red(first) == 11);
	CHECK(first.Data.Entries[0].PreviousValue == 1);
	CHECK(first.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 1);
	auto future = Run(d, Request(2, first.Data));
	CHECK(Red(future) == 11);
	CHECK(future.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 1);
	CHECK(future.Data.Entries[0].Values.size() == 3);
	Step(d, state);
	Step(d, state);
	auto published = Run(d, Request(2, state, false));
	CHECK(Red(published) == 22);
	CHECK(published.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 3);
	Step(d, state, true);
	auto completed = Run(d, Request(2, state));
	CHECK(Red(completed) == 22);
	CHECK_FALSE(completed.Data.Entries[0].SourceFrameCacheLoading->Loading);
	CHECK(completed.Data.Entries[0].SourceFrameCacheSerializedSlots == 4);
	// A published noone slot has cache_result=true but fails source cacheExist and cannot hide a miss.
	StatefulEvaluationResult refused = completed;
	Diagnostic e;
	CHECK(
		EvaluateStateful(d, Checked(d), "out", Request(1, state), refused, e) == Status::UnsupportedExecution
	);
	CHECK(refused.Data == completed.Data);
}
TEST_CASE(
	"Progressive Cache reads Animated on paused misses loading and hits",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(false, true);
	auto state = Begun(d);
	d.Nodes.push_back(
		{"animated-trap",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"function_name", std::string{"unread_animated"}}, {"return_type", EnumValue{0}}}}
	);
	d.Links.push_back({"animated-trap", "return_value", "cache", "animated"});
	for (bool hit : {false, true}) {
		if (hit) Step(d, state);
		for (bool playing : {false, true}) {
			StatefulEvaluationResult result;
			Diagnostic e;
			const auto code = EvaluateStateful(d, Checked(d), "out", Request(0, state, playing), result, e);
			CHECK(code == Status::UnsupportedExecution);
			CHECK(e.NodeId == "animated-trap");
			CHECK(result.Data.Entries.empty());
		}
	}
}
TEST_CASE(
	"Progressive Cache Array loading suppresses producer and range getters",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(true, true);
	auto state = Begun(d);
	d.Nodes.push_back(
		{"range-trap",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"function_name", std::string{"unread_range"}}, {"return_type", EnumValue{0}}}}
	);
	d.Links.push_back({"range-trap", "return_value", "cache", "start_frame"});
	for (bool playing : {false, true}) {
		auto r = Run(d, Request(2, state, playing));
		CHECK(r.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 0);
		CHECK(r.Data.Entries[0].Values.size() == 2);
	}
	Step(d, state);
	Step(d, state);
	Step(d, state);
	auto loading = Run(d, Request(2, state));
	CHECK(loading.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 3);
	Step(d, state, true);
	auto paused = Run(d, Request(2, state, false));
	CHECK_FALSE(paused.Data.Entries[0].SourceFrameCacheLoading->Loading);
	StatefulEvaluationResult refused = paused;
	Diagnostic e;
	CHECK(
		EvaluateStateful(d, Checked(d), "out", Request(2, state), refused, e) == Status::UnsupportedExecution
	);
	CHECK(refused.Data == paused.Data);
}
TEST_CASE(
	"Cold native progressive Cache initializes metadata without decoding future inventory",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(false, true);
	auto receipt = Receipt();
	ArrayValue chunks;
	Diagnostic e;
	REQUIRE(EncodeSourceFrameCacheReceipt(receipt, chunks, e) == Status::Ok);
	d.Nodes[1].SourceProperties.push_back(
		{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), receipt.LoadedCacheData}
	);
	d.Nodes[1].SourceProperties.push_back({std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), chunks});
	DataReplayState empty;
	auto cold = Run(d, Request(2, empty));
	CHECK(Noone(cold) == -4);
	REQUIRE(cold.Data.Entries.size() == 1);
	auto &row = cold.Data.Entries[0];
	REQUIRE(row.SourceFrameCacheLoading);
	CHECK(row.SourceFrameCacheLoading->NativeReceipt);
	CHECK(row.SourceFrameCacheLoading->PendingSlots.empty());
	CHECK(row.SourceFrameCacheLoading->NextSlot == 0);
	CHECK(row.Values.size() == 2);
	auto state = cold.Data;
	Step(d, state);
	auto first = Run(d, Request(0, state, false));
	CHECK(Red(first) == 11);
	CHECK(first.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 1);
}
TEST_CASE(
	"Progressive cache read policy keeps complete receipt shortcuts separate",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	using namespace engine::imagegraph::detail;
	CHECK(SourceFrameCacheReadPolicy("pc.cache", true, true, true, true) == SourceFrameCacheInputReads::None);
	CHECK(
		SourceFrameCacheReadPolicy("pc.cache", true, true, true, true, true, true) ==
		SourceFrameCacheInputReads::ControlsOnly
	);
	CHECK(
		SourceFrameCacheReadPolicy("pc.cache", true, true, true, false, true, true) ==
		SourceFrameCacheInputReads::ControlsOnly
	);
	CHECK(
		SourceFrameCacheReadPolicy("pc.cache_array", true, true, true, false, true, true) ==
		SourceFrameCacheInputReads::None
	);
	CHECK(
		SourceFrameCacheReadPolicy("pc.cache_array", true, true, true, false, true, false) ==
		SourceFrameCacheInputReads::All
	);
}
TEST_CASE(
	"Progressive constructor stages prepared load receipts without recovering their saved output",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(false, true);
	DataReplayState loads;
	loads.Entries.push_back(Receipt());
	const auto original = loads;
	DataReplayState empty;
	auto request = Request(2, empty);
	request.SourceFrameCacheLoads = &loads;
	auto result = Run(d, request);
	CHECK(Noone(result) == -4);
	REQUIRE(result.Data.Entries.size() == 1);
	auto &row = result.Data.Entries[0];
	REQUIRE(row.SourceFrameCacheLoading);
	CHECK_FALSE(row.SourceFrameCacheLoading->NativeReceipt);
	CHECK(row.SourceFrameCacheLoading->NextSlot == 0);
	CHECK(row.SourceFrameCacheLoading->PendingSlots.size() == 3);
	CHECK(row.Values.size() == 2);
	CHECK(loads == original);
	auto repeated = Run(d, Request(2, result.Data, false));
	CHECK(Noone(repeated) == -4);
	CHECK(repeated.Data.Entries[0].SourceFrameCacheLoading == row.SourceFrameCacheLoading);
}
TEST_CASE(
	"Progressive Cache distinguishes a published empty array from a published noone hole",
	"[imagegraph][source_frame_cache][frame_cache_loading]"
) {
	auto d = Scene(false, true);
	auto decoded = Receipt();
	decoded.Values[2].Data = ArrayValue{ValueType::Any, {}};
	DataReplayState state;
	DataReplayEntry begun;
	Diagnostic e;
	REQUIRE(BeginSourceFrameCacheLoading(d.Nodes[1], decoded, begun, e) == Status::Ok);
	state.Entries.push_back(begun);
	Step(d, state);
	auto recovered = Run(d, Request(0, state));
	REQUIRE(recovered.Data.Entries.size() == 1);
	REQUIRE(recovered.Data.Entries[0].Values.size() == 3);
	CHECK(std::holds_alternative<ArrayValue>(recovered.Data.Entries[0].Values[1].Data));
	CHECK(recovered.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 1);
	state = recovered.Data;
	Step(d, state);
	auto hole = Run(d, Request(1, state));
	CHECK(std::holds_alternative<ArrayValue>(hole.Data.Entries[0].Values[1].Data));
	CHECK(std::holds_alternative<int64_t>(hole.Data.Entries[0].Values.back().Data));
	CHECK(hole.Data.Entries[0].SourceFrameCacheLoading->NextSlot == 2);
}
