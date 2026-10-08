#include "SourceMarkovGradient.hpp"

#include "../SourceMappedInputs.hpp"
#include "Processor.hpp"
#include "SourceInterpret.hpp"

#include <engine/imagegraph/FrameTime.hpp>

#include <array>
#include <cmath>
#include <limits>

// Source algorithms: Copyright (c) 2023 Tanasart, MIT License.
// See docs/pixel-composer-m0/PixelComposer-LICENSE.txt for the source license.
// Native nearest CPU profile of pinned node_markov_gradient and sh_markov_gradient. Default
// normal blend entry state and white draw color are assumed, without matched GPU parity claims.

namespace engine::imagegraph::detail {
	bool ReadSourceMarkovActive(NodeContext &c, bool &active) {
		const auto *value = c.Find("active");
		if (!value) {
			active = true;
			return true;
		}
		if (const auto *flag = std::get_if<bool>(value)) {
			active = *flag;
			return true;
		}
		if (const auto *number = std::get_if<double>(value); number && !std::isfinite(*number))
			return c.Fail(Status::InvalidValue, "Markov Gradient Active must be finite", "active");
		const auto number = SourceChoiceNumber(*value);
		if (!number)
			return c.Fail(
				Status::UnsupportedExecution,
				"Markov Gradient Active requires a scalar boolean or number",
				"active"
			);
		// The pinned HTML5 bool conversion compares the original numeric value with 0.5.
		active = *number > .5;
		return true;
	}

	namespace {
		using MarkovPixel = std::array<float, 4>;
		constexpr uint64_t MARKOV_WORK_LIMIT = 64000000;
		// The native policy charges preflight, drawing's repeated quote and final pixel writes.
		// Base sampling/hash costs 96 units, each possible palette comparison 24, and a map tap 32.
		constexpr uint64_t MARKOV_PIXEL_WORK = 96, MARKOV_COMPARE_WORK = 24, MARKOV_MAP_WORK = 32;
		constexpr uint64_t MARKOV_INACTIVE_PIXEL_WORK = 16;
		struct MarkovInputs {
			const Image *Source = nullptr, *Map = nullptr;
			bool Active = true;
			float SeedOffset = 0, Threshold = .1f, ChanceLow = 1, ChanceHigh = 1;
			std::array<MarkovPixel, 256> Palette{};
			size_t PaletteAmount = 0;
		};

