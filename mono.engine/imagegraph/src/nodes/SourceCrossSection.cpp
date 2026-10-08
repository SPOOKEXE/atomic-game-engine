#include "SourceCrossSection.hpp"

#include "Sampler.hpp"

#include <cmath>
#include <limits>

// Source algorithms: Copyright (c) 2023 Tanasart, MIT License.
// See docs/pixel-composer-m0/PixelComposer-LICENSE.txt for the source license.
// Native CPU profile of pinned node_cross_section, sh_cross_section, sh_draw_r* and sh_mask_empty.
// Texture-stage arithmetic and single-channel swizzles remain explicit native profile choices.
// Normal blend entry state and white draw color are assumed, without matched GPU parity claims.

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t CROSS_SECTION_WORK_LIMIT = 64000000;
		// The native 64M batch policy charges validation, drawing's repeated quote and pixel writes.
		// Each pixel includes up to four AA texels; masking adds a raw nearest tap and a second quantize.
		constexpr uint64_t CROSS_SECTION_PIXEL_WORK = 96, CROSS_SECTION_MASKED_PIXEL_WORK = 160;
		struct CrossSectionInputs {
			const Image *Source = nullptr, *Mask = nullptr;
			float Position = 0, LevelLow = 0, LevelHigh = 1;
			int64_t Axis = 0, Mode = 0;
			bool AntiAliasing = false, ToAlpha = false, Grayscale = false;
		};

		bool CrossFloat(NodeContext &c, double value, float &out, std::string_view port) {
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				return c.Fail(Status::InvalidValue, "Cross Section value exceeds finite shader range", port);
			out = float(value);
			return true;
		}

		bool CrossScalar(NodeContext &c, std::string_view port, double fallback, float &out) {
			const auto *value = c.Find(port);
			if (!value) return CrossFloat(c, fallback, out, port);
			if (const auto *number = std::get_if<double>(value)) return CrossFloat(c, *number, out, port);
			if (const auto *flag = std::get_if<bool>(value)) return CrossFloat(c, *flag ? 1 : 0, out, port);
			const auto number = SourceChoiceNumber(*value);
			if (!number)
				return c.Fail(Status::UnsupportedExecution, "Cross Section requires a numeric control", port);
			return CrossFloat(c, *number, out, port);
		}

		bool CrossBoolean(NodeContext &c, std::string_view port, bool &out) {
			const auto *value = c.Find(port);
			if (!value) {
				out = false;
				return true;
			}
			if (const auto *flag = std::get_if<bool>(value)) {
				out = *flag;
				return true;
			}
			const auto number = SourceChoiceNumber(*value);
			if (!number)
				return c.Fail(
					Status::UnsupportedExecution, "Cross Section requires a finite boolean control", port
				);
			out = *number != 0;
			return true;
		}

		bool CrossChoice(NodeContext &c, std::string_view port, int64_t &out) {
			// Source widget clamping applies to scalar values; original array getters bypass it.
			const double value = c.SourceChoice(port, 0);
			if (c.FailureCode != Status::Ok) return false;
			if (!std::isfinite(value) || value != std::trunc(value) || value < 0 || value > 1)
				return c.Fail(
					Status::UnsupportedExecution,
					"Cross Section choice is outside the integral native profile",
					port
				);
			out = static_cast<int64_t>(value);
			return true;
		}

		bool CrossSurface(NodeContext &c, std::string_view port, bool required, const Image *&out) {
			if (const auto *value = c.Find(port); value && std::holds_alternative<AtlasValue>(*value))
				return c.Fail(
					Status::UnsupportedExecution, "Cross Section requires physical surface binding", port
				);
			out = c.Input(port);
			if (!out)
				return !required ||
					   c.Fail(Status::InvalidValue, "Cross Section source surface is missing", port);
			return ValidSurfaceLayout(*out, Limits::MaximumDimension, Limits::MaximumEvaluationBytes) ||
				   c.Fail(Status::InvalidValue, "Cross Section surface layout is invalid", port);
		}

		bool PrepareCrossSection(NodeContext &c, CrossSectionInputs &in) {
			if (!CrossSurface(c, "surface_in", true, in.Source) || !CrossSurface(c, "mask", false, in.Mask))
				return false;
			if (c.Request.MaximumImageDimension == 0 ||
				c.Request.MaximumImageDimension > Limits::MaximumDimension)
				return c.Fail(
					Status::InvalidValue, "Cross Section request dimension limit is invalid", "surface_out"
				);
			if (in.Source->Width > c.Request.MaximumImageDimension ||
				in.Source->Height > c.Request.MaximumImageDimension)
				return c.Fail(
					Status::LimitExceeded, "Cross Section dimensions exceed request limits", "surface_in"
				);
			if (!CrossChoice(c, "axis", in.Axis) || !CrossChoice(c, "mode", in.Mode) ||
				!CrossScalar(c, "position", 0, in.Position) ||
				!CrossBoolean(c, "anti_aliasing", in.AntiAliasing) ||
				!CrossBoolean(c, "to_alpha", in.ToAlpha))
				return false;
			const auto *level = c.Find("level");
			if (level && !std::holds_alternative<Vector2>(*level) &&
				!std::holds_alternative<double>(*level) && !std::holds_alternative<int64_t>(*level))
				return c.Fail(
					Status::UnsupportedExecution,
					"Cross Section Level requires a numeric pair or scalar",
					"level"
				);
			const auto levels = c.Vec2("level", {0, 1});
			if (!CrossFloat(c, levels.X, in.LevelLow, "level") ||
				!CrossFloat(c, levels.Y, in.LevelHigh, "level"))
				return false;
			in.Grayscale = DescribeSurfaceFormat(in.Source->Format)->Channels == 1;
			return c.FailureCode == Status::Ok;
		}

		using CrossPixel = std::array<float, 4>;
		bool CrossSample(
			NodeContext &c,
			const Image &image,
			float u,
			float v,
			bool filtered,
			std::string_view port,
			CrossPixel &out
		) {
			if (!std::isfinite(u) || !std::isfinite(v))
				return c.Fail(Status::InvalidValue, "Cross Section sample coordinate is nonfinite", port);
			const auto sampled = Texture(image, u, v, filtered);
			for (size_t channel = 0; channel < 4; ++channel)
				if (!CrossFloat(c, sampled[channel], out[channel], port)) return false;
			return true;
		}

		bool
		CrossSectionPixel(NodeContext &c, const CrossSectionInputs &in, uint32_t x, uint32_t y, Rgba &out) {
			const float u = (float(x) + .5f) / float(in.Source->Width),
						v = (float(y) + .5f) / float(in.Source->Height);
			CrossPixel pixel;
			if (in.Grayscale) {
				// draw_surface_safe temporarily substitutes sh_draw_r* at the original full-surface UV.
				if (!CrossSample(c, *in.Source, u, v, in.AntiAliasing, "surface_in", pixel)) return false;
				pixel = {pixel[0], pixel[0], pixel[0], 1.f};
			} else {
				if (!CrossSample(
						c,
						*in.Source,
						in.Axis == 0 ? u : in.Position,
						in.Axis == 0 ? in.Position : v,
						in.AntiAliasing,
						"surface_in",
						pixel
					))
					return false;
				const float luma = pixel[0] * .2126f + pixel[1] * .7152f + pixel[2] * .0722f;
				const float brightness = 1.f - luma * pixel[3];
				const float threshold = in.LevelLow * (1.f - brightness) + in.LevelHigh * brightness;
				if (!std::isfinite(luma) || !std::isfinite(brightness) || !std::isfinite(threshold))
					return c.Fail(
						Status::InvalidValue, "Cross Section brightness or threshold is nonfinite", "level"
					);
				const float result = (in.Axis == 0 ? v : 1.f - u) < threshold ? 0.f : 1.f;
				if (in.Mode == 0) pixel = {result, result, result, 1.f};
				if (in.ToAlpha) pixel[3] = result;
			}
			// surface_verify omits attrDepth, so the section pass is already quantized RGBA8.
			for (size_t channel = 0; channel < 4; ++channel) {
				if (!std::isfinite(pixel[channel]))
					return c.Fail(
						Status::InvalidValue, "Cross Section output sample is nonfinite", "surface_out"
					);
				pixel[channel] = float(Quantize(pixel[channel])) / 255.f;
			}
			if (in.Mask) {
				CrossPixel mask;
				if (!CrossSample(c, *in.Mask, u, v, false, "mask", mask)) return false;
				// mask_apply_empty ignores mask_alpha_only and uses the raw bound channels.
				const float maskMean = (mask[0] + mask[1] + mask[2]) / 3.f;
				const float alpha = maskMean * mask[3];
				const float maskedAlpha = pixel[3] * alpha;
				if (!std::isfinite(maskMean) || !std::isfinite(alpha) || !std::isfinite(maskedAlpha))
					return c.Fail(Status::InvalidValue, "Cross Section mask alpha is nonfinite", "mask");
				pixel[3] = float(Quantize(maskedAlpha)) / 255.f;
			}
			for (size_t channel = 0; channel < 4; ++channel)
				out[channel] = pixel[channel];
			return true;
		}

		bool
		QuoteCrossSection(NodeContext &c, const CrossSectionInputs &in, uint64_t &work, uint64_t &bytes) {
			const uint64_t pixels = uint64_t(in.Source->Width) * in.Source->Height;
			const uint64_t pixelWork = in.Mask ? CROSS_SECTION_MASKED_PIXEL_WORK : CROSS_SECTION_PIXEL_WORK;
			if (work > CROSS_SECTION_WORK_LIMIT || pixels > (CROSS_SECTION_WORK_LIMIT - work) / pixelWork)
				return c.Fail(
					Status::LimitExceeded, "Cross Section complete batch exceeds work limits", "surface_out"
				);
			const uint64_t outputBytes = pixels * 4;
			if (bytes > c.AvailableBytes() || outputBytes > c.AvailableBytes() - bytes)
				return c.Fail(
					Status::LimitExceeded,
					"Cross Section complete batch exceeds live output byte limits",
					"surface_out"
				);
			for (uint32_t y = 0; y < in.Source->Height; ++y)
				for (uint32_t x = 0; x < in.Source->Width; ++x) {
					Rgba pixel;
					if (!CrossSectionPixel(c, in, x, y, pixel)) return false;
				}
			work += pixels * pixelWork;
			bytes += outputBytes;
			return true;
		}
	}

	bool AdmitSourceCrossSection(NodeContext &context, uint64_t &batchWork, uint64_t &batchBytes) {
		ENGINE_PROFILE("imagegraph.source.cross_section_admission");
		CrossSectionInputs inputs;
		return PrepareCrossSection(context, inputs) &&
			   QuoteCrossSection(context, inputs, batchWork, batchBytes);
	}

	bool DrawSourceCrossSection(NodeContext &context) {
		ENGINE_PROFILE("imagegraph.source.cross_section");
		CrossSectionInputs inputs;
		uint64_t work = 0, bytes = 0;
		if (!PrepareCrossSection(context, inputs) || !QuoteCrossSection(context, inputs, work, bytes))
			return false;
		auto *out = context.NewImage(
			"surface_out", inputs.Source->Width, inputs.Source->Height, SurfaceFormat::RGBA8Unorm
		);
		if (!out) return false;
		for (uint32_t y = 0; y < inputs.Source->Height; ++y)
			for (uint32_t x = 0; x < inputs.Source->Width; ++x) {
				Rgba pixel;
				if (!CrossSectionPixel(context, inputs, x, y, pixel) || !WritePixel(*out, x, y, pixel))
					return context.FailureCode != Status::Ok ? false
															 : context.Fail(
																   Status::InvalidValue,
																   "Cross Section output sample is invalid",
																   "surface_out"
															   );
			}
		return true;
	}
}
