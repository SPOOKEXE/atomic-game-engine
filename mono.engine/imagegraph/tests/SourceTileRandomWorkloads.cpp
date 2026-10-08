#include "ProcessorBatch.hpp"

#include <engine/core/FrameGraph.hpp>
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
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_tile_random_workloads")
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
			"pc.tile_random",
			"",
			{},
			{{"dimension", std::move(dimensions)},
			 {"dimension_unit", EnumValue{0}},
			 {"randomness", ArrayValue{ValueType::Scalar, {0., .5, 1.}}}}
		};
	}

	void Inputs(detail::NodeContext &context, const Image &source, SurfaceFormat format) {
		context.InheritedSurfaceFormat = format;
		context.InheritedInterpolation = 1;
		context.Images.emplace_back("surface_in", &source);
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

	ImageArray Run(const Node &node, const Image &source, SurfaceFormat format) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source, format);
		ProfileFrame frame;
		{
			ENGINE_PROFILE("imagegraph.tile_random.workload");
			const bool processed = detail::RunProcessorBatch(context, executor);
			INFO(context.FailureMessage);
			REQUIRE(processed);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name : {"imagegraph.tile_random.workload", "imagegraph.source.tile_random"}) {
			const auto count = std::count_if(spans.begin(), spans.end(), [&](const auto &span) {
				if (span.Name != name) return false;
				CHECK(std::isfinite(span.Milliseconds));
				CHECK(std::isfinite(span.SelfMilliseconds));
				CHECK(span.Milliseconds >= 0);
				CHECK(span.SelfMilliseconds >= 0);
				CHECK(span.Category == engine::core::ProfileCategory::Engine);
				CHECK(span.Owner == engine::core::ProfileOwner::Engine);
				return true;
			});
			INFO(name);
			CHECK(count == (std::string_view(name) == "imagegraph.source.tile_random" ? 3 : 1));
		}
		REQUIRE(context.OutputImages.empty());
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays.front().first == "surface_out");
		return std::move(context.OutputImageArrays.front().second);
	}

	uint64_t Verify(const ImageArray &output) {
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
			CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
			const auto layout = CheckedSurfaceLayout(
				image.Width, image.Height, SurfaceFormat::RGBA8Unorm, Limits::MaximumOutputBytes
			);
			REQUIRE(layout);
			CHECK(image.Pixels.size() == layout->Bytes);
			storedBytes += image.Pixels.size();
			expectedBytes += uint64_t(OUTPUT_SIZES[row].X) * uint64_t(OUTPUT_SIZES[row].Y) * 4;
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

	void CheckBudget(const Node &node, const Image &source, SurfaceFormat format) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = 1;
		Inputs(context, source, format);
		const bool processed = detail::RunProcessorBatch(context, executor);
		INFO(context.FailureMessage);
		CHECK_FALSE(processed);
		CHECK(context.FailureCode == Status::LimitExceeded);
		CHECK(context.OutputImages.empty());
		CHECK(context.OutputImageArrays.empty());
		CHECK(context.OutputValues.empty());
		CHECK(context.OutputCharge.Bytes() == 0);
	}

	void CheckLateRowAdmission(const Image &source) {
		auto node = WorkloadNode();
		node.Values.front().Data =
			ArrayValue{ValueType::Vector2, {Vector2{64, 48}, Vector2{96, 64}, Vector2{512, 512}}};
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source, SurfaceFormat::RGBA8Unorm);
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
		const auto spans = engine::core::FrameGraph::Spans();
		CHECK(engine::core::FrameGraph::Dropped() == 0);
		CHECK(std::any_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.tile_random_admission";
		}));
		CHECK(std::none_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.tile_random";
		}));
	}
}

TEST_CASE(
	"Tile Random mixed row workloads retain deterministic numeric surfaces",
	"[imagegraph][tile_random_workloads]"
) {
	const auto node = WorkloadNode();
	for (const auto format : FORMATS) {
		INFO("format " << unsigned(format));
		const auto source = Source(format);
		const auto first = Run(node, source, format);
		const auto firstHash = Verify(first);
		const auto second = Run(node, source, format);
		CHECK(Verify(second) == firstHash);
		CHECK(second.Images == first.Images);
		CHECK(second.Items == first.Items);
		CheckBudget(node, source, format);
	}
	CheckLateRowAdmission(Source(SurfaceFormat::RGBA8Unorm));
}
