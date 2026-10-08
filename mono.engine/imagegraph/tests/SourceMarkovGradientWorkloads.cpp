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
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.source_markov_gradient_workloads")
using namespace engine::imagegraph;
namespace {
	constexpr std::array<Vector2, 3> OUTPUT_SIZES{{{32, 24}, {48, 32}, {64, 48}}};
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
		constexpr std::array<SurfacePixel, 4> labels{
			{{0, 0, 0, .75}, {1, 0, 0, .5}, {0, 0, 1, .25}, {0, 1, 0, 1}}
		};
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x)
				REQUIRE(StoreSurfacePixel(
					image, x, y, labels[(x / (width / 4) + y / (height / 2)) % labels.size()]
				));
		return image;
	}

	ArrayValue Palette(size_t count) {
		ArrayValue palette{ValueType::Colour, {}};
		palette.Elements.emplace_back(Colour{0, 0, 0, 64});
		if (count > 1) palette.Elements.emplace_back(Colour{255, 0, 0, 128});
		if (count > 2) {
			palette.Elements.emplace_back(Colour{0, 0, 255, 192});
			while (palette.Elements.size() < count - 1)
				palette.Elements.emplace_back(Colour{255, 255, 255, 32});
			palette.Elements.emplace_back(Colour{0, 255, 0, 96});
		}
		return palette;
	}

	Node WorkloadNode(ArrayValue palette, double chance, bool mapped = false) {
		Node node{
			"markov",
			"pc.markov_gradient",
			"",
			{},
			{{"active", true},
			 {"seed", 23.5},
			 {"colors", std::move(palette)},
			 {"threshold", .0001},
			 {"replace_chance_mapped", mapped}}
		};
		if (mapped)
			node.Values.push_back({"replace_chance_map_range", Vector2{0, 3}});
		else
			node.Values.push_back({"replace_chance", chance});
		return node;
	}

	void Inputs(detail::NodeContext &context, const ImageArray &source, const Image *map = nullptr) {
		context.InheritedSurfaceFormat = SurfaceFormat::RGBA32Float;
		context.InheritedInterpolation = 1;
		context.ImageArrays.emplace_back("surface_in", &source);
		if (map) context.Images.emplace_back("replace_chance_map", map);
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

	ImageArray Run(const Node &node, const ImageArray &source, const Image *map = nullptr) {
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		request.Tick = 7;
		request.NegativeFrame = true;
		request.Subframe = .375;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source, map);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const auto bytesBefore = Counter("imagegraph.image.allocated_payload_bytes");
		ProfileFrame frame;
		{
			ENGINE_PROFILE("imagegraph.markov_gradient.workload");
			const bool processed = detail::RunProcessorBatch(context, executor);
			INFO(context.FailureMessage);
			REQUIRE(processed);
		}
		frame.Finish();
		using engine::core::FrameGraph;
		CHECK(FrameGraph::Dropped() == 0);
		const auto spans = FrameGraph::Spans();
		for (const auto name :
			 {"imagegraph.markov_gradient.workload",
			  "imagegraph.source.markov_gradient",
			  "imagegraph.source.markov_gradient_admission",
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
				count == (std::string_view(name) == "imagegraph.markov_gradient.workload"
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

	uint8_t Byte(double value) {
		return uint8_t(std::lround(std::clamp(float(value), 0.f, 1.f) * 255));
	}
	std::array<uint8_t, 4> Pixel(const SurfacePixel &color) {
		std::array<uint8_t, 4> result;
		for (size_t channel = 0; channel < result.size(); ++channel)
			result[channel] = Byte(color[channel]);
		return result;
	}

	std::array<uint8_t, 4> Next(const SurfacePixel &original, const std::vector<ElementValue> &palette) {
		for (size_t index = 0; index + 1 < palette.size(); ++index) {
			const auto &color = std::get<Colour>(palette[index]);
			const double red = original[0] - double(color.Red) / 255;
			const double green = original[1] - double(color.Green) / 255;
			const double blue = original[2] - double(color.Blue) / 255;
			if (red * red + green * green + blue * blue > .0001 * .0001) continue;
			const auto &next = std::get<Colour>(palette[index + 1]);
			return {next.Red, next.Green, next.Blue, next.Alpha};
		}
		return Pixel(original);
	}

	// This oracle pins the native float shader profile, not cross-GPU sin hash parity.
	float Random(float u, float v) {
		constexpr float seed = float(23.5 - 7.375);
		const float reduced = seed - std::floor(seed / 100000.f) * 100000.f;
		const float offset = reduced / 10.f;
		const float phase = (u + offset) * 853.98598f + (v + offset) * 78.2345543f;
		const float wave = std::sin(phase) * 47.687523f;
		return wave - std::floor(wave);
	}

	float Probability(const Image &map, float u, float v) {
		const auto x = uint32_t(std::clamp(std::floor(double(u) * map.Width), 0., double(map.Width - 1)));
		const auto y = uint32_t(std::clamp(std::floor(double(v) * map.Height), 0., double(map.Height - 1)));
		SurfacePixel pixel;
		REQUIRE(LoadSurfacePixel(map, x, y, pixel));
		const float mean = (float(pixel[0]) + float(pixel[1]) + float(pixel[2])) / 3.f;
		return 3.f * mean;
	}

	uint64_t Verify(
		const ImageArray &output,
		const ImageArray &source,
		const ArrayValue &palette,
		double chance,
		const Image *map = nullptr
	) {
		REQUIRE(output.Images.size() == OUTPUT_SIZES.size());
		REQUIRE(output.Items.size() == OUTPUT_SIZES.size());
		uint64_t hash = 14695981039346656037ull, changed = 0, retained = 0, storedBytes = 0;
		for (size_t row = 0; row < output.Images.size(); ++row) {
			const auto *imageIndex = std::get_if<size_t>(&output.Items[row].Data);
			REQUIRE(imageIndex);
			CHECK(*imageIndex == row);
			const auto &image = output.Images[row];
			CHECK(image.Width == uint32_t(OUTPUT_SIZES[row].X));
			CHECK(image.Height == uint32_t(OUTPUT_SIZES[row].Y));
			CHECK(image.Format == SurfaceFormat::RGBA8Unorm);
			REQUIRE(image.Pixels.size() == size_t(image.Width) * image.Height * 4);
			storedBytes += image.Pixels.size();
			for (const auto byte : image.Pixels)
				hash = (hash ^ byte) * 1099511628211ull;
			const auto &colors = palette.Nested.empty() ? palette.Elements : palette.Nested[row];
			for (uint32_t y = 0; y < image.Height; ++y)
				for (uint32_t x = 0; x < image.Width; ++x) {
					SurfacePixel original;
					REQUIRE(LoadSurfacePixel(source.Images[row], x, y, original));
					const auto raw = Pixel(original), replacement = Next(original, colors);
					std::array<uint8_t, 4> actual;
					std::copy_n(image.Pixels.begin() + (size_t(y) * image.Width + x) * 4, 4, actual.begin());
					const float u = (float(x) + .5f) / float(image.Width);
					const float v = (float(y) + .5f) / float(image.Height);
					const float probability = map ? Probability(*map, u, v) : float(chance);
					CHECK(actual == (Random(u, v) <= probability ? replacement : raw));
					changed += actual != raw;
					retained += actual == raw;
				}
		}
		CHECK(storedBytes == uint64_t{4} * (32 * 24 + 48 * 32 + 64 * 48));
		if (chance == .5 && !map && palette.Elements.size() > 1) {
			CHECK(changed > 0);
			CHECK(retained > 0);
		}
		return hash;
	}

	void CheckAdmission(const ImageArray &source, bool byteLimit) {
		const auto node = WorkloadNode(Palette(256), 1);
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		request.Tick = 7;
		request.NegativeFrame = true;
		request.Subframe = .375;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = byteLimit ? 20 * 1024 : Limits::MaximumEvaluationBytes;
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
		const auto admissions = size_t(std::count_if(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.markov_gradient_admission";
		}));
		CHECK(admissions > 0);
		if (!byteLimit) CHECK(admissions == OUTPUT_SIZES.size());
		CHECK(std::none_of(spans.begin(), spans.end(), [](const auto &span) {
			return span.Name == "imagegraph.source.markov_gradient";
		}));
	}

	void CheckInactive(const ImageArray &source) {
		Node node{
			"markov",
			"pc.markov_gradient",
			"",
			{},
			{{"active", false},
			 {"seed", std::string{"unresolved"}},
			 {"colors", std::string{"unused"}},
			 {"threshold", std::string{"unused"}},
			 {"replace_chance", std::string{"unused"}}}
		};
		const auto *entry = FindCatalogueEntry(node.Type);
		const auto executor = detail::FindExecutor(node.Type);
		REQUIRE(entry);
		REQUIRE(executor);
		EvaluationRequest request;
		detail::NodeContext context{node, *entry, request};
		context.ByteBudget = Limits::MaximumEvaluationBytes;
		Inputs(context, source);
		const auto allocationsBefore = Counter("imagegraph.image.allocations");
		const bool processed = detail::RunProcessorBatch(context, executor);
		INFO(context.FailureMessage);
		REQUIRE(processed);
		REQUIRE(context.OutputImageArrays.size() == 1);
		CHECK(context.OutputImageArrays.front().second.Images == source.Images);
		CHECK(context.OutputImageArrays.front().second.Items == source.Items);
		CHECK(Counter("imagegraph.image.allocations") == allocationsBefore);
	}
}

TEST_CASE(
	"Markov Gradient workloads preserve ordered palettes raw formats and bounded processor admission",
	"[imagegraph][markov_gradient_workloads]"
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
		for (size_t count : {size_t{1}, size_t{2}, size_t{256}, size_t{0}}) {
			ArrayValue palette = Palette(count ? count : 1);
			if (!count) {
				palette.Elements.clear();
				for (size_t rowCount : {size_t{1}, size_t{2}, size_t{256}})
					palette.Nested.push_back(Palette(rowCount).Elements);
			}
			for (double chance : {-1., 0., .5, 1.}) {
				INFO("formats " << formatBase << " colors " << count << " chance " << chance);
				const auto node = WorkloadNode(palette, chance);
				const auto first = Run(node, source);
				const auto firstHash = Verify(first, source, palette, chance);
				const auto second = Run(node, source);
				CHECK(Verify(second, source, palette, chance) == firstHash);
				CHECK(second.Images == first.Images);
				CHECK(second.Items == first.Items);
			}
			Image rgbaMap{3, 2, std::vector<uint8_t>(3 * 2 * 4), 0};
			Image redMap{3, 2, std::vector<uint8_t>(3 * 2 * 4), 0, SurfaceFormat::R32Float};
			for (uint32_t y = 0; y < 2; ++y)
				for (uint32_t x = 0; x < 3; ++x) {
					const double red = (x + y) % 2 ? 1 : 0;
					REQUIRE(StoreSurfacePixel(rgbaMap, x, y, {red, 0, 0, 0}));
					REQUIRE(StoreSurfacePixel(redMap, x, y, {red, 0, 0, 1}));
				}
			const auto node = WorkloadNode(palette, 1, true);
			const auto mapped = Run(node, source, &rgbaMap);
			const auto mappedHash = Verify(mapped, source, palette, 1, &rgbaMap);
			const auto rawRedMapped = Run(node, source, &redMap);
			CHECK(Verify(rawRedMapped, source, palette, 1, &redMap) == mappedHash);
			CHECK(rawRedMapped.Images == mapped.Images);
			CHECK(rawRedMapped.Items == mapped.Items);
		}
		CheckInactive(source);
		CheckAdmission(source, true);
		source.Images.back() = Image{128, 128, std::vector<uint8_t>(size_t{128} * 128 * 4, 128), 0};
		CheckAdmission(source, false);
	}
}
