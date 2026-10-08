#include "ProcessorBatch.hpp"
#include "SourcePathShiftMemo.hpp"
#include "nodes/Path.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

TEST_SUITE_ID("engine.imagegraph.source_path_wave_workloads")
using namespace engine::imagegraph;
namespace {
	constexpr size_t PROCESSOR_ROWS = 3, INPUT_LINES = 3, ANCHORS_PER_LINE = 12, SAMPLES_PER_LINE = 32;

	Document WaveWorkload() {
		Path2D source;
		auto &combined = source.SourceOperation.emplace();
		combined.Kind = SourcePathOperationKind::Combine;
		for (size_t line = 0; line < INPUT_LINES; ++line) {
			Path2D path;
			for (size_t anchor = 0; anchor < ANCHORS_PER_LINE; ++anchor)
				path.Anchors.push_back(
					{{double(anchor) * 8, double(line) * 10 + double(anchor % 3), 0, 0, 0, 0}, 0}
				);
			path.Weights = {{0, double(line + 1)}, {100, double(line + 1)}};
			combined.Inputs.push_back(std::move(path));
		}
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"wave",
			 "pc.path_wave",
			 "",
			 {},
			 {{"path", std::move(source)},
			  {"seed", ArrayValue{ValueType::Scalar, {17., 31., 47.}}},
			  {"mode", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}}},
			  {"direction", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{0}}}},
			  {"wiggle", ArrayValue{ValueType::Boolean, {false, true, true}}},
			  {"range", Vector2{0, 1}},
			  {"frequency", Vector2{2, 5}},
			  {"amplitude", Vector2{2, 4}},
			  {"phase", Vector2{15, 45}},
			  {"angle", Vector2{35, 75}},
			  {"iteration", int64_t{3}},
			  {"freqency", 1.5},
			  {"amplitude_2", .6},
			  {"shift_2", .1},
			  {"wiggle_amplitude", Vector2{-1, 1}},
			  {"wiggle_frequency", 4.},
			  {"use_weight", true},
			  {"weight_mode", EnumValue{0}},
			  {"range_2", Vector2{.5, 1.5}}}}
		};
		document.Outputs = {{"paths", "wave", "path"}};
		return document;
	}

	struct ProfileFrame {
		bool PreviouslyEnabled = engine::core::FrameGraph::IsEnabled();
		bool Open = true;
		ProfileFrame() {
			engine::core::FrameGraph::SetEnabled(true);
			engine::core::FrameGraph::BeginFrame();
		}
		void Finish() {
			engine::core::FrameGraph::EndFrame();
			Open = false;
		}
		~ProfileFrame() {
			if (Open) engine::core::FrameGraph::EndFrame();
			engine::core::FrameGraph::SetEnabled(PreviouslyEnabled);
		}
	};

	double Counter(std::string_view name) {
		const auto counter = engine::core::Metrics::Get(name);
		return counter ? counter->Value : 0;
	}

	uint64_t Samples(const ArrayValue &paths) {
		ENGINE_PROFILE("imagegraph.path_wave.workload.samples");
		REQUIRE(paths.ElementType == ValueType::Path2D);
		REQUIRE(paths.Elements.size() == PROCESSOR_ROWS);
		REQUIRE(paths.Nested.empty());
		REQUIRE(paths.Items.empty());
		uint64_t hash = 14695981039346656037ull;
		const auto number = [&](double value) {
			REQUIRE(std::isfinite(value));
			const auto bits = std::bit_cast<uint64_t>(value);
			for (unsigned shift = 0; shift < 64; shift += 8)
				hash = (hash ^ uint8_t(bits >> shift)) * 1099511628211ull;
		};
		for (size_t row = 0; row < paths.Elements.size(); ++row) {
			const auto *path = std::get_if<Path2D>(&paths.Elements[row]);
			REQUIRE(path);
			REQUIRE(path->SourceOperation);
			REQUIRE(path->SourceOperation->Kind == SourcePathOperationKind::Wave);
			REQUIRE(path->SourceOperation->Wave);
			CHECK(path->SourceOperation->Wave->Seed == (row == 0 ? 17. : row == 1 ? 32. : 49.));
			CHECK(path->SourceOperation->Wave->Mode == row);
			Node node{"sample", "pc.path_sample", "", {}, {}};
			const auto *entry = FindCatalogueEntry(node.Type);
			REQUIRE(entry);
			EvaluationRequest request;
			detail::NodeContext context{node, *entry, request};
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			detail::SourcePathShiftMemo memo;
			context.PathShiftMemo = &memo;
			context.Values = {{"path", *path}};
			REQUIRE(detail::StampSourcePathShiftInputs(context));
			detail::PathRuntime runtime;
			const bool initialized = runtime.Init(context, std::get<Path2D>(context.Values.front().second));
			INFO(context.FailureMessage);
			REQUIRE(initialized);
			REQUIRE(runtime.LineCount() == INPUT_LINES);
			const auto missesBefore = Counter("imagegraph.path.wave.cache_misses");
			const auto childrenBefore = Counter("imagegraph.path.wave.child_samples");
			const auto iterationsBefore = Counter("imagegraph.path.wave.iterations");
			std::array<SourcePathPointBuffer, INPUT_LINES * SAMPLES_PER_LINE> first{};
			for (size_t line = 0; line < INPUT_LINES; ++line)
				for (size_t sample = 0; sample < SAMPLES_PER_LINE; ++sample) {
					auto &point = first[line * SAMPLES_PER_LINE + sample];
					runtime.PointRatioInto((double(sample) + .5) / SAMPLES_PER_LINE, line, point);
					REQUIRE(context.FailureCode == Status::Ok);
					number(point.Position.X);
					number(point.Position.Y);
					number(point.Weight);
				}
			const auto cacheEntries = memo.Entries.size();
			REQUIRE(cacheEntries >= INPUT_LINES * SAMPLES_PER_LINE);
			CHECK(
				Counter("imagegraph.path.wave.cache_misses") - missesBefore == INPUT_LINES * SAMPLES_PER_LINE
			);
			CHECK(
				Counter("imagegraph.path.wave.child_samples") - childrenBefore ==
				INPUT_LINES * SAMPLES_PER_LINE * (row == 1 ? 1 : 3)
			);
			CHECK(
				Counter("imagegraph.path.wave.iterations") - iterationsBefore ==
				INPUT_LINES * SAMPLES_PER_LINE * 3
			);
			const auto hitsBefore = Counter("imagegraph.path.wave.cache_hits");
			const auto missesAfter = Counter("imagegraph.path.wave.cache_misses");
			const auto childrenAfter = Counter("imagegraph.path.wave.child_samples");
			const auto iterationsAfter = Counter("imagegraph.path.wave.iterations");
			for (size_t line = 0; line < INPUT_LINES; ++line)
				for (size_t sample = 0; sample < SAMPLES_PER_LINE; ++sample) {
					SourcePathPointBuffer point;
					runtime.PointRatioInto((double(sample) + .5) / SAMPLES_PER_LINE, line, point);
					REQUIRE(context.FailureCode == Status::Ok);
					CHECK(point == first[line * SAMPLES_PER_LINE + sample]);
				}
			CHECK(memo.Entries.size() == cacheEntries);
			CHECK(Counter("imagegraph.path.wave.cache_hits") - hitsBefore == INPUT_LINES * SAMPLES_PER_LINE);
			CHECK(Counter("imagegraph.path.wave.cache_misses") == missesAfter);
			CHECK(Counter("imagegraph.path.wave.child_samples") == childrenAfter);
			CHECK(Counter("imagegraph.path.wave.iterations") == iterationsAfter);
		}
		return hash;
	}

	uint64_t Run(const Document &document, const Plan &plan, EvaluatedValue &output) {
		ProfileFrame frame;
		uint64_t hash = 0;
		{
			ENGINE_PROFILE("imagegraph.path_wave.workload");
			Diagnostic diagnostic;
			const auto evaluated = EvaluateValue(document, plan, "paths", {}, output, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(evaluated == Status::Ok);
			const auto *paths = std::get_if<ArrayValue>(&output.Data);
			REQUIRE(paths);
			hash = Samples(*paths);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name :
			 {"imagegraph.path_wave.workload",
			  "imagegraph.path_wave.workload.samples",
			  "imagegraph.source.path_wave",
			  "imagegraph.source.path_wave.admission",
			  "imagegraph.source.path_wave.sample"}) {
			const auto count = size_t(std::count_if(spans.begin(), spans.end(), [&](const auto &span) {
				if (span.Name != name) return false;
				CHECK(std::isfinite(span.Milliseconds));
				CHECK(std::isfinite(span.SelfMilliseconds));
				CHECK(span.Milliseconds >= 0);
				CHECK(span.SelfMilliseconds >= 0);
				CHECK(span.Category == engine::core::ProfileCategory::Engine);
				CHECK(span.Owner == engine::core::ProfileOwner::Engine);
				return true;
			}));
			INFO(name);
			const std::string_view scope = name;
			if (scope == "imagegraph.source.path_wave.sample")
				CHECK(count >= PROCESSOR_ROWS * INPUT_LINES * SAMPLES_PER_LINE * 2);
			else
				CHECK(
					count == (scope == "imagegraph.source.path_wave" ||
									  scope == "imagegraph.source.path_wave.admission"
								  ? PROCESSOR_ROWS
								  : 1)
				);
		}
		return hash;
	}

	void CheckBudget(const Document &document, bool lateRow = false) {
		const auto &node = document.Nodes.front();
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = lateRow ? Limits::MaximumEvaluationBytes : 1;
		detail::SourcePathShiftMemo memo;
		context.PathShiftMemo = &memo;
		for (const auto &input : entry->Inputs) {
			const auto authored =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
					return value.Port == input.Id;
				});
			if (authored != node.Values.end())
				context.Values.emplace_back(input.Id, authored->Data);
			else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		ProfileFrame frame;
		const bool processed = detail::RunProcessorBatch(context, executor);
		frame.Finish();
		INFO(context.FailureMessage);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.OutputCharge.Bytes() == 0);
		if (lateRow) {
			const auto spans = engine::core::FrameGraph::Spans();
			CHECK(engine::core::FrameGraph::Dropped() == 0);
			CHECK(size_t(std::count_if(spans.begin(), spans.end(), [](const auto &span) {
					  return span.Name == "imagegraph.source.path_wave.admission";
				  })) == PROCESSOR_ROWS);
			CHECK(std::none_of(spans.begin(), spans.end(), [](const auto &span) {
				return span.Name == "imagegraph.source.path_wave";
			}));
		}
	}
}

TEST_CASE(
	"Wave multiline processor workload records deterministic seeded samples and cache hits",
	"[imagegraph][path_wave_workloads]"
) {
	const auto document = WaveWorkload();
	Diagnostic diagnostic;
	Plan plan;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId << ":" << diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	EvaluatedValue output;
	const auto firstHash = Run(document, plan, output);
	const auto retained = output.Data;
	CHECK(Run(document, plan, output) == firstHash);
	CHECK(output.Data == retained);
	CheckBudget(document);
	auto rejected = document;
	for (auto &input : rejected.Nodes.front().Values)
		if (input.Port == "iteration")
			input.Data = ArrayValue{ValueType::Integer, {int64_t{3}, int64_t{3}, int64_t{4097}}};
	CheckBudget(rejected, true);
}
