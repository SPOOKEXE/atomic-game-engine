#include <engine/render/InterfaceMesh.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {

	namespace {
		using core::Rect;
		using core::Vector2;

		// The colour a command draws in, packed.
		//
		// **Transparency, not alpha** — `gui` spells it Roblox's way round,
		// where 0 is opaque, and converting here rather than at each use is what
		// stops one of them being written backwards.
		uint32_t Packed(const gui::DrawCommand &command) {
			const auto channel = [](float value) {
				return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			const uint8_t alpha = channel(1.0f - command.Transparency);
			return static_cast<uint32_t>(channel(command.Tint.R)) |
				   (static_cast<uint32_t>(channel(command.Tint.G)) << 8) |
				   (static_cast<uint32_t>(channel(command.Tint.B)) << 16) |
				   (static_cast<uint32_t>(alpha) << 24);
		}

		bool SameClip(const Rect &left, const Rect &right) {
			return left.Min.X == right.Min.X && left.Min.Y == right.Min.Y &&
				   left.Max.X == right.Max.X && left.Max.Y == right.Max.Y;
		}
	}

	InterfaceMesh::Rotation InterfaceMesh::TurnOf(const gui::DrawCommand &command) {
	Rotation turn;
		turn.Pivot = core::Vector2{
			(command.Bounds.Min.X + command.Bounds.Max.X) * 0.5f,
			(command.Bounds.Min.Y + command.Bounds.Max.Y) * 0.5f,
		};

		if (command.Rotation == 0.0f) {
			return turn;
		}

		// **Degrees on the wire, radians in the arithmetic.**
		// `Element::Rotation` is degrees because that is what a Roblox
		// author types, and converting at each use is how one of them ends
		// up missing the factor.
		constexpr float TO_RADIANS = 3.14159265f / 180.0f;
		const float angle = command.Rotation * TO_RADIANS;
		turn.Sine = std::sin(angle);
		turn.Cosine = std::cos(angle);
		return turn;
	}

	Vector2 InterfaceMesh::WhiteUV(const GlyphAtlas &atlas) {
		return atlas.WhiteTexel();
	}

	void InterfaceMesh::Push(
		const Rect &bounds, const Rect &uv, uint32_t colour, const Rotation &turn
	) {
		// **Sixteen-bit indices, so a quad past 65 532 vertices is dropped
		// rather than wrapped.** Wrapping would draw a triangle between three
		// unrelated corners of the interface — a stripe across the screen that
		// looks like a driver fault. An interface that large is an
		// instrumentation bug, and `Batches` is where a caller sees it stopped.
		if (VertexData.size() + 4 > 0xFFFFu) {
			return;
		}

		const auto base = static_cast<uint16_t>(VertexData.size());

		const auto vertex = [&](float x, float y, float u, float v) {
			InterfaceVertex made;
			made.X = x;
			made.Y = y;
			made.U = u;
			made.V = v;
			made.R = static_cast<uint8_t>(colour & 0xFFu);
			made.G = static_cast<uint8_t>((colour >> 8) & 0xFFu);
			made.B = static_cast<uint8_t>((colour >> 16) & 0xFFu);
			made.A = static_cast<uint8_t>((colour >> 24) & 0xFFu);
			VertexData.push_back(made);
		};

		// **Turned about the element's own centre, not the quad's.** A glyph
		// inside a rotated label rotates *with the label* — around the label's
		// middle — and a per-quad rotation would spin every letter on the spot
		// while leaving the run in a straight line, which is a distinctive and
		// completely wrong look. That is why the pivot is passed in rather than
		// derived from `bounds` here.
		const auto placed = [&](float x, float y, float u, float v) {
			if (turn.Sine == 0.0f && turn.Cosine == 1.0f) {
				vertex(x, y, u, v);
				return;
			}

			const float localX = x - turn.Pivot.X;
			const float localY = y - turn.Pivot.Y;

			vertex(
				turn.Pivot.X + localX * turn.Cosine - localY * turn.Sine,
				turn.Pivot.Y + localX * turn.Sine + localY * turn.Cosine,
				u,
				v
			);
		};

		placed(bounds.Min.X, bounds.Min.Y, uv.Min.X, uv.Min.Y);
		placed(bounds.Max.X, bounds.Min.Y, uv.Max.X, uv.Min.Y);
		placed(bounds.Max.X, bounds.Max.Y, uv.Max.X, uv.Max.Y);
		placed(bounds.Min.X, bounds.Max.Y, uv.Min.X, uv.Max.Y);

		// Two triangles, wound the same way as everything else this renderer
		// submits. A quad wound the other way is invisible under back-face
		// culling and looks exactly like an element that failed to lay out.
		const uint16_t order[6] = {0, 1, 2, 0, 2, 3};
		for (const uint16_t offset : order) {
			IndexData.push_back(static_cast<uint16_t>(base + offset));
		}
	}

	void InterfaceMesh::Build(const gui::DrawList &list, const GlyphAtlas &atlas) {
		// Cleared rather than reconstructed, so a steady interface stops
		// allocating after the frame that reached its high-water mark.
		VertexData.clear();
		IndexData.clear();
		BatchData.clear();

		const Vector2 white = atlas.Ready() ? atlas.WhiteTexel() : Vector2{0.0f, 0.0f};
		const Rect solid{white, white};

		for (const gui::DrawCommand &command : list.Commands) {
			const bool textured = command.Kind == gui::DrawKind::Image;
			const core::Name image = textured ? command.Image : core::Name{};

			// **A new batch when the scissor or the texture changes, and not
			// otherwise.** A clip is pipeline state rather than a vertex
			// attribute, so two elements clipped differently cannot be one draw
			// however alike their pixels are — and merging runs that *do* match
			// is the whole reason this is called a batched pipeline.
			const bool fresh = BatchData.empty() || !SameClip(BatchData.back().Clip, command.Clip) ||
							   BatchData.back().Image != image;

			if (fresh) {
				InterfaceBatch batch;
				batch.FirstIndex = static_cast<uint32_t>(IndexData.size());
				batch.Clip = command.Clip;
				batch.Image = image;
				BatchData.push_back(batch);
			}

			const uint32_t colour = Packed(command);
			const InterfaceMesh::Rotation turn = TurnOf(command);

			switch (command.Kind) {
			case gui::DrawKind::Rectangle:
				// **Corner rounding is not done here and the field is not
				// ignored.** A rounded corner is either more geometry or a
				// shader that knows the radius, and the second is what this
				// pipeline will do — the radius travels to the backend rather
				// than being tessellated into a fan whose segment count nobody
				// chose. Until that shader exists a corner draws square, which
				// is visibly plain rather than wrong.
				Push(command.Bounds, solid, colour, turn);
				break;

			case gui::DrawKind::Outline: {
				// Four quads rather than a line primitive: a line's width is a
				// device setting with no portable guarantee, and an outline
				// that is one pixel on one driver and three on another is the
				// kind of difference nobody reproduces.
				const float thickness = std::max(command.Thickness, 1.0f);
				const Rect &box = command.Bounds;

				Push(Rect{box.Min, Vector2{box.Max.X, box.Min.Y + thickness}}, solid, colour, turn);
				Push(Rect{Vector2{box.Min.X, box.Max.Y - thickness}, box.Max}, solid, colour, turn);
				Push(
					Rect{
						Vector2{box.Min.X, box.Min.Y + thickness},
						Vector2{box.Min.X + thickness, box.Max.Y - thickness}
					},
					solid,
					colour
				, turn);
				Push(
					Rect{
						Vector2{box.Max.X - thickness, box.Min.Y + thickness},
						Vector2{box.Max.X, box.Max.Y - thickness}
					},
					solid,
					colour
				, turn);
				break;
			}

			case gui::DrawKind::Image:
				// **The whole image, and the fit modes are the backend's.**
				// Nine-slice and tiling change how many quads there are and
				// against what source rectangle, and doing them here would mean
				// this type knew the image's pixel size — which is exactly what
				// `gui::DrawCommand::Image` being a content name says it must
				// not. The batch carries the name; the backend that resolved it
				// has the size.
				Push(command.Bounds, Rect{Vector2{0.0f, 0.0f}, Vector2{1.0f, 1.0f}}, colour, turn);
				break;

			case gui::DrawKind::Text: {
				if (!atlas.Ready()) {
					// No fonts staged. The interface still draws and the text is
					// visibly absent, which is what a client whose fonts failed
					// to stage should show.
					break;
				}

				const std::string_view text = command.Text;
				const Typeface face = Typeface::Interface;

				// Placed from the top-left of `Bounds`, on the baseline.
				// **Alignment is not applied here** for `CornerRadius`'s reason
				// one case up: `gui` already resolved where the run goes and the
				// two alignment fields are how it wants the *backend* to place
				// it within that. Left and top is the honest default until the
				// pass reads them.
				float penX = command.Bounds.Min.X;
				const float baseline = command.Bounds.Min.Y + atlas.PixelSize();

				for (const char character : text) {
					const auto codepoint = static_cast<char32_t>(static_cast<unsigned char>(character));
					const Glyph *glyph = atlas.Find(face, codepoint);
					if (glyph == nullptr) {
						continue;
					}

					if (glyph->Width > 0 && glyph->Height > 0) {
						const float left = penX + glyph->OffsetX;
						const float top = baseline + glyph->OffsetY;

						const auto width = static_cast<float>(atlas.Width());
						const auto height = static_cast<float>(atlas.Height());

						Push(
							Rect{
								Vector2{left, top},
								Vector2{
									left + static_cast<float>(glyph->Width),
									top + static_cast<float>(glyph->Height)
								}
							},
							Rect{
								Vector2{
									static_cast<float>(glyph->X) / width,
									static_cast<float>(glyph->Y) / height
								},
								Vector2{
									static_cast<float>(glyph->X + glyph->Width) / width,
									static_cast<float>(glyph->Y + glyph->Height) / height
								}
							},
							colour
						, turn);
					}

					penX += glyph->Advance;
				}
				break;
			}
			}

			BatchData.back().IndexCount =
				static_cast<uint32_t>(IndexData.size()) - BatchData.back().FirstIndex;
		}

		// **Empty batches dropped at the end rather than avoided at the
		// start.** Whether a command produces geometry is not known until it has
		// been tried — a text run of unbaked codepoints produces none — and a
		// batch with no indices is a scissor change and a pipeline bind for
		// nothing.
		BatchData.erase(
			std::remove_if(
				BatchData.begin(),
				BatchData.end(),
				[](const InterfaceBatch &batch) { return batch.IndexCount == 0; }
			),
			BatchData.end()
		);
	}
}
