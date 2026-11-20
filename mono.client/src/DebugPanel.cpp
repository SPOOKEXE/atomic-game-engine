#include <engine/core/Log.hpp>
#include <engine/ui/Interface.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cctype>
#include <client/DebugPanel.hpp>
#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace client {
	namespace {
		bool Contains(const std::string &text, const char *filter) {
			if (filter == nullptr || filter[0] == '\0') return true;
			const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
			std::string a = text;
			std::string b = filter;
			std::transform(a.begin(), a.end(), a.begin(), lower);
			std::transform(b.begin(), b.end(), b.begin(), lower);
			return a.find(b) != std::string::npos;
		}

		void Row(const char *label, const std::string &value) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
		}

		std::string Number(uint64_t value) {
			return std::to_string(value);
		}

		std::string Rate(double bytesPerSecond) {
			return FormatBytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
		}
	}

	std::string FormatBytes(uint64_t bytes) {
		static constexpr const char *UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB"};
		double value = static_cast<double>(bytes);
		size_t unit = 0;
		while (value >= 1024.0 && unit + 1 < std::size(UNITS)) {
			value /= 1024.0;
			++unit;
		}
		char text[32];
		if (unit == 0)
			std::snprintf(text, sizeof(text), "%llu B", static_cast<unsigned long long>(bytes));
		else
			std::snprintf(text, sizeof(text), "%.1f %s", value, UNITS[unit]);
		return text;
	}

	void DrawTrafficBreakdown(
		const char *id,
		const engine::replication::TrafficBreakdown &inbound,
		const engine::replication::TrafficBreakdown &outbound,
		const char *inLabel,
		const char *outLabel
	) {
		const ImGuiTableFlags flags =
			ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV;
		if (!ImGui::BeginTable(id, 5, flags)) return;
		const std::string inMessages = std::string(inLabel) + " msgs";
		const std::string inBytes = std::string(inLabel) + " bytes";
		const std::string outMessages = std::string(outLabel) + " msgs";
		const std::string outBytes = std::string(outLabel) + " bytes";
		ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn(inMessages.c_str());
		ImGui::TableSetupColumn(inBytes.c_str());
		ImGui::TableSetupColumn(outMessages.c_str());
		ImGui::TableSetupColumn(outBytes.c_str());
		ImGui::TableHeadersRow();
		const auto row = [](const char *name, uint64_t im, uint64_t ib, uint64_t om, uint64_t ob) {
			if (im == 0 && om == 0) return;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(name);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(im));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(FormatBytes(ib).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(om));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(FormatBytes(ob).c_str());
		};
		uint64_t inCount = inbound.UnknownMessages;
		uint64_t outCount = outbound.UnknownMessages;
		for (size_t kind = 0; kind < engine::replication::MESSAGE_KINDS; ++kind) {
			row(engine::replication::Describe(static_cast<engine::replication::MessageKind>(kind)),
				inbound.Messages[kind],
				inbound.Bytes[kind],
				outbound.Messages[kind],
				outbound.Bytes[kind]);
			inCount += inbound.Messages[kind];
			outCount += outbound.Messages[kind];
		}
		row("other",
			inbound.UnknownMessages,
			inbound.UnknownBytes,
			outbound.UnknownMessages,
			outbound.UnknownBytes);
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("total");
		ImGui::TableNextColumn();
		ImGui::Text("%llu", static_cast<unsigned long long>(inCount));
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(FormatBytes(inbound.TotalBytes()).c_str());
		ImGui::TableNextColumn();
		ImGui::Text("%llu", static_cast<unsigned long long>(outCount));
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(FormatBytes(outbound.TotalBytes()).c_str());
		ImGui::EndTable();
	}

	DebugPanel::DebugPanel() = default;

	DebugPanel::~DebugPanel() {
		Shutdown();
	}

	bool DebugPanel::Toggle(engine::render::Renderer &renderer, SDL_Window *window) {
		if (Open) {
			Open = false;
			return true;
		}
		if (Ui == nullptr) {
			if (window == nullptr) {
				ENGINE_INFO("F9: no debug panel without a window");
				return false;
			}
			auto made = std::make_unique<engine::ui::Interface>();
			engine::ui::InterfaceSettings settings;
			settings.Docking = false;
			settings.Scale =
				SDL_GetWindowDisplayScale(window) > 0.0f ? SDL_GetWindowDisplayScale(window) : 1.0f;
			if (!made->Initialise(renderer, window, settings)) {
				ENGINE_WARN("F9: the debug panel could not start its interface");
				return false;
			}
			Ui = std::move(made);
		}
		Open = true;
		return true;
	}

	bool DebugPanel::ProcessEvent(const SDL_Event &event) {
		if (!Open || Ui == nullptr) return false;
		Ui->ProcessEvent(event);
		const bool mouse = event.type == SDL_EVENT_MOUSE_MOTION ||
						   event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
						   event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL;
		const bool keyboard = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP ||
							  event.type == SDL_EVENT_TEXT_INPUT;
		return (mouse && Ui->WantsMouse()) || (keyboard && Ui->WantsKeyboard());
	}

	engine::render::FrameOverlayHook *DebugPanel::Hook() {
		return Open && Ui != nullptr && Ui->IsDrawable() ? Ui.get() : nullptr;
	}

	void DebugPanel::Shutdown() {
		if (Ui != nullptr) Ui->Shutdown();
		Ui.reset();
		Open = false;
	}

	void DebugPanel::Draw(const DebugPanelSample &sample) {
		if (!Open || Ui == nullptr) return;
		const uint64_t now = SDL_GetTicksNS();
		const float frameSeconds =
			LastDrawNanoseconds == 0 ? 1.0f / 60.0f : static_cast<float>(now - LastDrawNanoseconds) * 1e-9f;
		LastDrawNanoseconds = now;
		if (!Paused) Held = sample;
		const DebugPanelSample &shown = Held;

		Ui->Begin(frameSeconds);
		ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(520, 440), ImGuiCond_FirstUseEver);
		bool keep = true;
		if (ImGui::Begin("Debug (F9)", &keep)) {
			ImGui::Checkbox("Pause", &Paused);
			ImGui::SameLine();
			ImGui::Text(
				"frame %.2f ms  unmarked %.2f ms", shown.FrameMilliseconds, shown.UnmarkedMilliseconds
			);
			if (ImGui::BeginTabBar("debug tabs")) {
				if (ImGui::BeginTabItem("Frame Graph")) {
					ImGui::SetNextItemWidth(160);
					ImGui::InputTextWithHint("##filter", "filter", SpanFilter.data(), SpanFilter.size());
					ImGui::SameLine();
					ImGui::SetNextItemWidth(110);
					ImGui::SliderFloat("min ms", &MinimumMilliseconds, 0.0f, 2.0f, "%.2f");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(90);
					ImGui::SliderInt("depth", &DepthLimit, 1, 16);
					if (shown.Spans.empty()) ImGui::TextUnformatted("no spans recorded yet");
					const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
												  ImGuiTableFlags_SizingStretchProp |
												  ImGuiTableFlags_Resizable;
					if (ImGui::BeginTable("spans", 3, flags)) {
						ImGui::TableSetupScrollFreeze(0, 1);
						ImGui::TableSetupColumn("span", ImGuiTableColumnFlags_WidthStretch, 2.5f);
						ImGui::TableSetupColumn("ms");
						ImGui::TableSetupColumn("self ms");
						ImGui::TableHeadersRow();
						for (const DebugSpan &span : shown.Spans) {
							if (static_cast<int>(span.Depth) >= DepthLimit) continue;
							if (span.Milliseconds < MinimumMilliseconds) continue;
							if (!Contains(span.Name, SpanFilter.data())) continue;
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::Indent(10.0f * static_cast<float>(span.Depth) + 0.001f);
							ImGui::TextUnformatted(span.Name.c_str());
							ImGui::Unindent(10.0f * static_cast<float>(span.Depth) + 0.001f);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f", span.Milliseconds);
							ImGui::TableNextColumn();
							ImGui::Text("%.3f", span.SelfMilliseconds);
						}
						ImGui::EndTable();
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Network Diagnostics")) {
					if (!shown.Connected) {
						ImGui::TextUnformatted("not connected (run with --connect)");
					} else {
						const auto &link = shown.Link;
						if (ImGui::CollapsingHeader("Link", ImGuiTreeNodeFlags_DefaultOpen) &&
							ImGui::BeginTable("link", 2, ImGuiTableFlags_SizingStretchProp)) {
							Row("joined", link.Joined ? "yes" : "no");
							Row("down", Rate(link.ReceivedBytesPerSecond));
							Row("up", Rate(link.SentBytesPerSecond));
							Row("received", FormatBytes(link.ReceivedBytes));
							Row("sent", FormatBytes(link.SentBytes));
							char rtt[32];
							std::snprintf(rtt, sizeof(rtt), "%.1f ms", link.RoundTripMilliseconds);
							Row("round trip", rtt);
							Row("packets lost", Number(link.PacketsLost));
							Row("packets stale", Number(link.PacketsStale));
							Row("sends over budget", Number(link.SendsOverBudget));
							ImGui::EndTable();
						}
						if (ImGui::CollapsingHeader("Traffic by message", ImGuiTreeNodeFlags_DefaultOpen))
							DrawTrafficBreakdown("traffic", shown.Received, shown.Sent, "in", "out");
						if (ImGui::CollapsingHeader("Replica") &&
							ImGui::BeginTable("replica", 2, ImGuiTableFlags_SizingStretchProp)) {
							Row("applied tick", Number(link.AppliedTick));
							char rate[32];
							std::snprintf(rate, sizeof(rate), "%.1f Hz", link.TickRate);
							Row("tick rate", rate);
							std::snprintf(rate, sizeof(rate), "%.2f", link.BehindTicks);
							Row("ticks behind", rate);
							Row("snapshots", Number(link.Snapshots));
							Row("deltas", Number(link.Deltas));
							Row("structures", Number(link.Structures));
							Row("malformed", Number(link.Malformed));
							Row("stale", Number(link.Stale));
							Row("stalls", Number(link.Stalls));
							Row("interpolated", Number(link.Interpolated));
							Row("held", Number(link.Held));
							Row("entities", Number(link.Entities));
							Row("drawn", Number(link.Drawn));
							ImGui::EndTable();
						}
					}
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Memory")) {
					size_t entities = 0;
					size_t components = 0;
					size_t storage = 0;
					for (const auto &world : shown.Worlds) {
						entities += world.Entities;
						components += world.Components.size();
						storage += world.ResidentBytes;
					}
					if (ImGui::BeginTable("memory", 2, ImGuiTableFlags_SizingStretchProp)) {
						Row("entities", Number(entities));
						Row("component types", Number(components));
						Row("ecs storage", FormatBytes(storage));
						Row("cpu heap live",
							shown.HeapCompiledIn
								? FormatBytes(
									  static_cast<uint64_t>(std::max<int64_t>(shown.Heap.LiveBytes, 0))
								  )
								: std::string("not compiled in"));
						if (shown.HeapCompiledIn)
							Row(
								"cpu heap peak",
								FormatBytes(static_cast<uint64_t>(std::max<int64_t>(shown.Heap.PeakBytes, 0)))
							);
						Row("gpu live", FormatBytes(shown.Gpu.LiveBytes));
						Row("gpu peak", FormatBytes(shown.Gpu.PeakBytes));
						Row("gpu textures",
							Number(shown.Gpu.Textures) + " / " + FormatBytes(shown.Gpu.TextureBytes));
						Row("gpu buffers",
							Number(shown.Gpu.Buffers) + " / " + FormatBytes(shown.Gpu.BufferBytes));
						ImGui::EndTable();
					}
					ImGui::SetNextItemWidth(160);
					ImGui::InputTextWithHint(
						"##components", "component filter", ComponentFilter.data(), ComponentFilter.size()
					);
					for (const auto &world : shown.Worlds) {
						const std::string title = world.Name + "  " + Number(world.Entities) + " entities, " +
												  FormatBytes(world.ResidentBytes) + "###" + world.Name;
						if (!ImGui::CollapsingHeader(title.c_str())) continue;
						ImGui::Text("%zu tables", world.Tables);
						const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable |
													  ImGuiTableFlags_SizingStretchProp |
													  ImGuiTableFlags_ScrollY;
						const float height =
							std::min(240.0f, 24.0f + 20.0f * static_cast<float>(world.Components.size()));
						if (!ImGui::BeginTable(
								("components" + world.Name).c_str(), 3, flags, ImVec2(0, height)
							))
							continue;
						ImGui::TableSetupScrollFreeze(0, 1);
						ImGui::TableSetupColumn("component", ImGuiTableColumnFlags_WidthStretch, 2.0f);
						ImGui::TableSetupColumn("rows");
						ImGui::TableSetupColumn(
							"bytes",
							ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending
						);
						ImGui::TableHeadersRow();
						std::vector<const DebugWorldMemory::Component *> order;
						for (const auto &component : world.Components)
							if (Contains(component.Name, ComponentFilter.data())) order.push_back(&component);
						const ImGuiTableSortSpecs *sort = ImGui::TableGetSortSpecs();
						const int column =
							sort != nullptr && sort->SpecsCount > 0 ? sort->Specs[0].ColumnIndex : 2;
						const bool ascending = sort != nullptr && sort->SpecsCount > 0 &&
											   sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
						std::sort(order.begin(), order.end(), [&](const auto *a, const auto *b) {
							bool less = column == 0	  ? a->Name < b->Name
										: column == 1 ? a->Rows < b->Rows
													  : a->Bytes < b->Bytes;
							bool greater = column == 0	 ? b->Name < a->Name
										   : column == 1 ? b->Rows < a->Rows
														 : b->Bytes < a->Bytes;
							return ascending ? less : greater;
						});
						for (const auto *component : order) {
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(component->Name.c_str());
							ImGui::TableNextColumn();
							ImGui::Text("%zu", component->Rows);
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(FormatBytes(component->Bytes).c_str());
						}
						ImGui::EndTable();
					}
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::End();
		Ui->End();
		if (!keep) Open = false;
	}
}
