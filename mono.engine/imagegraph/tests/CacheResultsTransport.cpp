#include <engine/imagegraph/CacheResultsReplay.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.cache_results_transport")
TEST_DEPENDS("engine.imagegraph.cache_results_replay")
using namespace engine::imagegraph;
namespace {
	Document Scene(double selected = 0) {
		Document d;
		d.FormatVersion = 9;
		d.Timeline = TimelineSettings{5, 0, 4, "loop", 24};
		d.Project = ProjectSettings{};
		d.Project->SurfaceWidth = 2;
		d.Project->SurfaceHeight = 1;
		d.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{20, 40, 60, 255}}}},
			{"cache", "pc.cache_results", "", {}, {{"amount", int64_t{2}}}},
			{"sequence",
			 "pc.sequence_anim",
			 "",
			 {},
			 {{"speed", 0.}, {"sequence", ArrayValue{ValueType::Scalar, {selected}}}}}
		};
		d.Links = {
			{"source", "image", "cache", "surface_in"}, {"cache", "cache_surfaces", "sequence", "surface_in"}
		};
		d.Outputs = {{"list", "cache", "cache_surfaces"}, {"selected", "sequence", "surface_out"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	StatefulEvaluationResult Run(const Document &d, const std::string &id, EvaluationRequest request = {}) {
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		const auto status = EvaluateStateful(d, Compiled(d), id, request, result, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
	const ArrayValue &Record(const DataReplayState &state) {
		REQUIRE(state.Entries.size() == 1);
		REQUIRE(state.Entries[0].Values.size() == 1);
		return std::get<ArrayValue>(state.Entries[0].Values[0].Data);
	}
	DataReplayState Cleared(const Document &d) {
		auto first = Run(d, "list");
		EvaluationRequest clock;
		clock.DataReplay = &first.Data;
		auto second = Run(d, "list", clock);
		REQUIRE(Record(second.Data).Elements.size() == 2);
		DataReplayState cleared;
		Diagnostic diagnostic;
		REQUIRE(ClearCacheResultsReplay(second.Data, "cache", cleared, diagnostic) == Status::Ok);
		REQUIRE(Record(cleared).ElementType == ValueType::Struct);
		for (const auto &slot : Record(cleared).Elements)
			REQUIRE(IsFreedCacheResultsSlot(slot));
		return cleared;
	}
	const Image &ImageItem(const ArrayValue &array, size_t index) {
		REQUIRE(array.Items.size() > index);
		if (const auto *image = std::get_if<Image>(&array.Items[index].Data)) return *image;
		return std::get<SurfaceValue>(std::get<ElementValue>(array.Items[index].Data)).Data;
	}
	bool FreedItem(const ArrayValue &array, size_t index) {
		REQUIRE(array.Items.size() > index);
		const auto *leaf = std::get_if<ElementValue>(&array.Items[index].Data);
		return leaf && IsFreedCacheResultsSlot(*leaf);
	}
}
TEST_CASE(
	"Cache Results publishes and owns valid and freed positions in source order", "[cache_results_transport]"
) {
	const auto d = Scene();
	const auto cleared = Cleared(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.Subframe = .5;
	clock.NegativeFrame = true;
	clock.DataReplay = &cleared;
	auto result = Run(d, "list", clock);
	const auto &value = std::get<EvaluatedValue>(result.Output);
	REQUIRE(value.Domain);
	CHECK(value.Domain->Type == ValueType::Image);
	CHECK(value.Domain->Kind == SourceSocketKind::Surface);
	const auto &out = std::get<ArrayValue>(value.Data);
	REQUIRE(out.ElementType == ValueType::Any);
	REQUIRE(out.Items.size() == 2);
	CHECK(out.Elements.empty());
	CHECK(out.Nested.empty());
	const auto &pixels = ImageItem(out, 0);
	CHECK(pixels.Width == 2);
	CHECK(pixels.Height == 1);
	CHECK(pixels.Pixels == std::vector<uint8_t>{20, 40, 60, 255, 20, 40, 60, 255});
	CHECK(pixels.Hash == SurfaceHash(pixels));
	CHECK(FreedItem(out, 1));
	CHECK(FreedItem(Record(result.Data), 1));
	CHECK(result.Data.Entries[0].PreviousValue == 0);
	CHECK(result.Data.Entries[0].PreviousFrame == -1.5);
	CHECK(IsFreedCacheResultsSlot(Record(cleared).Elements[0]));
	Diagnostic diagnostic;
	CHECK(ValidateDataReplay(result.Data, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	// The publication, retained journal and caller's preceding generation are independent owners.
	auto &copy = std::get<ArrayValue>(std::get<EvaluatedValue>(result.Output).Data);
	std::get<SurfaceValue>(std::get<ElementValue>(copy.Items[0].Data)).Data.Pixels[0] = 99;
	CHECK(ImageItem(Record(result.Data), 0).Pixels[0] == 20);
}
TEST_CASE("Sequence diagnoses only a selected freed Cache Results slot", "[cache_results_transport]") {
	auto d = Scene();
	const auto cleared = Cleared(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &cleared;
	auto result = Run(d, "selected", clock);
	const auto good = std::get<Image>(result.Output);
	const auto retained = result.Data;
	REQUIRE(good.Pixels[0] == 20);
	REQUIRE(FreedItem(Record(retained), 1));
	d.Nodes[2].Values[1].Data = ArrayValue{ValueType::Scalar, {1.}};
	Diagnostic diagnostic;
	CHECK(
		EvaluateStateful(d, Compiled(d), "selected", clock, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "sequence");
	CHECK(diagnostic.Port == "surface_in");
	CHECK(std::get<Image>(result.Output) == good);
	CHECK(result.Data == retained);
	// Array Get forwards the same marker as a scalar Value, exercising the nonarray path.
	d.Nodes.push_back({"get", "pc.array_get", "", {}, {{"index", int64_t{1}}}});
	d.Links[1] = {"get", "value", "sequence", "surface_in"};
	d.Links.push_back({"cache", "cache_surfaces", "get", "array"});
	CHECK(
		EvaluateStateful(d, Compiled(d), "selected", clock, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(diagnostic.NodeId == "sequence");
	CHECK(result.Data == retained);
	d.Nodes.back().Values[0].Data = int64_t{0};
	result = Run(d, "selected", clock);
	CHECK(std::get<Image>(result.Output) == good);
}
TEST_CASE(
	"Cache Results refills freed cycles in the exact source first-frame rotation order",
	"[cache_results_transport]"
) {
	const auto d = Scene();
	const auto cleared = Cleared(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &cleared;
	auto result = Run(d, "list", clock);
	const auto mixed = result.Data;
	clock.Tick = 0;
	clock.DataReplay = &mixed;
	result = Run(d, "list", clock);
	REQUIRE(FreedItem(Record(result.Data), 0));
	CHECK(ImageItem(Record(result.Data), 1).Pixels[0] == 20);
	CHECK(result.Data.Entries[0].PreviousValue == 1);
	const auto rotated = result.Data;
	clock.DataReplay = &rotated;
	result = Run(d, "list", clock);
	REQUIRE(Record(result.Data).Elements.size() == 2);
	CHECK(Record(result.Data).ElementType == ValueType::Image);
	const auto &images = std::get<ImageArray>(result.Output).Images;
	REQUIRE(images.size() == 2);
	for (const auto &image : images)
		CHECK(image.Pixels[0] == 20);
	CHECK(FreedItem(Record(mixed), 1));
	CHECK(FreedItem(Record(rotated), 0));
}
TEST_CASE(
	"Mixed Cache Results clear and tight replacement budgets preserve previous ownership",
	"[cache_results_transport]"
) {
	const auto d = Scene();
	const auto cleared = Cleared(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &cleared;
	auto result = Run(d, "list", clock);
	const auto mixed = result.Data;
	DataReplayState output = mixed;
	Diagnostic diagnostic;
	const uint64_t cap = RetainedDataReplayBytes(mixed) + ClearedCacheResultsReplayBytes(mixed, "cache") +
						 mixed.Entries.size() * sizeof(size_t) + RetainedDataReplayBytes(output) -
						 sizeof(DataReplayState);
	CHECK(ClearCacheResultsReplay(mixed, "cache", output, diagnostic, cap - 1) == Status::LimitExceeded);
	CHECK(output == mixed);
	REQUIRE(ClearCacheResultsReplay(mixed, "cache", output, diagnostic, cap) == Status::Ok);
	REQUIRE(Record(output).Elements.size() == 2);
	CHECK(Record(output).Items.empty());
	CHECK(Record(output).ElementType == ValueType::Struct);
	CHECK(IsFreedCacheResultsSlot(Record(output).Elements[0]));
	CHECK(IsFreedCacheResultsSlot(Record(output).Elements[1]));
	CHECK(ImageItem(Record(mixed), 0).Pixels[0] == 20);
	const auto pixels = std::get<EvaluatedValue>(result.Output);
	CHECK(EvaluateStateful(d, Compiled(d), "list", clock, result, diagnostic, 4096) == Status::LimitExceeded);
	CHECK(std::get<EvaluatedValue>(result.Output) == pixels);
	CHECK(result.Data == mixed);
}
TEST_CASE(
	"Cache Results refuses malformed or nested retained slots without publishing", "[cache_results_transport]"
) {
	const auto d = Scene();
	const auto cleared = Cleared(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &cleared;
	auto result = Run(d, "list", clock);
	const auto original = result.Data;
	const auto good = std::get<EvaluatedValue>(result.Output);
	for (const bool nested : {false, true}) {
		auto invalid = original;
		auto &array = std::get<ArrayValue>(invalid.Entries[0].Values[0].Data);
		if (nested)
			array.Items[1].Data = std::vector<SourceArrayItem>{{ImageItem(array, 0)}};
		else {
			auto &record = std::get<StructValue>(std::get<ElementValue>(array.Items[1].Data));
			record.Data->Fields[0].second = false;
		}
		clock.DataReplay = &invalid;
		Diagnostic diagnostic;
		CHECK(EvaluateStateful(d, Compiled(d), "list", clock, result, diagnostic) == Status::InvalidValue);
		CHECK(result.Data == original);
		CHECK(std::get<EvaluatedValue>(result.Output) == good);
		DataReplayState output = original;
		CHECK(ClearCacheResultsReplay(invalid, "cache", output, diagnostic) == Status::InvalidValue);
		CHECK(output == original);
	}
}
TEST_CASE(
	"Captured Cache Results list seeks and valid selectors retain mixed slot histories",
	"[cache_results_transport]"
) {
	for (const bool selectPixels : {false, true}) {
		auto d = Scene();
		d.Timeline->First = 1;
		const auto p = Compiled(d);
		const std::string selected = selectPixels ? "selected" : "list";
		CapturedFeedbackHost host;
		EvaluationRequest clock;
		Diagnostic diagnostic;
		const auto prepare = [&] {
			const bool ok =
				host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, selected);
			INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
			REQUIRE(ok);
		};
		prepare();
		clock.Tick = 1;
		prepare();
		REQUIRE(Record(*clock.DataReplay).Elements.size() == 2);
		REQUIRE(host.ClearSourceCache(d, p, "cache", 1, 1, diagnostic));
		clock.Tick = 2;
		prepare();
		REQUIRE(FreedItem(Record(*clock.DataReplay), 1));
		if (selectPixels) {
			REQUIRE(host.Output(selected));
			CHECK(host.Output(selected)->Pixels[0] == 20);
			const auto before = *clock.DataReplay;
			const auto image = *host.Output(selected);
			clock.Tick = 1;
			CHECK_FALSE(
				host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, selected)
			);
			CHECK(diagnostic.Code == Status::UnsupportedExecution);
			CHECK(*clock.DataReplay == before);
			CHECK(*host.Output(selected) == image);
		} else {
			clock.Tick = 0;
			prepare();
			clock.Tick = 1;
			prepare();
			REQUIRE(FreedItem(Record(*clock.DataReplay), 0));
			host.RestartCycle();
			prepare();
			REQUIRE(Record(*clock.DataReplay).Elements.size() == 2);
			for (const auto &slot : Record(*clock.DataReplay).Elements)
				CHECK(std::get<SurfaceValue>(slot).Data.Pixels[0] == 20);
		}
	}
}

TEST_CASE(
	"Mixed Cache Results Amount edits retain source slots and current draw index", "[cache_results_transport]"
) {
	auto document = Scene();
	const auto cleared = Cleared(document);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &cleared;
	const auto mixed = Run(document, "list", clock).Data;
	document.Nodes[1].Values[0].Data = int64_t{1};
	clock.DataReplay = &mixed;
	auto result = Run(document, "list", clock);
	REQUIRE(Record(result.Data).Elements.size() == 1);
	CHECK(std::get<ImageArray>(result.Output).Images[0].Pixels[0] == 20);
	document.Nodes[1].Values[0].Data = int64_t{3};
	clock.Tick = 0;
	result = Run(document, "list", clock);
	REQUIRE(Record(result.Data).Items.size() == 3);
	CHECK(ImageItem(Record(result.Data), 0).Pixels[0] == 20);
	CHECK(FreedItem(Record(result.Data), 1));
	CHECK(ImageItem(Record(result.Data), 2).Pixels[0] == 20);
	CHECK(result.Data.Entries[0].PreviousValue == 2);
	const auto grown = result.Data;
	clock.Tick = 1;
	clock.DataReplay = &grown;
	result = Run(document, "list", clock);
	CHECK(result.Data.Entries[0].PreviousValue == 2);
	CHECK(Record(result.Data).Items.size() == 3);
	CHECK(FreedItem(Record(result.Data), 1));
	CHECK(FreedItem(Record(mixed), 1));
	Diagnostic diagnostic;
	DataReplayState freed;
	REQUIRE(ClearCacheResultsReplay(result.Data, "cache", freed, diagnostic) == Status::Ok);
	CHECK(Record(freed).Elements.size() == 3);
	CHECK(freed.Entries[0].PreviousValue == 0);
	for (const auto &slot : Record(freed).Elements)
		CHECK(IsFreedCacheResultsSlot(slot));
}

TEST_CASE(
	"Mixed Cache Results redraw converts captured formats and changes only its current slot shape",
	"[cache_results_transport]"
) {
	auto document = Scene();
	const auto cleared = Cleared(document);
	document.Nodes[0] = {"source", "image.captured", "", {}, {{"source_id", std::string{"input"}}}};
	for (const auto format : {SurfaceFormat::RGBA32Float, SurfaceFormat::R32Float}) {
		RequestImageSource source;
		source.SourceId = "input";
		source.Data.Width = 1;
		source.Data.Height = 2;
		source.Data.Format = format;
		const auto layout = CheckedSurfaceLayout(1, 2, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		source.Data.Pixels.resize(layout->Bytes);
		const bool mono = format == SurfaceFormat::R32Float;
		const SurfacePixel pixel = mono ? SurfacePixel{.5, 0, 0, 1} : SurfacePixel{0, .5, 1, .25};
		REQUIRE(StoreSurfacePixel(source.Data, 0, 0, pixel));
		REQUIRE(StoreSurfacePixel(source.Data, 0, 1, pixel));
		source.Data.Hash = SurfaceHash(source.Data);
		const auto original = source.Data;
		EvaluationRequest clock;
		clock.Tick = 1;
		clock.DataReplay = &cleared;
		clock.ImageSources = std::span<const RequestImageSource>(&source, 1);
		auto result = Run(document, "selected", clock);
		const auto &image = std::get<Image>(result.Output);
		const std::vector<uint8_t> expected = mono ? std::vector<uint8_t>{128, 0, 0, 255, 128, 0, 0, 255}
												   : std::vector<uint8_t>{0, 128, 255, 64, 0, 128, 255, 64};
		CHECK(image.Width == 1);
		CHECK(image.Height == 2);
		CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
		CHECK(image.Pixels == expected);
		CHECK(image.Hash == SurfaceHash(image));
		CHECK(FreedItem(Record(result.Data), 1));
		std::get<Image>(result.Output).Pixels[0] = 99;
		CHECK(ImageItem(Record(result.Data), 0).Pixels == expected);
		CHECK(source.Data == original);
		CHECK(IsFreedCacheResultsSlot(Record(cleared).Elements[0]));
	}
}
