#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraph/SourceCommonRuntime.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/imagegraphio/SourceFrameCacheLoading.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraphio.progressive_frame_cache_host")
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	constexpr std::string_view SAVED =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="},null])cache";
	std::string Quoted(std::string_view text) {
		std::string out = "\"";
		for (char c : text) {
			if (c == '"' || c == '\\') out += '\\';
			out += c;
		}
		return out + '"';
	}
	Document Imported(bool array, uint64_t frames) {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson =
			"{\"nodes\":[{\"id\":\"cache\",\"type\":\"" +
			std::string(array ? "Node_Cache_Array" : "Node_Cache") +
			"\",\"x\":0,\"y\":0,\"inputs\":[],\"attri\":{\"serialize\":true,\"cache_group\":[]},\"cache\":" +
			Quoted(SAVED) + "}]}";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		engine::bake::PxcxArchive checked;
		REQUIRE(engine::bake::ReadPxcx(bytes, checked, failure));
		const auto hash = engine::bake::SpriteCacheDataHash(SAVED);
		REQUIRE(hash);
		PxcxImportOptions options;
		options.FrameCacheLayouts = {
			{"cache", std::string(hash->data(), hash->size()), engine::bake::SpriteCacheLayout::Rgba8TopDown}
		};
		PxcxImport imported;
		const auto importedOk = ImportPxcxImageGraph(checked, imported, failure, options);
		INFO(failure);
		REQUIRE(importedOk);
		CHECK(imported.Source.OriginalBytes == bytes);
		REQUIRE(imported.Graph.Nodes.size() == 1);
		REQUIRE(imported.Graph.Nodes[0].Type == (array ? "pc.cache_array" : "pc.cache"));
		REQUIRE_FALSE(imported.Graph.SourceCommonOwners.empty());
		auto document = std::move(imported.Graph);
		REQUIRE(document.FormatVersion >= 11);
		document.Project.emplace();
		document.Project->SurfaceWidth = 2;
		document.Project->SurfaceHeight = 1;
		document.Timeline = TimelineSettings{frames, 0, frames - 1, "stop", 24};
		document.Nodes.push_back(
			{"input",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}}
		);
		document.Links.push_back({"input", "image", "cache", "surface_in"});
		document.Outputs = {
			{"probe", "input", "image"}, {"out", "cache", array ? "cache_array" : "cache_surface"}
		};
		Document restored;
		Diagnostic error;
		REQUIRE(Read(Write(document), restored, error) == Status::Ok);
		CHECK(restored == document);
		return restored;
	}
	const DataReplayValueFrame *Slot(const DataReplayEntry &row, uint64_t slot) {
		const auto found = std::find_if(row.Values.begin(), row.Values.end(), [&](const auto &value) {
			return value.Frame == slot + 2;
		});
		return found == row.Values.end() ? nullptr : &*found;
	}
	struct Host {
		Document Doc;
		Plan Compiled;
		CapturedFeedbackHost Feedback;
		EvaluationRequest Request;
		Diagnostic Error;
		explicit Host(bool array, uint64_t frames = 3) : Doc(Imported(array, frames)) {
			const auto status = Compile(Doc, Compiled, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			Request.SourceSafeMode = false;
			Request.SourceCachePlayback =
				SourceCachePlaybackObservation{false, SourceCacheSampling::ObservedFrame, true};
			Request.SourceCachePlayback->Loading = SourceCacheLoadMode::SourceStepLoading;
			const auto initialized = Feedback.InitializeSourceCommonRuntime(
				Doc, Compiled, Request, SourceNodeInitialState::Loaded, Error, 1, 1
			);
			INFO(Error.Message);
			REQUIRE(initialized == Status::Ok);
			Publish();
		}
		void Publish() {
			const bool ready = Feedback.Prepare(
				Doc, Compiled, 1, 1, Request, Error, Limits::MaximumEvaluationBytes, "probe"
			);
			INFO(Error.NodeId << ":" << Error.Port << " " << Error.Message);
			REQUIRE(ready);
		}
		DataReplayEntry Row() const {
			const auto *data = Feedback.PreparedData(1, 1);
			REQUIRE(data);
			const auto found = std::find_if(data->Entries.begin(), data->Entries.end(), [](const auto &row) {
				return row.NodeId == "cache";
			});
			REQUIRE(found != data->Entries.end());
			return *found;
		}
		void Pulse() {
			const auto status = Feedback.StepSourceCommonRuntime(Doc, Compiled, Request, {}, Error, 1, 1);
			INFO(Error.NodeId << ":" << Error.Port << " " << Error.Message);
			REQUIRE(status == Status::Ok);
			Publish();
		}
		Value Held() {
			const auto *groups = Feedback.PreparedGroups(1, 1, GetFrameTime(Request));
			REQUIRE(groups);
			CacheGroupReplayOutput value;
			const auto status = ReadGroupRenderOutput(Doc, "out", *groups, value, Error);
			INFO(Error.Message);
			REQUIRE(status == Status::Ok);
			REQUIRE(value.Data);
			return *value.Data;
		}
	};
	void Cold(const Value &value, bool array) {
		if (array) {
			const auto *items = std::get_if<ArrayValue>(&value);
			REQUIRE(items);
			CHECK(items->Elements.empty());
			CHECK(items->Nested.empty());
			CHECK(items->Items.empty());
		} else
			CHECK(value == Value{int64_t{-4}});
	}
	void Pixels(const Value &value, std::initializer_list<uint8_t> expected) {
		const auto *surface = std::get_if<SurfaceValue>(&value);
		REQUIRE(surface);
		CHECK(surface->Data.Pixels == std::vector<uint8_t>(expected));
	}
}

