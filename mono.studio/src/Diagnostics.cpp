// The two panels that answer "why is this slow".
//
// **imgui windows rather than the client's overlay, and that is the whole
// difference.** `render::DrawDebugPanels` writes pixels into a
// `render::OverlayImage`, which is right for a game — there is no other
// interface to put them in. In the editor there is: the overlay is drawn under
// the dockspace, so panels written into it are painted over by imgui every
// frame, and reusing it here meant two panels that drew correctly and could
// never be seen.
//
// So these are ordinary panels. They dock, they close, they are in the View
// menu, and they read exactly the same `core::FrameGraph` the client's overlay
// does — the data is shared even though the drawing is not.

#include <engine/core/FrameGraph.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
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
			// seventh category did not overflow — it silently took engine's
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
				0.00f, // idle — returned above, and here so the array lines up
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
			ImGui::ColorConvertHSVtoRGB(hue, std::max(saturation, 0.45f), std::max(value, 0.70f), red, green, blue);

			return IM_COL32(
				static_cast<int>(red * 255.0f), static_cast<int>(green * 255.0f), static_cast<int>(blue * 255.0f), 235
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

		// The headline, big enough to read from across a desk — which is what
		// somebody watching for a stutter is doing.
		{
			const engine::ui::ScopedFont large(
				engine::ui::Typeface::Interface, engine::ui::TextSize::Large
			);
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
			// frame every second does not — an average alone cannot tell those
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

		// **Collected only while this panel is open.** Recording every span of
		// every frame is real work, and the reason to open this panel is that
		// work is scarce. The client's overlay makes the same trade.
		const std::vector<FrameSpan> &spans = FrameGraph::Spans();

		const float frameMs = FrameGraph::FrameMilliseconds();
		const float idleMs = FrameGraph::CategoryMilliseconds(ProfileCategory::Idle);
		const float busyMs = frameMs > idleMs ? frameMs - idleMs : frameMs;

		ImGui::Text("%.2f ms", static_cast<double>(frameMs));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text("  busy %.2f   idle %.2f   unmarked %.2f", static_cast<double>(busyMs),
					static_cast<double>(idleMs),
					static_cast<double>(FrameGraph::UnmarkedMilliseconds()));
		ImGui::PopStyleColor();

		// **The share is of busy, not of the frame.** With vsync on, fifteen of
		// a sixteen millisecond frame are a sleep — so a span measured against
		// the whole frame reads as one per cent whatever it costs, and the
		// panel becomes one on which nothing is ever worth optimising.
		if (const size_t dropped = FrameGraph::Dropped(); dropped > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%zu spans dropped — the frame overflowed the buffer", dropped);
			ImGui::PopStyleColor();
		}

		if (spans.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled("nothing recorded yet — the next frame fills this in");
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
		for (const FrameSpan &span : spans) {
			deepest = std::max(deepest, span.Depth);
		}

		const float rowHeight = engine::ui::Scaled(engine::ui::Size::Row) * 0.72f;
		const float graphHeight = rowHeight * static_cast<float>(deepest + 1);

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float graphWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
		const float scale = frameMs > 0.0001f ? graphWidth / frameMs : 0.0f;

		ImDrawList *draw = ImGui::GetWindowDrawList();
		const FrameSpan *hovered = nullptr;

		for (const FrameSpan &span : spans) {
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
			ImGui::TextUnformatted(std::string(hovered->Name).c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text(
				"%.3f ms   self %.3f   idle %.3f",
				static_cast<double>(hovered->Milliseconds),
				static_cast<double>(hovered->SelfMilliseconds),
				static_cast<double>(hovered->IdleMilliseconds)
			);
			ImGui::Text("%s%s", GetCategoryName(hovered->Category).data(),
						hovered->Reported ? "   (reported from another thread)" : "");
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

			for (const FrameSpan &span : spans) {
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
