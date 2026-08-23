// The three panels that answer "why is this slow" and "what is holding this
// memory".
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
#include <engine/core/HeapProfile.hpp>
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
#include <studio/Diagnostics.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <unordered_map>
#include <vector>

namespace studio {
	void AccumulateDiagnosticSpans(
		std::span<const engine::core::FrameSpan> frame, std::vector<DiagnosticSpan> &totals
	) {
		struct SiblingKey {
			uint32_t Parent = engine::core::FrameGraph::NO_PARENT;
			std::string_view Name;

			bool operator==(const SiblingKey &) const = default;
		};
		struct StructuralKey {
			SiblingKey Sibling;
			uint32_t Depth = 0;
			uint32_t Ordinal = 0;

			bool operator==(const StructuralKey &) const = default;
		};
		struct SiblingHash {
			size_t operator()(const SiblingKey &key) const {
				return std::hash<std::string_view>{}(key.Name) ^ (static_cast<size_t>(key.Parent) << 1);
			}
		};
		struct StructuralHash {
			size_t operator()(const StructuralKey &key) const {
				const size_t sibling = SiblingHash{}(key.Sibling);
				return sibling ^ (static_cast<size_t>(key.Depth) << 3) ^
					   (static_cast<size_t>(key.Ordinal) << 11);
			}
		};

		// Retained because averaging runs every frame while the panel is open.
		// The previous pair of linear searches made N spans cost N squared and
		// turned a granular particle or server capture into profiler lag.
		static thread_local std::vector<uint32_t> targets;
		static thread_local std::unordered_map<SiblingKey, uint32_t, SiblingHash> ordinals;
		static thread_local std::unordered_map<StructuralKey, uint32_t, StructuralHash> targetByKey;

		totals.reserve(totals.size() + frame.size());
		targets.assign(frame.size(), engine::core::FrameGraph::NO_PARENT);
		ordinals.clear();
		targetByKey.clear();
		ordinals.reserve(totals.size());
		targetByKey.reserve(totals.size() + frame.size());

		for (size_t index = 0; index < totals.size(); index++) {
			const DiagnosticSpan &span = totals[index];
			const SiblingKey sibling{span.Parent, span.Name};
			const uint32_t ordinal = ordinals[sibling]++;
			targetByKey.emplace(
				StructuralKey{.Sibling = sibling, .Depth = span.Depth, .Ordinal = ordinal},
				static_cast<uint32_t>(index)
			);
		}
		ordinals.clear();

		for (size_t index = 0; index < frame.size(); index++) {
			const engine::core::FrameSpan &source = frame[index];
			const uint32_t parent =
				source.Parent < index ? targets[source.Parent] : engine::core::FrameGraph::NO_PARENT;

			// The ordinal is local to one parent. Three worlds can each contain an
			// `ecs.systems`; they are three children of three different parents, not
			// the first, second and third occurrence of one global name.
			const SiblingKey sibling{parent, source.Name};
			const uint32_t ordinal = ordinals[sibling]++;
			const StructuralKey key{.Sibling = sibling, .Depth = source.Depth, .Ordinal = ordinal};

			DiagnosticSpan *target = nullptr;
			uint32_t targetIndex = engine::core::FrameGraph::NO_PARENT;
			const auto found = targetByKey.find(key);
			if (found != targetByKey.end()) {
				targetIndex = found->second;
				target = &totals[targetIndex];
			}

			if (target == nullptr) {
				totals.push_back(
					DiagnosticSpan{
						.Name = std::string(source.Name),
						.Depth = source.Depth,
						.Parent = parent,
						.Category = source.Category,
						.Reported = source.Reported,
					}
				);
				targetIndex = static_cast<uint32_t>(totals.size() - 1);
				target = &totals.back();
				targetByKey.emplace(key, targetIndex);
			}

			targets[index] = targetIndex;
			target->StartMilliseconds += source.StartMilliseconds;
			target->Milliseconds += source.Milliseconds;
			target->SelfMilliseconds += source.SelfMilliseconds;
			target->IdleMilliseconds += source.IdleMilliseconds;
			target->Occurrences++;
		}

		// Keys borrow names from `totals` and `frame`. Keep the buckets between
		// frames, but never keep those borrowed views after either owner can move.
		ordinals.clear();
		targetByKey.clear();
	}

	void FinishDiagnosticAverage(std::vector<DiagnosticSpan> &spans, uint32_t frames) {
		if (frames == 0) {
			return;
		}

		const float frameCount = static_cast<float>(frames);
		for (DiagnosticSpan &span : spans) {
			const float occurrences = static_cast<float>(std::max(span.Occurrences, 1u));
			span.StartMilliseconds /= occurrences;
			span.Milliseconds /= frameCount;
			span.SelfMilliseconds /= frameCount;
			span.IdleMilliseconds /= frameCount;
		}
	}

