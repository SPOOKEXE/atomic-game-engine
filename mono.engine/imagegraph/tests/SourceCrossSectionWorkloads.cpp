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

TEST_SUITE_ID("engine.imagegraph.source_cross_section_workloads")
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

	Image Source(SurfaceFormat format, uint32_t width, uint32_t height) {
		const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
		REQUIRE(layout);
		Image image{width, height, std::vector<uint8_t>(layout->Bytes), 0, format};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double gray = .25 + (x >= width / 2 ? .25 : 0) + (y >= height / 2 ? .25 : 0);
				REQUIRE(StoreSurfacePixel(image, x, y, {gray, gray, gray, 1}));
			}
		return image;
	}

	Node WorkloadNode(int64_t axis, int64_t mode, bool antiAliasing, bool toAlpha) {
		return {
			"section",
			"pc.cross_section",
			"",
			{},
			{{"axis", EnumValue{axis}},
			 {"mode", EnumValue{mode}},
			 {"position", .5},
			 {"level", Vector2{.05, .95}},
			 {"anti_aliasing", antiAliasing},
			 {"to_alpha", toAlpha},
			 {"mask_alpha_only", true},
			 {"attribute_color_depth", EnumValue{5}}}
		};
	}

	void Inputs(detail::NodeContext &context, const ImageArray &source, const Image *mask = nullptr) {
		context.InheritedSurfaceFormat = SurfaceFormat::RGBA32Float;
		context.InheritedInterpolation = 1;
		context.ImageArrays.emplace_back("surface_in", &source);
		if (mask) context.Images.emplace_back("mask", mask);
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

	ImageArray Run(const Node &node, const ImageArray &source, const Image *mask = nullptr) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source, mask);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const auto bytesBefore = Counter("imagegraph.image.allocated_payload_bytes");
		ProfileFrame frame;
		{
			ENGINE_PROFILE("imagegraph.cross_section.workload");
			const bool processed = detail::RunProcessorBatch(context, executor);
			INFO(context.FailureMessage);
			REQUIRE(processed);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name :
			 {"imagegraph.cross_section.workload",
			  "imagegraph.source.cross_section",
			  "imagegraph.source.cross_section_admission",
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
				count == (std::string_view(name) == "imagegraph.cross_section.workload" ? size_t{1}
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

	uint8_t Byte(float value) {
		return uint8_t(std::lround(std::clamp(value, 0.f, 1.f) * 255));
	}

	std::array<uint8_t, 4> Oracle(
		const Image &source,
		uint32_t x,
		uint32_t y,
		int64_t axis,
		int64_t mode,
		bool antiAliasing,
		bool toAlpha,
		const Image *mask
	) {
		SurfacePixel color;
		const bool red = source.Format == SurfaceFormat::R8Unorm ||
						 source.Format == SurfaceFormat::R16Float || source.Format == SurfaceFormat::R32Float;
		if (red) {
			REQUIRE(LoadSurfacePixel(source, x, y, color));
			color = {color[0], color[0], color[0], 1};
		} else {
			const uint32_t sampleX = axis == 0 ? x : source.Width / 2;
			const uint32_t sampleY = axis == 0 ? source.Height / 2 : y;
			REQUIRE(LoadSurfacePixel(source, sampleX, sampleY, color));
			if (antiAliasing) {
				SurfacePixel previous;
				REQUIRE(LoadSurfacePixel(
					source, axis == 0 ? sampleX : sampleX - 1, axis == 0 ? sampleY - 1 : sampleY, previous
				));
				for (size_t channel = 0; channel < color.size(); ++channel)
					color[channel] = (color[channel] + previous[channel]) / 2;
			}
		}
		std::array<float, 4> pixel;
		for (size_t channel = 0; channel < pixel.size(); ++channel)
			pixel[channel] = float(color[channel]);
		if (!red) {
			const float brightness =
				1.f - (.2126f * pixel[0] + .7152f * pixel[1] + .0722f * pixel[2]) * pixel[3];
			const float threshold = .05f * (1.f - brightness) + .95f * brightness;
			const float position = axis == 0 ? (float(y) + .5f) / float(source.Height)
											 : 1.f - (float(x) + .5f) / float(source.Width);
			const float result = threshold <= position ? 1.f : 0.f;
			if (mode == 0) pixel = {result, result, result, 1};
			if (toAlpha) pixel[3] = result;
		}
		std::array<uint8_t, 4> expected;
		for (size_t channel = 0; channel < expected.size(); ++channel)
			expected[channel] = Byte(pixel[channel]);
		if (mask) {
			SurfacePixel coverage;
			REQUIRE(LoadSurfacePixel(*mask, 0, 0, coverage));
			const float alpha =
				(float(coverage[0]) + float(coverage[1]) + float(coverage[2])) / 3.f * float(coverage[3]);
			expected[3] = Byte(float(expected[3]) / 255.f * alpha);
		}
		return expected;
	}

	uint64_t Verify(
		const ImageArray &output,
		const ImageArray &source,
		int64_t axis,
		int64_t mode,
		bool antiAliasing,
		bool toAlpha,
		const Image *mask
	) {
		REQUIRE(output.Images.size() == OUTPUT_SIZES.size());
		REQUIRE(output.Items.size() == OUTPUT_SIZES.size());
		uint64_t hash = 14695981039346656037ull, storedBytes = 0;
		for (size_t row = 0; row < output.Images.size(); ++row) {
			const auto *imageIndex = std::get_if<size_t>(&output.Items[row].Data);
			REQUIRE(imageIndex);
			CHECK(*imageIndex == row);
			const auto &image = output.Images[row];
			CHECK(image.Width == uint32_t(OUTPUT_SIZES[row].X));
			CHECK(image.Height == uint32_t(OUTPUT_SIZES[row].Y));
			CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
			CHECK(image.Pixels.size() == size_t(image.Width) * image.Height * 4);
			storedBytes += image.Pixels.size();
			for (const auto byte : image.Pixels)
				hash = (hash ^ byte) * 1099511628211ull;
			for (const uint32_t y : {uint32_t{1}, image.Height / 2, image.Height - 2})
				for (const uint32_t x : {uint32_t{1}, image.Width / 4, image.Width / 2, image.Width - 2}) {
					const auto expected =
						Oracle(source.Images[row], x, y, axis, mode, antiAliasing, toAlpha, mask);
					const size_t offset = (size_t(y) * image.Width + x) * 4;
					for (size_t channel = 0; channel < expected.size(); ++channel)
						CHECK(std::abs(int(image.Pixels[offset + channel]) - int(expected[channel])) <= 1);
				}
		}
		CHECK(storedBytes == uint64_t{4} * (64 * 48 + 96 * 64 + 128 * 96));
		return hash;
	}

	void CheckAdmission(const ImageArray &source, bool byteLimit) {
		const auto node = WorkloadNode(0, 0, false, false);
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
			return span.Name == "imagegraph.source.cross_section_admission";
		}));
		CHECK(std::none_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.cross_section";
		}));
	}
}

