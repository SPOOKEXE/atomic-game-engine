#include "NativeFrameCacheReceipt.hpp"
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
	"Group owner dispatch precedes unread member drivers across joined roots",
	"[imagegraph][source_frame_cache][cache_group][timeline]"
) {
	for (const bool array : {false, true})
		for (const bool dynamic : {false, true})
			for (const bool endpoint : {false, true}) {
				auto document = Scene(array);
				const auto initial = Run(document, Clock(0));
				document.Nodes.push_back({"member", "pc.number_simple", "", {}, {{"value", 0.0}}});
				if (dynamic)
					document.Nodes.push_back(
						{"dormant", "pc.equation", "", {}, {{"equation", std::string{"0"}}}}
					);
				document.Nodes[1].SourceProperties = {
					{"cache_group", ArrayValue{ValueType::Text, {std::string{"member"}}}}
				};
				document.Keyframes = {{"member", "value", 0, 1.0, "source", KeyframeEase{}}};
				document.Keyframes.front().SourceDriver =
					KeyframeAudioDriver{"unavailable-member", "rms", 0, 1, 0};
				document.Tracks = {{"member", "value", "hold", -1}};
				document.Outputs.push_back({"member-out", "member", "number"});
				const auto plan = Compiled(document);
				REQUIRE(
					std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 1) <
					std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 2)
				);
				auto request = Clock(1);
				request.DataReplay = &initial.Data;
				request.SourceCacheProject =
					SourceFrameCacheProjectObservation{{1, 0, false}, endpoint ? 1.0 : 5.0, false, false};
				StatefulOutputEvaluationResult result;
				result.Data = initial.Data;
				Diagnostic diagnostic;
				const std::array<std::string, 2> outputs{"out", "member-out"};
				const auto status =
					EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
				INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
				if (endpoint) {
					REQUIRE(status == Status::Ok);
					REQUIRE(result.Outputs.size() == 2);
					CHECK(std::get<double>(std::get<EvaluatedValue>(result.Outputs[1].Output).Data) == 0.0);
					for (const auto &member : result.Data.CacheGroups.Nodes)
						if (member.NodeId == "member") CHECK_FALSE(member.RenderActive);
				} else {
					CHECK(status == Status::InvalidValue);
					CHECK(diagnostic.NodeId == "member");
					CHECK(diagnostic.Port == "value");
					CHECK(result.Data == initial.Data);
					CHECK(result.Outputs.empty());
				}
				EvaluationSnapshot inspected;
				CHECK(
					EvaluateNodeInputs(document, plan, "member", request, inspected, diagnostic) ==
					Status::InvalidValue
				);
				CHECK(diagnostic.NodeId == "member");
			}
}

TEST_CASE(
	"Computed cache controls do not admit members past an earlier unresolved owner",
	"[imagegraph][source_frame_cache][cache_group][timeline][pcx]"
) {
	auto document = Scene();
	const auto initial = Run(document, Clock(0));
	Node animator{"animator", "pc.equation", "", {}, {{"equation", std::string{}}}};
	Node program{"program", "pc.string", "", {}, {{"text", std::string{}}}};
	program.SourceInputExpressions = {{"text", "\"producer.outputs.result\"", true}};
	Node producer{"producer", "pc.equation", "", {}, {{"equation", std::string{"1"}}}};
	producer.SourceInternalName = "producer";
	document.Nodes.push_back(animator);
	document.Nodes.push_back(program);
	document.Nodes.push_back(producer);
	document.Nodes.push_back({"member", "pc.number_simple", "", {}, {{"value", 0.0}}});
	document.Links.push_back({"program", "text", "animator", "equation"});
	document.Links.push_back({"animator", "result", "cache", "animated"});
	document.Nodes[1].SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"member"}}}}
	};
	document.Keyframes = {{"member", "value", 0, 1.0, "source", KeyframeEase{}}};
	document.Keyframes.front().SourceDriver = KeyframeAudioDriver{"unavailable-member", "rms", 0, 1, 0};
	document.Tracks = {{"member", "value", "hold", -1}};
	document.Outputs.push_back({"member-out", "member", "number"});
	const auto plan = Compiled(document);
	REQUIRE(
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 1) <
		std::find(plan.NodeOrder.begin(), plan.NodeOrder.end(), 4)
	);
	auto request = Clock(1);
	request.DataReplay = &initial.Data;
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 1, false, false};
	StatefulOutputEvaluationResult result;
	Diagnostic diagnostic;
	const std::array<std::string, 2> outputs{"out", "member-out"};
	const auto status = EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(status == Status::Ok);
	REQUIRE(result.Outputs.size() == 2);
	CHECK(std::get<double>(std::get<EvaluatedValue>(result.Outputs[1].Output).Data) == 0.0);
}

