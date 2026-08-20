// The two panels that answer "why is this slow".
//
// **imgui windows rather than the client's overlay, and that is the whole
// difference.** `render::DrawDebugPanels` writes pixels into a
// `render::OverlayImage`, which is right for a game - there is no other
// interface to put them in. In the editor there is: the overlay is drawn under
// the dockspace, so panels written into it are painted over by imgui every
// frame, and reusing it here meant two panels that drew correctly and could
// never be seen.
//
// So these are ordinary panels. They dock, they close, they are in the View
// menu, and they read exactly the same `core::FrameGraph` the client's overlay
// does - the data is shared even though the drawing is not.

#include <engine/core/FrameGraph.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <imgui.h>
#include <iterator>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	namespace {
		using engine::core::FrameGraph;
		using engine::core::FrameSpan;
		using engine::core::ProfileCategory;

		// How often the frame graph publishes, in seconds. Index 0 is every
		// frame, which is what the panel did before it could be told otherwise.
		//
		// **Six, spanning two decades.** A quarter of a second is as slow as a
		// reading can be and still feel live; five seconds is what somebody
		// watching a load or a stall wants. Filling the gap between them with
		// more entries would be a longer menu answering the same question.
		constexpr std::array<float, 6> FRAME_GRAPH_INTERVALS{0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f};

		constexpr std::array<const char *, 6> FRAME_GRAPH_INTERVAL_NAMES{
			"every frame", "250 ms", "500 ms", "1 s", "2 s", "5 s"
		};

		// A colour per category, so a flame graph is readable as shape rather
		// than as a list of names.
		//
		// **Derived from the palette's accent rather than fixed**, because the
		// editor has seven themes and a bar chart in another theme's colours
		// reads as a foreign window. The hues are spread around the accent so
		// that the categories stay distinguishable in all of them.
		unsigned int ColourOf(ProfileCategory category) {
			const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(engine::ui::AccentColour());

			float hue = 0.0f;
			float saturation = 0.0f;
			float value = 0.0f;
			ImGui::ColorConvertRGBtoHSV(accent.x, accent.y, accent.z, hue, saturation, value);

			// Idle is the one that must not compete: a vsynced frame is mostly
			// idle, and a bright bar across the whole graph is the thing a
			// reader's eye lands on first for no reason at all.
			if (category == ProfileCategory::Idle) {
				return IM_COL32(70, 74, 86, 190);
			}

			// One turn per category, in enum order, spread around the circle so
			// that neighbours in the list are not neighbours in hue.
			//
			// **Positional, and sized from the enum rather than from a literal
			// count.** This was indexed with a hand-written `< 6`, so a
			// seventh category did not overflow - it silently took engine's
			// hue and drew two subsystems in one colour, which is the one
			// failure a colour key cannot survive. The array is now short by
			// construction if a category is added without a turn, and the
			// assert below says so at build time.
			constexpr float TURN[] = {
				0.00f, // engine
				0.52f, // render
				0.14f, // ECS
				0.86f, // physics
				0.72f, // simulation
				0.30f, // script
				0.42f, // network
				0.62f, // assets
				0.00f, // idle - returned above, and here so the array lines up
			};
			static_assert(
				std::size(TURN) == static_cast<size_t>(ProfileCategory::Count),
				"A ProfileCategory was added without a hue turn in TURN."
			);

			const auto index = static_cast<size_t>(category);
			hue += TURN[index < std::size(TURN) ? index : 0];
			hue -= static_cast<float>(static_cast<int>(hue));

			float red = 0.0f;
			float green = 0.0f;
			float blue = 0.0f;
			ImGui::ColorConvertHSVtoRGB(
				hue, std::max(saturation, 0.45f), std::max(value, 0.70f), red, green, blue
			);

			return IM_COL32(
				static_cast<int>(red * 255.0f),
				static_cast<int>(green * 255.0f),
				static_cast<int>(blue * 255.0f),
				235
			);
		}

		// One row of a label and a value, which is most of the statistics panel.
		void Row(const char *label, const char *format, ...) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(label);
			ImGui::PopStyleColor();

			ImGui::TableSetColumnIndex(1);
			va_list args;
			va_start(args, format);
			ImGui::TextV(format, args);
			va_end(args);
		}
	}

	void Editor::DrawStatistics() {
		if (!ShowStatistics) {
			return;
		}

		if (!ImGui::Begin("Statistics", &ShowStatistics)) {
			ImGui::End();
			return;
		}

		if (!Statistics.HasSamples()) {
			ImGui::TextDisabled("no frames measured yet");
			ImGui::End();
			return;
		}

		// The headline, big enough to read from across a desk - which is what
		// somebody watching for a stutter is doing.
		{
			const engine::ui::ScopedFont large(engine::ui::Typeface::Interface, engine::ui::TextSize::Large);
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::BrightColour());
			ImGui::Text("%.0f fps", static_cast<double>(Statistics.Current()));
			ImGui::PopStyleColor();
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text("  %.2f ms", static_cast<double>(Statistics.CurrentMilliseconds()));
		ImGui::PopStyleColor();

		ImGui::Separator();

		constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg;

		if (ImGui::BeginTable("##fps", 2, FLAGS)) {
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.45f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.55f);

			Row("minimum", "%.0f fps", static_cast<double>(Statistics.Minimum()));
			Row("average", "%.0f fps", static_cast<double>(Statistics.Average()));
			Row("maximum", "%.0f fps", static_cast<double>(Statistics.Maximum()));

			// **Jitter, and it is the number that matters for "laggy".** A
			// steady 40 fps reads as smooth and a 60 fps average that drops a
			// frame every second does not - an average alone cannot tell those
			// apart, which is exactly the complaint this panel exists to
			// answer.
			Row("jitter", "%.2f ms", static_cast<double>(Statistics.Jitter()));

			ImGui::EndTable();
		}

		ImGui::SeparatorText("this frame");

		if (ImGui::BeginTable("##frame", 2, FLAGS)) {
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.45f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.55f);

			Row("draw calls", "%u", LastFrame.DrawCalls);
			Row("triangles", "%llu", static_cast<unsigned long long>(LastFrame.Triangles));
			Row("uploads",
				"%.2f MiB in %u buffer%s",
				static_cast<double>(LastFrame.UploadedBytes) / (1024.0 * 1024.0),
				LastFrame.UploadCommandBuffers,
				LastFrame.UploadCommandBuffers == 1 ? "" : "s");
			Row("compute",
				"%u dispatch%s, %u dedicated buffer%s on SDL's unified queue",
				LastFrame.ComputeDispatches,
				LastFrame.ComputeDispatches == 1 ? "" : "es",
				LastFrame.AsyncComputeCommandBuffers,
				LastFrame.AsyncComputeCommandBuffers == 1 ? "" : "s");
			Row("downloads",
				"%u later-transfer buffer%s after the main stream",
				LastFrame.DownloadCommandBuffers,
				LastFrame.DownloadCommandBuffers == 1 ? "" : "s");
			Row("traffic plan",
				"%u command buffer%s, submitted serially",
				LastFrame.TrafficCommandBuffers,
				LastFrame.TrafficCommandBuffers == 1 ? "" : "s");

			size_t instances = 0;
			for (const WorldId world : Universe->Worlds()) {
				if (!Universe->IsRemote(world)) {
					instances += InstanceCountOf(world);
				}
			}
			Row("instances", "%zu", instances);
			Row("worlds", "%zu", Universe->Count());
			Row("backend", "%s", Renderer.BackendName().data());

			ImGui::EndTable();
		}

		ImGui::End();
	}

	void Editor::DrawFrameGraph() {
		if (!ShowFrameGraph) {
			return;
		}

		if (!ImGui::Begin("Frame Graph", &ShowFrameGraph)) {
			ImGui::End();
			return;
		}

		// --- what is shown, and when it changes -----------------------------
		//
		// **Collected only while this panel is open.** Recording every span of
		// every frame is real work, and the reason to open this panel is that
		// work is scarce. The client's overlay makes the same trade.
		//
		// What is *drawn* is a snapshot this function publishes rather than the
		// live buffer, and every control below decides when that publish
		// happens. Drawing the live buffer directly is what made the panel
		// hard to use for its one job: at sixty frames a second a bar is gone
		// before the pointer reaches it.
		FrameGraphView &view = FrameGraphState;
		const std::vector<FrameSpan> &live = FrameGraph::Spans();
		const double now = ImGui::GetTime();

		const int chosen = std::clamp(view.Interval, 0, static_cast<int>(FRAME_GRAPH_INTERVALS.size()) - 1);
		const float interval = FRAME_GRAPH_INTERVALS[static_cast<size_t>(chosen)];

		// One frame's spans, names copied - see `Editor::HeldSpan`.
		const auto snapshot = [&live](std::vector<HeldSpan> &into) {
			into.clear();
			into.reserve(live.size());
			for (const FrameSpan &span : live) {
				into.push_back(
					HeldSpan{
						std::string(span.Name),
						span.Depth,
						span.Parent,
						span.StartMilliseconds,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						span.Category,
						span.Reported,
					}
				);
			}
		};

		const auto readScalars = [&view]() {
			view.FrameMilliseconds = FrameGraph::FrameMilliseconds();
			view.IdleMilliseconds = FrameGraph::CategoryMilliseconds(ProfileCategory::Idle);
			view.UnmarkedMilliseconds = FrameGraph::UnmarkedMilliseconds();
			view.Dropped = FrameGraph::Dropped();
		};

		const auto forget = [&view]() {
			view.Summed.clear();
			view.SummedFrameMilliseconds = 0.0f;
			view.SummedIdleMilliseconds = 0.0f;
			view.SummedUnmarkedMilliseconds = 0.0f;
			view.SummedDropped = 0;
			view.Frames = 0;
		};

		if (!view.Paused) {
			if (interval <= 0.0f) {
				// Every frame, which is what this panel always did.
				snapshot(view.Spans);
				readScalars();
				forget();
			} else {
				if (view.Average) {
					// **Matched by name and depth rather than by position.** A
					// frame that opens one fewer span shifts every later index,
					// so summing by position would add a renderer's time to a
					// script's for the rest of the interval. The lists are a
					// few dozen entries, so a linear find per span is cheaper
					// than the map that would avoid it.
					for (const FrameSpan &span : live) {
						HeldSpan *into = nullptr;
						for (HeldSpan &candidate : view.Summed) {
							if (candidate.Depth == span.Depth && candidate.Name == span.Name) {
								into = &candidate;
								break;
							}
						}
						if (into == nullptr) {
							view.Summed.push_back(
								HeldSpan{
									std::string(span.Name),
									span.Depth,
									span.Parent,
									0.0f,
									0.0f,
									0.0f,
									0.0f,
									span.Category,
									span.Reported,
								}
							);
							into = &view.Summed.back();
						}
						into->StartMilliseconds += span.StartMilliseconds;
						into->Milliseconds += span.Milliseconds;
						into->SelfMilliseconds += span.SelfMilliseconds;
						into->IdleMilliseconds += span.IdleMilliseconds;
					}

					view.SummedFrameMilliseconds += FrameGraph::FrameMilliseconds();
					view.SummedIdleMilliseconds += FrameGraph::CategoryMilliseconds(ProfileCategory::Idle);
					view.SummedUnmarkedMilliseconds += FrameGraph::UnmarkedMilliseconds();
					view.SummedDropped += FrameGraph::Dropped();
					view.Frames++;
				}

				// **Also on the first frame the panel is open**, whatever the
				// interval says. Waiting five seconds to draw anything at all
				// reads as a panel that does not work.
				if (now >= view.NextPublish || view.Spans.empty()) {
					if (view.Average && view.Frames > 0) {
						const float frames = static_cast<float>(view.Frames);
						view.Spans = view.Summed;
						for (HeldSpan &span : view.Spans) {
							span.StartMilliseconds /= frames;
							span.Milliseconds /= frames;
							span.SelfMilliseconds /= frames;
							span.IdleMilliseconds /= frames;
						}
						view.FrameMilliseconds = view.SummedFrameMilliseconds / frames;
						view.IdleMilliseconds = view.SummedIdleMilliseconds / frames;
						view.UnmarkedMilliseconds = view.SummedUnmarkedMilliseconds / frames;
						view.Dropped = view.SummedDropped / view.Frames;
					} else {
						snapshot(view.Spans);
						readScalars();
					}

					view.NextPublish = now + static_cast<double>(interval);
					forget();
				}
			}
		}

		const std::vector<HeldSpan> &spans = view.Spans;

		const float frameMs = view.FrameMilliseconds;
		const float idleMs = view.IdleMilliseconds;
		const float busyMs = frameMs > idleMs ? frameMs - idleMs : frameMs;

		ImGui::Text("%.2f ms", static_cast<double>(frameMs));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text(
			"  busy %.2f   idle %.2f   unmarked %.2f",
			static_cast<double>(busyMs),
			static_cast<double>(idleMs),
			static_cast<double>(view.UnmarkedMilliseconds)
		);
		ImGui::PopStyleColor();

		// --- the controls ----------------------------------------------------

		if (ImGui::Button(view.Paused ? "Resume" : "Pause")) {
			view.Paused = !view.Paused;

			// Resuming starts the next interval from now rather than from
			// whenever it was due when the pause began - otherwise a graph
			// paused for a minute republishes on the frame it is resumed and
			// the freeze appears not to have ended cleanly.
			view.NextPublish = now + static_cast<double>(interval);
			forget();
		}

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Freeze what is on screen. Nothing is sampled while paused, so the\n"
				"numbers below are exactly the ones that were there when it was pressed."
			);
		}

		ImGui::SameLine();
		ImGui::SetNextItemWidth(engine::ui::Scaled(120.0f));
		if (ImGui::BeginCombo("update", FRAME_GRAPH_INTERVAL_NAMES[static_cast<size_t>(chosen)])) {
			for (int index = 0; index < static_cast<int>(FRAME_GRAPH_INTERVALS.size()); index++) {
				const bool selected = index == chosen;
				if (ImGui::Selectable(FRAME_GRAPH_INTERVAL_NAMES[static_cast<size_t>(index)], selected)) {
					view.Interval = index;
					view.NextPublish =
						now + static_cast<double>(FRAME_GRAPH_INTERVALS[static_cast<size_t>(index)]);
					forget();
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();

		// **Disabled at "every frame", because there is nothing to average
		// over.** Greyed rather than hidden: a control that disappears when a
		// neighbouring one changes is a control nobody finds again.
		ImGui::BeginDisabled(interval <= 0.0f);
		ImGui::Checkbox("average", &view.Average);
		ImGui::EndDisabled();

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"On: every frame in the interval is summed and the mean is shown, so a\n"
				"span's number is what it typically costs.\n"
				"Off: the interval simply decides how often the panel takes a new sample,\n"
				"and what is shown is one real frame - including the unlucky ones."
			);
		}

		if (view.Paused) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextUnformatted("- paused");
			ImGui::PopStyleColor();
		} else if (interval > 0.0f && view.Average) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("- mean of %u frame(s)", view.Frames);
			ImGui::PopStyleColor();
		}

		// **The share is of busy, not of the frame.** With vsync on, fifteen of
		// a sixteen millisecond frame are a sleep - so a span measured against
		// the whole frame reads as one per cent whatever it costs, and the
		// panel becomes one on which nothing is ever worth optimising.
		// **Three causes, and this used to name the rarest one.** `Dropped`
		// counts buffer overflow, depth past `MAXIMUM_DEPTH`, and scopes opened
		// off the frame's owning thread - and with `MAXIMUM_SPANS` at 4096
		// against a frame of a few dozen, overflow is the one that essentially
		// never happens. What does happen every frame is the third: two worlds
		// ticking on workers open a span per phase and per system, and every one
		// of them is refused. Reading "overflowed the buffer" sent at least one
		// investigation at the span budget, which was 1% used.
		if (const size_t dropped = view.Dropped; dropped > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%zu spans dropped - off-thread, too deep, or past the buffer", dropped);
			ImGui::PopStyleColor();
		}

		// **The switch belongs on the panel that is lying without it.** The
		// dropped-span line above is the symptom and this is the cure, so
		// putting them a menu apart would mean reading the warning and having
		// to know what to go and find. Flipping it here also means the two
		// flame graphs - parallel and serial - come from one session and one
		// build, which is what makes them comparable at all.
		bool serial = engine::parallel::ForceSerialCompute();
		if (ImGui::Checkbox("force serial compute", &serial)) {
			engine::parallel::SetForceSerialCompute(serial);
		}

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip(
				"Every parallel dispatch runs on the thread that asked, so no span is dropped.\n"
				"The frame gets slower on purpose: this measures a serial cost, not a verdict\n"
				"on the parallel one."
			);
		}

		// Said on the panel rather than left for somebody to remember, because
		// the whole risk of this switch is reading a number taken with it on and
		// treating it as the engine's real cost.
		if (serial) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextUnformatted("- timings are serial, not the shipped cost");
			ImGui::PopStyleColor();
		}

		if (spans.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled("nothing recorded yet - the next frame fills this in");
			ImGui::End();
			return;
		}

		ImGui::Separator();

		// --- the flame graph ------------------------------------------------
		//
		// **Laid out by time, not by rank.** A bar chart sorted by cost says
		// what was expensive; a flame graph says what was expensive *and* what
		// it happened inside, which is the question when a frame is slow for a
		// reason nobody wrote down. `StartMilliseconds` and `Depth` are exactly
		// the two axes.

		uint32_t deepest = 0;
		for (const HeldSpan &span : spans) {
			deepest = std::max(deepest, span.Depth);
		}

		const float rowHeight = engine::ui::Scaled(engine::ui::Size::Row) * 0.72f;
		const float graphHeight = rowHeight * static_cast<float>(deepest + 1);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float graphWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
		const float scale = frameMs > 0.0001f ? graphWidth / frameMs : 0.0f;

		ImDrawList *draw = ImGui::GetWindowDrawList();
		const HeldSpan *hovered = nullptr;

		for (const HeldSpan &span : spans) {
			const float left = origin.x + span.StartMilliseconds * scale;
			const float width = std::max(span.Milliseconds * scale, 1.0f);
			const float top = origin.y + static_cast<float>(span.Depth) * rowHeight;

			const ImVec2 upper(left, top);
			const ImVec2 lower(left + width, top + rowHeight - 1.0f);

			draw->AddRectFilled(upper, lower, ColourOf(span.Category), 2.0f);

			if (ImGui::IsMouseHoveringRect(upper, lower)) {
				hovered = &span;
				draw->AddRect(upper, lower, engine::ui::BrightColour(), 2.0f);
			}

			// Only where the label fits. Text clipped mid-word is noise, and a
			// flame graph is read as shape first.
			if (width > engine::ui::Scaled(34.0f)) {
				draw->PushClipRect(upper, lower, true);
				draw->AddText(
					ImVec2(left + 3.0f, top + 1.0f),
					IM_COL32(16, 18, 22, 235),
					span.Name.data(),
					span.Name.data() + span.Name.size()
				);
				draw->PopClipRect();
			}
		}

		ImGui::Dummy(ImVec2(graphWidth, graphHeight));

		if (hovered != nullptr) {
			ImGui::BeginTooltip();
			ImGui::TextUnformatted(hovered->Name.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text(
				"%.3f ms   self %.3f   idle %.3f",
				static_cast<double>(hovered->Milliseconds),
				static_cast<double>(hovered->SelfMilliseconds),
				static_cast<double>(hovered->IdleMilliseconds)
			);
			ImGui::Text(
				"%s%s",
				GetCategoryName(hovered->Category).data(),
				hovered->Reported ? "   (reported from another thread)" : ""
			);
			ImGui::PopStyleColor();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();

		// --- the table ------------------------------------------------------

		constexpr ImGuiTableFlags FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
										  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;

		if (ImGui::BeginTable("##spans", 4, FLAGS)) {
			ImGui::TableSetupColumn("span", ImGuiTableColumnFlags_WidthStretch, 0.52f);
			ImGui::TableSetupColumn("busy", ImGuiTableColumnFlags_WidthStretch, 0.16f);
			ImGui::TableSetupColumn("idle", ImGuiTableColumnFlags_WidthStretch, 0.16f);
			ImGui::TableSetupColumn("share", ImGuiTableColumnFlags_WidthStretch, 0.16f);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			for (const HeldSpan &span : spans) {
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::Indent(static_cast<float>(span.Depth) * engine::ui::Scaled(10.0f));
				ImGui::TextUnformatted(span.Name.data(), span.Name.data() + span.Name.size());
				ImGui::Unindent(static_cast<float>(span.Depth) * engine::ui::Scaled(10.0f));

				const float spanBusy = span.Milliseconds - span.IdleMilliseconds;
				const float share = busyMs > 0.0001f ? (spanBusy / busyMs) * 100.0f : 0.0f;

				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%.2f", static_cast<double>(spanBusy));

				ImGui::TableSetColumnIndex(2);
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				if (span.IdleMilliseconds > 0.0001f) {
					ImGui::Text("%.2f", static_cast<double>(span.IdleMilliseconds));
				} else {
					ImGui::TextUnformatted("-");
				}
				ImGui::PopStyleColor();

				ImGui::TableSetColumnIndex(3);
				// The expensive rows in the warning colour, so the thing worth
				// looking at is the thing that catches the eye.
				if (share >= 25.0f) {
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
					ImGui::Text("%.1f%%", static_cast<double>(share));
					ImGui::PopStyleColor();
				} else {
					ImGui::Text("%.1f%%", static_cast<double>(share));
				}
			}

			ImGui::EndTable();
		}

		ImGui::End();
	}
}
