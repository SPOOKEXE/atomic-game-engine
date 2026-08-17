#include <engine/render/InterfaceMesh.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace engine::render {

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

		std::vector<std::string>
		LinesOf(const gui::DrawCommand &command, const GlyphAtlas &atlas, Typeface face, float scale) {
			std::vector<std::string> lines;
			const float available = std::max(command.Bounds.Width(), 0.0f);

			size_t paragraphStart = 0;
			while (paragraphStart <= command.Text.size()) {
				const size_t newline = command.Text.find('\n', paragraphStart);
				const size_t paragraphEnd = newline == std::string::npos ? command.Text.size() : newline;
				std::string paragraph = command.Text.substr(paragraphStart, paragraphEnd - paragraphStart);

				if (!command.Wrapped || available <= 0.0f) {
					lines.push_back(std::move(paragraph));
				} else if (paragraph.empty()) {
					lines.emplace_back();
				} else {
					size_t first = 0;
					while (first < paragraph.size()) {
						while (first < paragraph.size() && paragraph[first] == ' ') {
							first++;
						}
						if (first >= paragraph.size()) {
							break;
						}

						size_t fit = first;
						size_t word = std::string::npos;
						for (size_t end = first + 1; end <= paragraph.size(); end++) {
							if (paragraph[end - 1] == ' ') {
								word = end - 1;
							}
							if (WidthOf(
									atlas, face, std::string_view(paragraph).substr(first, end - first), scale
								) > available) {
								break;
							}
							fit = end;
						}

						if (fit == first) {
							fit = first + 1;
						} else if (fit < paragraph.size() && word != std::string::npos && word > first) {
							fit = word;
						}

						lines.push_back(paragraph.substr(first, fit - first));
						first = fit;
					}
				}

				if (newline == std::string::npos) {
					break;
				}
				paragraphStart = newline + 1;
			}

			if (lines.empty()) {
				lines.emplace_back();
			}

			if (!command.Wrapped && command.Truncate == gui::TextTruncate::AtEnd && available > 0.0f) {
				for (std::string &line : lines) {
					if (WidthOf(atlas, face, line, scale) <= available) {
						continue;
					}

					constexpr std::string_view dots = "...";
					while (!line.empty() &&
						   WidthOf(atlas, face, line + std::string(dots), scale) > available) {
						line.pop_back();
					}
					line += dots;
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

	void InterfaceMesh::Push(const Rect &bounds, const Rect &uv, uint32_t colour, const Rotation &turn) {
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

		const auto base = static_cast<uint16_t>(VertexData.size());
		const auto put = [&](const Vector2 &point) {
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
			InterfaceVertex made;
			made.X = x;
			made.Y = y;
			made.U = uv.Min.X + uv.Width() * across;
			made.V = uv.Min.Y + uv.Height() * down;
			made.R = static_cast<uint8_t>(colour & 0xFFu);
			made.G = static_cast<uint8_t>((colour >> 8) & 0xFFu);
			made.B = static_cast<uint8_t>((colour >> 16) & 0xFFu);
			made.A = static_cast<uint8_t>((colour >> 24) & 0xFFu);
			VertexData.push_back(made);
		};

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
		const float maxThickness = std::min(bounds.Width(), bounds.Height()) * 0.5f;
		if (thickness >= maxThickness) {
			PushRounded(bounds, Rect{uv, uv}, radius, colour, turn);
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

		const auto ring = [&](const Rect &box, float arc, std::array<Vector2, count> &points) {
			const Vector2 centres[4]{
				{box.Min.X + arc, box.Min.Y + arc},
				{box.Max.X - arc, box.Min.Y + arc},
				{box.Max.X - arc, box.Max.Y - arc},
				{box.Min.X + arc, box.Max.Y - arc},
			};
			const float starts[4]{PI, PI * 1.5f, 0.0f, PI * 0.5f};
			size_t out = 0;
			for (size_t corner = 0; corner < 4; corner++) {
				for (size_t step = 0; step <= CORNER_STEPS; step++) {
					const float angle = starts[corner] + PI * 0.5f * static_cast<float>(step) /
															 static_cast<float>(CORNER_STEPS);
					points[out++] = Vector2{
						centres[corner].X + std::cos(angle) * arc,
						centres[corner].Y + std::sin(angle) * arc,
					};
				}
			}
		};

		std::array<Vector2, count> outer{};
		std::array<Vector2, count> inside{};
		ring(bounds, outerRadius, outer);
		ring(inner, innerRadius, inside);

		const auto base = static_cast<uint16_t>(VertexData.size());
		const auto put = [&](Vector2 point) {
			if (turn.Sine != 0.0f || turn.Cosine != 1.0f) {
				const float x = point.X - turn.Pivot.X;
				const float y = point.Y - turn.Pivot.Y;
				point = Vector2{
					turn.Pivot.X + x * turn.Cosine - y * turn.Sine,
					turn.Pivot.Y + x * turn.Sine + y * turn.Cosine,
				};
			}
			InterfaceVertex made;
			made.X = point.X;
			made.Y = point.Y;
			made.U = uv.X;
			made.V = uv.Y;
			made.R = static_cast<uint8_t>(colour & 0xFFu);
			made.G = static_cast<uint8_t>((colour >> 8) & 0xFFu);
			made.B = static_cast<uint8_t>((colour >> 16) & 0xFFu);
			made.A = static_cast<uint8_t>((colour >> 24) & 0xFFu);
			VertexData.push_back(made);
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
			const bool fresh = BatchData.empty() || !SameClip(BatchData.back().Clip, command.Clip) ||
							   BatchData.back().Image != image ||
							   BatchData.back().Collector != command.Collector ||
							   BatchData.back().Viewport != (viewport ? command.Source : ecs::NULL_ENTITY);

			if (fresh) {
				InterfaceBatch batch;
				batch.FirstIndex = static_cast<uint32_t>(IndexData.size());
				batch.Clip = command.Clip;
				batch.Image = image;
				batch.Collector = command.Collector;
				batch.Viewport = viewport ? command.Source : ecs::NULL_ENTITY;
				BatchData.push_back(batch);
			}

			const uint32_t colour = Packed(command);
			const Rotation turn = TurnOf(command);

			switch (command.Kind) {
			case gui::DrawKind::Rectangle:
				PushRounded(command.Bounds, solid, command.CornerRadius, colour, turn);
				break;

			case gui::DrawKind::Outline:
				PushRoundedOutline(
					command.Bounds, command.CornerRadius, command.Thickness, white, colour, turn
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

				const Typeface face = FaceFor(command.Font);
				const float requested =
					command.TextSize > 0 ? static_cast<float>(command.TextSize) : atlas.PixelSize();
				const float scale = requested / atlas.PixelSize();
				const float lineHeight = atlas.LineHeight() * scale * std::max(command.LineHeight, 0.0f);
				const std::vector<std::string> lines = LinesOf(command, atlas, face, scale);
				const float blockHeight = lineHeight * static_cast<float>(lines.size());
				float top = command.Bounds.Min.Y;
				if (command.YAlignment == gui::TextYAlignment::Center) {
					top += (command.Bounds.Height() - blockHeight) * 0.5f;
				} else if (command.YAlignment == gui::TextYAlignment::Bottom) {
					top = command.Bounds.Max.Y - blockHeight;
				}

				const auto draw = [&](uint32_t glyphColour, float offsetX, float offsetY) {
					for (size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
						const std::string &line = lines[lineIndex];
						const float lineWidth = WidthOf(atlas, face, line, scale);
						float penX = command.Bounds.Min.X;
						if (command.XAlignment == gui::TextXAlignment::Center) {
							penX += (command.Bounds.Width() - lineWidth) * 0.5f;
						} else if (command.XAlignment == gui::TextXAlignment::Right) {
							penX = command.Bounds.Max.X - lineWidth;
						}
						const float baseline =
							top + static_cast<float>(lineIndex) * lineHeight + atlas.PixelSize() * scale;

						for (const char character : line) {
							const auto codepoint =
								static_cast<char32_t>(static_cast<unsigned char>(character));
							const Glyph *glyph = atlas.Find(face, codepoint);
							if (glyph == nullptr) {
								continue;
							}
							if (glyph->Width > 0 && glyph->Height > 0) {
								const float left = penX + glyph->OffsetX * scale + offsetX;
								const float glyphTop = baseline + glyph->OffsetY * scale + offsetY;
								const float sheetWidth = static_cast<float>(atlas.Width());
								const float sheetHeight = static_cast<float>(atlas.Height());
								Push(
									Rect{
										{left, glyphTop},
										{left + static_cast<float>(glyph->Width) * scale,
										 glyphTop + static_cast<float>(glyph->Height) * scale},
									},
									Rect{
										{static_cast<float>(glyph->X) / sheetWidth,
										 static_cast<float>(glyph->Y) / sheetHeight},
										{static_cast<float>(glyph->X + glyph->Width) / sheetWidth,
										 static_cast<float>(glyph->Y + glyph->Height) / sheetHeight},
									},
									glyphColour,
									turn
								);
							}
							penX += glyph->Advance * scale;
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
						draw(stroke, offset.X, offset.Y);
					}
				}
				draw(colour, 0.0f, 0.0f);
				break;
			}
			}

			BatchData.back().IndexCount =
				static_cast<uint32_t>(IndexData.size()) - BatchData.back().FirstIndex;
		}

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
