#include "../PixelBoxMath.hpp"
#include "Processor.hpp"

namespace engine::imagegraph::detail {
	namespace {
		Rgba PixelEffectColour(NodeContext &context, std::string_view port) {
			const Colour colour = context.Get<Colour>(port, Colour{255, 255, 255, 255});
			return {colour.Red / 255.0, colour.Green / 255.0, colour.Blue / 255.0, colour.Alpha / 255.0};
		}
		std::array<double, 4> EffectBox(NodeContext &context, std::string_view port) {
			if (const Value *value = context.Find(port)) {
				if (const auto *box = std::get_if<PixelBoxValue>(value))
					return PixelBoxBounds(box->Data ? *box->Data : PixelBoxData{});
				context.Fail(Status::InvalidValue, "Extrude requires a typed PBbox", port);
			}
			return PixelBoxBounds(PixelBoxData{});
		}
	}
	bool PixelExtrude(NodeContext &context) {
		const Image *surface = context.Input("surface");
		if (!surface) return context.Fail(Status::InvalidValue, "Extrude requires a surface", "surface");
		const bool useBox = context.Boolean("use_pbbox", true), clone = context.Boolean("clone_color");
		const int64_t mode = context.Integer("pbbox_mode");
		const auto from = EffectBox(context, "shape_pbbox"), to = EffectBox(context, "target_pbbox");
		const double angle = context.Scalar("angle") * std::acos(-1.0) / 180;
		const double highlightAngle = context.Scalar("highlight_direction") * std::acos(-1.0) / 180;
		std::array<Vector2, 4> directions{Vector2{1, 0}, Vector2{-1, 0}, Vector2{0, 1}, Vector2{0, -1}};
		std::array<double, 4> distances{from[0] - to[0], to[2] - from[2], from[1] - to[1], to[3] - from[3]};
		double finalDistance = 0;
		size_t searches = 4;
		if (!useBox) {
			directions[0] = {-std::cos(angle), std::sin(angle)};
			distances[0] = context.Integer("distance", 1);
			searches = 1;
		} else if (mode == 1) {
			const double dx = ((to[0] + to[2]) - (from[0] + from[2])) / 2;
			const double dy = ((to[1] + to[3]) - (from[1] + from[3])) / 2;
			finalDistance = std::hypot(dx, dy);
			directions[0] =
				finalDistance == 0 ? Vector2{} : Vector2{-dx / finalDistance, -dy / finalDistance};
			distances[0] = std::ceil(finalDistance);
			searches = 1;
		}
		uint64_t samples = 1;
		for (size_t search = 0; search < searches; ++search) {
			if (!std::isfinite(distances[search]) || distances[search] > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "Extrude distance exceeds bounded canvas", "distance"
				);
			samples += uint64_t(std::max(0.0, std::floor(distances[search])));
		}
		if (uint64_t(surface->Width) * surface->Height > 64000000 / samples)
			return context.Fail(Status::LimitExceeded, "Extrude sampling exceeds work budget", "distance");
		if (context.FailureCode != Status::Ok) return false;
		const Rgba extrusion = PixelEffectColour(context, "color");
		const Rgba highlight = PixelEffectColour(context, "highlight_color");
		Image *output =
			context.NewImage("surface_out", surface->Width, surface->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				Rgba colour = ReadPixel(*surface, x, y);
				const auto sample = [&](Vector2 direction, double amount) {
					return SampleNearest(
						*surface,
						(x + 0.5 + direction.X * amount) / surface->Width,
						(y + 0.5 + direction.Y * amount) / surface->Height
					);
				};
				if (colour[3] != 0) {
					if (context.Boolean("highlight") &&
						sample({std::cos(highlightAngle), -std::sin(highlightAngle)}, 1)[3] == 0)
						colour = highlight;
				} else {
					for (size_t search = 0; search < searches; ++search)
						for (double distance = 1; distance <= distances[search]; ++distance) {
							const double amount =
								useBox && mode == 1 ? std::min(distance, finalDistance) : distance;
							const Rgba source = sample(directions[search], amount);
							if (source[3] == 0) continue;
							colour = extrusion;
							if (clone)
								for (size_t channel = 0; channel < 4; ++channel)
									colour[channel] *= source[channel];
							break;
						}
				}
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(Status::InvalidValue, "Extrude sample is nonfinite", "surface_out");
			}
		return context.FailureCode == Status::Ok;
	}
	bool PixelHighlight(NodeContext &context) {
		const Image *surface = context.Input("surface");
		if (!surface) return context.Fail(Status::InvalidValue, "Highlight requires a surface", "surface");
		const Vector4 authored = context.Get<Vector4>("width");
		const std::array<double, 4> widths{authored.Z, authored.X, authored.Y, authored.W};
		const std::array<std::string_view, 4> colours{
			"color_left", "color_right", "color_top", "color_bottom"
		};
		const std::array<Vector2, 4> directions{Vector2{-1, 0}, Vector2{1, 0}, Vector2{0, -1}, Vector2{0, 1}};
		uint64_t samples = 0;
		for (double width : widths) {
			if (!std::isfinite(width) || width > double(Limits::MaximumDimension))
				return context.Fail(
					Status::LimitExceeded, "Highlight width exceeds the bounded canvas", "width"
				);
			samples += uint64_t(std::max(0.0, std::floor(width)));
		}
		if (samples != 0 && uint64_t(surface->Width) * surface->Height > 64000000 / samples)
			return context.Fail(Status::LimitExceeded, "Highlight sampling exceeds the work budget", "width");
		Image *output =
			context.NewImage("surface_out", surface->Width, surface->Height, SurfaceFormat::RGBA8Unorm);
		if (!output) return false;
		for (uint32_t y = 0; y < output->Height; ++y)
			for (uint32_t x = 0; x < output->Width; ++x) {
				Rgba colour = ReadPixel(*surface, x, y);
				if (colour[3] > 0) {
					int nearest = -1;
					double distance = 9999;
					for (size_t side = 0; side < widths.size(); ++side)
						for (double offset = 1; offset <= widths[side]; ++offset) {
							const Rgba sample = SampleNearest(
								*surface,
								(x + 0.5 + directions[side].X * offset) / surface->Width,
								(y + 0.5 + directions[side].Y * offset) / surface->Height
							);
							if (sample[3] != 0) continue;
							if (offset < distance) {
								distance = offset;
								nearest = int(side);
							}
							break;
						}
					if (nearest >= 0) {
						const Colour replacement =
							context.Get<Colour>(colours[size_t(nearest)], Colour{255, 255, 255, 255});
						colour = {
							replacement.Red / 255.0,
							replacement.Green / 255.0,
							replacement.Blue / 255.0,
							replacement.Alpha / 255.0
						};
					}
				}
				if (!WritePixel(*output, x, y, colour))
					return context.Fail(Status::InvalidValue, "Highlight sample is nonfinite", "surface_out");
			}
		return context.FailureCode == Status::Ok;
	}
}
