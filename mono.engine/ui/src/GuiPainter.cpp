#include <engine/ui/Fonts.hpp>
#include <engine/ui/GuiPainter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
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

		struct GradientSpace {
			ImVec2 Origin;
			float Scale = 1.0f;

			ImVec2 Point(const core::Vector2 &value) const {
				return ImVec2{Origin.x + value.X * Scale, Origin.y + value.Y * Scale};
			}
		};

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

		size_t PaintText(const DrawCommand &command, ImDrawList *into, const GradientSpace &space) {
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
		size_t drawn = 0;

		for (const DrawCommand &command : list.Commands) {
			if (command.Spatial) {
				continue;
			}
			// **Pushed with intersection, so a caller may already have one.** A
			// panel drawing a canvas inside itself has a clip of its own, and a
			// replacing push would let an element draw over the panel's border.
			into->PushClipRect(space.Point(command.Clip.Min), space.Point(command.Clip.Max), true);

			const ImVec2 min = space.Point(command.Bounds.Min);
			const ImVec2 max = space.Point(command.Bounds.Max);
			const ImU32 tint = Colour(command.Tint, command.Transparency);
			const float radius = command.CornerRadius * space.Scale;
			const int firstVertex = into->VtxBuffer.Size;

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
				drawn += PaintText(command, into, space);
				break;
			}
			// Shade before turning, because the ramp is measured against the
			// unrotated bounds the compile resolved it from.
			if (command.Gradient >= 0 && static_cast<size_t>(command.Gradient) < list.Gradients.size()) {
				ShadeVertices(
					into, firstVertex, list.Gradients[static_cast<size_t>(command.Gradient)], space
				);
			}

			RotateVertices(into, firstVertex, min, max, command.Rotation);

			into->PopClipRect();
		}

		return drawn;
	}
}