	void FitReportedDiagnosticTimeline(std::vector<DiagnosticSpan> &spans, float frameMilliseconds) {
		const float frameEnd = std::max(frameMilliseconds, 0.0f);
		const size_t count = spans.size();
		for (size_t parentIndex = 0; parentIndex < count; parentIndex++) {
			DiagnosticSpan &parent = spans[parentIndex];
			const float parentStart = std::clamp(parent.StartMilliseconds, 0.0f, frameEnd);
			const float parentEnd =
				std::clamp(parent.StartMilliseconds + parent.Milliseconds, parentStart, frameEnd);
			if (parentEnd <= parentStart) {
				continue;
			}

			float reportedTotal = 0.0f;
			size_t reportedCount = 0;
			for (size_t childIndex = parentIndex + 1; childIndex < count; childIndex++) {
				const DiagnosticSpan &child = spans[childIndex];
				if (child.Depth <= parent.Depth) {
					break;
				}
				if (child.Parent == parentIndex && child.Reported) {
					reportedTotal += std::max(child.Milliseconds, 0.0f);
					reportedCount++;
				}
			}
			if (reportedCount == 0) {
				continue;
			}

			float gapStart = parentStart;
			float gapEnd = parentEnd;
			if (!parent.Reported) {
				float coveredUntil = parentStart;
				float widest = -1.0f;
				for (size_t childIndex = parentIndex + 1; childIndex < count; childIndex++) {
					const DiagnosticSpan &child = spans[childIndex];
					if (child.Depth <= parent.Depth) {
						break;
					}
					if (child.Parent != parentIndex || child.Reported || child.Milliseconds <= 0.0f) {
						continue;
					}

					const float childStart = std::clamp(child.StartMilliseconds, parentStart, parentEnd);
					const float childEnd =
						std::clamp(child.StartMilliseconds + child.Milliseconds, childStart, parentEnd);
					if (childStart - coveredUntil > widest) {
						widest = childStart - coveredUntil;
						gapStart = coveredUntil;
						gapEnd = childStart;
					}
					coveredUntil = std::max(coveredUntil, childEnd);
				}
				if (parentEnd - coveredUntil > widest) {
					gapStart = coveredUntil;
					gapEnd = parentEnd;
				}
			}

			const float gap = std::max(gapEnd - gapStart, 0.0f);
			float cursor = gapStart;
			for (size_t childIndex = parentIndex + 1; childIndex < count; childIndex++) {
				DiagnosticSpan &child = spans[childIndex];
				if (child.Depth <= parent.Depth) {
					break;
				}
				if (child.Parent != parentIndex || !child.Reported) {
					continue;
				}

				const float share = reportedTotal > 0.0f ? std::max(child.Milliseconds, 0.0f) / reportedTotal
														 : 1.0f / static_cast<float>(reportedCount);
				child.StartMilliseconds = cursor;
				child.Milliseconds = gap * share;
				cursor += child.Milliseconds;
			}
		}
	}

	void AppendUnaccountedDiagnosticSpans(std::vector<DiagnosticSpan> &spans) {
		const size_t recorded = spans.size();
		struct ChildInterval {
			uint32_t Parent = engine::core::FrameGraph::NO_PARENT;
			float Start = 0.0f;
			float End = 0.0f;
		};

		// Retained because this runs every repaint while the profiler is open.
		// A diagnostic that allocates a second span tree each frame would show up
		// in the heap panel as the problem it was opened to investigate.
		static thread_local std::vector<ChildInterval> children;
		children.clear();
		children.reserve(recorded);
		for (const DiagnosticSpan &child : spans) {
			if (child.Parent >= recorded || child.Milliseconds <= 0.0f || child.Reported) {
				continue;
			}

			const DiagnosticSpan &parent = spans[child.Parent];
			const float parentStart = parent.StartMilliseconds;
			const float parentEnd = parentStart + std::max(parent.Milliseconds, 0.0f);
			const float childStart = std::clamp(child.StartMilliseconds, parentStart, parentEnd);
			const float childEnd =
				std::clamp(child.StartMilliseconds + child.Milliseconds, parentStart, parentEnd);
			if (childEnd > childStart) {
				children.push_back({child.Parent, childStart, childEnd});
			}
		}

		std::sort(
			children.begin(), children.end(), [](const ChildInterval &left, const ChildInterval &right) {
				if (left.Parent != right.Parent) {
					return left.Parent < right.Parent;
				}
				if (left.Start != right.Start) {
					return left.Start < right.Start;
				}
				return left.End < right.End;
			}
		);
		spans.reserve(recorded + children.size() * 2);

		size_t firstChild = 0;
		for (size_t parentIndex = 0; parentIndex < recorded; parentIndex++) {
			const DiagnosticSpan parent = spans[parentIndex];
			// Reported hierarchies are logical worker-time breakdowns rather than
			// intervals on this thread. Inventing gaps between them would label
			// concurrency as unaccounted wall time.
			if (parent.Reported) {
				continue;
			}
			const float parentStart = parent.StartMilliseconds;
			const float parentEnd = parentStart + std::max(parent.Milliseconds, 0.0f);
			if (parentEnd <= parentStart) {
				continue;
			}

			while (firstChild < children.size() && children[firstChild].Parent < parentIndex) {
				firstChild++;
			}

			// A leaf's own work is already named by the leaf. Gaps only add
			// information when a parent has measured children to compare against.
			if (firstChild == children.size() || children[firstChild].Parent != parentIndex) {
				continue;
			}

			// `SelfMilliseconds` is computed from the original frame tree before
			// Studio averages anything. The geometric gaps below are exact for one
			// frame, but sparse children have conditional start positions and
			// per-frame averaged widths. Their apparent gaps can therefore be much
			// wider than the work the parent actually did. Scale the display gaps to
			// the measured self total so "unaccounted" never invents time.
			float geometricGaps = 0.0f;
			float coveredUntil = parentStart;
			for (size_t childIndex = firstChild;
				 childIndex < children.size() && children[childIndex].Parent == parentIndex;
				 childIndex++) {
				const float childStart = children[childIndex].Start;
				const float childEnd = children[childIndex].End;
				geometricGaps += std::max(childStart - coveredUntil, 0.0f);
				coveredUntil = std::max(coveredUntil, childEnd);
			}
			geometricGaps += std::max(parentEnd - coveredUntil, 0.0f);
			const float self = std::clamp(parent.SelfMilliseconds, 0.0f, parent.Milliseconds);
			const float gapScale = geometricGaps > 0.0f ? std::min(self / geometricGaps, 1.0f) : 0.0f;

			const auto appendGap = [&](float start, float available) {
				const float measured = available * gapScale;
				if (measured <= 0.0f) {
					return;
				}
				spans.push_back(
					DiagnosticSpan{
						.Name = "unaccounted",
						.Depth = parent.Depth + 1,
						.Parent = static_cast<uint32_t>(parentIndex),
						.StartMilliseconds = start,
						.Milliseconds = measured,
						.SelfMilliseconds = measured,
						.Category = engine::core::ProfileCategory::Engine,
					}
				);
			};

			coveredUntil = parentStart;
			for (size_t childIndex = firstChild;
				 childIndex < children.size() && children[childIndex].Parent == parentIndex;
				 childIndex++) {
				const float childStart = children[childIndex].Start;
				appendGap(coveredUntil, std::max(childStart - coveredUntil, 0.0f));
				coveredUntil = std::max(coveredUntil, children[childIndex].End);
			}
			appendGap(coveredUntil, std::max(parentEnd - coveredUntil, 0.0f));
		}
	}

