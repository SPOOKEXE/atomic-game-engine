#include "nodes/Path.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_path_bake")
using namespace engine::imagegraph;
namespace {
	Path2D BakeLine(bool loop = false) {
		Path2D path;
		path.Loop = loop;
		path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{10, 0, 0, 0, 0, 0}, 0}};
		path.Weights = {{0, 7}, {1, 9}};
		return path;
	}
	struct BakeGraph {
		Document Doc;
		Plan Compiled;
		Diagnostic Failure;
		BakeGraph(Path2D path, int64_t mode = 0, bool spread = true) {
			Doc.FormatVersion = 9;
			Doc.Nodes = {
				{"bake",
				 "pc.path_bake",
				 "",
				 {},
				 {{"path", std::move(path)},
				  {"sample_type", EnumValue{mode}},
				  {"segment_length", 4.},
				  {"output_amount", int64_t{2}},
				  {"spread_single_path", spread}}}
			};
			Doc.Outputs = {{"segments", "bake", "segments"}, {"path", "bake", "path"}};
			REQUIRE(Compile(Doc, Compiled, Failure) == Status::Ok);
		}
		EvaluatedValue Run(std::string_view output) {
			EvaluatedValue result;
			const auto status = EvaluateValue(Doc, Compiled, std::string(output), {}, result, Failure);
			INFO(Failure.Message);
			REQUIRE(status == Status::Ok);
			return result;
		}
	};
	std::vector<Vector3> Samples(const ArrayValue &array, bool spread = true, size_t line = 0) {
		const auto &items =
			spread ? array.Items : std::get<std::vector<SourceArrayItem>>(array.Items[line].Data);
		std::vector<Vector3> result;
		for (const auto &item : items) {
			const auto &coordinates = std::get<std::vector<SourceArrayItem>>(item.Data);
			REQUIRE(coordinates.size() == 3);
			result.push_back(
				{std::get<double>(std::get<ElementValue>(coordinates[0].Data)),
				 std::get<double>(std::get<ElementValue>(coordinates[1].Data)),
				 std::get<double>(std::get<ElementValue>(coordinates[2].Data))}
			);
		}
		return result;
	}
}
TEST_CASE(
	"Bake Path samples Length and Amount with source progress and open endpoint clamp",
	"[imagegraph][path_bake]"
) {
	BakeGraph length(BakeLine());
	const auto segments = length.Run("segments");
	CHECK(
		Samples(std::get<ArrayValue>(segments.Data)) ==
		std::vector<Vector3>{{0, 0, 0}, {4, 0, .4}, {8, 0, .8}}
	);
	REQUIRE(segments.Domain);
	CHECK(segments.Domain->Kind == SourceSocketKind::Float);
	BakeGraph amount(BakeLine(), 1);
	const auto samples = Samples(std::get<ArrayValue>(amount.Run("segments").Data));
	REQUIRE(samples.size() == 3);
	CHECK(samples[0] == Vector3{0, 0, 0});
	CHECK(samples[1] == Vector3{5, 0, .5});
	CHECK(samples[2].X == Catch::Approx(9.99));
	CHECK(samples[2].Z == 1);
	const auto output = amount.Run("path");
	const auto &baked = std::get<Path2D>(output.Data);
	REQUIRE(baked.SourceOperation);
	REQUIRE(baked.SourceOperation->Baked);
	CHECK(baked.SourceOperation->Baked->Lines.front() == samples);
	CHECK_FALSE(baked.Loop);
}
TEST_CASE(
	"Baked sampling keeps signed modulo and default weight after native roundtrip", "[imagegraph][path_bake]"
) {
	BakeGraph graph(BakeLine());
	const auto result = graph.Run("path");
	const auto baked = std::get<Path2D>(result.Data);
	Document stored;
	stored.FormatVersion = 9;
	stored.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", baked}, {"ratio", 1.}}}};
	stored.Outputs = {{"position", "sample", "position"}};
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(stored), restored, diagnostic) == Status::Ok);
	CHECK(restored == stored);
	Document preserved = restored;
	CHECK(Read(Write(stored), restored, diagnostic, 1) == Status::LimitExceeded);
	CHECK(restored == preserved);
	auto malformed = Write(stored);
	const auto marker = malformed.find("bake 1 3");
	REQUIRE(marker != std::string::npos);
	malformed.replace(marker, 8, "bake 1 4096");
	CHECK(Read(malformed, restored, diagnostic) != Status::Ok);
	CHECK(restored == preserved);
	const Node node{"runtime", "pc.path_sample", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, std::get<Path2D>(restored.Nodes[0].Values[0].Data)));
	CHECK(runtime.Length() == 8);
	CHECK(runtime.SegmentCount() == 3);
	CHECK(runtime.AccumulatedCount() == 3);
	CHECK(runtime.AccumulatedAt(0) == 0);
	CHECK(runtime.AccumulatedAt(2) == 8);
	SourcePathPointBuffer reused;
	reused.Weight = 7;
	runtime.PointDistanceInto(-2, 0, reused);
	CHECK(reused.Position.X == -2);
	CHECK(reused.Weight == 7);
	runtime.PointDistanceInto(-2, 0, reused);
	CHECK(reused.Weight == 1);
	reused.Weight = 9;
	runtime.PointDistanceInto(6, 0, reused);
	CHECK(reused.Position.X == 6);
	CHECK(reused.Weight == 9);
	runtime.PointDistanceInto(14, 0, reused);
	CHECK(reused.Position.X == 6);
	CHECK(reused.Weight == 1);
	CHECK(runtime.PointRatio(1).X == 0);
	CHECK(runtime.PointRatio(1.5).X == 4);
	CHECK(runtime.PointDistance(-2).X == -2);
	CHECK(runtime.PointRatio(.5).Weight == 1);
	CHECK(runtime.PointDistance(.5).Weight == 1);
}
TEST_CASE("Bake Path retains closed endpoints and multiline spread shape", "[imagegraph][path_bake]") {
	BakeGraph closed(BakeLine(true), 1);
	const auto samples = Samples(std::get<ArrayValue>(closed.Run("segments").Data));
	CHECK(samples == std::vector<Vector3>{{0, 0, 0}, {10, 0, .5}, {0, 0, 1}, {0, 0, 1}});
	Path2D combined;
	auto &operation = combined.SourceOperation.emplace();
	operation.Kind = SourcePathOperationKind::Combine;
	operation.Inputs = {BakeLine(), Path2D{}};
	BakeGraph multi(std::move(combined));
	const auto segments = std::get<ArrayValue>(multi.Run("segments").Data);
	REQUIRE(segments.Items.size() == 2);
	CHECK(Samples(segments, false) == std::vector<Vector3>{{0, 0, 0}, {4, 0, .4}, {8, 0, .8}});
	CHECK(std::get<std::vector<SourceArrayItem>>(segments.Items[1].Data).empty());
	const auto baked = std::get<Path2D>(multi.Run("path").Data);
	CHECK(baked.SourceOperation->Baked->Lines.size() == 2);
	BakeGraph nested(BakeLine(), 0, false);
	CHECK(Samples(std::get<ArrayValue>(nested.Run("segments").Data), false).size() == 3);
}
TEST_CASE(
	"Bake Path rejects zero steps, oversized work and byte limits before publication",
	"[imagegraph][path_bake]"
) {
	const Node node{"bake", "pc.path_bake", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(entry);
	REQUIRE(executor);
	EvaluationRequest request;
	for (int failure = 0; failure < 4; ++failure) {
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		detail::NodeContext context(node, *entry, request, budget);
		context.ByteBudget = failure == 1 ? 1 : Limits::MaximumEvaluationBytes;
		context.ProcessorCount = failure == 2 ? 1000000 : 1;
		context.Values = {
			{"path", BakeLine()},
			{"sample_type", EnumValue{0}},
			{"segment_length",
			 failure == 0	? 0.
			 : failure == 3 ? .000001
							: 4.}
		};
		CHECK_FALSE(executor(context));
		CHECK(context.FailureCode == (failure == 0 ? Status::InvalidValue : Status::LimitExceeded));
		CHECK(context.OutputValues.empty());
		CHECK(budget.Used() == 0);
	}
}

TEST_CASE(
	"Bake Path empty and reversed-length lines keep source sampling decisions", "[imagegraph][path_bake]"
) {
	BakeGraph empty(Path2D{});
	CHECK(std::get<ArrayValue>(empty.Run("segments").Data).Items.empty());
	const auto zero = std::get<Path2D>(empty.Run("path").Data);
	const Node node{"runtime", "pc.path_sample", "", {}, {}};
	const auto *entry = FindCatalogueEntry(node.Type);
	REQUIRE(entry);
	EvaluationRequest request;
	detail::NodeContext context(node, *entry, request);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	detail::PathRuntime runtime;
	REQUIRE(runtime.Init(context, zero));
	SourcePathPointBuffer reused;
	reused.Position = {12, 13};
	reused.Weight = 5;
	runtime.PointDistanceInto(2, 0, reused);
	CHECK(reused.Position == Vector2{});
	CHECK(reused.Weight == 5);
	Path2D reversed;
	auto &trim = reversed.SourceOperation.emplace();
	trim.Kind = SourcePathOperationKind::Trim;
	trim.Inputs = {BakeLine()};
	trim.TrimRange = {1, 0};
	BakeGraph negativeLength(reversed);
	CHECK(std::get<ArrayValue>(negativeLength.Run("segments").Data).Items.empty());
	BakeGraph negativeAmount(std::move(reversed), 1);
	const auto points = Samples(std::get<ArrayValue>(negativeAmount.Run("segments").Data));
	REQUIRE(points.size() == 3);
	CHECK(points[0].X == Catch::Approx(9.999));
	CHECK(points[1].X == Catch::Approx(5));
	CHECK(points[2].X == Catch::Approx(.01));
}
