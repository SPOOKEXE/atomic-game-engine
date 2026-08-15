#include <engine/ui/Fonts.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace engine::ui {

	namespace {
		using gui::DrawCommand;
		using gui::DrawKind;
		using gui::FontFace;
		using gui::ScaleType;
		using gui::TextXAlignment;
		using gui::TextYAlignment;

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

		struct Space {
			ImVec2 Origin;
			float Scale = 1.0f;

			ImVec2 Point(const core::Vector2 &value) const {
				return ImVec2{Origin.x + value.X * Scale, Origin.y + value.Y * Scale};
			}
		};

		// The four corners of a rectangle, rotated about its own centre.
		//
		// imgui has no rotated-rectangle primitive, so a rotated element is
		// drawn as a convex quad. **Only rectangles rotate.** Text and images
		// are drawn upright at the rotated rectangle's bounding centre, which
		// is stated rather than discovered: rotating a glyph run needs per-glyph
		// quads, and rotating a nine-slice needs the slices rotated
		// individually. Filed as `D00023`.
		void RotatedCorners(const ImVec2 &min, const ImVec2 &max, float degrees, ImVec2 out[4]) {
			const ImVec2 centre{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
			const float radians = degrees * 3.14159265358979323846f / 180.0f;
			const float cosine = std::cos(radians);
			const float sine = std::sin(radians);

			const ImVec2 raw[4]{
				{min.x, min.y},
				{max.x, min.y},
				{max.x, max.y},
				{min.x, max.y},
			};

			for (int index = 0; index < 4; index++) {
				const float x = raw[index].x - centre.x;
				const float y = raw[index].y - centre.y;
				out[index] = ImVec2{centre.x + x * cosine - y * sine, centre.y + x * sine + y * cosine};
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
			const DrawCommand &command, ImDrawList *into, const Space &space, const ImageSource &images
		) {
			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const ImU32 tint = Colour(command.Tint, command.Transparency);

			const ImageSource::Resolved resolved =
				images.Resolve ? images.Resolve(command.Image) : ImageSource::Resolved{};
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

		size_t PaintText(const DrawCommand &command, ImDrawList *into, const Space &space) {
			const std::string_view text = command.Text;
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

			into->AddText(
				font,
				size,
				ImVec2{x, y},
				Colour(command.Tint, command.Transparency),
				text.data(),
				text.data() + text.size(),
				wrap
			);
			return 1;
		}
	}

	size_t PaintGui(
		const gui::DrawList &list, ImDrawList *into, const PaintTarget &target, const ImageSource &images
	) {
		if (into == nullptr) {
			return 0;
		}

		const Space space{target.Origin, target.Scale};
		size_t drawn = 0;

		for (const DrawCommand &command : list.Commands) {
			// **Pushed with intersection, so a caller may already have one.** A
			// panel drawing a canvas inside itself has a clip of its own, and a
			// replacing push would let an element draw over the panel's border.
			into->PushClipRect(space.Point(command.Clip.Min), space.Point(command.Clip.Max), true);

			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const ImU32 tint = Colour(command.Tint, command.Transparency);
			const float radius = command.CornerRadius * space.Scale;

			switch (command.Kind) {
			case DrawKind::Rectangle:
				if (command.Rotation != 0.0f) {
					ImVec2 corners[4];
					RotatedCorners(min, max, command.Rotation, corners);
					into->AddConvexPolyFilled(corners, 4, tint);
				} else {
					into->AddRectFilled(min, max, tint, radius);
				}
				drawn++;
				break;

			case DrawKind::Outline:
				if (command.Rotation != 0.0f) {
					ImVec2 corners[4];
					RotatedCorners(min, max, command.Rotation, corners);
					into->AddPolyline(corners, 4, tint, ImDrawFlags_Closed, command.Thickness * space.Scale);
				} else {
					into->AddRect(min, max, tint, radius, ImDrawFlags_None, command.Thickness * space.Scale);
				}
				drawn++;
				break;

			case DrawKind::Image:
				drawn += PaintImage(command, into, space, images);
				break;

			case DrawKind::Text:
				drawn += PaintText(command, into, space);
				break;
			}

			into->PopClipRect();
		}

		return drawn;
	}
}
