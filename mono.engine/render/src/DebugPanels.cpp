#include <engine/core/Profiling.hpp>
#include <engine/render/DebugPanels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace engine::render {

	// ---------------------------------------------------------------------
	// FrameStatistics
	// ---------------------------------------------------------------------

	void FrameStatistics::Record(double now, float deltaSeconds) {
		// A zero delta is the first frame, or a clock that did not move. Either
		// way it would divide to infinity.
		if (deltaSeconds <= 0.0f) {
			return;
		}

		if (Count == Ring.size()) {
			// Full. Doubling and re-linearising, so the oldest sample is at
			// index zero again and the wrap arithmetic stays simple. How many
			// samples twenty seconds holds depends on the frame rate, so the
			// size cannot be picked up front — but it settles after a second or
			// two and never grows again.
			std::vector<Sample> grown(Ring.empty() ? 256 : Ring.size() * 2);
			for (size_t offset = 0; offset < Count; offset++) {
				grown[offset] = Ring[IndexOf(offset)];
			}
			Ring.swap(grown);
			Head = 0;
		}

		Ring[(Head + Count) % Ring.size()] = Sample{now, deltaSeconds};
		Count++;

		while (Count > 0 && now - Ring[Head].Time > WINDOW_SECONDS) {
			Head = (Head + 1) % Ring.size();
			Count--;
		}
	}

	void FrameStatistics::Clear() {
		// The storage is kept. It is the size the frame rate needs it to be, and
		// closing and reopening the panel should not pay to learn that again.
		Head = 0;
		Count = 0;
	}

	bool FrameStatistics::HasSamples() const {
		return Count > 0;
	}

	FrameSummary FrameStatistics::Summarise() const {
		FrameSummary summary;
		if (Count == 0) {
			return summary;
		}

		const float latest = Ring[IndexOf(Count - 1)].Delta;
		summary.Current = 1.0f / latest;
		summary.CurrentMilliseconds = latest * 1000.0f;

		// The slowest frame is the lowest FPS, so Minimum tracks the *largest*
		// delta. Getting these the wrong way round makes the panel say the
		// opposite of the truth, which is worse than not having it.
		float worst = Ring[Head].Delta;
		float best = Ring[Head].Delta;
		double total = 0.0;
		double change = 0.0;

		float previous = Ring[Head].Delta;
		for (size_t offset = 0; offset < Count; offset++) {
			const float delta = Ring[IndexOf(offset)].Delta;
			worst = std::max(worst, delta);
			best = std::min(best, delta);
			total += delta;
			if (offset > 0) {
				change += std::abs(delta - previous);
			}
			previous = delta;
		}

		summary.Minimum = 1.0f / worst;
		summary.Maximum = 1.0f / best;
		// The mean of the deltas, then inverted. Averaging the per-frame FPS
		// values instead would weight the fast frames far too heavily.
		summary.Average = static_cast<float>(static_cast<double>(Count) / total);
		summary.Jitter =
			Count < 2 ? 0.0f : static_cast<float>(change / static_cast<double>(Count - 1)) * 1000.0f;

		return summary;
	}

	// Each of these is the whole window for one number. They are here for a
	// caller that wants exactly one; anything wanting several wants Summarise.
	float FrameStatistics::Current() const {
		return Summarise().Current;
	}

	float FrameStatistics::CurrentMilliseconds() const {
		return Summarise().CurrentMilliseconds;
	}

	float FrameStatistics::Minimum() const {
		return Summarise().Minimum;
	}

	float FrameStatistics::Maximum() const {
		return Summarise().Maximum;
	}

	float FrameStatistics::Average() const {
		return Summarise().Average;
	}

	float FrameStatistics::Jitter() const {
		return Summarise().Jitter;
	}

	// ---------------------------------------------------------------------
	// Drawing
	// ---------------------------------------------------------------------

	std::string_view GetProfilerTabName(ProfilerTab tab) {
		switch (tab) {
		case ProfilerTab::Frame:
			return "frame";
		case ProfilerTab::Categories:
			return "categories";
		case ProfilerTab::Systems:
			return "systems";
		case ProfilerTab::Counters:
			return "counters";
		case ProfilerTab::Count:
			break;
		}
		return "?";
	}

	namespace {

		struct Colour {
			uint8_t R;
			uint8_t G;
			uint8_t B;
		};

		constexpr Colour PANEL_BACKGROUND{8, 10, 16};
		constexpr Colour TEXT{220, 226, 236};
		constexpr Colour TEXT_DIM{130, 138, 154};
		constexpr Colour TEXT_WARN{240, 180, 80};
		constexpr Colour TEXT_BAD{236, 96, 96};
		constexpr uint8_t PANEL_ALPHA = 208;

		// One colour per category, so a glance at the flamegraph says where the
		// frame went before any label is read.
		constexpr std::array<Colour, static_cast<size_t>(core::ProfileCategory::Count)> CATEGORY_COLOURS{
			Colour{108, 142, 216}, // engine
			Colour{96, 190, 130},  // render
			Colour{222, 158, 70},  // simulation
			Colour{190, 120, 210}, // script
		};

		std::string Format(const char *format, ...) {
			// snprintf twice rather than iostreams: this runs once per line per
			// frame while a panel is open, and the panels exist to measure the
			// frame rather than to move it.
			va_list arguments;
			va_start(arguments, format);
			char buffer[256];
			std::vsnprintf(buffer, sizeof(buffer), format, arguments);
			va_end(arguments);
			return std::string(buffer);
		}

		// Colour a frame time by whether it would hold 60 Hz.
		// Name, then this frame's time, then the recent worst, then share. Fixed
		// columns so the eye runs down the numbers instead of hunting for them
		// past names of different lengths.
		constexpr size_t NAME_FIELD = 26;
		constexpr size_t VALUE_FIELD = 6; // 999.99
		// Beside MS and in the same units, because the pair is the point: read
		// together they say "costs this much, except when it costs that much".
		constexpr size_t RMAX_FIELD = 6;
		constexpr size_t SHARE_FIELD = 6; // 100.0%
		constexpr size_t ROW_CHARS = NAME_FIELD + 1 + VALUE_FIELD + 1 + RMAX_FIELD + 1 + SHARE_FIELD;

		// The colour chip that starts every row, and what separates one line
		// from the next at a glance.
		constexpr int CHIP_WIDTH = 3;
		constexpr int CHIP_GAP = 3;
		constexpr int TIMELINE_GAP = 4;
		// Where in the frame a span ran, which the numbers cannot say: two
		// systems costing 2 ms each read the same whether they ran back to back
		// or with the GPU wait between them.
		constexpr int TIMELINE_WIDTH = 112;

		// Two spaces a level, and no deeper.
		constexpr uint32_t MAXIMUM_INDENT = 6;

		int MeasureChars(size_t count, int scale) {
			return count == 0 ? 0 : static_cast<int>((count * DebugText::ADVANCE - 1) * scale);
		}

		std::string PadRight(std::string text, size_t width) {
			if (text.size() > width) {
				text.resize(width);
			}
			text.append(width - text.size(), ' ');
			return text;
		}

		std::string PadLeft(std::string text, size_t width) {
			if (text.size() >= width) {
				return text;
			}
			return std::string(width - text.size(), ' ') + text;
		}

		// Deeper is dimmer, so a subtree reads as one thing shading away from
		// its root rather than as unrelated rows that happen to be adjacent.
		Colour Shade(Colour colour, uint32_t depth) {
			const float amount = std::max(1.0f - static_cast<float>(depth) * 0.08f, 0.5f);
			return Colour{
				static_cast<uint8_t>(static_cast<float>(colour.R) * amount),
				static_cast<uint8_t>(static_cast<float>(colour.G) * amount),
				static_cast<uint8_t>(static_cast<float>(colour.B) * amount),
			};
		}

		// White until it matters. A row worth looking at should be findable
		// without reading every number on the way down.
		Colour ColourForShare(float share) {
			if (share >= 0.5f) {
				return TEXT_BAD;
			}
			if (share >= 0.2f) {
				return TEXT_WARN;
			}
			return TEXT;
		}

		Colour ColourForMilliseconds(float milliseconds) {
			if (milliseconds > 33.4f) {
				return TEXT_BAD;
			}
			if (milliseconds > 16.7f) {
				return TEXT_WARN;
			}
			return TEXT;
		}

		struct Writer {
			OverlayImage &Image;
			int X;
			int Y;
			int Scale;

			void Line(std::string_view text, Colour colour = TEXT) {
				DebugText::Draw(Image, X, Y, text, colour.R, colour.G, colour.B, Scale);
				Y += DebugText::LineHeight(Scale);
			}

			void Skip(int lines = 1) {
				Y += DebugText::LineHeight(Scale) * lines;
			}
		};

		void DrawPanelBackground(OverlayImage &image, int x, int y, int width, int height) {
			// Fill, not Blend. DrawDebugPanels clears the overlay before either
			// panel is drawn and this is the first thing either one puts down, so
			// every pixel under it is transparent — which makes the read half of
			// the read-modify-write a read of zero, several hundred thousand
			// times, to combine with nothing.
			//
			// The two panels cannot overlap: the profiler is top-anchored beneath
			// the statistics panel's full height. If that ever stops being true
			// this has to go back to Blend, and it will look like one panel
			// punching a hole in the other.
			image.Fill(
				x, y, width, height, PANEL_BACKGROUND.R, PANEL_BACKGROUND.G, PANEL_BACKGROUND.B, PANEL_ALPHA
			);
			// A one-pixel top edge, so the panel reads as a surface rather than
			// as a dark patch of the scene. Blended, because it lands on the fill
			// above rather than on a cleared image — though at full alpha it
			// takes Blend's opaque path and is a store either way.
			image.Blend(x, y, width, 1, 90, 100, 120, 255);
		}

		// -----------------------------------------------------------------
		// F3
		// -----------------------------------------------------------------

		// Needed before either panel is drawn: both are top-anchored and the
		// frame graph starts below this one, so its own height depends on how
		// tall this is even when it is switched off.
		int StatisticsHeight(const DebugPanelData &data) {
			const int lines = data.TickRate > 0.0 ? 7 : 6;
			return lines * DebugText::LineHeight(data.Scale) + 4 * data.Scale * 2;
		}

		void DrawStatistics(OverlayImage &image, const DebugPanelData &data) {
			const int scale = data.Scale;
			const int padding = 4 * scale;

			const int width = DebugText::Measure("MIN 000.0  AVG 000.0  MAX 000.0", scale) + padding * 2;
			const int height = StatisticsHeight(data);

			{
				// The whole panel rectangle, at 208 alpha — so every pixel of it
				// is a read, four multiplies, four divides and a write. Seven
				// lines of text sit on top of it, and until this span existed the
				// two were one number.
				ENGINE_PROFILE_CAT("statistics background", core::ProfileCategory::Render);
				DrawPanelBackground(image, 0, 0, width, height);
			}

			ENGINE_PROFILE_CAT("statistics text", core::ProfileCategory::Render);

			Writer writer{image, padding, padding, scale};

			if (!data.Statistics || !data.Statistics->HasSamples()) {
				writer.Line("MEASURING", TEXT_DIM);
				return;
			}

			// One walk of the window for all six numbers. Asked one at a time
			// this was six walks of tens of thousands of samples to draw three
			// lines of text, and it cost more than the scene being measured.
			//
			// Its own span, because it is the only part of this panel whose cost
			// scales with the frame *rate* rather than with what is on screen —
			// the faster the game runs, the more samples twenty seconds holds.
			FrameSummary summary;
			{
				ENGINE_PROFILE_CAT("statistics summarise", core::ProfileCategory::Render);
				summary = data.Statistics->Summarise();
			}

			writer.Line(
				Format(
					"%.0f FPS   %.2f MS",
					static_cast<double>(summary.Current),
					static_cast<double>(summary.CurrentMilliseconds)
				),
				ColourForMilliseconds(summary.CurrentMilliseconds)
			);
			writer.Line(
				Format(
					"MIN %.1f  AVG %.1f  MAX %.1f",
					static_cast<double>(summary.Minimum),
					static_cast<double>(summary.Average),
					static_cast<double>(summary.Maximum)
				),
				TEXT_DIM
			);
			writer.Line(Format("JITTER %.2f MS", static_cast<double>(summary.Jitter)), TEXT_DIM);

			if (data.TickRate > 0.0) {
				// Measured against configured. They diverge when the machine
				// cannot keep up, and the dropped count says whether the
				// simulation gave up catching back up.
				const bool behind = data.TicksPerSecond < static_cast<float>(data.TickRate) * 0.95f;
				writer.Line(
					Format(
						"TICK %.0f HZ  (%.1f)%s",
						data.TickRate,
						static_cast<double>(data.TicksPerSecond),
						data.DroppedTicks > 0 ? "  DROPPED" : ""
					),
					data.DroppedTicks > 0 ? TEXT_BAD : (behind ? TEXT_WARN : TEXT_DIM)
				);
			}
			writer.Line(Format("ENTITIES %llu", static_cast<unsigned long long>(data.Entities)), TEXT_DIM);
			writer.Line(
				Format(
					"DRAWS %llu  TRIS %llu",
					static_cast<unsigned long long>(data.DrawCalls),
					static_cast<unsigned long long>(data.Triangles)
				),
				TEXT_DIM
			);
			writer.Line(
				data.Backend.empty()
					? "GPU ?"
					: Format("GPU %.*s", static_cast<int>(data.Backend.size()), data.Backend.data()),
				TEXT_DIM
			);
		}

		// -----------------------------------------------------------------
		// F5 — the flamegraph
		// -----------------------------------------------------------------

		// A row per span, rather than a bar per span.
		//
		// Bars that share a line save vertical space and cost every label, and
		// the label is the whole point: a wall of coloured rectangles says a
		// frame was busy without saying what it was busy with. The "when in the
		// frame" that the bars carried is kept, in the timeline strip on the
		// right — which the numbers cannot say, because two systems costing 2 ms
		// each read identically whether they ran back to back or with the GPU
		// wait between them.
		void DrawFlameGraph(
			OverlayImage &image, const DebugPanelData &data, int x, int y, int width, int available
		) {
			const int scale = data.Scale;
			const int rowHeight = DebugText::LineHeight(scale);
			const int glyphHeight = DebugText::GLYPH_HEIGHT * scale;

			if (data.Spans.empty()) {
				DebugText::Draw(
					image, x, y, "NO SPANS THIS FRAME", TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
				);
				return;
			}

			const int chipWidth = CHIP_WIDTH * scale;
			const int chipGap = CHIP_GAP * scale;
			const int textLeft = x + chipWidth + chipGap;

			const int columnsWidth = MeasureChars(ROW_CHARS, scale);
			const int timelineLeft = textLeft + columnsWidth + TIMELINE_GAP * scale;
			const int timelineWidth = std::max(0, x + width - timelineLeft);

			const float frameMilliseconds = std::max(data.FrameMilliseconds, 0.0001f);
			const float timelineScale = static_cast<float>(timelineWidth) / frameMilliseconds;

			// The column header, so the numbers are readable without knowing the
			// order they come in.
			DebugText::Draw(
				image,
				textLeft,
				y,
				PadRight("SPAN", NAME_FIELD) + " " + PadLeft("MS", VALUE_FIELD) + " " +
					PadLeft("RMAX", RMAX_FIELD) + " " + PadLeft("SHARE", SHARE_FIELD),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);

			int cursor = y + rowHeight;

			// Pinned under the header rather than sorted into the list, and it
			// does not scroll away. It is not a span — it is the part of the
			// frame no span covers — and it is the first thing worth knowing
			// when the rows below do not add up to the heading above.
			//
			// No timeline bar: unmarked time is not one interval, it is every
			// gap between the ones that are marked, and drawing it as a block
			// would put it somewhere it did not happen.
			{
				const float share = data.UnmarkedMilliseconds / frameMilliseconds;
				const std::string row =
					PadRight("(unmarked)", NAME_FIELD) + " " +
					PadLeft(Format("%.2f", static_cast<double>(data.UnmarkedMilliseconds)), VALUE_FIELD) +
					" " + PadLeft("-", RMAX_FIELD) + " " +
					PadLeft(Format("%.1f%%", static_cast<double>(share) * 100.0), SHARE_FIELD);

				// Coloured by how much of the frame it is, the same as any other
				// row. A frame that is mostly uninstrumented should look as bad
				// as a system that is mostly slow, because it is the same
				// problem seen from the other side.
				const auto textColour = ColourForShare(share);
				DebugText::Draw(
					image, textLeft, cursor, row, textColour.R, textColour.G, textColour.B, scale
				);
				cursor += rowHeight;
			}

			// Four passes over the visible rows rather than one pass doing
			// everything to each.
			//
			// The passes exist to be *measured*. Interleaved, the cost of a row
			// is one number covering a history scan, a handful of string
			// allocations, a few hundred glyph blends and two rectangle fills —
			// and "the panel costs 0.7 ms" is not something anybody can act on.
			// Split, each one is a span of its own in the very graph being drawn.
			//
			// Nothing is done twice: the passes divide the work, they do not
			// repeat it. Drawing order changes, and cannot matter — the chips,
			// the text and the timeline occupy disjoint columns.
			struct PlannedRow {
				size_t Index = 0;
				int Y = 0;
				bool Collapsed = false;
				float RecentMaximum = 0.0f;
			};

			std::vector<PlannedRow> rows;
			std::vector<std::string> text;

			{
				ENGINE_PROFILE_CAT("flame select", core::ProfileCategory::Render);
				rows.reserve(data.Spans.size());

				int skipped = 0;
				for (size_t index = 0; index < data.Spans.size(); index++) {
					const auto &span = data.Spans[index];

					// Below the depth budget the row is not drawn at all, and
					// its parent is marked instead. A hidden subtree that leaves
					// no trace makes a parent look like a leaf, and "physics.step
					// 8 ms" with nothing under it means two different things
					// depending on whether that is all of it.
					if (span.Depth > data.DepthLimit) {
						continue;
					}

					// Spans are in open order, so the next entry with a greater
					// depth is a direct child of this one.
					const bool collapsed = span.Depth == data.DepthLimit && index + 1 < data.Spans.size() &&
										   data.Spans[index + 1].Depth > span.Depth;

					if (skipped < data.Scroll) {
						skipped++;
						continue;
					}
					if (cursor + rowHeight > y + available) {
						break;
					}

					rows.push_back(PlannedRow{index, cursor, collapsed, 0.0f});
					cursor += rowHeight;
				}
			}

			{
				// One lookup and one window scan per visible row. Separated
				// because it is the only part of drawing a row that is not
				// drawing — it reads several hundred frames of history per
				// name, and it is the first thing to suspect when this panel
				// costs more than what it is reporting on.
				ENGINE_PROFILE_CAT("flame history", core::ProfileCategory::Render);
				for (auto &row : rows) {
					row.RecentMaximum = core::FrameGraph::RecentMaximum(data.Spans[row.Index].Name);
				}
			}

			{
				// Padding, formatting and the allocations they carry. Nothing
				// reaches the image in this pass.
				ENGINE_PROFILE_CAT("flame format", core::ProfileCategory::Render);
				text.reserve(rows.size());

				for (const auto &row : rows) {
					const auto &span = data.Spans[row.Index];

					// Two spaces a level, and no deeper: past this a row is more
					// indent than name, and the nesting is legible from the
					// timeline anyway.
					std::string name(std::min(span.Depth, MAXIMUM_INDENT) * 2, ' ');
					name.append(span.Name);
					if (row.Collapsed) {
						name += " +";
					}

					const float share = span.Milliseconds / frameMilliseconds;
					text.push_back(
						PadRight(std::move(name), NAME_FIELD) + " " +
						PadLeft(Format("%.2f", static_cast<double>(span.Milliseconds)), VALUE_FIELD) + " " +
						PadLeft(Format("%.2f", static_cast<double>(row.RecentMaximum)), RMAX_FIELD) + " " +
						PadLeft(Format("%.1f%%", static_cast<double>(share) * 100.0), SHARE_FIELD)
					);
				}
			}

			{
				// The rasteriser. Every glyph is a blend per lit pixel, so this
				// is the pass that scales with the font scale squared.
				ENGINE_PROFILE_CAT("flame glyphs", core::ProfileCategory::Render);
				for (size_t index = 0; index < rows.size(); index++) {
					const float share = data.Spans[rows[index].Index].Milliseconds / frameMilliseconds;
					const auto textColour = ColourForShare(share);
					DebugText::Draw(
						image,
						textLeft,
						rows[index].Y,
						text[index],
						textColour.R,
						textColour.G,
						textColour.B,
						scale
					);
				}
			}

			{
				// Rectangle fills: the category chip on the left and the
				// timeline bar on the right. Far fewer pixels than the glyphs,
				// and worth knowing that rather than assuming it.
				ENGINE_PROFILE_CAT("flame bars", core::ProfileCategory::Render);
				for (const auto &row : rows) {
					const auto &span = data.Spans[row.Index];

					// Deeper is dimmer, so a subtree reads as one thing shading
					// away from its root rather than as unrelated adjacent rows.
					const auto colour =
						Shade(CATEGORY_COLOURS[static_cast<size_t>(span.Category)], span.Depth);
					image.Blend(x, row.Y, chipWidth, glyphHeight, colour.R, colour.G, colour.B, 235);

					// Where in the frame it ran.
					if (timelineWidth > 0) {
						const int left =
							timelineLeft + static_cast<int>(span.StartMilliseconds * timelineScale);
						// One pixel minimum, so a span too short to see is still
						// visibly there rather than silently absent.
						const int barWidth = std::max(1, static_cast<int>(span.Milliseconds * timelineScale));
						image.Blend(left, row.Y, barWidth, glyphHeight, colour.R, colour.G, colour.B, 235);
					}
				}

				// The frame-budget line across the timeline. 16.7 ms, so a frame
				// going long is visible without reading a number.
				constexpr float BUDGET_MILLISECONDS = 1000.0f / 60.0f;
				if (timelineWidth > 0 && data.FrameMilliseconds > BUDGET_MILLISECONDS) {
					const int budgetX = timelineLeft + static_cast<int>(BUDGET_MILLISECONDS * timelineScale);
					image.Blend(budgetX, y, scale, cursor - y, TEXT_BAD.R, TEXT_BAD.G, TEXT_BAD.B, 180);
				}
			}
		}

		void DrawCategories(OverlayImage &image, const DebugPanelData &data, int x, int y, int width) {
			const int scale = data.Scale;
			const int lineHeight = DebugText::LineHeight(scale);
			const int barHeight = DebugText::GLYPH_HEIGHT * scale;
			const int labelWidth = DebugText::Measure("SIM 00.00", scale) + 4 * scale;

			const float total = std::max(data.FrameMilliseconds, 0.0001f);

			// One pass over the spans for every category, not one pass per
			// category. The old shape read the whole frame four times to
			// produce four numbers, and would have read it once more for every
			// category anybody added.
			std::array<float, static_cast<size_t>(core::ProfileCategory::Count)> totals{};
			{
				ENGINE_PROFILE_CAT("category totals", core::ProfileCategory::Render);
				for (const auto &span : data.Spans) {
					const auto index = static_cast<size_t>(span.Category);
					if (index < totals.size()) {
						totals[index] += span.SelfMilliseconds;
					}
				}
			}

			ENGINE_PROFILE_CAT("category glyphs", core::ProfileCategory::Render);

			int cursor = y;
			for (size_t index = 0; index < static_cast<size_t>(core::ProfileCategory::Count); index++) {
				const auto category = static_cast<core::ProfileCategory>(index);
				const std::string_view name = core::GetCategoryName(category);

				const float milliseconds = totals[index];

				DebugText::Draw(
					image,
					x,
					cursor,
					Format(
						"%.*s %.2f",
						static_cast<int>(name.size()),
						name.data(),
						static_cast<double>(milliseconds)
					),
					TEXT.R,
					TEXT.G,
					TEXT.B,
					scale
				);

				const int barWidth =
					static_cast<int>(static_cast<float>(width - labelWidth) * (milliseconds / total));
				const auto colour = CATEGORY_COLOURS[index];
				image.Blend(
					x + labelWidth,
					cursor,
					std::max(barWidth, 1),
					barHeight,
					colour.R,
					colour.G,
					colour.B,
					235
				);

				cursor += lineHeight;
			}

			// The category bars are self time, and self time only covers what a
			// scope was opened around. Without this last bar they sum to less
			// than the frame and nothing on the panel says why — the reader is
			// left comparing four bars against a total none of them reach.
			DebugText::Draw(
				image,
				x,
				cursor,
				Format("--- %.2f", static_cast<double>(data.UnmarkedMilliseconds)),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);

			const int unmarkedWidth = static_cast<int>(
				static_cast<float>(width - labelWidth) * (data.UnmarkedMilliseconds / total)
			);
			image.Blend(
				x + labelWidth,
				cursor,
				std::max(unmarkedWidth, 1),
				barHeight,
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				235
			);
		}

		void
		DrawRows(OverlayImage &image, const DebugPanelData &data, int x, int y, int width, int available) {
			const int scale = data.Scale;
			const int lineHeight = DebugText::LineHeight(scale);
			const int labelWidth = DebugText::Measure("00.00 MS  ", scale);

			// The widest cost in view sets the bar scale, so a list of cheap
			// systems still shows which is the expensive one.
			float widest = 0.0001f;
			if (data.Tab == ProfilerTab::Systems) {
				for (const auto &system : data.Systems) {
					widest = std::max(widest, system.Milliseconds);
				}
			}

			if (data.Tab == ProfilerTab::Systems && data.Systems.empty()) {
				DebugText::Draw(
					image, x, y, "NO SYSTEMS REGISTERED", TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
				);
				return;
			}
			if (data.Tab != ProfilerTab::Systems && data.Counters.empty()) {
				DebugText::Draw(
					image, x, y, "NO COUNTERS THIS FRAME", TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
				);
				return;
			}

			// The same four passes the flame graph runs, for the same reason:
			// interleaved, a row's cost is one number covering a history scan,
			// a string allocation and a few hundred glyph blends, and there is
			// nothing to act on in that.
			struct PlannedRow {
				std::string_view Name;
				float Value = 0.0f;
				bool IsTime = false;
				int Y = 0;
				float RecentMaximum = 0.0f;
			};

			std::vector<PlannedRow> rows;
			std::vector<std::string> text;

			{
				ENGINE_PROFILE_CAT("row select", core::ProfileCategory::Render);

				int cursor = y;
				int skipped = 0;
				const auto plan = [&](std::string_view name, float value, bool isTime) {
					if (skipped < data.Scroll) {
						skipped++;
						return;
					}
					if (cursor + lineHeight > y + available) {
						return;
					}
					rows.push_back(PlannedRow{name, value, isTime, cursor, 0.0f});
					cursor += lineHeight;
				};

				if (data.Tab == ProfilerTab::Systems) {
					rows.reserve(data.Systems.size());
					for (const auto &system : data.Systems) {
						plan(system.Name, system.Milliseconds, true);
					}
				} else {
					rows.reserve(data.Counters.size());
					for (const auto &counter : data.Counters) {
						// Times are accumulated in nanoseconds and shown as
						// milliseconds; counts are shown as they were written.
						plan(
							counter.Name.Text(),
							counter.IsTime ? static_cast<float>(counter.Value / 1'000'000.0)
										   : static_cast<float>(counter.Value),
							counter.IsTime
						);
					}
				}
			}

			{
				// A system's name is the name of the span the scheduler opens
				// around it, so the history already has it and the same RMAX
				// column works here without anything extra being recorded — at
				// the price of a window scan per row, which is why it is its own
				// span rather than hidden inside the formatting.
				ENGINE_PROFILE_CAT("row history", core::ProfileCategory::Render);
				for (auto &row : rows) {
					if (row.IsTime) {
						row.RecentMaximum = core::FrameGraph::RecentMaximum(row.Name);
					}
				}
			}

			{
				ENGINE_PROFILE_CAT("row format", core::ProfileCategory::Render);
				text.reserve(rows.size());
				for (const auto &row : rows) {
					text.push_back(
						row.IsTime ? Format(
										 "%6.2f %6.2f  %.*s",
										 static_cast<double>(row.Value),
										 static_cast<double>(row.RecentMaximum),
										 static_cast<int>(row.Name.size()),
										 row.Name.data()
									 )
								   : Format(
										 "%9.0f  %.*s",
										 static_cast<double>(row.Value),
										 static_cast<int>(row.Name.size()),
										 row.Name.data()
									 )
					);
				}
			}

			{
				// Before the glyphs, because the bar is behind the text.
				ENGINE_PROFILE_CAT("row bars", core::ProfileCategory::Render);
				for (const auto &row : rows) {
					if (!row.IsTime) {
						continue;
					}
					const int barWidth =
						static_cast<int>(static_cast<float>(width - labelWidth) * (row.Value / widest));
					image.Blend(
						x + labelWidth,
						row.Y,
						std::max(barWidth, 1),
						DebugText::GLYPH_HEIGHT * scale,
						60,
						72,
						96,
						220
					);
				}
			}

			{
				ENGINE_PROFILE_CAT("row glyphs", core::ProfileCategory::Render);
				for (size_t index = 0; index < rows.size(); index++) {
					DebugText::Draw(image, x, rows[index].Y, text[index], TEXT.R, TEXT.G, TEXT.B, scale);
				}
			}
		}

		void DrawFrameGraphPanel(OverlayImage &image, const DebugPanelData &data) {
			const int scale = data.Scale;
			const int padding = 4 * scale;
			const int lineHeight = DebugText::LineHeight(scale);

			// Wide enough for the columns, the chip and a timeline worth
			// reading. Narrower and the timeline is a smear; wider and the panel
			// is covering the game for no extra information.
			const int columnsWidth = MeasureChars(ROW_CHARS, scale);
			const int wanted = padding * 2 + CHIP_WIDTH * scale + CHIP_GAP * scale + columnsWidth +
							   TIMELINE_GAP * scale + TIMELINE_WIDTH * scale;
			const int width = std::min(image.GetWidth(), wanted);

			// Sized to what is actually in it. A fixed half-screen panel spends
			// most of its life as a large dark rectangle over the game, which
			// makes the tool something you close rather than something you
			// leave open.
			//
			// Measured because it is a pass over every span before a single
			// pixel is drawn, and because it decides how large the background
			// fill below is going to be — so it is upstream of the panel's
			// biggest cost as well as being a cost itself.
			int bodyRows = 1;
			{
				ENGINE_PROFILE_CAT("panel layout", core::ProfileCategory::Render);
				switch (data.Tab) {
				case ProfilerTab::Frame: {
					// A row per visible span now, not a row per depth. The two
					// were the same only while every level was one bar.
					int visible = 0;
					for (const auto &span : data.Spans) {
						if (span.Depth <= data.DepthLimit) {
							visible++;
						}
					}
					// Plus the column header, plus the pinned (unmarked) row. Both
					// are drawn before the scrollable body and neither scrolls, so
					// leaving them out of the budget does not move the panel — it
					// clips the last span off the bottom of it.
					bodyRows = visible + 2 - std::max(data.Scroll, 0);
					break;
				}
				case ProfilerTab::Categories:
					// Plus the unmarked bar, which is drawn after the categories so
					// that the bars sum to the frame rather than to less than it.
					bodyRows = static_cast<int>(core::ProfileCategory::Count) + 1;
					break;
				case ProfilerTab::Systems:
					bodyRows = static_cast<int>(data.Systems.size()) - std::max(data.Scroll, 0);
					break;
				case ProfilerTab::Counters:
					bodyRows = static_cast<int>(data.Counters.size()) - std::max(data.Scroll, 0);
					break;
				case ProfilerTab::Count:
					break;
				}
				bodyRows = std::clamp(bodyRows, 1, 40);
			}

			// Top-anchored, stacked under the statistics panel when that is
			// open. Anchored to the bottom, the rows grew upwards — so the
			// first row moved every time the frame's shape changed, and reading
			// down a tree meant re-finding where it started on every repaint.
			const int top = data.ShowStatistics ? StatisticsHeight(data) + padding : 0;
			const int room = std::max(image.GetHeight() - top, lineHeight);

			// Two header lines, a blank, then the body.
			const int height = std::min(room, (3 + bodyRows) * lineHeight + padding * 2);

			{
				// The largest single rectangle either panel draws, and the only
				// one that scales with how many rows are on screen. If opening
				// the profiler costs more the more it has to report, this is
				// where that comes from.
				ENGINE_PROFILE_CAT("panel background", core::ProfileCategory::Render);
				DrawPanelBackground(image, 0, top, width, height);
			}

			Writer writer{image, padding, top + padding, scale};

			{
				ENGINE_PROFILE_CAT("panel chrome", core::ProfileCategory::Render);

				// The header says which view is open, how to change it, and
				// whether Tracy is attached — because "the graph is empty" and
				// "nothing is collecting" look identical otherwise.
				const std::string_view tabName = GetProfilerTabName(data.Tab);
				writer.Line(
					Format(
						"FRAME GRAPH  [%.*s]  F6/F7 TAB  PGUP/PGDN SCROLL  -/= DEPTH %u  F8 SNAPSHOT",
						static_cast<int>(tabName.size()),
						tabName.data(),
						data.DepthLimit
					),
					TEXT_DIM
				);

				// RMAX is only meaningful against a window, so the window says how
				// much of itself it has: a reading over 0.2 s of history and one
				// over the full five seconds are not the same claim.
				writer.Line(
					Format(
						"%.2f MS   TRACY %s   RMAX OVER %.1fS%s",
						static_cast<double>(data.FrameMilliseconds),
						data.TracyAttached ? "ATTACHED" : "OFF",
						data.HistorySeconds,
						data.DroppedSpans > 0 ? "   SPANS DROPPED!" : ""
					),
					data.DroppedSpans > 0 ? TEXT_WARN : ColourForMilliseconds(data.FrameMilliseconds)
				);
				writer.Skip();
			}

			const int bodyTop = writer.Y;
			const int available = top + height - padding - bodyTop;
			const int bodyWidth = width - padding * 2;

			switch (data.Tab) {
			// One span per tab body rather than one for all of them. The tabs
			// cost different amounts — a flame graph is a row and a timeline bar
			// per span, categories are four bars — and a single name covering
			// all of them would report a number that changes when you press a
			// key and never say which key.
			case ProfilerTab::Frame: {
				ENGINE_PROFILE_CAT("flame graph", core::ProfileCategory::Render);
				DrawFlameGraph(image, data, padding, bodyTop, bodyWidth, available);
				break;
			}
			case ProfilerTab::Categories: {
				ENGINE_PROFILE_CAT("category bars", core::ProfileCategory::Render);
				DrawCategories(image, data, padding, bodyTop, bodyWidth);
				break;
			}
			case ProfilerTab::Systems:
			case ProfilerTab::Counters: {
				ENGINE_PROFILE_CAT("row list", core::ProfileCategory::Render);
				DrawRows(image, data, padding, bodyTop, bodyWidth, available);
				break;
			}
			case ProfilerTab::Count:
				break;
			}
		}
	}

	// The panels are drawn by rasterising glyphs into a CPU image, one pixel at
	// a time, every frame. That is the right trade for a tool that has to work
	// with no second process attached — but it is not free, and until these
	// scopes existed the whole cost landed as self time on whatever the caller
	// had opened around it. A panel reporting a frame it is a large part of, and
	// not saying so, is the one measurement error a profiler cannot afford.
	//
	// The reading is of the *previous* frame, so the panel does show its own
	// cost. That is not a flaw to be corrected away: the observer is part of the
	// frame while it is open, and hiding that would make closing the panel look
	// like it fixed something.
	void DrawDebugPanels(OverlayImage &image, const DebugPanelData &data) {
		{
			// A full-buffer write of the overlay every frame, whatever is drawn
			// on top of it afterwards. At 4K that is thirty megabytes touched
			// before any panel has drawn a single glyph.
			ENGINE_PROFILE_CAT("overlay clear", core::ProfileCategory::Render);
			image.Clear();
		}

		if (image.IsEmpty()) {
			return;
		}

		if (data.ShowStatistics) {
			ENGINE_PROFILE_CAT("statistics panel", core::ProfileCategory::Render);
			DrawStatistics(image, data);
		}
		if (data.ShowFrameGraph) {
			ENGINE_PROFILE_CAT("profiler panel", core::ProfileCategory::Render);
			DrawFrameGraphPanel(image, data);
		}
	}
}
