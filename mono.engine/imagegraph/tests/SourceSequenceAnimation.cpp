#include "NodeExecutors.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_sequence_animation")
using namespace engine::imagegraph;
namespace {
	Document Scene(int64_t mode = 0, double speed = 1, ArrayValue order = {ValueType::Scalar, {}}) {
		Document d;
		d.FormatVersion = 9;
		d.Timeline = TimelineSettings{12, 0, 11, "loop", 24};
		Node frames{"frames", "value.array", "", {}, {}};
		for (size_t i = 0; i < 3; ++i) {
			const auto id = "frame" + std::to_string(i);
			d.Nodes.push_back(
				{id,
				 "image.solid",
				 "",
				 {},
				 {{"width", int64_t{2}},
				  {"height", int64_t{2}},
				  {"colour", Colour{uint8_t(10 * (i + 1)), uint8_t(i), 0, 255}}}}
			);
			frames.DynamicInputs.push_back({id, ValueType::Image, std::nullopt});
			d.Links.push_back({id, "image", "frames", id});
		}
		d.Nodes.push_back(std::move(frames));
		d.Nodes.push_back(
			{"sequence",
			 "pc.sequence_anim",
			 "",
			 {},
			 {{"overflow", EnumValue{mode}}, {"speed", speed}, {"sequence", std::move(order)}}}
		);
		d.Links.push_back({"frames", "array", "sequence", "surface_in"});
		d.Outputs = {{"out", "sequence", "surface_out"}};
		return d;
	}
	Plan CompileNow(const Document &d) {
		Plan p;
		Diagnostic error;
		const auto status = Compile(d, p, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	int Frame(const StatefulEvaluationResult &r) {
		if (const auto *image = std::get_if<Image>(&r.Output)) {
			REQUIRE(image->Width == 2);
			REQUIRE(image->Height == 2);
			REQUIRE(image->Pixels.size() == 16);
			for (size_t i = 0; i < 4; ++i) {
				CHECK(image->Pixels[i * 4] == image->Pixels[0]);
				CHECK(image->Pixels[i * 4 + 3] == 255);
			}
			CHECK(image->Hash == SurfaceHash(*image));
			return image->Pixels[0] / 10 - 1;
		}
		const auto &v = std::get<EvaluatedValue>(r.Output).Data;
		if (const auto *n = std::get_if<int64_t>(&v)) return int(*n);
		return int(std::get<double>(v)) - 100;
	}
	StatefulEvaluationResult Run(const Document &d, const Plan &p, EvaluationRequest request = {}) {
		StatefulEvaluationResult r;
		Diagnostic error;
		const auto status = EvaluateStateful(d, p, "out", request, r, error);
		INFO(error.Message);
		REQUIRE(status == Status::Ok);
		return r;
	}
}
TEST_CASE(
	"Array to Anim source four overflow modes select real owned image frames",
	"[imagegraph][source_sequence_animation]"
) {
	constexpr int oracle[4][9] = {
		{0, 1, 2, 2, 2, 2, 2, 2, 2},
		{0, 1, 2, 0, 1, 2, 0, 1, 2},
		{0, 1, 2, 1, 0, 1, 2, 1, 0},
		{0, 1, 2, -4, -4, -4, -4, -4, -4}
	};
	for (int64_t mode = 0; mode < 4; ++mode) {
		const auto d = Scene(mode);
		const auto p = CompileNow(d);
		for (uint64_t tick = 0; tick < 9; ++tick) {
			EvaluationRequest request;
			request.Tick = tick;
			CHECK(Frame(Run(d, p, request)) == oracle[mode][tick]);
		}
		const auto d2 = Scene(mode, 1, {ValueType::Integer, {int64_t{2}, int64_t{0}, int64_t{1}}});
		Document restored;
		Diagnostic error;
		REQUIRE(Read(Write(d2), restored, error) == Status::Ok);
		const auto p2 = CompileNow(restored);
		const int permuted[3] = {2, 0, 1};
		for (uint64_t tick = 0; tick < 9; ++tick) {
			EvaluationRequest request;
			request.Tick = tick;
			const int selected = oracle[mode][tick];
			CHECK(Frame(Run(restored, p2, request)) == (selected < 0 ? selected : permuted[selected]));
		}
	}
}
TEST_CASE(
	"Array to Anim signed fractional clocks speed and direct seek reset remain deterministic",
	"[imagegraph][source_sequence_animation]"
) {
	const auto d = Scene(1, .5);
	const auto p = CompileNow(d);
	EvaluationRequest request;
	request.Tick = 3;
	request.Subframe = .5;
	CHECK(Frame(Run(d, p, request)) == 1);
	request.NegativeFrame = true;
	CHECK(Frame(Run(d, p, request)) == 1);
	request = {};
	request.Tick = 11;
	CHECK(Frame(Run(d, p, request)) == 2);
	request.Tick = 0;
	CHECK(Frame(Run(d, p, request)) == 0);
	request.Tick = 11;
	CHECK(Frame(Run(d, p, request)) == 2);
	CapturedFeedbackHost owner;
	Diagnostic error;
	for (uint64_t tick : {uint64_t{11}, uint64_t{0}, uint64_t{7}, uint64_t{11}}) {
		EvaluationRequest clock;
		clock.Tick = tick;
		REQUIRE(owner.Prepare(d, p, 1, 1, clock, error, Limits::MaximumEvaluationBytes, "out"));
		REQUIRE(!owner.Active());
		const auto sampled = Run(d, p, clock);
		CHECK(std::get<Image>(sampled.Output).Pixels[0] == uint8_t(10 * (int(tick / 2 % 3) + 1)));
	}
	owner.Clear();
	EvaluationRequest clock;
	clock.Tick = 11;
	REQUIRE(owner.Prepare(d, p, 1, 1, clock, error, Limits::MaximumEvaluationBytes, "out"));
	CHECK(Frame(Run(d, p, clock)) == 2);
	const auto backwards = Scene(3, -1);
	const auto bp = CompileNow(backwards);
	clock = {};
	clock.Tick = 1;
	CHECK(Frame(Run(backwards, bp, clock)) == 2);
}
TEST_CASE(
	"Array to Anim source noone and invalid indices preserve distinct typed sentinels",
	"[imagegraph][source_sequence_animation]"
) {
	const auto d = Scene(1, 1, {ValueType::Scalar, {-4., -1., 99., 1.9, -.5}});
	const auto p = CompileNow(d);
	const int oracle[] = {-4, -100, -100, 1, -100};
	for (uint64_t i = 0; i < 5; ++i) {
		EvaluationRequest clock;
		clock.Tick = i;
		CHECK(Frame(Run(d, p, clock)) == oracle[i]);
	}
	auto empty = Scene();
	empty.Nodes[3] = {
		"frames",
		"pc.array",
		"",
		{},
		{{"type", EnumValue{0}}, {"spread_array", true}},
		{{"empty", ValueType::Array, Value{ArrayValue{ValueType::Any, {}}}}}
	};
	empty.Links.erase(empty.Links.begin(), empty.Links.begin() + 3);
	empty.Outputs.push_back({"frames-array", "frames", "array"});
	const auto emptyPlan = CompileNow(empty);
	EvaluatedValue produced;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValue(empty, emptyPlan, "frames-array", {}, produced, diagnostic) == Status::Ok);
	const auto &array = std::get<ArrayValue>(produced.Data);
	CHECK(array.Elements.empty());
	CHECK(array.Nested.empty());
	CHECK(array.Items.empty());
	CHECK(Frame(Run(empty, emptyPlan)) == -100);
	empty.Nodes.back().Values[0].Data = EnumValue{3};
	CHECK(Frame(Run(empty, CompileNow(empty))) == -4);
}
TEST_CASE(
	"Array to Anim scalar passthrough and failures preserve ownership and last good frame",
	"[imagegraph][source_sequence_animation]"
) {
	auto scalar = Scene(2, std::numeric_limits<double>::max());
	scalar.Links.back() = {"frame1", "image", "sequence", "surface_in"};
	auto p = CompileNow(scalar);
	EvaluationRequest clock;
	clock.Tick = 2;
	auto r = Run(scalar, p, clock);
	CHECK(Frame(r) == 1);
	std::get<Image>(r.Output).Pixels[0] = 99;
	CHECK(Frame(Run(scalar, p, clock)) == 1);
	auto d = Scene();
	p = CompileNow(d);
	clock = {};
	r = Run(d, p, clock);
	const auto before = std::get<Image>(r.Output);
	Diagnostic error;
	CHECK(EvaluateStateful(d, p, "out", clock, r, error, 1) == Status::LimitExceeded);
	CHECK(std::get<Image>(r.Output).Pixels == before.Pixels);
	d.Nodes.back().Values[1].Data = std::numeric_limits<double>::max();
	p = CompileNow(d);
	clock.Tick = 2;
	CHECK(EvaluateStateful(d, p, "out", clock, r, error) == Status::InvalidValue);
	CHECK(std::get<Image>(r.Output).Pixels == before.Pixels);
	auto one = Scene(2);
	one.Nodes[3].DynamicInputs.resize(1);
	one.Links.erase(one.Links.begin() + 1, one.Links.begin() + 3);
	p = CompileNow(one);
	clock = {};
	CHECK(EvaluateStateful(one, p, "out", clock, r, error) == Status::UnsupportedExecution);
	CHECK(std::get<Image>(r.Output).Pixels == before.Pixels);
}

TEST_CASE(
	"Sequence admits complete temporary image trees while preserving unrelated live storage",
	"[imagegraph][source_sequence_animation][evaluation_budget]"
) {
	ImageArray images;
	for (uint8_t red : {uint8_t{20}, uint8_t{40}, uint8_t{60}}) {
		Image image;
		image.Width = image.Height = 64;
		image.Pixels.resize(64 * 64 * 4);
		for (size_t i = 0; i < image.Pixels.size(); i += 4) {
			image.Pixels[i] = red;
			image.Pixels[i + 3] = 255;
		}
		image.Hash = SurfaceHash(image);
		images.Items.push_back({images.Images.size()});
		images.Images.push_back(std::move(image));
	}
	const Node node{"sequence", "pc.sequence_anim", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest clock;
	clock.Tick = 1;
	uint64_t peak = 0;
	for (size_t attempt = 0; attempt < 3; ++attempt) {
		const uint64_t limit = attempt == 0 ? Limits::MaximumEvaluationBytes : attempt == 1 ? peak - 1 : peak;
		detail::EvaluationBudget budget(limit);
		auto retained = budget.Reserve(4096);
		REQUIRE(retained);
		const std::vector<uint8_t> previous(4096, 77);
		{
			detail::NodeContext context(node, *entry, clock, budget);
			context.ByteBudget = limit;
			context.Values = {
				{"sequence", ArrayValue{ValueType::Scalar, {}}}, {"speed", 1.}, {"overflow", EnumValue{1}}
			};
			context.ImageArrays = {{"surface_in", &images}};
			const bool ok = executor(context);
			INFO(context.FailureMessage);
			if (attempt == 1) {
				CHECK_FALSE(ok);
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputImages.empty());
				CHECK(context.OutputValues.empty());
			} else {
				REQUIRE(ok);
				REQUIRE(context.OutputImages.size() == 1);
				CHECK(context.OutputImages.front().second == images.Images[1]);
				if (attempt == 0) peak = budget.Peak();
				CHECK(budget.Peak() == peak);
				CHECK(budget.Peak() > budget.Used());
				context.OutputImages.front().second.Pixels[0] = 99;
				CHECK(images.Images[1].Pixels[0] == 40);
			}
			CHECK(budget.Peak() <= limit);
			CHECK(previous == std::vector<uint8_t>(4096, 77));
		}
		CHECK(budget.Used() == retained->Bytes());
	}
}

TEST_CASE(
	"Sequence preserves selected nested surface rows without flattening",
	"[imagegraph][source_sequence_animation]"
) {
	auto d = Scene(1);
	Node second{"second", "value.array", "", {}, {}};
	second.DynamicInputs = {{"last", ValueType::Image, {}}, {"first", ValueType::Image, {}}};
	Node rows{"rows", "value.array", "", {}, {}};
	rows.DynamicInputs = {{"first", ValueType::Array, {}}, {"second", ValueType::Array, {}}};
	d.Nodes.push_back(std::move(second));
	d.Nodes.push_back(std::move(rows));
	d.Links.back() = {"rows", "array", "sequence", "surface_in"};
	d.Links.push_back({"frame2", "image", "second", "last"});
	d.Links.push_back({"frame0", "image", "second", "first"});
	d.Links.push_back({"frames", "array", "rows", "first"});
	d.Links.push_back({"second", "array", "rows", "second"});
	const auto p = CompileNow(d);
	EvaluationRequest clock;
	clock.Tick = 1;
	auto result = Run(d, p, clock);
	const auto &row = std::get<ImageArray>(result.Output);
	REQUIRE(row.Items.size() == 2);
	REQUIRE(row.Images.size() == 2);
	CHECK(row.Images[std::get<size_t>(row.Items[0].Data)].Pixels[0] == 30);
	CHECK(row.Images[std::get<size_t>(row.Items[1].Data)].Pixels[0] == 10);
	std::get<ImageArray>(result.Output).Images[0].Pixels[0] = 99;
	CHECK(std::get<ImageArray>(Run(d, p, clock).Output).Images[0].Pixels[0] == 30);
	clock.Tick = 0;
	const auto first = Run(d, p, clock);
	const auto &original = std::get<ImageArray>(first.Output);
	REQUIRE(original.Items.size() == 3);
	for (size_t i = 0; i < 3; ++i)
		CHECK(original.Images[std::get<size_t>(original.Items[i].Data)].Pixels[0] == uint8_t(10 * (i + 1)));
}