TEST_CASE(
	"Deferred group dispatch samples inherited values and newly discovered PCX dependencies",
	"[imagegraph][source_frame_cache][cache_group][timeline][pcx]"
) {
	for (const bool computed : {false, true}) {
		auto document = Scene();
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"member"}}}}
		};
		if (computed) {
			document.Nodes.push_back({"member", "pc.equation", "", {}, {{"equation", std::string{}}}});
			Node program{"program", "pc.string", "", {}, {{"text", std::string{}}}};
			program.SourceInputExpressions = {{"text", "\"producer.outputs.result\"", true}};
			document.Nodes.push_back(program);
			Node producer{"producer", "pc.equation", "", {}, {{"equation", std::string{"7"}}}};
			producer.SourceInternalName = "producer";
			document.Nodes.push_back(producer);
			document.Links.push_back({"program", "text", "member", "equation"});
			document.Keyframes = {
				{"producer", "equation", 0, std::string{"7"}, "step"},
				{"producer", "equation", 1, std::string{"11"}, "step"}
			};
			document.Outputs.push_back({"member-out", "member", "result"});
		} else {
			Node member{"member", "pc.number", "", {}, {}};
			member.InstanceBase = "base";
			document.Nodes.push_back(member);
			document.Nodes.push_back({"base", "pc.number", "", {}, {{"value", 0.0}}});
			document.Keyframes = {{"base", "value", 0, 4.0, "linear"}, {"base", "value", 2, 8.0, "linear"}};
			document.Outputs.push_back({"member-out", "member", "number"});
		}
		auto request = Clock(1);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 5, false, false};
		StatefulOutputEvaluationResult result;
		Diagnostic diagnostic;
		const std::array<std::string, 2> outputs{"out", "member-out"};
		const auto status =
			EvaluateStatefulOutputs(document, Compiled(document), outputs, request, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(result.Outputs.size() == 2);
		CHECK(
			std::get<double>(std::get<EvaluatedValue>(result.Outputs[1].Output).Data) ==
			(computed ? 11.0 : 6.0)
		);
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
	CHECK(Red(hit) == 10);
	CHECK(Row(hit.Data).PreviousValue == 1);
	CHECK(Row(hit.Data).Values.size() == 3);
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
	"Frame-cache recovery keeps old slots until a miss executes source storage resize",
	"[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	const auto first = Run(d, Clock(5));
	CHECK(Red(first) == 10);
	d.Timeline = TimelineSettings{3, 0, 2, "loop", 24};
	ColourAt(d, 55);
	const auto shortened = Run(d, Clock(5), &first.Data);
	CHECK(Red(shortened) == 10);
	CHECK(Row(shortened.Data).Values.size() == 3);
	const auto missed = Run(d, Clock(4), &shortened.Data);
	CHECK(Red(missed) == 55);
	CHECK(Row(missed.Data).Values.size() == 2);
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
	DataReplayState loads = prior.Data;
	auto q = Clock(1, false);
	q.DataReplay = &prior.Data;
	bool fromSaved = false;
	SECTION("captured paused history") {}
	SECTION("decoded saved constructor history") {
		fromSaved = true;
		loads.Entries[0].LoadedCacheData = "[owned saved surface receipt]";
		d.Nodes[1].SourceProperties = {{"cache", loads.Entries[0].LoadedCacheData}};
		q.Tick = 0;
		q.DataReplay = nullptr;
		q.SourceFrameCacheLoads = &loads;
	}
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
	if (fromSaved) {
		REQUIRE(recipe.Data->FrameCacheLoads);
		CHECK(*recipe.Data->FrameCacheLoads == loads);
		loads.Entries.clear();
	} else {
		REQUIRE(recipe.Data->DataHistory);
		CHECK(*recipe.Data->DataHistory == prior.Data);
	}
	CHECK(recipe.Data->SourceCachePlayback == q.SourceCachePlayback);
	REQUIRE(pixels.Pixels.size() == 8);
	CHECK(pixels.Pixels[0] == 10);
	q.SourceCachePlayback.reset();
	ColourAt(d, 123);
	Image reraster;
	const auto rasterStatus =
		detail::RasterizePixelBuilder(recipe, {2, 1}, reraster, e, Limits::MaximumEvaluationBytes);
	INFO(e.Message);
	REQUIRE(rasterStatus == Status::Ok);
	CHECK(reraster == pixels);
	const auto lastGood = reraster;
	CHECK(detail::RasterizePixelBuilder(recipe, {2, 1}, reraster, e, 1) == Status::LimitExceeded);
	CHECK(reraster == lastGood);
	if (!fromSaved) CHECK(*recipe.Data->DataHistory == prior.Data);
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

namespace {
	DataReplayEntry CookRow(bool array = false) {
		DataReplayEntry row;
		row.NodeId = "cache";
		row.Initialized = true;
		row.PreviousValue = 1;
		row.LoadedCacheData = "[source-owned saved bytes]";
		Image image{2, 1, {0, 10, 13, 34, 92, 128, 255, 9}};
		image.Hash = SurfaceHash(image);
		row.Values = {
			{0, std::string(array ? "pc.cache_array" : "pc.cache")},
			{1, array ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}}},
			{2, SurfaceValue{image}},
			{4, SurfaceValue{image}}
		};
		return row;
	}
	Node NativeNode(const DataReplayEntry &row) {
		Node node{"cache", std::string(SourceFrameCacheRowType(row)), "", {}, {}};
		ArrayValue chunks;
		Diagnostic diagnostic;
		REQUIRE(EncodeSourceFrameCacheReceipt(row, chunks, diagnostic) == Status::Ok);
		node.SourceProperties = {
			{"cache", row.LoadedCacheData},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), row.LoadedCacheData},
			{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(chunks)}
		};
		return node;
	}
	std::string &PacketText(Node &node) {
		return std::get<std::string>(
			std::get<ArrayValue>(node.SourceProperties.back().Data).Elements.front()
		);
	}
}
TEST_CASE(
	"Native frame cache packets preserve sparse RGBA bytes through authored text",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		const auto row = CookRow(array);
		auto document = Scene(array);
		document.Nodes[1].SourceProperties = NativeNode(row).SourceProperties;
		const auto text = Write(document);
		Diagnostic diagnostic;
		Document roundtrip;
		const auto read = Read(text, roundtrip, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(read == Status::Ok);
		CHECK(roundtrip == document);
		DataReplayEntry decoded;
		uint64_t measured = 0;
		REQUIRE(MeasureSourceFrameCacheReceipt(roundtrip.Nodes[1], measured, diagnostic) == Status::Ok);
		REQUIRE(DecodeSourceFrameCacheReceipt(roundtrip.Nodes[1], decoded, diagnostic) == Status::Ok);
		CHECK(decoded == row);
		CHECK(RetainedDataReplayEntryBytes(decoded) <= measured);
		for (const uint64_t tick : {uint64_t{0}, uint64_t{1}, uint64_t{2}, UINT64_MAX}) {
			detail::NativeFrameCacheReceiptInspection inspection;
			REQUIRE(
				detail::InspectNativeFrameCacheReceipt(roundtrip.Nodes[1], tick, inspection) == Status::Ok
			);
			CHECK(inspection.DecodedBytes == measured);
			CHECK(inspection.HasFrame == (tick == 0 || tick == 2));
		}
		const auto frozen = decoded;
		CHECK(
			DecodeSourceFrameCacheReceipt(roundtrip.Nodes[1], decoded, diagnostic, measured - 1) ==
			Status::LimitExceeded
		);
		CHECK(decoded == frozen);
	}
}
TEST_CASE(
	"Native frame cache encoder agrees with independent little-endian packet bytes",
	"[imagegraph][source_frame_cache]"
) {
	const auto row = CookRow();
	const auto node = NativeNode(row);
	const auto &chunks = std::get<ArrayValue>(node.SourceProperties.back().Data);
	REQUIRE(chunks.Elements.size() == 1);
	const std::vector<uint8_t> expected{1,	 0,	  0,   0,  8,  0,  0,  0,	'p', 'c', '.', 'c', 'a',
										'c', 'h', 'e', 2,  0,  0,  0,  2,	0,	 0,	  0,   1,	2,
										0,	 0,	  0,   1,  0,  0,  0,  0,	10,	 13,  34,  92,	128,
										255, 9,	  4,   0,  0,  0,  1,  2,	0,	 0,	  0,   1,	0,
										0,	 0,	  0,   10, 13, 34, 92, 128, 255, 9};
	const auto &actual = std::get<std::string>(chunks.Elements.front());
	CHECK(std::vector<uint8_t>(actual.begin(), actual.end()) == expected);
}
TEST_CASE(
	"Native cache chunks cross byte boundaries without losing pixel ownership",
	"[imagegraph][source_frame_cache]"
) {
	auto row = CookRow();
	row.Values.resize(2);
	Image image{129, 129, std::vector<uint8_t>(129 * 129 * 4)};
	for (size_t index = 0; index < image.Pixels.size(); ++index)
		image.Pixels[index] = uint8_t(index);
	image.Hash = SurfaceHash(image);
	ArrayValue tree{ValueType::Any, {}};
	tree.Items = {
		{std::vector<SourceArrayItem>{{image}, {ElementValue{int64_t{-4}}}, {std::vector<SourceArrayItem>{}}}}
	};
	row.Values.push_back({2, std::move(tree)});
	row.Values.push_back({5, ArrayValue{ValueType::Any, {}}});
	auto node = NativeNode(row);
	auto &chunks = std::get<ArrayValue>(node.SourceProperties.back().Data);
	REQUIRE(chunks.Elements.size() == 2);
	CHECK(std::get<std::string>(chunks.Elements[0]).size() == 65536);
	DataReplayEntry decoded;
	Diagnostic diagnostic;
	REQUIRE(DecodeSourceFrameCacheReceipt(node, decoded, diagnostic) == Status::Ok);
	CHECK(decoded == row);
	std::get<std::vector<SourceArrayItem>>(std::get<ArrayValue>(decoded.Values[2].Data).Items[0].Data)[0]
		.Data = ElementValue{int64_t{-4}};
	CHECK(decoded != row);
	CHECK(std::get<std::string>(chunks.Elements[0])[0] == 1);
}
TEST_CASE(
	"Native frame cache malformed and stale packets preserve decoded rows", "[imagegraph][source_frame_cache]"
) {
	const auto original = CookRow();
	auto node = NativeNode(original);
	SECTION("wrong version") {
		PacketText(node)[0] = 2;
	}
	SECTION("wrong durable type") {
		PacketText(node)[8] = 'x';
	}
	SECTION("truncated scalar") {
		PacketText(node).resize(22);
	}
	SECTION("truncated pixels") {
		PacketText(node).pop_back();
	}
	SECTION("trailing bytes") {
		PacketText(node).push_back('x');
	}
	SECTION("unknown tag") {
		PacketText(node)[24] = 3;
	}
	SECTION("zero dimensions") {
		PacketText(node)[25] = 0;
	}
	SECTION("oversized dimensions") {
		auto &bytes = PacketText(node);
		bytes.resize(20 + 4 + 9 + 4097 * 4, '\0');
		bytes[16] = 1;
		bytes[25] = 1;
		bytes[26] = 16;
	}
	SECTION("repeated sparse frame") {
		PacketText(node)[41] = 2;
	}
	SECTION("too many frames") {
		PacketText(node)[17] = 32;
	}
	SECTION("stale source text") {
		node.SourceProperties[0].Data = std::string("changed saved text");
	}
	SECTION("duplicate native data") {
		node.SourceProperties.push_back(node.SourceProperties.back());
	}
	SECTION("duplicate source data") {
		node.SourceProperties.push_back(node.SourceProperties.front());
	}
	SECTION("invalid chunk kind") {
		std::get<ArrayValue>(node.SourceProperties.back().Data).Elements[0] = int64_t{1};
	}
	SECTION("excessive nested depth") {
		auto &bytes = PacketText(node);
		bytes.resize(20);
		bytes[16] = 1;
		bytes.append({2, 0, 0, 0});
		for (size_t depth = 0; depth <= Limits::MaximumArrayDepth; ++depth)
			bytes.append({2, 1, 0, 0, 0});
		bytes.push_back(0);
	}
	DataReplayEntry decoded = original;
	Diagnostic diagnostic;
	uint64_t measured = 123;
	CHECK(MeasureSourceFrameCacheReceipt(node, measured, diagnostic) != Status::Ok);
	CHECK(measured == 123);
	detail::NativeFrameCacheReceiptInspection inspection{123, true};
	const auto originalInspection = inspection;
	CHECK(detail::InspectNativeFrameCacheReceipt(node, 0, inspection) != Status::Ok);
	CHECK(inspection == originalInspection);
	CHECK(DecodeSourceFrameCacheReceipt(node, decoded, diagnostic) != Status::Ok);
	CHECK(decoded == original);
}
TEST_CASE(
	"Native cache encoding refuses unusable authored chunks atomically", "[imagegraph][source_frame_cache]"
) {
	auto row = CookRow();
	uint64_t maximumBytes = Limits::MaximumEvaluationBytes;
	bool expectTableRefusal = false;
	SECTION("pixel payload plus table exceeds authored value cap") {
		expectTableRefusal = true;
		row.Values.resize(2);
		Image image{4096, 255, std::vector<uint8_t>(4096 * 255 * 4)};
		row.Values.push_back({2, SurfaceValue{std::move(image)}});
		row.Values.push_back({3, SurfaceValue{Image{4080, 1, std::vector<uint8_t>(4080 * 4)}}});
	}
	SECTION("byte budget") {
		maximumBytes = 1;
	}
	SECTION("bad source identity") {
		row.LoadedCacheData.clear();
	}
	SECTION("wrong frame ordering") {
		row.Values.back().Frame = 2;
	}
	SECTION("unsupported non-surface saved value") {
		row.Values.back().Data = 1.;
	}
	ArrayValue chunks{ValueType::Text, {std::string("previous good packet")}};
	const auto original = chunks;
	Diagnostic diagnostic;
	const auto status = EncodeSourceFrameCacheReceipt(row, chunks, diagnostic, maximumBytes);
	CHECK(status != Status::Ok);
	CHECK(chunks == original);
	if (expectTableRefusal) {
		CHECK(status == Status::LimitExceeded);
		CHECK(diagnostic.Message.find("chunk table") != std::string::npos);
	}
}
TEST_CASE(
	"Cooked native cache initializes owned history and refuses stale reload",
	"[imagegraph][source_frame_cache]"
) {
	auto document = Scene();
	const auto row = CookRow();
	document.Nodes[1].SourceProperties = NativeNode(row).SourceProperties;
	const auto first = Run(document, Clock(0));
	CHECK(std::get<Image>(first.Output).Pixels == std::get<SurfaceValue>(row.Values[2].Data).Data.Pixels);
	CHECK(Row(first.Data).LoadedCacheData == row.LoadedCacheData);
	ColourAt(document, 99);
	const auto sparse = Run(document, Clock(2), &first.Data);
	CHECK(std::get<Image>(sparse.Output).Pixels == std::get<SurfaceValue>(row.Values[3].Data).Data.Pixels);
	const auto hole = Run(document, Clock(1), &sparse.Data);
	CHECK(Red(hole) == 99);
	const auto good = hole;
	document.Nodes[1].SourceProperties[0].Data = std::string("modified cache");
	auto request = Clock(0);
	request.DataReplay = &hole.Data;
	auto result = hole;
	Diagnostic diagnostic;
	CHECK(
		EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic) ==
		Status::InvalidValue
	);
	SameOutput(result, good);
	CHECK(result.Data == good.Data);
}

