#include "SourceTileTransform.hpp"

#include "../AtlasPayload.hpp"
#include "Families.hpp"
#include "Sampler.hpp"
#include "Source2DGenerator.hpp"
#include "SourceRepeatTexture.hpp"
#include "SourceTileRandom.hpp"

#include <cmath>
#include <numbers>

namespace engine::imagegraph::detail {
	namespace {
		constexpr uint64_t TILE_TRANSFORM_WORK_LIMIT = 64000000;
		struct TileTransformInputs {
			const Image *Source = nullptr, *Map = nullptr;
			Vector2 RawDimension{}, Spacing{}, Position{}, Scale{1, 1};
			uint32_t Width = 0, Height = 0;
			float Rotation = 0, Shift = 0, UvMix = 1;
			int64_t ShiftAxis = 0, Pattern = 0;
		};
		bool TileTransformFloat(NodeContext &context, std::string_view port, double number, float &out) {
			out = float(number);
			return std::isfinite(out) ||
				   context.Fail(Status::InvalidValue, "Tile control exceeds finite shader range", port);
		}
		bool TileTransformVector(NodeContext &context, std::string_view port, Vector2 value, Vector2 &out) {
			float x, y;
			if (!TileTransformFloat(context, port, value.X, x) ||
				!TileTransformFloat(context, port, value.Y, y))
				return false;
			out = {x, y};
			return true;
		}
		bool TileTransformMain(NodeContext &context, const Image *&source) {
			source = context.Input("surface_in");
			if (!source ||
				!ValidSurfaceLayout(*source, Limits::MaximumDimension, Limits::MaximumEvaluationBytes))
				return context.Fail(Status::InvalidValue, "Tile source surface is invalid", "surface_in");
			if (const auto *value = context.Find("surface_in");
				value && std::holds_alternative<AtlasValue>(*value) &&
				!ValidAtlasPayload(std::get<AtlasValue>(*value)))
				return context.Fail(Status::InvalidValue, "Tile source Atlas is invalid", "surface_in");
			// grug Tile calls raw draw_surface_stretched, not the Atlas-safe helper.
			if (const auto *value = context.Find("surface_in");
				value && std::holds_alternative<AtlasValue>(*value))
				return context.Fail(
					Status::UnsupportedExecution, "Tile raw source draw rejects Atlas", "surface_in"
				);
			return true;
		}
		bool TileTransformRawDimension(NodeContext &context, const Image &source, Vector2 &raw) {
			const auto type = context.Integer("scaling_type");
			if (type < 0 || type > 1)
				return context.Fail(
					Status::UnsupportedExecution, "Tile scaling type is undefined", "scaling_type"
				);
			if (type == 1) {
				const auto amount = context.Vec2("amount", {2, 2});
				raw = {source.Width * amount.X, source.Height * amount.Y};
			} else {
				raw = context.Vec2("dimension", {1, 1});
				if (!context.IsLinked("dimension")) {
					const auto unit = context.Integer("dimension_unit", 1);
					if (unit < 0 || unit > 1)
						return context.Fail(
							Status::UnsupportedExecution, "Tile dimension has no Mask unit", "dimension_unit"
						);
					if (unit == 1) {
						raw.X *= context.Project.SurfaceWidth;
						raw.Y *= context.Project.SurfaceHeight;
					}
				}
			}
			if (!std::isfinite(raw.X) || !std::isfinite(raw.Y))
				return context.Fail(
					Status::InvalidValue, "Tile dimensions must be finite", type == 1 ? "amount" : "dimension"
				);
			return context.FailureCode == Status::Ok;
		}
		bool TileTransformUnit(NodeContext &context, std::string_view port, Vector2 reference, Vector2 &out) {
			auto value = context.Vec2(port);
			if (!context.IsLinked(port)) {
				const auto unit = context.Integer(std::string(port) + "_unit", 1);
				if (unit < 0 || unit > 1)
					return context.Fail(
						Status::UnsupportedExecution,
						"Tile vector unit is undefined",
						std::string(port) + "_unit"
					);
				if (unit == 1) {
					value.X *= reference.X;
					value.Y *= reference.Y;
				}
			}
			return TileTransformVector(context, port, value, out);
		}
		bool PrepareTileTransform(NodeContext &context, TileTransformInputs &inputs) {
			if (!TileTransformMain(context, inputs.Source) ||
				!TileTransformRawDimension(context, *inputs.Source, inputs.RawDimension))
				return false;
			const double roundedX = std::max(1., source2d::GeneratorRoundHalfEven(inputs.RawDimension.X));
			const double roundedY = std::max(1., source2d::GeneratorRoundHalfEven(inputs.RawDimension.Y));
			if (roundedX > Limits::MaximumDimension || roundedY > Limits::MaximumDimension)
				return context.Fail(
					Status::LimitExceeded, "Tile dimensions exceed native limits", "dimension"
				);
			inputs.Width = uint32_t(roundedX);
			inputs.Height = uint32_t(roundedY);
			const auto reference = context.TileReferenceDimension.value_or(inputs.RawDimension);
			if (!TileTransformVector(context, "dimension", inputs.RawDimension, inputs.RawDimension) ||
				!TileTransformUnit(context, "spacing", reference, inputs.Spacing) ||
				!TileTransformUnit(context, "posiiton", reference, inputs.Position) ||
				!TileTransformVector(context, "scale", context.Vec2("scale", {1, 1}), inputs.Scale) ||
				!TileTransformFloat(
					context, "rotation", context.Scalar("rotation") * std::numbers::pi / 180., inputs.Rotation
				) ||
				!TileTransformFloat(context, "shift", context.Scalar("shift"), inputs.Shift) ||
				!TileTransformFloat(context, "uv_mix", context.Scalar("uv_mix", 1), inputs.UvMix))
				return false;
			if (!inputs.Scale.X || !inputs.Scale.Y)
				return context.Fail(Status::InvalidValue, "Tile scale divides by zero", "scale");
			if (float(inputs.Source->Width + inputs.Spacing.X) == 0 ||
				float(inputs.Source->Height + inputs.Spacing.Y) == 0)
				return context.Fail(Status::InvalidValue, "Tile repeat size divides by zero", "spacing");
			inputs.ShiftAxis = context.Integer("shift_axis");
			inputs.Pattern = context.Integer("pattern");
			if (inputs.ShiftAxis < 0 || inputs.ShiftAxis > 1)
				return context.Fail(
					Status::UnsupportedExecution, "Tile shift axis is undefined", "shift_axis"
				);
			if (inputs.Pattern < 0 || inputs.Pattern > 2)
				return context.Fail(Status::UnsupportedExecution, "Tile pattern is undefined", "pattern");
			inputs.Map = context.Input("uv_map");
			if (inputs.Map) {
				if (!ValidSurfaceLayout(
						*inputs.Map, Limits::MaximumDimension, Limits::MaximumEvaluationBytes
					))
					return context.Fail(Status::InvalidValue, "Tile UV map is invalid", "uv_map");
				if (const auto *value = context.Find("uv_map");
					value && std::holds_alternative<AtlasValue>(*value))
					return context.Fail(
						Status::UnsupportedExecution, "Tile raw UV binding rejects Atlas", "uv_map"
					);
			}
			return context.FailureCode == Status::Ok;
		}
		float TileTransformMod2(float value) {
			return value - std::floor(value / 2.f) * 2.f;
		}
		bool TileTransformCoordinate(
			NodeContext &context,
			const TileTransformInputs &inputs,
			uint32_t x,
			uint32_t y,
			float &u,
			float &v,
			float &alpha,
			bool &clipped
		) {
			alpha = 1;
			clipped = float(x) + .5f >= inputs.RawDimension.X || float(y) + .5f >= inputs.RawDimension.Y;
			if (clipped) return true;
			u = (float(x) + .5f) / float(inputs.RawDimension.X);
			v = (float(y) + .5f) / float(inputs.RawDimension.Y);
			if (inputs.Map) {
				const auto pixel = SampleNearest(*inputs.Map, u, v);
				const float mx = float(pixel[0]), my = 1.f - float(pixel[1]);
				alpha = float(pixel[3]);
				u += (mx - u) * inputs.UvMix;
				v += (my - v) * inputs.UvMix;
			}
			float px = (u * float(inputs.RawDimension.X) - float(inputs.Position.X)) / float(inputs.Scale.X);
			float py = (v * float(inputs.RawDimension.Y) - float(inputs.Position.Y)) / float(inputs.Scale.Y);
			const float cosine = std::cos(inputs.Rotation), sine = std::sin(inputs.Rotation);
			const float rotatedX = px * cosine - py * sine;
			py = px * sine + py * cosine;
			px = rotatedX;
			const float width = float(inputs.Source->Width), height = float(inputs.Source->Height);
			const float repeatX = width + float(inputs.Spacing.X), repeatY = height + float(inputs.Spacing.Y);
			float tileX = std::floor(px / repeatX), tileY = std::floor(py / repeatY);
			if (inputs.ShiftAxis == 0 && TileTransformMod2(tileY) >= 1.f) px += width * inputs.Shift;
			if (inputs.ShiftAxis == 1 && TileTransformMod2(tileX) >= 1.f) py += height * inputs.Shift;
			tileX = std::floor(px / repeatX);
			tileY = std::floor(py / repeatY);
			u = (px - tileX * repeatX) / width;
			v = (py - tileY * repeatY) / height;
			if (inputs.Pattern == 1) {
				if (TileTransformMod2(tileX) >= 1.f) u = 1.f - u;
				if (TileTransformMod2(tileY) >= 1.f) v = 1.f - v;
			} else if (inputs.Pattern == 2) {
				constexpr float PI = std::numbers::pi_v<float>, HALF_PI = PI / 2.f;
				const float angle =
					TileTransformMod2(tileY) >= 1.f ? PI + HALF_PI - HALF_PI * tileX : HALF_PI * tileX;
				const float c = std::cos(angle), s = std::sin(angle);
				const float ox = u - .5f, oy = v - .5f;
				u = .5f + ox * c - oy * s;
				v = .5f + ox * s + oy * c;
			}
			if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(alpha))
				return context.Fail(
					Status::InvalidValue,
					"Tile derived coordinate is nonfinite",
					inputs.Map ? "uv_map" : "scale"
				);
			clipped = u < 0 || v < 0 || u > 1 || v > 1;
			return true;
		}
		bool QuoteTileTransform(NodeContext &context, const TileTransformInputs &inputs, uint64_t &work) {
			const uint64_t pixels = uint64_t(inputs.Width) * inputs.Height;
			const uint64_t cost = pixels * (inputs.Map ? 128 : 64);
			if (work > TILE_TRANSFORM_WORK_LIMIT || cost > TILE_TRANSFORM_WORK_LIMIT - work)
				return context.Fail(
					Status::LimitExceeded, "Tile selected batch exceeds native work limit", "surface_out"
				);
			work += cost;
			for (uint32_t y = 0; y < inputs.Height; ++y)
				for (uint32_t x = 0; x < inputs.Width; ++x) {
					float u, v, alpha;
					bool clipped;
					if (!TileTransformCoordinate(context, inputs, x, y, u, v, alpha, clipped)) return false;
				}
			return true;
		}
		bool DrawSourceTileTransform(NodeContext &context) {
			ENGINE_PROFILE("imagegraph.source.tile");
			if (!context.TileReferenceDimension) {
				size_t index = 0;
				if (!SourceTilePreviewIndex(context, 1, index)) return false;
			}
			TileTransformInputs inputs;
			uint64_t work = 0;
			if (!PrepareTileTransform(context, inputs) || !QuoteTileTransform(context, inputs, work))
				return false;
			const auto format = ResolveProcessorSurfaceFormat(context, inputs.Source);
			if (!format) return false;
			auto *out = context.NewImage("surface_out", inputs.Width, inputs.Height, *format);
			if (!out) return false;
			for (uint32_t y = 0; y < inputs.Height; ++y)
				for (uint32_t x = 0; x < inputs.Width; ++x) {
					float u, v, alpha;
					bool clipped;
					if (!TileTransformCoordinate(context, inputs, x, y, u, v, alpha, clipped)) return false;
					Rgba pixel = clipped ? Rgba{} : SampleNearest(*inputs.Source, u, v);
					pixel[3] = float(pixel[3]) * alpha;
					if (!WritePixel(*out, x, y, pixel))
						return context.Fail(
							Status::InvalidValue, "Tile output sample is nonfinite", "surface_out"
						);
				}
			return true;
		}
	}
	bool SourceTilePreviewIndex(NodeContext &context, size_t rows, size_t &index) {
		index = 0;
		for (const auto &property : context.Authored.SourceProperties) {
			if (property.Port != "preview_index") continue;
			const auto *value = std::get_if<int64_t>(&property.Data);
			if (!value || *value < 0 || uint64_t(*value) >= rows)
				return context.Fail(
					Status::InvalidValue, "Tile preview row is outside the selected batch", "preview_index"
				);
			index = size_t(*value);
		}
		return true;
	}
	bool SourceTileReferenceDimension(NodeContext &context, Vector2 &dimension) {
		const Image *source = nullptr;
		return TileTransformMain(context, source) && TileTransformRawDimension(context, *source, dimension);
	}
	bool AdmitSourceTileTransform(NodeContext &context, uint64_t &work) {
		TileTransformInputs inputs;
		return PrepareTileTransform(context, inputs) &&
			   ResolveProcessorSurfaceFormat(context, inputs.Source).has_value() &&
			   QuoteTileTransform(context, inputs, work);
	}
	std::span<const ExecutorEntry> SourceTileTransformExecutors() {
		static constexpr ExecutorEntry entries[] = {
			{"pc.tile", DrawSourceTileTransform, true},
			{"pc.tile_random", DrawSourceTileRandom, true},
			{"pc.repeat_texture", DrawSourceRepeatTexture, true}
		};
		return entries;
	}
}