		bool MarkovFloat(NodeContext &c, double value, float &out, std::string_view port) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return c.Fail(
					Status::InvalidValue, "Markov Gradient value exceeds finite shader range", port
				);
			out = float(value);
			return true;
		}
		bool MarkovNumber(NodeContext &c, std::string_view port, double fallback, double &out) {
			const auto *value = c.Find(port);
			if (!value) {
				out = fallback;
				return true;
			}
			if (const auto *flag = std::get_if<bool>(value)) {
				out = *flag ? 1 : 0;
				return true;
			}
			if (const auto *number = std::get_if<double>(value)) {
				out = *number;
				return std::isfinite(out) ||
					   c.Fail(Status::InvalidValue, "Markov Gradient numeric control must be finite", port);
			}
			const auto number = SourceChoiceNumber(*value);
			if (!number)
				return c.Fail(
					Status::UnsupportedExecution, "Markov Gradient requires a numeric scalar", port
				);
			out = *number;
			return true;
		}
		bool MarkovSurface(NodeContext &c, std::string_view port, bool required, const Image *&out) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Markov Gradient requires a physical surface binding", port
				);
			out = c.Input(port);
			if (!out)
				return !required || c.Fail(
										Status::UnsupportedExecution,
										"Markov Gradient missing source retains uncaptured output history",
										port
									);
			return ValidSurfaceLayout(*out, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Markov Gradient surface layout is invalid", port);
		}

		template <class Variant>
		bool AppendMarkovColour(NodeContext &c, MarkovInputs &in, const Variant &value) {
			if (const auto *number = std::get_if<double>(&value); number && !std::isfinite(*number))
				return c.Fail(Status::InvalidValue, "Markov Gradient palette color must be finite", "colors");
			const auto colour = std::visit(
				[](const auto &raw) {
					auto result = InterpretPackedColour(raw);
					// colToVec4 reads packed alpha only for source int64 storage. Real packed RGB
					// values, booleans and the native numeric enum profile retain opaque alpha.
					using Raw = std::decay_t<decltype(raw)>;
					if constexpr (!std::is_same_v<Raw, Colour> && !std::is_same_v<Raw, int64_t>)
						if (result) result->Alpha = 255;
					return result;
				},
				value
			);
			if (!colour)
				return c.Fail(
					Status::UnsupportedExecution,
					"Markov Gradient palette requires bounded packed colors",
					"colors"
				);
			if (in.PaletteAmount == in.Palette.size())
				return c.Fail(Status::LimitExceeded, "Markov Gradient palette exceeds 256 colors", "colors");
			in.Palette[in.PaletteAmount++] = {
				float(colour->Red) / 255.f,
				float(colour->Green) / 255.f,
				float(colour->Blue) / 255.f,
				float(colour->Alpha) / 255.f
			};
			return true;
		}
		bool MarkovPalette(NodeContext &c, MarkovInputs &in) {
			if (!c.IsLinked("colors") && c.IsCatalogueDefault("colors").value_or(false)) {
				if (c.Project.Palette.empty())
					return c.Fail(
						Status::UnsupportedExecution,
						"Markov Gradient empty project palette retains uncaptured shader uniforms",
						"colors"
					);
				if (c.Project.Palette.size() > in.Palette.size())
					return c.Fail(
						Status::LimitExceeded, "Markov Gradient palette exceeds 256 colors", "colors"
					);
				// Source construction copies project colors once. An omitted native input uses the
				// current durable project palette; historical constructor state is not inferred.
				for (const auto &colour : c.Project.Palette)
					if (!AppendMarkovColour(c, in, ElementValue{colour})) return false;
				return true;
			}
			const auto *value = c.Find("colors");
			if (!value)
				return c.Fail(
					Status::UnsupportedExecution,
					"Markov Gradient requires its captured constructor palette",
					"colors"
				);
			const auto *array = std::get_if<ArrayValue>(value);
			if (!array) return AppendMarkovColour(c, in, *value);
			if (!array->Nested.empty() || (!array->Items.empty() && !array->Elements.empty()))
				return c.Fail(
					Status::UnsupportedExecution,
					"Markov Gradient requires a processor-selected flat palette",
					"colors"
				);
			const size_t count = array->Items.empty() ? array->Elements.size() : array->Items.size();
			if (!count)
				return c.Fail(
					Status::UnsupportedExecution,
					"Markov Gradient empty palette retains uncaptured shader uniforms",
					"colors"
				);
			if (count > in.Palette.size())
				return c.Fail(Status::LimitExceeded, "Markov Gradient palette exceeds 256 colors", "colors");
			if (array->Items.empty()) {
				for (const auto &element : array->Elements)
					if (!AppendMarkovColour(c, in, element)) return false;
			} else {
				for (const auto &item : array->Items) {
					const auto *element = std::get_if<ElementValue>(&item.Data);
					if (!element)
						return c.Fail(
							Status::UnsupportedExecution,
							"Markov Gradient requires a processor-selected flat palette",
							"colors"
						);
					if (!AppendMarkovColour(c, in, *element)) return false;
				}
			}
			return true;
		}

		bool PrepareMarkov(NodeContext &c, MarkovInputs &in) {
			if (!ReadSourceMarkovActive(c, in.Active) || !MarkovSurface(c, "surface_in", true, in.Source))
				return false;
			if (c.Request.MaximumImageDimension == 0 ||
				c.Request.MaximumImageDimension > Limits::MaximumDimension)
				return c.Fail(
					Status::InvalidValue, "Markov Gradient request dimension limit is invalid", "surface_out"
				);
			if (in.Source->Width > c.Request.MaximumImageDimension ||
				in.Source->Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Markov Gradient dimensions exceed request limits", "surface_in"
				);
			if (!in.Active) return true;
			if (!c.Find("seed"))
				return c.Fail(
					Status::UnsupportedExecution, "Markov Gradient requires its observed source seed", "seed"
				);
			double rawSeed, threshold;
			if (!MarkovNumber(c, "seed", 0, rawSeed) || !MarkovNumber(c, "threshold", .1, threshold))
				return false;
			const double frame = double(FrameTimeToReal(GetFrameTime(c.Request)));
			float seed;
			if (!MarkovFloat(c, rawSeed + frame, seed, "seed") ||
				!MarkovFloat(c, threshold, in.Threshold, "threshold"))
				return false;
			const float reduced = seed - std::floor(seed / 100000.f) * 100000.f;
			in.SeedOffset = reduced / 10.f;
			if (!std::isfinite(reduced) || !std::isfinite(in.SeedOffset))
				return c.Fail(Status::InvalidValue, "Markov Gradient seed modulo is nonfinite", "seed");
			if (!MarkovPalette(c, in)) return false;
			if (const auto *toggle = c.Find("replace_chance_mapped"); toggle) {
				if (const auto *number = std::get_if<double>(toggle); number && !std::isfinite(*number))
					return c.Fail(
						Status::InvalidValue,
						"Markov Gradient map toggle must be finite",
						"replace_chance_mapped"
					);
				if (!std::holds_alternative<bool>(*toggle) && !SourceChoiceNumber(*toggle))
					return c.Fail(
						Status::UnsupportedExecution,
						"Markov Gradient map toggle requires a scalar boolean",
						"replace_chance_mapped"
					);
			}
			if (SourceRangeMapped(c, "replace_chance")) {
				Vector2 range;
				if (!ReadSourceMappedRange(c, "replace_chance", range) ||
					!MarkovFloat(c, range.X, in.ChanceLow, "replace_chance") ||
					!MarkovFloat(c, range.Y, in.ChanceHigh, "replace_chance") ||
					!MarkovSurface(c, "replace_chance_map", false, in.Map))
					return false;
			} else {
				double chance;
				if (!MarkovNumber(c, "replace_chance", 1, chance) ||
					!MarkovFloat(c, chance, in.ChanceLow, "replace_chance"))
					return false;
				in.ChanceHigh = in.ChanceLow;
			}
			return c.FailureCode == Status::Ok;
		}

		bool MarkovSample(
			NodeContext &c, const Image &image, float u, float v, std::string_view port, MarkovPixel &pixel
		) {
			const auto sampled = SampleNearest(image, u, v);
			for (size_t channel = 0; channel < 4; ++channel)
				if (!MarkovFloat(c, sampled[channel], pixel[channel], port)) return false;
			return true;
		}
		bool MarkovPixelAt(NodeContext &c, const MarkovInputs &in, uint32_t x, uint32_t y, Rgba &out) {
			const float u = (float(x) + .5f) / float(in.Source->Width),
						v = (float(y) + .5f) / float(in.Source->Height);
			MarkovPixel pixel;
			if (!MarkovSample(c, *in.Source, u, v, "surface_in", pixel)) return false;
			float chance = in.ChanceLow;
			if (in.Map) {
				MarkovPixel map;
				if (!MarkovSample(c, *in.Map, u, v, "replace_chance_map", map)) return false;
				const float mean = (map[0] + map[1] + map[2]) / 3.f;
				chance = in.ChanceLow * (1.f - mean) + in.ChanceHigh * mean;
				if (!std::isfinite(mean) || !std::isfinite(chance))
					return c.Fail(
						Status::InvalidValue, "Markov Gradient mapped chance is nonfinite", "replace_chance"
					);
			}
			const float phase = (u + in.SeedOffset) * 853.98598f + (v + in.SeedOffset) * 78.2345543f;
			if (!std::isfinite(phase))
				return c.Fail(Status::InvalidValue, "Markov Gradient random phase is nonfinite", "seed");
			const float wave = std::sin(phase) * 47.687523f, random = wave - std::floor(wave);
			if (!std::isfinite(random))
				return c.Fail(Status::InvalidValue, "Markov Gradient random value is nonfinite", "seed");
			if (random <= chance)
				for (size_t i = 0; i + 1 < in.PaletteAmount; ++i) {
					const float dx = pixel[0] - in.Palette[i][0], dy = pixel[1] - in.Palette[i][1],
								dz = pixel[2] - in.Palette[i][2];
					const float squared = dx * dx + dy * dy + dz * dz, distance = std::sqrt(squared);
					if (!std::isfinite(squared) || !std::isfinite(distance))
						return c.Fail(
							Status::InvalidValue,
							"Markov Gradient palette distance is nonfinite",
							"surface_in"
						);
					if (distance <= in.Threshold) {
						pixel = in.Palette[i + 1];
						break;
					}
				}
			for (size_t channel = 0; channel < 4; ++channel)
				out[channel] = pixel[channel];
			return true;
		}

		bool QuoteMarkov(NodeContext &c, const MarkovInputs &in, uint64_t &work, uint64_t &bytes) {
			const uint64_t pixels = uint64_t(in.Source->Width) * in.Source->Height;
			const uint64_t pixelWork = in.Active ? MARKOV_PIXEL_WORK +
													   (in.PaletteAmount - 1) * MARKOV_COMPARE_WORK +
													   (in.Map ? MARKOV_MAP_WORK : 0)
												 : MARKOV_INACTIVE_PIXEL_WORK;
			if (work > MARKOV_WORK_LIMIT || pixels > (MARKOV_WORK_LIMIT - work) / pixelWork)
				return c.Fail(
					Status::LimitExceeded, "Markov Gradient complete batch exceeds work limits", "surface_out"
				);
			const auto format = in.Active ? SurfaceFormat::RGBA8Unorm : in.Source->Format;
			const auto layout =
				CheckedSurfaceLayout(in.Source->Width, in.Source->Height, format, c.AvailableBytes());
			if (!layout || bytes > c.AvailableBytes() || layout->Bytes > c.AvailableBytes() - bytes)
				return c.Fail(
					Status::LimitExceeded,
					"Markov Gradient complete batch exceeds live output byte limits",
					"surface_out"
				);
			if (in.Active)
				for (uint32_t y = 0; y < in.Source->Height; ++y)
					for (uint32_t x = 0; x < in.Source->Width; ++x) {
						Rgba pixel;
						if (!MarkovPixelAt(c, in, x, y, pixel)) return false;
					}
			work += pixels * pixelWork;
			bytes += layout->Bytes;
			return true;
		}
	}

	bool AdmitSourceMarkovGradient(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes) {
		ENGINE_PROFILE("imagegraph.source.markov_gradient_admission");
		MarkovInputs inputs;
		return PrepareMarkov(context, inputs) && QuoteMarkov(context, inputs, batchWork, batchBytes);
	}
	bool DrawSourceMarkovGradient(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.markov_gradient");
		MarkovInputs inputs;
		uint64_t work = 0, bytes = 0;
		if (!PrepareMarkov(context, inputs) || !QuoteMarkov(context, inputs, work, bytes)) return false;
		auto *out = context.NewImage(
			"surface_out",
			inputs.Source->Width,
			inputs.Source->Height,
			inputs.Active ? SurfaceFormat::RGBA8Unorm : inputs.Source->Format
		);
		if (!out) return false;
		if (!inputs.Active) {
			out->Pixels = inputs.Source->Pixels;
			out->Hash = inputs.Source->Hash;
			return true;
		}
		for (uint32_t y = 0; y < inputs.Source->Height; ++y)
			for (uint32_t x = 0; x < inputs.Source->Width; ++x) {
				Rgba pixel;
				if (!MarkovPixelAt(context, inputs, x, y, pixel) || !WritePixel(*out, x, y, pixel))
					return context.FailureCode != Status::Ok ? false
															 : context.Fail(
																   Status::InvalidValue,
																   "Markov Gradient output sample is invalid",
																   "surface_out"
															   );
			}
		return true;
	}
}