	uint32_t LayoutDiagnosticRows(
		std::span<const DiagnosticSpan> spans,
		float frameMilliseconds,
		float minimumMilliseconds,
		std::vector<uint32_t> &rows
	) {
		rows.assign(spans.size(), 0);
		if (spans.empty()) {
			return 0;
		}

		uint32_t deepest = 0;
		for (const DiagnosticSpan &span : spans) {
			deepest = std::max(deepest, span.Depth);
		}

		struct ParentLanes {
			uint32_t Parent = engine::core::FrameGraph::NO_PARENT;
			std::vector<float> Ends;
		};
		std::vector<std::vector<ParentLanes>> parentLanes(static_cast<size_t>(deepest) + 1);
		std::vector<uint32_t> depthLaneCounts(static_cast<size_t>(deepest) + 1, 1);
		std::vector<uint32_t> lanes(spans.size(), 0);
		const float frameEnd = std::max(frameMilliseconds, 0.0f);
		const float minimum = std::max(minimumMilliseconds, 0.0f);

		for (size_t index = 0; index < spans.size(); index++) {
			const DiagnosticSpan &span = spans[index];
			const float latestStart = std::max(frameEnd - minimum, 0.0f);
			const float left = std::clamp(span.StartMilliseconds, 0.0f, latestStart);
			const float right = std::min(frameEnd, left + std::max(span.Milliseconds, minimum));

			auto &groups = parentLanes[span.Depth];
			auto found = std::find_if(groups.begin(), groups.end(), [&](const ParentLanes &candidate) {
				return candidate.Parent == span.Parent;
			});
			if (found == groups.end()) {
				groups.push_back(ParentLanes{.Parent = span.Parent, .Ends = {}});
				found = std::prev(groups.end());
			}
			auto &ends = found->Ends;
			uint32_t lane = 0;
			while (lane < ends.size() && left < ends[lane]) {
				lane++;
			}
			if (lane == ends.size()) {
				ends.push_back(right);
			} else {
				ends[lane] = right;
			}
			lanes[index] = lane;
			depthLaneCounts[span.Depth] = std::max(depthLaneCounts[span.Depth], lane + 1);
		}

		std::vector<uint32_t> offsets(parentLanes.size(), 0);
		uint32_t rowCount = 0;
		for (size_t depth = 0; depth < parentLanes.size(); depth++) {
			offsets[depth] = rowCount;
			rowCount += depthLaneCounts[depth];
		}

		for (size_t index = 0; index < spans.size(); index++) {
			rows[index] = offsets[spans[index].Depth] + lanes[index];
		}
		return rowCount;
	}

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

				// The widest gap the wheel had left, and it needs to be: this
				// is the device and `render` above it is the CPU recording for
				// it, so the one comparison a reader makes on this panel is
				// between those two bars. Neighbouring hues would make that
				// comparison the hardest one instead of the easiest.
				0.22f, // GPU

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