TEST_CASE(
	"Imported Cache consumes one sparse slot per paused source step and completes at current frame count",
	"[imagegraphio][frame_cache][progressive][host]"
) {
	Host host(false);
	auto row = host.Row();
	REQUIRE(row.SourceFrameCacheSerializedSlots);
	CHECK(*row.SourceFrameCacheSerializedSlots == 4);
	REQUIRE(row.SourceFrameCacheLoading);
	CHECK(row.SourceFrameCacheLoading->NextSlot == 0);
	CHECK(row.SourceFrameCacheLoading->Loading);
	CHECK(Slot(row, 0) == nullptr);
	CHECK(Slot(row, 2) == nullptr);
	Cold(host.Held(), false);
	for (uint64_t step = 1; step <= 3; ++step) {
		host.Pulse();
		row = host.Row();
		REQUIRE(row.SourceFrameCacheLoading);
		CHECK(row.SourceFrameCacheLoading->NextSlot == step);
		CHECK(row.SourceFrameCacheLoading->Loading == (step < 3));
		REQUIRE(Slot(row, step - 1));
		if (step < 3) {
			CHECK(Slot(row, 2) == nullptr);
			Pixels(host.Held(), {12, 34, 56, 78});
		}
		const auto before = row;
		host.Publish();
		CHECK(host.Row() == before);
	}
	REQUIRE(Slot(row, 0));
	Pixels(Slot(row, 0)->Data, {12, 34, 56, 78});
	REQUIRE(Slot(row, 1));
	CHECK(Slot(row, 1)->Data == Value{int64_t{-4}});
	REQUIRE(Slot(row, 2));
	Pixels(Slot(row, 2)->Data, {200, 10, 40, 0, 255, 128, 64, 255});
	CHECK(Slot(row, 3) == nullptr);
	Pixels(host.Held(), {12, 34, 56, 78});
	host.Pulse();
	CHECK(host.Row().SourceFrameCacheLoading->NextSlot == 3);
}