TEST_CASE(
	"Saved Cache and Cache Array load distinct full-duration slot bounds independently of selected endpoint",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true})
		for (const bool native : {false, true}) {
			INFO("array=" << array << " native=" << native);
			auto document = Scene(array);
			document.Timeline = TimelineSettings{2, 0, 1, "stop", 24};
			document.Timeline->SourceBounds.emplace();
			document.Timeline->SourceBounds->End = {SourceFrameBoundPresence::Explicit, {1, 0, false}};
			auto row = CookRow(array);
			std::get<SurfaceValue>(row.Values[2].Data).Data.Pixels[0] = 12;
			std::get<SurfaceValue>(row.Values[3].Data).Data.Pixels[0] = 90;
			row.Values.push_back({8, row.Values[3].Data});
			DataReplayState loads{{row}};
			const auto frozen = loads;
			document.Nodes[1].SourceProperties =
				native ? NativeNode(row).SourceProperties
					   : std::vector<AuthoredValue>{{"cache", row.LoadedCacheData}};
			auto request = Clock(0, false);
			request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 0, false, false};
			if (!native) request.SourceFrameCacheLoads = &loads;
			const auto restored = Run(document, request);
			CHECK(Row(restored.Data).Values.size() == (array ? 5 : 3));
			CHECK(loads == frozen);
			if (!array) {
				CHECK(Red(restored) == 12);
				request.Tick = 2;
				const auto beyondLoad = Run(document, request);
				CHECK(std::get<int64_t>(std::get<EvaluatedValue>(beyondLoad.Output).Data) == -4);
				CHECK(std::get<int64_t>(Row(beyondLoad.Data).Values.back().Data) == -4);
			} else {
				CHECK(Row(restored.Data).Values.back().Frame == 8);
				request.SourceCachePlayback->Playing = true;
				const auto captured = Run(document, request, &restored.Data);
				CHECK(Slots(captured) == std::vector<int>{10, -1});
				CHECK(Row(captured.Data).Values.size() == 4);
				CHECK(Row(captured.Data).Values.back().Frame == 4);
				CHECK(std::get<SurfaceValue>(Row(captured.Data).Values.back().Data).Data.Pixels[0] == 90);
				CHECK(Row(restored.Data).Values.back().Frame == 8);
			}
		}
}
TEST_CASE(
	"Native saved frame cache mutation sweep preserves ownership on every refusal",
	"[imagegraph][source_frame_cache][fuzz]"
) {
	const auto original = CookRow();
	const auto valid = NativeNode(original);
	uint32_t random = 0x67d10329;
	for (size_t index = 0; index < 1024; ++index) {
		auto node = valid;
		auto &packet = PacketText(node);
		random = random * 1664525u + 1013904223u;
		if (index % 3 == 0)
			packet.resize(random % (packet.size() + 1));
		else if (index % 3 == 1)
			packet[random % packet.size()] = char(random >> 24);
		else
			packet.push_back(char(random >> 24));
		Diagnostic diagnostic;
		uint64_t measured = 0;
		const auto admitted = MeasureSourceFrameCacheReceipt(node, measured, diagnostic);
		auto decoded = original;
		const auto status = DecodeSourceFrameCacheReceipt(node, decoded, diagnostic);
		CAPTURE(index);
		if (admitted == Status::Ok) {
			REQUIRE(status == Status::Ok);
			CHECK(RetainedDataReplayEntryBytes(decoded) <= measured);
			DataReplayState owner{{decoded}};
			CHECK(ValidateDataReplay(owner, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		} else {
			CHECK(status != Status::Ok);
			CHECK(decoded == original);
		}
	}
}

TEST_CASE(
	"Frame-cache Clear keeps last output, clears every row and refuses atomically",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto d = Scene(array);
		const auto played = Run(d, Clock(0));
		DataReplayState source = played.Data;
		auto second = source.Entries.front();
		second.ProcessorRow = 7;
		source.Entries.push_back(second);
		auto unrelated = source.Entries.front();
		unrelated.NodeId = "other";
		source.Entries.push_back(unrelated);
		const auto original = source;
		DataReplayState cleared = source;
		Diagnostic diagnostic;
		CHECK(
			ClearSourceFrameCacheReplay(d.Nodes[1], source, cleared, diagnostic, 1) == Status::LimitExceeded
		);
		CHECK(cleared == original);
		REQUIRE(ClearSourceFrameCacheReplay(d.Nodes[1], source, cleared, diagnostic) == Status::Ok);
		CHECK(source == original);
		REQUIRE(cleared.Entries.size() == 3);
		for (size_t i = 0; i < 2; ++i) {
			CHECK(cleared.Entries[i].Values.size() == 2);
			CHECK(cleared.Entries[i].Values[1] == original.Entries[i].Values[1]);
			CHECK(cleared.Entries[i].FrameCacheConstructorCleared);
			CHECK(cleared.Entries[i].ProcessorRow == original.Entries[i].ProcessorRow);
		}
		CHECK(cleared.Entries[2] == unrelated);
		REQUIRE(ClearSourceFrameCacheReplay(d.Nodes[1], cleared, cleared, diagnostic) == Status::Ok);
		CHECK(cleared.Entries[0].Values.size() == 2);
		auto invalid = source;
		invalid.Entries[0].Values[0].Data = std::string("pc.wrong");
		const auto before = cleared;
		CHECK(ClearSourceFrameCacheReplay(d.Nodes[1], invalid, cleared, diagnostic) == Status::InvalidValue);
		CHECK(cleared == before);
	}
}

TEST_CASE(
	"Cleared constructor cannot reload saved pixels in a new processor row",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto d = Scene(array);
		const auto saved = CookRow(array);
		d.Nodes[1].SourceProperties = NativeNode(saved).SourceProperties;
		ColourAt(d, 99);
		DataReplayState empty, cleared;
		Diagnostic diagnostic;
		REQUIRE(ClearSourceFrameCacheReplay(d.Nodes[1], empty, cleared, diagnostic) == Status::Ok);
		REQUIRE(cleared.Entries.size() == 1);
		CHECK(cleared.Entries[0].LoadedCacheData == saved.LoadedCacheData);
		CHECK(cleared.Entries[0].FrameCacheConstructorCleared);
		cleared.Entries[0].ProcessorRow = 7;
		DataReplayState loads;
		loads.Entries.push_back(saved);
		auto request = Clock(0);
		if (array) {
			d.Nodes[1].SourceProperties = {{"cache", saved.LoadedCacheData}};
			request.SourceFrameCacheLoads = &loads;
		}
		const auto loadOriginal = loads;
		const auto live = Run(d, request, &cleared);
		CHECK(loads == loadOriginal);
		const auto row =
			std::find_if(live.Data.Entries.begin(), live.Data.Entries.end(), [](const auto &entry) {
				return entry.ProcessorRow == 0;
			});
		REQUIRE(row != live.Data.Entries.end());
		CHECK(row->FrameCacheConstructorCleared);
		if (array)
			CHECK(Slots(live) == std::vector<int>{99, -1, -1, -1, -1, -1});
		else
			CHECK(Red(live) == 99);
		const auto original = cleared;
		DataReplayState bad = cleared;
		d.Nodes[1].SourceProperties[0].Data = std::string("changed source");
		CHECK(ClearSourceFrameCacheReplay(d.Nodes[1], cleared, bad, diagnostic) == Status::InvalidValue);
		CHECK(bad == original);
		d.Nodes[1].SourceProperties.resize(Limits::MaximumPropertiesPerNode + 1);
		CHECK(ClearSourceFrameCacheReplay(d.Nodes[1], cleared, bad, diagnostic) == Status::InvalidValue);
		CHECK(bad == original);
	}
}

