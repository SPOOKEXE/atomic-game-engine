#include "ProcessorBatch.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_repeat_texture_workloads")
using namespace engine::imagegraph;
namespace {
	constexpr std::array<Vector2, 3> OUTPUT_SIZES{{{64, 48}, {96, 64}, {128, 96}}};
	constexpr std::array<SurfaceFormat, 7> FORMATS{
		SurfaceFormat::RGBA4Unorm,
		SurfaceFormat::RGBA8Unorm,
		SurfaceFormat::RGBA16Float,
		SurfaceFormat::RGBA32Float,
		SurfaceFormat::R8Unorm,
		SurfaceFormat::R16Float,
		SurfaceFormat::R32Float
	};

	Image Source(SurfaceFormat format) {
		const auto layout = CheckedSurfaceLayout(16, 12, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image image{16, 12, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < image.Height; ++y)
			for (uint32_t x = 0; x < image.Width; ++x)
				REQUIRE(StoreSurfacePixel(
					image,
					x,
					y,
					{double(x % 5) / 4,
					 double(y % 4) / 3,
					 double((x + y) % 7) / 6,
					 .25 + .75 * double((x * 3 + y) % 5) / 4}
				));
		return image;
	}

	Node WorkloadNode() {
		ArrayValue dimensions{ValueType::Vector2, {}};
		for (const auto size : OUTPUT_SIZES)
			dimensions.Elements.emplace_back(size);
		return {
			"tile",
			"pc.repeat_texture",
			"",
			{},
			{{"target_dimension", std::move(dimensions)},
			 {"target_dimension_unit", EnumValue{0}},
			 {"randomness", ArrayValue{ValueType::Scalar, {.25, .5, 1.}}},
			 {"type", ArrayValue{ValueType::Integer, {int64_t{0}, int64_t{1}, int64_t{2}}}},
			 {"seed", ArrayValue{ValueType::Scalar, {32., 64., 96.}}}}
		};
	}

	void Inputs(detail::NodeContext &context, const ImageArray &source) {
		context.InheritedSurfaceFormat = SurfaceFormat::RGBA32Float;
		context.InheritedInterpolation = 1;
		context.ImageArrays.emplace_back("surface_in", &source);
		for (const auto &input : context.Entry.Inputs) {
			const auto authored = std::find_if(
				context.Authored.Values.begin(), context.Authored.Values.end(), [&](const auto &value) {
					return value.Port == input.Id;
				}
			);
			if (authored != context.Authored.Values.end()) {
				context.Values.emplace_back(input.Id, authored->Data);
			} else if (const auto fallback = CatalogueDefault(input)) {
				context.Values.emplace_back(input.Id, *fallback);
				context.CatalogueDefaultInputs.emplace_back(input.Id);
			}
		}
		context.InputProvenanceResolved = true;
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

	ImageArray Run(const Node &node, const ImageArray &source) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const auto bytesBefore = Counter("imagegraph.image.allocated_payload_bytes");
		ProfileFrame frame;
		{
			ENGINE_PROFILE("imagegraph.repeat_texture.workload");
			const bool processed = detail::RunProcessorBatch(context, executor);
			INFO(context.FailureMessage);
			REQUIRE(processed);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name :
			 {"imagegraph.repeat_texture.workload",
			  "imagegraph.source.repeat_texture",
			  "imagegraph.source.repeat_texture_admission",
			  "imagegraph.image.allocate"}) {
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
			CHECK(
				count == (std::string_view(name) == "imagegraph.repeat_texture.workload"
							  ? size_t{1}
							  : OUTPUT_SIZES.size())
			);
		}
		REQUIRE(context.OutputImages.empty());
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays.front().first == "surface_out");
		uint64_t storedBytes = 0;
		for (const auto &image : context.OutputImageArrays.front().second.Images)
			storedBytes += image.Pixels.size();
		CHECK(Counter("imagegraph.image.allocations") - allocationsBefore == OUTPUT_SIZES.size());
		CHECK(Counter("imagegraph.image.allocated_payload_bytes") - bytesBefore == storedBytes);
		return std::move(context.OutputImageArrays.front().second);
	}

