#include "../AtlasPayload.hpp"
#include "../SourceSafeDraw.hpp"
#include "../TimelineDrivers.hpp"
#include "Processor.hpp"
#include "Sampler.hpp"
#include "SourceBlendFormula.hpp"

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	namespace {
		bool Single(const Image &image) {
			return image.Format == SurfaceFormat::R8Unorm || image.Format == SurfaceFormat::R16Float ||
				   image.Format == SurfaceFormat::R32Float;
		}
		Rgba SafeTexture(const Image &image, double u, double v, bool filter = false) {
			Rgba result = Texture(image, u, v, filter);
			if (Single(image)) result = {result[0], result[0], result[0], 1};
			return result;
		}

		bool FeatherSourceMask(NodeContext &context, Image &mask, double feather) {
			const int count = std::max(1, int(DriverRoundHalfEven(feather)));
			// A fractional extent can read an unuploaded weight as a contributing tap.
			if (std::ceil(feather) > count)
				return context.Fail(
					Status::UnsupportedExecution,
					"Blend mask Gaussian reads a contributing unobserved uniform weight",
					"mask_feather"
				);
			std::vector<double> weights(count);
			const double spread = .3 * ((count - 1) * .5 - 1) + .8;
			double total = 0;
			for (int i = 0; i < count; ++i) {
				weights[i] = std::exp(-std::pow(i * .5, 2) / (2 * spread * spread));
				total += weights[i] * (i ? 2 : 1);
			}
			for (double &weight : weights)
				weight /= total;
			Image horizontal = mask, vertical = mask;
			for (int pass = 0; pass < 2; ++pass) {
				const Image &source = pass == 0 ? mask : horizontal;
				Image &target = pass == 0 ? horizontal : vertical;
				for (uint32_t y = 0; y < mask.Height; ++y)
					for (uint32_t x = 0; x < mask.Width; ++x) {
						double alphaWeight = .00001, sumWeight = .00001;
						Rgba result{};
						const int extent = std::max(0, int(std::ceil(feather)) - 1);
						for (int i = -extent; i <= extent; ++i) {
							const double weight = weights[std::abs(i)];
							const double sx = x + .5 + (pass == 0 ? i : 0), sy = y + .5 + (pass == 1 ? i : 0);
							const Rgba pixel = sx < 0 || sy < 0 || sx > mask.Width || sy > mask.Height
												   ? Rgba{}
												   : Texture(source, sx / mask.Width, sy / mask.Height, true);
							alphaWeight += weight * pixel[3];
							sumWeight += weight;
							for (size_t c = 0; c < 3; ++c)
								result[c] += weight * pixel[3] * pixel[c];
						}
						for (size_t c = 0; c < 3; ++c)
							result[c] /= alphaWeight;
						result[3] = alphaWeight / sumWeight;
						if (!WritePixel(target, x, y, result))
							return context.Fail(
								Status::InvalidValue,
								"Blend Gaussian mask exceeds numeric format",
								"mask_feather"
							);
					}
			}
			mask = std::move(vertical);
			return true;
		}
	}
	bool SourceComposeBlend(NodeContext &context) {
		bool failed = false;
		if (CopyWhenInactive(context, failed)) return !failed;
		for (const std::string_view port : {"background", "foreground", "mask"}) {
			const Value *value = context.Find(port);
			if (const auto *object = value ? std::get_if<AtlasValue>(value) : nullptr;
				object && (!object->Data || object->Data->Kind != AtlasKind::SurfaceAtlas))
				return context.Fail(
					Status::UnsupportedExecution,
					"Blend source is_surface excludes generic Atlas objects",
					port
				);
			if (value && std::holds_alternative<DynamicSurfaceValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution,
					"Blend dynamic draw requires observed source shader and raw surface identity",
					port
				);
		}
		const Image *original = context.Input("background"), *second = context.Input("foreground");
		if (!original)
			return context.Fail(Status::InvalidValue, "Blend requires its Background", "background");
		const bool swap = context.Boolean("swap");
		const Image *background = swap ? second : original, *foreground = swap ? original : second;
		const Value *foregroundValue = context.Find(swap ? "background" : "foreground");
		const auto *atlas = foregroundValue ? std::get_if<AtlasValue>(foregroundValue) : nullptr;
		if (atlas && atlas->Data && atlas->Data->Kind != AtlasKind::SurfaceAtlas)
			return context.Fail(
				Status::UnsupportedExecution,
				"Blend requires source SurfaceAtlas identity, not a generic Atlas subtype",
				"foreground"
			);
		const int64_t dimension = context.Integer("output_dimension"), fill = context.Integer("fill_mode"),
					  mode = context.Integer("blend_mode");
		if (dimension < 0 || dimension > 4 || fill < 0 || fill > 2)
			return context.Fail(Status::InvalidValue, "Blend dimension or fill choice is invalid");
		if (foreground && !SupportedBlendMode(mode))
			return context.Fail(
				Status::UnsupportedExecution,
				"Blend source separators are not executable blend modes",
				"blend_mode"
			);
		if (atlas && dimension > 1)
			return context.Fail(
				Status::UnsupportedExecution,
				"Blend SurfaceAtlas output dimension is limited to Background or Foreground",
				"output_dimension"
			);
		const Image *mask = context.Input("mask");
		const auto extent = [](const Image *image) {
			return Vector2{double(image ? image->Width : 1), double(image ? image->Height : 1)};
		};
		Vector2 size = extent(background);
		if (dimension == 1)
			size = extent(foreground);
		else if (dimension == 2)
			size = extent(mask);
		else if (dimension == 3) {
			const auto f = extent(foreground), m = extent(mask);
			size = {std::max({size.X, f.X, m.X}), std::max({size.Y, f.Y, m.Y})};
		} else if (dimension == 4)
			size = context.Vec2("constant_dimension", {32, 32});
		const double rw = DriverRoundHalfEven(size.X), rh = DriverRoundHalfEven(size.Y);
		const uint32_t maximum = std::min(Limits::MaximumDimension, context.Request.MaximumImageDimension);
		if (!std::isfinite(rw) || !std::isfinite(rh) || rw < 1 || rh < 1 || rw > maximum || rh > maximum)
			return context.Fail(
				Status::LimitExceeded, "Blend output dimensions exceed native bounds", "constant_dimension"
			);
		const auto width = uint32_t(rw), height = uint32_t(rh);
		const auto format = ResolveProcessorSurfaceFormat(context, original);
		if (!format) return false;
		const auto layout = CheckedSurfaceLayout(width, height, *format, Limits::MaximumOutputBytes);
		if (!layout) return context.Fail(Status::LimitExceeded, "Blend output layout exceeds native bounds");
		const double feather = context.Scalar("mask_feather", 1);
		if (!std::isfinite(feather) || feather < 0 || feather > 64)
			return context.Fail(
				Status::LimitExceeded, "Blend mask feather exceeds bounded source work", "mask_feather"
			);
		const uint64_t pixels = uint64_t(width) * height,
					   maskPixels = mask ? uint64_t(mask->Width) * mask->Height : 0;
		const uint64_t cost = 64 + uint64_t(std::ceil(feather)) * 16,
					   rows = std::max(uint64_t{1}, uint64_t(context.ProcessorCount));
		if (rows > 64000000 / cost || pixels + maskPixels > 64000000 / (rows * cost))
			return context.Fail(Status::LimitExceeded, "Blend complete processor batch exceeds work budget");
		const uint64_t maskBytes = maskPixels * 4;
		auto scratch =
			context.ReserveWorkspace(layout->Bytes * 2 + maskBytes * 3 + 64 * sizeof(double), "foreground");
		if (!scratch) return false;
		Image back{width, height, std::vector<uint8_t>(layout->Bytes), 0, *format}, fore = back;
		Vector2 position = context.Vec2("position", {.5, .5});
		if (context.Integer("position_unit", 1) == 1 && !context.IsLinked("position")) {
			position.X *= width;
			position.Y *= height;
		}
		Vector2 foreOffset{}, backOffset{};
		if (atlas && atlas->Data) {
			foreOffset = dimension == 0 ? atlas->Data->Position : Vector2{};
			if (dimension == 1) backOffset = {-atlas->Data->Position.X, -atlas->Data->Position.Y};
		} else if (foreground)
			foreOffset = {position.X - foreground->Width / 2., position.Y - foreground->Height / 2.};
		const bool none = fill == 0 || atlas;
		const Value *backgroundValue = context.Find(swap ? "foreground" : "background");
		if (foreground && backgroundValue && std::holds_alternative<AtlasValue>(*backgroundValue) && none &&
			(!atlas || dimension == 0))
			return context.Fail(
				Status::UnsupportedExecution,
				"Blend selected path calls raw surface_get_width on a SurfaceAtlas object",
				"background"
			);
		if (mask && context.Find("mask") && std::holds_alternative<AtlasValue>(*context.Find("mask")) &&
			(context.Boolean("invert_mask") || feather != 0))
			return context.Fail(
				Status::UnsupportedExecution,
				"Blend mask modification calls raw surface_get_width on a SurfaceAtlas object",
				"mask"
			);
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				if (background) {
					Rgba pixel = dimension == 1 && atlas
									 ? ((x + .5 - backOffset.X >= 0 && y + .5 - backOffset.Y >= 0 &&
										 x + .5 - backOffset.X < background->Width &&
										 y + .5 - backOffset.Y < background->Height)
											? SafeTexture(
												  *background,
												  (x + .5 - backOffset.X) / background->Width,
												  (y + .5 - backOffset.Y) / background->Height
											  )
											: Rgba{})
									 : SafeTexture(*background, u, v);
					if (!WritePixel(back, x, y, pixel))
						return context.Fail(Status::InvalidValue, "Blend background staging is nonfinite");
				}
				if (foreground) {
					Rgba pixel{};
					if (none) {
						// Explicit CPU pixel-center reference; licensed plain-draw coverage remains external.
						const double fx = x + .5 - std::trunc(foreOffset.X),
									 fy = y + .5 - std::trunc(foreOffset.Y);
						if (fx >= 0 && fy >= 0 && fx < foreground->Width && fy < foreground->Height)
							pixel = SafeTexture(*foreground, fx / foreground->Width, fy / foreground->Height);
					} else if (fill == 1) {
						pixel = Single(*foreground) ? SafeTexture(*foreground, u, v)
													: SampleTexture(*foreground, u, v, ReadSampler(context));
					} else
						pixel = SafeTexture(
							*foreground,
							ShaderFract((x + .5) / foreground->Width),
							ShaderFract((y + .5) / foreground->Height)
						);
					if (!WritePixel(fore, x, y, pixel))
						return context.Fail(Status::InvalidValue, "Blend foreground staging is nonfinite");
				}
			}
		Image modified;
		if (mask && (context.Boolean("invert_mask") || feather != 0)) {
			modified = {mask->Width, mask->Height, std::vector<uint8_t>(maskBytes), 0};
			const bool alphaOnly = context.Boolean("mask_alpha_only");
			for (uint32_t y = 0; y < mask->Height; ++y)
				for (uint32_t x = 0; x < mask->Width; ++x) {
					Rgba p = SafeTexture(*mask, (x + .5) / mask->Width, (y + .5) / mask->Height);
					if (!Single(*mask)) {
						double amount = alphaOnly ? p[3] : (p[0] + p[1] + p[2]) / 3;
						if (context.Boolean("invert_mask")) amount = 1 - amount;
						p = alphaOnly ? Rgba{1, 1, 1, amount} : Rgba{amount, amount, amount, p[3]};
					}
					if (!WritePixel(modified, x, y, p))
						return context.Fail(Status::InvalidValue, "Blend modified mask is nonfinite");
				}
			if (feather > 0 && !FeatherSourceMask(context, modified, feather)) return false;
			mask = &modified;
		}
		auto atlasCharge =
			atlas ? context.ReserveWorkspace(AtlasStorageBytes(*atlas, true) + layout->Bytes, "surface_out")
				  : std::optional<AllocationReservation>{};
		if (atlas && !atlasCharge) return false;
		Image atlasImage;
		Image *output = nullptr;
		if (atlas) {
			atlasImage = {width, height, std::vector<uint8_t>(layout->Bytes), 0, *format};
			output = &atlasImage;
		} else
			output = context.NewImage("surface_out", width, height, *format);
		if (!output) return false;
		const Image *backDraw = none && (!atlas || dimension == 0) ? background : &back;
		for (uint32_t y = 0; y < height; ++y)
			for (uint32_t x = 0; x < width; ++x) {
				const double u = (x + .5) / width, v = (y + .5) / height;
				Rgba result = backDraw ? SafeTexture(*backDraw, u, v) : Rgba{};
				if (foreground && backDraw && !Single(*backDraw)) {
					double amount = 1;
					if (mask) {
						const auto m = Texture(*mask, u, v, false);
						amount = context.Boolean("mask_alpha_only") ? m[3] : (m[0] + m[1] + m[2]) / 3 * m[3];
					}
					const auto status = SourceBlendFormula(
						result,
						SafeTexture(fore, u, v),
						mode,
						context.Scalar("opacity", 1),
						amount,
						context.Boolean("preserve_alpha"),
						result
					);
					if (status != BlendPixelStatus::Ok)
						return context.Fail(
							Status::UnsupportedExecution,
							"Blend selected source equation has an undefined division",
							"blend_mode"
						);
				}
				if (!WritePixel(*output, x, y, result))
					return context.Fail(
						Status::InvalidValue, "Blend output exceeds its numeric surface range"
					);
			}
		if (atlas && atlas->Data) {
			AtlasValue result = *atlas;
			result.Data->Surface.Data = std::move(atlasImage);
			result.Data->Dimension = {double(width), double(height)};
			result.Data->OriginalDimension = result.Data->OriginalSurface ? Vector2{double(result.Data->OriginalSurface->Data.Width), double(result.Data->OriginalSurface->Data.Height)} : Vector2{1, 1};
			if (dimension == 0) result.Data->Position = {};
			context.SetValue("surface_out", std::move(result));
		}
		return context.FailureCode == Status::Ok;
	}
}