TEST_CASE(
	"Host Clear suppresses cooked reload on seek and preserves clocks", "[imagegraph][source_frame_cache]"
) {
	auto d = Scene();
	d.Nodes[1].SourceProperties = NativeNode(CookRow()).SourceProperties;
	d.Outputs.push_back({"other", "input", "image"});
	ColourAt(d, 99);
	const auto original = d;
	const auto plan = Compiled(d);
	CapturedFeedbackHost host;
	auto request = Clock(2);
	Diagnostic diagnostic;
	REQUIRE(host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(request.DataReplay);
	REQUIRE(host.Output("out"));
	CHECK(host.Output("out")->Pixels[0] == 0);
	const auto before = *request.DataReplay;
	CHECK_FALSE(host.ClearSourceCache(d, plan, "cache", 1, 1, diagnostic, 1));
	CHECK(*request.DataReplay == before);
	REQUIRE(host.ClearSourceCache(d, plan, "cache", 1, 1, diagnostic));
	CHECK(d == original);
	CHECK(host.PreparedFrame(1, 1) == std::optional<FrameTime>{{2, 0, false}});
	CHECK(host.Output("out") == nullptr);
	REQUIRE(host.CacheInvalidatedOutputs().size() == 1);
	CHECK(host.CacheInvalidatedOutputs()[0] == "out");
	CHECK(Row(*request.DataReplay).Values.size() == 2);
	CHECK_FALSE(host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	request = Clock(0);
	REQUIRE(host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
	REQUIRE(host.Output("out"));
	CHECK(host.Output("out")->Pixels[0] == 99);
	CHECK(Row(*request.DataReplay).FrameCacheConstructorCleared);
}

TEST_CASE(
	"Cache Array Clear follows serialization and project loading gates", "[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		for (int gate = 0; gate < 4; ++gate) {
			auto d = Scene(array);
			if (gate == 1) d.Nodes[1].SourceProperties = {{"serialize", false}};
			const auto plan = Compiled(d);
			CapturedFeedbackHost host;
			auto request = Clock(0);
			request.SourceCacheProject =
				SourceFrameCacheProjectObservation{{0, 0, false}, 5, gate == 2, gate == 3};
			Diagnostic diagnostic;
			REQUIRE(host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
			const auto before = *request.DataReplay;
			REQUIRE(host.ClearSourceCache(d, plan, "cache", 1, 1, diagnostic));
			if (array && gate != 0) {
				CHECK(*request.DataReplay == before);
				CHECK(host.CacheInvalidatedOutputs().empty());
				REQUIRE(
					host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out")
				);
				CHECK(*request.DataReplay == before);
			} else {
				CHECK(Row(*request.DataReplay).Values.size() == 2);
				CHECK(Row(*request.DataReplay).FrameCacheConstructorCleared);
				CHECK(host.CacheInvalidatedOutputs().size() == 1);
			}
		}
	}
}

TEST_CASE(
	"Cache Array Clear still releases selected input captures when frame Clear is gated",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool serialize : {false, true}) {
		auto d = Scene(true);
		d.Nodes[1].SourceProperties = {{"serialize", serialize}};
		const auto plan = Compiled(d);
		CapturedFeedbackHost host;
		auto request = Clock(0);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, true, false};
		Diagnostic diagnostic;
		REQUIRE(host.Prepare(d, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"));
		REQUIRE(host.PrepareNodeInputs(d, plan, 1, 1, "cache", request, diagnostic));
		REQUIRE(host.Snapshot().Images().size() == 1);
		const auto retained = *request.DataReplay;
		REQUIRE(host.ClearSourceCache(d, plan, "cache", 1, 1, diagnostic));
		CHECK(host.Snapshot().Images().empty());
		CHECK(*request.DataReplay == retained);
		CHECK(host.CacheInvalidatedOutputs().empty());
		REQUIRE(host.PrepareNodeInputs(d, plan, 1, 1, "cache", request, diagnostic));
		REQUIRE(host.Snapshot().Images().size() == 1);
	}
}

TEST_CASE(
	"Frame-cache Clear admits group sorting beside both replay copies", "[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		auto state = Run(document, Clock(0)).Data;
		CacheGroupReplayNode producer;
		producer.NodeId = "producer";
		producer.NodeType = "native.outputs";
		producer.RenderActive = false;
		for (size_t index = 0; index < 100; ++index)
			producer.Outputs.push_back({"port" + std::to_string(index), Value{int64_t(index)}, {}, {}});
		state.CacheGroups.Nodes.push_back(std::move(producer));
		const auto original = state;
		const auto cap = RetainedDataReplayBytes(state) +
						 ClearedSourceFrameCacheReplayBytes(document.Nodes[1], state) +
						 DataReplayValidationWorkspaceBytes(state, 1);
		Diagnostic diagnostic;
		CHECK(
			ClearSourceFrameCacheReplay(document.Nodes[1], state, state, diagnostic, cap - 1) ==
			Status::LimitExceeded
		);
		CHECK(state == original);
		REQUIRE(ClearSourceFrameCacheReplay(document.Nodes[1], state, state, diagnostic, cap) == Status::Ok);
		CHECK(state.CacheGroups == original.CacheGroups);
		CHECK(state.Entries.front().Values.size() == 2);
	}
}

TEST_CASE("Frame-cache Clear accepts a full existing processor ledger", "[imagegraph][source_frame_cache]") {
	for (const bool array : {false, true}) {
		const auto document = Scene(array);
		auto state = Run(document, Clock(0)).Data;
		const auto captured = state.Entries.front();
		state.Entries.assign(Limits::MaximumArrayElements, captured);
		for (size_t index = 0; index < state.Entries.size(); ++index)
			state.Entries[index].ProcessorRow = index;
		Diagnostic diagnostic;
		REQUIRE(ClearSourceFrameCacheReplay(document.Nodes[1], state, state, diagnostic) == Status::Ok);
		CHECK(state.Entries.size() == Limits::MaximumArrayElements);
		for (const auto &row : state.Entries) {
			CHECK(row.Values.size() == 2);
			CHECK(row.FrameCacheConstructorCleared);
			CHECK(row.Values[1] == captured.Values[1]);
		}
		const auto original = state;
		auto missing = document.Nodes[1];
		missing.Id = "uncaptured";
		CHECK(ClearSourceFrameCacheReplay(missing, state, state, diagnostic) == Status::InvalidValue);
		CHECK(state == original);
	}
}

namespace {
	DataReplayState FrozenButtonGroup(bool array, bool serialize) {
		auto document = Scene(array);
		document.Nodes[1].SourceProperties = {{"serialize", serialize}};
		auto state = Run(document, Clock(0)).Data;
		state.CacheGroups.Nodes = {
			{"cache", array ? "pc.cache_array" : "pc.cache", "", true, {}},
			{"other-owner", "pc.cache", "", true, {}},
			{"producer",
			 "image.solid",
			 "other-owner",
			 false,
			 {{"image", Value{SurfaceValue{Image{1, 1, {7, 8, 9, 255}}}}, {}, {}},
			  {"missing",
			   {},
			   {},
			   Diagnostic{Status::UnsupportedExecution, "producer", "missing", "saved refusal"}}}},
			{"unrelated", "image.solid", "", false, {{"image", Value{int64_t{-4}}, {}, {}}}}
		};
		state.CacheGroups.Owners = {
			{"cache", serialize, {"producer", "producer"}}, {"other-owner", true, {"producer"}}
		};
		return state;
	}
}
TEST_CASE(
	"Cache buttons wake overlapping members only under source enable gates",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true})
		for (int gate = 0; gate < 4; ++gate) {
			const bool serialize = gate != 1;
			auto document = Scene(array);
			document.Nodes[1].SourceProperties = {{"serialize", serialize}};
			auto source = FrozenButtonGroup(array, serialize);
			const auto original = source;
			std::optional<SourceFrameCacheProjectObservation> project{
				SourceFrameCacheProjectObservation{{0, 0, false}, 5, gate == 2, gate == 3}
			};
			DataReplayState cleared = source;
			Diagnostic diagnostic;
			CHECK(
				ClearSourceFrameCacheButtonReplay(
					document.Nodes[1], source, cleared, project, diagnostic, 1
				) == Status::LimitExceeded
			);
			CHECK(cleared == original);
			REQUIRE(
				ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, project, diagnostic) ==
				Status::Ok
			);
			CHECK(source == original);
			auto expectedGroups = original.CacheGroups;
			if (gate == 0) expectedGroups.Nodes[2].RenderActive = true;
			CHECK(cleared.CacheGroups == expectedGroups);
			CHECK(cleared.CacheGroups.Nodes[2].OwnerId == "other-owner");
			CHECK_FALSE(cleared.CacheGroups.Nodes[3].RenderActive);
			CHECK(cleared.Entries.front().Values[1] == source.Entries.front().Values[1]);
			if (!array || gate == 0) {
				CHECK(cleared.Entries.front().Values.size() == 2);
				CHECK(cleared.Entries.front().FrameCacheConstructorCleared);
			} else
				CHECK(cleared.Entries == source.Entries);
			REQUIRE(
				ClearSourceFrameCacheButtonReplay(document.Nodes[1], cleared, cleared, project, diagnostic) ==
				Status::Ok
			);
			CHECK(cleared.CacheGroups == expectedGroups);
		}
}
TEST_CASE(
	"Cache button rejects unknown group gates and partial initialization atomically",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		auto source = FrozenButtonGroup(array, true);
		DataReplayState cleared = source;
		const auto original = cleared;
		Diagnostic diagnostic;
		CHECK(
			ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, {}, diagnostic) ==
			Status::UnsupportedExecution
		);
		CHECK(cleared == original);
		ArrayValue group{ValueType::Text, {ElementValue{std::string{"producer"}}}};
		document.Nodes[1].SourceProperties = {{"cache_group", group}};
		source.CacheGroups.Nodes[2].OwnerId.clear();
		source.CacheGroups.Owners.clear();
		std::optional<SourceFrameCacheProjectObservation> project{SourceFrameCacheProjectObservation{}};
		CHECK(
			ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, project, diagnostic) ==
			Status::UnsupportedExecution
		);
		CHECK(cleared == original);
		document.Nodes[1].SourceProperties.push_back({"cache_group", group});
		CHECK(
			ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, project, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(cleared == original);
		document.Nodes[1].SourceProperties = {{"serialize", std::string{"wrong"}}};
		CHECK(
			ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, project, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(cleared == original);
	}
}
TEST_CASE(
	"Cache button charges group validation beside source and destination", "[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		auto source = FrozenButtonGroup(array, true);
		DataReplayState cleared = source;
		const auto original = cleared;
		const auto cap = RetainedDataReplayBytes(source) + RetainedDataReplayBytes(cleared) -
						 sizeof(cleared) + ClearedSourceFrameCacheReplayBytes(document.Nodes[1], source) +
						 DataReplayValidationWorkspaceBytes(source);
		std::optional<SourceFrameCacheProjectObservation> project{SourceFrameCacheProjectObservation{}};
		Diagnostic diagnostic;
		CHECK(
			ClearSourceFrameCacheButtonReplay(
				document.Nodes[1], source, cleared, project, diagnostic, cap - 1
			) == Status::LimitExceeded
		);
		CHECK(cleared == original);
		REQUIRE(
			ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, cleared, project, diagnostic, cap) ==
			Status::Ok
		);
		CHECK(cleared.CacheGroups.Nodes[2].RenderActive);
		CHECK_FALSE(source.CacheGroups.Nodes[2].RenderActive);
	}
}
TEST_CASE(
	"Gated Cache Array button needs no constructor row in a full unrelated ledger",
	"[imagegraph][source_frame_cache]"
) {
	auto document = Scene(true);
	document.Nodes[1].SourceProperties = {{"serialize", false}};
	auto source = Run(document, Clock(0)).Data;
	const auto row = source.Entries.front();
	source.Entries.assign(Limits::MaximumArrayElements, row);
	for (size_t index = 0; index < source.Entries.size(); ++index) {
		source.Entries[index].NodeId = "unrelated";
		source.Entries[index].ProcessorRow = index;
	}
	const auto original = source;
	Diagnostic diagnostic;
	REQUIRE(
		ClearSourceFrameCacheButtonReplay(document.Nodes[1], source, source, {}, diagnostic) == Status::Ok
	);
	CHECK(source == original);
}
TEST_CASE(
	"Host initializes groups for an ordinary selected member and invalidates awakened previews",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		ArrayValue group{ValueType::Text, {ElementValue{std::string{"input"}}}};
		document.Nodes[1].SourceProperties = {{"cache_group", group}};
		document.Nodes.push_back({"consumer", "image.passthrough", "", {}, {}});
		document.Nodes.push_back(
			{"other",
			 "image.solid",
			 "",
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{99, 0, 0, 255}}}}
		);
		document.Links.push_back({"input", "image", "consumer", "image"});
		document.Outputs.push_back({"member", "input", "image"});
		document.Outputs.push_back({"downstream", "consumer", "image"});
		document.Outputs.push_back({"other", "other", "image"});
		Diagnostic diagnostic;
		auto plan = Compiled(document);
		CapturedFeedbackHost simple;
		auto request = Clock(0);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
		REQUIRE(simple.Prepare(
			document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "member"
		));
		REQUIRE(request.DataReplay);
		REQUIRE(request.DataReplay->CacheGroups.Owners.size() == 1);
		REQUIRE(simple.Output("member"));
		CHECK_FALSE(simple.ClearSourceCache(document, plan, "cache", 1, 1, diagnostic, 1));
		REQUIRE(simple.Output("member"));
		REQUIRE(simple.ClearSourceCache(document, plan, "cache", 1, 1, diagnostic));
		CHECK(simple.Output("member") == nullptr);
		CHECK(
			std::find(
				simple.CacheInvalidatedOutputs().begin(), simple.CacheInvalidatedOutputs().end(), "downstream"
			) != simple.CacheInvalidatedOutputs().end()
		);
		for (const std::string id : {"member", "downstream", "other"})
			document.Nodes.push_back(
				{"binding-" + id, "image.captured", "", {}, {{"source_id", std::string{"feedback:"} + id}}}
			);
		plan = Compiled(document);
		CapturedFeedbackHost host;
		request = Clock(0);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
		REQUIRE(host.Prepare(
			document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "downstream"
		));
		REQUIRE(host.Output("member"));
		REQUIRE(host.Output("downstream"));
		REQUIRE(host.Output("other"));
		REQUIRE(host.PrepareNodeInputs(document, plan, 1, 1, "consumer", request, diagnostic));
		REQUIRE(host.Snapshot().Images().size() == 1);
		const auto getters = request.DataReplay->CacheGroups;
		REQUIRE(host.ClearSourceCache(document, plan, "cache", 1, 1, diagnostic));
		CHECK(host.Output("member") == nullptr);
		CHECK(host.Output("downstream") == nullptr);
		REQUIRE(host.Output("other"));
		CHECK(host.Output("other")->Pixels[0] == 99);
		CHECK(host.Snapshot().Images().empty());
		CHECK(request.DataReplay->CacheGroups == getters);
		CHECK_FALSE(
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "member")
		);
		request.Tick = 1;
		REQUIRE(
			host.Prepare(document, plan, 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "member")
		);
		REQUIRE(host.Output("member"));
		CHECK(host.Output("member")->Pixels[0] == 10);
	}
}

