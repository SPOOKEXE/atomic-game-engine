#include "PanelShares.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/render/DebugPanels.hpp>
#include <engine/render/DebugText.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace engine::render {

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
		case ProfilerTab::Heap:
			return "heap";
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

			// Pink, and deliberately nowhere near the render green above it.
			//
			// **The one pair on this panel that must never be misread for each
			// other.** `render` is the CPU recording commands and this is the
			// device running them, and telling those two apart *is* the
			// diagnosis: a frame long on the green bar is CPU-bound on the draw
			// list, a frame long on this one is GPU-bound on fill or bandwidth,
			// and the fixes have nothing in common. A shade of the same green
			// would have read as "the render bar, slightly different" - which is
			// the reading that sends somebody to optimise the wrong half of the
			// engine.
			//
			// Distinct from the script violet below it by being warm: this is
			// red-dominant, that one is blue-dominant.
			Colour{235, 125, 185}, // GPU

			// Cyan, and deliberately the most legible colour here after the
			// warnings. Every engine and game system runs through the ECS, so
			// this is the bar a reader is looking for - and it has to be
			// unmistakable against both the engine blue above it and the render
			// green beside it.
			//
			// **Positional.** This array is indexed by `ProfileCategory`, so a
			// value inserted into that enum without a colour inserted here
			// silently shifts every colour below it.
			Colour{80, 200, 230}, // ECS

			// Red, and the only warm colour above the middle of the list. The
			// physics bar is the one a reader goes looking for when a frame
			// went long, so it is findable without reading a label - and it
			// sits between the cyan above it and the amber below it, which is
			// the largest hue gap the palette had left.
			Colour{236, 112, 108}, // physics

			Colour{222, 158, 70},  // simulation
			Colour{190, 120, 210}, // script
			Colour{232, 208, 96},  // network
			Colour{150, 205, 100}, // assets
			// Grey, and deliberately the dullest thing on the panel. Idle is the
			// largest bar on a vsynced frame and the least interesting.
			Colour{96, 104, 118}, // idle
		};

		// The positional rule above, made a build error rather than a comment.
		//
		// A `std::array` sized from the enum accepts too few initialisers in
		// silence - the missing entries become black and every colour after an
		// insertion point shifts by one, which reads as a palette somebody
		// chose rather than as a mistake. Idle is last and its grey is
		// distinctive, so checking that the last slot still holds it catches
		// exactly the failure: a category added without a colour.
		static_assert(
			CATEGORY_COLOURS[static_cast<size_t>(core::ProfileCategory::Idle)].R == 96 &&
				CATEGORY_COLOURS[static_cast<size_t>(core::ProfileCategory::Idle)].G == 104 &&
				CATEGORY_COLOURS[static_cast<size_t>(core::ProfileCategory::Idle)].B == 118,
			"A ProfileCategory was added without a colour in CATEGORY_COLOURS."
		);

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
		// **Beside BUSY rather than replacing it**, because an inclusive
		// duration is not a cost and the two are worth reading together. A
		// vsynced `Renderer::Render` is 0.15 busy and 15.9 idle: the first says
		// what the renderer did, the second says what it waited for, and the
		// sum is the wall time that used to be the only number here - which
		// sent two separate investigations at the renderer for work it was not
		// doing.
		constexpr size_t IDLE_FIELD = 6;
		// Beside BUSY and in the same units, because the pair is the point: read
		// together they say "costs this much, except when it costs that much".
		constexpr size_t RMAX_FIELD = 6;
		constexpr size_t SHARE_FIELD = 6; // 100.0%
		constexpr size_t ROW_CHARS =
			NAME_FIELD + 1 + VALUE_FIELD + 1 + IDLE_FIELD + 1 + RMAX_FIELD + 1 + SHARE_FIELD;

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

		// Thousands separated. A counter reading 1048576 and one reading 104857
		// are the same width of digits at a glance, and the panel exists to be
		// read at a glance.
		std::string Grouped(double value) {
			char digits[32];
			std::snprintf(digits, sizeof(digits), "%.0f", value < 0.0 ? 0.0 : value);

			std::string text = digits;
			for (size_t at = text.size(); at > 3;) {
				at -= 3;
				text.insert(at, ",");
			}
			return text;
		}

		// A faint tint behind every other row.
		//
		// The rows are a pixel apart and a column of numbers with nothing
		// between them is read wrong - the eye slides up a line between the name
		// and the figure beside it.
		constexpr uint8_t STRIPE_ALPHA = 16;

		// The track a timeline bar sits in, and the quarter marks across it. A
		// bar's *position* is the whole reason the timeline is there, and a
		// position means nothing without something to measure it against.
		constexpr uint8_t TRACK_ALPHA = 20;
		constexpr uint8_t GRID_ALPHA = 26;

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

		// Every tab, with the open one filled in.
		//
		// The header used to name the open tab in brackets and list the keys
		// beside it, which told you what you were looking at and nothing about
		// what else there was. A strip says both: four names, one lit, and the
		// keys that move between them - a panel with a hidden control has none.
		void DrawTabStrip(OverlayImage &image, const DebugPanelData &data, int x, int y, int width) {
			const int scale = data.Scale;
			const int glyphHeight = DebugText::GLYPH_HEIGHT * scale;
			const int gap = DebugText::ADVANCE * scale;

			int pen = x;
			for (size_t index = 0; index < static_cast<size_t>(ProfilerTab::Count); index++) {
				const auto tab = static_cast<ProfilerTab>(index);
				const std::string_view name = GetProfilerTabName(tab);
				const int textWidth = DebugText::Measure(name, scale);

				if (tab == data.Tab) {
					// A filled chip rather than a brighter word. Brightness alone
					// is not a difference a person picks out of four short words
					// in the corner of a game.
					image.Blend(
						pen - scale,
						y - scale,
						textWidth + scale * 2,
						glyphHeight + scale * 2,
						90,
						130,
						190,
						200
					);
					DebugText::Draw(image, pen, y, name, 255, 255, 255, scale);
				} else {
					DebugText::Draw(image, pen, y, name, TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale);
				}

				pen += textWidth + gap * 2;
			}

			// Right-aligned, and only when it fits. A hint that overlaps the tab
			// it is explaining is worse than no hint.
			constexpr std::string_view HINT = "F6/F7 TAB  PGUP/PGDN  -/= DEPTH  F8 SNAP";
			const int hintWidth = DebugText::Measure(HINT, scale);
			if (pen + gap + hintWidth <= x + width) {
				DebugText::Draw(
					image, x + width - hintWidth, y, HINT, TEXT_DIM.R, TEXT_DIM.G, TEXT_DIM.B, scale
				);
			}
		}

		void DrawPanelBackground(OverlayImage &image, int x, int y, int width, int height) {
			// Fill, not Blend. DrawDebugPanels clears the overlay before either
			// panel is drawn and this is the first thing either one puts down, so
			// every pixel under it is transparent - which makes the read half of
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
			// above rather than on a cleared image - though at full alpha it
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
				// The whole panel rectangle, at 208 alpha - so every pixel of it
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
			// scales with the frame *rate* rather than with what is on screen -
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
		// F4 - the network panel
		// -----------------------------------------------------------------

		// A byte rate, in whichever unit keeps it three digits wide.
		//
		// Both units, because the two questions are different: kilobytes per
		// second is what a bandwidth budget is written in, and bytes per second
		// is what a per-tick message count is checked against. A panel that
		// only showed the first turns a 900 B/s stream into "0.9" and hides the
		// difference between that and 90.
		std::string FormatRate(double bytesPerSecond) {
			if (bytesPerSecond >= 1024.0) {
				return Format("%.1f KB/S (%.0f B/S)", bytesPerSecond / 1024.0, bytesPerSecond);
			}
			return Format("%.0f B/S", bytesPerSecond);
		}

		// Top-right, and deliberately not below the statistics panel.
		//
		// The other two are top-left and stacked: F5's height is worked out from
		// F3's so they cannot overlap, and adding a third to that column would
		// make every one of those numbers depend on two others. The right edge
		// has nothing on it, so this panel's position depends on nothing but the
		// image width - and its background blends rather than fills, so if a
		// wide flamegraph ever does reach this far the result is a translucent
		// overlap rather than a hole punched in the panel underneath.
		void DrawNetwork(OverlayImage &image, const DebugPanelData &data) {
			const NetworkStatistics &net = data.Network;
			const int scale = data.Scale;
			const int padding = 4 * scale;

			// Sized from the widest line this can produce, so the box does not
			// change width as the numbers do.
			const int width = DebugText::Measure("DOWN 1000.0 KB/S (100000 B/S)", scale) + padding * 2;
			const int lines = 12;
			const int height = lines * DebugText::LineHeight(scale) + padding * 2;
			const int left = std::max(0, image.GetWidth() - width);

			{
				ENGINE_PROFILE_CAT("network background", core::ProfileCategory::Render);
				image.Blend(
					left,
					0,
					width,
					height,
					PANEL_BACKGROUND.R,
					PANEL_BACKGROUND.G,
					PANEL_BACKGROUND.B,
					PANEL_ALPHA
				);
				image.Blend(left, 0, width, 1, 90, 100, 120, 255);
			}

			ENGINE_PROFILE_CAT("network text", core::ProfileCategory::Render);
			Writer writer{image, left + padding, padding, scale};

			writer.Line("NETWORK", TEXT);

			if (!net.Joined) {
				// Connected and not joined. Worth a line of its own, because
				// this is the state where every other number below is honestly
				// zero and a reader needs to know that is expected.
				writer.Line("JOINING", TEXT_WARN);
			} else {
				writer.Line(Format("TICK %llu", static_cast<unsigned long long>(net.AppliedTick)), TEXT_DIM);
			}

			writer.Line(Format("DOWN %s", FormatRate(net.ReceivedBytesPerSecond).c_str()), TEXT);
			writer.Line(Format("UP   %s", FormatRate(net.SentBytesPerSecond).c_str()), TEXT_DIM);
			writer.Line(
				Format("PPS  %.0f DN  %.0f UP", net.ReceivedPacketsPerSecond, net.SentPacketsPerSecond),
				TEXT_DIM
			);

			// The rate the *authority* runs at, which is not the rate this
			// process was configured with and is the number a mismatch shows up
			// in first.
			writer.Line(Format("RATE %.1f HZ", net.TickRate), TEXT_DIM);

			// Round trip and loss together: a link is bad in one of those two
			// ways and they need different answers.
			writer.Line(
				Format("RTT  %.1f MS", static_cast<double>(net.RoundTripMilliseconds)),
				net.RoundTripMilliseconds > 150.0f ? TEXT_WARN : TEXT_DIM
			);
			writer.Line(
				Format(
					"LOST %llu  STALE %llu",
					static_cast<unsigned long long>(net.PacketsLost),
					static_cast<unsigned long long>(net.PacketsStale)
				),
				net.PacketsLost > 0 ? TEXT_WARN : TEXT_DIM
			);

			// **The budget, which is not congestion.** `D00007` was found by
			// this coming off zero, so it is on the panel rather than in a log.
			writer.Line(
				Format("BUDGET %llu REFUSED", static_cast<unsigned long long>(net.SendsOverBudget)),
				net.SendsOverBudget > 0 ? TEXT_BAD : TEXT_DIM
			);

			writer.Line(
				Format(
					"MSGS %llu SNAP %llu DELTA %llu STRUCT",
					static_cast<unsigned long long>(net.Snapshots),
					static_cast<unsigned long long>(net.Deltas),
					static_cast<unsigned long long>(net.Structures)
				),
				TEXT_DIM
			);

			// The interpolation half. `BEHIND` sits at the configured delay on a
			// healthy link and falls toward zero as a late packet eats it;
			// `STALL` is what it costs when it runs out.
			writer.Line(
				Format(
					"BEHIND %.2f T  STALL %llu", net.BehindTicks, static_cast<unsigned long long>(net.Stalls)
				),
				net.BehindTicks < 0.25 ? TEXT_WARN : TEXT_DIM
			);

			// **Rows arrived against rows drawn, and it is the line to read
			// when the scene is empty.** Equal and non-zero means the world is
			// being drawn and the camera is the problem; unequal means rows
			// arrived without a size or a colour to draw them with.
			writer.Line(
				Format(
					"ENT %llu  DRAWN %llu",
					static_cast<unsigned long long>(net.Entities),
					static_cast<unsigned long long>(net.Drawn)
				),
				(net.Joined && net.Drawn == 0) ? TEXT_BAD : TEXT_DIM
			);
		}

		// -----------------------------------------------------------------
		// F5 - the flamegraph
		// -----------------------------------------------------------------

		// A row per span, rather than a bar per span.
		//
		// Bars that share a line save vertical space and cost every label, and
		// the label is the whole point: a wall of coloured rectangles says a
		// frame was busy without saying what it was busy with. The "when in the
		// frame" that the bars carried is kept, in the timeline strip on the
		// right - which the numbers cannot say, because two systems costing 2 ms
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

			// The timeline is laid out against the whole frame, because that is
			// what a span's start offset is measured from - but the SHARE column
			// is a share of the *busy* part. On a vsynced frame the two differ by
			// an order of magnitude, and a share of the whole frame would report
			// everything that did work as one per cent of it.
			const float frameMilliseconds = std::max(data.FrameMilliseconds, 0.0001f);
			const float busyMilliseconds = std::max(data.BusyMilliseconds(), 0.0001f);

			const std::vector<float> shares = BusyShares(data);
			const auto busyShare = [&](size_t index) { return index < shares.size() ? shares[index] : 0.0f; };
			const float timelineScale = static_cast<float>(timelineWidth) / frameMilliseconds;

			// The column header, so the numbers are readable without knowing the
			// order they come in.
			DebugText::Draw(
				image,
				textLeft,
				y,
				PadRight("SPAN", NAME_FIELD) + " " + PadLeft("BUSY", VALUE_FIELD) + " " +
					PadLeft("IDLE", IDLE_FIELD) + " " + PadLeft("RMAX", RMAX_FIELD) + " " +
					PadLeft("SHARE", SHARE_FIELD),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);

			int cursor = y + rowHeight;

			// Pinned under the header rather than sorted into the list, and it
			// does not scroll away. It is not a span - it is the part of the
			// frame no span covers - and it is the first thing worth knowing
			// when the rows below do not add up to the heading above.
			//
			// No timeline bar: unmarked time is not one interval, it is every
			// gap between the ones that are marked, and drawing it as a block
			// would put it somewhere it did not happen.
			{
				const float share = data.UnmarkedMilliseconds / busyMilliseconds;
				const std::string row =
					PadRight("(unmarked)", NAME_FIELD) + " " +
					PadLeft(Format("%.2f", static_cast<double>(data.UnmarkedMilliseconds)), VALUE_FIELD) +
					// Unmarked time is by definition uncategorised, so none of it
					// is known to be a wait. Dashed rather than zeroed: this
					// column says "measured no waiting", and here nothing was
					// measured at all.
					" " + PadLeft("-", IDLE_FIELD) + " " + PadLeft("-", RMAX_FIELD) + " " +
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
			// allocations, a few hundred glyph blends and two rectangle fills -
			// and "the panel costs 0.7 ms" is not something anybody can act on.
			// Split, each one is a span of its own in the very graph being drawn.
			//
			// Nothing is done twice: the passes divide the work, they do not
			// repeat it. Drawing order changes, and cannot matter - the chips,
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
				// drawing - it reads several hundred frames of history per
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

					const float share = busyShare(row.Index);

					// A dash rather than 0.00 for a span that never waited, so
					// the column reads as "these are the ones that blocked"
					// instead of a wall of zeroes with the answer buried in it.
					const std::string idle = span.IdleMilliseconds > 0.005f
												 ? Format("%.2f", static_cast<double>(span.IdleMilliseconds))
												 : std::string("-");

					text.push_back(
						PadRight(std::move(name), NAME_FIELD) + " " +
						PadLeft(Format("%.2f", static_cast<double>(BusyMillisecondsOf(span))), VALUE_FIELD) +
						" " + PadLeft(idle, IDLE_FIELD) + " " +
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
					const float share = busyShare(rows[index].Index);
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

				// Quarters of the frame, behind everything else. A bar's
				// position is the whole reason the timeline is there, and a
				// position means nothing without something to measure it
				// against.
				if (timelineWidth > 0 && !rows.empty()) {
					const int top = rows.front().Y;
					const int height = rows.back().Y + rowHeight - top;
					for (int quarter = 1; quarter < 4; quarter++) {
						const int line = timelineLeft + timelineWidth * quarter / 4;
						image.Blend(line, top, scale, height, 255, 255, 255, GRID_ALPHA);
					}
				}

				for (size_t index = 0; index < rows.size(); index++) {
					const auto &row = rows[index];
					const auto &span = data.Spans[row.Index];

					// Every other row tinted. The rows are a pixel apart, and a
					// column of numbers with nothing between them is read wrong
					// - the eye slides up a line between a name and its figure.
					if ((index & 1) != 0) {
						image.Blend(x, row.Y - scale, width, rowHeight, 255, 255, 255, STRIPE_ALPHA);
					}

					// Deeper is dimmer, so a subtree reads as one thing shading
					// away from its root rather than as unrelated adjacent rows.
					const auto colour =
						Shade(CATEGORY_COLOURS[static_cast<size_t>(span.Category)], span.Depth);
					image.Blend(x, row.Y, chipWidth, glyphHeight, colour.R, colour.G, colour.B, 235);

					// Where in the frame it ran, in a track of its own - an empty
					// track reads as "did not run", where empty space reads as a
					// row nobody drew.
					if (timelineWidth > 0) {
						image.Blend(
							timelineLeft, row.Y, timelineWidth, glyphHeight, 255, 255, 255, TRACK_ALPHA
						);

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

		// The widest category name, plus room for its figure.
		//
		// **Measured rather than written down.** This was sized against the
		// literal `engine 00.00`, which held while every name was six
		// characters or fewer and stopped holding the moment one was not - a
		// `physics` bar starting one glyph early is not a compile error and
		// not a test failure, it is a panel that looks very slightly wrong to
		// somebody who is busy reading the numbers on it.
		int CategoryLabelWidth(int scale) {
			size_t widest = 0;
			for (size_t index = 0; index < static_cast<size_t>(core::ProfileCategory::Count); index++) {
				widest =
					std::max(widest, core::GetCategoryName(static_cast<core::ProfileCategory>(index)).size());
			}
			// The name, a space, and `00.00`.
			return MeasureChars(widest + 1 + 5, scale) + 4 * scale;
		}

		// Columns for the heap tree. Wider names than the flamegraph's, because a
		// tag path is read by its leaf and the leaf is at the end of the indent.
		constexpr size_t HEAP_NAME_FIELD = 24;
		constexpr size_t HEAP_BYTES_FIELD = 10; // 1023.99 MiB
		constexpr size_t HEAP_BLOCKS_FIELD = 8;
		constexpr size_t HEAP_RATE_FIELD = 11; // -1023.99 MiB/s

		// How tall the live-bytes plot is, in text rows.
		//
		// Six, because the shape being read is "flat, sawtooth or ramp" and
		// three pixels of amplitude cannot show the difference between the first
		// two. It is the tallest single thing on the tab and it is meant to be:
		// the tree below says where the bytes are, and only this says whether
		// they are going anywhere.
		constexpr int HEAP_PLOT_ROWS = 6;

		// Renders a byte count to three significant figures, the way somebody
		// reads one. Signed, because a growth figure can be negative and
		// "-2.10 MiB/s" is the reading that says a subsystem is giving memory
		// back.
		std::string FormatBytes(double bytes) {
			const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
			size_t unit = 0;
			while ((bytes >= 1024.0 || bytes <= -1024.0) && unit + 1 < std::size(units)) {
				bytes /= 1024.0;
				unit++;
			}
			return Format(unit == 0 ? "%.0f %s" : "%.2f %s", bytes, units[unit]);
		}

		// The live-bytes plot: one filled column per pixel of width.
		//
		// **An area rather than a line, and drawn from the newest reading on the
		// right.** A line one pixel wide is invisible against a panel this dark
		// at any sane alpha, and a plot that filled from the left would have a
		// run's first minute anchored to the left edge and the present moment
		// drifting - so the thing being watched would move while being watched.
		void
		DrawHeapPlot(OverlayImage &image, const DebugPanelData &data, int x, int y, int width, int height) {
			image.Blend(x, y, width, height, 255, 255, 255, TRACK_ALPHA);

			const std::span<const core::HeapSample> history = data.HeapHistory;
			if (history.size() < 2 || width <= 0 || height <= 0) {
				return;
			}

			// Scaled to the window's own peak rather than to the process peak,
			// because a run that allocated 900 MB once and settled at 20 would
			// otherwise plot as a flat line along the bottom for the rest of it.
			int64_t peak = 1;
			for (const core::HeapSample &sample : history) {
				peak = std::max(peak, sample.LiveBytes);
			}
			for (const uint64_t bytes : data.GpuHeapHistory) {
				peak = std::max<int64_t>(
					peak, static_cast<int64_t>(std::min<uint64_t>(bytes, std::numeric_limits<int64_t>::max()))
				);
			}

			const int scale = data.Scale;
			const auto readings = static_cast<int>(history.size());

			for (int column = 0; column < width; column += scale) {
				// **Right-aligned, so the count is taken from the right edge
				// and clamped there.** Written as `history.size() - 1 - n` with
				// the bounds test after it, this was correct only because
				// integer division truncates towards zero on the one column
				// where the numerator goes negative - which is not a property
				// anybody should have to re-derive to read a plot.
				const int fromRight = std::clamp((width - scale - column) / scale, 0, readings - 1);
				const auto index = static_cast<size_t>(readings - 1 - fromRight);

				const auto share = static_cast<double>(history[index].LiveBytes) / static_cast<double>(peak);
				const int filled = std::clamp(static_cast<int>(share * height), 1, height);
				image.Blend(x + column, y + height - filled, scale, filled, 120, 190, 240, 150);
			}

			// GPU history shares the clock and horizontal axis with the process
			// heap. A line rather than a second fill keeps both readable where the
			// logical resource payload is larger than the CPU heap.
			const auto gpuReadings = static_cast<int>(data.GpuHeapHistory.size());
			if (gpuReadings >= 2) {
				for (int column = 0; column < width; column += scale) {
					const int fromRight = std::clamp((width - scale - column) / scale, 0, gpuReadings - 1);
					const auto index = static_cast<size_t>(gpuReadings - 1 - fromRight);
					const auto share =
						static_cast<double>(data.GpuHeapHistory[index]) / static_cast<double>(peak);
					const int line = std::clamp(static_cast<int>(share * height), 1, height);
					image.Blend(x + column, y + height - line, scale, scale, 245, 177, 76, 230);
				}
			}

			// The window's peak, so the plot's height is a number rather than a
			// shape. Drawn last and over the fill, in the corner the newest
			// readings are least likely to reach.
			DebugText::Draw(
				image,
				x + scale * 2,
				y + scale,
				FormatBytes(static_cast<double>(peak)),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);
		}

		// The heap tab: the totals, the plot, then the tag tree.
		void
		DrawHeap(OverlayImage &image, const DebugPanelData &data, int x, int y, int width, int available) {
			const int scale = data.Scale;
			const int rowHeight = DebugText::LineHeight(scale);

			if (!data.HeapCompiledIn) {
				// Not an empty tree. Every figure would read zero, and a heap
				// panel reporting no bytes is a much more alarming thing than
				// one saying it was left out of this build.
				DebugText::Draw(
					image,
					x,
					y,
					"HEAP PROFILER NOT COMPILED IN - CONFIGURE WITH MONO_HEAP_PROFILE=ON",
					TEXT_DIM.R,
					TEXT_DIM.G,
					TEXT_DIM.B,
					scale
				);
				return;
			}

			const core::HeapTotals &totals = data.Heap;
			int cursor = y;

			// The process line. Overhead is named because it is the profiler's
			// own cost and a `dev` footprint is wrong by exactly that much when
			// compared against a shipped one.
			DebugText::Draw(
				image,
				x,
				cursor,
				Format(
					"LIVE %s IN %s BLOCKS   PEAK %s   HEADERS %s",
					FormatBytes(static_cast<double>(totals.LiveBytes)).c_str(),
					Grouped(static_cast<double>(totals.LiveBlocks)).c_str(),
					FormatBytes(static_cast<double>(totals.PeakBytes)).c_str(),
					FormatBytes(static_cast<double>(totals.OverheadBytes)).c_str()
				),
				TEXT.R,
				TEXT.G,
				TEXT.B,
				scale
			);
			cursor += rowHeight;

			DebugText::Draw(
				image,
				x,
				cursor,
				Format(
					"GPU LOGICAL %s   PEAK %s   BUFFER %s   TRANSFER %s   TEXTURE %s",
					FormatBytes(static_cast<double>(data.GpuHeapLiveBytes)).c_str(),
					FormatBytes(static_cast<double>(data.GpuHeapPeakBytes)).c_str(),
					FormatBytes(static_cast<double>(data.GpuBufferBytes)).c_str(),
					FormatBytes(static_cast<double>(data.GpuTransferBufferBytes)).c_str(),
					FormatBytes(static_cast<double>(data.GpuTextureBytes)).c_str()
				),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);
			cursor += rowHeight;

			// What the run is doing *now*, which is the whole question this tab
			// exists for. The process slope is the first history entry's own,
			// fitted by the caller over the same window as every other row.
			const double processRate = data.HeapGrowth.empty() ? 0.0 : data.HeapGrowth.front().BytesPerSecond;
			const bool dropped = totals.DroppedScopes > 0 || totals.ForeignFrees > 0;
			DebugText::Draw(
				image,
				x,
				cursor,
				Format(
					"TAGS %u   WORST %s/S OVER %.0fS%s",
					totals.Nodes,
					FormatBytes(processRate).c_str(),
					data.HeapHistorySeconds,
					dropped ? "   TAGS DROPPED!" : ""
				),
				dropped ? TEXT_WARN.R : TEXT_DIM.R,
				dropped ? TEXT_WARN.G : TEXT_DIM.G,
				dropped ? TEXT_WARN.B : TEXT_DIM.B,
				scale
			);
			cursor += rowHeight;

			const int plotHeight = rowHeight * HEAP_PLOT_ROWS;
			if (cursor + plotHeight <= y + available) {
				DrawHeapPlot(image, data, x, cursor, width, plotHeight);
				cursor += plotHeight + rowHeight / 2;
			}

			DebugText::Draw(
				image,
				x,
				cursor,
				PadRight("TAG", HEAP_NAME_FIELD) + " " + PadLeft("LIVE", HEAP_BYTES_FIELD) + " " +
					PadLeft("SELF", HEAP_BYTES_FIELD) + " " + PadLeft("BLOCKS", HEAP_BLOCKS_FIELD) + " " +
					PadLeft("GROWTH", HEAP_RATE_FIELD),
				TEXT_DIM.R,
				TEXT_DIM.G,
				TEXT_DIM.B,
				scale
			);
			cursor += rowHeight;

			if (data.HeapRows.empty()) {
				DebugText::Draw(
					image,
					x,
					cursor,
					"NO TAGGED ALLOCATIONS - EVERY BYTE IS UNTAGGED",
					TEXT_DIM.R,
					TEXT_DIM.G,
					TEXT_DIM.B,
					scale
				);
				return;
			}

			// The rate for a node, looked up by index. The report is short - it
			// is the tracked nodes that cleared a byte floor - so a scan beats
			// building a map per repaint.
			const auto rateOf = [&](uint32_t node) {
				for (const core::HeapGrowth &entry : data.HeapGrowth) {
					if (entry.Node == node) {
						return entry.BytesPerSecond;
					}
				}
				return 0.0;
			};

			const auto live = static_cast<double>(std::max<int64_t>(totals.LiveBytes, 1));

			int skipped = 0;
			for (const core::HeapTreeRow &row : data.HeapRows) {
				if (skipped < data.Scroll) {
					skipped++;
					continue;
				}
				if (cursor + rowHeight > y + available) {
					break;
				}

				// `Depth` is one for a top-level tag - `TreeRows` never emits
				// the root - so the indent is one level less than the depth.
				// Guarded rather than assumed: this struct is filled by a
				// caller, and an unsigned zero less one is a very wide indent.
				const uint32_t level = row.Depth > 0 ? row.Depth - 1 : 0;
				std::string name(std::min(level, MAXIMUM_INDENT) * 2, ' ');
				name.append(row.Name);

				const double rate = rateOf(row.Node);
				const double share = static_cast<double>(row.InclusiveBytes) / live;

				// The share bar sits behind the row rather than in a column of
				// its own: the numbers are already five fields wide, and what
				// the bar adds is proportion at a glance, which does not need
				// its own space to say.
				const int barWidth = std::clamp(static_cast<int>(share * width), 0, width);
				image.Blend(x, cursor - scale, barWidth, rowHeight, 96, 190, 130, 26);

				// A dash rather than a zero for a tag that is not moving, so the
				// column reads as a list of suspects rather than a wall of
				// zeroes with the answer buried in it.
				const std::string growth =
					std::abs(rate) < 64.0 ? std::string("-") : FormatBytes(rate) + "/s";

				const auto colour = rate > 64.0 * 1024.0  ? TEXT_BAD
									: rate > 4.0 * 1024.0 ? TEXT_WARN
														  : Shade(TEXT, row.Depth - 1);

				DebugText::Draw(
					image,
					x,
					cursor,
					PadRight(std::move(name), HEAP_NAME_FIELD) + " " +
						PadLeft(FormatBytes(static_cast<double>(row.InclusiveBytes)), HEAP_BYTES_FIELD) +
						" " + PadLeft(FormatBytes(static_cast<double>(row.SelfBytes)), HEAP_BYTES_FIELD) +
						" " + PadLeft(Grouped(static_cast<double>(row.LiveBlocks)), HEAP_BLOCKS_FIELD) + " " +
						PadLeft(growth, HEAP_RATE_FIELD),
					colour.R,
					colour.G,
					colour.B,
					scale
				);
				cursor += rowHeight;
			}
		}

		void DrawCategories(OverlayImage &image, const DebugPanelData &data, int x, int y, int width) {
			const int scale = data.Scale;
			const int lineHeight = DebugText::LineHeight(scale);
			const int barHeight = DebugText::GLYPH_HEIGHT * scale;
			const int labelWidth = CategoryLabelWidth(scale);
			const int trackWidth = std::max(width - labelWidth, 1);

			// One pass over the spans for every category, not one pass per
			// category. The old shape read the whole frame four times to
			// produce four numbers, and would have read it once more for every
			// category anybody added.
			std::array<float, CATEGORY_BAR_COUNT> shares{};
			{
				ENGINE_PROFILE_CAT("category totals", core::ProfileCategory::Render);
				shares = CategoryShares(data);
			}

			// The figures beside the bars, which the shares no longer carry -
			// a share is what the bar is drawn from and a millisecond count is
			// what the reader is told, and they are not the same number.
			std::array<float, static_cast<size_t>(core::ProfileCategory::Count)> totals{};
			for (const auto &span : data.Spans) {
				const auto index = static_cast<size_t>(span.Category);
				if (index < totals.size()) {
					totals[index] += span.SelfMilliseconds;
				}
			}

			ENGINE_PROFILE_CAT("category glyphs", core::ProfileCategory::Render);

			const auto bar = [&](int cursor, float share, Colour colour) {
				// One pixel minimum, so a category that ran and cost nothing
				// measurable is still visibly present rather than absent.
				const auto barWidth =
					std::clamp(static_cast<int>(static_cast<float>(trackWidth) * share), 1, trackWidth);
				image.Blend(x + labelWidth, cursor, barWidth, barHeight, colour.R, colour.G, colour.B, 235);
			};

			int cursor = y;
			for (size_t index = 0; index < static_cast<size_t>(core::ProfileCategory::Count); index++) {
				const auto category = static_cast<core::ProfileCategory>(index);

				// Idle is not part of the busy total these bars are drawn
				// against, so a bar for it would be a share of something it is
				// not in. The header reports it as a number instead, which is
				// the only place it means anything.
				if (category == core::ProfileCategory::Idle) {
					continue;
				}

				const std::string_view name = core::GetCategoryName(category);

				DebugText::Draw(
					image,
					x,
					cursor,
					Format(
						"%.*s %.2f",
						static_cast<int>(name.size()),
						name.data(),
						static_cast<double>(totals[index])
					),
					TEXT.R,
					TEXT.G,
					TEXT.B,
					scale
				);

				bar(cursor, shares[index], CATEGORY_COLOURS[index]);
				cursor += lineHeight;
			}

			// The category bars are self time, and self time only covers what a
			// scope was opened around. Without this last bar they sum to less
			// than the frame and nothing on the panel says why - the reader is
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

			bar(cursor, shares[UNMARKED_BAR], TEXT_DIM);
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
				// Writes that went into Value this frame. A total summed over
				// six calls is a different number from one read once.
				uint32_t Samples = 0;
			};

			std::vector<PlannedRow> rows;
			std::vector<std::string> text;

			{
				ENGINE_PROFILE_CAT("row select", core::ProfileCategory::Render);

				int cursor = y;
				int skipped = 0;
				const auto plan = [&](std::string_view name, float value, bool isTime, uint32_t samples) {
					if (skipped < data.Scroll) {
						skipped++;
						return;
					}
					if (cursor + lineHeight > y + available) {
						return;
					}
					rows.push_back(PlannedRow{name, value, isTime, cursor, 0.0f, samples});
					cursor += lineHeight;
				};

				if (data.Tab == ProfilerTab::Systems) {
					rows.reserve(data.Systems.size());
					for (const auto &system : data.Systems) {
						plan(system.Name, system.Milliseconds, true, 1);
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
							counter.IsTime,
							counter.Samples
						);
					}
				}
			}

			{
				// A system's name is the name of the span the scheduler opens
				// around it, so the history already has it and the same RMAX
				// column works here without anything extra being recorded - at
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
					// A count written six times in a frame is a total, not a
					// reading. The panel says which it is rather than leaving
					// the difference to be guessed at.
					std::string name(row.Name);
					if (row.Samples > 1) {
						name += " X" + std::to_string(row.Samples);
					}

					text.push_back(
						row.IsTime ? Format(
										 "%6.2f %6.2f  %.*s",
										 static_cast<double>(row.Value),
										 static_cast<double>(row.RecentMaximum),
										 static_cast<int>(name.size()),
										 name.data()
									 )
								   // Grouped, because a counter reading 1048576 and one
								   // reading 104857 are the same width of digits at the
								   // glance this panel is read at.
								   : Format(
										 "%9s  %.*s",
										 Grouped(static_cast<double>(row.Value)).c_str(),
										 static_cast<int>(name.size()),
										 name.data()
									 )
					);
				}
			}

			{
				// Before the glyphs, because the bar is behind the text.
				ENGINE_PROFILE_CAT("row bars", core::ProfileCategory::Render);
				for (size_t index = 0; index < rows.size(); index++) {
					const auto &row = rows[index];

					// The same tint the flame graph uses, for the same reason.
					if ((index & 1) != 0) {
						image.Blend(x, row.Y - scale, width, lineHeight, 255, 255, 255, STRIPE_ALPHA);
					}

					if (!row.IsTime) {
						continue;
					}

					// A track, so a short bar reads as short rather than as
					// missing.
					image.Blend(
						x + labelWidth,
						row.Y,
						width - labelWidth,
						DebugText::GLYPH_HEIGHT * scale,
						255,
						255,
						255,
						TRACK_ALPHA
					);
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
			// fill below is going to be - so it is upstream of the panel's
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
					// leaving them out of the budget does not move the panel - it
					// clips the last span off the bottom of it.
					bodyRows = visible + 2 - std::max(data.Scroll, 0);
					break;
				}
				case ProfilerTab::Categories:
					// Every category but `Idle`, which is a figure in the header
					// rather than a bar, plus the unmarked bar drawn after them so
					// that the bars sum to the busy frame rather than to less than
					// it. The two adjustments cancel, which is why this reads as
					// the plain category count and is not one.
					bodyRows = static_cast<int>(core::ProfileCategory::Count) - 1 + 1;
					break;
				case ProfilerTab::Systems:
					bodyRows = static_cast<int>(data.Systems.size()) - std::max(data.Scroll, 0);
					break;
				case ProfilerTab::Counters:
					bodyRows = static_cast<int>(data.Counters.size()) - std::max(data.Scroll, 0);
					break;
				case ProfilerTab::Heap:
					// Two heading lines, the plot, the column header, then the
					// tree. The plot is counted here rather than left to
					// overflow, because it is drawn before the rows and a panel
					// sized for the rows alone would clip the tree away entirely.
					bodyRows = static_cast<int>(data.HeapRows.size()) - std::max(data.Scroll, 0) +
							   HEAP_PLOT_ROWS + 4;
					break;
				case ProfilerTab::Count:
					break;
				}
				bodyRows = std::clamp(bodyRows, 1, 40);
			}

			// Top-anchored, stacked under the statistics panel when that is
			// open. Anchored to the bottom, the rows grew upwards - so the
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

				// Which views there are, which is open, and the keys that move
				// between them.
				DrawTabStrip(image, data, writer.X, writer.Y, width - padding * 2);
				writer.Skip();

				// RMAX is only meaningful against a window, so the window says how
				// much of itself it has: a reading over 0.2 s of history and one
				// over the full five seconds are not the same claim.
				// Frame, then busy, then what the difference was spent on. A
				// vsynced frame is mostly a sleep, and a panel that reports only
				// the total makes a fast engine look like a slow one.
				writer.Line(
					Format(
						"%.2f MS  BUSY %.2f  IDLE %.2f  DEPTH %u  TRACY %s  RMAX %.1fS%s",
						static_cast<double>(data.FrameMilliseconds),
						static_cast<double>(data.BusyMilliseconds()),
						static_cast<double>(data.IdleMilliseconds),
						data.DepthLimit,
						data.TracyAttached ? "ON" : "OFF",
						data.HistorySeconds,
						data.DroppedSpans > 0 ? "   SPANS DROPPED!" : ""
					),
					data.DroppedSpans > 0 ? TEXT_WARN : ColourForMilliseconds(data.BusyMilliseconds())
				);
				writer.Skip();
			}

			const int bodyTop = writer.Y;
			const int available = top + height - padding - bodyTop;
			const int bodyWidth = width - padding * 2;

			switch (data.Tab) {
			// One span per tab body rather than one for all of them. The tabs
			// cost different amounts - a flame graph is a row and a timeline bar
			// per span, categories are four bars - and a single name covering
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
			case ProfilerTab::Heap: {
				ENGINE_PROFILE_CAT("heap panel", core::ProfileCategory::Render);
				DrawHeap(image, data, padding, bodyTop, bodyWidth, available);
				break;
			}
			case ProfilerTab::Count:
				break;
			}
		}
	}

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
		// **`Connected` and not merely `ShowNetwork`.** A client with no
		// `--connect` has no link, and a panel of zeroes says "the link is up
		// and idle" - which is a different and far more alarming reading than
		// "there is no link". Enforced here rather than left to the caller,
		// because there is one place to get it wrong and this is it.
		if (data.ShowNetwork && data.Network.Connected) {
			ENGINE_PROFILE_CAT("network panel", core::ProfileCategory::Render);
			DrawNetwork(image, data);
		}
		if (data.ShowFrameGraph) {
			ENGINE_PROFILE_CAT("profiler panel", core::ProfileCategory::Render);
			DrawFrameGraphPanel(image, data);
		}
	}
}
