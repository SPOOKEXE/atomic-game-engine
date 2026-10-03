#include "PixelBuilderPayload.hpp"
#include "nodes/ArraySource.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.frame_cache_replay")
using namespace engine::imagegraph;
namespace {
	Document Scene(bool array = false) {
		Document d;
		d.FormatVersion = 9;
		d.Timeline = TimelineSettings{6, 0, 5, "loop", 24};
		d.Project = ProjectSettings{};
		d.Project->SurfaceWidth = 2;
		d.Project->SurfaceHeight = 1;
		d.Nodes = {
			{"input",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{10, 20, 30, 255}}}},
			{"cache",
			 array ? "pc.cache_array" : "pc.cache",
			 "",
			 {},
			 array
				 ? std::vector<
					   AuthoredValue>{{"start_frame", int64_t{-1}}, {"stop_frame", int64_t{-1}}, {"step", int64_t{1}}}
				 : std::vector<AuthoredValue>{{"animated", false}}}
		};
		d.Links = {{"input", "image", "cache", "surface_in"}};
		d.Outputs = {{"out", "cache", array ? "cache_array" : "cache_surface"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic e;
		const auto code = Compile(d, p, e);
		INFO(e.Message);
		REQUIRE(code == Status::Ok);
		return p;
	}
	EvaluationRequest Clock(
		uint64_t tick, bool playing = true, SourceCacheSampling sampling = SourceCacheSampling::ObservedFrame
	) {
		EvaluationRequest q;
		q.Tick = tick;
		q.SourceCachePlayback = SourceCachePlaybackObservation{playing, sampling, true};
		return q;
	}
	StatefulEvaluationResult
	Run(const Document &d, EvaluationRequest q = {}, const DataReplayState *prior = nullptr) {
		q.DataReplay = prior;
		StatefulEvaluationResult r;
		Diagnostic e;
		const auto code = EvaluateStateful(d, Compiled(d), "out", q, r, e);
		INFO(e.NodeId << ":" << e.Port << " " << e.Message);
		REQUIRE(code == Status::Ok);
		return r;
	}
	const DataReplayEntry &Row(const DataReplayState &s) {
		REQUIRE(s.Entries.size() == 1);
		return s.Entries[0];
	}
	int Red(const StatefulEvaluationResult &r) {
		return std::get<Image>(r.Output).Pixels[0];
	}
	std::vector<int> Slots(const StatefulEvaluationResult &r) {
		if (const auto *images = std::get_if<ImageArray>(&r.Output)) {
			std::vector<int> values;
			for (const auto &i : images->Items) {
				REQUIRE(std::holds_alternative<size_t>(i.Data));
				values.push_back(images->Images[std::get<size_t>(i.Data)].Pixels[0]);
			}
			return values;
		}
		const auto &a = std::get<ArrayValue>(std::get<EvaluatedValue>(r.Output).Data);
		std::vector<int> values;
		if (!a.Items.empty())
			for (const auto &i : a.Items) {
				if (const auto *im = std::get_if<Image>(&i.Data))
					values.push_back(im->Pixels[0]);
				else {
					const auto &v = std::get<ElementValue>(i.Data);
					if (const auto *im = std::get_if<SurfaceValue>(&v))
						values.push_back(im->Data.Pixels[0]);
					else
						values.push_back(int(std::get<int64_t>(v)));
				}
			}
		else
			for (const auto &v : a.Elements) {
				if (const auto *im = std::get_if<SurfaceValue>(&v))
					values.push_back(im->Data.Pixels[0]);
				else
					values.push_back(int(std::get<int64_t>(v)));
			}
		return values;
	}
	void SameOutput(const StatefulEvaluationResult &a, const StatefulEvaluationResult &b) {
		REQUIRE(a.Output.index() == b.Output.index());
		if (const auto *image = std::get_if<Image>(&a.Output))
			CHECK(*image == std::get<Image>(b.Output));
		else if (const auto *images = std::get_if<ImageArray>(&a.Output)) {
			const auto &other = std::get<ImageArray>(b.Output);
			CHECK(images->Images == other.Images);
			CHECK(
				detail::source_array::FromImages(*images, images->Items) ==
				detail::source_array::FromImages(other, other.Items)
			);
		} else
			CHECK(std::get<EvaluatedValue>(a.Output).Data == std::get<EvaluatedValue>(b.Output).Data);
	}
	void ColourAt(Document &d, uint8_t red) {
		d.Nodes[0].Values[2].Data = Colour{red, 20, 30, 255};
	}
	const Value &Last(const DataReplayState &s) {
		const auto *v = SourceFrameCacheLastOutput(Row(s));
		REQUIRE(v);
		return *v;
	}
}
TEST_CASE(
	"Source Cache recovers owned frames and paused misses retain the last output",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	auto first = Run(d, Clock(0));
	CHECK(Red(first) == 10);
	CHECK(Row(first.Data).Values.size() == 3);
	const auto frozen = first.Data;
	ColourAt(d, 70);
	auto hit = Run(d, Clock(0), &first.Data);
	CHECK(Red(hit) == 10);
	CHECK(first.Data == frozen);
	auto paused = Run(d, Clock(2, false), &hit.Data);
	CHECK(Red(paused) == 10);
	CHECK(Row(paused.Data).Values.size() == 4);
	auto resume = Run(d, Clock(2), &paused.Data);
	CHECK(Red(resume) == 10);
	auto fresh = Run(d, Clock(3), &resume.Data);
	CHECK(Red(fresh) == 70);
	CHECK(Row(fresh.Data).Values.size() == 5);
	std::get<Image>(fresh.Output).Pixels[0] = 1;
	CHECK(std::get<SurfaceValue>(Last(fresh.Data)).Data.Pixels[0] == 70);
}
TEST_CASE(
	"Source Cache constructor noone and Animated scheduling state follow wrapper order",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	auto empty = Run(d, Clock(0, false));
	CHECK(std::get<int64_t>(std::get<EvaluatedValue>(empty.Output).Data) == -4);
	d.Nodes[1].Values[0].Data = true;
	auto hit = Run(d, Clock(0), &empty.Data);
	CHECK(std::get<int64_t>(std::get<EvaluatedValue>(hit.Output).Data) == -4);
	CHECK(Row(hit.Data).PreviousValue == 0);
	auto next = Run(d, Clock(1), &hit.Data);
	CHECK(Red(next) == 10);
	CHECK(Row(next.Data).PreviousValue == 1);
}
TEST_CASE(
	"Cache Array captures unsampled frames and publishes exact sparse minus-one positions",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	d.Nodes[1].Values[2].Data = int64_t{2};
	auto a = Run(d, Clock(1));
	CHECK(Slots(a) == std::vector<int>{-1, -1, -1});
	CHECK(Row(a.Data).Values.size() == 3);
	d.Nodes[1].Values[2].Data = int64_t{1};
	ColourAt(d, 40);
	auto b = Run(d, Clock(2), &a.Data);
	CHECK(Slots(b) == std::vector<int>{-1, 10, 40, -1, -1, -1});
	auto selected = d;
	selected.Nodes.push_back(
		{"pick",
		 "pc.sequence_anim",
		 "",
		 {},
		 {{"speed", 0.}, {"sequence", ArrayValue{ValueType::Scalar, {1.}}}}}
	);
	selected.Links.push_back({"cache", "cache_array", "pick", "surface_in"});
	selected.Outputs[0] = {"out", "pick", "surface_out"};
	CHECK(Red(Run(selected, Clock(3), &b.Data)) == 10);
}
TEST_CASE(
	"Cache Array range and inactive observations retain exact previous shape",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	d.Nodes[1].Values[0].Data = int64_t{2};
	d.Nodes[1].Values[1].Data = int64_t{4};
	auto a = Run(d, Clock(1));
	CHECK(Slots(a) == std::vector<int>{10, -1, -1});
	d.Nodes[1].Values[1].Data = int64_t{1};
	auto invalid = Run(d, Clock(2), &a.Data);
	CHECK(Slots(invalid) == Slots(a));
	CHECK(Row(invalid.Data).Values.size() == Row(a.Data).Values.size());
	d.Nodes[1].Values[1].Data = int64_t{4};
	d.Nodes[1].Values[2].Data = int64_t{0};
	auto stopped = Run(d, Clock(3), &invalid.Data);
	CHECK(Slots(stopped) == Slots(a));
	d.Nodes[1].Values[2].Data = int64_t{1};
	d.Nodes[1].Values[0].Data = int64_t{1};
	d.Nodes[1].Values[1].Data = int64_t{0};
	auto equal = Run(d, Clock(0), &stopped.Data);
	CHECK(Slots(equal) == Slots(a));
	d.Nodes[1].Values[1].Data = int64_t{6};
	auto paused = Run(d, Clock(4, false), &equal.Data);
	CHECK(Slots(paused) == Slots(a));
	d.Nodes[1].Values[1].Data = int64_t{2};
	auto outside = Run(d, Clock(5), &paused.Data);
	CHECK(Slots(outside) == Slots(a));
}
TEST_CASE(
	"Cache Array Amount-like range changes preserve sparse history and permit beyond-timeline holes",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	auto a = Run(d, Clock(0));
	d.Nodes[1].Values[1].Data = int64_t{2};
	ColourAt(d, 25);
	auto b = Run(d, Clock(1), &a.Data);
	CHECK(Slots(b) == std::vector<int>{10, 25});
	d.Nodes[1].Values[1].Data = int64_t{8};
	ColourAt(d, 35);
	auto c = Run(d, Clock(2), &b.Data);
	CHECK(Slots(c) == std::vector<int>{10, 25, 35, -1, -1, -1, -1, -1});
	d.Nodes[1].Values[2].Data = int64_t{2};
	auto e = Run(d, Clock(3), &c.Data);
	CHECK(Slots(e) == std::vector<int>{10, 35, -1, -1});
	CHECK(Row(e.Data).Values.size() == 6);
}
TEST_CASE(
	"Cache whole surface arrays keep nested ownership instead of processor rows",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	Node row{"row", "value.array", "", {}, {}};
	row.DynamicInputs = {{"leaf", ValueType::Image, std::nullopt}};
	Node tree{"tree", "value.array", "", {}, {}};
	tree.DynamicInputs = {{"row", ValueType::Array, std::nullopt}};
	d.Nodes.insert(d.Nodes.begin() + 1, std::move(row));
	d.Nodes.insert(d.Nodes.begin() + 2, std::move(tree));
	d.Links = {
		{"input", "image", "row", "leaf"},
		{"row", "array", "tree", "row"},
		{"tree", "array", "cache", "surface_in"}
	};
	auto result = Run(d, Clock(0));
	REQUIRE(std::holds_alternative<ImageArray>(result.Output));
	auto &images = std::get<ImageArray>(result.Output);
	REQUIRE(images.Items.size() == 1);
	REQUIRE(std::holds_alternative<std::vector<ImageArrayItem>>(images.Items[0].Data));
	CHECK(images.Images[0].Pixels[0] == 10);
	CHECK(result.Data.Entries.size() == 1);
	const auto frozen = result.Data;
	images.Images[0].Pixels[0] = 99;
	CHECK(result.Data == frozen);
	CHECK(SourceFrameCacheRowType(Row(result.Data)) == "pc.cache");
}
TEST_CASE(
	"Frame-cache owner preserves observed seeks and control revisions but retires removed identities",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	auto plan = Compiled(d);
	CapturedFeedbackHost host;
	Diagnostic e;
	auto q = Clock(2);
	REQUIRE(host.Prepare(d, plan, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(q.DataReplay);
	CHECK(Row(*q.DataReplay).Values.size() == 3);
	const auto record = *q.DataReplay;
	ColourAt(d, 55);
	plan = Compiled(d);
	q = Clock(0);
	REQUIRE(host.Prepare(d, plan, 2, 2, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(Row(*q.DataReplay).Values.size() == 4);
	CHECK(std::get<SurfaceValue>(Row(*q.DataReplay).Values.back().Data).Data.Pixels[0] == 10);
	host.RestartCycle();
	q = Clock(1);
	REQUIRE(host.Prepare(d, plan, 2, 2, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(Row(*q.DataReplay).Values.size() == 5);
	d.Nodes.erase(d.Nodes.begin() + 1);
	d.Links.clear();
	d.Outputs = {{"out", "input", "image"}};
	plan = Compiled(d);
	q = Clock(2);
	REQUIRE(host.Prepare(d, plan, 3, 2, q, e, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(q.DataReplay);
	CHECK(q.DataReplay->Entries.empty());
	CHECK(Row(record).Values.size() == 3);
}
TEST_CASE(
	"Frame-cache host declared played-prefix seek matches independent sequential captures",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	auto p = Compiled(d);
	CapturedFeedbackHost seek, sequence;
	Diagnostic e;
	auto q = Clock(4, true, SourceCacheSampling::NativePlayedPrefix);
	REQUIRE(seek.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	const auto expected = *q.DataReplay;
	for (uint64_t i = 0; i <= 4; ++i) {
		q = Clock(i, true, SourceCacheSampling::NativePlayedPrefix);
		REQUIRE(sequence.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	}
	CHECK(*q.DataReplay == expected);
	seek.RestartCycle();
	q = Clock(4, true, SourceCacheSampling::NativePlayedPrefix);
	REQUIRE(seek.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*q.DataReplay == expected);
	seek.Clear();
	q = Clock(4, true, SourceCacheSampling::NativePlayedPrefix);
	REQUIRE(seek.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*q.DataReplay == expected);
}
TEST_CASE(
	"Same-clock source Playing changes retain actual paused output and never duplicate history",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	Diagnostic e;
	auto q = Clock(0, false);
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(Row(*q.DataReplay).Values.size() == 2);
	q = Clock(0);
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	const auto played = *q.DataReplay;
	CHECK(Row(played).Values.size() == 3);
	q = Clock(0, false);
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(Row(*q.DataReplay).Values.size() == 3);
	CHECK(Last(*q.DataReplay) == Last(played));
	const auto paused = *q.DataReplay;
	q = Clock(0, false);
	REQUIRE(host.Prepare(d, p, 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	CHECK(*q.DataReplay == paused);
}
TEST_CASE(
	"Source cache metadata and missing observations refuse atomically instead of guessing scheduler state",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	const auto initial = Run(d, Clock(0));
	const auto prior = initial.Data;
	for (const auto &metadata : std::vector<std::vector<AuthoredValue>>{
			 {{"cache_group", ArrayValue{ValueType::Text, {std::string("input")}}}},
			 {{"cache", std::string("[]")}}
		 }) {
		d.Nodes[1].SourceProperties = metadata;
		StatefulEvaluationResult r = initial;
		Diagnostic e;
		auto q = Clock(1);
		q.DataReplay = &prior;
		CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e) == Status::UnsupportedExecution);
		SameOutput(r, initial);
		CHECK(r.Data == initial.Data);
		CHECK(r.Simulation == initial.Simulation);
		CHECK(r.Surfaces == initial.Surfaces);
		CHECK(r.Random == initial.Random);
		CHECK(r.Rigid == initial.Rigid);
		CHECK(prior == initial.Data);
		d.Nodes[1].SourceProperties.push_back({"serialize", false});
		CHECK(Red(Run(d, q, &prior)) == 10);
	}
	d.Nodes[1].SourceProperties.clear();
	StatefulEvaluationResult r = initial;
	Diagnostic e;
	EvaluationRequest q;
	q.DataReplay = &prior;
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e) == Status::UnsupportedExecution);
	SameOutput(r, initial);
	CHECK(r.Data == initial.Data);
	CHECK(r.Simulation == initial.Simulation);
	CHECK(r.Surfaces == initial.Surfaces);
	CHECK(r.Random == initial.Random);
	CHECK(r.Rigid == initial.Rigid);
	q = Clock(1);
	q.Subframe = .5;
	q.DataReplay = &prior;
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e) == Status::UnsupportedExecution);
	SameOutput(r, initial);
	CHECK(r.Data == initial.Data);
	CHECK(r.Simulation == initial.Simulation);
	CHECK(r.Surfaces == initial.Surfaces);
	CHECK(r.Random == initial.Random);
	CHECK(r.Rigid == initial.Rigid);
	q = Clock(1);
	q.NegativeFrame = true;
	q.DataReplay = &prior;
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e) == Status::UnsupportedExecution);
	SameOutput(r, initial);
	CHECK(r.Data == initial.Data);
	CHECK(r.Simulation == initial.Simulation);
	CHECK(r.Surfaces == initial.Surfaces);
	CHECK(r.Random == initial.Random);
	CHECK(r.Rigid == initial.Rigid);
}
TEST_CASE(
	"Frame cache admission bounds overlapping generations and sparse output before mutating caller",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	const auto initial = Run(d, Clock(0));
	const auto prior = initial.Data;
	auto q = Clock(1);
	q.DataReplay = &prior;
	Diagnostic e;
	auto r = initial;
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e, 1024) == Status::LimitExceeded);
	SameOutput(r, initial);
	CHECK(r.Data == initial.Data);
	CHECK(r.Simulation == initial.Simulation);
	CHECK(r.Surfaces == initial.Surfaces);
	CHECK(r.Random == initial.Random);
	CHECK(r.Rigid == initial.Rigid);
	CHECK(prior == initial.Data);
	d.Nodes[1].Values[1].Data = int64_t{INT64_MAX};
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, r, e) == Status::LimitExceeded);
	SameOutput(r, initial);
	CHECK(r.Data == initial.Data);
	CHECK(r.Simulation == initial.Simulation);
	CHECK(r.Surfaces == initial.Surfaces);
	CHECK(r.Random == initial.Random);
	CHECK(r.Rigid == initial.Rigid);
	DataReplayState target = prior;
	const auto frozen = target;
	CHECK(
		OverlaySourceFrameCacheRows(d, prior, target, FrameCacheOutputPolicy::RetainedObservation, e, 1) ==
		Status::LimitExceeded
	);
	CHECK(target == frozen);
	d.Nodes[1].Type = "pc.cache";
	d.Nodes[1].Values = {{"animated", false}};
	d.Outputs[0].Port = "cache_surface";
	target = prior;
	const DataReplayState none;
	REQUIRE(
		OverlaySourceFrameCacheRows(d, none, target, FrameCacheOutputPolicy::RetainedObservation, e) ==
		Status::Ok
	);
	CHECK(target.Entries.empty());
}
TEST_CASE(
	"Source frame snapshots preserve finite typed surface formats and captured input ownership",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	d.Nodes[0] = {"input", "image.captured", "", {}, {{"source_id", std::string("pixels")}}};
	for (const auto format :
		 {SurfaceFormat::RGBA8Unorm,
		  SurfaceFormat::RGBA16Float,
		  SurfaceFormat::RGBA32Float,
		  SurfaceFormat::R8Unorm,
		  SurfaceFormat::R16Float,
		  SurfaceFormat::R32Float}) {
		RequestImageSource input;
		input.SourceId = "pixels";
		input.Data.Width = 1;
		input.Data.Height = 2;
		input.Data.Format = format;
		const auto layout = CheckedSurfaceLayout(1, 2, format, Limits::MaximumArrayBytes);
		REQUIRE(layout);
		input.Data.Pixels.resize(layout->Bytes);
		const SurfacePixel pixel{.25, .5, .75, 1};
		REQUIRE(StoreSurfacePixel(input.Data, 0, 0, pixel));
		REQUIRE(StoreSurfacePixel(input.Data, 0, 1, pixel));
		input.Data.Hash = SurfaceHash(input.Data);
		const auto original = input.Data;
		auto q = Clock(0);
		q.ImageSources = std::span<const RequestImageSource>(&input, 1);
		auto first = Run(d, q);
		CHECK(std::get<Image>(first.Output) == original);
		CHECK(std::get<SurfaceValue>(Last(first.Data)).Data == original);
		const auto prior = first.Data;
		input.Data.Pixels[0] ^= 1;
		q = Clock(0);
		q.ImageSources = std::span<const RequestImageSource>(&input, 1);
		auto hit = Run(d, q, &prior);
		CHECK(std::get<Image>(hit.Output) == original);
		CHECK(first.Data == prior);
	}
}
TEST_CASE(
	"Retained frame cache histories overlay pre-observation output without copying a future output",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	auto old = Run(d, Clock(0));
	ColourAt(d, 80);
	auto current = Run(d, Clock(3), &old.Data);
	DataReplayState pre = old.Data;
	Diagnostic e;
	REQUIRE(
		OverlaySourceFrameCacheRows(d, current.Data, pre, FrameCacheOutputPolicy::PreObservation, e) ==
		Status::Ok
	);
	CHECK(std::get<SurfaceValue>(Last(pre)).Data.Pixels[0] == 10);
	CHECK(Row(pre).Values.size() == 4);
	CHECK(std::get<SurfaceValue>(Row(pre).Values.back().Data).Data.Pixels[0] == 80);
	DataReplayState restart;
	REQUIRE(
		OverlaySourceFrameCacheRows(d, current.Data, restart, FrameCacheOutputPolicy::Constructor, e) ==
		Status::Ok
	);
	CHECK(std::get<int64_t>(Last(restart)) == -4);
	CHECK(Row(restart).Values.size() == 4);
}

