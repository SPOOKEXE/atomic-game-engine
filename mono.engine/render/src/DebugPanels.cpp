#include <engine/render/DebugPanels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

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

		Samples.push_back(Sample{now, deltaSeconds});
		while (!Samples.empty() && now - Samples.front().Time > WINDOW_SECONDS) {
			Samples.pop_front();
		}
	}

	void FrameStatistics::Clear() {
		Samples.clear();
	}

	bool FrameStatistics::HasSamples() const {
		return !Samples.empty();
	}

	float FrameStatistics::Current() const {
		if (Samples.empty()) {
			return 0.0f;
		}
		return 1.0f / Samples.back().Delta;
	}

	float FrameStatistics::CurrentMilliseconds() const {
		if (Samples.empty()) {
			return 0.0f;
		}
		return Samples.back().Delta * 1000.0f;
	}

	// The slowest frame is the lowest FPS, so Minimum walks for the largest
	// delta. Getting these the wrong way round makes the panel say the opposite
	// of the truth, which is worse than not having it.
	float FrameStatistics::Minimum() const {
		if (Samples.empty()) {
			return 0.0f;
		}
		float worst = Samples.front().Delta;
		for (const auto &sample : Samples) {
			worst = std::max(worst, sample.Delta);
		}
		return 1.0f / worst;
	}

	float FrameStatistics::Maximum() const {
		if (Samples.empty()) {
			return 0.0f;
		}
		float best = Samples.front().Delta;
		for (const auto &sample : Samples) {
			best = std::min(best, sample.Delta);
		}
		return 1.0f / best;
	}

	float FrameStatistics::Average() const {
		if (Samples.empty()) {
			return 0.0f;
		}

		// The mean of the deltas, then inverted. Averaging the per-frame FPS
		// values instead would weight the fast frames far too heavily.
		double total = 0.0;
		for (const auto &sample : Samples) {
			total += sample.Delta;
		}
		return static_cast<float>(static_cast<double>(Samples.size()) / total);
	}

	float FrameStatistics::Jitter() const {
		if (Samples.size() < 2) {
			return 0.0f;
		}

		double total = 0.0;
		for (size_t index = 1; index < Samples.size(); index++) {
			total += std::abs(Samples[index].Delta - Samples[index - 1].Delta);
		}
		return static_cast<float>(total / static_cast<double>(Samples.size() - 1)) * 1000.0f;
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
			image.Blend(
				x, y, width, height, PANEL_BACKGROUND.R, PANEL_BACKGROUND.G, PANEL_BACKGROUND.B, PANEL_ALPHA
			);
			// A one-pixel top edge, so the panel reads as a surface rather than
			// as a dark patch of the scene.
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

			DrawPanelBackground(image, 0, 0, width, height);

			Writer writer{image, padding, padding, scale};

			if (!data.Statistics || !data.Statistics->HasSamples()) {
				writer.Line("MEASURING", TEXT_DIM);
				return;
			}

			const auto &statistics = *data.Statistics;
			const float milliseconds = statistics.CurrentMilliseconds();

			writer.Line(
				Format(
					"%.0f FPS   %.2f MS",
					static_cast<double>(statistics.Current()),
					static_cast<double>(milliseconds)
				),
				ColourForMilliseconds(milliseconds)
			);
			writer.Line(
				Format(
					"MIN %.1f  AVG %.1f  MAX %.1f",
					static_cast<double>(statistics.Minimum()),
					static_cast<double>(statistics.Average()),
					static_cast<double>(statistics.Maximum())
				),
				TEXT_DIM
			);
			writer.Line(Format("JITTER %.2f MS", static_cast<double>(statistics.Jitter())), TEXT_DIM);

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
			int skipped = 0;

			for (size_t index = 0; index < data.Spans.size(); index++) {
				const auto &span = data.Spans[index];

				// Below the depth budget the row is not drawn at all, and its
				// parent is marked instead. A hidden subtree that leaves no
				// trace makes a parent look like a leaf, and "physics.step
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

				// Deeper is dimmer, so a subtree reads as one thing shading away
				// from its root rather than as unrelated adjacent rows.
				const auto colour = Shade(CATEGORY_COLOURS[static_cast<size_t>(span.Category)], span.Depth);
				image.Blend(x, cursor, chipWidth, glyphHeight, colour.R, colour.G, colour.B, 235);

				// Two spaces a level, and no deeper: past this a row is more
				// indent than name, and the nesting is legible from the timeline
				// anyway.
				std::string name(std::min(span.Depth, MAXIMUM_INDENT) * 2, ' ');
				name.append(span.Name);
				if (collapsed) {
					name += " +";
				}

				const float share = span.Milliseconds / frameMilliseconds;
				const float recentMaximum = core::FrameGraph::RecentMaximum(span.Name);

				const std::string row =
					PadRight(std::move(name), NAME_FIELD) + " " +
					PadLeft(Format("%.2f", static_cast<double>(span.Milliseconds)), VALUE_FIELD) + " " +
					PadLeft(Format("%.2f", static_cast<double>(recentMaximum)), RMAX_FIELD) + " " +
					PadLeft(Format("%.1f%%", static_cast<double>(share) * 100.0), SHARE_FIELD);

				const auto textColour = ColourForShare(share);
				DebugText::Draw(
					image, textLeft, cursor, row, textColour.R, textColour.G, textColour.B, scale
				);

				// Where in the frame it ran.
				if (timelineWidth > 0) {
					const int left = timelineLeft + static_cast<int>(span.StartMilliseconds * timelineScale);
					// One pixel minimum, so a span too short to see is still
					// visibly there rather than silently absent.
					const int barWidth = std::max(1, static_cast<int>(span.Milliseconds * timelineScale));
					image.Blend(left, cursor, barWidth, glyphHeight, colour.R, colour.G, colour.B, 235);
				}

				cursor += rowHeight;
			}

			// The frame-budget line across the timeline. 16.7 ms, so a frame
			// going long is visible without reading a number.
			constexpr float BUDGET_MILLISECONDS = 1000.0f / 60.0f;
			if (timelineWidth > 0 && data.FrameMilliseconds > BUDGET_MILLISECONDS) {
				const int budgetX = timelineLeft + static_cast<int>(BUDGET_MILLISECONDS * timelineScale);
				image.Blend(budgetX, y, scale, cursor - y, TEXT_BAD.R, TEXT_BAD.G, TEXT_BAD.B, 180);
			}
		}

		void DrawCategories(OverlayImage &image, const DebugPanelData &data, int x, int y, int width) {
			const int scale = data.Scale;
			const int lineHeight = DebugText::LineHeight(scale);
			const int barHeight = DebugText::GLYPH_HEIGHT * scale;
			const int labelWidth = DebugText::Measure("SIM 00.00", scale) + 4 * scale;

			const float total = std::max(data.FrameMilliseconds, 0.0001f);

			int cursor = y;
			for (size_t index = 0; index < static_cast<size_t>(core::ProfileCategory::Count); index++) {
				const auto category = static_cast<core::ProfileCategory>(index);
				const std::string_view name = core::GetCategoryName(category);

				float milliseconds = 0.0f;
				for (const auto &span : data.Spans) {
					if (span.Category == category) {
						milliseconds += span.SelfMilliseconds;
					}
				}

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

			int cursor = y;
			int skipped = 0;

			auto row = [&](std::string_view name, float value, bool isTime) {
				if (skipped < data.Scroll) {
					skipped++;
					return;
				}
				if (cursor + lineHeight > y + available) {
					return;
				}

				// A system's name is the name of the span the scheduler opens
				// around it, so the history already has it and the same RMAX
				// column works here without anything extra being recorded.
				const std::string label =
					isTime ? Format(
								 "%6.2f %6.2f  %.*s",
								 static_cast<double>(value),
								 static_cast<double>(core::FrameGraph::RecentMaximum(name)),
								 static_cast<int>(name.size()),
								 name.data()
							 )
						   : Format(
								 "%9.0f  %.*s",
								 static_cast<double>(value),
								 static_cast<int>(name.size()),
								 name.data()
							 );

				if (isTime) {
					const int barWidth =
						static_cast<int>(static_cast<float>(width - labelWidth) * (value / widest));
					image.Blend(
						x + labelWidth,
						cursor,
						std::max(barWidth, 1),
						DebugText::GLYPH_HEIGHT * scale,
						60,
						72,
						96,
						220
					);
				}

				DebugText::Draw(image, x, cursor, label, TEXT.R, TEXT.G, TEXT.B, scale);
				cursor += lineHeight;
			};

			if (data.Tab == ProfilerTab::Systems) {
				if (data.Systems.empty()) {
					DebugText::Draw(
						image, x, y, "NO SYSTEMS REGISTERED", TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
					);
					return;
				}
				for (const auto &system : data.Systems) {
					row(system.Name, system.Milliseconds, true);
				}
				return;
			}

			if (data.Counters.empty()) {
				DebugText::Draw(
					image, x, y, "NO COUNTERS THIS FRAME", TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
				);
				return;
			}
			for (const auto &counter : data.Counters) {
				// Times are accumulated in nanoseconds and shown as
				// milliseconds; counts are shown as they were written.
				row(counter.Name.Text(),
					counter.IsTime ? static_cast<float>(counter.Value / 1'000'000.0)
								   : static_cast<float>(counter.Value),
					counter.IsTime);
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
			int bodyRows = 1;
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
				// Plus the column header.
				bodyRows = visible + 1 - std::max(data.Scroll, 0);
				break;
			}
			case ProfilerTab::Categories:
				bodyRows = static_cast<int>(core::ProfileCategory::Count);
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

			// Top-anchored, stacked under the statistics panel when that is
			// open. Anchored to the bottom, the rows grew upwards — so the
			// first row moved every time the frame's shape changed, and reading
			// down a tree meant re-finding where it started on every repaint.
			const int top = data.ShowStatistics ? StatisticsHeight(data) + padding : 0;
			const int room = std::max(image.GetHeight() - top, lineHeight);

			// Two header lines, a blank, then the body.
			const int height = std::min(room, (3 + bodyRows) * lineHeight + padding * 2);

			DrawPanelBackground(image, 0, top, width, height);

			Writer writer{image, padding, top + padding, scale};

			// The header says which view is open, how to change it, and whether
			// Tracy is attached — because "the graph is empty" and "nothing is
			// collecting" look identical otherwise.
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

			const int bodyTop = writer.Y;
			const int available = top + height - padding - bodyTop;
			const int bodyWidth = width - padding * 2;

			switch (data.Tab) {
			case ProfilerTab::Frame:
				DrawFlameGraph(image, data, padding, bodyTop, bodyWidth, available);
				break;
			case ProfilerTab::Categories:
				DrawCategories(image, data, padding, bodyTop, bodyWidth);
				break;
			case ProfilerTab::Systems:
			case ProfilerTab::Counters:
				DrawRows(image, data, padding, bodyTop, bodyWidth, available);
				break;
			case ProfilerTab::Count:
				break;
			}
		}
	}

	void DrawDebugPanels(OverlayImage &image, const DebugPanelData &data) {
		image.Clear();
		if (image.IsEmpty()) {
			return;
		}

		if (data.ShowStatistics) {
			DrawStatistics(image, data);
		}
		if (data.ShowFrameGraph) {
			DrawFrameGraphPanel(image, data);
		}
	}
}