	uint64_t Verify(const ImageArray &output, const std::array<SurfaceFormat, 3> &formats) {
		REQUIRE(output.Images.size() == OUTPUT_SIZES.size());
		REQUIRE(output.Items.size() == OUTPUT_SIZES.size());
		uint64_t hash = 14695981039346656037ull, storedBytes = 0, expectedBytes = 0;
		for (size_t row = 0; row < output.Images.size(); ++row) {
			const auto *imageIndex = std::get_if<size_t>(&output.Items[row].Data);
			REQUIRE(imageIndex);
			CHECK(*imageIndex == row);
			const auto &image = output.Images[row];
			CHECK(image.Width == uint32_t(OUTPUT_SIZES[row].X));
			CHECK(image.Height == uint32_t(OUTPUT_SIZES[row].Y));
			CHECK(image.Format == formats[row]);
			const auto layout = CheckedSurfaceLayout(
				uint32_t(OUTPUT_SIZES[row].X),
				uint32_t(OUTPUT_SIZES[row].Y),
				formats[row],
				Limits::MaximumOutputBytes
			);
			REQUIRE(layout);
			CHECK(image.Pixels.size() == layout->Bytes);
			storedBytes += image.Pixels.size();
			expectedBytes += layout->Bytes;
			for (const auto byte : image.Pixels)
				hash = (hash ^ byte) * 1099511628211ull;
			SurfacePixel first;
			REQUIRE(LoadSurfacePixel(image, 0, 0, first));
			bool varied = false, finite = true;
			for (uint32_t y = 0; y < image.Height; ++y)
				for (uint32_t x = 0; x < image.Width; ++x) {
					SurfacePixel pixel;
					REQUIRE(LoadSurfacePixel(image, x, y, pixel));
					for (const auto channel : pixel)
						finite = finite && std::isfinite(channel);
					varied = varied || pixel != first;
				}
			CHECK(varied);
			CHECK(finite);
		}
		CHECK(storedBytes == expectedBytes);
		return hash;
	}

	void CheckBudget(const Node &node, const ImageArray &source) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = 1;
		Inputs(context, source);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const auto bytesBefore = Counter("imagegraph.image.allocated_payload_bytes");
		const bool processed = detail::RunProcessorBatch(context, executor);
		INFO(context.FailureMessage);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputCharge.Bytes() == 0);
		CHECK(Counter("imagegraph.image.allocations") == allocationsBefore);
		CHECK(Counter("imagegraph.image.allocated_payload_bytes") == bytesBefore);
	}

	void CheckLateRowAdmission(const ImageArray &source, bool byteLimit = false) {
		auto node = WorkloadNode();
		if (!byteLimit)
			node.Values.front().Data =
				ArrayValue{ValueType::Vector2, {Vector2{64, 48}, Vector2{96, 64}, Vector2{512, 512}}};
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = byteLimit ? 24 * 1024 : Limits::MaximumEvaluationBytes;
		Inputs(context, source);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const auto bytesBefore = Counter("imagegraph.image.allocated_payload_bytes");
		ProfileFrame frame;
		const bool processed = detail::RunProcessorBatch(context, executor);
		frame.Finish();
		INFO(context.FailureMessage);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputCharge.Bytes() == 0);
		CHECK(Counter("imagegraph.image.allocations") == allocationsBefore);
		CHECK(Counter("imagegraph.image.allocated_payload_bytes") == bytesBefore);
		const auto spans = engine::core::FrameGraph::Spans();
		CHECK(engine::core::FrameGraph::Dropped() == 0);
		CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.repeat_texture_admission";
		}));
		CHECK(std::none_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.repeat_texture";
		}));
	}
}

TEST_CASE(
	"Repeat Texture mixed row workloads retain deterministic numeric surfaces",
	"[imagegraph][repeat_texture_workloads]"
) {
	ImageArray source;
	const std::array<SurfaceFormat, 3> inputFormats{
		SurfaceFormat::RGBA4Unorm, SurfaceFormat::RGBA16Float, SurfaceFormat::R32Float
	};
	for (const auto format : inputFormats) {
		source.Items.push_back({source.Images.size()});
		source.Images.push_back(Source(format));
	}
	for (int64_t depth = 0; depth <= 8; ++depth) {
		INFO("depth " << depth);
		auto node = WorkloadNode();
		node.Values.push_back({"attribute_color_depth", EnumValue{depth}});
		std::array<SurfaceFormat, 3> outputFormats;
		outputFormats.fill(inputFormats.front());
		if (depth == 1) outputFormats.fill(SurfaceFormat::RGBA32Float);
		if (depth >= 2) outputFormats.fill(FORMATS[size_t(depth - 2)]);
		const auto first = Run(node, source);
		const auto firstHash = Verify(first, outputFormats);
		const auto second = Run(node, source);
		CHECK(Verify(second, outputFormats) == firstHash);
		CHECK(second.Images == first.Images);
		CHECK(second.Items == first.Items);
		CheckBudget(node, source);
	}
	CheckLateRowAdmission(source);
	CheckLateRowAdmission(source, true);
}
