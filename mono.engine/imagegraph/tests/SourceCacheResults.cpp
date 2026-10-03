#include "NodeExecutors.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_cache_results")
TEST_DEPENDS("engine.imagegraph.source_sequence_animation")
namespace {
	using namespace engine::imagegraph;
	Document Scene(int64_t amount = 2) {
		Document d;
		d.FormatVersion = 9;
		d.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
		d.Nodes = {
			{"source",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 0, 0, 255}}}},
			{"cache", "pc.cache_results", "", {}, {{"amount", amount}}}
		};
		d.Keyframes = {
			{"source", "colour", 0, Colour{10, 0, 0, 255}, "step"},
			{"source", "colour", 1, Colour{20, 0, 0, 255}, "step"},
			{"source", "colour", 2, Colour{30, 0, 0, 255}, "step"}
		};
		d.Links = {{"source", "image", "cache", "surface_in"}};
		d.Outputs = {{"out", "cache", "cache_surfaces"}};
		return d;
	}
	Plan CompileNow(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	StatefulEvaluationResult Run(const Document &d, const Plan &p, EvaluationRequest request = {}) {
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		const auto status = EvaluateStateful(d, p, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
	std::vector<uint8_t> Colours(const StatefulEvaluationResult &r) {
		const auto &images = std::get<ImageArray>(r.Output);
		std::vector<uint8_t> colours;
		for (const auto &item : images.Items) {
			const auto &image = images.Images.at(std::get<size_t>(item.Data));
			CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
			CHECK(image.Hash == SurfaceHash(image));
			colours.push_back(image.Pixels[0]);
		}
		return colours;
	}
	const ArrayValue &Retained(const StatefulEvaluationResult &r) {
		REQUIRE(r.Data.Entries.size() == 1);
		REQUIRE(r.Data.Entries[0].Values.size() == 1);
		return std::get<ArrayValue>(r.Data.Entries[0].Values[0].Data);
	}
}
TEST_CASE(
	"Cache Results retains completed cycles and rotates the source Amount list",
	"[imagegraph][source_cache_results]"
) {
	const auto d = Scene();
	const auto p = CompileNow(d);
	auto result = Run(d, p);
	CHECK(Colours(result) == std::vector<uint8_t>{10});
	EvaluationRequest clock;
	clock.DataReplay = &result.Data;
	clock.Tick = 1;
	auto next = Run(d, p, clock);
	CHECK(Colours(next) == std::vector<uint8_t>{20});
	CHECK(Colours(result) == std::vector<uint8_t>{10});
	clock.DataReplay = &next.Data;
	clock.Tick = 0;
	result = Run(d, p, clock);
	CHECK(Colours(result) == std::vector<uint8_t>{20, 10});
	clock.DataReplay = &result.Data;
	clock.Tick = 2;
	next = Run(d, p, clock);
	CHECK(Colours(next) == std::vector<uint8_t>{20, 30});
	clock.DataReplay = &next.Data;
	clock.Tick = 0;
	result = Run(d, p, clock);
	CHECK(Colours(result) == std::vector<uint8_t>{30, 10});
	CHECK(Retained(result).Elements.size() == 2);
	CHECK(result.Data.Entries[0].PreviousValue == 1);
	std::get<ImageArray>(result.Output).Images[0].Pixels[0] = 99;
	CHECK(std::get<SurfaceValue>(Retained(result).Elements[0]).Data.Pixels[0] == 30);
}
TEST_CASE(
	"Cache Results shrink keeps the source current index and growth starts at the next cycle",
	"[imagegraph][source_cache_results]"
) {
	auto d = Scene(3);
	auto p = CompileNow(d);
	auto first = Run(d, p);
	EvaluationRequest clock;
	clock.DataReplay = &first.Data;
	auto second = Run(d, p, clock);
	CHECK(Colours(second) == std::vector<uint8_t>{10, 10});
	d.Nodes[1].Values[0].Data = int64_t{1};
	p = CompileNow(d);
	clock.DataReplay = &second.Data;
	clock.Tick = 2;
	auto shrunk = Run(d, p, clock);
	// Source retains surfaceIndex=1 after truncating to Amount=1, then writes that slot again.
	CHECK(Colours(shrunk) == std::vector<uint8_t>{10, 30});
	CHECK(shrunk.Data.Entries[0].PreviousValue == 1);
	d.Nodes[1].Values[0].Data = int64_t{4};
	p = CompileNow(d);
	clock.DataReplay = &shrunk.Data;
	clock.Tick = 1;
	auto grown = Run(d, p, clock);
	CHECK(Colours(grown) == std::vector<uint8_t>{10, 20});
	clock.DataReplay = &grown.Data;
	clock.Tick = 0;
	const auto cycle = Run(d, p, clock);
	CHECK(Colours(cycle) == std::vector<uint8_t>{10, 20, 10});
}
TEST_CASE(
	"Cache Results observes exact signed fractional frame ordering without invented tick replay",
	"[imagegraph][source_cache_results]"
) {
	const auto d = Scene();
	const auto p = CompileNow(d);
	EvaluationRequest clock;
	clock.Tick = 2;
	auto current = Run(d, p, clock);
	CHECK(Colours(current) == std::vector<uint8_t>{30});
	clock.DataReplay = &current.Data;
	clock.Tick = 0;
	clock.Subframe = .5;
	auto fractional = Run(d, p, clock);
	CHECK(Colours(fractional) == std::vector<uint8_t>{10});
	CHECK(fractional.Data.Entries[0].Subframe == .5);
	clock.DataReplay = &fractional.Data;
	clock.Tick = 1;
	clock.NegativeFrame = true;
	auto negative = Run(d, p, clock);
	CHECK(Colours(negative) == std::vector<uint8_t>{10});
	CHECK(negative.Data.Entries[0].PreviousFrame == -1.5);
	clock.DataReplay = &negative.Data;
	clock = EvaluationRequest{.Tick = 0, .DataReplay = &negative.Data};
	const auto newCycle = Run(d, p, clock);
	CHECK(Colours(newCycle) == std::vector<uint8_t>{10, 10});
	CHECK(newCycle.Data.Entries[0].Subframe == 0);
}
TEST_CASE(
	"Cache Results host preserves same-frame output and owned cycles across seek and clear",
	"[imagegraph][source_cache_results]"
) {
	const auto d = Scene();
	const auto p = CompileNow(d);
	CapturedFeedbackHost host;
	Diagnostic diagnostic;
	EvaluationRequest clock;
	clock.Tick = 2;
	REQUIRE(host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(host.Active());
	REQUIRE(host.Value("out"));
	CHECK(std::get<ImageArray>(host.Value("out")->Output).Images[0].Pixels[0] == 30);
	const auto before = *clock.DataReplay;
	clock = {};
	clock.Tick = 2;
	REQUIRE(host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*clock.DataReplay == before);
	clock = {};
	REQUIRE(host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(clock.DataReplay);
	CHECK(std::get<ArrayValue>(clock.DataReplay->Entries[0].Values[0].Data).Elements.size() == 2);
	host.Clear();
	clock = {};
	REQUIRE(host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(std::get<ArrayValue>(clock.DataReplay->Entries[0].Values[0].Data).Elements.size() == 1);
	const auto good = *clock.DataReplay;
	clock = {};
	clock.Tick = 1;
	CHECK_FALSE(host.Prepare(d, p, 1, 1, clock, diagnostic, 1, "out"));
	CHECK(diagnostic.Code == Status::LimitExceeded);
	clock = {};
	REQUIRE(host.Prepare(d, p, 1, 1, clock, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*clock.DataReplay == good);
}
TEST_CASE(
	"Cache Results converts defined numeric formats into owned source-default RGBA8 surfaces",
	"[imagegraph][source_cache_results]"
) {
	auto d = Scene();
	d.Nodes[0] = {"source", "image.captured", "", {}, {{"source_id", std::string{"input"}}}};
	d.Keyframes.clear();
	const auto p = CompileNow(d);
	for (const auto format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA4Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		RequestImageSource source;
		source.SourceId = "input";
		source.Data.Width = 2;
		source.Data.Height = 1;
		source.Data.Format = format;
		const auto layout = CheckedSurfaceLayout(2, 1, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		source.Data.Pixels.resize(layout->Bytes);
		const bool single = DescribeSurfaceFormat(format)->Channels == 1;
		const SurfacePixel pixel = format == SurfaceFormat::RGBA4Unorm ? SurfacePixel{.2, .4, .6, .8}
								   : single							   ? SurfacePixel{.5, 0, 0, 1}
																	   : SurfacePixel{0, .5, 1, .25};
		REQUIRE(StoreSurfacePixel(source.Data, 0, 0, pixel));
		REQUIRE(StoreSurfacePixel(source.Data, 1, 0, pixel));
		source.Data.Hash = SurfaceHash(source.Data);
		EvaluationRequest clock;
		clock.ImageSources = std::span<const RequestImageSource>(&source, 1);
		auto result = Run(d, p, clock);
		const auto &out = std::get<ImageArray>(result.Output).Images[0];
		const std::vector<uint8_t> expected =
			format == SurfaceFormat::RGBA4Unorm ? std::vector<uint8_t>{51, 102, 153, 204, 51, 102, 153, 204}
			: single							? std::vector<uint8_t>{128, 0, 0, 255, 128, 0, 0, 255}
												: std::vector<uint8_t>{0, 128, 255, 64, 0, 128, 255, 64};
		CHECK(out.Format == SurfaceFormat::RGBA8Unorm);
		CHECK(out.Pixels == expected);
		CHECK(out.Hash == SurfaceHash(out));
		const auto original = source.Data;
		std::get<ImageArray>(result.Output).Images[0].Pixels[0] = 99;
		CHECK(source.Data == original);
		CHECK(std::get<SurfaceValue>(Retained(result).Elements[0]).Data.Pixels == expected);
		clock.DataReplay = &result.Data;
		source.Data.Width = 1;
		source.Data.Height = 2;
		source.Data.Hash = SurfaceHash(source.Data);
		const auto resized = Run(d, p, clock);
		const auto &list = std::get<ImageArray>(resized.Output);
		REQUIRE(list.Images.size() == 2);
		CHECK(list.Images[0].Width == 2);
		CHECK(list.Images[0].Height == 1);
		CHECK(list.Images[1].Width == 1);
		CHECK(list.Images[1].Height == 2);
	}
}
TEST_CASE(
	"Cache Results refuses sparse source slots and oversized history without changing prior results",
	"[imagegraph][source_cache_results]"
) {
	auto d = Scene(3);
	auto p = CompileNow(d);
	auto a = Run(d, p);
	EvaluationRequest clock;
	clock.DataReplay = &a.Data;
	auto b = Run(d, p, clock);
	clock.DataReplay = &b.Data;
	auto result = Run(d, p, clock);
	const auto prior = result.Data;
	const auto pixels = std::get<ImageArray>(result.Output);
	d.Nodes[1].Values[0].Data = int64_t{1};
	p = CompileNow(d);
	clock.DataReplay = &prior;
	clock.Tick = 1;
	Diagnostic diagnostic;
	CHECK(EvaluateStateful(d, p, "out", clock, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(result.Data == prior);
	CHECK(std::get<ImageArray>(result.Output).Images == pixels.Images);
	CHECK(std::get<ImageArray>(result.Output).Items == pixels.Items);
	d.Nodes[1].Values[0].Data = int64_t{Limits::MaximumArrayElements + 1};
	p = CompileNow(d);
	CHECK(EvaluateStateful(d, p, "out", clock, result, diagnostic) == Status::LimitExceeded);
	CHECK(result.Data == prior);
	CHECK(std::get<ImageArray>(result.Output).Images == pixels.Images);
	CHECK(std::get<ImageArray>(result.Output).Items == pixels.Items);
	d = Scene(0);
	p = CompileNow(d);
	clock.Tick = 0;
	CHECK(EvaluateStateful(d, p, "out", clock, result, diagnostic) == Status::InvalidValue);
	CHECK(result.Data == prior);
	CHECK(std::get<ImageArray>(result.Output).Images == pixels.Images);
	CHECK(std::get<ImageArray>(result.Output).Items == pixels.Items);
	d = Scene();
	p = CompileNow(d);
	CHECK(EvaluateStateful(d, p, "out", clock, result, diagnostic, 1) == Status::LimitExceeded);
	CHECK(result.Data == prior);
	CHECK(std::get<ImageArray>(result.Output).Images == pixels.Images);
	CHECK(std::get<ImageArray>(result.Output).Items == pixels.Items);
}
TEST_CASE(
	"Cache Results rejects first-frame unwritten storage and preadmits complete temporary lists",
	"[imagegraph][source_cache_results][evaluation_budget]"
) {
	const Node node{"cache", "pc.cache_results", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	DataReplayState history;
	EvaluationRequest clock;
	clock.DataReplay = &history;
	{
		detail::NodeContext context(node, *entry, clock);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"amount", int64_t{2}}, {"surface_in", int64_t{-4}}};
		CHECK_FALSE(executor(context));
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(history.Entries.empty());
	}
	Image input;
	input.Width = input.Height = 64;
	input.Pixels.resize(64 * 64 * 4, 255);
	input.Hash = SurfaceHash(input);
	ArrayValue retainedSurfaces{ValueType::Image, {}};
	for (uint8_t red : {uint8_t{40}, uint8_t{80}, uint8_t{120}}) {
		Image image = input;
		for (size_t pixel = 0; pixel < image.Pixels.size(); pixel += 4)
			image.Pixels[pixel] = red;
		image.Hash = SurfaceHash(image);
		retainedSurfaces.Elements.push_back(SurfaceValue{std::move(image)});
	}
	DataReplayEntry captured;
	captured.NodeId = "cache";
	captured.Initialized = true;
	captured.PreviousValue = 2;
	captured.Values.push_back({0, std::move(retainedSurfaces)});
	history.Entries.push_back(std::move(captured));
	const auto prior = history;
	clock.Tick = 1;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto retained = budget.Reserve(4096);
		REQUIRE(retained);
		const auto previous = input;
		{
			detail::NodeContext context(node, *entry, clock, budget);
			context.ByteBudget = limit;
			context.Values = {{"amount", int64_t{3}}};
			context.Images = {{"surface_in", &input}};
			const bool ok = executor(context);
			INFO(context.FailureMessage);
			if (attempt == 1) {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputImageArrays.empty());
				CHECK(context.DataUpdates.empty());
			} else {
				REQUIRE(ok);
				REQUIRE(context.OutputImageArrays.size() == 1);
				REQUIRE(context.DataUpdates.size() == 1);
				REQUIRE(context.OutputImageArrays[0].second.Images.size() == 3);
				CHECK(context.OutputImageArrays[0].second.Images[0].Pixels[0] == 40);
				CHECK(context.OutputImageArrays[0].second.Images[1].Pixels[0] == 80);
				CHECK(context.OutputImageArrays[0].second.Images[2] == input);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
			}
			CHECK(input == previous);
			CHECK(history == prior);
			CHECK(budget.Peak() <= limit);
		}
		CHECK(budget.Used() == retained->Bytes());
	}
}

TEST_CASE(
	"Cache Results nonsurface observations retain defined surfaces and refuse atlas approximations",
	"[imagegraph][source_cache_results]"
) {
	const auto d = Scene();
	const auto p = CompileNow(d);
	const auto retained = Run(d, p);
	EvaluationRequest clock;
	clock.Tick = 1;
	clock.DataReplay = &retained.Data;
	const auto *entry = FindCatalogueEntry("pc.cache_results");
	const auto executor = detail::FindExecutor("pc.cache_results");
	REQUIRE(entry);
	REQUIRE(executor);
	{
		detail::NodeContext context(d.Nodes[1], *entry, clock);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"amount", int64_t{2}}, {"surface_in", int64_t{-4}}};
		REQUIRE(executor(context));
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays[0].second.Images == std::get<ImageArray>(retained.Output).Images);
		REQUIRE(context.DataUpdates.size() == 1);
		CHECK(context.DataUpdates[0].Values == retained.Data.Entries[0].Values);
		CHECK(context.DataUpdates[0].Tick == 1);
	}
	{
		AtlasValue atlas;
		atlas.Data.emplace();
		atlas.Data->Surface = SurfaceValue{std::get<ImageArray>(retained.Output).Images[0]};
		atlas.Data->RotationDegrees = 90;
		detail::NodeContext context(d.Nodes[1], *entry, clock);
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		context.Values = {{"amount", int64_t{2}}, {"surface_in", atlas}};
		CHECK_FALSE(executor(context));
		CHECK(context.FailureCode == Status::UnsupportedExecution);
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.DataUpdates.empty());
	}
}
TEST_CASE(
	"Cache Results feeds retained cycle surfaces into the real source animation selector",
	"[imagegraph][source_cache_results]"
) {
	auto d = Scene();
	d.Nodes.push_back(
		{"select",
		 "pc.sequence_anim",
		 "",
		 {},
		 {{"overflow", EnumValue{1}}, {"speed", 1.}, {"sequence", ArrayValue{ValueType::Scalar, {}}}}}
	);
	d.Links.push_back({"cache", "cache_surfaces", "select", "surface_in"});
	d.Outputs = {{"out", "select", "surface_out"}};
	const auto p = CompileNow(d);
	auto first = Run(d, p);
	CHECK(std::get<Image>(first.Output).Pixels[0] == 10);
	EvaluationRequest clock;
	clock.Tick = 2;
	clock.DataReplay = &first.Data;
	auto completed = Run(d, p, clock);
	CHECK(std::get<Image>(completed.Output).Pixels[0] == 30);
	clock.Tick = 0;
	clock.DataReplay = &completed.Data;
	auto cycle = Run(d, p, clock);
	CHECK(std::get<Image>(cycle.Output).Pixels[0] == 30);
	CHECK(Retained(cycle).Elements.size() == 2);
	clock.Tick = 1;
	clock.DataReplay = &cycle.Data;
	const auto selected = Run(d, p, clock);
	CHECK(std::get<Image>(selected.Output).Pixels[0] == 20);
	CHECK(std::get<Image>(completed.Output).Pixels[0] == 30);
	CHECK(std::get<SurfaceValue>(Retained(selected).Elements[0]).Data.Pixels[0] == 30);
	CHECK(std::get<SurfaceValue>(Retained(selected).Elements[1]).Data.Pixels[0] == 20);
}
