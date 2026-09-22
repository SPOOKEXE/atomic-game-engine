#include <engine/render/ShapedGlyphAtlas.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <imgui_internal.h>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine::ui {

	namespace {
		using gui::DrawCommand;
		using gui::DrawKind;
		using gui::FontFace;
		using gui::ScaleType;
		using gui::TextXAlignment;
		using gui::TextYAlignment;

		struct GlyphKey {
			core::Name Face;
			uint32_t Index = 0;
			uint16_t Size = 0;
			bool operator==(const GlyphKey &) const = default;
		};
		struct GlyphKeyHash {
			size_t operator()(const GlyphKey &key) const {
				return (size_t(key.Face.Id()) * 65599u + key.Index) * 257u + key.Size;
			}
		};
		struct CachedGlyph {
			render::ShapedAtlasGlyph Glyph;
			uint16_t X = 0;
			uint16_t Y = 0;
			bool Visible = false;
		};
		struct GlyphCache {
			static constexpr size_t MAXIMUM_GLYPHS = 1024;
			ImFontAtlas *Owner = nullptr;
			uint64_t Package = 0;
			uint64_t Use = 0;
			int Frame = -1;
			bool ResetOnNextFrame = false;
			uint16_t NextX = 1;
			uint16_t NextY = 1;
			uint16_t RowHeight = 0;
			render::ShapedGlyphAtlas Atlas{16.0f};
			std::unordered_map<GlyphKey, CachedGlyph, GlyphKeyHash> Entries;
		};

		void ResetGlyphPacking(GlyphCache &cache) {
			cache.NextX = 1;
			cache.NextY = 1;
			cache.RowHeight = 0;
		}

		GlyphCache &Glyphs(const gui::FontPackage &fonts) {
			static std::unordered_map<ImGuiContext *, GlyphCache> caches;
			if (caches.find(ImGui::GetCurrentContext()) == caches.end() && caches.size() >= 8) caches.clear();
			GlyphCache &cache = caches[ImGui::GetCurrentContext()];
			ImFontAtlas *owner = ImGui::GetIO().Fonts;
			if (cache.Owner != owner) {
				cache = GlyphCache{};
				cache.Owner = owner;
			}
			const int frame = ImGui::GetFrameCount();
			if (cache.ResetOnNextFrame && cache.Frame != frame) {
				// The replacement package was first used after old vertices were
				// recorded. Its temporary placements cannot survive a new frame: the
				// allocator is about to reuse them, so discard their lookup entries
				// and coverage together.
				ResetGlyphPacking(cache);
				cache.Atlas.Clear();
				cache.Entries.clear();
				cache.ResetOnNextFrame = false;
			}
			const uint64_t signature = fonts.Signature();
			if (cache.Package != signature) {
				// A package replacement can happen while the current frame still
				// references its glyph pixels. Keep its allocations until the next
				// frame, when its submitted draw data can no longer use them.
				if (cache.Frame == frame) {
					cache.ResetOnNextFrame = true;
				} else {
					ResetGlyphPacking(cache);
					cache.ResetOnNextFrame = false;
				}
				cache.Package = signature;
				cache.Atlas.Clear();
				cache.Entries.clear();
			}
			cache.Frame = frame;
			return cache;
		}

		bool AllocateGlyphRect(
			GlyphCache &cache,
			const ImFontAtlasRect &region,
			uint16_t width,
			uint16_t height,
			uint16_t &x,
			uint16_t &y
		) {
			constexpr uint16_t padding = 1;
			if (width == 0 || height == 0 || width + padding * 2 > region.w ||
				height + padding * 2 > region.h) {
				return false;
			}
			const uint16_t occupiedWidth = static_cast<uint16_t>(width + padding * 2);
			const uint16_t occupiedHeight = static_cast<uint16_t>(height + padding * 2);
			if (cache.NextX + occupiedWidth > region.w) {
				cache.NextX = padding;
				cache.NextY = static_cast<uint16_t>(cache.NextY + cache.RowHeight);
				cache.RowHeight = 0;
			}
			if (cache.NextY + occupiedHeight > region.h) {
				return false;
			}
			x = static_cast<uint16_t>(region.x + cache.NextX + padding);
			y = static_cast<uint16_t>(region.y + cache.NextY + padding);
			cache.NextX = static_cast<uint16_t>(cache.NextX + occupiedWidth);
			cache.RowHeight = std::max(cache.RowHeight, occupiedHeight);
			return true;
		}

		void QueueGlyphUpload(ImFontAtlas *atlas, int x, int y, int width, int height) {
			ImTextureData *texture = atlas->TexData;
			if (!atlas->RendererHasTextures || texture == nullptr ||
				texture->Status == ImTextureStatus_WantCreate) {
				return;
			}
			if (texture->Status != ImTextureStatus_OK && texture->Status != ImTextureStatus_WantUpdates) {
				return;
			}
			texture->Updates.push_back(
				ImTextureRect{
					static_cast<unsigned short>(x),
					static_cast<unsigned short>(y),
					static_cast<unsigned short>(width),
					static_cast<unsigned short>(height)
				}
			);
			if (texture->UpdateRect.w == 0 || texture->UpdateRect.h == 0) {
				texture->UpdateRect = texture->Updates.back();
			} else {
				const int right = std::max<int>(texture->UpdateRect.x + texture->UpdateRect.w, x + width);
				const int bottom = std::max<int>(texture->UpdateRect.y + texture->UpdateRect.h, y + height);
				texture->UpdateRect.x = std::min<int>(texture->UpdateRect.x, x);
				texture->UpdateRect.y = std::min<int>(texture->UpdateRect.y, y);
				texture->UpdateRect.w = static_cast<unsigned short>(right - texture->UpdateRect.x);
				texture->UpdateRect.h = static_cast<unsigned short>(bottom - texture->UpdateRect.y);
			}
			texture->SetStatus(ImTextureStatus_WantUpdates);
		}

		bool GlyphUv(const CachedGlyph &glyph, ImFontAtlasRect &out) {
			ImFontAtlasRect region;
			if (!glyph.Visible || !ShapedGlyphAtlasRect(&region) || region.w == 0 || region.h == 0) {
				return false;
			}
			const float scaleX = (region.uv1.x - region.uv0.x) / static_cast<float>(region.w);
			const float scaleY = (region.uv1.y - region.uv0.y) / static_cast<float>(region.h);
			const uint16_t localX = static_cast<uint16_t>(glyph.X - region.x);
			const uint16_t localY = static_cast<uint16_t>(glyph.Y - region.y);
			out.x = glyph.X;
			out.y = glyph.Y;
			out.w = glyph.Glyph.Width;
			out.h = glyph.Glyph.Height;
			out.uv0 = ImVec2{region.uv0.x + localX * scaleX, region.uv0.y + localY * scaleY};
			out.uv1 = ImVec2{out.uv0.x + glyph.Glyph.Width * scaleX, out.uv0.y + glyph.Glyph.Height * scaleY};
			return true;
		}

		const CachedGlyph *ResolveGlyph(const gui::FontPackage &fonts, const gui::ShapedGlyph &glyph) {
			if (glyph.Face.IsValid() == false || !std::isfinite(glyph.PixelSize) || glyph.PixelSize <= 0.0f ||
				glyph.PixelSize > gui::MAXIMUM_SHAPED_PIXEL_SIZE)
				return nullptr;
			GlyphCache &cache = Glyphs(fonts);
			const GlyphKey key{glyph.Face, glyph.Index, static_cast<uint16_t>(std::lround(glyph.PixelSize))};
			if (const auto found = cache.Entries.find(key); found != cache.Entries.end())
				return &found->second;
			if (cache.Entries.size() >= GlyphCache::MAXIMUM_GLYPHS) return nullptr;
			ImFontAtlas *atlas = ImGui::GetIO().Fonts;
			ImFontAtlasRect region;
			if (atlas == nullptr || atlas->TexData == nullptr || !ShapedGlyphAtlasRect(&region))
				return nullptr;
			// Refuse before rasterisation. A hostile document can request the
			// shaper's allowed maximum size, but no single glyph may consume the
			// fixed editor atlas or force a giant temporary coverage bitmap.
			if (glyph.PixelSize + 2.0f > std::min(region.w, region.h)) return nullptr;
			const std::array<gui::ShapedGlyph, 1> one{glyph};
			const std::vector<render::ShapedAtlasGlyph> resolved =
				cache.Atlas.Resolve(fonts, one, ++cache.Use);
			if (resolved.empty() || !resolved.front().Present) return nullptr;
			const render::ShapedAtlasGlyph source = resolved.front();
			if (source.Width == 0 || source.Height == 0) {
				return &cache.Entries.emplace(key, CachedGlyph{.Glyph = source}).first->second;
			}
			uint16_t destinationX = 0;
			uint16_t destinationY = 0;
			if (!AllocateGlyphRect(cache, region, source.Width, source.Height, destinationX, destinationY))
				return nullptr;
			const std::vector<uint8_t> &coverage = cache.Atlas.Coverage(source.Page);
			if (coverage.empty() || atlas->TexData->BytesPerPixel != 4) return nullptr;
			for (uint16_t glyphY = 0; glyphY < source.Height; glyphY++) {
				for (uint16_t glyphX = 0; glyphX < source.Width; glyphX++) {
					const uint8_t alpha = coverage
						[(size_t(source.Y + glyphY) * render::ShapedGlyphAtlas::PAGE_EXTENT) + source.X +
						 glyphX];
					auto *pixel = static_cast<uint8_t *>(
						atlas->TexData->GetPixelsAt(destinationX + glyphX, destinationY + glyphY)
					);
					pixel[0] = pixel[1] = pixel[2] = pixel[3] = alpha;
				}
			}
			QueueGlyphUpload(atlas, destinationX, destinationY, source.Width, source.Height);
			return &cache.Entries
						.emplace(
							key,
							CachedGlyph{
								.Glyph = source, .X = destinationX, .Y = destinationY, .Visible = true
							}
						)
						.first->second;
		}

		void Quad(ImDrawList *into, const ImFontAtlasRect &uv, ImVec2 min, ImVec2 max, ImU32 colour) {
			into->PrimReserve(6, 4);
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx));
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx + 1));
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx + 2));
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx));
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx + 2));
			into->PrimWriteIdx(static_cast<ImDrawIdx>(into->_VtxCurrentIdx + 3));
			into->PrimWriteVtx(min, uv.uv0, colour);
			into->PrimWriteVtx(ImVec2{max.x, min.y}, ImVec2{uv.uv1.x, uv.uv0.y}, colour);
			into->PrimWriteVtx(max, uv.uv1, colour);
			into->PrimWriteVtx(ImVec2{min.x, max.y}, ImVec2{uv.uv0.x, uv.uv1.y}, colour);
		}

		// Which vendored face a `gui::FontFace` asks for.
		//
		// **A mapping and not a rename.** `gui::FontFace` is a `shared` enum a
		// game file carries; `ui::Typeface` is what this module vendored. Four
		// of one onto three of the other, with `Italic` falling back to the
		// interface face - because the vendored families are variable fonts used
		// at their default instance, which for all four is upright regular, and
		// synthesising a slant is a rasteriser feature stb_truetype does not
		// have. An author asking for italic gets upright text rather than
		// nothing, and this comment is why.
		Typeface FaceFor(FontFace font) {
			switch (font) {
			case FontFace::Code:
				return Typeface::Monospace;
			case FontFace::Bold:
				return Typeface::Display;
			case FontFace::Regular:
			case FontFace::Italic:
				return Typeface::Interface;
			}
			return Typeface::Interface;
		}

		ImU32 Colour(const core::Color3 &tint, float transparency) {
			const float alpha = std::clamp(1.0f - transparency, 0.0f, 1.0f);
			return ImGui::GetColorU32(ImVec4{tint.R, tint.G, tint.B, alpha});
		}

		struct GradientSpace {
			ImVec2 Origin;
			float Scale = 1.0f;

			ImVec2 Point(const core::Vector2 &value) const {
				return ImVec2{Origin.x + value.X * Scale, Origin.y + value.Y * Scale};
			}
		};

		struct ActiveMask {
			ecs::Entity Source;
			ecs::Entity Collector;
			core::Rect Bounds;
			float Radius = 0.0f;
		};

		constexpr size_t MAXIMUM_STUDIO_MASKS = 8;
		constexpr size_t MAXIMUM_MASK_CLIP_VERTICES = 8192;
		constexpr int MASK_ARC_SEGMENTS = 12;
		constexpr int MAXIMUM_GROUP_TARGET_EXTENT = 2048;
		constexpr size_t MAXIMUM_GROUP_TARGET_BYTES = 16 * 1024 * 1024;
		constexpr size_t MAXIMUM_STUDIO_GROUP_TARGETS = 8;
		constexpr size_t MAXIMUM_STUDIO_GROUP_TARGET_BYTES = 64 * 1024 * 1024;

		core::Rect Intersect(const core::Rect &left, const core::Rect &right) {
			return {
				{std::max(left.Min.X, right.Min.X), std::max(left.Min.Y, right.Min.Y)},
				{std::min(left.Max.X, right.Max.X), std::min(left.Max.Y, right.Max.Y)},
			};
		}

		bool Empty(const core::Rect &rect) {
			return !std::isfinite(rect.Min.X) || !std::isfinite(rect.Min.Y) || !std::isfinite(rect.Max.X) ||
				   !std::isfinite(rect.Max.Y) || rect.Max.X <= rect.Min.X || rect.Max.Y <= rect.Min.Y;
		}

		struct GroupTexture {
			uint64_t Key = 0;
			uint64_t Identity = 0;
			uint64_t Use = 0;
			bool Valid = false;
			std::unique_ptr<ImTextureData> Texture;
		};
		struct GroupTextureStore {
			size_t Bytes = 0;
			uint64_t Uses = 0;
			std::deque<GroupTexture> Textures;
		};
		using GroupTextureStores = std::unordered_map<ImGuiContext *, GroupTextureStore>;

		GroupTextureStores &AllGroupTextures() {
			static GroupTextureStores stores;
			return stores;
		}

		GroupTextureStore &GroupTextures() {
			return AllGroupTextures()[ImGui::GetCurrentContext()];
		}

		uint64_t FoldGroupKey(uint64_t value, uint64_t part) {
			return (value ^ part) * 1099511628211ull;
		}

		GroupTexture *CreateGroupTexture(uint64_t key, uint64_t identity, int width, int height) {
			if (width <= 0 || height <= 0 || width > MAXIMUM_GROUP_TARGET_EXTENT ||
				height > MAXIMUM_GROUP_TARGET_EXTENT ||
				static_cast<size_t>(width) > MAXIMUM_GROUP_TARGET_BYTES / (static_cast<size_t>(height) * 4)) {
				return nullptr;
			}
			GroupTextureStore &store = GroupTextures();
			for (GroupTexture &entry : store.Textures) {
				if (entry.Key == key && entry.Texture->Width == width && entry.Texture->Height == height) {
					entry.Use = ++store.Uses;
					return &entry;
				}
			}
			const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
			while (!store.Textures.empty() && (store.Textures.size() >= MAXIMUM_STUDIO_GROUP_TARGETS ||
											   bytes > MAXIMUM_STUDIO_GROUP_TARGET_BYTES - store.Bytes)) {
				auto oldest = std::min_element(
					store.Textures.begin(),
					store.Textures.end(),
					[](const GroupTexture &left, const GroupTexture &right) { return left.Use < right.Use; }
				);
				store.Bytes -= static_cast<size_t>(oldest->Texture->Width) * oldest->Texture->Height * 4;
				ImGui::UnregisterUserTexture(oldest->Texture.get());
				store.Textures.erase(oldest);
			}
			if (bytes > MAXIMUM_STUDIO_GROUP_TARGET_BYTES - store.Bytes) return nullptr;
			auto texture = std::make_unique<ImTextureData>();
			texture->Create(ImTextureFormat_RGBA32, width, height);
			texture->UseColors = true;
			ImGui::RegisterUserTexture(texture.get());
			store.Textures.push_back(GroupTexture{key, identity, ++store.Uses, false, std::move(texture)});
			store.Bytes += bytes;
			return &store.Textures.back();
		}

		GroupTexture *PriorGroupTexture(uint64_t identity, int width, int height) {
			GroupTextureStore &store = GroupTextures();
			for (GroupTexture &entry : store.Textures) {
				if (entry.Identity == identity && entry.Valid && entry.Texture->Width == width &&
					entry.Texture->Height == height) {
					entry.Use = ++store.Uses;
					return &entry;
				}
			}
			return nullptr;
		}

		float Channel(ImU32 colour, int shift) {
			return static_cast<float>((colour >> shift) & 0xFFu) / 255.0f;
		}

		bool SampleTexture(
			const ImTextureRef &reference,
			const ImVec2 &uv,
			const ImageSource &images,
			float &red,
			float &green,
			float &blue,
			float &alpha
		) {
			const ImTextureData *texture = reference._TexData;
			// Scratch draw lists inherit the current font texture by its raw id.
			// Before the backend's first upload that id is invalid, while the atlas
			// pixels are already available, so recover the known atlas explicitly.
			if (texture == nullptr && ImGui::GetIO().Fonts != nullptr &&
				ImGui::GetIO().Fonts->TexData != nullptr &&
				(reference._TexID == ImTextureID_Invalid ||
				 reference._TexID == ImGui::GetIO().Fonts->TexData->TexID)) {
				texture = ImGui::GetIO().Fonts->TexData;
			}
			if (texture == nullptr || texture->Pixels == nullptr ||
				texture->Format != ImTextureFormat_RGBA32 || texture->Width <= 0 || texture->Height <= 0) {
				// Content images use the renderer's raw device handles, which ImGui
				// cannot read on the CPU. The Studio owner may provide a bounded
				// retained source copy for the group-only raster path.
				return images.Sample && images.Sample(reference._TexID, uv, red, green, blue, alpha);
			}
			const int x = std::clamp(static_cast<int>(uv.x * texture->Width), 0, texture->Width - 1);
			const int y = std::clamp(static_cast<int>(uv.y * texture->Height), 0, texture->Height - 1);
			const uint8_t *pixel = texture->Pixels + (y * texture->Width + x) * 4;
			red = static_cast<float>(pixel[0]) / 255.0f;
			green = static_cast<float>(pixel[1]) / 255.0f;
			blue = static_cast<float>(pixel[2]) / 255.0f;
			alpha = static_cast<float>(pixel[3]) / 255.0f;
			return true;
		}

		// Rasterises the emitted ImGui triangles into a small straight-alpha image.
		// The resulting texture is composited once by the parent draw list, which
		// preserves CanvasGroup overlap semantics without requiring a second GPU pass.
		bool RasterGroup(
			const ImDrawList &source,
			const core::Rect &bounds,
			ImTextureData &destination,
			const ImageSource &images
		) {
			struct Pixel {
				float Red = 0.0f;
				float Green = 0.0f;
				float Blue = 0.0f;
				float Alpha = 0.0f;
			};
			std::vector<Pixel> pixels(static_cast<size_t>(destination.Width) * destination.Height);
			const auto edge = [](const ImVec2 &a, const ImVec2 &b, const ImVec2 &point) {
				return (point.x - a.x) * (b.y - a.y) - (point.y - a.y) * (b.x - a.x);
			};
			const auto ownsEdge = [](const ImVec2 &a, const ImVec2 &b) {
				// Adjacent triangles traverse their shared edge in opposite directions.
				// Giving only one direction ownership matches GPU top-left coverage and
				// prevents a translucent diagonal from compositing twice.
				return a.y < b.y || (a.y == b.y && a.x > b.x);
			};
			const auto covers = [&](float weight, const ImVec2 &a, const ImVec2 &b) {
				return weight > 0.00001f || (std::abs(weight) <= 0.00001f && ownsEdge(a, b));
			};
			for (const ImDrawCmd &command : source.CmdBuffer) {
				if (command.UserCallback != nullptr || command.ElemCount % 3 != 0) return false;
				for (unsigned int index = 0; index < command.ElemCount; index += 3) {
					const unsigned int first =
						command.VtxOffset + source.IdxBuffer[command.IdxOffset + index];
					const unsigned int second =
						command.VtxOffset + source.IdxBuffer[command.IdxOffset + index + 1];
					const unsigned int third =
						command.VtxOffset + source.IdxBuffer[command.IdxOffset + index + 2];
					if (first >= static_cast<unsigned int>(source.VtxBuffer.Size) ||
						second >= static_cast<unsigned int>(source.VtxBuffer.Size) ||
						third >= static_cast<unsigned int>(source.VtxBuffer.Size))
						return false;
					const std::array<ImDrawVert, 3> triangle{
						{source.VtxBuffer[first], source.VtxBuffer[second], source.VtxBuffer[third]}
					};
					const float area = edge(triangle[0].pos, triangle[1].pos, triangle[2].pos);
					if (std::abs(area) < 0.00001f) continue;
					const float left = std::max(
						{bounds.Min.X,
						 command.ClipRect.x,
						 std::min({triangle[0].pos.x, triangle[1].pos.x, triangle[2].pos.x})}
					);
					const float top = std::max(
						{bounds.Min.Y,
						 command.ClipRect.y,
						 std::min({triangle[0].pos.y, triangle[1].pos.y, triangle[2].pos.y})}
					);
					const float right = std::min(
						{bounds.Max.X,
						 command.ClipRect.z,
						 std::max({triangle[0].pos.x, triangle[1].pos.x, triangle[2].pos.x})}
					);
					const float bottom = std::min(
						{bounds.Max.Y,
						 command.ClipRect.w,
						 std::max({triangle[0].pos.y, triangle[1].pos.y, triangle[2].pos.y})}
					);
					for (int y = std::max(0, static_cast<int>(std::floor(top - bounds.Min.Y)));
						 y < std::min(destination.Height, static_cast<int>(std::ceil(bottom - bounds.Min.Y)));
						 y++) {
						for (int x = std::max(0, static_cast<int>(std::floor(left - bounds.Min.X)));
							 x <
							 std::min(destination.Width, static_cast<int>(std::ceil(right - bounds.Min.X)));
							 x++) {
							const ImVec2 point{
								bounds.Min.X + static_cast<float>(x) + 0.5f,
								bounds.Min.Y + static_cast<float>(y) + 0.5f
							};
							const float a = edge(triangle[1].pos, triangle[2].pos, point) / area;
							const float b = edge(triangle[2].pos, triangle[0].pos, point) / area;
							const float c = 1.0f - a - b;
							if (!covers(a, triangle[1].pos, triangle[2].pos) ||
								!covers(b, triangle[2].pos, triangle[0].pos) ||
								!covers(c, triangle[0].pos, triangle[1].pos)) {
								continue;
							}
							const ImVec2 uv{
								a * triangle[0].uv.x + b * triangle[1].uv.x + c * triangle[2].uv.x,
								a * triangle[0].uv.y + b * triangle[1].uv.y + c * triangle[2].uv.y,
							};
							float sampleRed = 0.0f;
							float sampleGreen = 0.0f;
							float sampleBlue = 0.0f;
							float sampleAlpha = 0.0f;
							if (!SampleTexture(
									command.TexRef,
									uv,
									images,
									sampleRed,
									sampleGreen,
									sampleBlue,
									sampleAlpha
								))
								return false;
							const float opacity =
								sampleAlpha * (a * Channel(triangle[0].col, IM_COL32_A_SHIFT) +
											   b * Channel(triangle[1].col, IM_COL32_A_SHIFT) +
											   c * Channel(triangle[2].col, IM_COL32_A_SHIFT));
							const float red = sampleRed * (a * Channel(triangle[0].col, IM_COL32_R_SHIFT) +
														   b * Channel(triangle[1].col, IM_COL32_R_SHIFT) +
														   c * Channel(triangle[2].col, IM_COL32_R_SHIFT));
							const float green =
								sampleGreen * (a * Channel(triangle[0].col, IM_COL32_G_SHIFT) +
											   b * Channel(triangle[1].col, IM_COL32_G_SHIFT) +
											   c * Channel(triangle[2].col, IM_COL32_G_SHIFT));
							const float blue = sampleBlue * (a * Channel(triangle[0].col, IM_COL32_B_SHIFT) +
															 b * Channel(triangle[1].col, IM_COL32_B_SHIFT) +
															 c * Channel(triangle[2].col, IM_COL32_B_SHIFT));
							Pixel &pixel = pixels[static_cast<size_t>(y) * destination.Width + x];
							pixel.Red = red * opacity + pixel.Red * (1.0f - opacity);
							pixel.Green = green * opacity + pixel.Green * (1.0f - opacity);
							pixel.Blue = blue * opacity + pixel.Blue * (1.0f - opacity);
							pixel.Alpha = opacity + pixel.Alpha * (1.0f - opacity);
						}
					}
				}
			}
			for (size_t index = 0; index < pixels.size(); index++) {
				const Pixel &pixel = pixels[index];
				uint8_t *out = destination.Pixels + index * 4;
				const float inverse = pixel.Alpha > 0.0f ? 1.0f / pixel.Alpha : 0.0f;
				out[0] = static_cast<uint8_t>(std::clamp(pixel.Red * inverse * 255.0f + 0.5f, 0.0f, 255.0f));
				out[1] =
					static_cast<uint8_t>(std::clamp(pixel.Green * inverse * 255.0f + 0.5f, 0.0f, 255.0f));
				out[2] = static_cast<uint8_t>(std::clamp(pixel.Blue * inverse * 255.0f + 0.5f, 0.0f, 255.0f));
				out[3] = static_cast<uint8_t>(std::clamp(pixel.Alpha * 255.0f + 0.5f, 0.0f, 255.0f));
			}
			return true;
		}

		ImDrawVert BlendVertex(const ImDrawVert &left, const ImDrawVert &right, float amount) {
			const auto blend = [amount](float a, float b) { return a + (b - a) * amount; };
			const auto channel = [amount](ImU32 colour, int shift, ImU32 other) {
				const float a = static_cast<float>((colour >> shift) & 0xFFu);
				const float b = static_cast<float>((other >> shift) & 0xFFu);
				return static_cast<ImU32>(std::clamp(a + (b - a) * amount + 0.5f, 0.0f, 255.0f)) << shift;
			};
			return {
				{blend(left.pos.x, right.pos.x), blend(left.pos.y, right.pos.y)},
				{blend(left.uv.x, right.uv.x), blend(left.uv.y, right.uv.y)},
				channel(left.col, IM_COL32_R_SHIFT, right.col) |
					channel(left.col, IM_COL32_G_SHIFT, right.col) |
					channel(left.col, IM_COL32_B_SHIFT, right.col) |
					channel(left.col, IM_COL32_A_SHIFT, right.col),
			};
		}

		std::vector<ImVec2> RoundedPolygon(const ActiveMask &mask, const GradientSpace &space) {
			const ImVec2 min = space.Point(mask.Bounds.Min);
			const ImVec2 max = space.Point(mask.Bounds.Max);
			const float extent = std::min(max.x - min.x, max.y - min.y);
			const float radius = std::clamp(mask.Radius * space.Scale, 0.0f, extent * 0.5f);
			if (!(radius > 0.0f) || !std::isfinite(radius)) return {};

			std::vector<ImVec2> polygon;
			polygon.reserve(MASK_ARC_SEGMENTS * 4);
			const std::array<ImVec2, 4> centres{{
				{max.x - radius, min.y + radius},
				{max.x - radius, max.y - radius},
				{min.x + radius, max.y - radius},
				{min.x + radius, min.y + radius},
			}};
			for (size_t corner = 0; corner < centres.size(); corner++) {
				const float start =
					-1.57079632679489661923f + static_cast<float>(corner) * 1.57079632679489661923f;
				for (int segment = 0; segment < MASK_ARC_SEGMENTS; segment++) {
					const float angle =
						start + static_cast<float>(segment) * 1.57079632679489661923f / MASK_ARC_SEGMENTS;
					polygon.push_back(
						{centres[corner].x + std::cos(angle) * radius,
						 centres[corner].y + std::sin(angle) * radius}
					);
				}
			}
			return polygon;
		}

		float Cross(const ImVec2 &first, const ImVec2 &second, const ImVec2 &point) {
			return (second.x - first.x) * (point.y - first.y) - (second.y - first.y) * (point.x - first.x);
		}

		bool Inside(const ImDrawVert &vertex, const std::vector<ImVec2> &polygon) {
			for (size_t edge = 0; edge < polygon.size(); edge++) {
				if (Cross(polygon[edge], polygon[(edge + 1) % polygon.size()], vertex.pos) < -0.001f)
					return false;
			}
			return true;
		}

		std::vector<ImDrawVert>
		ClipTriangle(const std::array<ImDrawVert, 3> &triangle, const std::vector<ImVec2> &polygon) {
			std::vector<ImDrawVert> input(triangle.begin(), triangle.end());
			std::vector<ImDrawVert> output;
			for (size_t edge = 0; edge < polygon.size() && !input.empty(); edge++) {
				output.clear();
				const ImVec2 first = polygon[edge];
				const ImVec2 second = polygon[(edge + 1) % polygon.size()];
				for (size_t index = 0; index < input.size(); index++) {
					const ImDrawVert &from = input[index];
					const ImDrawVert &to = input[(index + 1) % input.size()];
					const float fromSide = Cross(first, second, from.pos);
					const float toSide = Cross(first, second, to.pos);
					const bool fromInside = fromSide >= -0.001f;
					const bool toInside = toSide >= -0.001f;
					if (fromInside) output.push_back(from);
					if (fromInside != toInside) {
						const float denominator = fromSide - toSide;
						if (std::abs(denominator) > 0.000001f)
							output.push_back(BlendVertex(from, to, fromSide / denominator));
					}
				}
				input.swap(output);
			}
			return input;
		}

		// Dear ImGui exposes no stencil or transient target through an ImDrawList.
		// Clip the just-recorded triangles against a bounded rounded polygon instead,
		// preserving texture coordinates and alpha at every new edge vertex. The
		// rectangular scissor already installed by the caller stays as the safe
		// fallback when this small editor-side mesh budget is exhausted.
		bool ClipRoundedMaskGeometry(
			ImDrawList *into,
			int firstCommand,
			int firstIndex,
			const std::vector<ActiveMask> &masks,
			const GradientSpace &space
		) {
			if (masks.empty() || firstIndex >= into->IdxBuffer.Size) return true;
			std::vector<std::vector<ImVec2>> polygons;
			for (const ActiveMask &mask : masks) {
				std::vector<ImVec2> polygon = RoundedPolygon(mask, space);
				if (!polygon.empty()) polygons.push_back(std::move(polygon));
			}
			if (polygons.empty()) return true;

			const int endIndex = into->IdxBuffer.Size;
			const int endCommand = into->CmdBuffer.Size;
			if (into->VtxBuffer.Size > static_cast<int>(std::numeric_limits<ImDrawIdx>::max()) -
										   static_cast<int>(MAXIMUM_MASK_CLIP_VERTICES))
				return false;

			std::vector<ImDrawIdx> source(into->IdxBuffer.Data + firstIndex, into->IdxBuffer.Data + endIndex);
			std::vector<ImDrawVert> generated;
			std::vector<ImDrawIdx> clipped;
			generated.reserve(256);
			clipped.reserve(source.size());
			std::vector<unsigned int> counts(static_cast<size_t>(endCommand - firstCommand));

			for (int commandIndex = firstCommand; commandIndex < endCommand; commandIndex++) {
				ImDrawCmd &command = into->CmdBuffer[commandIndex];
				const size_t countIndex = static_cast<size_t>(commandIndex - firstCommand);
				if (command.UserCallback != nullptr || command.ElemCount == 0) continue;
				const int offset = static_cast<int>(command.IdxOffset) - firstIndex;
				if (offset < 0 ||
					offset + static_cast<int>(command.ElemCount) > static_cast<int>(source.size()))
					return false;
				for (unsigned int index = 0; index + 2 < command.ElemCount; index += 3) {
					const unsigned int first =
						command.VtxOffset + source[static_cast<size_t>(offset) + index];
					const unsigned int second =
						command.VtxOffset + source[static_cast<size_t>(offset) + index + 1];
					const unsigned int third =
						command.VtxOffset + source[static_cast<size_t>(offset) + index + 2];
					if (first >= static_cast<unsigned int>(into->VtxBuffer.Size) ||
						second >= static_cast<unsigned int>(into->VtxBuffer.Size) ||
						third >= static_cast<unsigned int>(into->VtxBuffer.Size))
						return false;
					const std::array<ImDrawVert, 3> triangle{
						{into->VtxBuffer[first], into->VtxBuffer[second], into->VtxBuffer[third]}
					};
					bool whollyInside = true;
					for (const std::vector<ImVec2> &polygon : polygons) {
						whollyInside = whollyInside && Inside(triangle[0], polygon) &&
									   Inside(triangle[1], polygon) && Inside(triangle[2], polygon);
					}
					if (whollyInside) {
						clipped.insert(
							clipped.end(),
							{static_cast<ImDrawIdx>(first),
							 static_cast<ImDrawIdx>(second),
							 static_cast<ImDrawIdx>(third)}
						);
						counts[countIndex] += 3;
						continue;
					}

					std::vector<std::array<ImDrawVert, 3>> pieces{triangle};
					for (const std::vector<ImVec2> &polygon : polygons) {
						std::vector<std::array<ImDrawVert, 3>> next;
						for (const std::array<ImDrawVert, 3> &piece : pieces) {
							const std::vector<ImDrawVert> shape = ClipTriangle(piece, polygon);
							for (size_t fan = 2; fan < shape.size(); fan++)
								next.push_back({shape[0], shape[fan - 1], shape[fan]});
						}
						pieces.swap(next);
						if (pieces.empty()) break;
					}
					for (const std::array<ImDrawVert, 3> &piece : pieces) {
						if (generated.size() + 3 > MAXIMUM_MASK_CLIP_VERTICES) return false;
						const unsigned int base =
							static_cast<unsigned int>(into->VtxBuffer.Size + generated.size());
						if (base + 2 > std::numeric_limits<ImDrawIdx>::max()) return false;
						generated.insert(generated.end(), piece.begin(), piece.end());
						clipped.insert(
							clipped.end(),
							{static_cast<ImDrawIdx>(base),
							 static_cast<ImDrawIdx>(base + 1),
							 static_cast<ImDrawIdx>(base + 2)}
						);
						counts[countIndex] += 3;
					}
				}
			}

			into->IdxBuffer.resize(firstIndex);
			for (int commandIndex = firstCommand; commandIndex < endCommand; commandIndex++) {
				ImDrawCmd &command = into->CmdBuffer[commandIndex];
				command.IdxOffset = static_cast<unsigned int>(into->IdxBuffer.Size);
				command.VtxOffset = 0;
				command.ElemCount = counts[static_cast<size_t>(commandIndex - firstCommand)];
				const size_t count = command.ElemCount;
				const size_t start = [&] {
					size_t total = 0;
					for (int before = firstCommand; before < commandIndex; before++)
						total += counts[static_cast<size_t>(before - firstCommand)];
					return total;
				}();
				for (size_t index = 0; index < count; index++)
					into->IdxBuffer.push_back(clipped[start + index]);
			}
			for (const ImDrawVert &vertex : generated)
				into->VtxBuffer.push_back(vertex);
			into->_IdxWritePtr = into->IdxBuffer.Data + into->IdxBuffer.Size;
			into->_VtxWritePtr = into->VtxBuffer.Data + into->VtxBuffer.Size;
			into->_VtxCurrentIdx = static_cast<unsigned int>(into->VtxBuffer.Size);
			into->_CmdHeader.VtxOffset = 0;
			return true;
		}

		// A rounded mask that cannot be represented must not silently become a
		// square mask. Leave this command empty instead: a missing local preview is
		// diagnosable, while a square corner invites an author to ship the wrong UI.
		void SuppressMaskedGeometry(ImDrawList *into, int firstCommand, int firstIndex) {
			into->IdxBuffer.resize(firstIndex);
			for (int command = firstCommand; command < into->CmdBuffer.Size; command++) {
				into->CmdBuffer[command].IdxOffset = static_cast<unsigned int>(firstIndex);
				into->CmdBuffer[command].ElemCount = 0;
			}
			into->_IdxWritePtr = into->IdxBuffer.Data + into->IdxBuffer.Size;
		}

		// Turns every primitive one command just appended about the element's
		// centre. Working on imgui's generated vertices preserves rounded corners,
		// glyph quads, tiles and all nine slices with one rule.
		void
		RotateVertices(ImDrawList *into, int first, const ImVec2 &min, const ImVec2 &max, float degrees) {
			if (degrees == 0.0f) {
				return;
			}
			const ImVec2 centre{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
			const float radians = degrees * 3.14159265358979323846f / 180.0f;
			const float cosine = std::cos(radians);
			const float sine = std::sin(radians);

			for (int index = first; index < into->VtxBuffer.Size; index++) {
				ImVec2 &point = into->VtxBuffer[index].pos;
				const float x = point.x - centre.x;
				const float y = point.y - centre.y;
				point = ImVec2{centre.x + x * cosine - y * sine, centre.y + x * sine + y * cosine};
			}
		}

		// Multiplies a `UIGradient` into whatever this command just emitted.
		//
		// **A pass over the vertices imgui produced, exactly as the rotation
		// above is.** Dear ImGui has no gradient primitive and no vertex
		// callback, so the only seam is the buffer it just appended to - and
		// the same seam already serves rotation, which is why this sits beside
		// it rather than inside every `Add*` call.
		//
		// **Before the rotation and not after.** A `DrawGradient` is resolved
		// against the command's unrotated bounds, so a point has to be projected
		// onto the ramp while it is still where the compile put it.
		//
		// **Per vertex, which is the studio's approximation of what the client
		// does exactly.** `render::InterfaceMesh` splits a shape at every
		// keypoint so each piece interpolates one linear segment; imgui's
		// rectangle is four vertices and cannot be split without rebuilding its
		// geometry. A two-stop ramp is therefore identical in both and a
		// many-stop one is smoother here than it should be. The editor is not
		// the shipping surface and `ui/AGENTS.md` says so; what matters is that
		// neither backend disagrees about *where* an element is.
		void ShadeVertices(
			ImDrawList *into, int first, const gui::DrawGradient &ramp, const GradientSpace &space
		) {
			const float span = ramp.Axis.X * ramp.Axis.X + ramp.Axis.Y * ramp.Axis.Y;

			for (int index = first; index < into->VtxBuffer.Size; index++) {
				ImDrawVert &vertex = into->VtxBuffer[index];

				// Back out of the panel's own placement, because the ramp is in
				// canvas pixels and these are in the panel's.
				const float canvasX =
					space.Scale != 0.0f ? (vertex.pos.x - space.Origin.x) / space.Scale : 0.0f;
				const float canvasY =
					space.Scale != 0.0f ? (vertex.pos.y - space.Origin.y) / space.Scale : 0.0f;

				const float where = span > 0.0f ? std::clamp(
													  ((canvasX - ramp.Origin.X) * ramp.Axis.X +
													   (canvasY - ramp.Origin.Y) * ramp.Axis.Y) /
														  span,
													  0.0f,
													  1.0f
												  )
												: 0.0f;

				const engine::core::Color3 tint = ramp.Color.Evaluate(where);
				const float fade = ramp.Transparency.Evaluate(where);

				const auto channel = [](ImU32 packed, int shift, float scale) {
					const float value = static_cast<float>((packed >> shift) & 0xFFu) * scale;
					return static_cast<ImU32>(std::clamp(value + 0.5f, 0.0f, 255.0f)) << shift;
				};

				vertex.col = channel(vertex.col, IM_COL32_R_SHIFT, std::clamp(tint.R, 0.0f, 1.0f)) |
							 channel(vertex.col, IM_COL32_G_SHIFT, std::clamp(tint.G, 0.0f, 1.0f)) |
							 channel(vertex.col, IM_COL32_B_SHIFT, std::clamp(tint.B, 0.0f, 1.0f)) |
							 channel(vertex.col, IM_COL32_A_SHIFT, std::clamp(1.0f - fade, 0.0f, 1.0f));
			}
		}

		// The marker an unresolvable image draws.
		//
		// **Visible rather than absent.** An `ImageLabel` whose content name
		// nothing can resolve is a mistake somebody made - a typo, an asset that
		// did not stage - and drawing nothing makes it look like the label is
		// broken rather than like the image is missing. A crossed box is the
		// convention every editor uses and it says which of the two it is.
		size_t PaintMissingImage(ImDrawList *into, const ImVec2 &min, const ImVec2 &max, ImU32 colour) {
			into->AddRect(min, max, colour);
			into->AddLine(min, max, colour);
			into->AddLine(ImVec2{min.x, max.y}, ImVec2{max.x, min.y}, colour);
			return 3;
		}

		// Where an image goes inside its element, per `ScaleType`.
		//
		// `Fit` shrinks the drawn rectangle to preserve the aspect ratio;
		// `Crop` keeps the rectangle and shrinks the sampled region instead.
		// The two are opposites and both are commonly wanted, which is why
		// Roblox has both and why neither is the default.
		void FitInto(
			ScaleType scale, const ImVec2 &imageSize, ImVec2 &min, ImVec2 &max, ImVec2 &uvMin, ImVec2 &uvMax
		) {
			if (imageSize.x <= 0.0f || imageSize.y <= 0.0f) {
				return;
			}

			const float boxWidth = max.x - min.x;
			const float boxHeight = max.y - min.y;
			if (boxWidth <= 0.0f || boxHeight <= 0.0f) {
				return;
			}

			const float boxAspect = boxWidth / boxHeight;
			const float imageAspect = imageSize.x / imageSize.y;

			if (scale == ScaleType::Fit) {
				if (imageAspect > boxAspect) {
					const float height = boxWidth / imageAspect;
					const float slack = (boxHeight - height) * 0.5f;
					min.y += slack;
					max.y -= slack;
				} else {
					const float width = boxHeight * imageAspect;
					const float slack = (boxWidth - width) * 0.5f;
					min.x += slack;
					max.x -= slack;
				}
				return;
			}

			// Crop: the rectangle stays and the sampled window narrows, centred.
			if (imageAspect > boxAspect) {
				const float keep = boxAspect / imageAspect;
				const float slack = (1.0f - keep) * 0.5f;
				uvMin.x += slack;
				uvMax.x -= slack;
			} else {
				const float keep = imageAspect / boxAspect;
				const float slack = (1.0f - keep) * 0.5f;
				uvMin.y += slack;
				uvMax.y -= slack;
			}
		}

		// A nine-slice: four corners at their own size, four edges stretched
		// along one axis, one middle stretched along both.
		size_t PaintSlice(
			ImDrawList *into,
			ImTextureID texture,
			const ImVec2 &imageSize,
			const ImVec2 &min,
			const ImVec2 &max,
			const core::Rect &centre,
			float sliceScale,
			ImU32 tint
		) {
			// The four insets, in image pixels, and the same four magnified on
			// screen. `SliceScale` magnifies the corners without magnifying
			// what is sampled, which is what makes a rounded panel keep its
			// radius at a larger size.
			const float left = centre.Min.X;
			const float top = centre.Min.Y;
			const float right = std::max(imageSize.x - centre.Max.X, 0.0f);
			const float bottom = std::max(imageSize.y - centre.Max.Y, 0.0f);

			// Clamped so two opposite insets cannot exceed the element and
			// produce a middle of negative width, which draws as an inverted
			// quad rather than as nothing.
			const float width = max.x - min.x;
			const float height = max.y - min.y;
			const float shrink = std::min(
				{1.0f,
				 (left + right) > 0.0f ? width / ((left + right) * sliceScale) : 1.0f,
				 (top + bottom) > 0.0f ? height / ((top + bottom) * sliceScale) : 1.0f}
			);

			const float scale = sliceScale * shrink;
			const float xs[4]{min.x, min.x + left * scale, max.x - right * scale, max.x};
			const float ys[4]{min.y, min.y + top * scale, max.y - bottom * scale, max.y};

			const float us[4]{0.0f, left / imageSize.x, centre.Max.X / imageSize.x, 1.0f};
			const float vs[4]{0.0f, top / imageSize.y, centre.Max.Y / imageSize.y, 1.0f};

			size_t drawn = 0;
			for (int row = 0; row < 3; row++) {
				for (int column = 0; column < 3; column++) {
					if (xs[column + 1] <= xs[column] || ys[row + 1] <= ys[row]) {
						continue;
					}
					into->AddImage(
						texture,
						ImVec2{xs[column], ys[row]},
						ImVec2{xs[column + 1], ys[row + 1]},
						ImVec2{us[column], vs[row]},
						ImVec2{us[column + 1], vs[row + 1]},
						tint
					);
					drawn++;
				}
			}
			return drawn;
		}

		size_t PaintImage(
			const DrawCommand &command,
			ImDrawList *into,
			const GradientSpace &space,
			const ImageSource &images
		) {
			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const ImU32 tint = Colour(command.Tint, command.Transparency);

			const ImageSource::Resolved resolved =
				command.Kind == DrawKind::Viewport
					? (images.ResolveViewport ? images.ResolveViewport(command.Source)
											  : ImageSource::Resolved{})
					: (images.Resolve ? images.Resolve(command.Image) : ImageSource::Resolved{});
			const ImTextureID texture = resolved.Texture;

			if (texture == ImTextureID{}) {
				return PaintMissingImage(into, min, max, tint);
			}

			// **The animation cell is where the coordinates start**, so
			// everything below composes inside the frame that is showing rather
			// than across the whole sheet. A still image resolves to the whole
			// image and the arithmetic is the identity.
			ImVec2 uvMin = resolved.CellMin;
			ImVec2 uvMax = resolved.CellMax;

			const ImVec2 span{uvMax.x - uvMin.x, uvMax.y - uvMin.y};

			// **The *cell's* pixel size, not the sheet's.** A nine-slice inset
			// and a tile size are authored against the picture somebody can see,
			// and on an 8x8 sheet the sheet is eight times that in each
			// direction - so measuring against it would put every slice border
			// an eighth of the way in.
			ImVec2 imageSize{resolved.Size.x * span.x, resolved.Size.y * span.y};

			// A non-empty `Sample` narrows the whole thing to a sub-rectangle
			// of the image before anything else happens - Roblox's
			// `ImageRectOffset`/`ImageRectSize`, which composes with every
			// `ScaleType` rather than replacing one.
			if (imageSize.x > 0.0f && imageSize.y > 0.0f && command.Sample.Width() > 0.0f &&
				command.Sample.Height() > 0.0f) {
				uvMin = ImVec2{
					resolved.CellMin.x + command.Sample.Min.X / imageSize.x * span.x,
					resolved.CellMin.y + command.Sample.Min.Y / imageSize.y * span.y
				};
				uvMax = ImVec2{
					resolved.CellMin.x + command.Sample.Max.X / imageSize.x * span.x,
					resolved.CellMin.y + command.Sample.Max.Y / imageSize.y * span.y
				};
				imageSize = ImVec2{command.Sample.Width(), command.Sample.Height()};
			}

			switch (command.Scale) {
			case ScaleType::Slice:
				return PaintSlice(
					into, texture, imageSize, min, max, command.SliceCenter, command.SliceScale, tint
				);

			case ScaleType::Tile: {
				const float tileWidth = command.Tile.X > 0.0f ? command.Tile.X * space.Scale : max.x - min.x;
				const float tileHeight = command.Tile.Y > 0.0f ? command.Tile.Y * space.Scale : max.y - min.y;
				if (tileWidth <= 0.0f || tileHeight <= 0.0f) {
					return 0;
				}

				// The clip is already pushed by the caller, so a partial tile at
				// the far edge is cut by the scissor rather than by arithmetic
				// here - which is what keeps the last row of tiles the same
				// image as the first rather than a squashed one.
				size_t drawn = 0;
				for (float y = min.y; y < max.y; y += tileHeight) {
					for (float x = min.x; x < max.x; x += tileWidth) {
						into->AddImage(
							texture, ImVec2{x, y}, ImVec2{x + tileWidth, y + tileHeight}, uvMin, uvMax, tint
						);
						drawn++;
					}
				}
				return drawn;
			}

			case ScaleType::Fit:
			case ScaleType::Crop: {
				ImVec2 fitMin = min;
				ImVec2 fitMax = max;
				FitInto(command.Scale, imageSize, fitMin, fitMax, uvMin, uvMax);
				into->AddImage(texture, fitMin, fitMax, uvMin, uvMax, tint);
				return 1;
			}

			case ScaleType::Stretch:
				break;
			}

			into->AddImage(texture, min, max, uvMin, uvMax, tint);
			return 1;
		}

		// A marked-up run, one styled range at a time.
		//
		// **Split into one `AddText` per span rather than one for the run**,
		// because imgui's is one colour, one face and one size - which is exactly
		// what a span overrides. The x is accumulated with `CalcTextSizeA`, which
		// is the same measurement imgui would have used itself, so the words land
		// where they would have.
		//
		// **Wrapping is not applied to a marked-up run here**, and that is the
		// editor stopping short rather than the feature doing so:
		// `render::InterfaceMesh` wraps spans exactly, because it lays out
		// glyph by glyph and can ask which span each one is in. Reproducing that
		// against imgui's wrapper would be a second line-breaker, which is the
		// one thing `PaintText` below refuses in as many words. `ui/AGENTS.md`
		// carries why the editor is allowed to be the poorer of the two: what
		// must agree between them is *where an element is*, and that does.
		size_t PaintRichText(const DrawCommand &command, ImDrawList *into, const GradientSpace &space) {
			const float baseSize = static_cast<float>(command.TextSize) * space.Scale;
			ImFont *baseFont = Font(FaceFor(command.Font));
			ImFont *measure = baseFont != nullptr ? baseFont : ImGui::GetFont();

			struct Piece {
				size_t Begin = 0;
				size_t End = 0;
				const gui::DrawSpan *Span = nullptr;
			};

			// One piece per span and one per gap between them, in order. The
			// spans are already sorted and non-overlapping - `DrawSpan` says so -
			// so this is a walk rather than a merge.
			std::vector<Piece> pieces;
			size_t at = 0;
			for (const gui::DrawSpan &span : command.Spans) {
				if (span.Begin > at) {
					pieces.push_back(Piece{at, span.Begin, nullptr});
				}
				pieces.push_back(Piece{span.Begin, span.End, &span});
				at = span.End;
			}
			if (at < command.Text.size()) {
				pieces.push_back(Piece{at, command.Text.size(), nullptr});
			}

			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);

			const auto sizeOf = [&](const Piece &piece) {
				return piece.Span != nullptr && piece.Span->Size > 0
						   ? static_cast<float>(piece.Span->Size) * space.Scale
						   : baseSize;
			};
			const auto fontOf = [&](const Piece &piece) {
				return piece.Span != nullptr ? Font(FaceFor(piece.Span->Font)) : baseFont;
			};

			// The block's extent, so the two alignments have something to work
			// against. Lines are split on the newlines the parse already put in.
			float widest = 0.0f;
			float lineWidth = 0.0f;
			float lineHeight = baseSize;
			float blockHeight = 0.0f;
			for (const Piece &piece : pieces) {
				ImFont *font = fontOf(piece);
				ImFont *pieceMeasure = font != nullptr ? font : measure;
				const float size = sizeOf(piece);

				size_t start = piece.Begin;
				for (size_t index = piece.Begin; index <= piece.End; index++) {
					const bool ends = index == piece.End;
					if (!ends && command.Text[index] != '\n') {
						continue;
					}

					lineWidth +=
						pieceMeasure
							->CalcTextSizeA(
								size, FLT_MAX, 0.0f, command.Text.data() + start, command.Text.data() + index
							)
							.x;
					lineHeight = std::max(lineHeight, size);
					if (!ends) {
						widest = std::max(widest, lineWidth);
						blockHeight += lineHeight;
						lineWidth = 0.0f;
						lineHeight = baseSize;
						start = index + 1;
					}
				}
			}
			widest = std::max(widest, lineWidth);
			blockHeight += lineHeight;

			float originX = min.x;
			if (command.XAlignment == TextXAlignment::Center) {
				originX = min.x + ((max.x - min.x) - widest) * 0.5f;
			} else if (command.XAlignment == TextXAlignment::Right) {
				originX = max.x - widest;
			}

			float y = min.y;
			if (command.YAlignment == TextYAlignment::Center) {
				y = min.y + ((max.y - min.y) - blockHeight) * 0.5f;
			} else if (command.YAlignment == TextYAlignment::Bottom) {
				y = max.y - blockHeight;
			}

			float x = originX;
			size_t drawn = 0;

			for (const Piece &piece : pieces) {
				ImFont *font = fontOf(piece);
				ImFont *pieceMeasure = font != nullptr ? font : measure;
				const float size = sizeOf(piece);
				const ImU32 tint = piece.Span != nullptr ? Colour(piece.Span->Tint, piece.Span->Transparency)
														 : Colour(command.Tint, command.Transparency);

				size_t start = piece.Begin;
				for (size_t index = piece.Begin; index <= piece.End; index++) {
					const bool ends = index == piece.End;
					if (!ends && command.Text[index] != '\n') {
						continue;
					}

					const char *from = command.Text.data() + start;
					const char *to = command.Text.data() + index;
					if (to > from) {
						into->AddText(font, size, ImVec2{x, y}, tint, from, to);
						drawn++;

						const float width = pieceMeasure->CalcTextSizeA(size, FLT_MAX, 0.0f, from, to).x;
						if (piece.Span != nullptr && (piece.Span->Underline || piece.Span->Strike)) {
							const float weight = std::max(size * 0.1f, 1.0f);
							const float rule = piece.Span->Strike ? y + size * 0.55f : y + size * 0.95f;
							into->AddRectFilled(ImVec2{x, rule}, ImVec2{x + width, rule + weight}, tint);
							drawn++;
						}
						x += width;
					}

					if (!ends) {
						x = originX;
						y += std::max(size, baseSize);
						start = index + 1;
					}
				}
			}

			return drawn;
		}

		size_t PaintText(
			const DrawCommand &command,
			ImDrawList *into,
			const GradientSpace &space,
			const ImageSource &images
		) {
			// A game list can arrive with the canonical shaper's answer. Dear ImGui
			// still owns its editor font texture, but positions each source cluster
			// from that answer instead of measuring, wrapping, or applying bidi a
			// second time. Rich styling remains on the legacy path until spans are
			// represented by shaped runs too.
			if (command.Shaping.Status == gui::TextShapeStatus::Ok && command.TextSize > 0 &&
				images.Fonts != nullptr) {
				const float size = static_cast<float>(command.TextSize) * space.Scale;
				const ImVec2 min = space.Point(command.Bounds.Min);
				const ImVec2 max = space.Point(command.Bounds.Max);
				std::vector<gui::ShapedLine> lines = command.Shaping.Lines;
				if (lines.empty()) {
					lines.push_back(
						gui::ShapedLine{
							.GlyphCount = static_cast<uint32_t>(command.Shaping.Glyphs.size()),
							.Advance = command.Shaping.Advance,
						}
					);
				}
				const auto heightOf = [&](const gui::ShapedLine &line) {
					return std::max(line.Ascent + line.Descent, static_cast<float>(command.TextSize)) *
						   space.Scale * std::max(command.LineHeight, 0.0f);
				};
				float blockHeight = 0.0f;
				for (const gui::ShapedLine &line : lines)
					blockHeight += heightOf(line);
				float top = min.y;
				if (command.YAlignment == TextYAlignment::Center) {
					top += (max.y - min.y - blockHeight) * 0.5f;
				} else if (command.YAlignment == TextYAlignment::Bottom) {
					top = max.y - blockHeight;
				}
				const auto lineFor = [&](size_t glyphIndex) -> const gui::ShapedLine & {
					for (const gui::ShapedLine &line : lines) {
						if (glyphIndex >= line.GlyphOffset && glyphIndex < line.GlyphOffset + line.GlyphCount)
							return line;
					}
					return lines.front();
				};

				const auto draw = [&](ImU32 colour, ImVec2 offset, bool styling) {
					for (size_t glyphIndex = 0; glyphIndex < command.Shaping.Glyphs.size(); glyphIndex++) {
						const gui::ShapedGlyph &glyph = command.Shaping.Glyphs[glyphIndex];
						if (images.Fonts == nullptr) continue;
						const CachedGlyph *cached = ResolveGlyph(*images.Fonts, glyph);
						if (cached != nullptr && !cached->Visible) continue;
						const gui::ShapedLine &line = lineFor(glyphIndex);
						float originX = min.x;
						if (command.XAlignment == TextXAlignment::Center) {
							originX += (max.x - min.x - line.Advance * space.Scale) * 0.5f;
						} else if (command.XAlignment == TextXAlignment::Right) {
							originX = max.x - line.Advance * space.Scale;
						}
						float preceding = 0.0f;
						for (const gui::ShapedLine &before : lines) {
							if (&before == &line) break;
							preceding += heightOf(before);
						}
						const float baseline = top + preceding + line.Baseline * space.Scale;
						if (cached == nullptr) {
							const ImVec2 mark{
								originX + glyph.X * space.Scale + offset.x,
								baseline + glyph.Y * space.Scale + offset.y
							};
							into->AddRect(
								mark,
								ImVec2{mark.x + std::max(glyph.AdvanceX * space.Scale, 1.0f), mark.y + size},
								colour
							);
							continue;
						}
						ImFontAtlasRect uv;
						if (!GlyphUv(*cached, uv)) continue;
						const ImVec2 glyphMin{
							originX + (glyph.X + cached->Glyph.OffsetX) * space.Scale + offset.x,
							baseline + (glyph.Y + cached->Glyph.OffsetY) * space.Scale + offset.y
						};
						const ImVec2 glyphMax{
							glyphMin.x + cached->Glyph.Width * space.Scale,
							glyphMin.y + cached->Glyph.Height * space.Scale
						};
						into->PushTexture(ImGui::GetIO().Fonts->TexRef);
						Quad(
							into,
							uv,
							glyphMin,
							glyphMax,
							styling ? Colour(glyph.Tint, glyph.Transparency) : colour
						);
						into->PopTexture();
					}
				};
				size_t drawn = 0;
				if (command.StrokeTransparency < 1.0f) {
					const ImU32 stroke = Colour(command.StrokeTint, command.StrokeTransparency);
					for (const ImVec2 offset : std::array<ImVec2, 8>{
							 ImVec2{-1.0f, -1.0f},
							 ImVec2{0.0f, -1.0f},
							 ImVec2{1.0f, -1.0f},
							 ImVec2{-1.0f, 0.0f},
							 ImVec2{1.0f, 0.0f},
							 ImVec2{-1.0f, 1.0f},
							 ImVec2{0.0f, 1.0f},
							 ImVec2{1.0f, 1.0f},
						 }) {
						draw(stroke, ImVec2{offset.x * space.Scale, offset.y * space.Scale}, false);
						drawn++;
					}
				}
				draw(Colour(command.Tint, command.Transparency), {}, true);
				return drawn + 1;
			}
			if (!command.Spans.empty() && command.TextSize > 0 && !command.Text.empty()) {
				return PaintRichText(command, into, space);
			}

			std::string visible = command.Text;
			std::string_view text = visible;
			if (text.empty() || command.TextSize <= 0) {
				return 0;
			}

			ImFont *font = Font(FaceFor(command.Font));
			const float size = static_cast<float>(command.TextSize) * space.Scale;

			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const float width = max.x - min.x;

			// **Wrapping is imgui's, at the element's own width.** A second
			// line-breaker here would disagree with the one that measured, and
			// the visible symptom of two line-breakers is a last line that is
			// clipped on some strings and not others.
			const float wrap = command.Wrapped ? width : 0.0f;

			ImFont *measureFont = font != nullptr ? font : ImGui::GetFont();
			if (!command.Wrapped && command.Truncate == gui::TextTruncate::AtEnd && width > 0.0f &&
				measureFont->CalcTextSizeA(size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x >
					width) {
				constexpr std::string_view dots = "...";
				while (!visible.empty()) {
					visible.pop_back();
					while (!visible.empty() &&
						   (static_cast<unsigned char>(visible.back()) & 0xC0u) == 0x80u) {
						visible.pop_back();
					}
					const std::string candidate = visible + std::string(dots);
					if (measureFont->CalcTextSizeA(size, FLT_MAX, 0.0f, candidate.c_str()).x <= width) {
						visible = candidate;
						break;
					}
				}
				text = visible;
			}
			const ImVec2 extent =
				measureFont->CalcTextSizeA(size, FLT_MAX, wrap, text.data(), text.data() + text.size());

			float x = min.x;
			switch (command.XAlignment) {
			case TextXAlignment::Center:
				x = min.x + (width - extent.x) * 0.5f;
				break;
			case TextXAlignment::Right:
				x = max.x - extent.x;
				break;
			case TextXAlignment::Left:
				break;
			}

			float y = min.y;
			switch (command.YAlignment) {
			case TextYAlignment::Center:
				y = min.y + ((max.y - min.y) - extent.y) * 0.5f;
				break;
			case TextYAlignment::Bottom:
				y = max.y - extent.y;
				break;
			case TextYAlignment::Top:
				break;
			}

			const auto draw = [&](const ImVec2 &position, ImU32 colour) {
				into->AddText(font, size, position, colour, text.data(), text.data() + text.size(), wrap);
			};

			size_t drawn = 0;
			if (command.StrokeTransparency < 1.0f) {
				const ImU32 stroke = Colour(command.StrokeTint, command.StrokeTransparency);
				for (const ImVec2 offset : std::array<ImVec2, 8>{
						 ImVec2{-1.0f, -1.0f},
						 ImVec2{0.0f, -1.0f},
						 ImVec2{1.0f, -1.0f},
						 ImVec2{-1.0f, 0.0f},
						 ImVec2{1.0f, 0.0f},
						 ImVec2{-1.0f, 1.0f},
						 ImVec2{0.0f, 1.0f},
						 ImVec2{1.0f, 1.0f},
					 }) {
					draw(ImVec2{x + offset.x * space.Scale, y + offset.y * space.Scale}, stroke);
					drawn++;
				}
			}
			draw(ImVec2{x, y}, Colour(command.Tint, command.Transparency));
			return drawn + 1;
		}
	}

	size_t PaintGui(
		const gui::DrawList &list, ImDrawList *into, const PaintTarget &target, const ImageSource &images
	) {
		if (into == nullptr) {
			return 0;
		}

		const GradientSpace space{target.Origin, target.Scale};
		// Reserve every shaped glyph before recording any quad. ImGui may repack
		// and resize its atlas while reserving; doing that after a prior command
		// wrote vertices would leave those vertices with stale UVs.
		if (images.Fonts != nullptr) {
			for (const DrawCommand &command : list.Commands) {
				if (!command.Spatial && command.Kind == DrawKind::Text &&
					command.Shaping.Status == gui::TextShapeStatus::Ok) {
					for (const gui::ShapedGlyph &glyph : command.Shaping.Glyphs) {
						(void)ResolveGlyph(*images.Fonts, glyph);
					}
				}
			}
		}
		struct GroupRange {
			size_t End = 0;
			size_t BeginOperation = 0;
			size_t EndOperation = 0;
			gui::DrawOperation Operation;
		};
		std::unordered_map<size_t, std::vector<GroupRange>> groups;
		std::vector<size_t> groupStack;
		for (size_t index = 0; index < list.Operations.size(); index++) {
			const gui::DrawOperation &entry = list.Operations[index];
			if (entry.Kind == gui::DrawOperationKind::BeginGroup) {
				groupStack.push_back(index);
				continue;
			}
			if (entry.Kind != gui::DrawOperationKind::EndGroup || groupStack.empty()) continue;
			const size_t begin = groupStack.back();
			groupStack.pop_back();
			const gui::DrawOperation &opening = list.Operations[begin];
			if (entry.Command < opening.Command || entry.Command > list.Commands.size()) continue;
			groups[opening.Command].push_back(GroupRange{entry.Command, begin, index, opening});
		}
		for (auto &[command, ranges] : groups) {
			(void)command;
			// A parent and child may both begin before the first child command. The
			// parent has the later end boundary and must own the recursive paint.
			std::sort(ranges.begin(), ranges.end(), [](const GroupRange &left, const GroupRange &right) {
				if (left.End != right.End) return left.End > right.End;
				return left.BeginOperation < right.BeginOperation;
			});
		}
		size_t drawn = 0;
		std::vector<ActiveMask> activeMasks;
		size_t operation = 0;
		const auto applyOperations = [&](size_t command) {
			while (operation < list.Operations.size() && list.Operations[operation].Command == command) {
				const gui::DrawOperation &entry = list.Operations[operation++];
				if (entry.Kind == gui::DrawOperationKind::BeginMask) {
					if (activeMasks.size() >= MAXIMUM_STUDIO_MASKS || Empty(entry.Bounds)) continue;
					const float extent = std::min(entry.Bounds.Width(), entry.Bounds.Height());
					const float radius = std::clamp(entry.CornerRadius, 0.0f, extent * 0.5f);
					activeMasks.push_back({entry.Source, entry.Collector, entry.Bounds, radius});
				} else if (entry.Kind == gui::DrawOperationKind::EndMask) {
					for (size_t index = activeMasks.size(); index-- > 0;) {
						if (activeMasks[index].Source != entry.Source ||
							activeMasks[index].Collector != entry.Collector)
							continue;
						activeMasks.erase(activeMasks.begin() + static_cast<std::ptrdiff_t>(index));
						break;
					}
				}
				// Group boundaries are consumed by the recursive target path below.
				// They must not alter the mask stack at this level.
			}
		};

		for (size_t commandIndex = 0; commandIndex < list.Commands.size(); commandIndex++) {
			applyOperations(commandIndex);
			if (const auto found = groups.find(commandIndex);
				found != groups.end() && !found->second.empty()) {
				const GroupRange &group = found->second.front();
				if (group.End <= commandIndex) continue;
				gui::DrawList subtree;
				subtree.Commands.insert(
					subtree.Commands.end(),
					list.Commands.begin() + static_cast<std::ptrdiff_t>(commandIndex),
					list.Commands.begin() + static_cast<std::ptrdiff_t>(group.End)
				);
				for (size_t index = group.BeginOperation + 1; index < group.EndOperation; index++) {
					gui::DrawOperation nested = list.Operations[index];
					if (nested.Command < commandIndex || nested.Command > group.End) continue;
					nested.Command -= commandIndex;
					subtree.Operations.push_back(nested);
				}
				subtree.Gradients = list.Gradients;
				core::Rect bounds = group.Operation.Bounds;
				if (Empty(bounds)) {
					bounds = list.Commands[commandIndex].Bounds;
					for (size_t index = commandIndex + 1; index < group.End; index++) {
						const core::Rect &next = list.Commands[index].Bounds;
						bounds.Min.X = std::min(bounds.Min.X, next.Min.X);
						bounds.Min.Y = std::min(bounds.Min.Y, next.Min.Y);
						bounds.Max.X = std::max(bounds.Max.X, next.Max.X);
						bounds.Max.Y = std::max(bounds.Max.Y, next.Max.Y);
					}
				}
				core::Rect groupClip =
					Empty(group.Operation.Clip) ? bounds : Intersect(bounds, group.Operation.Clip);
				const ImVec2 groupMin = space.Point(groupClip.Min);
				const ImVec2 groupMax = space.Point(groupClip.Max);
				const float extentX = groupMax.x - groupMin.x;
				const float extentY = groupMax.y - groupMin.y;
				const int width =
					std::isfinite(extentX) && extentX > 0.0f && extentX <= MAXIMUM_GROUP_TARGET_EXTENT
						? static_cast<int>(std::ceil(extentX))
						: 0;
				const int height =
					std::isfinite(extentY) && extentY > 0.0f && extentY <= MAXIMUM_GROUP_TARGET_EXTENT
						? static_cast<int>(std::ceil(extentY))
						: 0;
				uint64_t identity = 1469598103934665603ull;
				identity = FoldGroupKey(identity, group.BeginOperation);
				identity = FoldGroupKey(identity, group.EndOperation);
				uint64_t key = FoldGroupKey(identity, images.CompiledSignature);
				key = FoldGroupKey(key, images.Revision);
				key = FoldGroupKey(key, static_cast<uint64_t>(width));
				key = FoldGroupKey(key, static_cast<uint64_t>(height));
				const bool cacheable = std::none_of(
					subtree.Commands.begin(), subtree.Commands.end(), [](const DrawCommand &command) {
						return command.Kind == DrawKind::Viewport;
					}
				);
				if (!cacheable) key = FoldGroupKey(key, static_cast<uint64_t>(ImGui::GetFrameCount()));
				GroupTexture *groupTexture = CreateGroupTexture(key, identity, width, height);
				const core::Rect screenBounds{{groupMin.x, groupMin.y}, {groupMax.x, groupMax.y}};
				size_t childDrawn = group.End - commandIndex;
				if (groupTexture != nullptr && !groupTexture->Valid) {
					ImDrawList layer(into->_Data);
					layer._ResetForNewFrame();
					// A scratch list has no window-owned fullscreen clip. Seed one before
					// PaintGui intersects the subtree's clips, or every intersection is
					// empty and a nested CanvasGroup rasterises as transparent.
					layer.PushClipRectFullScreen();
					childDrawn = PaintGui(subtree, &layer, target, images);
					if (!RasterGroup(layer, screenBounds, *groupTexture->Texture, images)) {
						groupTexture = cacheable ? PriorGroupTexture(identity, width, height) : nullptr;
					} else {
						groupTexture->Valid = true;
					}
				}
				if (groupTexture != nullptr && groupTexture->Valid) {
					core::Rect clip = groupClip;
					std::vector<ActiveMask> roundedMasks;
					for (const ActiveMask &mask : activeMasks) {
						if (mask.Collector != group.Operation.Collector) continue;
						clip = Intersect(clip, mask.Bounds);
						if (mask.Radius > 0.0f) roundedMasks.push_back(mask);
					}
					into->PushClipRect(space.Point(clip.Min), space.Point(clip.Max), true);
					const int firstCommand = into->CmdBuffer.Size;
					const int firstIndex = into->IdxBuffer.Size;
					if (!roundedMasks.empty()) into->AddDrawCmd();
					into->AddImage(
						groupTexture->Texture->GetTexRef(),
						groupMin,
						groupMax,
						ImVec2{0.0f, 0.0f},
						ImVec2{1.0f, 1.0f},
						Colour(core::Color3{1.0f, 1.0f, 1.0f}, group.Operation.Transparency)
					);
					if (!roundedMasks.empty()) {
						if (!ClipRoundedMaskGeometry(into, firstCommand, firstIndex, roundedMasks, space)) {
							SuppressMaskedGeometry(into, firstCommand, firstIndex);
						}
						into->AddDrawCmd();
					}
					into->PopClipRect();
					drawn += childDrawn;
				} else {
					// A group is still one compositing boundary when its CPU target cannot
					// be made. A visible placeholder preserves its single group alpha;
					// replaying children here would apply overlap alpha incorrectly.
					into->AddRectFilled(
						groupMin,
						groupMax,
						Colour(core::Color3{1.0f, 0.0f, 1.0f}, group.Operation.Transparency)
					);
					drawn++;
				}
				while (operation < list.Operations.size() && list.Operations[operation].Command < group.End) {
					operation++;
				}
				commandIndex = group.End - 1;
				continue;
			}
			const DrawCommand &command = list.Commands[commandIndex];
			if (command.Spatial) {
				continue;
			}
			core::Rect clip = command.Clip;
			std::vector<ActiveMask> roundedMasks;
			for (const ActiveMask &mask : activeMasks) {
				if (mask.Collector != command.Collector) continue;
				clip = Intersect(clip, mask.Bounds);
				if (mask.Radius > 0.0f) roundedMasks.push_back(mask);
			}
			// **Pushed with intersection, so a caller may already have one.** A
			// panel drawing a canvas inside itself has a clip of its own, and a
			// replacing push would let an element draw over the panel's border.
			into->PushClipRect(space.Point(clip.Min), space.Point(clip.Max), true);
			const int firstCommand = into->CmdBuffer.Size;
			const int firstIndex = into->IdxBuffer.Size;
			if (!roundedMasks.empty()) into->AddDrawCmd();

			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const ImU32 tint = Colour(command.Tint, command.Transparency);
			const float radius = command.CornerRadius * space.Scale;
			const int firstVertex = into->VtxBuffer.Size;
			const ImGuiPlatformIO &platform = ImGui::GetPlatformIO();
			const bool nearest = command.Kind == DrawKind::Image &&
								 command.Resample == gui::ResampleMode::Pixelated &&
								 platform.DrawCallback_SetSamplerNearest != nullptr &&
								 platform.DrawCallback_SetSamplerLinear != nullptr;
			if (nearest) {
				into->AddCallback(platform.DrawCallback_SetSamplerNearest);
			}

			switch (command.Kind) {
			case DrawKind::Rectangle:
				into->AddRectFilled(min, max, tint, radius);
				drawn++;
				break;

			case DrawKind::Outline:
				into->AddRect(min, max, tint, radius, ImDrawFlags_None, command.Thickness * space.Scale);
				drawn++;
				break;

			case DrawKind::Image:
			case DrawKind::Viewport:
				drawn += PaintImage(command, into, space, images);
				break;

			case DrawKind::Text:
				drawn += PaintText(command, into, space, images);
				break;
			}
			if (nearest) {
				// The sampler is backend state rather than part of an imgui draw
				// command. Restore the default so host chrome after this image does
				// not inherit point sampling.
				into->AddCallback(platform.DrawCallback_SetSamplerLinear);
			}
			// Shade before turning, because the ramp is measured against the
			// unrotated bounds the compile resolved it from.
			if (command.Gradient >= 0 && static_cast<size_t>(command.Gradient) < list.Gradients.size()) {
				ShadeVertices(
					into, firstVertex, list.Gradients[static_cast<size_t>(command.Gradient)], space
				);
			}

			RotateVertices(into, firstVertex, min, max, command.Rotation);
			if (!roundedMasks.empty()) {
				if (!ClipRoundedMaskGeometry(into, firstCommand, firstIndex, roundedMasks, space)) {
					SuppressMaskedGeometry(into, firstCommand, firstIndex);
				}
				// Seal this command so the next one cannot merge geometry that was
				// rebuilt against a different mask stack.
				into->AddDrawCmd();
			}

			into->PopClipRect();
		}

		return drawn;
	}

	void ShutdownGuiPainter() {
		if (ImGui::GetCurrentContext() == nullptr) return;
		GroupTextureStores &stores = AllGroupTextures();
		const auto found = stores.find(ImGui::GetCurrentContext());
		if (found == stores.end()) return;
		for (const GroupTexture &texture : found->second.Textures) {
			ImGui::UnregisterUserTexture(texture.Texture.get());
		}
		stores.erase(found);
	}
}