namespace {
	void AddUnreadableRangeGetter(Document &document) {
		document.Nodes.push_back(
			{"unreadable-range",
			 "pc.lua_compute",
			 "",
			 {},
			 {{"function_name", std::string{"range_trap"}}, {"return_type", EnumValue{0}}}}
		);
		document.Links.push_back({"unreadable-range", "return_value", "cache", "step"});
	}
	void FreezeInputForAnotherOwner(Document &document, DataReplayState &state) {
		document.Nodes.push_back({"input-owner", "pc.cache", "", {}, {}});
		state.CacheGroups.Nodes = {
			{"input-owner", "pc.cache", "", true, {}},
			{"input",
			 "image.solid",
			 "input-owner",
			 false,
			 {{"image",
			   {},
			   {},
			   Diagnostic{Status::UnsupportedExecution, "input", "image", "unreadable frozen getter"}}}}
		};
		state.CacheGroups.Owners = {{"input-owner", true, {"input"}}};
	}
}
TEST_CASE(
	"Cache Array early returns do not read range or surface getters", "[imagegraph][source_frame_cache]"
) {
	for (int branch = 0; branch < 3; ++branch) {
		auto document = Scene(true);
		const auto earlier = Run(document, Clock(0));
		auto state = earlier.Data;
		if (branch == 1) document.Links.clear();
		if (branch == 2) {
			document.Nodes[1].SourceProperties = {{"serialize", false}};
			FreezeInputForAnotherOwner(document, state);
		}
		AddUnreadableRangeGetter(document);
		ColourAt(document, 99);
		const auto original = state;
		auto request = Clock(1, branch != 0);
		const auto result = Run(document, request, &state);
		CHECK(Slots(result) == Slots(earlier));
		CHECK(Row(result.Data).Values == Row(earlier.Data).Values);
		CHECK(result.Data.CacheGroups == state.CacheGroups);
		CHECK(state == original);
		StatefulEvaluationResult refused = result;
		Diagnostic diagnostic;
		request.DataReplay = &state;
		if (branch == 0) request.SourceCachePlayback->Playing = true;
		if (branch == 1) document.Links.push_back({"input", "image", "cache", "surface_in"});
		if (branch == 2) state.CacheGroups.Nodes[1].RenderActive = true;
		CHECK(
			EvaluateStateful(document, Compiled(document), "out", request, refused, diagnostic) ==
			Status::UnsupportedExecution
		);
		SameOutput(refused, result);
		CHECK(refused.Data == result.Data);
	}
}
TEST_CASE(
	"Unlinked and disabled Cache inputs leave latest output for outer auto capture",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool disabled : {false, true}) {
		auto document = Scene();
		const auto earlier = Run(document, Clock(0));
		auto state = earlier.Data;
		if (disabled) {
			document.Nodes[1].SourceProperties = {{"serialize", false}};
			FreezeInputForAnotherOwner(document, state);
		} else
			document.Links.clear();
		const auto original = state;
		const auto result = Run(document, Clock(1), &state);
		CHECK(Red(result) == 10);
		REQUIRE(Row(result.Data).Values.size() == 4);
		CHECK(Row(result.Data).Values.back().Frame == 3);
		CHECK(Row(result.Data).Values.back().Data == Row(earlier.Data).Values[1].Data);
		CHECK(result.Data.CacheGroups == state.CacheGroups);
		CHECK(state == original);
	}
}
TEST_CASE(
	"Cache Array early-return pruning preserves independent roots and input inspection",
	"[imagegraph][source_frame_cache]"
) {
	auto document = Scene(true);
	AddUnreadableRangeGetter(document);
	document.Outputs.push_back({"range", "unreadable-range", "return_value"});
	const auto plan = Compiled(document);
	EvaluationRequest request = Clock(0, false);
	StatefulEvaluationResult cold;
	Diagnostic diagnostic;
	REQUIRE(EvaluateStateful(document, plan, "out", request, cold, diagnostic) == Status::Ok);
	CHECK(Slots(cold).empty());
	StatefulOutputEvaluationResult batch;
	const std::vector<std::string> selected{"out", "range"};
	CHECK(
		EvaluateStatefulOutputs(document, plan, selected, request, batch, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(batch.Outputs.empty());
	EvaluationSnapshot snapshot;
	CHECK(
		EvaluateNodeInputs(document, plan, "cache", request, snapshot, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(snapshot.Images().empty());
}

TEST_CASE("Cache still reads Animated before its missing-input return", "[imagegraph][source_frame_cache]") {
	auto document = Scene();
	const auto earlier = Run(document, Clock(0));
	document.Links.clear();
	document.Nodes.push_back(
		{"unreadable-animated",
		 "pc.lua_compute",
		 "",
		 {},
		 {{"function_name", std::string{"animated_trap"}}, {"return_type", EnumValue{0}}}}
	);
	document.Links.push_back({"unreadable-animated", "return_value", "cache", "animated"});
	auto request = Clock(1);
	request.DataReplay = &earlier.Data;
	StatefulEvaluationResult result = earlier;
	Diagnostic diagnostic;
	CHECK(
		EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	SameOutput(result, earlier);
	CHECK(result.Data == earlier.Data);
}

TEST_CASE(
	"Disabled cache input getters are not restored under the current request dimension cap",
	"[imagegraph][source_frame_cache]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		const auto earlier = Run(document, Clock(0));
		auto state = earlier.Data;
		document.Nodes[1].SourceProperties = {{"serialize", false}};
		FreezeInputForAnotherOwner(document, state);
		auto &getter = state.CacheGroups.Nodes[1].Outputs[0];
		getter.Data = SurfaceValue{Image{3, 1, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255}}};
		getter.Refusal.reset();
		const auto original = state;
		auto request = Clock(1);
		request.MaximumImageDimension = 2;
		const auto result = Run(document, request, &state);
		if (array)
			CHECK(Slots(result) == Slots(earlier));
		else
			CHECK(Red(result) == 10);
		CHECK(result.Data.CacheGroups == original.CacheGroups);
		CHECK(state == original);
	}
}

TEST_CASE(
	"Cache Array early returns do not sample authored animated controls",
	"[imagegraph][source_frame_cache][timeline]"
) {
	for (int branch = 0; branch < 3; ++branch) {
		auto document = Scene(true);
		const auto earlier = Run(document, Clock(0));
		auto state = earlier.Data;
		if (branch == 1) document.Links.clear();
		if (branch == 2) {
			document.Nodes[1].SourceProperties = {{"serialize", false}};
			FreezeInputForAnotherOwner(document, state);
		}
		document.Keyframes = {
			{"cache", "step", 0, int64_t{1}, "source", KeyframeEase{}},
			{"cache", "step", 5, int64_t{2}, "source", KeyframeEase{}}
		};
		for (auto &key : document.Keyframes)
			key.SourceDriver = KeyframeAudioDriver{"unavailable-capture", "rms", 0, 1, 0};
		document.Tracks = {{"cache", "step", "hold", -1}};
		const auto original = state;
		auto request = Clock(1, branch != 0);
		const auto result = Run(document, request, &state);
		CHECK(Slots(result) == Slots(earlier));
		CHECK(state == original);
		CHECK(result.Data.CacheGroups == original.CacheGroups);
		request.DataReplay = &state;
		EvaluationSnapshot snapshot;
		Diagnostic inspectionDiagnostic;
		CHECK(
			EvaluateNodeInputs(
				document, Compiled(document), "cache", request, snapshot, inspectionDiagnostic
			) == Status::InvalidValue
		);
		CHECK(inspectionDiagnostic.NodeId == "cache");
		CHECK(inspectionDiagnostic.Port == "step");
		if (branch == 0) request.SourceCachePlayback->Playing = true;
		if (branch == 1) document.Links.push_back({"input", "image", "cache", "surface_in"});
		if (branch == 2) state.CacheGroups.Nodes[1].RenderActive = true;
		request.DataReplay = &state;
		StatefulEvaluationResult refused = result;
		Diagnostic diagnostic;
		CHECK(
			EvaluateStateful(document, Compiled(document), "out", request, refused, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(diagnostic.NodeId == "cache");
		CHECK(diagnostic.Port == "step");
		SameOutput(refused, result);
		CHECK(refused.Data == result.Data);
	}
}

TEST_CASE(
	"Group Enable clears every cache row only after its source gates admit the action",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true})
		for (int gate = 0; gate < 4; ++gate)
			for (const bool inPlace : {false, true}) {
				const bool serialize = gate != 1;
				auto document = Scene(array);
				document.Nodes[1].SourceProperties = {{"serialize", serialize}};
				auto source = FrozenButtonGroup(array, serialize);
				source.Entries.push_back(source.Entries.front());
				source.Entries.back().ProcessorRow = 7;
				const auto original = source;
				DataReplayState distinct = source;
				auto &output = inPlace ? source : distinct;
				const SourceFrameCacheProjectObservation project{{1, 0, false}, 5, gate == 2, gate == 3};
				Diagnostic diagnostic;
				REQUIRE(
					ApplySourceFrameCacheGroupReplay(
						document.Nodes[1],
						CacheGroupReplayAction::Enable,
						source,
						output,
						false,
						project,
						diagnostic
					) == Status::Ok
				);
				if (!inPlace) CHECK(source == original);
				auto expectedGroups = original.CacheGroups;
				if (gate == 0) expectedGroups.Nodes[2].RenderActive = true;
				CHECK(output.CacheGroups == expectedGroups);
				REQUIRE(output.Entries.size() == original.Entries.size());
				for (size_t index = 0; index < output.Entries.size(); ++index) {
					const auto &row = output.Entries[index];
					CHECK(row.Values[1] == original.Entries[index].Values[1]);
					CHECK(row.ProcessorRow == original.Entries[index].ProcessorRow);
					if (gate == 0) {
						CHECK(row.Values.size() == 2);
						CHECK(row.FrameCacheConstructorCleared);
					} else
						CHECK(row == original.Entries[index]);
				}
			}
}

TEST_CASE(
	"Group Disable uses authoritative rounded project endpoints and retains frames and getters",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true})
		for (int branch = 0; branch < 8; ++branch) {
			const bool serialize = branch != 1;
			auto document = Scene(array);
			document.Nodes[1].SourceProperties = {{"serialize", serialize}};
			auto source = FrozenButtonGroup(array, serialize);
			source.CacheGroups.Nodes[2].RenderActive = true;
			const auto original = source;
			SourceFrameCacheProjectObservation project{{5, 0, false}, 5, branch == 2, branch == 3};
			if (branch == 5) project.ProjectFrame = {4, .5, false};
			if (branch == 6) {
				project.ProjectFrame = {5, .5, false};
				project.ProjectLastFrame = 6;
			}
			if (branch == 7) project.ProjectFrame = {5, .5, false};
			const bool admitted = branch == 0 || branch == 6;
			DataReplayState output = source;
			Diagnostic diagnostic;
			REQUIRE(
				ApplySourceFrameCacheGroupReplay(
					document.Nodes[1],
					CacheGroupReplayAction::Disable,
					source,
					output,
					branch != 4,
					project,
					diagnostic
				) == Status::Ok
			);
			CHECK(source == original);
			CHECK(output.Entries == original.Entries);
			auto expected = original.CacheGroups;
			if (admitted) expected.Nodes[2].RenderActive = false;
			CHECK(output.CacheGroups == expected);
			REQUIRE(
				ApplySourceFrameCacheGroupReplay(
					document.Nodes[1],
					CacheGroupReplayAction::Disable,
					output,
					output,
					branch != 4,
					project,
					diagnostic
				) == Status::Ok
			);
			CHECK(output.CacheGroups == expected);
			CHECK(output.Entries == original.Entries);
		}
}

TEST_CASE(
	"Group actions reject unknown observations malformed clocks and stale source types atomically",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const auto action : {CacheGroupReplayAction::Enable, CacheGroupReplayAction::Disable}) {
		auto document = Scene();
		const auto source = FrozenButtonGroup(false, true);
		DataReplayState output = source;
		const auto original = output;
		Diagnostic diagnostic;
		SourceFrameCacheProjectObservation project{{5, 0, false}, 5, false, false};
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, output, true, {}, diagnostic
			) == Status::UnsupportedExecution
		);
		CHECK(output == original);
		document.Nodes[1].Type = "pc.cache_array";
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, output, true, project, diagnostic
			) == Status::InvalidValue
		);
		CHECK(output == original);
		document.Nodes[1].Type = "pc.cache";
		document.Nodes[1].SourceProperties = {{"serialize", true}, {"serialize", true}};
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, output, true, project, diagnostic
			) == Status::InvalidValue
		);
		CHECK(output == original);
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		DataReplayState cold;
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, cold, output, true, project, diagnostic
			) == Status::UnsupportedExecution
		);
		CHECK(output == original);
		if (action == CacheGroupReplayAction::Disable) {
			project.ProjectFrame.Subframe = 1;
			CHECK(
				ApplySourceFrameCacheGroupReplay(
					document.Nodes[1], action, source, output, true, project, diagnostic
				) == Status::InvalidValue
			);
			CHECK(output == original);
		}
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1],
				CacheGroupReplayAction::TransferMember,
				source,
				output,
				true,
				project,
				diagnostic
			) == Status::InvalidValue
		);
		CHECK(output == original);
	}
}

