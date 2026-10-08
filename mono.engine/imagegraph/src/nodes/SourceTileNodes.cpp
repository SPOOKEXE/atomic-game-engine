#include "../SourceGradient.hpp"
#include "Families.hpp"
#include "Processor.hpp"
#include "SourceTileProperties.hpp"
#include "SourceTileRender.hpp"
#include "SourceTileRule.hpp"
#include "SourceTileTerrain.hpp"

#include <engine/imagegraph/FrameTime.hpp>

namespace engine::imagegraph::detail {
	namespace {
		// Node_Tiler and sh_draw_tile_map use fixed RGBA8 output and RGBA16 tile IDs.
		// The source pin is b69eca232217360cf1502ef0223523d818606652.
		bool TileSurface(NodeContext &context, const Image *image, std::string_view port) {
			if (!image || !ValidSurfaceLayout(*image, Limits::MaximumDimension, Limits::MaximumOutputBytes) ||
				!FiniteSurfaceSamples(*image))
				return context.Fail(Status::InvalidValue, "tile input requires a finite owned surface", port);
			return true;
		}
		const TilesetData *TileResource(NodeContext &context, std::string_view port) {
			const Value *value = context.Find(port);
			const auto *tileset = value ? std::get_if<TilesetValue>(value) : nullptr;
			if (!tileset || !tileset->Data || !ValidTilesetPayload(*tileset)) {
				context.Fail(
					Status::TypeMismatch, "tile operation requires a bounded tileset resource", port
				);
				return nullptr;
			}
			return &*tileset->Data;
		}
		uint64_t TilePropertyBytes(const NodeContext &context) {
			uint64_t bytes = 0;
			for (const auto &property : context.Authored.SourceProperties)
				bytes = MeshAddBytes(bytes, RetainedPayloadBytes(property.Data));
			return bytes;
		}
		bool TileScratch(Image &image, uint32_t width, uint32_t height, SurfaceFormat format) {
			const auto layout = CheckedSurfaceLayout(width, height, format, Limits::MaximumOutputBytes);
			if (!layout) return false;
			image.Width = width;
			image.Height = height;
			image.Format = format;
			image.Pixels.assign(size_t(layout->Bytes), 0);
			return true;
		}
		bool TileCopy(const Image &source, Image &target) {
			for (uint32_t y = 0; y < source.Height; y++)
				for (uint32_t x = 0; x < source.Width; x++)
					if (!WritePixel(target, x, y, ReadPixel(source, x, y))) return false;
			return true;
		}
		bool TileDimensions(
			NodeContext &context,
			const TilesetData &tileset,
			const Image &map,
			uint32_t &width,
			uint32_t &height
		) {
			if (map.Width < 2 || map.Height < 2)
				return context.Fail(
					Status::UnsupportedExecution,
					"source tile sampling divides by tilemap dimension minus one",
					"tilemap"
				);
			const double w = std::max(1.0, SourceRoundEven(tileset.TileSize.X * map.Width)),
						 h = std::max(1.0, SourceRoundEven(tileset.TileSize.Y * map.Height));
			if (w > Limits::MaximumDimension || h > Limits::MaximumDimension ||
				w > context.Request.MaximumImageDimension || h > context.Request.MaximumImageDimension)
				return context.Fail(
					Status::LimitExceeded, "rendered tile dimensions exceed the image budget", "rendered"
				);
			if (float(tileset.Texture.Width) / float(tileset.TileSize.X) < 1)
				return context.Fail(
					Status::UnsupportedExecution, "source tileset has no complete texture column", "input_0"
				);
			width = uint32_t(w);
			height = uint32_t(h);
			return true;
		}
		bool TileRenderMap(NodeContext &context, const TilesetData &tileset, const Image &map) {
			uint32_t width = 0, height = 0;
			if (!TileDimensions(context, tileset, map, width, height)) return false;
			const uint64_t samples = uint64_t(width) * height;
			if (samples > 64'000'000 / (1 + tileset.Animations.size() * 2))
				return context.Fail(
					Status::LimitExceeded, "tile render exceeds the bounded sample count", "rendered"
				);
			Image *output = context.NewImage("rendered", width, height, SurfaceFormat::RGBA8Unorm);
			if (!output) return false;
			const float frame = float(FrameTimeToReal(
				{context.Request.Tick, context.Request.Subframe, context.Request.NegativeFrame}
			));
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = 0; x < width; x++) {
					Rgba pixel;
					if (!RenderSourceTilePixel(tileset, map, x, y, width, height, frame, pixel))
						return context.Fail(
							Status::UnsupportedExecution,
							"source tile animation or index reads undefined shader storage",
							"tilemap"
						);
					if (!WritePixel(*output, x, y, pixel))
						return context.Fail(
							Status::InvalidValue, "tile render produced a nonfinite pixel", "rendered"
						);
				}
			return true;
		}
		bool TileSet(NodeContext &context) {
			const Image *texture = context.Input("texture");
			if (!TileSurface(context, texture, "texture")) return false;
			const Vector2 size = context.Vec2("tile_size", {16, 16});
			if (!MeshFinite(size) || size.X <= 0 || size.Y <= 0 || size.X > Limits::MaximumDimension ||
				size.Y > Limits::MaximumDimension)
				return context.Fail(
					Status::InvalidValue, "tile size requires finite positive coordinates", "tile_size"
				);
			const uint64_t properties = TilePropertyBytes(context);
			const uint64_t bytes = MeshAddBytes(
				MeshAddBytes(texture->Pixels.capacity(), sizeof(TilesetData) + 1024),
				properties > UINT64_MAX / 4 ? UINT64_MAX : properties * 4
			);
			if (!context.ReserveOutput(bytes, "tileset")) return false;
			TilesetData data;
			data.Texture = *texture;
			data.TileSize = size;
			data.DisplayName =
				context.Authored.SourceDisplayName.empty() ? "Tileset" : context.Authored.SourceDisplayName;
			if (!ReadTilesetProperties(context, data)) return false;
			TilesetValue output;
			output.Data.emplace() = std::move(data);
			if (!ValidTilesetPayload(output))
				return context.Fail(
					Status::LimitExceeded, "tileset resource exceeds source storage bounds", "tileset"
				);
			context.SetValue("tileset", std::move(output));
			return context.FailureCode == Status::Ok;
		}
		bool TileRender(NodeContext &context) {
			const TilesetData *tileset = TileResource(context, "input_0");
			const Image *map = context.Input("tilemap");
			return tileset && TileSurface(context, map, "tilemap") && TileRenderMap(context, *tileset, *map);
		}
		// Source rules group the original input, apply ordered passes in RGBA16, then
		// write the result back through the original input format before returning it.
		bool TileApplyRules(
			NodeContext &context,
			const TilesetData &tileset,
			const std::vector<TileRuleData> &rules,
			const Image &source,
			Image &result,
			float seed
		) {
			if (!std::isfinite(seed))
				return context.Fail(
					Status::UnsupportedExecution,
					"source tile rule seed exceeds finite shader uniform range",
					"seed"
				);
			const auto layout = CheckedSurfaceLayout(
				source.Width, source.Height, SurfaceFormat::RGBA16Float, Limits::MaximumOutputBytes
			);
			const auto groupLayout = CheckedSurfaceLayout(
				source.Width, source.Height, SurfaceFormat::R16Float, Limits::MaximumOutputBytes
			);
			if (!layout || !groupLayout)
				return context.Fail(
					Status::LimitExceeded, "tile rules exceed scratch image bounds", "tilemap"
				);
			auto charge = context.ReserveWorkspace(
				layout->Bytes * 2 + groupLayout->Bytes + source.Pixels.size(), "tilemap"
			);
			if (!charge) return false;
			Image current, next, groups, sourceFormat;
			if (!TileScratch(current, source.Width, source.Height, SurfaceFormat::RGBA16Float) ||
				!TileScratch(next, source.Width, source.Height, SurfaceFormat::RGBA16Float) ||
				!TileScratch(groups, source.Width, source.Height, SurfaceFormat::R16Float) ||
				!TileScratch(sourceFormat, source.Width, source.Height, source.Format) ||
				!TileCopy(source, current))
				return context.Fail(Status::InvalidValue, "tile rule scratch storage is invalid", "tilemap");
			bool applied = false;
			uint64_t work = 0;
			for (const auto &rule : rules) {
				if (!rule.Active || rule.Replacements.empty()) continue;
				PreparedTileRule prepared;
				bool undefinedGroup = false;
				if (!PrepareSourceTileRule(rule, tileset, prepared, undefinedGroup))
					return context.Fail(
						undefinedGroup ? Status::UnsupportedExecution : Status::LimitExceeded,
						undefinedGroup ? "source terrain selection reads a group band that was never uploaded"
									   : "tile rule exceeds shader selection or replacement bounds",
						"ruleTiles"
					);
				const uint64_t origins =
					(std::max(0, int32_t(prepared.Width) - int32_t(prepared.ScanWidth)) + 1) *
					uint64_t(std::max(0, int32_t(prepared.Height) - int32_t(prepared.ScanHeight)) + 1);
				const uint64_t selectionArea =
					(prepared.Width + 2 * prepared.Range) * uint64_t(prepared.Height + 2 * prepared.Range);
				const uint64_t cost = origins * selectionArea + uint64_t(prepared.UniqueCount) * 64 + 1;
				const uint64_t pixels = uint64_t(source.Width) * source.Height;
				if (cost > 64'000'000 || pixels > (64'000'000 - work) / cost)
					return context.Fail(
						Status::LimitExceeded, "tile rules exceed the bounded sample count", "ruleTiles"
					);
				work += pixels * cost;
				for (uint32_t y = 0; y < source.Height; y++)
					for (uint32_t x = 0; x < source.Width; x++) {
						if (!WritePixel(
								groups,
								x,
								y,
								{SourceTileRuleGroup(prepared, float(ReadPixel(source, x, y)[0])), 0, 0, 1}
							))
							return context.Fail(Status::InvalidValue, "tile group packing failed", "tilemap");
					}
				for (uint32_t y = 0; y < source.Height; y++)
					for (uint32_t x = 0; x < source.Width; x++) {
						if (!WritePixel(
								next, x, y, SourceTileRulePixel(prepared, current, groups, x, y, seed)
							))
							return context.Fail(
								Status::InvalidValue, "tile rule produced invalid pixels", "tilemap"
							);
					}
				std::swap(current, next);
				applied = true;
			}
			if (applied) {
				if (!TileCopy(current, sourceFormat) || !TileCopy(sourceFormat, result))
					return context.Fail(
						Status::InvalidValue, "tile rule format conversion failed", "tilemap"
					);
			} else if (!TileCopy(source, result))
				return context.Fail(Status::InvalidValue, "tile rule input copy failed", "tilemap");
			return true;
		}
		bool TileRule(NodeContext &context) {
			const TilesetData *tileset = TileResource(context, "input_0");
			const Image *source = context.Input("tilemap");
			if (!tileset || !TileSurface(context, source, "tilemap")) return false;
			auto charge =
				context.ReserveWorkspace(MeshAddBytes(TilePropertyBytes(context) * 4, 4096), "ruleTiles");
			if (!charge) return false;
			std::vector<TileRuleData> rules;
			if (!ReadTileRules(context, TileProperty(context, "ruleTiles"), rules)) return false;
			Image *map =
				context.NewImage("tilemap", source->Width, source->Height, SurfaceFormat::RGBA16Float);
			if (!map ||
				!TileApplyRules(context, *tileset, rules, *source, *map, float(context.Scalar("seed"))))
				return false;
			if (!TileRenderMap(context, *tileset, *map)) return false;
			context.SetValue("tileset", *std::get_if<TilesetValue>(context.Find("input_0")));
			return context.FailureCode == Status::Ok;
		}
		Rgba TilePackedColour(uint32_t colour) {
			return {
				double(colour & 255) / 255,
				double((colour >> 8) & 255) / 255,
				double((colour >> 16) & 255) / 255,
				1
			};
		}
		bool TileTerrainPass(
			NodeContext &context,
			const TileTerrainData &terrain,
			const Image &draw,
			Image &map,
			Image &mask,
			Image &next
		) {
			for (uint32_t y = 0; y < map.Height; y++)
				for (uint32_t x = 0; x < map.Width; x++) {
					const Rgba base = ReadPixel(map, x, y), drawing = ReadPixel(draw, x, y);
					const float raw = float(base[0]) - 1.f;
					if (double(raw) < INT32_MIN || double(raw) > INT32_MAX)
						return context.Fail(
							Status::UnsupportedExecution,
							"terrain mask tile ID exceeds shader integer range",
							"tilemap"
						);
					const int32_t id = int32_t(raw);
					double membership = 0;
					for (int32_t index : terrain.Indices)
						if (index != -1 && id == index) membership = .5;
					if (!WritePixel(
							mask,
							x,
							y,
							{std::max(membership, std::clamp(drawing[0] * drawing[3], 0.0, 1.0)), 0, 0, 0}
						))
						return context.Fail(Status::InvalidValue, "terrain mask packing failed", "tilemap");
				}
			for (uint32_t y = 0; y < map.Height; y++)
				for (uint32_t x = 0; x < map.Width; x++) {
					const float u = (float(x) + .5f) / float(map.Width),
								v = (float(y) + .5f) / float(map.Height);
					std::array<int, 9> neighbors{};
					float maximum = 0, center = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							const float m = float(SampleNearest(
								mask, u + float(dx) / float(map.Width), v + float(dy) / float(map.Height)
							)[0]);
							neighbors[size_t((dy + 1) * 3 + dx + 1)] = int(std::ceil(m));
							maximum = std::max(maximum, m);
							if (dx == 0 && dy == 0) center = m;
						}
					Rgba pixel = ReadPixel(map, x, y);
					if (center != 0 && maximum >= 1 && pixel[0] > 0) {
						const int index = SourceTerrainIndex(terrain.Type, neighbors);
						if (index < 0 || size_t(index) >= terrain.Indices.size())
							return context.Fail(
								Status::UnsupportedExecution,
								"source terrain layout reads an index that was never uploaded",
								"autoterrain"
							);
						pixel = {double(terrain.Indices[size_t(index)]) + 1, 0, 0, 1};
					}
					if (!WritePixel(next, x, y, pixel))
						return context.Fail(Status::InvalidValue, "terrain output packing failed", "tilemap");
				}
			std::swap(map, next);
			return true;
		}
		bool TileConvert(NodeContext &context) {
			const TilesetData *tileset = TileResource(context, "input_1");
			const Image *source = context.Input("surface");
			context.SetSourceUpdateOnFrame(context.Boolean("animated"));
			if (!tileset || !TileSurface(context, source, "surface")) return false;
			const auto layout = CheckedSurfaceLayout(
						   source->Width,
						   source->Height,
						   SurfaceFormat::RGBA16Float,
						   Limits::MaximumOutputBytes
					   ),
					   maskLayout = CheckedSurfaceLayout(
						   source->Width, source->Height, SurfaceFormat::R8Unorm, Limits::MaximumOutputBytes
					   );
			if (!layout || !maskLayout)
				return context.Fail(
					Status::LimitExceeded, "tile conversion dimensions exceed scratch bounds", "surface"
				);
			auto charge = context.ReserveWorkspace(
				MeshAddBytes(
					layout->Bytes * 3 + maskLayout->Bytes, MeshAddBytes(TilePropertyBytes(context) * 4, 4096)
				),
				"surface"
			);
			if (!charge) return false;
			std::vector<TileConversionEntry> mapping;
			if (!ReadTileConversions(context, mapping)) return false;
			Image temp[2], next, mask;
			if (!TileScratch(temp[0], source->Width, source->Height, SurfaceFormat::RGBA16Float) ||
				!TileScratch(temp[1], source->Width, source->Height, SurfaceFormat::RGBA16Float) ||
				!TileScratch(next, source->Width, source->Height, SurfaceFormat::RGBA16Float) ||
				!TileScratch(mask, source->Width, source->Height, SurfaceFormat::R8Unorm) ||
				!TileCopy(*source, temp[1]))
				return context.Fail(Status::InvalidValue, "tile conversion scratch is invalid", "surface");
			bool plain = false;
			uint64_t work = 0;
			for (const auto &entry : mapping) {
				if (!entry.Present) continue;
				if (!entry.Target.Terrain) {
					plain = true;
					continue;
				}
				if (entry.Target.Index < 0 || entry.Target.Index >= double(tileset->Terrains.size()) ||
					std::trunc(entry.Target.Index) != entry.Target.Index)
					continue;
				const auto &terrain = tileset->Terrains[size_t(entry.Target.Index)];
				const uint64_t cost = 12 + terrain.Indices.size();
				const uint64_t pixels = uint64_t(source->Width) * source->Height;
				if (pixels > (64'000'000 - work) / cost)
					return context.Fail(
						Status::LimitExceeded,
						"terrain conversion exceeds the bounded sample count",
						"autoterrain"
					);
				work += pixels * cost;
				const double preview =
					terrain.PreviewIndex < terrain.Indices.size() ? terrain.Indices[terrain.PreviewIndex] : 0;
				const Rgba target = TilePackedColour(entry.Colour);
				for (uint32_t y = 0; y < source->Height; y++)
					for (uint32_t x = 0; x < source->Width; x++) {
						Rgba pixel = ReadPixel(temp[1], x, y);
						const float dr = float(pixel[0]) - float(target[0]),
									dg = float(pixel[1]) - float(target[1]),
									db = float(pixel[2]) - float(target[2]);
						if (pixel[3] != 2 && std::sqrt(dr * dr + dg * dg + db * db) < .01f)
							pixel = {preview + 1, 0, 0, 2};
						if (!WritePixel(temp[0], x, y, pixel))
							return context.Fail(
								Status::InvalidValue, "conversion terrain mask failed", "surface"
							);
					}
				if (!TileCopy(temp[0], temp[1]) ||
					!TileTerrainPass(context, terrain, temp[0], temp[1], mask, next))
					return false;
			}
			// The source flips bg after the plain shader and returns temp[!bg], retaining
			// the pre-corner surface instead of the shader result. Keep that observable ordering.
			const Image &selected = temp[plain ? 0 : 1];
			Image *map =
				context.NewImage("tilemap", source->Width, source->Height, SurfaceFormat::RGBA16Float);
			if (!map || !TileApplyRules(
							context, *tileset, tileset->Rules, selected, *map, float(context.Scalar("seed"))
						))
				return false;
			if (!TileRenderMap(context, *tileset, *map)) return false;
			context.SetValue("tileset", *std::get_if<TilesetValue>(context.Find("input_1")));
			return context.FailureCode == Status::Ok;
		}
	}
	std::span<const ExecutorEntry> SourceTileExecutors() {
		static constexpr ExecutorEntry executors[]{
			{"pc.tile_tileset", TileSet, true},
			{"pc.tile_render", TileRender, true},
			{"pc.tile_rule", TileRule, true},
			{"pc.tile_convert", TileConvert, true}
		};
		return executors;
	}
}
