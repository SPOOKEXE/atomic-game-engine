#include "FontTextRaster.hpp"

#include "FontPayload.hpp"
#include "FontTextLayout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		std::array<FontGlyphPlacement, 2>
		SplitAstral(const FontData &font, const FontGlyphPlacement &placement) {
			auto first = placement, second = placement;
			const uint32_t point = placement.Character - 0x10000;
			first.Character = 0xd800 + (point >> 10);
			second.Character = 0xdc00 + (point & 1023);
			const double angle = std::remainder(placement.Rotation, 360.0) * std::numbers::pi / 180;
			const double advance = FontUnitAdvance(font, first.Character) * placement.Scale.X;
			second.Position.X += advance * std::cos(angle);
			second.Position.Y -= advance * std::sin(angle);
			return {first, second};
		}

		struct FontGlyphTransform {
			const FontGlyph *Glyph = nullptr;
			const Image *Frame = nullptr;
			double Cosine = 1, Sine = 0, Left = 0, Top = 0;
		};
		Status GlyphTransform(
			const FontData &font,
			const FontGlyphPlacement &placement,
			const FontTextRasterOptions &options,
			FontGlyphTransform &output,
			std::string &failure
		) {
			const auto fail = [&](Status status, const char *message) {
				failure = message;
				return status;
			};
			if (!std::isfinite(placement.Position.X) || !std::isfinite(placement.Position.Y) ||
				!std::isfinite(placement.Scale.X) || !std::isfinite(placement.Scale.Y) ||
				!std::isfinite(placement.Rotation) || !std::isfinite(placement.VerticalOrigin))
				return fail(Status::InvalidValue, "glyph transform is nonfinite");
			if (options.Sampler.Interpolation == 6)
				return fail(Status::UnsupportedExecution, "CleanEdge glyph interpolation is unrepresented");
			if (options.Sampler.Interpolation < 1 || options.Sampler.Interpolation > 4)
				return fail(Status::UnsupportedExecution, "glyph interpolation choice is unrepresented");
			const auto *glyph = FontGlyphForUnit(font, placement.Character);
			if (!glyph) {
				if (!font.GlyphMapComplete)
					return fail(Status::UnsupportedExecution, "glyph raster has no captured mapping");
				return Status::Ok;
			}
			if (!glyph->Present || !glyph->Frame || placement.Scale.X == 0 || placement.Scale.Y == 0)
				return Status::Ok;
			if (*glyph->Frame >= font.Frames.size())
				return fail(Status::InvalidValue, "glyph raster frame is outside font storage");
			if (options.Texture && (!font.SourceTexture || !glyph->TextureRectangle) &&
				((font.Raster == FontRasterProfile::BitmapSurface && !options.NativeBitmapTexture) ||
				 options.DebugTexture))
				return fail(
					Status::UnsupportedExecution,
					"text texture requires observed source glyph atlas coordinates"
				);
			const double radians = std::remainder(placement.Rotation, 360.0) * std::numbers::pi / 180;
			output.Glyph = glyph;
			output.Frame = &font.Frames[*glyph->Frame];
			if (!ValidSurfaceLayout(*output.Frame, Limits::MaximumDimension, Limits::MaximumArrayBytes))
				return fail(Status::InvalidValue, "glyph raster frame layout is malformed");
			output.Cosine = std::cos(radians);
			output.Sine = std::sin(radians);
			output.Left = glyph->Offset.X;
			output.Top = glyph->Offset.Y - placement.VerticalOrigin;
			return Status::Ok;
		}
	}
	Status MeasureFontGlyphRaster(
		const FontData &font,
		const FontGlyphPlacement &placement,
		const FontTextRasterOptions &options,
		uint32_t width,
		uint32_t height,
		FontGlyphRasterFootprint &output,
		std::string &failure
	) {
		if (width > Limits::MaximumDimension || height > Limits::MaximumDimension) {
			failure = "glyph raster canvas exceeds bounded dimensions";
			return Status::LimitExceeded;
		}
		if (font.Characters == FontCharacterProfile::Utf16 && placement.Character > 0xffff &&
			placement.Character <= 0x10ffff) {
			const auto split = SplitAstral(font, placement);
			FontGlyphRasterFootprint first, second;
			if (const auto status =
					MeasureFontGlyphRaster(font, split[0], options, width, height, first, failure);
				status != Status::Ok)
				return status;
			if (const auto status =
					MeasureFontGlyphRaster(font, split[1], options, width, height, second, failure);
				status != Status::Ok)
				return status;
			FontGlyphRasterFootprint candidate;
			if (!first.Work)
				candidate = second;
			else if (!second.Work)
				candidate = first;
			else {
				candidate = {
					std::min(first.Left, second.Left),
					std::min(first.Top, second.Top),
					std::max(first.Right, second.Right),
					std::max(first.Bottom, second.Bottom),
					first.Work + second.Work
				};
			}
			output = candidate;
			return Status::Ok;
		}
		FontGlyphTransform transform;
		const auto status = GlyphTransform(font, placement, options, transform, failure);
		if (status != Status::Ok) return status;
		FontGlyphRasterFootprint candidate;
		if (!transform.Frame || !width || !height) {
			output = candidate;
			return Status::Ok;
		}
		double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
		for (const auto corner : std::array<Vector2, 4>{
				 {{transform.Left, transform.Top},
				  {transform.Left + transform.Frame->Width, transform.Top},
				  {transform.Left, transform.Top + transform.Frame->Height},
				  {transform.Left + transform.Frame->Width, transform.Top + transform.Frame->Height}}
			 }) {
			const double x = corner.X * placement.Scale.X, y = corner.Y * placement.Scale.Y;
			const double px = placement.Position.X + x * transform.Cosine + y * transform.Sine;
			const double py = placement.Position.Y - x * transform.Sine + y * transform.Cosine;
			if (!std::isfinite(px) || !std::isfinite(py)) {
				failure = "glyph transformed extent is nonfinite";
				return Status::InvalidValue;
			}
			left = std::min(left, px);
			right = std::max(right, px);
			top = std::min(top, py);
			bottom = std::max(bottom, py);
		}
		candidate.Left = static_cast<uint32_t>(std::clamp(std::floor(left), 0.0, double(width)));
		candidate.Right = static_cast<uint32_t>(std::clamp(std::ceil(right), 0.0, double(width)));
		candidate.Top = static_cast<uint32_t>(std::clamp(std::floor(top), 0.0, double(height)));
		candidate.Bottom = static_cast<uint32_t>(std::clamp(std::ceil(bottom), 0.0, double(height)));
		const uint64_t taps = options.Sampler.Interpolation == 4   ? 36
							  : options.Sampler.Interpolation == 1 ? 1
																   : 4;
		candidate.Work = uint64_t(candidate.Right - candidate.Left) * (candidate.Bottom - candidate.Top) *
						 (taps * (options.Texture ? 2 : 1) + 2);
		output = candidate;
		failure.clear();
		return Status::Ok;
	}
	Status DrawFontGlyphRaster(
		const FontData &font,
		const FontGlyphPlacement &placement,
		const FontTextRasterOptions &options,
		const FontGlyphRasterFootprint &footprint,
		Image &target,
		std::string &failure
	) {
		FontGlyphRasterFootprint checked;
		const auto status =
			MeasureFontGlyphRaster(font, placement, options, target.Width, target.Height, checked, failure);
		if (status != Status::Ok) return status;
		if (checked.Left != footprint.Left || checked.Top != footprint.Top ||
			checked.Right != footprint.Right || checked.Bottom != footprint.Bottom ||
			checked.Work != footprint.Work) {
			failure = "glyph raster footprint differs from admitted draw";
			return Status::InvalidValue;
		}
		if (font.Characters == FontCharacterProfile::Utf16 && placement.Character > 0xffff &&
			placement.Character <= 0x10ffff) {
			for (const auto &unit : SplitAstral(font, placement)) {
				FontGlyphRasterFootprint part;
				if (const auto result = MeasureFontGlyphRaster(
						font, unit, options, target.Width, target.Height, part, failure
					);
					result != Status::Ok)
					return result;
				if (const auto result = DrawFontGlyphRaster(font, unit, options, part, target, failure);
					result != Status::Ok)
					return result;
			}
			return Status::Ok;
		}

		FontGlyphTransform transform;
		if (const auto result = GlyphTransform(font, placement, options, transform, failure);
			result != Status::Ok)
			return result;
		if (!transform.Frame) return Status::Ok;
		for (uint32_t y = footprint.Top; y < footprint.Bottom; ++y)
			for (uint32_t x = footprint.Left; x < footprint.Right; ++x) {
				const double dx = (x + .5) - placement.Position.X, dy = (y + .5) - placement.Position.Y;
				const double localX =
					(dx * transform.Cosine - dy * transform.Sine) / placement.Scale.X - transform.Left;
				const double localY =
					(dx * transform.Sine + dy * transform.Cosine) / placement.Scale.Y - transform.Top;
				if (localX < 0 || localY < 0 || localX >= transform.Frame->Width ||
					localY >= transform.Frame->Height)
					continue;
				const Vector2 local{localX / transform.Frame->Width, localY / transform.Frame->Height};
				const bool observedAtlas =
					options.Texture && font.SourceTexture && transform.Glyph->TextureRectangle;
				Rgba source = observedAtlas
								  ? Rgba{}
								  : TextureInterpolated(*transform.Frame, local.X, local.Y, options.Sampler);
				const auto decodeDistance = [&] {
					if (font.Raster != FontRasterProfile::NativeSignedDistance) return;
					const double distance = (source[3] * 255 - 128) * font.DistanceSpread / 128;
					const double scale = std::min(std::abs(placement.Scale.X), std::abs(placement.Scale.Y));
					source = {
						1,
						1,
						1,
						options.DistanceAntialias ? std::clamp(.5 + distance * scale, 0.0, 1.0)
												  : (distance >= 0 ? 1.0 : 0.0)
					};
				};

				if (options.Texture) {
					double gx = 0, gy = 0;
					if (font.SourceTexture && transform.Glyph->TextureRectangle) {
						const auto &rectangle = *transform.Glyph->TextureRectangle;
						gx = rectangle.X + local.X * rectangle.Z;
						gy = rectangle.Y + local.Y * rectangle.W;
						source = SampleTexture(
							*font.SourceTexture,
							gx / font.SourceTexture->Width,
							gy / font.SourceTexture->Height,
							options.Sampler
						);
					}
					decodeDistance();
					const auto texture = SampleTexture(*options.Texture, local.X, local.Y, options.Sampler);
					for (size_t channel = 0; channel < 4; ++channel)
						source[channel] *= texture[channel];
					if (options.DebugTexture) source = {gx, gy, 0, 1};
				} else
					decodeDistance();

				if (!options.DebugTexture || !options.Texture) {
					const std::array<double, 4> tint{
						placement.Tint.Red / 255.0,
						placement.Tint.Green / 255.0,
						placement.Tint.Blue / 255.0,
						placement.Tint.Alpha / 255.0
					};
					for (size_t channel = 0; channel < 4; ++channel)
						source[channel] *= tint[channel];
				}
				const auto destination = ReadPixel(target, x, y);
				Rgba result;
				for (size_t channel = 0; channel < 3; ++channel)
					result[channel] =
						source[channel] * (options.Blend == FontTextBlend::AlphaMultiply ? source[3] : 1) +
						destination[channel] * (1 - source[3]);
				result[3] = source[3] + destination[3];
				if (!WritePixel(target, x, y, result)) {
					failure = "glyph raster output cannot represent finite pixels";
					return Status::UnsupportedExecution;
				}
			}
		failure.clear();
		return Status::Ok;
	}
}
