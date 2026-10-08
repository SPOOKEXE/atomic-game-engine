#include "ProcessorBatch.hpp"
#include "nodes/Path.hpp"

#include <engine/core/FrameGraph.hpp>
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

TEST_SUITE_ID("engine.imagegraph.source_path_repeat_workloads")
using namespace engine::imagegraph;
namespace {
	constexpr std::array<int64_t, 3> COPY_COUNTS{8, 12, 16};
	constexpr size_t INPUT_LINES = 3, ANCHORS_PER_LINE = 12, SAMPLES_PER_LINE = 16;

	Document RepeatWorkload(bool circular) {
		Path2D source;
		auto &combined = source.SourceOperation.emplace();
		combined.Kind = SourcePathOperationKind::Combine;
		for (size_t line = 0; line < INPUT_LINES; ++line) {
			Path2D path;
			for (size_t anchor = 0; anchor < ANCHORS_PER_LINE; ++anchor)
				path.Anchors.push_back(
					{{double(anchor) * 4, double(line) * 7 + double(anchor % 3), 0, 0, 0, 0}, 0}
				);
			path.Weights = {{0, double(line + 1)}, {100, double(line + 1)}};
			combined.Inputs.push_back(std::move(path));
		}
		ArrayValue amounts{ValueType::Integer, {}};
		for (const auto copies : COPY_COUNTS)
			amounts.Elements.emplace_back(copies);
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"repeat",
			 "pc.path_repeat",
			 "",
			 {},
			 {{"path", std::move(source)},
			  {"amount", std::move(amounts)},
			  {"pattern", EnumValue{circular ? 1 : 0}},
			  {"position", Vector2{3, 5}},
			  {"shift_position",
			   ArrayValue{ValueType::Vector2, {Vector2{2, 1}, Vector2{-1, 3}, Vector2{4, -2}}}},
			  {"scale", Vector2{1.5, .75}},
			  {"shift_scale", Vector2{.99, 1.01}},
			  {"rotation", 17.},
			  {"shift_rotation", 3.},
			  {"center", Vector2{64, 48}},
			  {"radius", Vector2{24, 16}},
			  {"rotate_along", true}}}
		};
		for (const auto *port :
			 {"center_unit", "radius_unit", "position_unit", "shift_position_unit", "anchor_unit"})
			document.Nodes[0].Values.push_back({port, EnumValue{0}});
		document.Outputs = {{"paths", "repeat", "path"}};
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

	uint64_t SampleHash(const ArrayValue &paths) {
		ENGINE_PROFILE("imagegraph.repeat.workload.samples");
		REQUIRE(paths.ElementType == ValueType::Path2D);
		REQUIRE(paths.Elements.size() == COPY_COUNTS.size());
		REQUIRE(paths.Nested.empty());
		REQUIRE(paths.Items.empty());
		uint64_t hash = 14695981039346656037ull;
		const auto hashNumber = [&](double number) {
			REQUIRE(std::isfinite(number));
			const auto bits = std::bit_cast<uint64_t>(number);
			for (unsigned shift = 0; shift < 64; shift += 8)
				hash = (hash ^ uint8_t(bits >> shift)) * 1099511628211ull;
		};
		Node node{"sample", "pc.path_sample", "", {}, {}};
		EvaluationRequest request;
		const auto *entry = FindCatalogueEntry(node.Type);
		REQUIRE(entry);
		for (size_t row = 0; row < paths.Elements.size(); ++row) {
			const auto *path = std::get_if<Path2D>(&paths.Elements[row]);
			REQUIRE(path);
			REQUIRE(path->SourceOperation);
			REQUIRE(path->SourceOperation->Kind == SourcePathOperationKind::Repeat);
			detail::NodeContext context{node, *entry, request};
			context.ByteBudget = Limits::MaximumEvaluationBytes;
			detail::PathRuntime runtime;
			const bool initialized = runtime.Init(context, *path);
			INFO(context.FailureMessage);
			REQUIRE(initialized);
			REQUIRE(runtime.LineCount() == size_t(COPY_COUNTS[row]) * INPUT_LINES);
			for (size_t line = 0; line < runtime.LineCount(); ++line) {
				hashNumber(runtime.Length(line));
				for (size_t sample = 0; sample < SAMPLES_PER_LINE; ++sample) {
					SourcePathPointBuffer point;
					runtime.PointRatioInto((double(sample) + .5) / SAMPLES_PER_LINE, line, point);
					REQUIRE(context.FailureCode == Status::Ok);
					CHECK(point.Weight == double(line % INPUT_LINES + 1));
					hashNumber(point.Position.X);
					hashNumber(point.Position.Y);
					hashNumber(point.Weight);
				}
			}
		}
		return hash;
	}

	uint64_t ProfileWorkload(const Document &document, const Plan &plan, EvaluatedValue &output) {
		ProfileFrame frame;
		uint64_t hash = 0;
		{
			ENGINE_PROFILE("imagegraph.repeat.workload");
			Diagnostic diagnostic;
			const auto evaluated = EvaluateValue(document, plan, "paths", {}, output, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(evaluated == Status::Ok);
			const auto *paths = std::get_if<ArrayValue>(&output.Data);
			REQUIRE(paths);
			hash = SampleHash(*paths);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name :
			 {"imagegraph.repeat.workload",
			  "imagegraph.repeat.workload.samples",
			  "imagegraph.source.path_repeat"}) {
			const auto count = std::count_if(spans.begin(), spans.end(), [&](const auto &span) {
				if (span.Name != name) return false;
				CHECK(std::isfinite(span.Milliseconds));
				CHECK(span.Milliseconds >= 0);
				CHECK(span.SelfMilliseconds >= 0);
				CHECK(span.Category == engine::core::ProfileCategory::Engine);
				CHECK(span.Owner == engine::core::ProfileOwner::Engine);
				return true;
			});
			INFO(name);
			CHECK(count == (std::string_view(name) == "imagegraph.source.path_repeat" ? 3 : 1));
		}
		return hash;
	}

	void CheckWorkloadBudget(const Document &document) {
		const auto &node = document.Nodes.front();
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = 1;
		for (const auto &input : entry->Inputs) {
			const auto authored =
				std::find_if(node.Values.begin(), node.Values.end(), [&](const auto &value) {
					return value.Port == input.Id;
				});
			if (authored != node.Values.end()) {
				context.Values.emplace_back(input.Id, authored->Data);
			} else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
		const auto processed = detail::RunProcessorBatch(context, executor);
		INFO(context.FailureMessage);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.OutputCharge.Bytes() == 0);
	}
}

TEST_CASE(
	"Repeat multiline processor workloads record bounded work and deterministic samples",
	"[imagegraph][path_repeat][path_repeat_workloads][workload]"
) {
	std::array<uint64_t, 2> hashes{};
	for (bool circular : {false, true}) {
		INFO("circular " << circular);
		const auto document = RepeatWorkload(circular);
		Diagnostic diagnostic;
		Plan plan;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluatedValue output;
		const auto firstHash = ProfileWorkload(document, plan, output);
		const auto retained = output.Data;
		const auto secondHash = ProfileWorkload(document, plan, output);
		CHECK(firstHash == secondHash);
		CHECK(output.Data == retained);
		hashes[circular ? 1 : 0] = firstHash;
		CheckWorkloadBudget(document);
	}
	CHECK(hashes[0] != hashes[1]);
}