TEST_CASE(
	"Group actions admit validation beside both owned replay copies",
	"[imagegraph][source_frame_cache][cache_group][evaluation_budget]"
) {
	for (const auto action : {CacheGroupReplayAction::Enable, CacheGroupReplayAction::Disable}) {
		auto document = Scene();
		auto source = FrozenButtonGroup(false, true);
		DataReplayState output = source;
		const auto original = output;
		const uint64_t replacement = action == CacheGroupReplayAction::Enable
										 ? ClearedSourceFrameCacheReplayBytes(document.Nodes[1], source)
										 : RetainedDataReplayBytes(source);
		const uint64_t cap = RetainedDataReplayBytes(source) + RetainedDataReplayBytes(output) -
							 sizeof(output) + replacement + DataReplayValidationWorkspaceBytes(source);
		const SourceFrameCacheProjectObservation project{{5, 0, false}, 5, false, false};
		Diagnostic diagnostic;
		CHECK(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, output, true, project, diagnostic, cap - 1
			) == Status::LimitExceeded
		);
		CHECK(output == original);
		REQUIRE(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, output, true, project, diagnostic, cap
			) == Status::Ok
		);
	}
	for (const auto action : {CacheGroupReplayAction::Enable, CacheGroupReplayAction::Disable}) {
		auto document = Scene();
		document.Nodes[1].SourceProperties = {{"serialize", false}};
		auto source = Run(document, Clock(0)).Data;
		const auto row = source.Entries.front();
		source.Entries.assign(Limits::MaximumArrayElements, row);
		for (size_t index = 0; index < source.Entries.size(); ++index) {
			source.Entries[index].NodeId = "unrelated";
			source.Entries[index].ProcessorRow = index;
		}
		const auto original = source;
		Diagnostic diagnostic;
		REQUIRE(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], action, source, source, false, {}, diagnostic
			) == Status::Ok
		);
		CHECK(source == original);
	}
}

TEST_CASE(
	"Group Enable clears history before a later paused Cache outer capture",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		const auto initial = Run(document, Clock(0));
		auto source = initial.Data;
		source.CacheGroups.Nodes = {
			{"cache", document.Nodes[1].Type, "", true, {}},
			{"input",
			 "image.solid",
			 "cache",
			 false,
			 {{"image", Value{SurfaceValue{std::get<Image>(Run(Scene(), Clock(0)).Output)}}, {}, {}}}}
		};
		source.CacheGroups.Owners = {{"cache", true, {"input"}}};
		const auto original = source;
		DataReplayState enabled;
		Diagnostic diagnostic;
		const SourceFrameCacheProjectObservation project{{1, 0, false}, 5, false, false};
		REQUIRE(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1], CacheGroupReplayAction::Enable, source, enabled, false, project, diagnostic
			) == Status::Ok
		);
		CHECK(source == original);
		REQUIRE(enabled.Entries.front().Values.size() == 2);
		const auto observed = Run(document, Clock(1, false), &enabled);
		if (array) {
			CHECK(Slots(observed) == Slots(initial));
			CHECK(observed.Data.Entries.front().Values.size() == 2);
		} else {
			CHECK(Red(observed) == Red(initial));
			REQUIRE(observed.Data.Entries.front().Values.size() == 3);
			CHECK(observed.Data.Entries.front().Values.back().Frame == 3);
			CHECK(
				observed.Data.Entries.front().Values.back().Data == original.Entries.front().Values[1].Data
			);
		}
		CHECK(enabled.Entries.front().Values.size() == 2);
	}
}

