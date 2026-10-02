// Outline executor, following shaders/sh_outline and node_outline.gml processData.

#include "Families.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr double TAU = 2.0 * std::numbers::pi;

		struct OutlineSearch {
			bool Found = false;
			bool Collected = false;
			Rgba Closest{};
			double Distance = 99999.0;
		};

		struct OutlineControls {
			int64_t Side = 0;
			int64_t Profile = 0;
			bool Crop = false;
			double Threshold = 1.0;
			int64_t Oversample = 4;
			std::array<int64_t, 9> Filter{1, 1, 1, 1, 1, 1, 1, 1, 1};
		};

		// angleFiltered: maps the direction to one of nine anchor cells; a 0 cell skips it.
		bool AngleFiltered(double angle, int64_t side, const OutlineControls &controls) {
			double degrees = angle * 180.0 / std::numbers::pi + 360.0 + (side == 0 ? 180.0 : 0.0);
			degrees = degrees - 360.0 * std::floor(degrees / 360.0);
			size_t index = 0;
			if (degrees == 0.0) index = 3;
			else if (degrees == 90.0) index = 1;
			else if (degrees == 180.0) index = 5;
			else if (degrees == 270.0) index = 7;
			else if (degrees < 90.0) index = 0;
			else if (degrees < 180.0) index = 2;
			else if (degrees < 270.0) index = 8;
			else index = 6;
			return controls.Filter[index] == 0;
		}

		// checkPixel: p is a pixel-space sample position around the output pixel centre.
		void CheckPixel(
			const Image &source,
			const OutlineControls &controls,
			int64_t side,
			double pixelX,
			double pixelY,
			double sampleX,
			double sampleY,
			OutlineSearch &search
		) {
			const double u = sampleX / source.Width, v = sampleY / source.Height;
			if (side == 0 && controls.Crop && (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)) return;
			const Rgba sample = SampleTextureSimple(source, u, v, controls.Oversample, false);
			if (side == 0 && sample[3] > 1.0 - controls.Threshold) return;
			if (side == 1 && sample[3] < controls.Threshold) return;
			search.Found = true;
			const double centreX = std::floor(sampleX) + 0.5, centreY = std::floor(sampleY) + 0.5;
			double distance = 0.0;
			if (controls.Profile == 0)
				distance = std::hypot(pixelX - centreX, pixelY - centreY);
			else if (controls.Profile == 1)
				distance = std::max(std::abs(pixelX - centreX), std::abs(pixelY - centreY));
			else if (controls.Profile == 2)
				distance = std::abs(pixelX - centreX) + std::abs(pixelY - centreY);
			if (distance < search.Distance) {
				search.Distance = distance;
				search.Closest = sample;
			}
		}

		Rgba BlendOver(const Rgba &base, const Rgba &colour) {
			const double baseAlpha = base[3], alpha = colour[3];
			const double total = alpha + baseAlpha * (1.0 - alpha);
			Rgba result{};
			for (size_t channel = 0; channel < 4; channel++)
				result[channel] = (base[channel] * baseAlpha * (1.0 - alpha) + colour[channel] * alpha) / total;
			result[3] = total;
			return result;
		}

		Rgba MixColour(const Rgba &background, const Rgba &foreground, double amount, int64_t mode) {
			Rgba result{};
			for (size_t channel = 0; channel < 4; channel++) {
				const double bg = background[channel], fg = foreground[channel];
				double blended = fg;
				if (mode == 1) blended = bg * fg;
				else if (mode == 2) blended = 1.0 - (1.0 - bg) * (1.0 - fg);
				else if (mode == 3) blended = bg + fg;
				result[channel] = bg + (blended - bg) * amount;
			}
			return result;
		}

		double SmoothStep(double edge0, double edge1, double x) {
			const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}

		bool Outline(NodeContext &context) {
			bool failed = false;
			if (CopyWhenInactive(context, failed)) return !failed;
			const Image *source = context.Input("surface_in");
			if (!source) return context.Fail(Status::InvalidValue, "image input is missing", "surface_in");
			Image *out = context.NewImage("surface_out", source->Width, source->Height);
			Image *outline = out ? context.NewImage("outline", source->Width, source->Height) : nullptr;
			if (!outline) return false;
			OutlineControls controls;
			controls.Side = context.Integer("position", 1);
			controls.Profile = context.Integer("profile", 0);
			controls.Crop = context.Boolean("crop_border");
			controls.Threshold = context.Scalar("threshold", 1.0);
			controls.Oversample = ReadSampler(context).Oversample;
			if (const auto *filter = std::get_if<ArrayValue>(context.Find("attribute_filter"))) {
				for (size_t index = 0; index < std::min<size_t>(9, filter->Elements.size()); index++)
					if (const auto *cell = std::get_if<double>(&filter->Elements[index]))
						controls.Filter[index] = static_cast<int64_t>(*cell);
			}
			const Colour border = context.Get<Colour>("color", Colour{255, 255, 255, 255});
			const Rgba borderColour{border.Red / 255.0, border.Green / 255.0, border.Blue / 255.0, border.Alpha / 255.0};
			const bool antialias = context.Boolean("anti_aliasing"), highRes = context.Boolean("high_res");
			const bool blendOriginal = context.Boolean("blend");
			const int64_t blendMode = context.Integer("blend_mode", 0);
			const Image *texture = context.Input("texture");
			for (uint32_t y = 0; y < source->Height; y++) {
				for (uint32_t x = 0; x < source->Width; x++) {
					const double u = (x + 0.5) / source->Width, v = (y + 0.5) / source->Height;
					double start = MappedScalar(context, "start", u, v);
					double size = MappedScalar(context, "width", u, v);
					if (!antialias) size = std::floor(size);
					const double blendAlpha = MappedScalar(context, "blend_alpha", u, v);
					const double pixelX = x + 0.5, pixelY = y + 0.5;
					Rgba base = ReadPixel(*source, x, y);
					Rgba result = base, resultOutline{};
					int64_t side = controls.Side;
					if (controls.Side == 2) {
						side = base[3] > 1.0 - controls.Threshold ? 0 : 1;
						const double bandStart = -std::floor(size / 2.0) + start;
						const double bandEnd = std::ceil(size / 2.0) + start;
						if (side == 0) {
							size = std::min(-bandStart, size);
							start = -bandStart - size;
						} else {
							size = std::min(bandEnd, size);
							start = bandEnd - size;
						}
					}
					const bool border = side == 0 ? base[3] > 1.0 - controls.Threshold : base[3] < controls.Threshold;
					OutlineSearch search;
					if (border && size + start > 0.0) {
						const double reach = start + size + (antialias ? 1.0 : 0.0);
						const double ring = (highRes ? 16.0 : 8.0) + (antialias ? 2.0 : 0.0);
						if (controls.Profile == 0) {
							for (double radius = 1.0; radius <= reach; radius++) {
								double denominator = 1.0, numerator = 0.0;
								const double steps = 4.0 + radius * ring;
								for (double step = 0.0; step <= steps; step++) {
									double angle = 0.0;
									if (highRes) {
										angle = numerator / denominator * TAU;
										numerator += 2.0;
										if (numerator >= denominator) {
											numerator = 1.0;
											denominator *= 2.0;
										}
									} else {
										angle = step / steps * TAU;
									}
									if (AngleFiltered(angle, side, controls)) continue;
									CheckPixel(
										*source, controls, side, pixelX, pixelY, pixelX + std::cos(angle) * radius,
										pixelY + std::sin(angle) * radius, search
									);
								}
							}
						} else if (controls.Profile == 1) {
							for (double row = -reach; row <= reach; row++)
								for (double column = -reach; column <= reach; column++) {
									if (row == 0.0 && column == 0.0) continue;
									CheckPixel(*source, controls, side, pixelX, pixelY, pixelX + column, pixelY + row, search);
								}
						} else if (controls.Profile == 2) {
							// Four passes per radius in shader order; the first equal-distance sample wins.
							constexpr std::array<std::array<double, 2>, 4> QUADRANTS = {{{1, 1}, {-1, -1}, {-1, 1}, {1, -1}}};
							for (double radius = 1.0; radius <= reach; radius++)
								for (const auto &[signX, signY] : QUADRANTS)
									for (double step = 0.0; step < reach && step < radius; step++)
										CheckPixel(
											*source, controls, side, pixelX, pixelY, pixelX + signX * step,
											pixelY + signY * (radius - step), search
										);
						}
					} else if (border) {
						search.Distance = 0.0;
						for (double step = 0.0; step < 4.0; step++) {
							const double angle = step * TAU / 4.0;
							if (AngleFiltered(angle, side, controls)) continue;
							const double su = (pixelX + std::cos(angle)) / source->Width;
							const double sv = (pixelY + std::sin(angle)) / source->Height;
							if (side == 0 && controls.Crop && (su < 0.0 || su > 1.0 || sv < 0.0 || sv > 1.0)) continue;
							const Rgba sample = SampleTextureSimple(*source, su, sv, controls.Oversample, false);
							if ((side == 0 && sample[3] < controls.Threshold) || (side == 1 && sample[3] >= controls.Threshold)) {
								search.Found = true;
								if (!search.Collected) {
									search.Collected = true;
									search.Closest = sample;
								}
								break;
							}
						}
					}
					double coverage = 0.0;
					if (search.Found) {
						if (antialias)
							coverage = std::min(
								SmoothStep(size + start + 0.5, size + start, search.Distance),
								SmoothStep(start - 0.5, start, search.Distance)
							);
						else
							coverage = std::min(
								-search.Distance >= -(size + start + 0.5) ? 1.0 : 0.0,
								search.Distance >= start - 0.5 ? 1.0 : 0.0
							);
					}
					if (coverage != 0.0) {
						if (!blendOriginal) {
							Rgba colour{borderColour[0], borderColour[1], borderColour[2], borderColour[3] * coverage};
							if (side == 0) {
								for (size_t channel = 0; channel < 3; channel++)
									base[channel] += (borderColour[channel] - base[channel]) * colour[3];
								colour[3] *= base[3];
								result = base;
								resultOutline = colour;
							} else {
								result = BlendOver(colour, base);
								resultOutline = colour;
							}
						} else {
							Rgba blended = MixColour(side == 0 ? base : search.Closest, borderColour, blendAlpha, blendMode);
							blended[3] = side == 0 ? base[3] : coverage;
							result = blended;
							resultOutline = blended;
						}
						if (texture) {
							const Rgba sample = SampleNearest(*texture, u, v);
							for (size_t channel = 0; channel < 4; channel++) {
								result[channel] *= sample[channel];
								resultOutline[channel] *= sample[channel];
							}
						}
					}
					WritePixel(*out, x, y, result);
					WritePixel(*outline, x, y, resultOutline);
				}
			}
			// The source applies Mask and Mix to both outputs and has no Channel input.
			const Image *mask = context.Input("mask");
			Image feathered;
			if (mask && context.Scalar("mask_feather") > 0.0) {
				feathered = FeatherMask(*mask, context.Scalar("mask_feather"));
				mask = &feathered;
			}
			const double mix = context.Scalar("mix", 1.0);
			const bool invert = context.Boolean("invert_mask"), alphaOnly = context.Boolean("mask_alpha_only");
			ApplyMaskMix(*source, *out, mask, mix, invert, alphaOnly);
			ApplyMaskMix(*source, *outline, mask, mix, invert, alphaOnly);
			return true;
		}

		constexpr ExecutorEntry OUTLINE_EXECUTORS[] = {
			{"pc.outline", Outline},
		};
	}

	std::span<const ExecutorEntry> OutlineExecutors() {
		return OUTLINE_EXECUTORS;
	}
}