			const engine::render::FrameResult frame = FocusedViewport < ViewportResults.size()
														  ? ViewportResults[FocusedViewport]
														  : engine::render::FrameResult{};
			Row("draw calls", "%u", frame.DrawCalls);
			Row("triangles", "%llu", static_cast<unsigned long long>(frame.Triangles));
			Row("uploads",
				"%.2f MiB in %u buffer%s",
				static_cast<double>(frame.UploadedBytes) / (1024.0 * 1024.0),
				frame.UploadCommandBuffers,
				frame.UploadCommandBuffers == 1 ? "" : "s");
			Row("compute",
				"%u dispatch%s, %u dedicated buffer%s on SDL's unified queue",
				frame.ComputeDispatches,
				frame.ComputeDispatches == 1 ? "" : "es",
				frame.AsyncComputeCommandBuffers,
				frame.AsyncComputeCommandBuffers == 1 ? "" : "s");
			Row("downloads",
				"%u later-transfer buffer%s after the main stream",
				frame.DownloadCommandBuffers,
				frame.DownloadCommandBuffers == 1 ? "" : "s");
			Row("traffic plan",
				"%u command buffer%s, submitted serially",
				frame.TrafficCommandBuffers,
				frame.TrafficCommandBuffers == 1 ? "" : "s");

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

		// One frame's spans, names copied - see `DiagnosticSpan`.
		const auto snapshot = [&live](std::vector<DiagnosticSpan> &into) {
			into.clear();
			into.reserve(live.size());
			for (const FrameSpan &span : live) {
				into.push_back(
					DiagnosticSpan{
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

		// **The graph pauses itself on the frame the rule fired**, which is the
		// frame worth reading. What is taken here is that frame rather than
		// whatever an interval was accumulating: a mean over 250 ms with one bad
		// frame in it is exactly the picture the rule exists to replace.
		if (!view.Paused) {
			if (const engine::core::FrameTriggerHit *hit = FrameGraph::Triggered(); hit != nullptr) {
				snapshot(view.Spans);
				readScalars();
				view.PublishedFrames = 1;
				forget();
				view.Paused = true;
				view.PausedByRule = true;
				view.Fired = *hit;
			}
		}

		if (!view.Paused) {
			if (interval <= 0.0f) {
				// Every frame, which is what this panel always did.
				snapshot(view.Spans);
				readScalars();
				view.PublishedFrames = 1;
				forget();
			} else {
				if (view.Average) {
					// Structural matching keeps repeated world and phase trees
					// separate. Matching only name and depth collapses all of their
					// bars onto one time range.
					AccumulateDiagnosticSpans(live, view.Summed);

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
						FinishDiagnosticAverage(view.Spans, view.Frames);
						view.FrameMilliseconds = view.SummedFrameMilliseconds / frames;
						view.IdleMilliseconds = view.SummedIdleMilliseconds / frames;
						view.UnmarkedMilliseconds = view.SummedUnmarkedMilliseconds / frames;
						view.Dropped = view.SummedDropped / view.Frames;
						view.PublishedFrames = view.Frames;
					} else {
						snapshot(view.Spans);
						readScalars();
						view.PublishedFrames = 1;
					}

					view.NextPublish = now + static_cast<double>(interval);
					forget();
				}
			}
		}

		const std::vector<DiagnosticSpan> &spans = view.Spans;
		const float frameMs = view.FrameMilliseconds;
		const float idleMs = view.IdleMilliseconds;
		const float busyMs = std::max(frameMs - idleMs, 0.0f);

		std::vector<DiagnosticSpan> &graphSpans = view.DisplaySpans;
		graphSpans = spans;
		FitReportedDiagnosticTimeline(graphSpans, frameMs);
		AppendUnaccountedDiagnosticSpans(graphSpans);

		const char *millisecondsFormat = frameMs < 1.0f ? "%.3f" : "%.2f";
		ImGui::Text(frameMs < 1.0f ? "%.3f ms" : "%.2f ms", static_cast<double>(frameMs));
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		if (frameMs < 1.0f) {
			ImGui::Text(
				"  busy %.3f   idle %.3f   unmarked %.3f",
				static_cast<double>(busyMs),
				static_cast<double>(idleMs),
				static_cast<double>(view.UnmarkedMilliseconds)
			);
		} else {
			ImGui::Text(
				"  busy %.2f   idle %.2f   unmarked %.2f",
				static_cast<double>(busyMs),
				static_cast<double>(idleMs),
				static_cast<double>(view.UnmarkedMilliseconds)
			);
		}
		ImGui::PopStyleColor();

		// --- the controls ----------------------------------------------------

		if (ImGui::Button(view.Paused ? "Resume" : "Pause")) {
			view.Paused = !view.Paused;

			// Resuming disarms the latch, so the next matching frame stops the
			// graph again. Pausing by hand clears the label rather than leaving
			// a stale rule named beside a pause nobody's rule took.
			view.PausedByRule = false;
			FrameGraph::ClearTrigger();

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
			if (view.PausedByRule) {
				ImGui::Text(
					"- stopped: %s was %.2f ms",
					view.Fired.Subject.c_str(),
					static_cast<double>(view.Fired.Reading)
				);
			} else {
				ImGui::TextUnformatted("- paused");
			}
			ImGui::PopStyleColor();
		} else if (interval > 0.0f && view.Average) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::Text("- mean of %u frame(s)", view.PublishedFrames);
			ImGui::PopStyleColor();
		}

		// **The share is of busy, not of the frame.** With vsync on, fifteen of
		// a sixteen millisecond frame are a sleep - so a span measured against
		// the whole frame reads as one per cent whatever it costs, and the
		// panel becomes one on which nothing is ever worth optimising.
		// **Three causes, and this used to name the rarest one.** `Dropped`
		// counts buffer overflow, depth past `MAXIMUM_DEPTH`, and scopes opened
		// off the frame's owning thread - and with `MAXIMUM_SPANS` at 65536
		// against a frame of a few dozen, overflow is the one that essentially
		// never happens. What does happen every frame is the third: two worlds
		// ticking on workers open a span per phase and per system, and every one
		// of them is refused. Reading "overflowed the buffer" sent at least one
		// investigation at the span budget, which was 1% used.
		const size_t dropped = view.Dropped;
		if (dropped > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::Text("%zu spans dropped - off-thread, too deep, or past the buffer", dropped);
			ImGui::PopStyleColor();
		}

		// **The switch lives in Preferences > Compute now, and this says so.**
		// It used to sit here, on the argument that the dropped-span line above
		// is the symptom and the switch is the cure, so a menu between them is a
		// warning you have to know what to do about. That argument was right
		// about the *pairing* and wrong about the place: it is one of several
		// decisions about how this process uses its cores, the rest of which had
		// nowhere to be, and a checkbox that changes how the whole engine
		// dispatches is not a property of a panel. The pairing survives as this
		// line, which is the part that was actually load-bearing.
		//
		// Said whenever it is on rather than only when spans are dropped,
		// because the whole risk of the switch is reading a number taken with it
		// and treating it as the engine's real cost.
		// **Ahead of the serial-compute line, because it is the larger of the
		// two and neither excuses the other.** `just studio` builds the `dev`
		// preset, which compiles the engine at `-O0` and leaves the vendored
		// code at `-O2` - the right trade for a build somebody iterates on, and
		// the wrong one to read a profile from. Measured on one scene, one
		// display, one frame, `dev` against `release`: `convert instances`
		// 11.99 ms against 0.179, `graph.light-bounds` 10.18 against 0.120,
		// `sync rendered` 26.5 against 0.74.
		//
		// Nothing said so, so the panel looked like a normal frame with a
		// plausible distribution and every conclusion drawn from it was about
		// the compiler rather than the engine. `preset=release just studio` is
		// the fix, and this is the line that says to.
#if defined(__OPTIMIZE__) || (defined(_MSC_VER) && defined(NDEBUG))
		constexpr bool optimised = true;
#else
		constexpr bool optimised = false;
#endif
		if (!optimised) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextUnformatted("unoptimised build - every figure here is tens of times its shipped cost");
			ImGui::PopStyleColor();
		}

		if (engine::parallel::ForceSerialCompute()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
			ImGui::TextUnformatted("serial compute is on - these timings are not the shipped cost");
			ImGui::PopStyleColor();
		} else if (dropped > 0) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted("Preferences > Compute > force serial compute keeps every span");
			ImGui::PopStyleColor();
		}