namespace {
	void AddUnreadableCacheGetters(Document &document) {
		document.Nodes[0].Type = "pc.lua_compute";
		document.Nodes[0].Values = {
			{"function_name", std::string{"surface_trap"}}, {"return_type", EnumValue{0}}
		};
		document.Links[0].FromPort = "return_value";
		document.Nodes.push_back(
			{"animated-trap",
			 "pc.lua_compute",
			 "",
			 {},
			 {{"function_name", std::string{"animated_trap"}}, {"return_type", EnumValue{0}}}}
		);
		document.Links.push_back({"animated-trap", "return_value", "cache", "animated"});
	}
}
TEST_CASE("Cache hits skip connected getters and preserve Animated", "[imagegraph][source_frame_cache]") {
	for (const bool grouped : {false, true})
		for (const bool playing : {false, true}) {
			for (const int constructor : {0, 1, 2}) {
				const bool saved = constructor != 0;
				auto document = Scene();
				auto prior = Run(document, Clock(0));
				prior.Data.Entries[0].PreviousValue = 1;
				DataReplayState loads = prior.Data;
				if (saved) {
					loads.Entries[0].LoadedCacheData = "[owned exact receipt]";
					document.Nodes[1].SourceProperties =
						constructor == 2
							? NativeNode(loads.Entries[0]).SourceProperties
							: std::vector<AuthoredValue>{{"cache", loads.Entries[0].LoadedCacheData}};
				}
				AddUnreadableCacheGetters(document);
				if (grouped)
					document.Nodes[1].SourceProperties.push_back(
						{"cache_group",
						 ArrayValue{ValueType::Text, {std::string{"input"}, std::string{"animated-trap"}}}}
					);
				const auto original = prior.Data;
				const auto originalLoads = loads;
				auto request = Clock(0, playing);
				request.DataReplay = saved ? nullptr : &prior.Data;
				if (constructor == 1) request.SourceFrameCacheLoads = &loads;
				StatefulEvaluationResult result;
				Diagnostic diagnostic;
				const auto code =
					EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
				INFO(diagnostic.Message);
				REQUIRE(code == Status::Ok);
				CHECK(Red(result) == 10);
				CHECK(Row(result.Data).PreviousValue == 1);
				CHECK(Row(result.Data).Values == Row(saved ? loads : prior.Data).Values);
				CHECK(prior.Data == original);
				CHECK(loads == originalLoads);
				request.Tick = 1;
				StatefulEvaluationResult refused = result;
				CHECK(
					EvaluateStateful(document, Compiled(document), "out", request, refused, diagnostic) ==
					Status::UnsupportedExecution
				);
				SameOutput(refused, result);
				CHECK(refused.Data == result.Data);
			}
		}
}
TEST_CASE(
	"Grouped paused caches retain outputs without project gates or unread producers",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		const auto initial = Run(document, Clock(0));
		AddUnreadableCacheGetters(document);
		document.Nodes.resize(2);
		document.Links.resize(1);
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		const auto observed = Run(document, Clock(1, false), &initial.Data);
		SameOutput(observed, initial);
		for (const auto &node : observed.Data.CacheGroups.Nodes)
			CHECK(node.RenderActive);
		if (array)
			CHECK(Row(observed.Data).Values == Row(initial.Data).Values);
		else {
			REQUIRE(Row(observed.Data).Values.size() == 4);
			CHECK(Row(observed.Data).Values.back().Frame == 3);
			CHECK(Row(observed.Data).Values.back().Data == Row(initial.Data).Values[1].Data);
		}
	}
}
TEST_CASE(
	"Grouped cache misses freeze members only on an admitted endpoint action",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true})
		for (const bool authored : {false, true})
			for (int gate = 0; gate < 5; ++gate) {
				auto document = Scene(array);
				document.Nodes[1].SourceProperties = {
					{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
				};
				auto first = Clock(0);
				first.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
				const auto initial = Run(document, first);
				if (!authored) document.Nodes[1].SourceProperties.clear();
				auto request = Clock(1);
				request.DataReplay = &initial.Data;
				if (gate != 0)
					request.SourceCacheProject = SourceFrameCacheProjectObservation{
						{5, 0, false}, gate == 1 ? 6.0 : 5.0, gate == 3, gate == 4
					};
				StatefulEvaluationResult result = initial;
				Diagnostic diagnostic;
				const auto status =
					EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
				INFO(diagnostic.Message);
				if (gate == 0) {
					CHECK(status == Status::UnsupportedExecution);
					CHECK(diagnostic.NodeId == "cache");
					CHECK(diagnostic.Port == "cache_group");
					SameOutput(result, initial);
					CHECK(result.Data == initial.Data);
				} else {
					REQUIRE(status == Status::Ok);
					if (array)
						CHECK(Slots(result) == std::vector<int>{10, 10, -1, -1, -1, -1});
					else
						CHECK(Red(result) == 10);
					for (const auto &node : result.Data.CacheGroups.Nodes)
						CHECK(node.RenderActive == (gate != 2 || node.NodeId != "input"));
				}
			}
}
TEST_CASE(
	"Grouped Cache Array selects its endpoint action after range validation and before clock inclusion",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (int range = 0; range < 4; ++range) {
		auto document = Scene(true);
		const auto initial = Run(document, Clock(0));
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		document.Nodes[1].Values = {
			{"start_frame", int64_t{4}},
			{"stop_frame",
			 int64_t{
				 range == 0	  ? 2
				 : range == 3 ? 3
							  : 5
			 }},
			{"step", int64_t{range == 1 ? 0 : 1}}
		};
		auto request = Clock(1);
		request.DataReplay = &initial.Data;
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{5, 0, false}, 5, false, false};
		StatefulEvaluationResult result = initial;
		Diagnostic diagnostic;
		const auto status =
			EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
		if (range < 2) {
			REQUIRE(status == Status::Ok);
			SameOutput(result, initial);
			CHECK(Row(result.Data).Values == Row(initial.Data).Values);
		} else {
			REQUIRE(status == Status::Ok);
			SameOutput(result, initial);
			CHECK(Row(result.Data).Values == Row(initial.Data).Values);
			for (const auto &member : result.Data.CacheGroups.Nodes)
				if (member.NodeId == "input") CHECK_FALSE(member.RenderActive);
		}
	}
}
TEST_CASE(
	"Frozen cache producers wake without reading getters only when source gates allow it",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true})
		for (int gate = 0; gate < 5; ++gate) {
			auto document = Scene(array);
			document.Nodes[1].SourceProperties = {
				{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
			};
			auto first = Clock(0);
			first.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
			const auto initial = Run(document, first);
			DataReplayState frozen;
			Diagnostic diagnostic;
			REQUIRE(
				ApplySourceFrameCacheGroupReplay(
					document.Nodes[1],
					CacheGroupReplayAction::Disable,
					initial.Data,
					frozen,
					true,
					SourceFrameCacheProjectObservation{{5, 0, false}, 5, false, false},
					diagnostic
				) == Status::Ok
			);
			const auto original = frozen;
			if (gate == 4) document.Nodes[1].SourceProperties.push_back({"serialize", false});
			document.Keyframes = {{"input", "width", 0, int64_t{2}, "source", KeyframeEase{}}};
			document.Keyframes[0].SourceDriver = KeyframeAudioDriver{"unavailable-capture", "rms", 0, 1, 0};
			document.Tracks = {{"input", "width", "hold", -1}};
			auto request = Clock(1);
			request.DataReplay = &frozen;
			if (gate != 0)
				request.SourceCacheProject =
					SourceFrameCacheProjectObservation{{1, 0, false}, 5, gate == 2, gate == 3};
			StatefulEvaluationResult result = initial;
			const auto status =
				EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic);
			INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
			if (gate == 0) {
				CHECK(status == Status::UnsupportedExecution);
				CHECK(diagnostic.NodeId == "cache");
				CHECK(diagnostic.Port == "cache_group");
				CHECK(result.Data == initial.Data);
			} else {
				REQUIRE(status == Status::Ok);
				SameOutput(result, initial);
				const auto input = std::find_if(
					result.Data.CacheGroups.Nodes.begin(),
					result.Data.CacheGroups.Nodes.end(),
					[](const auto &node) { return node.NodeId == "input"; }
				);
				REQUIRE(input != result.Data.CacheGroups.Nodes.end());
				CHECK(input->RenderActive == (gate == 1));
				if (gate == 1) {
					CHECK(Row(result.Data).FrameCacheConstructorCleared);
					CHECK(Row(result.Data).Values.size() == (array ? 2 : 3));
					if (!array) CHECK(Row(result.Data).Values.back().Frame == 3);
				}
			}
			CHECK(frozen == original);
		}
}
TEST_CASE(
	"Cache hits skip producer driver sampling but retain input inspection",
	"[imagegraph][source_frame_cache][timeline]"
) {
	auto document = Scene();
	const auto prior = Run(document, Clock(0));
	document.Keyframes = {
		{"input", "width", 0, int64_t{2}, "source", KeyframeEase{}},
		{"input", "width", 5, int64_t{1}, "source", KeyframeEase{}}
	};
	for (auto &key : document.Keyframes)
		key.SourceDriver = KeyframeAudioDriver{"unavailable-capture", "rms", 0, 1, 0};
	document.Tracks = {{"input", "width", "hold", -1}};
	auto request = Clock(0);
	request.DataReplay = &prior.Data;
	const auto result = Run(document, request, &prior.Data);
	CHECK(Red(result) == 10);
	EvaluationSnapshot snapshot;
	Diagnostic diagnostic;
	CHECK(
		EvaluateNodeInputs(document, Compiled(document), "cache", request, snapshot, diagnostic) ==
		Status::InvalidValue
	);
	CHECK(diagnostic.NodeId == "input");
	CHECK(diagnostic.Port == "width");
}
TEST_CASE("Cache hit pruning preserves independent selected roots", "[imagegraph][source_frame_cache]") {
	auto document = Scene();
	const auto prior = Run(document, Clock(0));
	AddUnreadableCacheGetters(document);
	document.Outputs.push_back({"independent", "animated-trap", "return_value"});
	auto request = Clock(0);
	request.DataReplay = &prior.Data;
	const auto single = Run(document, request, &prior.Data);
	CHECK(Red(single) == 10);
	StatefulOutputEvaluationResult result;
	Diagnostic diagnostic;
	const std::array<std::string, 2> outputs{"out", "independent"};
	CHECK(
		EvaluateStatefulOutputs(document, Compiled(document), outputs, request, result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result.Outputs.empty());
	CHECK(prior.Data.Entries.size() == 1);
}

TEST_CASE("Cold native Cache distinguishes array hits and noone misses", "[imagegraph][source_frame_cache]") {
	for (const bool arrayHit : {false, true}) {
		auto row = CookRow();
		row.Values[2].Data = arrayHit ? Value{ArrayValue{ValueType::Any, {}}} : Value{int64_t{-4}};
		auto document = Scene();
		document.Nodes[1].SourceProperties = NativeNode(row).SourceProperties;
		AddUnreadableCacheGetters(document);
		const auto original = document;
		detail::NativeFrameCacheReceiptInspection inspection;
		REQUIRE(detail::InspectNativeFrameCacheReceipt(document.Nodes[1], 0, inspection) == Status::Ok);
		CHECK(inspection.HasFrame == arrayHit);
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		const auto code = EvaluateStateful(document, Compiled(document), "out", Clock(0), result, diagnostic);
		INFO(diagnostic.Message);
		if (arrayHit) {
			REQUIRE(code == Status::Ok);
			CHECK(Slots(result).empty());
			CHECK(std::holds_alternative<ArrayValue>(Row(result.Data).Values[1].Data));
		} else {
			CHECK(code == Status::UnsupportedExecution);
			CHECK(result.Data.Entries.empty());
		}
		CHECK(document == original);
	}
}
TEST_CASE(
	"Cold native Cache admission preserves receipt and clear precedence", "[imagegraph][source_frame_cache]"
) {
	for (const int precedence : {0, 1, 2, 3}) {
		auto row = CookRow();
		auto document = Scene();
		document.Nodes[1].SourceProperties = NativeNode(row).SourceProperties;
		AddUnreadableCacheGetters(document);
		DataReplayState current, loads;
		if (precedence == 0) {
			row.Values.resize(2);
			current.Entries.push_back(row);
		} else if (precedence == 1) {
			row.Values.resize(2);
			loads.Entries.push_back(row);
		} else if (precedence == 2) {
			row.ProcessorRow = 1;
			row.FrameCacheConstructorCleared = true;
			row.Values.resize(2);
			current.Entries.push_back(row);
		} else {
			row.LoadedCacheData = "[different exact source receipt]";
			loads.Entries.push_back(row);
		}
		const auto originalCurrent = current, originalLoads = loads;
		auto request = Clock(0);
		request.DataReplay = &current;
		request.SourceFrameCacheLoads = &loads;
		StatefulEvaluationResult result;
		Diagnostic diagnostic;
		CHECK(
			EvaluateStateful(document, Compiled(document), "out", request, result, diagnostic) ==
			Status::UnsupportedExecution
		);
		CHECK(result.Data.Entries.empty());
		CHECK(current == originalCurrent);
		CHECK(loads == originalLoads);
	}
}
TEST_CASE("Cold native Cache admission keeps loaded duration bounds", "[imagegraph][source_frame_cache]") {
	const auto row = CookRow();
	auto document = Scene();
	document.Timeline = TimelineSettings{2, 0, 1, "loop", 24};
	document.Nodes[1].SourceProperties = NativeNode(row).SourceProperties;
	AddUnreadableCacheGetters(document);
	StatefulEvaluationResult result;
	Diagnostic diagnostic;
	CHECK(
		EvaluateStateful(document, Compiled(document), "out", Clock(2), result, diagnostic) ==
		Status::UnsupportedExecution
	);
	CHECK(result.Data.Entries.empty());
	const auto loaded = Run(Scene(), Clock(2));
	auto retained = loaded.Data;
	retained.Entries[0].LoadedCacheData = row.LoadedCacheData;
	const auto hit = Run(document, Clock(2), &retained);
	CHECK(Red(hit) == 10);
}

TEST_CASE(
	"Automatic group Enable clears all owner rows and preserves completed frozen getters",
	"[imagegraph][source_frame_cache][cache_group][timeline]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		auto first = Clock(0);
		first.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
		const auto initial = Run(document, first);
		DataReplayState frozen;
		Diagnostic diagnostic;
		REQUIRE(
			ApplySourceFrameCacheGroupReplay(
				document.Nodes[1],
				CacheGroupReplayAction::Disable,
				initial.Data,
				frozen,
				true,
				SourceFrameCacheProjectObservation{{5, 0, false}, 5, false, false},
				diagnostic
			) == Status::Ok
		);
		auto otherRow = Row(frozen);
		otherRow.ProcessorRow = 9;
		frozen.Entries.push_back(std::move(otherRow));
		const auto original = frozen;
		ColourAt(document, 70);
		document.Keyframes = {{"input", "width", 0, int64_t{2}, "source", KeyframeEase{}}};
		document.Keyframes.front().SourceDriver = KeyframeAudioDriver{"unavailable-input", "rms", 0, 1, 0};
		document.Tracks = {{"input", "width", "hold", -1}};
		document.Outputs.push_back({"input-out", "input", "image"});
		auto request = Clock(1);
		request.DataReplay = &frozen;
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 5, false, false};
		const std::array<std::string, 2> outputs{"out", "input-out"};
		StatefulOutputEvaluationResult result;
		const auto status =
			EvaluateStatefulOutputs(document, Compiled(document), outputs, request, result, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(result.Outputs.size() == 2);
		CHECK(std::get<Image>(result.Outputs[1].Output).Pixels[0] == 10);
		REQUIRE(result.Data.Entries.size() == 2);
		for (const auto &row : result.Data.Entries) {
			CHECK(row.FrameCacheConstructorCleared);
			CHECK(row.Values.size() == (array || row.ProcessorRow == 9 ? 2 : 3));
			if (!array && row.ProcessorRow == 0) CHECK(row.Values.back().Frame == 3);
		}
		for (const auto &member : result.Data.CacheGroups.Nodes)
			CHECK(member.RenderActive);
		CHECK(frozen == original);
		request.Tick = 2;
		request.DataReplay = &result.Data;
		request.SourceCacheProject->ProjectFrame = {2, 0, false};
		auto refused = result;
		CHECK(
			EvaluateStatefulOutputs(document, Compiled(document), outputs, request, refused, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(diagnostic.NodeId == "input");
		CHECK(refused.Data == result.Data);
		document.Keyframes.clear();
		document.Tracks.clear();
		StatefulOutputEvaluationResult live;
		REQUIRE(
			EvaluateStatefulOutputs(document, Compiled(document), outputs, request, live, diagnostic) ==
			Status::Ok
		);
		CHECK(std::get<Image>(live.Outputs[1].Output).Pixels[0] == 70);
		CHECK(frozen == original);
	}
}

TEST_CASE(
	"Automatic Disable captures the current owner output when its own group freezes it",
	"[imagegraph][source_frame_cache][cache_group][timeline]"
) {
	auto document = Scene();
	document.Nodes[1].SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"cache"}, std::string{"input"}}}}
	};
	auto request = Clock(0);
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 0, false, false};
	const auto result = Run(document, request);
	CHECK(Red(result) == 10);
	for (const auto &member : result.Data.CacheGroups.Nodes)
		CHECK_FALSE(member.RenderActive);
	document.Keyframes = {{"input", "width", 0, int64_t{2}, "source", KeyframeEase{}}};
	document.Keyframes.front().SourceDriver = KeyframeAudioDriver{"unavailable-owner", "rms", 0, 1, 0};
	document.Tracks = {{"input", "width", "hold", -1}};
	ColourAt(document, 70);
	const auto frozen = Run(document, Clock(1), &result.Data);
	CHECK(Red(frozen) == 10);
	CHECK(frozen.Data == result.Data);
}

