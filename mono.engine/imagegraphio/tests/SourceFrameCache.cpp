#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphio/SourceFrameCache.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraphio.source_frame_cache")
namespace bake = engine::bake;
using namespace engine::imagegraph;
using namespace engine::imagegraphio;
namespace {
	constexpr std::string_view Single = R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"}])cache";
	constexpr std::string_view Frames =
		R"cache([{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	constexpr std::string_view Nested =
		R"cache([[{"width":1,"height":1,"buffer":"eJzjUbLwAwABWAC1"},null,[]],[],{"width":2,"height":1,"buffer":"eJw7waXB8L/B4T8ADnkDuQ=="}])cache";
	constexpr std::string_view Vertical =
		R"cache([{"width":1,"height":2,"buffer":"eJzjEpHTSMmraAIABqwCMQ=="}])cache";
	Document Scene(std::string_view type, std::string cache, bool serialize = true) {
		Document document;
		document.FormatVersion = 9;
		document.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 2;
		document.Project->SurfaceHeight = 1;
		const bool array = type == "pc.cache_array";
		document.Nodes = {
			{"input",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{1, 2, 3, 255}}}},
			{"cache",
			 std::string(type),
			 "",
			 {},
			 array
				 ? std::vector<
					   AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				 : std::vector<AuthoredValue>{{"animated", false}}}
		};
		document.Nodes[1].SourceProperties = {{"serialize", serialize}, {"cache", std::move(cache)}};
		document.Links = {{"input", "image", "cache", "surface_in"}};
		document.Outputs = {{"out", "cache", array ? "cache_array" : "cache_surface"}};
		return document;
	}
	SourceFrameCacheLayoutObservation
	Observation(const Node &node, std::string_view text, bake::SpriteCacheLayout layout) {
		const auto hash = bake::SpriteCacheDataHash(text);
		REQUIRE(hash.has_value());
		return {node.Id, std::string(hash->data(), hash->size()), layout};
	}
	DataReplayEntry Decode(const Node &node, std::string_view text, bake::SpriteCacheLayout layout) {
		DataReplayEntry row;
		Diagnostic diagnostic;
		const auto observation = Observation(node, text, layout);
		const auto status = DecodeSourceFrameCache(node, observation, row, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return row;
	}
	Plan Compiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	StatefulEvaluationResult Evaluate(const Document &document, const DataReplayState &loads) {
		EvaluationRequest request;
		request.Tick = 0;
		request.SourceCachePlayback =
			SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
		request.Tick = document.Nodes[1].Type == "pc.cache_array" ? 2 : 0;
		request.SourceFrameCacheLoads = &loads;
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		const auto status =
			EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
	Document Cook(Document source, bake::SpriteCacheLayout layout = bake::SpriteCacheLayout::Rgba8TopDown) {
		const auto &node = source.Nodes[1];
		const auto &text = std::get<std::string>(node.SourceProperties[1].Data);
		const auto observation = Observation(node, text, layout);
		Document cooked;
		Diagnostic diagnostic;
		const auto status = CookSourceFrameCaches(source, std::span(&observation, 1), cooked, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return cooked;
	}
	StatefulEvaluationResult EvaluateCooked(const Document &document, uint64_t tick, bool playing = false) {
		EvaluationRequest request;
		request.Tick = tick;
		request.SourceCachePlayback =
			SourceCachePlaybackObservation{playing, SourceCacheSampling::ObservedFrame, true};
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		const auto status =
			EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
}

TEST_CASE(
	"Cooked sparse saved caches retain source load extents beyond the selected playback endpoint",
	"[imagegraphio][source_frame_cache]"
) {
	for (const std::string_view type : {"pc.cache", "pc.cache_array"}) {
		auto source = Scene(type, std::string(Frames));
		source.Timeline = TimelineSettings{1, 0, 0, "stop", 24};
		source.Timeline->SourceBounds.emplace();
		source.Timeline->SourceBounds->End = {SourceFrameBoundPresence::Explicit, {1, 0, false}};
		const auto cooked = Cook(source);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(cooked), restored, diagnostic) == Status::Ok);
		const auto output = EvaluateCooked(restored, 0);
		REQUIRE(output.Data.Entries.size() == 1);
		const auto &row = output.Data.Entries.front();
		if (type == "pc.cache") {
			CHECK(row.Values.size() == 3);
			CHECK(std::get<Image>(output.Output).Pixels == std::vector<uint8_t>{12, 34, 56, 78});
		} else {
			CHECK(row.Values.size() == 4);
			CHECK(row.Values.back().Frame == 4);
			CHECK(std::get<SurfaceValue>(row.Values.back().Data).Data.Width == 2);
		}
		CHECK(source.Timeline == restored.Timeline);
	}
}
TEST_CASE(
	"Saved source frame cache converts sparse and nested source values", "[imagegraphio][source_frame_cache]"
) {
	auto document = Scene("pc.cache", std::string(Nested));
	const auto &node = document.Nodes[1];
	const auto row = Decode(node, Nested, bake::SpriteCacheLayout::Rgba8TopDown);
	REQUIRE(row.Values.size() == 5);
	CHECK(row.Values[0].Data == Value{std::string("pc.cache")});
	CHECK(std::get<int64_t>(row.Values[1].Data) == -4);
	const auto *nested = std::get_if<ArrayValue>(&row.Values[2].Data);
	REQUIRE(nested);
	REQUIRE(nested->Items.size() == 3);
	const auto *first = std::get_if<Image>(&nested->Items[0].Data);
	REQUIRE(first);
	CHECK(first->Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	CHECK(std::get<ElementValue>(nested->Items[1].Data) == ElementValue{int64_t{-4}});
	const auto *empty = std::get_if<std::vector<SourceArrayItem>>(&nested->Items[2].Data);
	REQUIRE(empty);
	CHECK(empty->empty());
	CHECK(std::get<ArrayValue>(row.Values[3].Data).Items.empty());
	CHECK(
		std::get<SurfaceValue>(row.Values[4].Data).Data.Pixels ==
		std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255}
	);
	CHECK(row.LoadedCacheData == Nested);
}

TEST_CASE(
	"Saved source frame cache applies explicit channel and row layouts", "[imagegraphio][source_frame_cache]"
) {
	auto document = Scene("pc.cache", std::string(Vertical));
	const auto &node = document.Nodes[1];
	const std::array<std::pair<bake::SpriteCacheLayout, std::vector<uint8_t>>, 4> expected{{
		{bake::SpriteCacheLayout::Rgba8TopDown, {10, 20, 30, 40, 100, 110, 120, 130}},
		{bake::SpriteCacheLayout::Bgra8TopDown, {30, 20, 10, 40, 120, 110, 100, 130}},
		{bake::SpriteCacheLayout::Rgba8BottomUp, {100, 110, 120, 130, 10, 20, 30, 40}},
		{bake::SpriteCacheLayout::Bgra8BottomUp, {120, 110, 100, 130, 30, 20, 10, 40}},
	}};
	for (const auto &[layout, pixels] : expected) {
		const auto row = Decode(node, Vertical, layout);
		REQUIRE(row.Values.size() == 3);
		const auto &image = std::get<SurfaceValue>(row.Values[2].Data).Data;
		CHECK(image.Width == 1);
		CHECK(image.Height == 2);
		CHECK(image.Pixels == pixels);
	}
	CapturedFeedbackHost host;
	const auto plan = Compiled(document);
	for (const auto &[layout, pixels] : expected) {
		DataReplayState loads{{Decode(node, Vertical, layout)}};
		host.Clear();
		EvaluationRequest request;
		request.SourceCachePlayback =
			SourceCachePlaybackObservation{false, SourceCacheSampling::ObservedFrame, true};
		request.SourceFrameCacheLoads = &loads;
		Diagnostic diagnostic;
		const auto ready =
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out");
		INFO(diagnostic.Message);
		REQUIRE(ready);
		REQUIRE(request.DataReplay);
		const auto *output = SourceFrameCacheLastOutput(request.DataReplay->Entries[0]);
		REQUIRE(output);
		CHECK(std::get<SurfaceValue>(*output).Data.Pixels == pixels);
	}
}

TEST_CASE(
	"Saved source frame cache rejects stale identity and malformed data atomically",
	"[imagegraphio][source_frame_cache]"
) {
	auto document = Scene("pc.cache", std::string(Single));
	const auto &node = document.Nodes[1];
	DataReplayEntry result;
	result.NodeId = "prior";
	result.Values.push_back({7, int64_t{91}});
	const auto prior = result;
	Diagnostic diagnostic;
	auto observation = Observation(node, Single, bake::SpriteCacheLayout::Rgba8TopDown);
	for (const auto invalid : {
			 SourceFrameCacheLayoutObservation{"other", observation.DataHash, observation.Layout},
			 SourceFrameCacheLayoutObservation{node.Id, std::string(64, '0'), observation.Layout},
			 SourceFrameCacheLayoutObservation{
				 node.Id, observation.DataHash, static_cast<bake::SpriteCacheLayout>(255)
			 },
		 }) {
		CHECK(DecodeSourceFrameCache(node, invalid, result, diagnostic) == Status::InvalidValue);
		CHECK(result == prior);
	}
	auto malformed = Scene("pc.cache", R"cache([{"width":1,"height":1,"buffer":"not-zlib"}])cache");
	const auto malformedObservation = Observation(
		malformed.Nodes[1],
		std::get<std::string>(malformed.Nodes[1].SourceProperties[1].Data),
		bake::SpriteCacheLayout::Rgba8TopDown
	);
	CHECK(
		DecodeSourceFrameCache(malformed.Nodes[1], malformedObservation, result, diagnostic) ==
		Status::Malformed
	);
	CHECK(result == prior);
	CHECK(DecodeSourceFrameCache(node, observation, result, diagnostic, 32) == Status::LimitExceeded);
	CHECK(result == prior);
	CHECK(DecodeSourceFrameCache(node, observation, result, diagnostic, 65536) == Status::LimitExceeded);
	CHECK(result == prior);
}

TEST_CASE("Loaded source cache receipts seed owned stateful outputs", "[imagegraphio][source_frame_cache]") {
	{
		auto document = Scene("pc.cache", std::string(Single));
		DataReplayState loads{{Decode(document.Nodes[1], Single, bake::SpriteCacheLayout::Rgba8TopDown)}};
		auto result = Evaluate(document, loads);
		const auto &image = std::get<Image>(result.Output);
		CHECK(image.Pixels == std::vector<uint8_t>{12, 34, 56, 78});
		REQUIRE(result.Data.Entries.size() == 1);
		CHECK(result.Data.Entries[0].LoadedCacheData == Single);
		loads.Entries.clear();
		CHECK(std::get<Image>(result.Output).Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	}
	{
		auto document = Scene("pc.cache_array", std::string(Frames));
		DataReplayState loads{{Decode(document.Nodes[1], Frames, bake::SpriteCacheLayout::Rgba8TopDown)}};
		auto result = Evaluate(document, loads);
		const auto &array = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
		REQUIRE(array.Items.size() == 3);
		CHECK(std::get<Image>(array.Items[0].Data).Pixels == std::vector<uint8_t>{12, 34, 56, 78});
		CHECK(std::get<ElementValue>(array.Items[1].Data) == ElementValue{int64_t{-1}});
		CHECK(
			std::get<Image>(array.Items[2].Data).Pixels == std::vector<uint8_t>{1, 2, 3, 255, 1, 2, 3, 255}
		);
		REQUIRE(result.Data.Entries.size() == 1);
		CHECK(result.Data.Entries[0].LoadedCacheData == Frames);
		loads.Entries.clear();
		CHECK(result.Data.Entries[0].LoadedCacheData == Frames);
	}
}

TEST_CASE("Serialize false ignores stale saved source cache text", "[imagegraphio][source_frame_cache]") {
	auto document = Scene("pc.cache", "not-json", false);
	DataReplayState unusedLoads;
	EvaluationRequest request;
	request.Tick = 0;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{false, SourceCacheSampling::ObservedFrame, true};
	request.SourceFrameCacheLoads = &unusedLoads;
	StatefulEvaluationResult result;
	Diagnostic diagnostic;
	const auto status = EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::get<int64_t>(std::get<EvaluatedValue>(result.Output).Data) == -4);
	REQUIRE(result.Data.Entries.size() == 1);
	CHECK(result.Data.Entries[0].LoadedCacheData == "not-json");
}

TEST_CASE(
	"Bulk source cache decoding publishes only explicitly observed enabled payloads",
	"[imagegraphio][source_frame_cache]"
) {
	auto document = Scene("pc.cache", std::string(Single));
	document.Nodes.push_back({"unobserved", "pc.cache", "", {}, {{"animated", false}}});
	document.Nodes.back().SourceProperties = {{"serialize", true}, {"cache", std::string(Single)}};
	document.Nodes.push_back({"disabled", "pc.cache", "", {}, {{"animated", false}}});
	document.Nodes.back().SourceProperties = {{"serialize", false}, {"cache", "not-json"}};
	document.Nodes.push_back({"empty", "pc.cache", "", {}, {{"animated", false}}});
	document.Nodes.back().SourceProperties = {{"serialize", false}, {"cache", std::string{}}};
	const auto observation = Observation(document.Nodes[1], Single, bake::SpriteCacheLayout::Rgba8TopDown);
	DataReplayState result;
	Diagnostic diagnostic;
	REQUIRE(DecodeSourceFrameCaches(document, std::span(&observation, 1), result, diagnostic) == Status::Ok);
	REQUIRE(result.Entries.size() == 1);
	CHECK(result.Entries[0].NodeId == "cache");
	const DataReplayState prior{{DataReplayEntry{"prior"}}};
	result = prior;
	const std::array duplicate{observation, observation};
	CHECK(DecodeSourceFrameCaches(document, duplicate, result, diagnostic) == Status::DuplicateId);
	CHECK(result == prior);
}

TEST_CASE(
	"Retained source frame-cache receipts outlive the loader and bind to current authored text",
	"[imagegraphio][source_frame_cache]"
) {
	auto original = Scene("pc.cache", std::string(Single));
	DataReplayState loads{{Decode(original.Nodes[1], Single, bake::SpriteCacheLayout::Rgba8TopDown)}};
	auto first = Evaluate(original, loads);
	EvaluationRequest retainedRequest;
	retainedRequest.Tick = 0;
	retainedRequest.SourceCachePlayback =
		SourceCachePlaybackObservation{true, SourceCacheSampling::ObservedFrame, true};
	retainedRequest.DataReplay = &first.Data;
	StatefulEvaluationResult retained;
	Diagnostic diagnostic;
	REQUIRE(
		EvaluateStateful(original, Compiled(original), "out", retainedRequest, retained, diagnostic) ==
		Status::Ok
	);
	CHECK(std::get<Image>(retained.Output).Pixels == std::vector<uint8_t>{12, 34, 56, 78});

	auto changed = Scene("pc.cache", std::string(Frames));
	retainedRequest.DataReplay = &first.Data;
	StatefulEvaluationResult unchanged = retained;
	CHECK(
		EvaluateStateful(changed, Compiled(changed), "out", retainedRequest, unchanged, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(std::get<Image>(unchanged.Output) == std::get<Image>(retained.Output));
	DataReplayState staleLoads = loads;
	retainedRequest.DataReplay = nullptr;
	retainedRequest.SourceFrameCacheLoads = &staleLoads;
	CHECK(
		EvaluateStateful(changed, Compiled(changed), "out", retainedRequest, unchanged, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(std::get<Image>(unchanged.Output) == std::get<Image>(retained.Output));
	DataReplayState replacement{{Decode(changed.Nodes[1], Frames, bake::SpriteCacheLayout::Rgba8TopDown)}};
	retainedRequest.SourceFrameCacheLoads = &replacement;
	REQUIRE(
		EvaluateStateful(changed, Compiled(changed), "out", retainedRequest, unchanged, diagnostic) ==
		Status::Ok
	);
	CHECK(std::get<Image>(unchanged.Output).Pixels == std::vector<uint8_t>{12, 34, 56, 78});
}

TEST_CASE(
	"Frame-cache overlay removes loaded histories after authored text changes",
	"[imagegraphio][source_frame_cache]"
) {
	auto original = Scene("pc.cache", std::string(Single));
	DataReplayState retained{{Decode(original.Nodes[1], Single, bake::SpriteCacheLayout::Rgba8TopDown)}};
	auto changed = Scene("pc.cache", std::string(Frames));
	DataReplayState target = retained;
	Diagnostic diagnostic;
	CHECK(
		OverlaySourceFrameCacheRows(
			changed, retained, target, FrameCacheOutputPolicy::RetainedObservation, diagnostic
		) == Status::Ok
	);
	CHECK(target.Entries.empty());
}

TEST_CASE(
	"Cooked source frame cache round trips and plays sparse native frames",
	"[imagegraphio][source_frame_cache]"
) {
	auto cooked = Cook(Scene("pc.cache", std::string(Frames)));
	const auto &properties = cooked.Nodes[1].SourceProperties;
	CHECK(std::count_if(properties.begin(), properties.end(), [](const auto &property) {
			  return property.Port == "composer_frame_cache_text";
		  }) == 1);
	CHECK(std::count_if(properties.begin(), properties.end(), [](const auto &property) {
			  return property.Port == "composer_frame_cache_data";
		  }) == 1);
	Document roundTrip;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(cooked), roundTrip, diagnostic) == Status::Ok);
	const auto plan = Compiled(roundTrip);
	(void)plan;
	CHECK(
		std::get<Image>(EvaluateCooked(roundTrip, 0).Output).Pixels == std::vector<uint8_t>{12, 34, 56, 78}
	);
	CHECK(
		std::get<Image>(EvaluateCooked(roundTrip, 2).Output).Pixels ==
		std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255}
	);
}

TEST_CASE(
	"Cooked nested source cache keeps array shape and cache-array capture replaces the current slot",
	"[imagegraphio][source_frame_cache]"
) {
	auto nested = Cook(Scene("pc.cache", std::string(Nested)));
	const auto nestedResult = EvaluateCooked(nested, 0);
	const auto &tree = std::get<ArrayValue>(std::get<EvaluatedValue>(nestedResult.Output).Data);
	REQUIRE(tree.Items.size() == 3);
	CHECK(std::get<Image>(tree.Items[0].Data).Pixels == std::vector<uint8_t>{12, 34, 56, 78});
	CHECK(std::get<ElementValue>(tree.Items[1].Data) == ElementValue{int64_t{-4}});
	const auto *empty = std::get_if<std::vector<SourceArrayItem>>(&tree.Items[2].Data);
	REQUIRE(empty);
	CHECK(empty->empty());

	auto array = Cook(Scene("pc.cache_array", std::string(Frames)));
	const auto live = EvaluateCooked(array, 0, true);
	const auto &slots = std::get<ArrayValue>(std::get<EvaluatedValue>(live.Output).Data);
	REQUIRE(slots.Items.size() == 3);
	CHECK(std::get<Image>(slots.Items[0].Data).Pixels == std::vector<uint8_t>{1, 2, 3, 255, 1, 2, 3, 255});
	CHECK(std::get<ElementValue>(slots.Items[1].Data) == ElementValue{int64_t{-1}});
	CHECK(
		std::get<Image>(slots.Items[2].Data).Pixels == std::vector<uint8_t>{200, 10, 40, 0, 255, 128, 64, 255}
	);
}

TEST_CASE("Cooked source cache supports each explicit byte layout", "[imagegraphio][source_frame_cache]") {
	auto source = Scene("pc.cache", std::string(Vertical));
	const std::array<std::pair<bake::SpriteCacheLayout, std::vector<uint8_t>>, 4> expected{{
		{bake::SpriteCacheLayout::Rgba8TopDown, {10, 20, 30, 40, 100, 110, 120, 130}},
		{bake::SpriteCacheLayout::Bgra8TopDown, {30, 20, 10, 40, 120, 110, 100, 130}},
		{bake::SpriteCacheLayout::Rgba8BottomUp, {100, 110, 120, 130, 10, 20, 30, 40}},
		{bake::SpriteCacheLayout::Bgra8BottomUp, {120, 110, 100, 130, 30, 20, 10, 40}},
	}};
	for (const auto &[layout, pixels] : expected) {
		const auto cooked = Cook(source, layout);
		const auto result = EvaluateCooked(cooked, 0);
		CHECK(std::get<Image>(result.Output).Pixels == pixels);
	}
}

TEST_CASE(
	"Source frame-cache cooking failures preserve destination and recooking replaces annotations",
	"[imagegraphio][source_frame_cache]"
) {
	auto source = Scene("pc.cache", std::string(Single));
	const auto observation = Observation(source.Nodes[1], Single, bake::SpriteCacheLayout::Rgba8TopDown);
	auto cooked = Cook(source);
	const auto cookedPrior = cooked;
	Diagnostic diagnostic;
	std::string staleHash = observation.DataHash;
	staleHash[0] = staleHash[0] == '0' ? '1' : '0';
	const SourceFrameCacheLayoutObservation stale{observation.NodeId, staleHash, observation.Layout};
	CHECK(CookSourceFrameCaches(source, std::span(&stale, 1), cooked, diagnostic) == Status::InvalidValue);
	CHECK(cooked == cookedPrior);
	const std::array duplicate{observation, observation};
	CHECK(CookSourceFrameCaches(source, duplicate, cooked, diagnostic) == Status::DuplicateId);
	CHECK(cooked == cookedPrior);
	const SourceFrameCacheLayoutObservation unknown{"absent", observation.DataHash, observation.Layout};
	CHECK(CookSourceFrameCaches(source, std::span(&unknown, 1), cooked, diagnostic) == Status::InvalidValue);
	CHECK(cooked == cookedPrior);
	CHECK(
		CookSourceFrameCaches(source, std::span(&observation, 1), cooked, diagnostic, 1) ==
		Status::LimitExceeded
	);
	CHECK(cooked == cookedPrior);
	auto replacementSource = Scene("pc.cache", std::string(Frames));
	const auto replacementObservation =
		Observation(replacementSource.Nodes[1], Frames, bake::SpriteCacheLayout::Rgba8TopDown);
	REQUIRE(
		CookSourceFrameCaches(replacementSource, std::span(&replacementObservation, 1), cooked, diagnostic) ==
		Status::Ok
	);
	CHECK(
		std::count_if(
			cooked.Nodes[1].SourceProperties.begin(),
			cooked.Nodes[1].SourceProperties.end(),
			[](const auto &property) { return property.Port == "composer_frame_cache_text"; }
		) == 1
	);
	CHECK(
		std::count_if(
			cooked.Nodes[1].SourceProperties.begin(),
			cooked.Nodes[1].SourceProperties.end(),
			[](const auto &property) { return property.Port == "composer_frame_cache_data"; }
		) == 1
	);
	const auto staleCooked = EvaluateCooked(cooked, 0);
	auto edited = cooked;
	std::get<std::string>(edited.Nodes[1].SourceProperties[1].Data).push_back(' ');
	StatefulEvaluationResult priorOutput = staleCooked;
	EvaluationRequest request;
	request.SourceCachePlayback =
		SourceCachePlaybackObservation{false, SourceCacheSampling::ObservedFrame, true};
	CHECK(
		EvaluateStateful(edited, Compiled(edited), "out", request, priorOutput, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(std::get<Image>(priorOutput.Output) == std::get<Image>(staleCooked.Output));
	REQUIRE(
		CookSourceFrameCaches(
			replacementSource, std::span(&replacementObservation, 1), replacementSource, diagnostic
		) == Status::Ok
	);
	CHECK(replacementSource == cooked);
}