TEST_CASE(
	"Imported Cache Array preserves trailing holes and paused completion holds its constructor output",
	"[imagegraphio][frame_cache][progressive][host]"
) {
	Host host(true);
	Cold(host.Held(), true);
	for (uint64_t step = 1; step <= 4; ++step) {
		host.Pulse();
		const auto row = host.Row();
		REQUIRE(row.SourceFrameCacheLoading);
		CHECK(row.SourceFrameCacheLoading->NextSlot == step);
		CHECK(row.SourceFrameCacheLoading->Loading == (step < 4));
		REQUIRE(row.SourceFrameCacheSerializedSlots);
		CHECK(*row.SourceFrameCacheSerializedSlots == 4);
		REQUIRE(Slot(row, step - 1));
		Cold(host.Held(), true);
	}
	const auto completed = host.Row();
	REQUIRE(Slot(completed, 3));
	CHECK(Slot(completed, 3)->Data == Value{int64_t{-4}});
	host.Publish();
	CHECK(host.Row() == completed);
	host.Request.SourceCachePlayback->Playing = true;
	const auto prepared = host.Feedback.Prepare(
		host.Doc, host.Compiled, 1, 1, host.Request, host.Error, Limits::MaximumEvaluationBytes, "out"
	);
	INFO(host.Error.NodeId << ":" << host.Error.Port << " " << host.Error.Message);
	REQUIRE(prepared);
	const auto row = host.Row();
	REQUIRE(row.SourceFrameCacheLoading);
	CHECK(row.SourceFrameCacheLoading->NextSlot == 4);
	CHECK_FALSE(row.SourceFrameCacheLoading->Loading);
	const auto *output = SourceFrameCacheLastOutput(row);
	REQUIRE(output);
	const auto *array = std::get_if<ArrayValue>(output);
	REQUIRE(array);
	REQUIRE(array->Items.size() == 3);
	const auto *first = std::get_if<Image>(&array->Items[0].Data);
	REQUIRE(first);
	CHECK(first->Pixels == std::vector<uint8_t>{1, 2, 3, 255, 1, 2, 3, 255});
	CHECK(std::get<ElementValue>(array->Items[1].Data) == ElementValue{int64_t{-1}});
	const auto *third = std::get_if<Image>(&array->Items[2].Data);
	REQUIRE(third);
	CHECK(third->Pixels == std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255});
}

TEST_CASE(
	"Imported source Clear frees published cache slots without rewinding pending load progress",
	"[imagegraphio][frame_cache][progressive][host]"
) {
	for (bool array : {false, true}) {
		Host host(array);
		host.Pulse();
		const auto before = host.Row();
		REQUIRE(before.SourceFrameCacheLoading);
		REQUIRE(Slot(before, 0));
		const auto cleared =
			host.Feedback.ClearSourceCache(host.Doc, host.Compiled, "cache", 1, 1, host.Error);
		INFO(host.Error.Message);
		REQUIRE(cleared);
		host.Publish();
		const auto row = host.Row();
		REQUIRE(row.SourceFrameCacheLoading);
		CHECK(row.SourceFrameCacheLoading == before.SourceFrameCacheLoading);
		CHECK(row.SourceFrameCacheSerializedSlots == before.SourceFrameCacheSerializedSlots);
		CHECK(Slot(row, 0) == nullptr);
		host.Pulse();
		const auto next = host.Row();
		CHECK(next.SourceFrameCacheLoading->NextSlot == 2);
		CHECK(Slot(next, 0) == nullptr);
		REQUIRE(Slot(next, 1));
		CHECK(Slot(next, 1)->Data == Value{int64_t{-4}});
	}
}

TEST_CASE(
	"Progressive host refuses stale saved identity and low byte steps without consuming a source slot",
	"[imagegraphio][frame_cache][progressive][host]"
) {
	Host host(true);
	host.Pulse();
	const auto data = *host.Feedback.PreparedData(1, 1);
	const auto held = host.Held();
	const auto *preparedGroups = host.Feedback.PreparedGroups(1, 1, {});
	REQUIRE(preparedGroups);
	const auto groups = *preparedGroups;
	CHECK(
		host.Feedback.StepSourceCommonRuntime(
			host.Doc, host.Compiled, host.Request, {}, host.Error, 1, 1, 1
		) == Status::LimitExceeded
	);
	REQUIRE(host.Feedback.PreparedData(1, 1));
	CHECK(*host.Feedback.PreparedData(1, 1) == data);
	CHECK(host.Held() == held);
	CHECK(host.Feedback.PreparedGroups(1, 1, {})->Common == groups.Common);
	auto stale = host.Doc;
	const auto saved = std::find_if(
		stale.Nodes[0].SourceProperties.begin(),
		stale.Nodes[0].SourceProperties.end(),
		[](const auto &value) { return value.Port == SOURCE_FRAME_CACHE_NATIVE_TEXT; }
	);
	REQUIRE(saved != stale.Nodes[0].SourceProperties.end());
	std::get<std::string>(saved->Data) += " ";
	Plan plan;
	REQUIRE(Compile(stale, plan, host.Error) == Status::Ok);
	CHECK(
		host.Feedback.StepSourceCommonRuntime(stale, plan, host.Request, {}, host.Error, 1, 1) ==
		Status::InvalidValue
	);
	REQUIRE(host.Feedback.PreparedData(1, 1));
	CHECK(*host.Feedback.PreparedData(1, 1) == data);
	CHECK(host.Held() == held);
	host.Pulse();
	CHECK(host.Row().SourceFrameCacheLoading->NextSlot == 2);
}
