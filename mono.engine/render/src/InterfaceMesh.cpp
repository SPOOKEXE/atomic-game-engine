#include <engine/render/InterfaceMesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace engine::render {

	InterfaceScissor
	ScissorFor(const core::Rect &clip, const core::Vector2 &canvas, const core::Vector2 &targetPixels) {
		const float scaleX = targetPixels.X > 0.0f && canvas.X > 0.0f ? targetPixels.X / canvas.X : 1.0f;
		const float scaleY = targetPixels.Y > 0.0f && canvas.Y > 0.0f ? targetPixels.Y / canvas.Y : 1.0f;

		const float left = std::max(0.0f, std::floor(clip.Min.X * scaleX));
		const float top = std::max(0.0f, std::floor(clip.Min.Y * scaleY));
		const float right = std::max(left, std::ceil(clip.Max.X * scaleX));
		const float bottom = std::max(top, std::ceil(clip.Max.Y * scaleY));

		InterfaceScissor scissor;
		scissor.X = static_cast<int32_t>(left);
		scissor.Y = static_cast<int32_t>(top);
		scissor.Width = static_cast<int32_t>(right - left);
		scissor.Height = static_cast<int32_t>(bottom - top);
		return scissor;
	}

	namespace {
		using core::Rect;
		using core::Vector2;

		constexpr float PI = 3.14159265358979323846f;
		constexpr size_t CORNER_STEPS = 4;

		uint32_t Packed(const core::Color3 &tint, float transparency) {
			const auto channel = [](float value) {
				return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
			};
			const uint8_t alpha = channel(1.0f - transparency);
			return static_cast<uint32_t>(channel(tint.R)) | (static_cast<uint32_t>(channel(tint.G)) << 8) |
				   (static_cast<uint32_t>(channel(tint.B)) << 16) | (static_cast<uint32_t>(alpha) << 24);
		}

		uint32_t Packed(const gui::DrawCommand &command) {
			return Packed(command.Tint, command.Transparency);
		}

		bool SameClip(const Rect &left, const Rect &right) {
			return left.Min.X == right.Min.X && left.Min.Y == right.Min.Y && left.Max.X == right.Max.X &&
				   left.Max.Y == right.Max.Y;
		}

		Typeface FaceFor(gui::FontFace face) {
			switch (face) {
			case gui::FontFace::Code:
				return Typeface::Monospace;
			case gui::FontFace::Bold:
				return Typeface::Display;
			case gui::FontFace::Regular:
			case gui::FontFace::Italic:
				return Typeface::Interface;
			}
			return Typeface::Interface;
		}

		float WidthOf(const GlyphAtlas &atlas, Typeface face, std::string_view text, float scale) {
			return atlas.Measure(face, text) * scale;
		}

		// --- rich text --------------------------------------------------------
		//
		// **The whole feature is "look up the style at a byte offset".** A
		// `gui::DrawSpan` is a range over the command's own string, so measuring
		// and drawing both walk the run character by character and ask which
		// span covers the one in hand. That is what lets one pass lay out mixed
		// faces and sizes with the *renderer's* metrics, which is the property
		// `gui/RichText.hpp` was arranged around.

		const gui::DrawSpan *SpanAt(const gui::DrawCommand &command, size_t offset) {
			// Linear, because a marked-up run has a handful of spans and a
			// binary search over three entries is slower than looking at them.
			for (const gui::DrawSpan &span : command.Spans) {
				if (offset >= span.Begin && offset < span.End) {
					return &span;
				}
			}
			return nullptr;
		}

		// How one character is drawn, after any span covering it.
		struct RunStyle {
			Typeface Face = Typeface::Interface;
			float Scale = 1.0f;
			uint32_t Colour = 0xFFFFFFFFu;
			bool Underline = false;
			bool Strike = false;
		};

		RunStyle StyleAt(
			const gui::DrawCommand &command,
			size_t offset,
			const RunStyle &base,
			float atlasPixels,
			bool colouring
		) {
			const gui::DrawSpan *span = SpanAt(command, offset);
			if (span == nullptr) {
				return base;
			}

			RunStyle style = base;
			style.Face = FaceFor(span->Font);
			if (span->Size > 0 && atlasPixels > 0.0f) {
				style.Scale = static_cast<float>(span->Size) / atlasPixels;
			}

			// **The stroke pass keeps the stroke's colour whatever the span
			// says.** A span's colour is the colour of the letter, and an
			// outline drawn in it would be an outline nobody could see.
			if (colouring) {
				style.Colour = Packed(span->Tint, span->Transparency);
			}
			style.Underline = span->Underline;
			style.Strike = span->Strike;
			return style;
		}

		// The width of one byte range, with each character's own face and size.
		float WidthOfRange(
			const GlyphAtlas &atlas,
			const gui::DrawCommand &command,
			size_t begin,
			size_t end,
			const RunStyle &base,
			float atlasPixels
		) {
			// The overwhelmingly common case is a run with no spans at all, and
			// `GlyphAtlas::Measure` is what that path has always used.
			if (command.Spans.empty()) {
				return WidthOf(
					atlas, base.Face, std::string_view(command.Text).substr(begin, end - begin), base.Scale
				);
			}

			float width = 0.0f;
			for (size_t at = begin; at < end; at++) {
				const RunStyle style = StyleAt(command, at, base, atlasPixels, false);
				const auto codepoint = static_cast<char32_t>(static_cast<unsigned char>(command.Text[at]));
				if (const Glyph *glyph = atlas.Find(style.Face, codepoint)) {
					width += glyph->Advance * style.Scale;
				}
			}
			return width;
		}

		// One drawn line, as a range over the command's own string.
		//
		// **Ranges and not copies, and that is what rich text needed.** A span
		// is a byte offset into `DrawCommand::Text`, so a line held as a
		// substring has lost the one thing the style lookup asks for. It also
		// stops the wrap allocating a string per line per frame, which it did.
		struct Line {
			size_t Begin = 0;
			size_t End = 0;

			// Whether an ellipsis is drawn after the range. The dots are not in
			// the string, so they carry the run's own style rather than that of
			// whatever span happened to end there.
			bool Ellipsis = false;
		};

		std::vector<Line> LinesOf(
			const gui::DrawCommand &command, const GlyphAtlas &atlas, const RunStyle &base, float atlasPixels
		) {
			std::vector<Line> lines;
			const float available = std::max(command.Bounds.Width(), 0.0f);

			size_t paragraphStart = 0;
			while (paragraphStart <= command.Text.size()) {
				const size_t newline = command.Text.find('\n', paragraphStart);
				const size_t paragraphEnd = newline == std::string::npos ? command.Text.size() : newline;

				if (!command.Wrapped || available <= 0.0f || paragraphStart == paragraphEnd) {
					lines.push_back(Line{paragraphStart, paragraphEnd, false});
				} else {
					size_t first = paragraphStart;
					while (first < paragraphEnd) {
						while (first < paragraphEnd && command.Text[first] == ' ') {
							first++;
						}
						if (first >= paragraphEnd) {
							break;
						}

						size_t fit = first;
						size_t word = std::string::npos;
						for (size_t end = first + 1; end <= paragraphEnd; end++) {
							if (command.Text[end - 1] == ' ') {
								word = end - 1;
							}
							if (WidthOfRange(atlas, command, first, end, base, atlasPixels) > available) {
								break;
							}
							fit = end;
						}

						if (fit == first) {
							fit = first + 1;
						} else if (fit < paragraphEnd && word != std::string::npos && word > first) {
							fit = word;
						}

						lines.push_back(Line{first, fit, false});
						first = fit;
					}
				}

				if (newline == std::string::npos) {
					break;
				}
				paragraphStart = newline + 1;
			}

			if (lines.empty()) {
				lines.push_back(Line{});
			}

			if (!command.Wrapped && command.Truncate == gui::TextTruncate::AtEnd && available > 0.0f) {
				constexpr std::string_view dots = "...";
				const float dotsWidth = WidthOf(atlas, base.Face, dots, base.Scale);

				for (Line &line : lines) {
					if (WidthOfRange(atlas, command, line.Begin, line.End, base, atlasPixels) <= available) {
						continue;
					}

					line.Ellipsis = true;
					while (line.End > line.Begin &&
						   WidthOfRange(atlas, command, line.Begin, line.End, base, atlasPixels) + dotsWidth >
							   available) {
						line.End--;
					}
				}
			}

			return lines;
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

		const float angle = command.Rotation * PI / 180.0f;
		turn.Sine = std::sin(angle);
		turn.Cosine = std::cos(angle);
		return turn;
	}

	Vector2 InterfaceMesh::WhiteUV(const GlyphAtlas &atlas) {
		return atlas.WhiteTexel();
	}

	uint32_t InterfaceMesh::Shade(uint32_t colour, const Vector2 &point) const {
		if (Ramp == nullptr) {
			return colour;
		}

		const float span = Ramp->Axis.X * Ramp->Axis.X + Ramp->Axis.Y * Ramp->Axis.Y;

		// A ramp with no length is one flat colour rather than a division by
		// zero, and the flat colour is the one at the start - which is what a
		// zero-width element ends up with and is invisible anyway.
		const float where = span > 0.0f ? std::clamp(
											  ((point.X - Ramp->Origin.X) * Ramp->Axis.X +
											   (point.Y - Ramp->Origin.Y) * Ramp->Axis.Y) /
												  span,
											  0.0f,
											  1.0f
										  )
										: 0.0f;

		const core::Color3 tint = Ramp->Color.Evaluate(where);
		const float fade = Ramp->Transparency.Evaluate(where);

		// **Multiplied into the colour and added to the transparency**, which is
		// Roblox's composition and is what makes a white ramp a no-op: a
		// gradient is a modifier on what the element already draws rather than a
		// second fill drawn over it.
		const float red = static_cast<float>(colour & 0xFFu) * std::clamp(tint.R, 0.0f, 1.0f);
		const float green = static_cast<float>((colour >> 8) & 0xFFu) * std::clamp(tint.G, 0.0f, 1.0f);
		const float blue = static_cast<float>((colour >> 16) & 0xFFu) * std::clamp(tint.B, 0.0f, 1.0f);
		const float alpha = static_cast<float>((colour >> 24) & 0xFFu) * std::clamp(1.0f - fade, 0.0f, 1.0f);

		const auto byte = [](float value) {
			return static_cast<uint32_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
		};
		return byte(red) | (byte(green) << 8) | (byte(blue) << 16) | (byte(alpha) << 24);
	}

	void InterfaceMesh::PushVertex(
		const Vector2 &point, const Rect &bounds, const Rect &uv, uint32_t colour, const Rotation &turn
	) {
		float x = point.X;
		float y = point.Y;
		if (turn.Sine != 0.0f || turn.Cosine != 1.0f) {
			const float localX = x - turn.Pivot.X;
			const float localY = y - turn.Pivot.Y;
			x = turn.Pivot.X + localX * turn.Cosine - localY * turn.Sine;
			y = turn.Pivot.Y + localX * turn.Sine + localY * turn.Cosine;
		}

		const float across = bounds.Width() > 0.0f ? (point.X - bounds.Min.X) / bounds.Width() : 0.0f;
		const float down = bounds.Height() > 0.0f ? (point.Y - bounds.Min.Y) / bounds.Height() : 0.0f;

		const uint32_t shaded = Shade(colour, point);

		InterfaceVertex made;
		made.X = x;
		made.Y = y;
		made.U = uv.Min.X + uv.Width() * across;
		made.V = uv.Min.Y + uv.Height() * down;
		made.R = static_cast<uint8_t>(shaded & 0xFFu);
		made.G = static_cast<uint8_t>((shaded >> 8) & 0xFFu);
		made.B = static_cast<uint8_t>((shaded >> 16) & 0xFFu);
		made.A = static_cast<uint8_t>((shaded >> 24) & 0xFFu);
		VertexData.push_back(made);
	}

	void InterfaceMesh::PushShaded(
		std::span<const Vector2> polygon,
		const Rect &bounds,
		const Rect &uv,
		uint32_t colour,
		const Rotation &turn
	) {
		if (polygon.size() < 3) {
			return;
		}

		// Where the ramp changes slope, which is where the shape has to be cut.
		// Both sequences contribute, because a colour stop and a transparency
		// stop are equally a corner in the function being interpolated.
		RampStops.clear();
		RampStops.push_back(0.0f);
		RampStops.push_back(1.0f);
		if (Ramp != nullptr) {
			for (uint32_t index = 0; index < Ramp->Color.Count; index++) {
				RampStops.push_back(std::clamp(Ramp->Color.Keypoints[index].Time, 0.0f, 1.0f));
			}
			for (uint32_t index = 0; index < Ramp->Transparency.Count; index++) {
				RampStops.push_back(std::clamp(Ramp->Transparency.Keypoints[index].Time, 0.0f, 1.0f));
			}
		}
		std::sort(RampStops.begin(), RampStops.end());
		RampStops.erase(std::unique(RampStops.begin(), RampStops.end()), RampStops.end());

		const float span = Ramp != nullptr ? Ramp->Axis.X * Ramp->Axis.X + Ramp->Axis.Y * Ramp->Axis.Y : 0.0f;

		// Where a point falls on the ramp, unclamped - the clamp belongs in
		// `Shade`, and clamping here would collapse every point beyond the ends
		// into one band and cut the shape in the wrong place.
		const auto along = [&](const Vector2 &point) {
			if (!(span > 0.0f)) {
				return 0.0f;
			}
			return ((point.X - Ramp->Origin.X) * Ramp->Axis.X + (point.Y - Ramp->Origin.Y) * Ramp->Axis.Y) /
				   span;
		};

		// Sutherland-Hodgman against one half-plane of the ramp's parameter.
		// Convex in, convex out, which is what lets each piece be a fan.
		const auto clip = [&](std::vector<Vector2> &shape, float limit, bool keepBelow) {
			ClipBack.clear();
			for (size_t index = 0; index < shape.size(); index++) {
				const Vector2 &current = shape[index];
				const Vector2 &next = shape[(index + 1) % shape.size()];
				const float here = along(current) - limit;
				const float there = along(next) - limit;
				const bool insideHere = keepBelow ? here <= 0.0f : here >= 0.0f;
				const bool insideThere = keepBelow ? there <= 0.0f : there >= 0.0f;

				if (insideHere) {
					ClipBack.push_back(current);
				}
				if (insideHere != insideThere) {
					const float denominator = here - there;
					const float alpha = denominator != 0.0f ? here / denominator : 0.0f;
					ClipBack.push_back(
						Vector2{
							current.X + (next.X - current.X) * alpha,
							current.Y + (next.Y - current.Y) * alpha,
						}
					);
				}
			}
			shape.swap(ClipBack);
		};

		for (size_t stop = 0; stop + 1 < RampStops.size(); stop++) {
			ClipFront.assign(polygon.begin(), polygon.end());

			// The first band keeps everything before the ramp starts and the last
			// keeps everything after it ends, because the ramp is clamped outside
			// `[0, 1]` and the shape may well reach past both.
			if (stop > 0) {
				clip(ClipFront, RampStops[stop], false);
			}
			if (stop + 2 < RampStops.size()) {
				clip(ClipFront, RampStops[stop + 1], true);
			}

			if (ClipFront.size() < 3 || VertexData.size() + ClipFront.size() > 0xFFFFu) {
				continue;
			}

			const auto base = static_cast<uint16_t>(VertexData.size());
			for (const Vector2 &point : ClipFront) {
				PushVertex(point, bounds, uv, colour, turn);
			}
			for (size_t index = 1; index + 1 < ClipFront.size(); index++) {
				IndexData.push_back(base);
				IndexData.push_back(static_cast<uint16_t>(base + index));
				IndexData.push_back(static_cast<uint16_t>(base + index + 1));
			}
		}
	}

	void InterfaceMesh::Push(const Rect &bounds, const Rect &uv, uint32_t colour, const Rotation &turn) {
		if (Ramp != nullptr) {
			const std::array<Vector2, 4> corners{
				Vector2{bounds.Min.X, bounds.Min.Y},
				Vector2{bounds.Max.X, bounds.Min.Y},
				Vector2{bounds.Max.X, bounds.Max.Y},
				Vector2{bounds.Min.X, bounds.Max.Y},
			};
			PushShaded(corners, bounds, uv, colour, turn);
			return;
		}

		if (VertexData.size() + 4 > 0xFFFFu) {
			return;
		}

		const auto base = static_cast<uint16_t>(VertexData.size());
		const auto vertex = [&](float x, float y, float u, float v) {
			if (turn.Sine != 0.0f || turn.Cosine != 1.0f) {
				const float localX = x - turn.Pivot.X;
				const float localY = y - turn.Pivot.Y;
				x = turn.Pivot.X + localX * turn.Cosine - localY * turn.Sine;
				y = turn.Pivot.Y + localX * turn.Sine + localY * turn.Cosine;
			}

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

		vertex(bounds.Min.X, bounds.Min.Y, uv.Min.X, uv.Min.Y);
		vertex(bounds.Max.X, bounds.Min.Y, uv.Max.X, uv.Min.Y);
		vertex(bounds.Max.X, bounds.Max.Y, uv.Max.X, uv.Max.Y);
		vertex(bounds.Min.X, bounds.Max.Y, uv.Min.X, uv.Max.Y);

		constexpr uint16_t order[6] = {0, 1, 2, 0, 2, 3};
		for (const uint16_t offset : order) {
			IndexData.push_back(static_cast<uint16_t>(base + offset));
		}
	}

	void InterfaceMesh::PushRounded(
		const Rect &bounds, const Rect &uv, float radius, uint32_t colour, const Rotation &turn
	) {
		radius = std::clamp(radius, 0.0f, std::min(bounds.Width(), bounds.Height()) * 0.5f);
		if (!(radius > 0.0f)) {
			Push(bounds, uv, colour, turn);
			return;
		}

		constexpr size_t boundaryCount = CORNER_STEPS * 4 + 4;
		if (VertexData.size() + boundaryCount + 1 > 0xFFFFu) {
			return;
		}

		std::array<Vector2, boundaryCount> boundary{};
		size_t at = 0;
		const Vector2 centres[4]{
			{bounds.Min.X + radius, bounds.Min.Y + radius},
			{bounds.Max.X - radius, bounds.Min.Y + radius},
			{bounds.Max.X - radius, bounds.Max.Y - radius},
			{bounds.Min.X + radius, bounds.Max.Y - radius},
		};
		const float starts[4]{PI, PI * 1.5f, 0.0f, PI * 0.5f};
		for (size_t corner = 0; corner < 4; corner++) {
			for (size_t step = 0; step <= CORNER_STEPS; step++) {
				const float angle =
					starts[corner] + PI * 0.5f * static_cast<float>(step) / static_cast<float>(CORNER_STEPS);
				boundary[at++] = Vector2{
					centres[corner].X + std::cos(angle) * radius,
					centres[corner].Y + std::sin(angle) * radius,
				};
			}
		}

		// **The rounded boundary is convex, so a ramped one goes through the
		// clipper rather than through the fan below.** A fan from the centre
		// interpolates each triangle between the middle and two edge points,
		// which is exact for one linear segment and wrong across a keypoint -
		// and a rounded panel with a three-stop gradient is an ordinary thing to
		// author.
		if (Ramp != nullptr) {
			PushShaded(boundary, bounds, uv, colour, turn);
			return;
		}

		const auto base = static_cast<uint16_t>(VertexData.size());
		const auto put = [&](const Vector2 &point) { PushVertex(point, bounds, uv, colour, turn); };

		put(Vector2{(bounds.Min.X + bounds.Max.X) * 0.5f, (bounds.Min.Y + bounds.Max.Y) * 0.5f});
		for (const Vector2 &point : boundary) {
			put(point);
		}

		for (size_t index = 0; index < boundaryCount; index++) {
			IndexData.push_back(base);
			IndexData.push_back(static_cast<uint16_t>(base + 1 + index));
			IndexData.push_back(static_cast<uint16_t>(base + 1 + (index + 1) % boundaryCount));
		}
	}

	void InterfaceMesh::PushRoundedOutline(
		const Rect &bounds,
		float radius,
		float thickness,
		gui::LineJoin join,
		const Vector2 &uv,
		uint32_t colour,
		const Rotation &turn
	) {
		thickness = std::max(thickness, 1.0f);
		if (!(radius > 0.0f)) {
			Push(Rect{bounds.Min, {bounds.Max.X, bounds.Min.Y + thickness}}, Rect{uv, uv}, colour, turn);
			Push(Rect{{bounds.Min.X, bounds.Max.Y - thickness}, bounds.Max}, Rect{uv, uv}, colour, turn);
			Push(
				Rect{
					{bounds.Min.X, bounds.Min.Y + thickness},
					{bounds.Min.X + thickness, bounds.Max.Y - thickness}
				},
				Rect{uv, uv},
				colour,
				turn
			);
			Push(
				Rect{
					{bounds.Max.X - thickness, bounds.Min.Y + thickness},
					{bounds.Max.X, bounds.Max.Y - thickness}
				},
				Rect{uv, uv},
				colour,
				turn
			);
			return;
		}
		// **A stroke at least half the smaller side has no hole left in it**, so
		// it is a filled shape rather than a ring. `Miter` fills a *sharp* one,
		// which is what a mitred corner at full thickness is - and passing the
		// radius here instead would round the very corner the mode exists to
		// keep. `Bevel` is the one approximation: its chamfer is not a shape
		// `PushRounded` can draw, and it fills round rather than growing a
		// second filled path for a stroke nobody can see the inside of.
		const float maxThickness = std::min(bounds.Width(), bounds.Height()) * 0.5f;
		if (thickness >= maxThickness) {
			const float filled = join == gui::LineJoin::Miter ? 0.0f : radius;
			PushRounded(bounds, Rect{uv, uv}, filled, colour, turn);
			return;
		}

		const Rect inner{
			Vector2{bounds.Min.X + thickness, bounds.Min.Y + thickness},
			Vector2{bounds.Max.X - thickness, bounds.Max.Y - thickness},
		};
		const float outerRadius = std::clamp(radius, 0.0f, std::min(bounds.Width(), bounds.Height()) * 0.5f);
		const float innerRadius = std::max(outerRadius - thickness, 0.0f);

		constexpr size_t count = CORNER_STEPS * 4 + 4;
		if (VertexData.size() + count * 2 > 0xFFFFu) {
			return;
		}

		// **The three joins are one ring with its corner points moved**, which
		// is what makes a join cost nothing. Every mode emits the same
		// `CORNER_STEPS + 1` points per corner between the same two tangent
		// points, so the vertex budget, the index loop and the stitching below
		// are identical whichever was asked for - only where the points sit
		// changes.
		//
		// `Round` walks the arc. `Bevel` runs straight across it, which is the
		// chord. `Miter` runs out to the corner the arc was hiding and back,
		// which is the sharp corner a radius removed.
		const auto ring = [&](const Rect &box, float arc, std::array<Vector2, count> &points) {
			const Vector2 centres[4]{
				{box.Min.X + arc, box.Min.Y + arc},
				{box.Max.X - arc, box.Min.Y + arc},
				{box.Max.X - arc, box.Max.Y - arc},
				{box.Min.X + arc, box.Max.Y - arc},
			};

			// Where the arc was hiding a corner, per corner, in the same order.
			const Vector2 corners[4]{
				{box.Min.X, box.Min.Y},
				{box.Max.X, box.Min.Y},
				{box.Max.X, box.Max.Y},
				{box.Min.X, box.Max.Y},
			};

			const float starts[4]{PI, PI * 1.5f, 0.0f, PI * 0.5f};
			const auto lerp = [](const Vector2 &from, const Vector2 &to, float at) {
				return Vector2{from.X + (to.X - from.X) * at, from.Y + (to.Y - from.Y) * at};
			};

			size_t out = 0;
			for (size_t corner = 0; corner < 4; corner++) {
				const auto onArc = [&](size_t step) {
					const float angle = starts[corner] + PI * 0.5f * static_cast<float>(step) /
															 static_cast<float>(CORNER_STEPS);
					return Vector2{
						centres[corner].X + std::cos(angle) * arc,
						centres[corner].Y + std::sin(angle) * arc,
					};
				};

				const Vector2 from = onArc(0);
				const Vector2 to = onArc(CORNER_STEPS);

				for (size_t step = 0; step <= CORNER_STEPS; step++) {
					const float at = static_cast<float>(step) / static_cast<float>(CORNER_STEPS);
					switch (join) {
					case gui::LineJoin::Bevel:
						points[out++] = lerp(from, to, at);
						break;
					case gui::LineJoin::Miter:
						// Out along one straight edge and back along the other,
						// so the run has a real vertex *at* the corner rather
						// than a fold between two segments that miss it.
						points[out++] = at <= 0.5f ? lerp(from, corners[corner], at * 2.0f)
												   : lerp(corners[corner], to, (at - 0.5f) * 2.0f);
						break;
					case gui::LineJoin::Round:
						points[out++] = onArc(step);
						break;
					}
				}
			}
		};

		std::array<Vector2, count> outer{};
		std::array<Vector2, count> inside{};
		ring(bounds, outerRadius, outer);
		ring(inner, innerRadius, inside);

		const auto base = static_cast<uint16_t>(VertexData.size());

		// **A ring is not convex, so a ramp on one is shaded per vertex rather
		// than clipped.** Every other shape here goes through `PushShaded`,
		// which is exact; this one cannot, because Sutherland-Hodgman needs a
		// convex input and an outline has a hole in it. The ring carries four
		// arcs' worth of vertices, so a ramp with a hard stop between two of them
		// blends across one segment instead of stepping - the one approximation
		// in this file's gradient path, and it is stated rather than discovered.
		const auto put = [&](const Vector2 &point) {
			const Rect flat{uv, uv};
			PushVertex(point, flat, flat, colour, turn);
		};
		for (size_t index = 0; index < count; index++) {
			put(outer[index]);
			put(inside[index]);
		}

		for (size_t index = 0; index < count; index++) {
			const uint16_t outerA = static_cast<uint16_t>(base + index * 2);
			const uint16_t innerA = static_cast<uint16_t>(outerA + 1);
			const uint16_t outerB = static_cast<uint16_t>(base + ((index + 1) % count) * 2);
			const uint16_t innerB = static_cast<uint16_t>(outerB + 1);
			const uint16_t order[6]{outerA, outerB, innerB, outerA, innerB, innerA};
			IndexData.insert(IndexData.end(), std::begin(order), std::end(order));
		}
	}

	void InterfaceMesh::Build(
		const gui::DrawList &list,
		const GlyphAtlas &atlas,
		const std::function<InterfaceImageInfo(const core::Name &)> &images,
		const std::function<InterfaceImageInfo(ecs::Entity)> &viewports
	) {
		VertexData.clear();
		IndexData.clear();
		BatchData.clear();

		const Vector2 white = atlas.Ready() ? atlas.WhiteTexel() : Vector2{0.0f, 0.0f};
		const Rect solid{white, white};

		for (const gui::DrawCommand &command : list.Commands) {
			const bool textured = command.Kind == gui::DrawKind::Image;
			const bool viewport = command.Kind == gui::DrawKind::Viewport;
			const core::Name image = textured ? command.Image : core::Name{};
			const bool fresh =
				BatchData.empty() || !SameClip(BatchData.back().Clip, command.Clip) ||
				BatchData.back().Image != image || BatchData.back().Collector != command.Collector ||
				BatchData.back().Viewport != (viewport ? command.Source : ecs::NULL_ENTITY) ||
				BatchData.back().Shader != command.Shader || BatchData.back().Resample != command.Resample;

			if (fresh) {
				InterfaceBatch batch;
				batch.FirstIndex = static_cast<uint32_t>(IndexData.size());
				batch.Clip = command.Clip;
				batch.Image = image;
				batch.Collector = command.Collector;
				batch.Viewport = viewport ? command.Source : ecs::NULL_ENTITY;
				batch.Shader = command.Shader;
				batch.Resample = command.Resample;
				BatchData.push_back(batch);
			}

			const uint32_t colour = Packed(command);
			const Rotation turn = TurnOf(command);

			// **Set for the whole command and cleared after it.** A `UIGradient`
			// ramps everything its parent draws - the fill, the image and the
			// text run alike - so it belongs to the command rather than to any
			// one quad, and every push below reads it where it writes a vertex.
			// The index is bounds-checked because the list is a value a caller
			// may have built by hand, and a bad index would read a sequence out
			// of a vector rather than drawing nothing.
			Ramp = command.Gradient >= 0 && static_cast<size_t>(command.Gradient) < list.Gradients.size()
					   ? &list.Gradients[static_cast<size_t>(command.Gradient)]
					   : nullptr;

			switch (command.Kind) {
			case gui::DrawKind::Rectangle:
				PushRounded(command.Bounds, solid, command.CornerRadius, colour, turn);
				break;

			case gui::DrawKind::Outline:
				PushRoundedOutline(
					command.Bounds, command.CornerRadius, command.Thickness, command.Join, white, colour, turn
				);
				break;

			case gui::DrawKind::Image:
			case gui::DrawKind::Viewport: {
				const InterfaceImageInfo info =
					command.Kind == gui::DrawKind::Viewport
						? (viewports ? viewports(command.Source) : InterfaceImageInfo{})
						: (images ? images(command.Image) : InterfaceImageInfo{});
				Vector2 source = info.Size;
				Rect uv{
					Vector2{info.Cell.OffsetU, info.Cell.OffsetV},
					Vector2{
						info.Cell.OffsetU + info.Cell.Scale * info.UVMax.X,
						info.Cell.OffsetV + info.Cell.Scale * info.UVMax.Y,
					},
				};

				if (source.X > 0.0f && source.Y > 0.0f && command.Sample.Width() > 0.0f &&
					command.Sample.Height() > 0.0f) {
					uv = Rect{
						Vector2{command.Sample.Min.X / source.X, command.Sample.Min.Y / source.Y},
						Vector2{command.Sample.Max.X / source.X, command.Sample.Max.Y / source.Y},
					};
					source = command.Sample.Size();
				}

				if (command.Scale == gui::ScaleType::Slice && source.X > 0.0f && source.Y > 0.0f) {
					const float left = std::max(command.SliceCenter.Min.X, 0.0f);
					const float top = std::max(command.SliceCenter.Min.Y, 0.0f);
					const float right = std::max(source.X - command.SliceCenter.Max.X, 0.0f);
					const float bottom = std::max(source.Y - command.SliceCenter.Max.Y, 0.0f);
					const float width = command.Bounds.Width();
					const float height = command.Bounds.Height();
					const float shrink = std::min(
						{1.0f,
						 (left + right) > 0.0f ? width / ((left + right) * std::max(command.SliceScale, 0.0f))
											   : 1.0f,
						 (top + bottom) > 0.0f
							 ? height / ((top + bottom) * std::max(command.SliceScale, 0.0f))
							 : 1.0f}
					);
					const float scale = std::max(command.SliceScale, 0.0f) * shrink;
					const float xs[4]{
						command.Bounds.Min.X,
						command.Bounds.Min.X + left * scale,
						command.Bounds.Max.X - right * scale,
						command.Bounds.Max.X,
					};
					const float ys[4]{
						command.Bounds.Min.Y,
						command.Bounds.Min.Y + top * scale,
						command.Bounds.Max.Y - bottom * scale,
						command.Bounds.Max.Y,
					};
					const float us[4]{
						uv.Min.X,
						uv.Min.X + uv.Width() * left / source.X,
						uv.Min.X + uv.Width() * command.SliceCenter.Max.X / source.X,
						uv.Max.X,
					};
					const float vs[4]{
						uv.Min.Y,
						uv.Min.Y + uv.Height() * top / source.Y,
						uv.Min.Y + uv.Height() * command.SliceCenter.Max.Y / source.Y,
						uv.Max.Y,
					};
					for (size_t row = 0; row < 3; row++) {
						for (size_t column = 0; column < 3; column++) {
							if (xs[column + 1] > xs[column] && ys[row + 1] > ys[row]) {
								Push(
									Rect{{xs[column], ys[row]}, {xs[column + 1], ys[row + 1]}},
									Rect{{us[column], vs[row]}, {us[column + 1], vs[row + 1]}},
									colour,
									turn
								);
							}
						}
					}
					break;
				}

				if (command.Scale == gui::ScaleType::Tile) {
					const float tileWidth = command.Tile.X > 0.0f ? command.Tile.X : command.Bounds.Width();
					const float tileHeight = command.Tile.Y > 0.0f ? command.Tile.Y : command.Bounds.Height();
					if (tileWidth > 0.0f && tileHeight > 0.0f) {
						for (float y = command.Bounds.Min.Y; y < command.Bounds.Max.Y; y += tileHeight) {
							for (float x = command.Bounds.Min.X; x < command.Bounds.Max.X; x += tileWidth) {
								const float right = std::min(x + tileWidth, command.Bounds.Max.X);
								const float bottom = std::min(y + tileHeight, command.Bounds.Max.Y);
								const float u = (right - x) / tileWidth;
								const float v = (bottom - y) / tileHeight;
								Push(
									Rect{{x, y}, {right, bottom}},
									Rect{uv.Min, {uv.Min.X + uv.Width() * u, uv.Min.Y + uv.Height() * v}},
									colour,
									turn
								);
							}
						}
					}
					break;
				}

				Rect placed = command.Bounds;
				if ((command.Scale == gui::ScaleType::Fit || command.Scale == gui::ScaleType::Crop) &&
					source.X > 0.0f && source.Y > 0.0f && placed.Width() > 0.0f && placed.Height() > 0.0f) {
					const float boxAspect = placed.Width() / placed.Height();
					const float imageAspect = source.X / source.Y;
					if (command.Scale == gui::ScaleType::Fit) {
						if (imageAspect > boxAspect) {
							const float slack = (placed.Height() - placed.Width() / imageAspect) * 0.5f;
							placed.Min.Y += slack;
							placed.Max.Y -= slack;
						} else {
							const float slack = (placed.Width() - placed.Height() * imageAspect) * 0.5f;
							placed.Min.X += slack;
							placed.Max.X -= slack;
						}
					} else if (imageAspect > boxAspect) {
						const float keep = boxAspect / imageAspect;
						const float slack = uv.Width() * (1.0f - keep) * 0.5f;
						uv.Min.X += slack;
						uv.Max.X -= slack;
					} else {
						const float keep = imageAspect / boxAspect;
						const float slack = uv.Height() * (1.0f - keep) * 0.5f;
						uv.Min.Y += slack;
						uv.Max.Y -= slack;
					}
				}

				PushRounded(placed, uv, command.CornerRadius, colour, turn);
				break;
			}

			case gui::DrawKind::Text: {
				if (!atlas.Ready() || command.Text.empty()) {
					break;
				}

				const float atlasPixels = atlas.PixelSize();
				const float requested =
					command.TextSize > 0 ? static_cast<float>(command.TextSize) : atlasPixels;

				RunStyle base;
				base.Face = FaceFor(command.Font);
				base.Scale = requested / atlasPixels;
				base.Colour = colour;

				const float scale = base.Scale;
				const float lineHeight = atlas.LineHeight() * scale * std::max(command.LineHeight, 0.0f);
				const std::vector<Line> lines = LinesOf(command, atlas, base, atlasPixels);
				const float blockHeight = lineHeight * static_cast<float>(lines.size());
				float top = command.Bounds.Min.Y;
				if (command.YAlignment == gui::TextYAlignment::Center) {
					top += (command.Bounds.Height() - blockHeight) * 0.5f;
				} else if (command.YAlignment == gui::TextYAlignment::Bottom) {
					top = command.Bounds.Max.Y - blockHeight;
				}

				const Vector2 white = atlas.WhiteTexel();
				constexpr std::string_view dots = "...";

				// **`colouring` is false on the eight stroke passes.** They draw
				// the same glyphs offset by a pixel in each direction, and a span
				// that recoloured them would put a rainbow outline round a word.
				// The face and the size still apply, because an outline has to
				// be the shape of the letter it is outlining.
				const auto draw = [&](uint32_t glyphColour, float offsetX, float offsetY, bool colouring) {
					RunStyle pass = base;
					pass.Colour = glyphColour;

					for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
						const Line &line = lines[lineIndex];
						const float lineWidth =
							WidthOfRange(atlas, command, line.Begin, line.End, pass, atlasPixels) +
							(line.Ellipsis ? WidthOf(atlas, pass.Face, dots, pass.Scale) : 0.0f);

						float penX = command.Bounds.Min.X;
						if (command.XAlignment == gui::TextXAlignment::Center) {
							penX += (command.Bounds.Width() - lineWidth) * 0.5f;
						} else if (command.XAlignment == gui::TextXAlignment::Right) {
							penX = command.Bounds.Max.X - lineWidth;
						}
						const float baseline =
							top + static_cast<float>(lineIndex) * lineHeight + atlasPixels * scale;

						// Where the current run of underlined or struck
						// characters started, so a rule is one quad per run
						// rather than one per letter.
						float ruleStart = penX;
						bool ruling = false;
						bool rulingStrike = false;

						const auto closeRule = [&](float endX) {
							if (!ruling || !(endX > ruleStart)) {
								ruling = false;
								return;
							}

							// One tenth of an em, which is the weight every text
							// engine settles on and is thin enough not to close
							// the gaps in a lower-case g.
							const float weight = std::max(requested * 0.1f, 1.0f);
							const float at =
								rulingStrike ? baseline - requested * 0.3f : baseline + requested * 0.1f;
							Push(
								Rect{
									{ruleStart + offsetX, at + offsetY},
									{endX + offsetX, at + weight + offsetY}
								},
								Rect{white, white},
								glyphColour,
								turn
							);
							ruling = false;
						};

						const auto glyphAt =
							[&](Typeface face, float glyphScale, char32_t codepoint, uint32_t tint) {
								const Glyph *glyph = atlas.Find(face, codepoint);
								if (glyph == nullptr) {
									return;
								}
								if (glyph->Width > 0 && glyph->Height > 0) {
									const float left = penX + glyph->OffsetX * glyphScale + offsetX;
									const float glyphTop = baseline + glyph->OffsetY * glyphScale + offsetY;
									const float sheetWidth = static_cast<float>(atlas.Width());
									const float sheetHeight = static_cast<float>(atlas.Height());
									Push(
										Rect{
											{left, glyphTop},
											{left + static_cast<float>(glyph->Width) * glyphScale,
											 glyphTop + static_cast<float>(glyph->Height) * glyphScale},
										},
										Rect{
											{static_cast<float>(glyph->X) / sheetWidth,
											 static_cast<float>(glyph->Y) / sheetHeight},
											{static_cast<float>(glyph->X + glyph->Width) / sheetWidth,
											 static_cast<float>(glyph->Y + glyph->Height) / sheetHeight},
										},
										tint,
										turn
									);
								}
								penX += glyph->Advance * glyphScale;
							};

						for (size_t at = line.Begin; at < line.End; at++) {
							const RunStyle style = StyleAt(command, at, pass, atlasPixels, colouring);
							const bool wants = style.Underline || style.Strike;

							if (wants != ruling || (ruling && style.Strike != rulingStrike)) {
								closeRule(penX);
								if (wants) {
									ruling = true;
									rulingStrike = style.Strike;
									ruleStart = penX;
								}
							}

							glyphAt(
								style.Face,
								style.Scale,
								static_cast<char32_t>(static_cast<unsigned char>(command.Text[at])),
								style.Colour
							);
						}

						closeRule(penX);

						if (line.Ellipsis) {
							for (const char character : dots) {
								glyphAt(
									pass.Face,
									pass.Scale,
									static_cast<char32_t>(static_cast<unsigned char>(character)),
									glyphColour
								);
							}
						}
					}
				};

				if (command.StrokeTransparency < 1.0f) {
					const uint32_t stroke = Packed(command.StrokeTint, command.StrokeTransparency);
					for (const Vector2 offset : std::array<Vector2, 8>{
							 Vector2{-1.0f, -1.0f},
							 Vector2{0.0f, -1.0f},
							 Vector2{1.0f, -1.0f},
							 Vector2{-1.0f, 0.0f},
							 Vector2{1.0f, 0.0f},
							 Vector2{-1.0f, 1.0f},
							 Vector2{0.0f, 1.0f},
							 Vector2{1.0f, 1.0f},
						 }) {
						draw(stroke, offset.X, offset.Y, false);
					}
				}
				draw(colour, 0.0f, 0.0f, true);
				break;
			}
			}

			BatchData.back().IndexCount =
				static_cast<uint32_t>(IndexData.size()) - BatchData.back().FirstIndex;
		}

		// **Cleared, because the pointer is into the caller's list.** Nothing
		// below reads it, and leaving it set would leave a member pointing into a
		// `DrawList` this object does not own once `Build` returns.
		Ramp = nullptr;

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
