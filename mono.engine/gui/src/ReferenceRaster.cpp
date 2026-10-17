#include <engine/gui/ReferenceRaster.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace engine::gui {
	using core::Color3;
	using core::Rect;
	using core::Vector2;

	namespace {
		struct LinearPixel {
			Color3 Color;
			float Alpha = 0.0f;
		};

		struct RasterMaskRegion {
			Rect Bounds;
			float Radius = 0.0f;
		};

		struct Target {
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<LinearPixel> Pixels;
		};

		struct FreeTypeLibrary {
			FT_Library Value = nullptr;

			FreeTypeLibrary() {
				FT_Init_FreeType(&Value);
			}

			~FreeTypeLibrary() {
				if (Value != nullptr) {
					FT_Done_FreeType(Value);
				}
			}
		};

		FT_Library FreeType() {
			static FreeTypeLibrary library;
			return library.Value;
		}

		struct GlyphKey {
			core::Name Face;
			uint32_t Index = 0;
			uint16_t PixelSize = 0;

			bool operator==(const GlyphKey &) const = default;
		};

		struct GlyphKeyHash {
			size_t operator()(const GlyphKey &key) const {
				return (static_cast<size_t>(key.Face.Id()) * 1315423911u + key.Index) * 257u + key.PixelSize;
			}
		};

		struct GlyphCoverage {
			uint16_t Width = 0;
			uint16_t Height = 0;
			float OffsetX = 0.0f;
			float OffsetY = 0.0f;
			std::vector<uint8_t> Pixels;
		};

		struct GlyphCache {
			std::unordered_map<GlyphKey, GlyphCoverage, GlyphKeyHash> Entries;
			size_t Bytes = 0;
			size_t Draws = 0;
		};

		enum class GlyphLoad {
			Ready,
			Skip,
			Refused,
		};

		bool PixelCount(uint32_t width, uint32_t height, size_t &count) {
			if (width == 0 || height == 0 || width > std::numeric_limits<size_t>::max() / height) {
				return false;
			}
			count = static_cast<size_t>(width) * height;
			return count <= MAXIMUM_REFERENCE_RASTER_PIXELS;
		}

		bool FitsByteBudget(size_t pixelCount, size_t groupDepth) {
			const size_t targetCount = groupDepth + 1;
			if (targetCount > std::numeric_limits<size_t>::max() / pixelCount) {
				return false;
			}
			const size_t linearPixels = targetCount * pixelCount;
			if (linearPixels > std::numeric_limits<size_t>::max() / sizeof(LinearPixel)) {
				return false;
			}
			const size_t workingBytes = linearPixels * sizeof(LinearPixel);
			if (pixelCount > std::numeric_limits<size_t>::max() / sizeof(ReferencePixel)) {
				return false;
			}
			const size_t outputBytes = pixelCount * sizeof(ReferencePixel);
			return workingBytes <= MAXIMUM_REFERENCE_RASTER_BYTES &&
				   outputBytes <= MAXIMUM_REFERENCE_RASTER_BYTES - workingBytes;
		}

		bool OperationDepths(const DrawList &list, size_t &groups, size_t &masks) {
			size_t currentGroups = 0;
			size_t currentMasks = 0;
			groups = 0;
			masks = 0;
			for (const DrawOperation &operation : list.Operations) {
				if (operation.Kind == DrawOperationKind::BeginGroup) {
					if (++currentGroups > MAXIMUM_REFERENCE_RASTER_GROUPS) {
						return false;
					}
					groups = std::max(groups, currentGroups);
				} else if (operation.Kind == DrawOperationKind::EndGroup) {
					if (currentGroups == 0) {
						return false;
					}
					currentGroups--;
				} else if (operation.Kind == DrawOperationKind::BeginMask) {
					if (++currentMasks > MAXIMUM_REFERENCE_RASTER_MASKS) {
						return false;
					}
					masks = std::max(masks, currentMasks);
				} else if (operation.Kind == DrawOperationKind::EndMask) {
					if (currentMasks == 0) {
						return false;
					}
					currentMasks--;
				}
			}
			return currentGroups == 0 && currentMasks == 0;
		}

		float Clamp(float value) {
			return std::clamp(value, 0.0f, 1.0f);
		}

		uint8_t ToByte(float value) {
			return static_cast<uint8_t>(std::lround(Clamp(value) * 255.0f));
		}

		Rect Transform(const Rect &rect, const CollectorTransform *transform) {
			if (transform == nullptr) {
				return rect;
			}
			return {
				transform->Origin + rect.Min * transform->Scale,
				transform->Origin + rect.Max * transform->Scale,
			};
		}

		const CollectorTransform *FindTransform(const DrawList &list, ecs::Entity collector) {
			for (const CollectorTransform &transform : list.Transforms) {
				if (transform.Collector == collector) {
					return &transform;
				}
			}
			return nullptr;
		}

		bool InsideRounded(const Rect &bounds, float radius, const Vector2 point) {
			if (!bounds.Contains(point)) {
				return false;
			}
			radius = std::clamp(radius, 0.0f, std::min(bounds.Width(), bounds.Height()) * 0.5f);
			if (radius == 0.0f) {
				return true;
			}
			const float x = std::clamp(point.X, bounds.Min.X + radius, bounds.Max.X - radius);
			const float y = std::clamp(point.Y, bounds.Min.Y + radius, bounds.Max.Y - radius);
			const float dx = point.X - x;
			const float dy = point.Y - y;
			return dx * dx + dy * dy <= radius * radius;
		}

		bool Allowed(const Rect &clip, std::span<const RasterMaskRegion> masks, const Vector2 point) {
			if (!clip.Contains(point)) {
				return false;
			}
			for (const RasterMaskRegion &mask : masks) {
				if (!InsideRounded(mask.Bounds, mask.Radius, point)) {
					return false;
				}
			}
			return true;
		}

		void Blend(LinearPixel &destination, Color3 colour, float alpha) {
			alpha = Clamp(alpha);
			const float output = alpha + destination.Alpha * (1.0f - alpha);
			if (output <= 0.0f) {
				return;
			}
			const Color3 mixed = colour * alpha + destination.Color * (destination.Alpha * (1.0f - alpha));
			destination.Color = {mixed.R / output, mixed.G / output, mixed.B / output};
			destination.Alpha = output;
		}

		Color3 GradientColour(const DrawList &list, const DrawCommand &command, Vector2 point, float &alpha) {
			Color3 colour = command.Tint;
			alpha = 1.0f - command.Transparency;
			if (command.Gradient < 0 || static_cast<size_t>(command.Gradient) >= list.Gradients.size()) {
				return colour;
			}
			const DrawGradient &gradient = list.Gradients[static_cast<size_t>(command.Gradient)];
			const Vector2 delta = point - gradient.Origin;
			const float denominator = gradient.Axis.MagnitudeSquared();
			const float progress = denominator > 0.0f ? delta.Dot(gradient.Axis) / denominator : 0.0f;
			colour = colour * gradient.Color.Evaluate(progress);
			alpha *= 1.0f - gradient.Transparency.Evaluate(progress);
			return colour;
		}

		const ReferenceImage *FindAsset(std::span<const ReferenceAsset> assets, core::Name name) {
			for (const ReferenceAsset &asset : assets) {
				if (asset.Name == name && asset.Image.Valid()) {
					return &asset.Image;
				}
			}
			return nullptr;
		}

		ReferencePixel Sample(const ReferenceImage &image, float u, float v, bool tile) {
			if (tile) {
				u -= std::floor(u);
				v -= std::floor(v);
			} else {
				u = Clamp(u);
				v = Clamp(v);
			}
			const uint32_t x = std::min(static_cast<uint32_t>(u * image.Width), image.Width - 1);
			const uint32_t y = std::min(static_cast<uint32_t>(v * image.Height), image.Height - 1);
			return image.At(x, y);
		}

		ReferencePixel SampleImage(const DrawCommand &command, const ReferenceImage &image, Vector2 point) {
			const Rect sample =
				command.Sample.Width() <= 0.0f || command.Sample.Height() <= 0.0f
					? Rect{0.0f, 0.0f, static_cast<float>(image.Width), static_cast<float>(image.Height)}
					: command.Sample;
			float u = (point.X - command.Bounds.Min.X) / std::max(command.Bounds.Width(), 1.0f);
			float v = (point.Y - command.Bounds.Min.Y) / std::max(command.Bounds.Height(), 1.0f);
			if (command.Scale == ScaleType::Tile && command.Tile.X > 0.0f && command.Tile.Y > 0.0f) {
				u = (point.X - command.Bounds.Min.X) / command.Tile.X;
				v = (point.Y - command.Bounds.Min.Y) / command.Tile.Y;
			}
			if (command.Scale == ScaleType::Slice && command.SliceCenter.Width() > 0.0f &&
				command.SliceCenter.Height() > 0.0f) {
				const float left = command.SliceCenter.Min.X * command.SliceScale;
				const float right =
					(static_cast<float>(image.Width) - command.SliceCenter.Max.X) * command.SliceScale;
				const float top = command.SliceCenter.Min.Y * command.SliceScale;
				const float bottom =
					(static_cast<float>(image.Height) - command.SliceCenter.Max.Y) * command.SliceScale;
				const auto map =
					[](float value, float extent, float start, float finish, float sourceExtent) {
						if (value < start) return value / std::max(start, 1.0f) * start;
						if (value > extent - finish)
							return sourceExtent - finish + (value - (extent - finish));
						return start + (value - start) / std::max(extent - start - finish, 1.0f) *
										   (sourceExtent - start - finish);
					};
				const float sourceX =
					map(point.X - command.Bounds.Min.X,
						command.Bounds.Width(),
						left,
						right,
						static_cast<float>(image.Width));
				const float sourceY =
					map(point.Y - command.Bounds.Min.Y,
						command.Bounds.Height(),
						top,
						bottom,
						static_cast<float>(image.Height));
				u = sourceX / std::max(static_cast<float>(image.Width), 1.0f);
				v = sourceY / std::max(static_cast<float>(image.Height), 1.0f);
			}
			u = (sample.Min.X + u * sample.Width()) / std::max(static_cast<float>(image.Width), 1.0f);
			v = (sample.Min.Y + v * sample.Height()) / std::max(static_cast<float>(image.Height), 1.0f);
			return Sample(image, u, v, command.Scale == ScaleType::Tile);
		}

		GlyphLoad LoadGlyph(
			const FontPackage &package,
			const ShapedGlyph &glyph,
			uint16_t pixelSize,
			GlyphCache &cache,
			const GlyphCoverage *&out
		) {
			out = nullptr;
			if (!glyph.Face.IsValid() || pixelSize == 0 || pixelSize > MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE) {
				return pixelSize > MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE ? GlyphLoad::Refused : GlyphLoad::Skip;
			}
			const GlyphKey key{glyph.Face, glyph.Index, pixelSize};
			if (const auto found = cache.Entries.find(key); found != cache.Entries.end()) {
				out = &found->second;
				return GlyphLoad::Ready;
			}
			const auto face = std::find_if(
				package.Faces().begin(), package.Faces().end(), [&glyph](const FontPackageFace &candidate) {
					return candidate.Name == glyph.Face;
				}
			);
			if (face == package.Faces().end() || FreeType() == nullptr) {
				return GlyphLoad::Skip;
			}

			FT_Face opened = nullptr;
			if (FT_New_Memory_Face(
					FreeType(),
					reinterpret_cast<const FT_Byte *>(face->Bytes.data()),
					static_cast<FT_Long>(face->Bytes.size()),
					0,
					&opened
				) != 0 ||
				FT_Set_Pixel_Sizes(opened, 0, pixelSize) != 0 ||
				FT_Load_Glyph(opened, glyph.Index, FT_LOAD_RENDER) != 0) {
				if (opened != nullptr) {
					FT_Done_Face(opened);
				}
				return GlyphLoad::Skip;
			}

			const FT_Bitmap &bitmap = opened->glyph->bitmap;
			const bool usable = bitmap.pixel_mode == FT_PIXEL_MODE_GRAY &&
								std::abs(bitmap.pitch) >= static_cast<int>(bitmap.width);
			const size_t pixels = static_cast<size_t>(bitmap.width) * bitmap.rows;
			if (!usable || bitmap.width > MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE ||
				bitmap.rows > MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE ||
				pixels > MAXIMUM_REFERENCE_GLYPH_CACHE_BYTES - cache.Bytes) {
				FT_Done_Face(opened);
				return GlyphLoad::Refused;
			}

			GlyphCoverage coverage;
			coverage.Width = static_cast<uint16_t>(bitmap.width);
			coverage.Height = static_cast<uint16_t>(bitmap.rows);
			coverage.OffsetX = static_cast<float>(opened->glyph->bitmap_left);
			coverage.OffsetY = static_cast<float>(-opened->glyph->bitmap_top);
			coverage.Pixels.resize(pixels);
			if (pixels != 0 && bitmap.buffer != nullptr) {
				for (uint16_t row = 0; row < coverage.Height; row++) {
					const size_t sourceRow = bitmap.pitch >= 0 ? row : coverage.Height - 1 - row;
					std::copy_n(
						bitmap.buffer + sourceRow * static_cast<size_t>(std::abs(bitmap.pitch)),
						coverage.Width,
						coverage.Pixels.data() + static_cast<size_t>(row) * coverage.Width
					);
				}
			}
			FT_Done_Face(opened);
			cache.Bytes += coverage.Pixels.size();
			out = &cache.Entries.emplace(key, std::move(coverage)).first->second;
			return GlyphLoad::Ready;
		}

		void PaintGlyph(
			Target &target,
			const CollectorTransform *transform,
			const Rect &clip,
			std::span<const RasterMaskRegion> masks,
			const GlyphCoverage &coverage,
			float left,
			float top,
			Color3 tint,
			float transparency
		) {
			if (coverage.Width == 0 || coverage.Height == 0) {
				return;
			}
			const Rect bounds = Transform(
				{{left, top},
				 {left + static_cast<float>(coverage.Width), top + static_cast<float>(coverage.Height)}},
				transform
			);
			if (bounds.Width() <= 0.0f || bounds.Height() <= 0.0f) {
				return;
			}
			const int firstX = std::max(0, static_cast<int>(std::floor(bounds.Min.X)));
			const int firstY = std::max(0, static_cast<int>(std::floor(bounds.Min.Y)));
			const int lastX =
				std::min(static_cast<int>(target.Width), static_cast<int>(std::ceil(bounds.Max.X)));
			const int lastY =
				std::min(static_cast<int>(target.Height), static_cast<int>(std::ceil(bounds.Max.Y)));
			for (int y = firstY; y < lastY; y++) {
				for (int x = firstX; x < lastX; x++) {
					const Vector2 point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
					if (!Allowed(clip, masks, point)) {
						continue;
					}
					const uint16_t sourceX = std::min(
						static_cast<uint16_t>((point.X - bounds.Min.X) / bounds.Width() * coverage.Width),
						static_cast<uint16_t>(coverage.Width - 1)
					);
					const uint16_t sourceY = std::min(
						static_cast<uint16_t>((point.Y - bounds.Min.Y) / bounds.Height() * coverage.Height),
						static_cast<uint16_t>(coverage.Height - 1)
					);
					const float alpha =
						coverage.Pixels[static_cast<size_t>(sourceY) * coverage.Width + sourceX] / 255.0f;
					if (alpha > 0.0f) {
						Blend(
							target.Pixels[static_cast<size_t>(y) * target.Width + static_cast<size_t>(x)],
							tint,
							alpha * (1.0f - transparency)
						);
					}
				}
			}
		}

		bool Paint(
			Target &target,
			const DrawList &list,
			const DrawCommand &command,
			std::span<const RasterMaskRegion> masks,
			std::span<const ReferenceAsset> assets,
			const FontPackage *fonts,
			GlyphCache &glyphs
		) {
			const CollectorTransform *transform = FindTransform(list, command.Collector);
			const Rect bounds = Transform(command.Bounds, transform);
			const Rect clip = Transform(command.Clip, transform);
			const int firstX = std::max(0, static_cast<int>(std::floor(bounds.Min.X)));
			const int firstY = std::max(0, static_cast<int>(std::floor(bounds.Min.Y)));
			const int lastX =
				std::min(static_cast<int>(target.Width), static_cast<int>(std::ceil(bounds.Max.X)));
			const int lastY =
				std::min(static_cast<int>(target.Height), static_cast<int>(std::ceil(bounds.Max.Y)));
			const ReferenceImage *image =
				command.Kind == DrawKind::Image ? FindAsset(assets, command.Image) : nullptr;
			if (command.Kind == DrawKind::Text) {
				if (command.Shaping.Glyphs.size() > MAXIMUM_REFERENCE_RASTER_GLYPHS ||
					glyphs.Draws > MAXIMUM_REFERENCE_RASTER_GLYPHS - command.Shaping.Glyphs.size()) {
					return false;
				}
				glyphs.Draws += command.Shaping.Glyphs.size();
				if (fonts == nullptr || command.Shaping.Status != TextShapeStatus::Ok ||
					command.TextSize <= 0) {
					return true;
				}
				std::vector<ShapedLine> lines = command.Shaping.Lines;
				if (lines.empty()) {
					lines.push_back(
						ShapedLine{
							.GlyphCount = static_cast<uint32_t>(command.Shaping.Glyphs.size()),
							.Advance = command.Shaping.Advance,
							.Ascent = static_cast<float>(command.TextSize),
						}
					);
				}
				const float blockHeight = lines.back().Baseline + lines.back().Ascent + lines.back().Descent;
				float textTop = command.Bounds.Min.Y;
				if (command.YAlignment == TextYAlignment::Center) {
					textTop += (command.Bounds.Height() - blockHeight) * 0.5f;
				} else if (command.YAlignment == TextYAlignment::Bottom) {
					textTop = command.Bounds.Max.Y - blockHeight;
				}
				const auto lineFor = [&lines](size_t glyphIndex) -> const ShapedLine & {
					for (const ShapedLine &line : lines) {
						if (glyphIndex >= line.GlyphOffset &&
							glyphIndex < line.GlyphOffset + line.GlyphCount) {
							return line;
						}
					}
					return lines.front();
				};
				for (const ShapedGlyph &glyph : command.Shaping.Glyphs) {
					const size_t glyphIndex = static_cast<size_t>(&glyph - command.Shaping.Glyphs.data());
					const float requested =
						glyph.PixelSize > 0.0f ? glyph.PixelSize : static_cast<float>(command.TextSize);
					if (!std::isfinite(requested) || requested <= 0.0f ||
						requested > MAXIMUM_REFERENCE_GLYPH_PIXEL_SIZE) {
						return false;
					}
					const uint16_t pixelSize = static_cast<uint16_t>(std::lround(requested));
					const GlyphCoverage *coverage = nullptr;
					const GlyphLoad loaded = LoadGlyph(*fonts, glyph, pixelSize, glyphs, coverage);
					if (loaded == GlyphLoad::Refused) {
						return false;
					}
					if (loaded == GlyphLoad::Skip || coverage == nullptr) {
						continue;
					}
					const ShapedLine &line = lineFor(glyphIndex);
					float originX = command.Bounds.Min.X;
					if (command.XAlignment == TextXAlignment::Center) {
						originX += (command.Bounds.Width() - line.Advance) * 0.5f;
					} else if (command.XAlignment == TextXAlignment::Right) {
						originX = command.Bounds.Max.X - line.Advance;
					}
					const float baseline = textTop + line.Baseline + line.Ascent;
					PaintGlyph(
						target,
						transform,
						clip,
						masks,
						*coverage,
						originX + glyph.X + coverage->OffsetX,
						baseline + glyph.Y + coverage->OffsetY,
						glyph.Tint,
						glyph.Transparency
					);
				}
				return true;
			}

			for (int y = firstY; y < lastY; y++) {
				for (int x = firstX; x < lastX; x++) {
					const Vector2 point{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
					if (!Allowed(clip, masks, point) || !InsideRounded(bounds, command.CornerRadius, point)) {
						continue;
					}
					if (command.Kind == DrawKind::Outline) {
						const Rect inside{
							bounds.Min.X + command.Thickness,
							bounds.Min.Y + command.Thickness,
							bounds.Max.X - command.Thickness,
							bounds.Max.Y - command.Thickness
						};
						if (InsideRounded(
								inside, std::max(0.0f, command.CornerRadius - command.Thickness), point
							)) {
							continue;
						}
					}
					Color3 colour;
					float alpha;
					if (command.Kind == DrawKind::Image && image != nullptr) {
						const ReferencePixel sample = SampleImage(command, *image, point);
						colour =
							Color3{sample.R / 255.0f, sample.G / 255.0f, sample.B / 255.0f} * command.Tint;
						alpha = (sample.A / 255.0f) * (1.0f - command.Transparency);
					} else if (command.Kind == DrawKind::Viewport) {
						const bool odd = ((x / 4) + (y / 4)) % 2 != 0;
						colour = odd ? Color3{0.8f, 0.0f, 0.8f} : Color3{0.2f, 0.0f, 0.2f};
						alpha = 1.0f - command.Transparency;
					} else {
						colour = GradientColour(list, command, point, alpha);
					}
					Blend(
						target.Pixels[static_cast<size_t>(y) * target.Width + static_cast<size_t>(x)],
						colour,
						alpha
					);
				}
			}
			return true;
		}

		void Composite(Target &destination, const Target &source, float transparency) {
			for (size_t index = 0; index < destination.Pixels.size(); index++) {
				Blend(
					destination.Pixels[index],
					source.Pixels[index].Color,
					source.Pixels[index].Alpha * (1.0f - transparency)
				);
			}
		}
	}

	bool ReferenceImage::Valid() const {
		size_t pixelCount = 0;
		return PixelCount(Width, Height, pixelCount) && Pixels.size() == pixelCount;
	}

	const ReferencePixel &ReferenceImage::At(uint32_t x, uint32_t y) const {
		return Pixels[static_cast<size_t>(y) * Width + x];
	}

	ReferenceImage RasterizeReference(
		const DrawList &list,
		uint32_t width,
		uint32_t height,
		std::span<const ReferenceAsset> assets,
		const FontPackage *fonts
	) {
		size_t pixelCount = 0;
		size_t groupDepth = 0;
		size_t maskDepth = 0;
		if (!PixelCount(width, height, pixelCount) || !OperationDepths(list, groupDepth, maskDepth) ||
			!FitsByteBudget(pixelCount, groupDepth)) {
			return {};
		}

		Target root{width, height, std::vector<LinearPixel>(pixelCount)};
		std::vector<Target> groups;
		std::vector<float> groupTransparency;
		std::vector<RasterMaskRegion> masks;
		GlyphCache glyphs;
		groups.reserve(groupDepth);
		groupTransparency.reserve(groupDepth);
		masks.reserve(maskDepth);
		size_t operation = 0;
		auto apply = [&](size_t command) {
			while (operation < list.Operations.size() && list.Operations[operation].Command == command) {
				const DrawOperation &entry = list.Operations[operation++];
				const CollectorTransform *transform = FindTransform(list, entry.Collector);
				if (entry.Kind == DrawOperationKind::BeginMask) {
					masks.push_back({Transform(entry.Bounds, transform), entry.CornerRadius});
				} else if (entry.Kind == DrawOperationKind::EndMask && !masks.empty()) {
					masks.pop_back();
				} else if (entry.Kind == DrawOperationKind::BeginGroup) {
					groups.push_back(
						Target{width, height, std::vector<LinearPixel>(static_cast<size_t>(width) * height)}
					);
					groupTransparency.push_back(entry.Transparency);
				} else if (entry.Kind == DrawOperationKind::EndGroup && !groups.empty()) {
					Target completed = std::move(groups.back());
					groups.pop_back();
					const float transparency = groupTransparency.back();
					groupTransparency.pop_back();
					Composite(groups.empty() ? root : groups.back(), completed, transparency);
				}
			}
		};
		for (size_t command = 0; command < list.Commands.size(); command++) {
			apply(command);
			if (!Paint(
					groups.empty() ? root : groups.back(),
					list,
					list.Commands[command],
					masks,
					assets,
					fonts,
					glyphs
				)) {
				return {};
			}
		}
		apply(list.Commands.size());
		while (!groups.empty()) {
			Target completed = std::move(groups.back());
			groups.pop_back();
			const float transparency = groupTransparency.back();
			groupTransparency.pop_back();
			Composite(groups.empty() ? root : groups.back(), completed, transparency);
		}

		ReferenceImage image;
		image.Width = width;
		image.Height = height;
		image.Pixels.resize(pixelCount);
		for (size_t index = 0; index < root.Pixels.size(); index++) {
			const LinearPixel &pixel = root.Pixels[index];
			image.Pixels[index] = {
				ToByte(pixel.Color.R), ToByte(pixel.Color.G), ToByte(pixel.Color.B), ToByte(pixel.Alpha)
			};
		}
		return image;
	}

	uint64_t ReferenceImageHash(const ReferenceImage &image) {
		uint64_t hash = 1469598103934665603ull;
		auto fold = [&hash](uint8_t byte) {
			hash ^= byte;
			hash *= 1099511628211ull;
		};
		for (const uint8_t byte :
			 {static_cast<uint8_t>(image.Width),
			  static_cast<uint8_t>(image.Width >> 8),
			  static_cast<uint8_t>(image.Height),
			  static_cast<uint8_t>(image.Height >> 8)}) {
			fold(byte);
		}
		for (const ReferencePixel pixel : image.Pixels) {
			fold(pixel.R);
			fold(pixel.G);
			fold(pixel.B);
			fold(pixel.A);
		}
		return hash;
	}

	ReferenceComparison CompareReferenceImages(
		const ReferenceImage &expected,
		const ReferenceImage &actual,
		uint8_t channelTolerance,
		float maximumChangedArea
	) {
		ReferenceComparison result;
		if (!expected.Valid() || !actual.Valid() || expected.Width != actual.Width ||
			expected.Height != actual.Height) {
			result.ChangedPixels = std::max(expected.Pixels.size(), actual.Pixels.size());
			result.ChangedArea = 1.0f;
			result.MaximumChannelDifference = std::numeric_limits<uint8_t>::max();
			result.WithinChangedAreaCap = false;
			return result;
		}
		for (size_t index = 0; index < expected.Pixels.size(); index++) {
			const ReferencePixel before = expected.Pixels[index];
			const ReferencePixel after = actual.Pixels[index];
			const uint8_t difference = std::max({
				static_cast<uint8_t>(std::abs(static_cast<int>(before.R) - static_cast<int>(after.R))),
				static_cast<uint8_t>(std::abs(static_cast<int>(before.G) - static_cast<int>(after.G))),
				static_cast<uint8_t>(std::abs(static_cast<int>(before.B) - static_cast<int>(after.B))),
				static_cast<uint8_t>(std::abs(static_cast<int>(before.A) - static_cast<int>(after.A))),
			});
			result.MaximumChannelDifference = std::max(result.MaximumChannelDifference, difference);
			result.ChangedPixels += difference > channelTolerance ? 1 : 0;
		}
		result.ChangedArea = expected.Pixels.empty()
								 ? 0.0f
								 : static_cast<float>(result.ChangedPixels) / expected.Pixels.size();
		result.WithinChangedAreaCap = result.ChangedArea <= maximumChangedArea;
		return result;
	}
}
