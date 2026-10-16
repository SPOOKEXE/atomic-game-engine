#include <engine/ecs/Store.hpp>
#include <engine/procedural/Canonical.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <limits>
#include <studio/ProceduralEditor.hpp>
#include <studio/ProceduralPanel.hpp>
#include <utility>

namespace studio {
	namespace procedural_panel_detail {
		constexpr int64_t CELL = engine::procedural::SUBCELLS_PER_CELL;
		bool Cells(const std::array<int64_t, 2> &cells, engine::procedural::Point &point) {
			constexpr int64_t maximum = (int64_t{1} << 48) / CELL;
			if (cells[0] < -maximum || cells[0] > maximum || cells[1] < -maximum || cells[1] > maximum)
				return false;
			point = {cells[0] * CELL, cells[1] * CELL};
			return true;
		}
		const char *Status(engine::script::ProceduralJobStatus status) {
			using enum engine::script::ProceduralJobStatus;
			switch (status) {
			case Pending:
				return "Generating";
			case Ready:
				return "Ready to activate";
			case Canceled:
				return "Canceled";
			case Failed:
				return "Failed";
			case Published:
				return "Accepted";
			}
			return "Unavailable";
		}
	}
	bool ProceduralPanelState::Queue(const ProceduralAction &action) {
		if (Pending.size() >= 64 || action.Output.size() > 256 || action.Node.size() > 128 ||
			action.Parameter.size() > 128 || action.Source.Name.size() > 128 ||
			action.Source.TargetVillage.size() > 128 || action.Spline.Name.size() > 128 ||
			action.Spline.EntranceName.size() > 128 || action.Override.TargetId.size() > 128 ||
			action.Override.SourceName.size() > 128) {
			Status = "Pending action limit reached";
			return false;
		}
		Pending.push_back(action);
		return true;
	}
	bool IsProceduralAuthoringAction(ProceduralActionKind kind) {
		return kind <= ProceduralActionKind::RemoveOverride;
	}
	bool ApplyProceduralAuthoringAction(
		engine::ecs::Store &store, const ProceduralAction &action, engine::procedural::Diagnostic &diagnostic
	) {
		using namespace engine;
		switch (action.Kind) {
		case ProceduralActionKind::Initialize: {
			const std::array<procedural::RegionScope, 2> scopes{{{0, 0, 16}, {20, 0, 16}}};
			const std::array<script::ProceduralSource, 2> sources{
				{{procedural::SourceKind::Village,
				  "oakstead",
				  {8 * procedural_panel_detail::CELL, 8 * procedural_panel_detail::CELL},
				  ""},
				 {procedural::SourceKind::Entrance,
				  "west-gate",
				  {12 * procedural_panel_detail::CELL, 8 * procedural_panel_detail::CELL},
				  "oakstead"}}
			};
			return script::ConfigureProceduralWorld(
				store,
				"forest-village",
				procedural::ForestVillageDefaults("forest-village", action.Seed),
				scopes,
				sources,
				diagnostic
			);
		}
		case ProceduralActionKind::Seed: {
			const auto *authority = store.Resource<script::ProceduralAuthority>();
			procedural::Recipe recipe;
			if (!authority || procedural::DecodeRecipe(authority->RecipeBytes, recipe, diagnostic) !=
								  procedural::Status::Ok)
				return false;
			recipe.RootSeed = action.Seed;
			return script::SetProceduralRecipe(store, recipe, diagnostic);
		}
		case ProceduralActionKind::Parameter: {
			const auto *authority = store.Resource<script::ProceduralAuthority>();
			procedural::Recipe recipe;
			if (!authority || procedural::DecodeRecipe(authority->RecipeBytes, recipe, diagnostic) !=
								  procedural::Status::Ok)
				return false;
			for (auto &node : recipe.Nodes)
				if (node.Id == action.Node)
					for (auto &parameter : node.Parameters)
						if (parameter.Name == action.Parameter &&
							std::holds_alternative<uint64_t>(parameter.Value)) {
							parameter.Value = action.ParameterValue;
							return script::SetProceduralRecipe(store, recipe, diagnostic);
						}
			diagnostic = {procedural::Status::InvalidInput, "editor.parameter"};
			return false;
		}
		case ProceduralActionKind::SetSource:
			return script::SetProceduralSource(store, action.Source, diagnostic);
		case ProceduralActionKind::RemoveSource:
			return script::RemoveProceduralSource(store, action.Source.Kind, action.Source.Name, diagnostic);
		case ProceduralActionKind::SetSpline:
			return script::SetProceduralSpline(store, action.Spline, diagnostic);
		case ProceduralActionKind::RemoveSpline:
			return script::RemoveProceduralSpline(store, action.Spline.Name, diagnostic);
		case ProceduralActionKind::SetOverride:
			return script::SetProceduralOverride(store, action.Override, diagnostic);
		case ProceduralActionKind::RemoveOverride:
			return script::RemoveProceduralOverride(store, action.Override.TargetId, diagnostic);
		default:
			diagnostic = {procedural::Status::InvalidInput, "editor.action"};
			return false;
		}
	}
	void DrawProceduralPanel(
		ProceduralPanelState &state,
		bool &open,
		const engine::ecs::Store *source,
		const ProceduralEditor *preview,
		void *retainedImage
	) {
		using namespace engine;
		if (!open) return;
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 1));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
		if (!ImGui::Begin("Procedural Generation", &open)) {
			ImGui::End();
			ImGui::PopStyleColor(2);
			return;
		}
		const auto *authority = source ? source->Resource<script::ProceduralAuthority>() : nullptr;
		ImGui::BeginDisabled(!source || source->AdoptOnly());
		if (!authority) {
			if (ImGui::Button("Create forest and village")) state.Queue({ProceduralActionKind::Initialize});
		} else {
			procedural::Recipe recipe;
			procedural::Diagnostic diagnostic;
			if (procedural::DecodeRecipe(authority->RecipeBytes, recipe, diagnostic) ==
				procedural::Status::Ok) {
				uint64_t seed = recipe.RootSeed;
				if (ImGui::InputScalar("Seed", ImGuiDataType_U64, &seed)) {
					ProceduralAction action{ProceduralActionKind::Seed};
					action.Seed = seed;
					state.Queue(action);
				}
			}
			if (ImGui::CollapsingHeader("Recipe parameters")) {
				for (const auto &node : recipe.Nodes) {
					ImGui::PushID(node.Id.c_str());
					ImGui::TextUnformatted(node.Kind.c_str());
					for (const auto &parameter : node.Parameters)
						if (const auto *number = std::get_if<uint64_t>(&parameter.Value)) {
							uint64_t value = *number;
							if (ImGui::InputScalar(parameter.Name.c_str(), ImGuiDataType_U64, &value)) {
								ProceduralAction action{ProceduralActionKind::Parameter};
								action.Node = node.Id;
								action.Parameter = parameter.Name;
								action.ParameterValue = value;
								state.Queue(action);
							}
						}
					ImGui::PopID();
				}
			}
			if (ImGui::Button("Preview")) state.Queue({ProceduralActionKind::Preview});
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) state.Queue({ProceduralActionKind::Cancel});
			ImGui::SameLine();
			if (ImGui::Button("Activate ready regions")) state.Queue({ProceduralActionKind::Activate});
			if (preview)
				for (const auto &progress : preview->Progress())
					ImGui::Text(
						"%llu  %s  cells %u  tests %u  placements %u",
						static_cast<unsigned long long>(progress.Ticket),
						procedural_panel_detail::Status(progress.Status),
						progress.CandidateCells,
						progress.SpatialTests,
						progress.Placements
					);
			if (!authority->Regions.empty()) {
				state.RegionIndex =
					std::clamp(state.RegionIndex, 0, static_cast<int>(authority->Regions.size()) - 1);
				if (ImGui::BeginCombo("Region", std::to_string(state.RegionIndex).c_str())) {
					for (size_t index = 0; index < authority->Regions.size(); ++index) {
						const auto scope = authority->Regions[index].Scope;
						const auto label = std::to_string(scope.X) + ", " + std::to_string(scope.Y);
						if (ImGui::Selectable(label.c_str(), state.RegionIndex == static_cast<int>(index)))
							state.RegionIndex = static_cast<int>(index);
					}
					ImGui::EndCombo();
				}
				ImGui::SliderFloat("View angle", &state.Orbit, -3.14f, 3.14f);
				const auto imageSelection = authority->Regions[static_cast<size_t>(state.RegionIndex)].Scope;
				if (retainedImage && state.HasImageScope && state.ImageScope.X == imageSelection.X &&
					state.ImageScope.Y == imageSelection.Y &&
					state.ImageScope.CellsPerSide == imageSelection.CellsPerSide) {
					const float side = std::min(480.0f, ImGui::GetContentRegionAvail().x);
					ImGui::Image(ImTextureRef(retainedImage), ImVec2(side, side));
				}
				ImGui::InputText("Baked output", state.Output.data(), state.Output.size());
				for (const auto &[label, kind] :
					 {std::pair{"Bake ordinary assets", ProceduralActionKind::Bake},
					  std::pair{"Bake and signed publish", ProceduralActionKind::Publish}}) {
					if (ImGui::Button(label)) {
						ProceduralAction action{kind};
						action.Scope = authority->Regions[static_cast<size_t>(state.RegionIndex)].Scope;
						action.Output = state.Output.data();
						state.Queue(action);
					}
				}
			}
			if (ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen)) {
				for (const auto &fact : authority->Sources) {
					const auto label =
						(fact.Kind == procedural::SourceKind::Village ? "Village " : "Entrance ") + fact.Name;
					if (ImGui::Selectable(label.c_str())) {
						std::snprintf(
							state.SourceName.data(), state.SourceName.size(), "%s", fact.Name.c_str()
						);
						std::snprintf(
							state.TargetVillage.data(),
							state.TargetVillage.size(),
							"%s",
							fact.TargetVillage.c_str()
						);
						state.SourceKind = fact.Kind == procedural::SourceKind::Village ? 0 : 1;
						state.SourceCell = {
							fact.Anchor.X / procedural_panel_detail::CELL,
							fact.Anchor.Y / procedural_panel_detail::CELL
						};
					}
				}
				ImGui::Combo("Kind", &state.SourceKind, "Village\0Entrance\0");
				ImGui::InputText("Source name", state.SourceName.data(), state.SourceName.size());
				ImGui::InputScalarN("Source cells X/Z", ImGuiDataType_S64, state.SourceCell.data(), 2);
				if (state.SourceKind == 1)
					ImGui::InputText("Village", state.TargetVillage.data(), state.TargetVillage.size());
				for (const auto &[label, kind] :
					 {std::pair{"Set source", ProceduralActionKind::SetSource},
					  std::pair{"Remove source", ProceduralActionKind::RemoveSource}})
					if (ImGui::Button(label)) {
						ProceduralAction action{kind};
						action.Source.Kind = state.SourceKind ? procedural::SourceKind::Entrance
															  : procedural::SourceKind::Village;
						action.Source.Name = state.SourceName.data();
						action.Source.TargetVillage = state.SourceKind ? state.TargetVillage.data() : "";
						if (procedural_panel_detail::Cells(state.SourceCell, action.Source.Anchor))
							state.Queue(action);
						else
							state.Status = "Source position is outside the authored range";
					}
			}
			if (ImGui::CollapsingHeader("Road controls")) {
				for (const auto &fact : authority->Splines)
					if (ImGui::Selectable(fact.Name.c_str())) {
						std::snprintf(
							state.SplineName.data(), state.SplineName.size(), "%s", fact.Name.c_str()
						);
						std::snprintf(
							state.EntranceName.data(),
							state.EntranceName.size(),
							"%s",
							fact.EntranceName.c_str()
						);
						state.ControlCell = {
							fact.Control.X / procedural_panel_detail::CELL,
							fact.Control.Y / procedural_panel_detail::CELL
						};
					}
				ImGui::InputText("Control name", state.SplineName.data(), state.SplineName.size());
				ImGui::InputText("Entrance", state.EntranceName.data(), state.EntranceName.size());
				ImGui::InputScalarN("Control cells X/Z", ImGuiDataType_S64, state.ControlCell.data(), 2);
				for (const auto &[label, kind] :
					 {std::pair{"Set road control", ProceduralActionKind::SetSpline},
					  std::pair{"Remove road control", ProceduralActionKind::RemoveSpline}})
					if (ImGui::Button(label)) {
						ProceduralAction action{kind};
						action.Spline.Name = state.SplineName.data();
						action.Spline.EntranceName = state.EntranceName.data();
						if (procedural_panel_detail::Cells(state.ControlCell, action.Spline.Control))
							state.Queue(action);
						else
							state.Status = "Road control is outside the authored range";
					}
			}
			if (ImGui::CollapsingHeader("Placement overrides")) {
				if (preview && !authority->Regions.empty()) {
					const auto selected = authority->Regions[static_cast<size_t>(state.RegionIndex)].Scope;
					if (ImGui::BeginChild("Generated placements", ImVec2(0, 150))) {
						for (const auto &target : preview->Targets()) {
							if (target.Scope.X != selected.X || target.Scope.Y != selected.Y ||
								target.Scope.CellsPerSide != selected.CellsPerSide)
								continue;
							const auto *region =
								preview->PreviewStore().Get<script::ProceduralSceneRegion>(target.Mesh);
							if (!region) continue;
							ImGuiListClipper clipper;
							clipper.Begin(static_cast<int>(region->Objects.size()));
							while (clipper.Step())
								for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index) {
									const auto &object = region->Objects[static_cast<size_t>(index)];
									ImGui::BeginDisabled(object.Kind != "tree" && object.Kind != "house");
									if (ImGui::Selectable(object.Id.c_str()))
										std::snprintf(
											state.PlacementId.data(),
											state.PlacementId.size(),
											"%s",
											object.Id.c_str()
										);
									ImGui::EndDisabled();
								}
						}
					}
					ImGui::EndChild();
				}
				for (const auto &fact : authority->Overrides)
					if (ImGui::Selectable(fact.TargetId.c_str())) {
						std::snprintf(
							state.PlacementId.data(), state.PlacementId.size(), "%s", fact.TargetId.c_str()
						);
						std::snprintf(
							state.PlacementSource.data(),
							state.PlacementSource.size(),
							"%s",
							fact.SourceName.c_str()
						);
						state.OverrideKind = fact.Action == procedural::OverrideAction::Replace ? 1 : 0;
						state.PlacementCell = {
							fact.Position.X / procedural_panel_detail::CELL,
							fact.Position.Y / procedural_panel_detail::CELL
						};
						state.Rotation = fact.RotationQuarterTurns;
						state.Variant = fact.Variant;
						state.Material = fact.Material;
					}
				ImGui::InputText("Placement ID", state.PlacementId.data(), state.PlacementId.size());
				ImGui::Combo("Action", &state.OverrideKind, "Delete\0Replace\0");
				ImGui::InputScalarN("Placement cells X/Z", ImGuiDataType_S64, state.PlacementCell.data(), 2);
				ImGui::SliderInt("Rotation quarter turns", &state.Rotation, 0, 3);
				ImGui::InputInt("Variant", &state.Variant);
				ImGui::InputInt("Material", &state.Material);
				ImGui::InputText(
					"Placement source", state.PlacementSource.data(), state.PlacementSource.size()
				);
				for (const auto &[label, kind] :
					 {std::pair{"Set override", ProceduralActionKind::SetOverride},
					  std::pair{"Remove override", ProceduralActionKind::RemoveOverride}})
					if (ImGui::Button(label)) {
						ProceduralAction action{kind};
						auto &value = action.Override;
						value.TargetId = state.PlacementId.data();
						value.Action = state.OverrideKind ? procedural::OverrideAction::Replace
														  : procedural::OverrideAction::Delete;
						if (value.Action == procedural::OverrideAction::Replace) {
							value.SourceName = state.PlacementSource.data();
							value.RotationQuarterTurns = static_cast<uint8_t>(state.Rotation);
							value.Variant = static_cast<uint16_t>(std::clamp(state.Variant, 0, 65535));
							value.Material = static_cast<uint16_t>(std::clamp(state.Material, 0, 65535));
							if (!procedural_panel_detail::Cells(state.PlacementCell, value.Position)) {
								state.Status = "Placement is outside the authored range";
								continue;
							}
						}
						state.Queue(action);
					}
			}
		}
		ImGui::EndDisabled();
		if (!state.Status.empty()) ImGui::TextWrapped("%s", state.Status.c_str());
		ImGui::End();
		ImGui::PopStyleColor(2);
	}
}