TEST_CASE(
	"Frame cache retirement precedes a different data node reusing the durable ID",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	CapturedFeedbackHost host;
	Diagnostic e;
	auto q = Clock(1);
	REQUIRE(host.Prepare(d, Compiled(d), 1, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	const auto old = *q.DataReplay;
	d.Nodes[1].Type = "pc.differential";
	d.Nodes[1].Values = {{"value", 8.}};
	d.Links.clear();
	d.Outputs[0].Port = "result";
	q = Clock(2);
	REQUIRE(host.Prepare(d, Compiled(d), 2, 1, q, e, Limits::MaximumEvaluationBytes, "out"));
	const auto *value = host.Value("out");
	REQUIRE(value);
	CHECK(std::get<double>(std::get<EvaluatedValue>(value->Output).Data) == 4.);
	CHECK(SourceFrameCacheRowType(Row(*q.DataReplay)).empty());
	CHECK(Row(old).PreviousFrame == 1.);
}

TEST_CASE(
	"Frame-cache source slot resize drops captures outside a shortened timeline",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	const auto first = Run(d, Clock(5));
	CHECK(Red(first) == 10);
	d.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
	ColourAt(d, 55);
	const auto shortened = Run(d, Clock(5), &first.Data);
	CHECK(Red(shortened) == 55);
	CHECK(Row(shortened.Data).Values.size() == 2);
	CHECK(Row(first.Data).Values.size() == 3);
	CHECK(Red(first) == 10);
}

TEST_CASE(
	"Frame cache overlay admits whole membership work before copying or merging",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene(true);
	const auto first = Run(d, Clock(0));
	for (size_t i = d.Nodes.size(); i < Limits::MaximumNodes; ++i) {
		Node cold = d.Nodes.front();
		cold.Id = "cold-" + std::string(59, 'x') + std::to_string(i);
		d.Nodes.push_back(std::move(cold));
	}
	const auto plan = Compiled(d);
	CHECK(!plan.NodeOrder.empty());
	DataReplayState history;
	for (size_t i = 0; i < 512; ++i) {
		auto entry = Row(first.Data);
		entry.ProcessorRow = i;
		history.Entries.push_back(std::move(entry));
	}
	auto target = history;
	const auto frozen = target;
	Diagnostic e;
	CHECK(
		OverlaySourceFrameCacheRows(d, history, target, FrameCacheOutputPolicy::RetainedObservation, e) ==
		Status::LimitExceeded
	);
	CHECK(e.Message.find("work bounds") != std::string::npos);
	CHECK(target == frozen);
	CHECK(history == frozen);
}

TEST_CASE(
	"Retained Pixel Builder raster owns paused cache observations and immutable prior history",
	"[imagegraph][source_frame_cache][pixel_builder]"
) {
	auto d = Scene();
	const auto prior = Run(d, Clock(0));
	ColourAt(d, 99);
	for (auto &node : d.Nodes)
		node.GroupId = "pb";
	d.Nodes.push_back(
		{"builder",
		 "pc.pixel_builder",
		 "",
		 {},
		 {{"dimension", Vector2{2, 1}}, {"dimension_unit", EnumValue{0}}}}
	);
	d.Nodes.push_back({"draw", "pc.pb_draw_surface", "pb", {}, {{"crop", false}}});
	d.Nodes.push_back({"layer", "pc.pb_output", "pb", {}, {}});
	Group group{"pb", "Builder"};
	group.OwnerNodeId = "builder";
	d.Groups.push_back(std::move(group));
	d.Links.push_back({"cache", "cache_surface", "draw", "surface"});
	d.Links.push_back({"draw", "surface", "layer", "surface"});
	d.Outputs = {{"recipe", "builder", "dynamic_builder"}, {"pixels", "builder", "surface_out"}};
	auto q = Clock(1, false);
	q.DataReplay = &prior.Data;
	StatefulOutputEvaluationResult result;
	Diagnostic e;
	const std::array<std::string, 2> outputs{"recipe", "pixels"};
	const auto status = EvaluateStatefulOutputs(d, Compiled(d), outputs, q, result, e);
	INFO(e.Message);
	REQUIRE(status == Status::Ok);
	const auto recipe =
		std::get<DynamicSurfaceValue>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data);
	const auto pixels = std::get<Image>(result.Outputs[1].Output);
	REQUIRE(recipe.Data);
	REQUIRE(recipe.Data->DataHistory);
	CHECK(*recipe.Data->DataHistory == prior.Data);
	CHECK(recipe.Data->SourceCachePlayback == q.SourceCachePlayback);
	REQUIRE(pixels.Pixels.size() == 8);
	CHECK(pixels.Pixels[0] == 10);
	q.SourceCachePlayback.reset();
	ColourAt(d, 123);
	Image reraster;
	REQUIRE(
		detail::RasterizePixelBuilder(recipe, {2, 1}, reraster, e, Limits::MaximumEvaluationBytes) ==
		Status::Ok
	);
	CHECK(reraster == pixels);
	const auto lastGood = reraster;
	CHECK(detail::RasterizePixelBuilder(recipe, {2, 1}, reraster, e, 1) == Status::LimitExceeded);
	CHECK(reraster == lastGood);
	CHECK(*recipe.Data->DataHistory == prior.Data);
}

TEST_CASE(
	"Source frame storage includes TOTAL_FRAMES but excludes the following frame",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	const auto boundary = Run(d, Clock(6));
	CHECK(Red(boundary) == 10);
	REQUIRE(Row(boundary.Data).Values.size() == 3);
	CHECK(Row(boundary.Data).Values.back().Frame == 8);
	const auto prior = boundary.Data;
	ColourAt(d, 90);
	const auto beyond = Run(d, Clock(7), &prior);
	CHECK(Red(beyond) == 90);
	CHECK(Row(beyond.Data).Values.size() == 3);
	CHECK(Row(beyond.Data).Values.back().Frame == 8);
	CHECK(std::get<SurfaceValue>(Row(beyond.Data).Values.back().Data).Data.Pixels[0] == 10);
	CHECK(Red(Run(d, Clock(6), &beyond.Data)) == 10);
	CHECK(prior == boundary.Data);

	d = Scene(true);
	d.Nodes[1].Values[1].Data = int64_t{8};
	const auto arrayBoundary = Run(d, Clock(6));
	CHECK(Slots(arrayBoundary) == std::vector<int>{-1, -1, -1, -1, -1, -1, 10, -1});
	const auto arrayPrior = arrayBoundary.Data;
	ColourAt(d, 90);
	const auto arrayBeyond = Run(d, Clock(7), &arrayPrior);
	CHECK(Slots(arrayBeyond) == Slots(arrayBoundary));
	CHECK(Row(arrayBeyond.Data).Values.size() == 3);
	CHECK(Row(arrayBeyond.Data).Values.back().Frame == 8);
	CHECK(arrayPrior == arrayBoundary.Data);
}
TEST_CASE(
	"Source extra storage slot still obeys the native tagged history record limit",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	d.Timeline->Frames = 4094;
	d.Timeline->Last = 4093;
	const auto initial = Run(d, Clock(0));
	auto prior = initial.Data;
	auto &row = prior.Entries[0];
	row.Values.resize(2);
	for (uint64_t frame = 0; frame < 4094; ++frame)
		row.Values.push_back({frame + 2, int64_t{-4}});
	REQUIRE(row.Values.size() == 4096);
	const auto frozen = prior;
	Diagnostic admission;
	REQUIRE(ValidateDataReplay(prior, Limits::MaximumEvaluationBytes, admission) == Status::Ok);
	auto q = Clock(4094);
	q.DataReplay = &prior;
	auto result = initial;
	Diagnostic diagnostic;
	CHECK(EvaluateStateful(d, Compiled(d), "out", q, result, diagnostic) == Status::LimitExceeded);
	INFO(diagnostic.Message);
	CHECK(diagnostic.Message == "frame-cache history exceeds native frame count");
	SameOutput(result, initial);
	CHECK(result.Data == initial.Data);
	CHECK(result.Simulation == initial.Simulation);
	CHECK(result.Surfaces == initial.Surfaces);
	CHECK(result.Random == initial.Random);
	CHECK(result.Rigid == initial.Rigid);
	CHECK(prior == frozen);
}