TEST_CASE(
	"Read-only Cache outputs use private group journals without changing supplied history",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		document.Timeline = TimelineSettings{1, 0, 0, "loop", 24};
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		auto request = Clock(0);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 0, false, false};
		const auto captured = Run(document, request);
		const auto plan = Compiled(document);
		Diagnostic diagnostic;
		const auto read = [&] {
			if (array) {
				ImageArray result;
				REQUIRE(EvaluateArray(document, plan, "out", request, result, diagnostic) == Status::Ok);
				const auto &expected = std::get<ImageArray>(captured.Output);
				CHECK(result.Items == expected.Items);
				REQUIRE(result.Images.size() == expected.Images.size());
				CHECK(result.Images[0].Pixels == expected.Images[0].Pixels);
			} else {
				Image result;
				REQUIRE(Evaluate(document, plan, "out", request, result, diagnostic) == Status::Ok);
				CHECK(result.Pixels == std::get<Image>(captured.Output).Pixels);
			}
		};
		read();
		auto prior = captured.Data;
		auto extra = Row(prior);
		extra.ProcessorRow = 9;
		prior.Entries.push_back(std::move(extra));
		const auto original = prior;
		request.Tick = 1;
		request.DataReplay = &prior;
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 5, false, false};
		document.Keyframes = {{"input", "width", 0, int64_t{2}, "source", KeyframeEase{}}};
		document.Keyframes.front().SourceDriver =
			KeyframeAudioDriver{"unavailable-private-input", "rms", 0, 1, 0};
		document.Tracks = {{"input", "width", "hold", -1}};
		const auto frozenPlan = Compiled(document);
		if (array) {
			ImageArray result;
			REQUIRE(EvaluateArray(document, frozenPlan, "out", request, result, diagnostic) == Status::Ok);
			CHECK(result.Images[0].Pixels[0] == 10);
		} else {
			Image result;
			REQUIRE(Evaluate(document, frozenPlan, "out", request, result, diagnostic) == Status::Ok);
			CHECK(result.Pixels[0] == 10);
			const auto before = result;
			CHECK(
				Evaluate(document, frozenPlan, "out", request, result, diagnostic, 1) == Status::LimitExceeded
			);
			CHECK(result.Pixels == before.Pixels);
		}
		CHECK(prior == original);
		request.SourceCacheProject.reset();
		if (array) {
			ImageArray result;
			CHECK(
				EvaluateArray(document, frozenPlan, "out", request, result, diagnostic) ==
				Status::UnsupportedExecution
			);
		} else {
			Image result;
			CHECK(
				Evaluate(document, frozenPlan, "out", request, result, diagnostic) ==
				Status::UnsupportedExecution
			);
		}
		CHECK(diagnostic.NodeId == "cache");
		CHECK(diagnostic.Port == "cache_group");
		CHECK(prior == original);
	}
}

TEST_CASE(
	"Read-only endpoint groups freeze downstream PCX getters before their dispatch",
	"[imagegraph][source_frame_cache][cache_group][pcx]"
) {
	auto document = Scene(true);
	document.Timeline = TimelineSettings{1, 0, 0, "loop", 24};
	document.Nodes.push_back({"late", "pc.array_length", "", {}, {}});
	document.Nodes.back().SourceInputExpressions = {{"array", "draw(value,0,0)", true}};
	document.Links.push_back({"cache", "cache_array", "late", "array"});
	document.Outputs.push_back({"late-out", "late", "size"});
	document.Nodes[1].SourceProperties = {
		{"cache_group", ArrayValue{ValueType::Text, {std::string{"late"}}}}
	};
	auto first = Clock(0);
	first.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
	const auto prior = Run(document, first);
	const auto original = prior.Data;
	auto request = Clock(1);
	request.DataReplay = &prior.Data;
	request.SourceCacheProject = SourceFrameCacheProjectObservation{{1, 0, false}, 1, false, false};
	EvaluatedValue value;
	Diagnostic diagnostic;
	const auto plan = Compiled(document);
	const auto status = EvaluateValue(document, plan, "late-out", request, value, diagnostic);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port << " " << diagnostic.Message);
	REQUIRE(status == Status::Ok);
	CHECK(std::get<int64_t>(value.Data) == 0);
	CHECK(prior.Data == original);
	request.SourceCacheProject->ProjectLastFrame = 5;
	CHECK(
		EvaluateValue(document, plan, "late-out", request, value, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(prior.Data == original);
}

TEST_CASE(
	"Source input edits follow current owner pointers and enclosing groups atomically",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	auto document = Scene();
	document.Nodes.push_back({"other", "pc.cache_array", "", {}, {}});
	document.Nodes.push_back({"outer", "pc.cache", "", {}, {}});
	document.Nodes.push_back({"wrapper", "pc.number_simple", "", {}, {}});
	document.Nodes.push_back({"spare", "pc.number_simple", "", {}, {}});
	document.Nodes[0].GroupId = "inside";
	document.Groups = {{"inside", "inside", "parent", {}, 1, {}, 0, 0, "wrapper"}, {"parent", "parent"}};
	const auto captured = Run(Scene(), Clock(0));
	DataReplayState source = captured.Data;
	for (const auto id : {"other", "outer"}) {
		auto row = id == std::string_view{"other"} ? Run(Scene(true), Clock(0)).Data.Entries.front()
												   : source.Entries.front();
		row.NodeId = id;
		source.Entries.push_back(std::move(row));
	}
	source.CacheGroups.Nodes = {
		{"cache", "pc.cache", "", true, {}},
		{"other", "pc.cache_array", "", true, {}},
		{"outer", "pc.cache", "", true, {}},
		{"input", "image.solid", "other", false, {{"image", SurfaceValue{std::get<Image>(captured.Output)}}}},
		{"wrapper", "pc.number_simple", "outer", false, {}},
		{"spare", "pc.number_simple", "cache", false, {}}
	};
	source.CacheGroups.Owners = {
		{"cache", true, {"input", "spare"}}, {"other", true, {"input"}}, {"outer", true, {"wrapper", "input"}}
	};
	const auto original = source;
	Diagnostic diagnostic;
	REQUIRE(ValidateDataReplay(source, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	const std::array<std::string_view, 2> edits{"input", "input"};
	DataReplayState updated;
	REQUIRE(
		EnableSourceFrameCacheEditedGroups(
			document,
			edits,
			source,
			updated,
			SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false},
			diagnostic
		) == Status::Ok
	);
	CHECK(source == original);
	CHECK(updated.Entries[0] == original.Entries[0]);
	for (size_t i = 1; i < updated.Entries.size(); ++i) {
		CHECK(updated.Entries[i].FrameCacheConstructorCleared);
		CHECK(updated.Entries[i].Values.size() == 2);
		CHECK(updated.Entries[i].Values[1] == original.Entries[i].Values[1]);
	}
	CHECK(updated.CacheGroups.Nodes[3].RenderActive);
	CHECK(updated.CacheGroups.Nodes[4].RenderActive);
	CHECK_FALSE(updated.CacheGroups.Nodes[5].RenderActive);
	CHECK(updated.CacheGroups.Nodes[3].Outputs == original.CacheGroups.Nodes[3].Outputs);
	for (const auto cap : {uint64_t{1}, RetainedDataReplayBytes(source)}) {
		auto refused = original;
		CHECK(
			EnableSourceFrameCacheEditedGroups(
				document,
				edits,
				source,
				refused,
				SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false},
				diagnostic,
				cap
			) == Status::LimitExceeded
		);
		CHECK(refused == original);
	}
	for (const bool full : {false, true})
		for (const int gate : {0, 1, 2}) {
			auto gatedSource = source;
			if (full) {
				gatedSource.Entries.clear();
				for (size_t i = 0; i < Limits::MaximumArrayElements; ++i) {
					auto row = source.Entries.front();
					row.ProcessorRow = i;
					gatedSource.Entries.push_back(std::move(row));
				}
			}
			auto gatedDocument = document;
			if (gate == 0) {
				gatedDocument.Nodes[2].SourceProperties.push_back({"serialize", false});
				gatedDocument.Nodes[3].SourceProperties.push_back({"serialize", false});
			}
			REQUIRE(
				EnableSourceFrameCacheEditedGroups(
					gatedDocument,
					edits,
					gatedSource,
					updated,
					SourceFrameCacheProjectObservation{{0, 0, false}, 5, gate == 1, gate == 2},
					diagnostic
				) == Status::Ok
			);
			CHECK(updated.Entries == gatedSource.Entries);
			CHECK(updated.CacheGroups.Nodes == gatedSource.CacheGroups.Nodes);
		}
	auto cyclic = document;
	cyclic.Groups[1].ParentId = "inside";
	updated = original;
	CHECK(
		EnableSourceFrameCacheEditedGroups(
			cyclic,
			edits,
			source,
			updated,
			SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false},
			diagnostic
		) == Status::InvalidValue
	);
	CHECK(updated == original);
}

TEST_CASE(
	"Host source input edit wakes frozen members at the same clock and preserves clear on replay",
	"[imagegraph][source_frame_cache][cache_group]"
) {
	for (const bool array : {false, true}) {
		auto document = Scene(array);
		document.Nodes[1].SourceProperties = {
			{"cache_group", ArrayValue{ValueType::Text, {std::string{"input"}}}}
		};
		document.Outputs.push_back({"input-out", "input", "image"});
		CapturedFeedbackHost host;
		Diagnostic diagnostic;
		auto request = Clock(0);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 0, false, false};
		REQUIRE(host.Prepare(
			document, Compiled(document), 1, 1, request, diagnostic, Limits::MaximumEvaluationBytes, "out"
		));
		REQUIRE(request.DataReplay);
		const auto original = *request.DataReplay;
		for (const auto &member : original.CacheGroups.Nodes)
			if (member.NodeId == "input") CHECK_FALSE(member.RenderActive);
		ColourAt(document, 70);
		const std::array<std::string_view, 1> edits{"input"};
		CHECK_FALSE(host.NotifySourceInputEdits(document, edits, diagnostic, host.RetainedBytes()));
		REQUIRE(host.PreparedData(1, 1));
		CHECK(*host.PreparedData(1, 1) == original);
		REQUIRE(host.NotifySourceInputEdits(document, edits, diagnostic));
		const auto edited = *host.PreparedData(1, 1);
		for (const auto &member : edited.CacheGroups.Nodes)
			if (member.NodeId == "input") CHECK(member.RenderActive);
		if (!edited.Entries.empty()) {
			CHECK(edited.Entries.front().FrameCacheConstructorCleared);
			CHECK(edited.Entries.front().Values.size() == 2);
		}
		request = Clock(0, false);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
		REQUIRE(host.Prepare(
			document,
			Compiled(document),
			2,
			1,
			request,
			diagnostic,
			Limits::MaximumEvaluationBytes,
			"input-out"
		));
		const auto *output = host.Output("input-out");
		REQUIRE(output);
		CHECK(output->Pixels[0] == 70);
		request = Clock(0, false);
		request.SourceCacheProject = SourceFrameCacheProjectObservation{{0, 0, false}, 5, false, false};
		REQUIRE(host.Prepare(
			document,
			Compiled(document),
			2,
			1,
			request,
			diagnostic,
			Limits::MaximumEvaluationBytes,
			"input-out"
		));
		output = host.Output("input-out");
		REQUIRE(output);
		CHECK(output->Pixels[0] == 70);
	}
}