TEST_CASE(
	"Cross Section workloads preserve source slices masks and bounded processor admission",
	"[imagegraph][cross_section_workloads]"
) {
	for (size_t formatBase : {size_t{0}, size_t{3}, size_t{6}}) {
		ImageArray source;
		for (size_t row = 0; row < OUTPUT_SIZES.size(); ++row) {
			source.Items.push_back({row});
			source.Images.push_back(Source(
				FORMATS[(formatBase + row) % FORMATS.size()],
				uint32_t(OUTPUT_SIZES[row].X),
				uint32_t(OUTPUT_SIZES[row].Y)
			));
		}
		for (int64_t axis : {int64_t{0}, int64_t{1}})
			for (int64_t mode : {int64_t{0}, int64_t{1}})
				for (bool antiAliasing : {false, true}) {
					const bool toAlpha = axis != mode;
					const auto node = WorkloadNode(axis, mode, antiAliasing, toAlpha);
					Image mask{1, 1, {128, 128, 128, 128}, 0};
					const auto maskKind = (axis + mode + int64_t(antiAliasing)) % 3;
					if (maskKind == 2) mask.Pixels = {255, 0, 0, 64};
					const Image *selectedMask = maskKind == 0 ? nullptr : &mask;
					INFO(
						"formats " << formatBase << " axis " << axis << " mode " << mode << " AA "
								   << antiAliasing << " mask " << maskKind
					);
					const auto first = Run(node, source, selectedMask);
					const auto firstHash =
						Verify(first, source, axis, mode, antiAliasing, toAlpha, selectedMask);
					const auto second = Run(node, source, selectedMask);
					CHECK(
						Verify(second, source, axis, mode, antiAliasing, toAlpha, selectedMask) == firstHash
					);
					CHECK(second.Images == first.Images);
					CHECK(second.Items == first.Items);
				}
		CheckAdmission(source, true);
		source.Images.back() = Image{1024, 1024, std::vector<uint8_t>(size_t{1024} * 1024 * 4, 128), 0};
		CheckAdmission(source, false);
	}
}
