// What crosses between worlds, which is the only thing they share.
//
// **This panel exists because of rule 3, not in spite of it.** "Nothing crossing
// a world boundary is a pointer" means every crossing is already a copy with a
// bus, a key, a sender and a payload - sitting in two ordinary resources on the
// world's own store. So this reads what is there. There is no instrumentation
// here, no second recording path, and nothing that would have to be switched on.
//
// It is the one subsystem where "it works" and "I can see it working" were
// furthest apart: a universe of worlds that communicate only by message was the
// engine's central architectural bet, and until now the editor showed a world
// count and nothing about the traffic between them.
//
// **Read every frame, cached nowhere.** `Inbox` is replaced wholesale each
// barrier - `Postbox.hpp` says so - so a cached copy would be showing a barrier
// that has already been and gone. That is `AGENTS.md`'s rule for every panel
// here and it bites harder than usual on this one.

#include <engine/ui/Theme.hpp>
#include <engine/world/Bus.hpp>
#include <engine/world/Postbox.hpp>

#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::ecs::Store;
	using engine::world::WorldId;

	namespace {
		using engine::world::BusBudget;
		using engine::world::Delivery;
		using engine::world::Envelope;
		using engine::world::Inbox;
		using engine::world::Outbox;

		// A payload's size, in the units somebody reads at a glance.
		//
		// Bytes up to a kilobyte, because a bus message that is genuinely 900
		// bytes is a different thing from one that is 0.9 KB and the whole
		// reason to look is to notice which.
		void WriteSize(char *into, size_t capacity, size_t bytes) {
			if (bytes < 1024) {
				std::snprintf(into, capacity, "%zu B", bytes);
			} else {
				std::snprintf(into, capacity, "%.1f KB", static_cast<double>(bytes) / 1024.0);
			}
		}

		// The budget bar. **Coloured at three quarters rather than at full**,
		// because a world that has spent its allowance is already dropping
		// requests and the useful moment to notice is before that.
		void DrawBudget(const BusBudget &budget) {
			const float fraction =
				budget.PerTick == 0 ? 0.0f
									: static_cast<float>(budget.Spent) / static_cast<float>(budget.PerTick);

			char label[64];
			std::snprintf(label, sizeof(label), "%u / %u this tick", budget.Spent, budget.PerTick);

			if (fraction >= 0.75f) {
				ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.55f, 0.2f, 1.0f));
			}
			ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), label);
			if (fraction >= 0.75f) {
				ImGui::PopStyleColor();
			}
		}
	}

	void Editor::DrawBus() {
		if (!ShowBus) {
			return;
		}

		if (!ImGui::Begin("Bus", &ShowBus)) {
			ImGui::End();
			return;
		}

		if (Universe == nullptr || Universe->Worlds().empty()) {
			ImGui::TextDisabled("no worlds");
			ImGui::End();
			return;
		}

		// **Totals first.** The question that brings somebody to this panel is
		// almost always "is anything crossing at all", and answering it should
		// not require reading five per-world tables.
		size_t totalPending = 0;
		size_t totalArrived = 0;
		for (const WorldId world : Universe->Worlds()) {
			Universe->Enter(world, [&](Store &store) {
				if (const auto *outbox = store.Resource<Outbox>()) {
					totalPending += outbox->Pending.size();
				}
				if (const auto *inbox = store.Resource<Inbox>()) {
					totalArrived += inbox->Arrived.size();
				}
			});
		}

		ImGui::Text("%zu queued", totalPending);
		ImGui::SameLine();
		ImGui::TextDisabled("· %zu arrived at the last barrier", totalArrived);
		ImGui::Separator();

		for (const WorldId world : Universe->Worlds()) {
			const std::string name(Label(Universe->NameOf(world)));

			// Snapshotted inside `Enter` and drawn outside it. Drawing from
			// within would hold a scoped store reference across imgui calls that
			// can open a popup and run other code - and `Enter` aborts on
			// re-entry rather than allowing it.
			std::vector<Envelope> pending;
			std::vector<Delivery> arrived;
			BusBudget budget;
			bool replica = false;
			bool haveBudget = false;
			uint64_t nextTicket = 0;

			Universe->Enter(world, [&](Store &store) {
				if (const auto *outbox = store.Resource<Outbox>()) {
					pending = outbox->Pending;
					nextTicket = outbox->NextTicket;
				}
				if (const auto *inbox = store.Resource<Inbox>()) {
					arrived = inbox->Arrived;
				}
				if (const auto *limit = store.Resource<BusBudget>()) {
					budget = *limit;
					haveBudget = true;
				}
				replica = store.HasResource<engine::world::Replica>();
			});

			ImGui::PushID(static_cast<int>(world.Index));

			char header[192];
			std::snprintf(
				header,
				sizeof(header),
				"%s - %zu queued, %zu arrived###world",
				name.c_str(),
				pending.size(),
				arrived.size()
			);

			if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
				if (replica) {
					// Worth saying out loud: a replica may not write to a bus at
					// all, so an outbox with anything in it here is a bug rather
					// than traffic.
					ImGui::TextDisabled("replica - DataStore, MemoryStore and Teleport are refused here");
				}

				if (haveBudget) {
					DrawBudget(budget);
				}

				ImGui::TextDisabled("next ticket %llu", static_cast<unsigned long long>(nextTicket));

				// --- what this world has asked for ---------------------------
				if (pending.empty()) {
					ImGui::TextDisabled("outbox empty");
				} else if (ImGui::BeginTable(
							   "##outbox", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
						   )) {
					ImGui::TableSetupColumn("bus");
					ImGui::TableSetupColumn("operation");
					ImGui::TableSetupColumn("key");
					ImGui::TableSetupColumn("seq");
					ImGui::TableSetupColumn("payload");
					ImGui::TableHeadersRow();

					for (const Envelope &envelope : pending) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(engine::world::Describe(envelope.Bus));
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(engine::world::Describe(envelope.Operation));
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(Label(envelope.Key));
						ImGui::TableNextColumn();
						ImGui::Text("%llu", static_cast<unsigned long long>(envelope.Sequence));
						ImGui::TableNextColumn();

						char size[32];
						WriteSize(size, sizeof(size), envelope.Payload.size());
						ImGui::TextUnformatted(size);
					}
					ImGui::EndTable();
				}

				// --- what reached it at the last barrier ---------------------
				if (arrived.empty()) {
					ImGui::TextDisabled("inbox empty");
				} else if (ImGui::BeginTable(
							   "##inbox", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
						   )) {
					ImGui::TableSetupColumn("bus");
					ImGui::TableSetupColumn("key");
					ImGui::TableSetupColumn("from");
					ImGui::TableSetupColumn("status");
					ImGui::TableSetupColumn("payload");
					ImGui::TableHeadersRow();

					for (const Delivery &delivery : arrived) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(engine::world::Describe(delivery.Bus));
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(Label(delivery.Key));
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(Label(delivery.From));
						ImGui::TableNextColumn();

						// **A failed delivery is the row worth finding**, and a
						// table of forty greys is where one goes missing.
						const bool ok = delivery.Status == engine::world::BusStatus::Ok;
						if (ok) {
							ImGui::TextDisabled("%s", engine::world::Describe(delivery.Status));
						} else {
							ImGui::TextColored(
								ImVec4(0.9f, 0.4f, 0.35f, 1.0f),
								"%s",
								engine::world::Describe(delivery.Status)
							);
						}

						ImGui::TableNextColumn();
						char size[32];
						WriteSize(size, sizeof(size), delivery.Payload.size());
						ImGui::TextUnformatted(size);
					}
					ImGui::EndTable();
				}
			}

			ImGui::PopID();
		}

		ImGui::End();
	}
}