		// --- the event scheduler ---------------------------------------------
		//
		// **Rules live here and run in `FrameGraph::EndFrame`.** This panel
		// samples four times a second at its shortest interval, and the frame a
		// rule is written for is one frame long, so a rule tested here would
		// miss almost every spike it was written to catch. What is here is the
		// writing of them.
		//
		// Folded away by default: most sessions never write one, and an empty
		// table above the flame graph would push the thing somebody opened the
		// panel for off the screen.
		if (ImGui::CollapsingHeader(view.Triggers.empty() ? "Event scheduler" : "Event scheduler (armed)")) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped(
				"A rule stops the graph on the frame that meets it, so the picture left on "
				"screen is the bad frame rather than an average that contains it."
			);
			ImGui::PopStyleColor();

			bool changed = false;
			size_t remove = view.Triggers.size();

			for (size_t index = 0; index < view.Triggers.size(); index++) {
				engine::core::FrameTrigger &rule = view.Triggers[index];
				ImGui::PushID(static_cast<int>(index));

				changed |= ImGui::Checkbox("##on", &rule.Enabled);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(
						"A rule that is off is kept, so it can be armed again without retyping."
					);
				}

				// **The names come from `core` rather than from a table here.**
				// The preferences file writes the same words, and two tables
				// that have to agree are one commit from disagreeing - a rule
				// that loads as a different subject than it was saved as.
				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(110.0f));
				const std::string subject(engine::core::GetTriggerSubjectName(rule.Subject));
				if (ImGui::BeginCombo("##subject", subject.c_str())) {
					for (size_t at = 0; at <= static_cast<size_t>(engine::core::TriggerSubject::Dropped);
						 at++) {
						const auto option = static_cast<engine::core::TriggerSubject>(at);
						const std::string name(engine::core::GetTriggerSubjectName(option));
						if (ImGui::Selectable(name.c_str(), option == rule.Subject)) {
							rule.Subject = option;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}

				// Only the subjects that name something get a name to edit. A
				// field that is ignored is one somebody fills in and then
				// wonders why the rule does not fire.
				if (rule.Subject == engine::core::TriggerSubject::Span ||
					rule.Subject == engine::core::TriggerSubject::SpanSelf) {
					ImGui::SameLine();
					ImGui::SetNextItemWidth(engine::ui::Scaled(160.0f));

					std::vector<char> buffer(128, '\0');
					std::snprintf(buffer.data(), buffer.size(), "%s", rule.Name.c_str());
					if (ImGui::InputTextWithHint("##name", "span name", buffer.data(), buffer.size())) {
						rule.Name = buffer.data();
						changed = true;
					}

					// **The names in the frame, offered rather than typed.** A
					// rule matches exactly, and `pump events` typed as `pump
					// event` is a rule that never fires and says nothing about
					// why. The list is what was published, which is what a
					// person is looking at when they write the rule.
					ImGui::SameLine();
					if (ImGui::BeginCombo("##pick", "", ImGuiComboFlags_NoPreview)) {
						for (const DiagnosticSpan &span : spans) {
							if (ImGui::Selectable(span.Name.c_str())) {
								rule.Name = span.Name;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				} else if (rule.Subject == engine::core::TriggerSubject::Category) {
					ImGui::SameLine();
					ImGui::SetNextItemWidth(engine::ui::Scaled(120.0f));
					const std::string current(engine::core::GetCategoryName(rule.Category));
					if (ImGui::BeginCombo("##category", current.c_str())) {
						for (size_t at = 0; at < static_cast<size_t>(ProfileCategory::Count); at++) {
							const auto category = static_cast<ProfileCategory>(at);
							const std::string name(engine::core::GetCategoryName(category));
							if (ImGui::Selectable(name.c_str(), category == rule.Category)) {
								rule.Category = category;
								changed = true;
							}
						}
						ImGui::EndCombo();
					}
				}

				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(70.0f));
				const std::string test(engine::core::GetTriggerTestName(rule.Test));
				if (ImGui::BeginCombo("##test", test.c_str())) {
					for (const engine::core::TriggerTest option :
						 {engine::core::TriggerTest::Above, engine::core::TriggerTest::Below}) {
						const std::string name(engine::core::GetTriggerTestName(option));
						if (ImGui::Selectable(name.c_str(), option == rule.Test)) {
							rule.Test = option;
							changed = true;
						}
					}
					ImGui::EndCombo();
				}

				ImGui::SameLine();
				ImGui::SetNextItemWidth(engine::ui::Scaled(90.0f));
				const bool counting = rule.Subject == engine::core::TriggerSubject::Dropped;
				if (ImGui::InputFloat(
						"##threshold", &rule.Threshold, 0.0f, 0.0f, counting ? "%.0f" : "%.2f ms"
					)) {
					changed = true;
				}

				ImGui::SameLine();
				if (ImGui::Button("x")) {
					remove = index;
				}

				ImGui::PopID();
			}

			if (remove < view.Triggers.size()) {
				view.Triggers.erase(view.Triggers.begin() + static_cast<ptrdiff_t>(remove));
				changed = true;
			}

			if (ImGui::Button("Add rule")) {
				// Seeded with the case this was built for rather than with an
				// empty row: a rule with no name matches no span, and a first
				// row that does nothing teaches nothing about what a rule is.
				view.Triggers.push_back(
					engine::core::FrameTrigger{
						.Name = "pump events",
						.Subject = engine::core::TriggerSubject::Span,
						.Test = engine::core::TriggerTest::Above,
						.Threshold = 2.0f,
					}
				);
				changed = true;
			}

			// Pushed only when something moved. The list is copied on the way
			// in, and copying it every repaint would be a per-frame allocation
			// to say nothing changed.
			if (changed) {
				FrameGraph::SetTriggers(view.Triggers);
			}
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

		const float rowHeight = engine::ui::Scaled(engine::ui::Size::Row) * 0.72f;
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float graphWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
		const float scale = frameMs > 0.0001f ? graphWidth / frameMs : 0.0f;
		const float onePixelMilliseconds = scale > 0.0f ? 1.0f / scale : 0.0f;
		const uint32_t graphRows = LayoutDiagnosticRows(graphSpans, frameMs, onePixelMilliseconds, view.Rows);
		const float graphHeight = rowHeight * static_cast<float>(graphRows);

		ImDrawList *draw = ImGui::GetWindowDrawList();
		const DiagnosticSpan *hovered = nullptr;

		for (size_t index = 0; index < graphSpans.size(); index++) {
			const DiagnosticSpan &span = graphSpans[index];
			// **Clamped to the graph, whatever the arithmetic above produced.**
			// An averaged span can still exceed the averaged frame - a span that
			// ran in only some of the frames divides by all of them for its
			// width and by its own count for its start - and a bar drawn past
			// the right edge lands on top of whatever is beside it. A flamegraph
			// that lies about a width is worse than one that clips.
			const float left =
				origin.x +
				std::clamp(span.StartMilliseconds * scale, 0.0f, std::max(graphWidth - 1.0f, 0.0f));
			const float room = std::max(origin.x + graphWidth - left, 1.0f);
			const float width = std::clamp(span.Milliseconds * scale, 1.0f, room);
			const float top = origin.y + static_cast<float>(view.Rows[index]) * rowHeight;

			const ImVec2 upper(left, top);
			const ImVec2 lower(left + width, top + rowHeight - 1.0f);

			const ImU32 colour =
				span.Name == "unaccounted" ? IM_COL32(94, 99, 112, 210) : ColourOf(span.Category);
			draw->AddRectFilled(upper, lower, colour, 2.0f);

			if (ImGui::IsMouseHoveringRect(upper, lower)) {
				hovered = index < spans.size() ? &spans[index] : &span;
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

			for (const DiagnosticSpan &span : spans) {
				ImGui::TableNextRow();

				ImGui::TableSetColumnIndex(0);
				ImGui::Indent(static_cast<float>(span.Depth) * engine::ui::Scaled(10.0f));
				ImGui::TextUnformatted(span.Name.data(), span.Name.data() + span.Name.size());
				ImGui::Unindent(static_cast<float>(span.Depth) * engine::ui::Scaled(10.0f));

				const float spanBusy = std::max(span.Milliseconds - span.IdleMilliseconds, 0.0f);
				const float share = busyMs > 0.0001f ? (spanBusy / busyMs) * 100.0f : 0.0f;

				ImGui::TableSetColumnIndex(1);
				ImGui::Text(millisecondsFormat, static_cast<double>(spanBusy));

				ImGui::TableSetColumnIndex(2);
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				if (span.IdleMilliseconds > 0.0001f) {
					ImGui::Text(millisecondsFormat, static_cast<double>(span.IdleMilliseconds));
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

	// --- the heap -------------------------------------------------------------

	namespace {

		// Renders a byte count to three significant figures, signed, so a rate
		// that is giving memory back reads as one.
		std::string FormatBytes(double bytes) {
			const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
			size_t unit = 0;
			while ((bytes >= 1024.0 || bytes <= -1024.0) && unit + 1 < std::size(units)) {
				bytes /= 1024.0;
				unit++;
			}

			char text[64];
			std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.2f %s", bytes, units[unit]);
			return text;
		}
	}

	void Editor::DrawHeap() {
		if (!ShowHeap) {
			return;
		}

		if (!ImGui::Begin("Heap", &ShowHeap)) {
			ImGui::End();
			return;
		}

		if (!engine::core::HeapProfile::IsCompiledIn()) {
			// Not an empty tree. Every figure would read zero, and a heap panel
			// reporting no bytes is a much more alarming thing than one saying
			// it was left out of this build.
			ImGui::TextUnformatted("The allocator hooks are not compiled into this build.");
			ImGui::TextUnformatted(
				"Configure with MONO_HEAP_PROFILE=ON, the dev preset, or the -dev archive of this release."
			);
			ImGui::End();
			return;
		}

		// **Refreshed on the sampler's clock, not on the panel's.** `Editor::Run`
		// calls `SampleIfDue` once a frame and it records once a second; this
		// notices the count changing rather than keeping a second timer, so the
		// panel and the readings cannot drift apart.
		HeapView &view = HeapState;
		const std::vector<engine::core::HeapSample> history = engine::core::HeapProfile::History();
		if (history.size() != view.Plot.size()) {
			constexpr int64_t TREE_FLOOR = 16 * 1024;
			constexpr int64_t GROWTH_FLOOR = 256 * 1024;

			view.Totals = engine::core::HeapProfile::Totals();
			view.Rows = engine::core::HeapProfile::TreeRows(TREE_FLOOR);
			view.Growth = engine::core::HeapProfile::Growth(0.0, GROWTH_FLOOR);
			view.HistorySeconds = engine::core::HeapProfile::HistorySeconds();

			view.Plot.clear();
			view.Plot.reserve(history.size());
			for (const engine::core::HeapSample &sample : history) {
				view.Plot.push_back(static_cast<float>(sample.LiveBytes) / (1024.0f * 1024.0f));
			}
		}

		const engine::core::HeapTotals &totals = view.Totals;
		ImGui::Text(
			"%s live in %" PRId64 " blocks   peak %s   headers %s",
			FormatBytes(static_cast<double>(totals.LiveBytes)).c_str(),
			totals.LiveBlocks,
			FormatBytes(static_cast<double>(totals.PeakBytes)).c_str(),
			FormatBytes(static_cast<double>(totals.OverheadBytes)).c_str()
		);
		ImGui::Text(
			"GPU logical %s   peak %s   buffers %s   transfers %s   textures %s",
			FormatBytes(static_cast<double>(view.Gpu.LiveBytes)).c_str(),
			FormatBytes(static_cast<double>(view.Gpu.PeakBytes)).c_str(),
			FormatBytes(static_cast<double>(view.Gpu.BufferBytes)).c_str(),
			FormatBytes(static_cast<double>(view.Gpu.TransferBufferBytes)).c_str(),
			FormatBytes(static_cast<double>(view.Gpu.TextureBytes)).c_str()
		);
		ImGui::Text(
			"GPU churn %s allocated   %s released",
			FormatBytes(static_cast<double>(view.Gpu.AllocatedBytes)).c_str(),
			FormatBytes(static_cast<double>(view.Gpu.ReleasedBytes)).c_str()
		);
		ImGui::Text(
			"GPU creates %" PRIu64 " buffers   %" PRIu64 " transfers   %" PRIu64 " textures",
			view.Gpu.BufferAllocations,
			view.Gpu.TransferBufferAllocations,
			view.Gpu.TextureAllocations
		);

		if (totals.DroppedScopes > 0 || totals.ForeignFrees > 0) {
			// A partial tree must not look complete, which is the same position
			// the frame graph takes about dropped spans.
			ImGui::TextColored(
				ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
				"%" PRIu64 " scope(s) dropped, %" PRIu64 " foreign free(s) - the tree is not the whole "
				"picture",
				totals.DroppedScopes,
				totals.ForeignFrees
			);
		}

		// **The graph, and it is the reason this panel is not a table.** A leak
		// is a slope, and a slope is a shape rather than a number: a subsystem
		// that saw-tooths between four and six megabytes and one that climbs
		// steadily through five read identically on any row of figures.
		if (view.Plot.size() >= 2) {
			ImGui::PlotLines(
				"##heaplive",
				view.Plot.data(),
				static_cast<int>(view.Plot.size()),
				0,
				nullptr,
				0.0f,
				FLT_MAX,
				ImVec2(-1.0f, ImGui::GetTextLineHeight() * 6.0f)
			);
			ImGui::Text("live MiB over the last %.0f s, sampled once a second", view.HistorySeconds);
			if (view.GpuPlot.size() >= 2) {
				ImGui::PlotLines(
					"##gpuheaplive",
					view.GpuPlot.data(),
					static_cast<int>(view.GpuPlot.size()),
					0,
					nullptr,
					0.0f,
					FLT_MAX,
					ImVec2(-1.0f, ImGui::GetTextLineHeight() * 4.0f)
				);
				ImGui::TextDisabled("GPU logical live MiB on the same samples");
			}
		} else {
			ImGui::TextDisabled("collecting - the plot needs a second reading");
		}

		ImGui::Separator();

		if (view.Rows.empty()) {
			ImGui::TextDisabled("no tagged allocations - every byte is untagged");
			ImGui::End();
			return;
		}

		// The rate for a node, looked up by index. The growth report is short -
		// the tracked nodes that cleared a byte floor - so a scan beats building
		// a map per repaint.
		const auto rateOf = [&view](uint32_t node) {
			for (const engine::core::HeapGrowth &entry : view.Growth) {
				if (entry.Node == node) {
					return entry.BytesPerSecond;
				}
			}
			return 0.0;
		};

		constexpr ImGuiTableFlags TABLE = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
										  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("heaptags", 5, TABLE)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch, 3.0f);
			ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Blocks", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("Growth", ImGuiTableColumnFlags_WidthStretch, 1.2f);
			ImGui::TableHeadersRow();

			for (const engine::core::HeapTreeRow &row : view.Rows) {
				ImGui::TableNextRow();

				ImGui::TableNextColumn();
				// Two spaces a level. Past a handful of levels a row is more
				// indent than name, and the path is in the tooltip anyway.
				const uint32_t level = row.Depth > 0 ? row.Depth - 1 : 0;
				const std::string indent(std::min<uint32_t>(level, 6) * 2, ' ');
				ImGui::Text("%s%.*s", indent.c_str(), static_cast<int>(row.Name.size()), row.Name.data());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FormatBytes(static_cast<double>(row.InclusiveBytes)).c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FormatBytes(static_cast<double>(row.SelfBytes)).c_str());

				ImGui::TableNextColumn();
				ImGui::Text("%" PRId64, row.LiveBlocks);

				ImGui::TableNextColumn();
				const double rate = rateOf(row.Node);
				if (std::abs(rate) < 64.0) {
					// A dash rather than a zero, so the column reads as a list
					// of suspects instead of a wall of zeroes with the answer
					// buried in it.
					ImGui::TextDisabled("-");
				} else {
					const bool bad = rate > 64.0 * 1024.0;
					const bool warn = rate > 4.0 * 1024.0;
					const ImVec4 colour = bad	 ? ImVec4(0.93f, 0.38f, 0.38f, 1.0f)
										  : warn ? ImVec4(0.95f, 0.70f, 0.30f, 1.0f)
												 : ImVec4(0.86f, 0.89f, 0.93f, 1.0f);
					ImGui::TextColored(colour, "%s/s", FormatBytes(rate).c_str());
				}
			}
			ImGui::EndTable();
		}

		ImGui::End();
	}
}
