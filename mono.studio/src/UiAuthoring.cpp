#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Values.hpp>
#include <engine/gui/Accessibility.hpp>
#include <engine/gui/Animation.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Compile.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Document.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Style.hpp>
#include <engine/ui/Prompts.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <imgui.h>
#include <optional>
#include <span>
#include <studio/Editor.hpp>
#include <studio/UiDocumentFile.hpp>
#include <studio/UiExternalImport.hpp>
#include <vector>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	namespace {
		const char *BindingFailureName(engine::gui::BindingFailure failure) {
			using Failure = engine::gui::BindingFailure;
			switch (failure) {
			case Failure::None:
				return "none";
			case Failure::InvalidSourcePath:
				return "invalid source path";
			case Failure::MissingSource:
				return "missing source";
			case Failure::MissingAttribute:
				return "missing attribute";
			case Failure::TypeMismatch:
				return "type mismatch";
			case Failure::UnsupportedTarget:
				return "unsupported target";
			case Failure::OutputTooLong:
				return "output too long";
			}
			return "unknown";
		}

		const char *SemanticRoleName(engine::gui::SemanticRole role) {
			using Role = engine::gui::SemanticRole;
			switch (role) {
			case Role::Text:
				return "text";
			case Role::Button:
				return "button";
			case Role::TextField:
				return "text field";
			case Role::Image:
				return "image";
			case Role::Group:
				return "group";
			}
			return "unknown";
		}

		const char *StyleSourceName(engine::gui::StyleSource source) {
			switch (source) {
			case engine::gui::StyleSource::Theme:
				return "theme";
			case engine::gui::StyleSource::ClassRule:
				return "class rule";
			case engine::gui::StyleSource::LocalRule:
				return "local rule";
			case engine::gui::StyleSource::Direct:
				return "direct property";
			}
			return "unknown";
		}

		bool StyleValuesMatch(const engine::gui::StyleSet &left, const engine::gui::StyleSet &right) {
			if (left.Declarations().size() != right.Declarations().size()) return false;
			for (const auto &declaration : left.Declarations()) {
				const auto *other = right.Find(declaration.Name);
				if (other == nullptr || other->Type != declaration.Value.Type) return false;
				if (other->Type == engine::gui::StyleValueType::Color) {
					if (!(other->Color == declaration.Value.Color)) return false;
				} else if (other->Number != declaration.Value.Number) {
					return false;
				}
			}
			return true;
		}

		bool SameStyleValue(const engine::gui::StyleValue &left, const engine::gui::StyleValue &right) {
			return left.Type == right.Type &&
				   (left.Type == engine::gui::StyleValueType::Color ? left.Color == right.Color
																	: left.Number == right.Number);
		}

		struct ThemeExtraction {
			Name Token;
			engine::gui::StyleValue Value;
			engine::gui::StyleDirectProperty Property;
		};

		constexpr std::array EXTRACTION_PROPERTIES{
			std::pair{"BackgroundColor3", engine::gui::StyleDirectProperty::BackgroundColor},
			std::pair{"BackgroundTransparency", engine::gui::StyleDirectProperty::BackgroundTransparency},
			std::pair{"TextColor3", engine::gui::StyleDirectProperty::TextColor},
			std::pair{"TextTransparency", engine::gui::StyleDirectProperty::TextTransparency},
			std::pair{"ImageColor3", engine::gui::StyleDirectProperty::ImageColor},
			std::pair{"ImageTransparency", engine::gui::StyleDirectProperty::ImageTransparency},
		};

		bool
		OwnsStyleProperty(const Store &store, Entity instance, engine::gui::StyleDirectProperty property) {
			switch (property) {
			case engine::gui::StyleDirectProperty::BackgroundColor:
			case engine::gui::StyleDirectProperty::BackgroundTransparency:
				return store.Get<engine::gui::Background>(instance) != nullptr;
			case engine::gui::StyleDirectProperty::TextColor:
			case engine::gui::StyleDirectProperty::TextTransparency:
				return store.Get<engine::gui::Label>(instance) != nullptr;
			case engine::gui::StyleDirectProperty::ImageColor:
			case engine::gui::StyleDirectProperty::ImageTransparency:
				return store.Get<engine::gui::Picture>(instance) != nullptr;
			}
			return false;
		}

		struct UiSummary {
			bool IsGui = false;
			bool IsCollector = false;
			bool HasStyle = false;
			bool HasTheme = false;
			bool HasBinding = false;
			bool HasAnimation = false;
			bool HasLocalizedArguments = false;
			engine::gui::StyleClass StyleClass;
			engine::gui::UIStyle Style;
			engine::gui::UITheme Theme;
			engine::gui::ThemeBinding ThemeBinding;
			engine::gui::Binding Binding;
			engine::gui::BindingOutput BindingOutput;
			engine::gui::AnimationPlayback Animation;
			engine::gui::LabelPresentation LabelPresentation;
			engine::gui::LabelLocalizationArguments LocalizedArguments;
			engine::gui::ResolvedStyle ResolvedStyle;
			engine::gui::StyleResolution StyleTrace;
			std::array<Name, engine::gui::MAXIMUM_STYLE_RULES> StyleRuleNames{};
			Name StyleThemeName;
			bool HasResolvedStyle = false;
			bool HasStyleTrace = false;
		};

		struct PropertyEdit {
			Name Property;
			engine::game::PropertyValue Value;
			std::string Description;
		};

		void CopyText(char *destination, size_t capacity, std::string_view source) {
			const size_t copied = std::min(capacity - 1, source.size());
			std::memcpy(destination, source.data(), copied);
			destination[copied] = '\0';
		}

		const engine::ecs::PropertyDescriptor *PropertyOf(const Store &store, Entity instance, Name name) {
			for (const engine::ecs::PropertyDescriptor &property : store.PropertiesOf(instance))
				if (property.Name == name) return &property;
			return nullptr;
		}

		constexpr ImGuiID DRAFT_ACTIVE = 0x41C0u;
		constexpr ImGuiID DRAFT_CANCELLED = 0x41C1u;
		constexpr ImGuiID DRAFT_VALUE = 0x41D0u;

		ImGuiID DraftKey(Entity selected, uint32_t epoch, ImGuiID item, ImGuiID field) {
			const uint64_t identity = selected.Id;
			return item ^ field ^ epoch ^ static_cast<ImGuiID>(identity) ^
				   static_cast<ImGuiID>(identity >> 32U);
		}

		float DraftFloat(
			ImGuiStorage &storage, Entity selected, uint32_t epoch, ImGuiID item, size_t index, float current
		) {
			if (!storage.GetBool(DraftKey(selected, epoch, item, DRAFT_ACTIVE), false)) return current;
			return storage.GetFloat(
				DraftKey(selected, epoch, item, DRAFT_VALUE + static_cast<ImGuiID>(index)), current
			);
		}

		bool FinishDraft(
			ImGuiStorage &storage,
			Entity selected,
			uint32_t epoch,
			ImGuiID item,
			std::span<const float> values
		) {
			if (ImGui::IsItemActive()) {
				for (size_t index = 0; index < values.size(); ++index)
					storage.SetFloat(
						DraftKey(selected, epoch, item, DRAFT_VALUE + static_cast<ImGuiID>(index)),
						values[index]
					);
				storage.SetBool(DraftKey(selected, epoch, item, DRAFT_ACTIVE), true);
				if (ImGui::IsKeyPressed(ImGuiKey_Escape))
					storage.SetBool(DraftKey(selected, epoch, item, DRAFT_CANCELLED), true);
			}
			if (!ImGui::IsItemDeactivated()) return false;
			const bool cancelled = storage.GetBool(DraftKey(selected, epoch, item, DRAFT_CANCELLED), false);
			const bool edited = ImGui::IsItemDeactivatedAfterEdit();
			storage.SetBool(DraftKey(selected, epoch, item, DRAFT_ACTIVE), false);
			storage.SetBool(DraftKey(selected, epoch, item, DRAFT_CANCELLED), false);
			return edited && !cancelled;
		}
	}

	void Editor::DrawUiAuthoring() {
		if (!ShowUiAuthoring) return;
		if (!ImGui::Begin("UI Authoring", &ShowUiAuthoring)) {
			ImGui::End();
			return;
		}
		if (ImGui::Button("Import UI document...")) ImGui::OpenPopup("Import UI Document");
		if (engine::ui::FilePrompt("Import UI Document", UiDocumentImportPath, "Import", {".aui"}, true)) {
			engine::gui::UiDocument document;
			engine::gui::DocumentReport report;
			if (!ReadUiDocumentFile(UiDocumentImportPath, document, report)) {
				UiDocumentStatus =
					report.Issues.empty() ? "invalid UI document" : report.Issues.front().Message;
			} else if (!Active.IsValid() || AuthorityOf(Active) != EditAuthority::Authoritative ||
					   Commands == nullptr) {
				UiDocumentStatus = "open an editable world to import";
			} else {
				const auto recording = Commands->TryBeginRecording("Import UI document");
				if (!recording) {
					UiDocumentStatus = "finish the active edit before importing";
				} else {
					std::vector<Entity> roots;
					const Entity selected =
						SelectionWorld == Active && Selection.size() == 1 ? Selection.front() : NULL_ENTITY;
					bool imported = false;
					const auto status = Universe->Enter(Active, [&](Store &store) {
						imported =
							ImportUiDocumentEdit(store, Active, selected, document, *Commands, roots, report);
					});
					if (status != engine::world::WorldStatus::Ok) {
						UiDocumentStatus = "the edit world is unavailable";
					} else if (!imported && !report.Issues.empty()) {
						UiDocumentStatus = report.Issues.front().Message;
					}
					Commands->FinishRecording(
						*recording, imported ? FinishOperation::Commit : FinishOperation::Cancel
					);
					if (imported) {
						MarkModified();
						UiDocumentStatus = std::to_string(roots.size()) + " UI root(s) imported";
						if (!roots.empty()) Select(Active, roots.front(), false);
					}
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Import design tokens...")) ImGui::OpenPopup("Import design tokens");
		if (engine::ui::FilePrompt(
				"Import design tokens", UiThemeTokenImportPath, "Import", {".json"}, true
			)) {
			if (!Active.IsValid() || AuthorityOf(Active) != EditAuthority::Authoritative ||
				Commands == nullptr) {
				UiDocumentStatus = "open an editable world to import";
			} else if (const auto recording = Commands->TryBeginRecording("Import design tokens")) {
				std::vector<Entity> roots;
				engine::gui::DocumentReport report;
				const Entity parent =
					SelectionWorld == Active && Selection.size() == 1 ? Selection.front() : NULL_ENTITY;
				bool imported = false;
				const auto status = Universe->Enter(Active, [&](Store &store) {
					imported = ImportUiDesignTokensFileEdit(
						store, Active, parent, UiThemeTokenImportPath, *Commands, roots, report
					);
				});
				Commands->FinishRecording(
					*recording, imported ? FinishOperation::Commit : FinishOperation::Cancel
				);
				if (status != engine::world::WorldStatus::Ok)
					UiDocumentStatus = "the edit world is unavailable";
				else if (!imported)
					UiDocumentStatus = report.Issues.empty() ? "could not import design tokens"
															 : report.Issues.front().Message;
				else {
					MarkModified();
					UiDocumentStatus = "design token theme imported";
					if (!roots.empty()) Select(Active, roots.front(), false);
				}
			} else {
				UiDocumentStatus = "finish the active edit before importing";
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Import localization...")) ImGui::OpenPopup("Import localization");
		if (engine::ui::FilePrompt(
				"Import localization", UiLocalizationImportPath, "Import", {".csv"}, true
			)) {
			if (!Active.IsValid() || AuthorityOf(Active) != EditAuthority::Authoritative ||
				Commands == nullptr) {
				UiDocumentStatus = "open an editable world to import";
			} else if (const auto recording = Commands->TryBeginRecording("Import localization")) {
				engine::gui::DocumentReport report;
				Entity table = NULL_ENTITY;
				bool imported = false;
				const auto status = Universe->Enter(Active, [&](Store &store) {
					imported = ImportUiLocalizationFileEdit(
						store, Active, UiLocalizationImportPath, *Commands, table, report
					);
				});
				Commands->FinishRecording(
					*recording, imported ? FinishOperation::Commit : FinishOperation::Cancel
				);
				if (status != engine::world::WorldStatus::Ok)
					UiDocumentStatus = "the edit world is unavailable";
				else if (!imported)
					UiDocumentStatus = report.Issues.empty() ? "could not import localization"
															 : report.Issues.front().Message;
				else {
					MarkModified();
					UiDocumentStatus = "localization table imported";
				}
			} else {
				UiDocumentStatus = "finish the active edit before importing";
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Export selected...")) ImGui::OpenPopup("Export UI Document");
		if (engine::ui::FilePrompt("Export UI Document", UiDocumentExportPath, "Export", {".aui"}, false)) {
			engine::gui::UiDocument document;
			engine::gui::DocumentReport report;
			bool exported = false;
			if (SelectionWorld.IsValid() && Selection.size() == 1) {
				const Entity selected = Selection.front();
				Universe->Enter(SelectionWorld, [&](Store &store) {
					exported = engine::gui::ExportDocument(store, selected, document, report);
				});
			}
			if (!exported) {
				UiDocumentStatus = report.Issues.empty() ? "select one GUI subtree to export"
														 : report.Issues.front().Message;
			} else if (!WriteUiDocumentFile(UiDocumentExportPath, document, report)) {
				UiDocumentStatus =
					report.Issues.empty() ? "could not write UI document" : report.Issues.front().Message;
			} else {
				UiDocumentStatus = "UI document exported";
			}
		}
		if (!UiDocumentStatus.empty()) ImGui::TextUnformatted(UiDocumentStatus.c_str());
		ImGui::Separator();
		if (!SelectionWorld.IsValid() || Selection.size() != 1) {
			ImGui::TextDisabled("select one GUI instance or UI modifier");
			ImGui::End();
			return;
		}

		const Entity selected = Selection.front();
		if (UiAuthoringDraftEntity != selected.Id) {
			UiAuthoringDraftEntity = selected.Id;
			UiAuthoringDraftEpoch++;
		}
		const std::string selectedDraftPrefix = std::to_string(selected.Id) + ":";
		std::erase_if(UiAuthoringTextDrafts, [&](const auto &entry) {
			return !entry.first.starts_with(selectedDraftPrefix);
		});
		UiSummary summary;
		std::optional<PropertyEdit> propertyEdit;
		std::optional<engine::gui::UITheme> themeEdit;
		std::optional<engine::gui::AnimationPlayback> animationEdit;
		std::optional<engine::gui::StyleClass> styleClassEdit;
		std::optional<engine::gui::UIStyle> styleEdit;
		std::optional<engine::gui::LabelLocalizationArguments> localizedArgumentsEdit;
		bool extractRepeatedDirect = false;
		std::vector<Entity> availableThemes;
		std::vector<engine::gui::SemanticNode> semanticNodes;
		bool semanticTruncated = false;
		engine::gui::SemanticAudit semanticAudit;
		const auto editText = [&](const char *label, std::string_view current) -> std::optional<std::string> {
			const std::string key = selectedDraftPrefix + std::to_string(ImGui::GetID(label));
			auto draft = UiAuthoringTextDrafts.try_emplace(key, current).first;
			std::vector<char> text(std::max<size_t>(256, draft->second.size() + 256), '\0');
			CopyText(text.data(), text.size(), draft->second);
			ImGui::InputText(label, text.data(), text.size());
			if (ImGui::IsItemActive()) {
				if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
					UiAuthoringTextDrafts.erase(draft);
					return std::nullopt;
				}
				draft->second = text.data();
			}
			if (!ImGui::IsItemDeactivated()) return std::nullopt;
			const bool commit = ImGui::IsItemDeactivatedAfterEdit();
			std::string completed = std::move(draft->second);
			UiAuthoringTextDrafts.erase(draft);
			if (!commit) return std::nullopt;
			return completed;
		};
		const bool hasList =
			FocusedViewport < GuiLists.size() && ViewportWorld(FocusedViewport) == SelectionWorld;
		const size_t commandCount = hasList ? GuiLists[FocusedViewport].Commands().Commands.size() : 0;

		Universe->Enter(SelectionWorld, [&](Store &store) {
			if (!store.Alive(selected)) return;
			summary.IsGui = store.IsA(selected, engine::gui::GuiClass("GuiBase"));
			summary.IsCollector = store.IsA(selected, engine::gui::GuiClass("LayerCollector"));
			if (const auto *value = store.Get<engine::gui::StyleClass>(selected)) summary.StyleClass = *value;
			if (const auto *value = store.Get<engine::gui::UIStyle>(selected)) {
				summary.HasStyle = true;
				summary.Style = *value;
			}
			if (const auto *value = store.Get<engine::gui::UITheme>(selected)) {
				summary.HasTheme = true;
				summary.Theme = *value;
			}
			if (const auto *value = store.Get<engine::gui::ThemeBinding>(selected))
				summary.ThemeBinding = *value;
			if (const auto *value = store.Get<engine::gui::Binding>(selected)) {
				summary.HasBinding = true;
				summary.Binding = *value;
				if (const auto *output = store.Get<engine::gui::BindingOutput>(selected))
					summary.BindingOutput = *output;
			}
			if (const auto *value = store.Get<engine::gui::AnimationPlayback>(selected)) {
				summary.HasAnimation = true;
				summary.Animation = *value;
			}
			if (const auto *value = store.Get<engine::gui::LabelPresentation>(selected)) {
				summary.LabelPresentation = *value;
				summary.HasLocalizedArguments = true;
			}
			if (const auto *value = store.Get<engine::gui::LabelLocalizationArguments>(selected)) {
				summary.HasLocalizedArguments = true;
				summary.LocalizedArguments = *value;
			}
			if (const auto *value = store.Get<engine::gui::ResolvedStyle>(selected)) {
				summary.ResolvedStyle = *value;
				summary.HasResolvedStyle = true;
				Entity collector = selected;
				while (collector != NULL_ENTITY &&
					   !store.IsA(collector, engine::gui::GuiClass("LayerCollector"))) {
					collector = store.ParentOf(collector);
				}
				if (collector != NULL_ENTITY) {
					engine::gui::StyleSet direct;
					engine::gui::CollectDirectStyleValues(store, selected, direct);
					std::array<engine::gui::StyleRule, engine::gui::MAXIMUM_STYLE_RULES> rules{};
					size_t count = 0;
					bool overflow = false;
					store.EachChild(selected, [&](Entity child) {
						const auto *style = store.Get<engine::gui::UIStyle>(child);
						if (style == nullptr) return;
						if (count == rules.size()) {
							overflow = true;
							return;
						}
						rules[count] = style->Rule;
						summary.StyleRuleNames[count++] = store.InstanceNameOf(child);
					});
					const engine::gui::StyleSet *theme = nullptr;
					if (const auto *binding = store.Get<engine::gui::ThemeBinding>(collector);
						binding != nullptr && store.Alive(binding->Theme)) {
						if (const auto *value = store.Get<engine::gui::UITheme>(binding->Theme)) {
							theme = &value->Tokens;
							summary.StyleThemeName = store.InstanceNameOf(binding->Theme);
						}
					}
					const engine::gui::StyleSet emptyTheme;
					summary.HasStyleTrace =
						!overflow &&
						engine::gui::ResolveStyleTrace(
							theme != nullptr ? *theme : emptyTheme,
							summary.StyleClass.Names,
							{rules.data(), count},
							direct,
							summary.ResolvedStyle.State,
							summary.StyleTrace
						) &&
						StyleValuesMatch(summary.ResolvedStyle.Values, summary.StyleTrace.Values);
				}
			}
			store.Each<const engine::gui::UITheme>([&](Entity entity, const engine::gui::UITheme &) {
				availableThemes.push_back(entity);
			});
			if (hasList) {
				const engine::gui::SemanticSnapshot snapshot =
					engine::gui::CompileSemantics(store, GuiLists[FocusedViewport].Commands());
				semanticNodes = snapshot.Nodes;
				semanticTruncated = snapshot.Truncated;
				semanticAudit = engine::gui::AuditSemantics(snapshot);
			}
		});

		if (!summary.IsGui && !summary.HasStyle && !summary.HasTheme && !summary.HasBinding &&
			!summary.HasAnimation) {
			ImGui::TextDisabled("the selected instance is not part of the game UI system");
			ImGui::End();
			return;
		}
		ImGui::TextDisabled("edits use the same authored values and undo history as Properties");

		if (summary.IsGui && ImGui::CollapsingHeader("Styles", ImGuiTreeNodeFlags_DefaultOpen)) {
			const auto classes = summary.StyleClass.Names.Names();
			ImGui::Text("classes: %zu", classes.size());
			for (const Name value : classes)
				ImGui::BulletText("%s", value.Text().data());
			for (size_t index = 0; index < classes.size(); index++) {
				ImGui::PushID(static_cast<int>(index));
				ImGui::TextUnformatted(classes[index].Text().data());
				ImGui::SameLine();
				const bool remove = ImGui::SmallButton("remove");
				ImGui::SameLine();
				const bool up = index > 0 && ImGui::SmallButton("up");
				ImGui::SameLine();
				const bool down = index + 1 < classes.size() && ImGui::SmallButton("down");
				if (remove || up || down) {
					std::vector<Name> rebuilt(classes.begin(), classes.end());
					if (remove)
						rebuilt.erase(rebuilt.begin() + static_cast<std::ptrdiff_t>(index));
					else
						std::swap(rebuilt[index], rebuilt[up ? index - 1 : index + 1]);
					engine::gui::StyleClass changed;
					for (const Name name : rebuilt)
						(void)changed.Names.Add(name);
					styleClassEdit = changed;
				}
				ImGui::PopID();
			}
			if (const auto name = editText("add class", "")) {
				engine::gui::StyleClass changed = summary.StyleClass;
				if (changed.Names.Add(Name(*name))) styleClassEdit = changed;
			}
			if (summary.IsCollector) {
				ImGui::Text("theme: %s", summary.ThemeBinding.Theme == NULL_ENTITY ? "none" : "assigned");
				int themeIndex = summary.ThemeBinding.Theme == NULL_ENTITY ? 0 : 0;
				for (size_t index = 0; index < availableThemes.size(); ++index)
					if (availableThemes[index] == summary.ThemeBinding.Theme)
						themeIndex = static_cast<int>(index + 1);
				if (ImGui::BeginCombo("assign theme", themeIndex == 0 ? "none" : "imported theme")) {
					if (ImGui::Selectable("none", themeIndex == 0)) {
						engine::game::PropertyValue value;
						value.Type = engine::ecs::PropertyType::Reference;
						value.Reference = NULL_ENTITY;
						propertyEdit = {Name("Theme"), std::move(value), "Clear UI theme"};
					}
					for (size_t index = 0; index < availableThemes.size(); ++index) {
						ImGui::PushID(static_cast<int>(index));
						if (ImGui::Selectable("imported theme", themeIndex == static_cast<int>(index + 1))) {
							engine::game::PropertyValue value;
							value.Type = engine::ecs::PropertyType::Reference;
							value.Reference = availableThemes[index];
							propertyEdit = {Name("Theme"), std::move(value), "Assign UI theme"};
						}
						ImGui::PopID();
					}
					ImGui::EndCombo();
				}
			}
			if (ImGui::Button("Add local style")) {
				InsertInstance(SelectionWorld, engine::gui::GuiClass("UIStyle"), selected);
			}
			if (summary.IsCollector && summary.ThemeBinding.Theme != NULL_ENTITY) {
				ImGui::SameLine();
				if (ImGui::Button("Extract repeated direct values")) extractRepeatedDirect = true;
			}
			ImGui::SeparatorText("Resolved style trace");
			if (!summary.HasResolvedStyle) {
				ImGui::TextDisabled("compile this viewport to inspect resolved values");
			} else if (!summary.HasStyleTrace) {
				ImGui::TextDisabled("the style inputs could not be resolved");
			} else {
				ImGui::Text("state flags: %u", static_cast<unsigned>(summary.ResolvedStyle.State));
				for (const auto &source : summary.StyleTrace.Sources()) {
					ImGui::BulletText("%s: %s", source.Name.Text().data(), StyleSourceName(source.Source));
					ImGui::SameLine();
					if (source.Value.Type == engine::gui::StyleValueType::Color) {
						ImGui::TextDisabled(
							"(%.2f, %.2f, %.2f)",
							source.Value.Color.R,
							source.Value.Color.G,
							source.Value.Color.B
						);
					} else {
						ImGui::TextDisabled("(%.3g)", source.Value.Number);
					}
					if (source.Source == engine::gui::StyleSource::Theme &&
						summary.StyleThemeName.IsValid()) {
						ImGui::SameLine();
						ImGui::TextDisabled("(%s)", summary.StyleThemeName.Text().data());
					} else if (source.RuleIndex != engine::gui::StyleProvenance::NO_RULE &&
							   source.RuleIndex < summary.StyleRuleNames.size()) {
						ImGui::SameLine();
						const Name ruleName = summary.StyleRuleNames[source.RuleIndex];
						ImGui::TextDisabled(
							"(%s%s%s)",
							ruleName.IsValid() ? ruleName.Text().data() : "UIStyle",
							source.Class.IsValid() ? ": " : "",
							source.Class.IsValid() ? source.Class.Text().data() : ""
						);
					}
				}
			}
		}
		if (summary.HasStyle && ImGui::CollapsingHeader("Style rule", ImGuiTreeNodeFlags_DefaultOpen)) {
			const engine::gui::StyleRule &rule = summary.Style.Rule;
			if (const auto name = editText("rule class", rule.Class.Text())) {
				engine::gui::UIStyle changed = summary.Style;
				changed.Rule.Class = Name(*name);
				styleEdit = changed;
			}
			int state = static_cast<int>(rule.State);
			if (ImGui::InputInt("state flags", &state) && state >= 0 && state <= 63) {
				engine::gui::UIStyle changed = summary.Style;
				changed.Rule.State = static_cast<engine::gui::StyleState>(state);
				styleEdit = changed;
			}
			for (const auto &declaration : rule.Declarations.Declarations()) {
				ImGui::PushID(declaration.Name.Text().data());
				if (declaration.Value.Type == engine::gui::StyleValueType::Color) {
					ImGuiStorage &drafts = *ImGui::GetStateStorage();
					const ImGuiID item = ImGui::GetID("color");
					float color[] = {
						DraftFloat(
							drafts, selected, UiAuthoringDraftEpoch, item, 0, declaration.Value.Color.R
						),
						DraftFloat(
							drafts, selected, UiAuthoringDraftEpoch, item, 1, declaration.Value.Color.G
						),
						DraftFloat(
							drafts, selected, UiAuthoringDraftEpoch, item, 2, declaration.Value.Color.B
						)
					};
					ImGui::ColorEdit3("color", color, ImGuiColorEditFlags_Float);
					if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, item, color)) {
						engine::gui::UIStyle changed = summary.Style;
						changed.Rule.Declarations.Set(
							{declaration.Name,
							 engine::gui::StyleValue::FromColor({color[0], color[1], color[2]})}
						);
						styleEdit = changed;
					}
				} else {
					ImGuiStorage &drafts = *ImGui::GetStateStorage();
					const ImGuiID item = ImGui::GetID("number");
					float value = DraftFloat(
						drafts, selected, UiAuthoringDraftEpoch, item, 0, declaration.Value.Number
					);
					ImGui::DragFloat("number", &value, 0.01f);
					if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, item, {&value, 1})) {
						engine::gui::UIStyle changed = summary.Style;
						changed.Rule.Declarations.Set(
							{declaration.Name, engine::gui::StyleValue::FromNumber(value)}
						);
						styleEdit = changed;
					}
				}
				ImGui::SameLine();
				ImGui::TextUnformatted(declaration.Name.Text().data());
				ImGui::SameLine();
				if (ImGui::SmallButton("remove")) {
					engine::gui::UIStyle changed;
					changed.Rule.Class = rule.Class;
					changed.Rule.State = rule.State;
					for (const auto &other : rule.Declarations.Declarations())
						if (other.Name != declaration.Name) (void)changed.Rule.Declarations.Set(other);
					styleEdit = changed;
				}
				ImGui::PopID();
			}
			if (const auto name = editText("add number declaration", "")) {
				engine::gui::UIStyle changed = summary.Style;
				if (changed.Rule.Declarations.Set({Name(*name), engine::gui::StyleValue::FromNumber(0.0f)}))
					styleEdit = changed;
			}
			if (const auto name = editText("add color declaration", "")) {
				engine::gui::UIStyle changed = summary.Style;
				if (changed.Rule.Declarations.Set({Name(*name), engine::gui::StyleValue::FromColor({})}))
					styleEdit = changed;
			}
		}
		if (summary.HasTheme && ImGui::CollapsingHeader("Theme tokens", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const auto &token : summary.Theme.Tokens.Declarations()) {
				ImGui::PushID(token.Name.Text().data());
				if (token.Value.Type == engine::gui::StyleValueType::Color) {
					ImGuiStorage &drafts = *ImGui::GetStateStorage();
					const ImGuiID item = ImGui::GetID(token.Name.Text().data());
					float color[] = {
						DraftFloat(drafts, selected, UiAuthoringDraftEpoch, item, 0, token.Value.Color.R),
						DraftFloat(drafts, selected, UiAuthoringDraftEpoch, item, 1, token.Value.Color.G),
						DraftFloat(drafts, selected, UiAuthoringDraftEpoch, item, 2, token.Value.Color.B)
					};
					ImGui::ColorEdit3(token.Name.Text().data(), color, ImGuiColorEditFlags_Float);
					if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, item, color)) {
						engine::gui::UITheme changed = summary.Theme;
						changed.Tokens.Set(
							{token.Name, engine::gui::StyleValue::FromColor({color[0], color[1], color[2]})}
						);
						themeEdit = changed;
					}
				} else {
					ImGuiStorage &drafts = *ImGui::GetStateStorage();
					const ImGuiID item = ImGui::GetID(token.Name.Text().data());
					float number =
						DraftFloat(drafts, selected, UiAuthoringDraftEpoch, item, 0, token.Value.Number);
					ImGui::DragFloat(token.Name.Text().data(), &number, 0.01f);
					if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, item, {&number, 1})) {
						engine::gui::UITheme changed = summary.Theme;
						changed.Tokens.Set({token.Name, engine::gui::StyleValue::FromNumber(number)});
						themeEdit = changed;
					}
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("remove")) {
					engine::gui::UITheme changed;
					for (const auto &kept : summary.Theme.Tokens.Declarations())
						if (kept.Name != token.Name) changed.Tokens.Set(kept);
					themeEdit = changed;
				}
				ImGui::PopID();
			}
			if (summary.Theme.Tokens.Declarations().empty()) ImGui::TextDisabled("no tokens");
			std::array<char, 64> tokenName{};
			CopyText(tokenName.data(), tokenName.size(), UiThemeTokenDraft);
			if (ImGui::InputText("new token", tokenName.data(), tokenName.size()))
				UiThemeTokenDraft = tokenName.data();
			if (ImGui::Button("Add colour token")) {
				const Name name(UiThemeTokenDraft);
				if (name.IsValid()) {
					engine::gui::UITheme changed = summary.Theme;
					if (changed.Tokens.Set({name, engine::gui::StyleValue::FromColor({})}))
						themeEdit = changed;
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Add number token")) {
				const Name name(UiThemeTokenDraft);
				if (name.IsValid()) {
					engine::gui::UITheme changed = summary.Theme;
					if (changed.Tokens.Set({name, engine::gui::StyleValue::FromNumber(0.0f)}))
						themeEdit = changed;
				}
			}
		}

		if (summary.IsGui && ImGui::CollapsingHeader("Binding", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Button("Add binding"))
				InsertInstance(SelectionWorld, engine::gui::GuiClass("UIBinding"), selected);
			if (!summary.HasBinding) ImGui::TextDisabled("no binding attached");
		}
		if (summary.HasBinding) {
			if (const auto source = editText("source", summary.Binding.SourcePath)) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::String;
				value.String = *source;
				propertyEdit = {Name("SourcePath"), std::move(value), "Set UI binding source"};
			}
			if (const auto attribute = editText("attribute", summary.Binding.Attribute.Text())) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::Name;
				value.Name = Name(*attribute);
				propertyEdit = {Name("Attribute"), std::move(value), "Set UI binding attribute"};
			}
			if (const auto target = editText("target", summary.Binding.Target.Text())) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::Name;
				value.Name = Name(*target);
				propertyEdit = {Name("Target"), std::move(value), "Set UI binding target"};
			}
			if (const auto fallback = editText("fallback", summary.Binding.Fallback)) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::String;
				value.String = *fallback;
				propertyEdit = {Name("Fallback"), std::move(value), "Set UI binding fallback"};
			}
			ImGui::Text(
				"result: %s",
				summary.BindingOutput.Valid ? "valid" : BindingFailureName(summary.BindingOutput.Failure)
			);
		}

		if (summary.IsGui &&
			ImGui::CollapsingHeader("Presentation animation", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (ImGui::Button("Add animation"))
				InsertInstance(SelectionWorld, engine::gui::GuiClass("UIAnimation"), selected);
			if (!summary.HasAnimation) ImGui::TextDisabled("no animation attached");
		}
		if (summary.HasAnimation) {
			bool playing = summary.Animation.Playing;
			if (ImGui::Checkbox("playing", &playing)) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::Bool;
				value.Bool = playing;
				propertyEdit = {Name("Playing"), std::move(value), "Set UI animation playback"};
			}
			const auto tracks = summary.Animation.Clip.Tracks();
			for (size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
				const auto keys = tracks[trackIndex].Keys();
				ImGui::PushID(static_cast<int>(trackIndex));
				ImGui::Text("track %zu: %zu keyframes", trackIndex + 1, keys.size());
				for (size_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
					ImGui::PushID(static_cast<int>(keyIndex));
					const auto replaceValue = [&](engine::gui::PresentationValue value) {
						engine::gui::AnimationPlayback changed = summary.Animation;
						engine::gui::UIAnimation rebuilt;
						for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
							engine::gui::PresentationTrack rebuiltTrack;
							rebuiltTrack.Property = tracks[rebuildIndex].Property;
							for (size_t rebuildKey = 0; rebuildKey < tracks[rebuildIndex].Keys().size();
								 ++rebuildKey) {
								auto key = tracks[rebuildIndex].Keys()[rebuildKey];
								if (rebuildIndex == trackIndex && rebuildKey == keyIndex) key.Value = value;
								rebuiltTrack.Add(key);
							}
							rebuilt.AddTrack(rebuiltTrack);
						}
						for (const auto marker : summary.Animation.Clip.Markers())
							rebuilt.AddMarker(marker);
						changed.Clip = rebuilt;
						animationEdit = changed;
					};
					ImGuiStorage &drafts = *ImGui::GetStateStorage();
					const ImGuiID timeItem = ImGui::GetID("keyframe");
					float time =
						DraftFloat(drafts, selected, UiAuthoringDraftEpoch, timeItem, 0, keys[keyIndex].Time);
					constexpr float minimumGap = 0.0001f;
					const float low = keyIndex == 0 ? 0.0f : keys[keyIndex - 1].Time + minimumGap;
					const float high =
						keyIndex + 1 == keys.size() ? 1.0f : keys[keyIndex + 1].Time - minimumGap;
					if (low >= high) {
						ImGui::TextDisabled("keyframe has no free time range");
					} else {
						ImGui::SliderFloat("keyframe", &time, low, high);
					}
					if (low < high &&
						FinishDraft(drafts, selected, UiAuthoringDraftEpoch, timeItem, {&time, 1})) {
						engine::gui::AnimationPlayback changed = summary.Animation;
						engine::gui::UIAnimation rebuilt;
						for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
							engine::gui::PresentationTrack rebuiltTrack;
							rebuiltTrack.Property = tracks[rebuildIndex].Property;
							for (size_t rebuildKey = 0; rebuildKey < tracks[rebuildIndex].Keys().size();
								 ++rebuildKey) {
								auto key = tracks[rebuildIndex].Keys()[rebuildKey];
								if (rebuildIndex == trackIndex && rebuildKey == keyIndex) key.Time = time;
								rebuiltTrack.Add(key);
							}
							rebuilt.AddTrack(rebuiltTrack);
						}
						for (const auto marker : summary.Animation.Clip.Markers())
							rebuilt.AddMarker(marker);
						changed.Clip = rebuilt;
						animationEdit = changed;
					}
					switch (keys[keyIndex].Value.Type) {
					case engine::gui::PresentationValueType::Color: {
						const ImGuiID valueItem = ImGui::GetID("value");
						float color[] = {
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								0,
								keys[keyIndex].Value.Color.R
							),
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								1,
								keys[keyIndex].Value.Color.G
							),
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								2,
								keys[keyIndex].Value.Color.B
							)
						};
						ImGui::ColorEdit3("value", color, ImGuiColorEditFlags_Float);
						if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, valueItem, color))
							replaceValue(
								engine::gui::PresentationValue::FromColor({color[0], color[1], color[2]})
							);
						break;
					}
					case engine::gui::PresentationValueType::Number: {
						const ImGuiID valueItem = ImGui::GetID("value");
						float number = DraftFloat(
							drafts, selected, UiAuthoringDraftEpoch, valueItem, 0, keys[keyIndex].Value.Number
						);
						ImGui::DragFloat("value", &number, 0.01f);
						if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, valueItem, {&number, 1}))
							replaceValue(engine::gui::PresentationValue::FromNumber(number));
						break;
					}
					case engine::gui::PresentationValueType::UDim2: {
						const ImGuiID valueItem = ImGui::GetID("value");
						float dimensions[] = {
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								0,
								keys[keyIndex].Value.UDim2.X.Scale
							),
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								1,
								keys[keyIndex].Value.UDim2.X.Offset
							),
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								2,
								keys[keyIndex].Value.UDim2.Y.Scale
							),
							DraftFloat(
								drafts,
								selected,
								UiAuthoringDraftEpoch,
								valueItem,
								3,
								keys[keyIndex].Value.UDim2.Y.Offset
							)
						};
						ImGui::DragFloat4("value", dimensions, 0.01f);
						if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, valueItem, dimensions))
							replaceValue(
								engine::gui::PresentationValue::FromUDim2(
									{dimensions[0], dimensions[1], dimensions[2], dimensions[3]}
								)
							);
						break;
					}
					}
					ImGui::SameLine();
					if (keys.size() > 1 && ImGui::SmallButton("remove keyframe")) {
						engine::gui::AnimationPlayback changed = summary.Animation;
						engine::gui::UIAnimation rebuilt;
						for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
							engine::gui::PresentationTrack rebuiltTrack;
							rebuiltTrack.Property = tracks[rebuildIndex].Property;
							for (size_t rebuildKey = 0; rebuildKey < tracks[rebuildIndex].Keys().size();
								 ++rebuildKey)
								if (rebuildIndex != trackIndex || rebuildKey != keyIndex)
									rebuiltTrack.Add(tracks[rebuildIndex].Keys()[rebuildKey]);
							rebuilt.AddTrack(rebuiltTrack);
						}
						for (const auto marker : summary.Animation.Clip.Markers())
							rebuilt.AddMarker(marker);
						changed.Clip = rebuilt;
						animationEdit = changed;
					}
					ImGui::PopID();
				}
				if (ImGui::SmallButton("Add keyframe")) {
					float largestGap = 0.0f;
					float insertTime = 0.0f;
					float previousTime = 0.0f;
					for (const auto key : tracks[trackIndex].Keys()) {
						if (key.Time - previousTime > largestGap) {
							largestGap = key.Time - previousTime;
							insertTime = (previousTime + key.Time) * 0.5f;
						}
						previousTime = key.Time;
					}
					if (1.0f - previousTime > largestGap) {
						largestGap = 1.0f - previousTime;
						insertTime = (previousTime + 1.0f) * 0.5f;
					}
					if (largestGap <= 0.0002f) {
						UiDocumentStatus = "the animation track has no free keyframe time";
						ImGui::PopID();
						continue;
					}
					engine::gui::AnimationPlayback changed = summary.Animation;
					engine::gui::UIAnimation rebuilt;
					for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
						engine::gui::PresentationTrack rebuiltTrack;
						rebuiltTrack.Property = tracks[rebuildIndex].Property;
						for (const auto key : tracks[rebuildIndex].Keys())
							rebuiltTrack.Add(key);
						if (rebuildIndex == trackIndex) {
							auto key = tracks[rebuildIndex].Keys().back();
							key.Time = insertTime;
							rebuiltTrack.Add(key);
						}
						rebuilt.AddTrack(rebuiltTrack);
					}
					for (const auto marker : summary.Animation.Clip.Markers())
						rebuilt.AddMarker(marker);
					changed.Clip = rebuilt;
					animationEdit = changed;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("remove track")) {
					engine::gui::AnimationPlayback changed = summary.Animation;
					engine::gui::UIAnimation rebuilt;
					for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex)
						if (rebuildIndex != trackIndex) rebuilt.AddTrack(tracks[rebuildIndex]);
					for (const auto marker : summary.Animation.Clip.Markers())
						rebuilt.AddMarker(marker);
					changed.Clip = rebuilt;
					animationEdit = changed;
				}
				ImGui::SameLine();
				if (trackIndex > 0 && ImGui::SmallButton("move track earlier")) {
					engine::gui::AnimationPlayback changed = summary.Animation;
					engine::gui::UIAnimation rebuilt;
					for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
						const size_t source = rebuildIndex == trackIndex - 1 ? trackIndex
											  : rebuildIndex == trackIndex	 ? trackIndex - 1
																			 : rebuildIndex;
						rebuilt.AddTrack(tracks[source]);
					}
					for (const auto marker : summary.Animation.Clip.Markers())
						rebuilt.AddMarker(marker);
					changed.Clip = rebuilt;
					animationEdit = changed;
				}
				ImGui::SameLine();
				if (trackIndex + 1 < tracks.size() && ImGui::SmallButton("move track later")) {
					engine::gui::AnimationPlayback changed = summary.Animation;
					engine::gui::UIAnimation rebuilt;
					for (size_t rebuildIndex = 0; rebuildIndex < tracks.size(); ++rebuildIndex) {
						const size_t source = rebuildIndex == trackIndex	   ? trackIndex + 1
											  : rebuildIndex == trackIndex + 1 ? trackIndex
																			   : rebuildIndex;
						rebuilt.AddTrack(tracks[source]);
					}
					for (const auto marker : summary.Animation.Clip.Markers())
						rebuilt.AddMarker(marker);
					changed.Clip = rebuilt;
					animationEdit = changed;
				}
				ImGui::PopID();
			}
			if (ImGui::Button("Add track")) {
				for (const auto property :
					 {engine::gui::PresentationProperty::BackgroundColor,
					  engine::gui::PresentationProperty::BackgroundTransparency,
					  engine::gui::PresentationProperty::ImageColor,
					  engine::gui::PresentationProperty::ImageTransparency,
					  engine::gui::PresentationProperty::TextColor,
					  engine::gui::PresentationProperty::TextTransparency,
					  engine::gui::PresentationProperty::Rotation,
					  engine::gui::PresentationProperty::Position,
					  engine::gui::PresentationProperty::Size}) {
					const bool exists = std::any_of(tracks.begin(), tracks.end(), [&](const auto &track) {
						return track.Property == property;
					});
					if (exists) continue;
					engine::gui::PresentationTrack track;
					track.Property = property;
					const bool colour = property == engine::gui::PresentationProperty::BackgroundColor ||
										property == engine::gui::PresentationProperty::ImageColor ||
										property == engine::gui::PresentationProperty::TextColor;
					const bool layout = property == engine::gui::PresentationProperty::Position ||
										property == engine::gui::PresentationProperty::Size;
					track.Add({
						0.0f,
						colour	 ? engine::gui::PresentationValue::FromColor({})
						: layout ? engine::gui::PresentationValue::FromUDim2({})
								 : engine::gui::PresentationValue::FromNumber(0.0f),
					});
					engine::gui::AnimationPlayback changed = summary.Animation;
					changed.Clip.AddTrack(track);
					animationEdit = changed;
					break;
				}
			}
			auto markers = summary.Animation.Clip.Markers();
			const auto replaceMarkers = [&](std::vector<engine::gui::AnimationMarker> edited) {
				std::sort(edited.begin(), edited.end(), [](const auto &left, const auto &right) {
					return left.Time < right.Time;
				});
				engine::gui::AnimationPlayback changed = summary.Animation;
				engine::gui::UIAnimation rebuilt;
				for (const auto &track : tracks)
					rebuilt.AddTrack(track);
				for (const auto &marker : edited)
					rebuilt.AddMarker(marker);
				changed.Clip = rebuilt;
				animationEdit = changed;
			};
			ImGui::Text("markers: %zu", markers.size());
			for (size_t markerIndex = 0; markerIndex < markers.size(); ++markerIndex) {
				ImGui::PushID(static_cast<int>(markerIndex));
				if (const auto markerName = editText("marker", markers[markerIndex].Name.Text())) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					edited[markerIndex].Name = Name(*markerName);
					const bool duplicate = std::any_of(edited.begin(), edited.end(), [&](const auto &marker) {
						return &marker != &edited[markerIndex] && marker.Name == edited[markerIndex].Name;
					});
					if (edited[markerIndex].Name.IsValid() && !duplicate) replaceMarkers(std::move(edited));
				}
				ImGuiStorage &drafts = *ImGui::GetStateStorage();
				const ImGuiID timeItem = ImGui::GetID("marker time");
				float time = DraftFloat(
					drafts, selected, UiAuthoringDraftEpoch, timeItem, 0, markers[markerIndex].Time
				);
				ImGui::SliderFloat("marker time", &time, 0.0f, 1.0f);
				if (FinishDraft(drafts, selected, UiAuthoringDraftEpoch, timeItem, {&time, 1})) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					edited[markerIndex].Time = time;
					replaceMarkers(std::move(edited));
				}
				if (ImGui::SmallButton("remove marker")) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					edited.erase(edited.begin() + static_cast<ptrdiff_t>(markerIndex));
					replaceMarkers(std::move(edited));
				}
				ImGui::SameLine();
				if (markerIndex > 0 && ImGui::SmallButton("move marker earlier")) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					std::swap(edited[markerIndex].Time, edited[markerIndex - 1].Time);
					replaceMarkers(std::move(edited));
				}
				ImGui::SameLine();
				if (markerIndex + 1 < markers.size() && ImGui::SmallButton("move marker later")) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					std::swap(edited[markerIndex].Time, edited[markerIndex + 1].Time);
					replaceMarkers(std::move(edited));
				}
				ImGui::PopID();
			}
			std::array<char, 64> markerDraft{};
			CopyText(markerDraft.data(), markerDraft.size(), UiAnimationMarkerDraft);
			if (ImGui::InputText("new marker", markerDraft.data(), markerDraft.size()))
				UiAnimationMarkerDraft = markerDraft.data();
			if (ImGui::Button("Add marker")) {
				const Name name(UiAnimationMarkerDraft);
				const bool duplicate = std::any_of(markers.begin(), markers.end(), [&](const auto &marker) {
					return marker.Name == name;
				});
				if (name.IsValid() && !duplicate) {
					auto edited = std::vector<engine::gui::AnimationMarker>(markers.begin(), markers.end());
					const float time = edited.empty() ? 0.0f : std::min(1.0f, edited.back().Time + 0.1f);
					edited.push_back({name, time});
					replaceMarkers(std::move(edited));
				}
			}
		}

		if (ImGui::CollapsingHeader("Localization", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (const auto localeKey =
					editText("catalogue key", summary.LabelPresentation.LocalizationKey.Text())) {
				engine::game::PropertyValue value;
				value.Type = engine::ecs::PropertyType::Name;
				value.Name = Name(*localeKey);
				propertyEdit = {Name("LocalizationKey"), std::move(value), "Set UI localization key"};
			}
			if (!summary.LabelPresentation.LocalizationKey.IsValid()) ImGui::TextDisabled("uses label text");
			if (summary.HasLocalizedArguments) {
				auto edited = [&]() -> engine::gui::LabelLocalizationArguments & {
					if (!localizedArgumentsEdit) localizedArgumentsEdit = summary.LocalizedArguments;
					return *localizedArgumentsEdit;
				};
				for (size_t index = 0; index < summary.LocalizedArguments.Count; index++) {
					const auto &argument = summary.LocalizedArguments.Values[index];
					ImGui::PushID(static_cast<int>(index));
					ImGui::Text("argument %zu", index + 1);
					ImGui::SameLine();
					if (ImGui::SmallButton("remove")) {
						auto &values = edited();
						for (size_t next = index + 1; next < values.Count; next++)
							values.Values[next - 1] = std::move(values.Values[next]);
						values.Values[--values.Count] = {};
						ImGui::PopID();
						break;
					}
					if (const auto name = editText("name", argument.Name.Text())) {
						const Name next(*name);
						bool duplicate = !next.IsValid();
						for (size_t other = 0; other < summary.LocalizedArguments.Count; other++)
							duplicate = duplicate || (other != index &&
													  summary.LocalizedArguments.Values[other].Name == next);
						if (!duplicate) edited().Values[index].Name = next;
					}
					int type = static_cast<int>(argument.Type);
					if (ImGui::Combo("type", &type, "String\0Number\0Date\0"))
						edited().Values[index].Type = static_cast<engine::gui::LocalizedArgumentType>(type);
					if (argument.Type == engine::gui::LocalizedArgumentType::String) {
						if (const auto value = editText("value", argument.String)) {
							if (value->size() <=
								engine::gui::LabelLocalizationArguments::MAXIMUM_STRING_BYTES)
								edited().Values[index].String = *value;
						}
					} else {
						const std::string current =
							argument.Type == engine::gui::LocalizedArgumentType::Number
								? std::to_string(argument.Number)
								: std::to_string(argument.UnixSeconds);
						if (const auto value = editText("value", current)) {
							if (argument.Type == engine::gui::LocalizedArgumentType::Number) {
								double parsed = 0.0;
								const auto result =
									std::from_chars(value->data(), value->data() + value->size(), parsed);
								if (result.ec == std::errc{} && result.ptr == value->data() + value->size() &&
									std::isfinite(parsed))
									edited().Values[index].Number = parsed;
							} else {
								int64_t parsed = 0;
								const auto result =
									std::from_chars(value->data(), value->data() + value->size(), parsed);
								if (result.ec == std::errc{} && result.ptr == value->data() + value->size())
									edited().Values[index].UnixSeconds = parsed;
							}
						}
					}
					ImGui::PopID();
				}
				if (summary.LocalizedArguments.Count <
						engine::gui::LabelLocalizationArguments::MAXIMUM_ARGUMENTS &&
					ImGui::Button("Add string argument")) {
					auto &values = edited();
					const size_t index = values.Count++;
					values.Values[index] = {
						Name("argument" + std::to_string(index + 1)),
						engine::gui::LocalizedArgumentType::String,
						""
					};
				}
			}
		}

		if (ImGui::CollapsingHeader("Accessibility and diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
			if (!hasList) {
				ImGui::TextDisabled("focus a viewport showing this world to inspect compiled output");
			} else {
				ImGui::Text("draw commands: %zu", commandCount);
				ImGui::Text(
					"semantic nodes: %zu%s", semanticNodes.size(), semanticTruncated ? " (truncated)" : ""
				);
				ImGui::Text(
					"accessibility issues: %zu%s",
					semanticAudit.Issues.size(),
					semanticAudit.Truncated ? " (truncated)" : ""
				);
				for (const auto &issue : semanticAudit.Issues) {
					if (issue.Instance != selected) continue;
					const char *description = "missing name";
					if (issue.Kind == engine::gui::SemanticIssueKind::SmallTarget)
						description = "small interaction target";
					else if (issue.Kind == engine::gui::SemanticIssueKind::DuplicateControlName)
						description = "duplicate control name";
					ImGui::BulletText("%s", description);
				}
				const auto node =
					std::find_if(semanticNodes.begin(), semanticNodes.end(), [&](const auto &value) {
						return value.Instance == selected;
					});
				if (node != semanticNodes.end()) {
					ImGui::Text("role: %s", SemanticRoleName(node->Role));
					ImGui::Text("name: %s", node->Name.c_str());
					ImGui::Text("%s", node->Focused ? "focused" : "not focused");
				} else {
					ImGui::TextDisabled("selected instance has no visible semantic node");
				}
			}
		}

		if (propertyEdit && Commands != nullptr &&
			AuthorityOf(SelectionWorld) == EditAuthority::Authoritative) {
			bool wrote = false;
			Universe->Enter(SelectionWorld, [&](Store &store) {
				const auto *property = PropertyOf(store, selected, propertyEdit->Property);
				engine::game::PropertyValue before;
				if (property == nullptr || !engine::game::ReadProperty(store, selected, *property, before) ||
					!engine::game::WriteAuthoredProperty(store, selected, *property, propertyEdit->Value))
					return;
				engine::game::PropertyValue after;
				if (!engine::game::ReadProperty(store, selected, *property, after) ||
					engine::game::ValuesEqual(before, after))
					return;
				Commands->RecordProperty(
					SelectionWorld, selected, propertyEdit->Property, before, after, propertyEdit->Description
				);
				wrote = true;
			});
			if (wrote) MarkModified();
		}

		const auto replaceComponent = [&](auto component, std::string_view description) {
			if (Commands == nullptr || AuthorityOf(SelectionWorld) != EditAuthority::Authoritative) return;
			const auto recording = Commands->TryBeginRecording(std::string(description));
			if (!recording) return;
			bool wrote = false;
			Universe->Enter(SelectionWorld, [&](Store &store) {
				if (!store.Alive(selected)) return;
				using Component = decltype(component);
				const Component *before = store.Get<Component>(selected);
				const auto id = engine::ecs::Components::Of<Component>();
				const auto &type = engine::ecs::Components::Describe(id);
				if (!type.Serialisable || type.Write == nullptr) return;
				engine::core::ByteWriter beforeWriter;
				engine::core::ByteWriter afterWriter;
				if (before == nullptr) return;
				type.Write(beforeWriter, before, 1);
				type.Write(afterWriter, &component, 1);
				if (beforeWriter.Bytes().size() == afterWriter.Bytes().size() &&
					std::equal(
						beforeWriter.Bytes().begin(), beforeWriter.Bytes().end(), afterWriter.Bytes().begin()
					))
					return;
				store.Set(selected, component);
				Commands->RecordComponent(
					SelectionWorld,
					selected,
					type.Name,
					std::vector<std::byte>(beforeWriter.Bytes().begin(), beforeWriter.Bytes().end()),
					std::vector<std::byte>(afterWriter.Bytes().begin(), afterWriter.Bytes().end()),
					std::string(description)
				);
				wrote = true;
			});
			Commands->FinishRecording(*recording, wrote ? FinishOperation::Commit : FinishOperation::Cancel);
			if (wrote) MarkModified();
		};
		if (themeEdit) replaceComponent(*themeEdit, "Edit UI theme token");
		if (animationEdit) replaceComponent(*animationEdit, "Edit UI animation keyframe");
		if (styleClassEdit) replaceComponent(*styleClassEdit, "Edit UI style classes");
		if (styleEdit) replaceComponent(*styleEdit, "Edit UI style rule");
		if (localizedArgumentsEdit) replaceComponent(*localizedArgumentsEdit, "Edit UI localized argument");
		if (extractRepeatedDirect && Commands != nullptr &&
			AuthorityOf(SelectionWorld) == EditAuthority::Authoritative) {
			const auto recording = Commands->TryBeginRecording("Extract repeated UI style values");
			bool wrote = false;
			if (recording) {
				Universe->Enter(SelectionWorld, [&](Store &store) {
					const auto *binding = store.Get<engine::gui::ThemeBinding>(selected);
					if (binding == nullptr || !store.Alive(binding->Theme)) {
						UiDocumentStatus = "assign a UI theme before extracting values";
						return;
					}
					const auto *theme = store.Get<engine::gui::UITheme>(binding->Theme);
					if (theme == nullptr) {
						UiDocumentStatus = "the assigned UI theme is unavailable";
						return;
					}
					bool sharedTheme = false;
					store.Each<const engine::gui::ThemeBinding>([&](Entity entity,
																	const engine::gui::ThemeBinding &other) {
						if (entity != selected && other.Theme == binding->Theme) sharedTheme = true;
					});
					if (sharedTheme) {
						UiDocumentStatus = "the assigned UI theme is also bound outside this collector";
						return;
					}
					std::vector<Entity> descendants;
					store.Each<const engine::gui::Element>([&](Entity entity, const engine::gui::Element &) {
						if (entity != selected && !store.IsDescendantOf(entity, selected)) return;
						for (Entity parent = store.ParentOf(entity);
							 parent != NULL_ENTITY && parent != selected;
							 parent = store.ParentOf(parent))
							if (store.IsA(parent, engine::gui::GuiClass("LayerCollector"))) return;
						descendants.push_back(entity);
					});
					std::vector<ThemeExtraction> extractions;
					for (const auto &[tokenText, property] : EXTRACTION_PROPERTIES) {
						const Name token(tokenText);
						std::vector<engine::gui::StyleValue> values;
						for (const Entity entity : descendants) {
							engine::gui::StyleSet direct;
							engine::gui::CollectDirectStyleValues(store, entity, direct);
							if (const auto *value = direct.Find(token); value != nullptr)
								values.push_back(*value);
						}
						for (const auto &candidate : values) {
							const size_t matches = static_cast<size_t>(
								std::count_if(values.begin(), values.end(), [&](const auto &value) {
									return SameStyleValue(value, candidate);
								})
							);
							if (matches < 2) continue;
							bool styleTypeConflict = false;
							for (const Entity entity : descendants) {
								store.EachChild(entity, [&](Entity child) {
									const auto *style = store.Get<engine::gui::UIStyle>(child);
									const auto *declaration =
										style != nullptr ? style->Rule.Declarations.Find(token) : nullptr;
									if (declaration != nullptr && declaration->Type != candidate.Type)
										styleTypeConflict = true;
								});
							}
							if (styleTypeConflict) {
								UiDocumentStatus =
									"a UIStyle declaration conflicts with the extracted token type";
								return;
							}
							const auto *existing = theme->Tokens.Find(token);
							if (existing != nullptr && !SameStyleValue(*existing, candidate)) {
								UiDocumentStatus = "theme token conflicts with repeated direct value";
								return;
							}
							bool unsafeInheritance = false;
							for (const Entity entity : descendants) {
								if (!OwnsStyleProperty(store, entity, property)) continue;
								bool authoredRule = false;
								store.EachChild(entity, [&](Entity child) {
									const auto *style = store.Get<engine::gui::UIStyle>(child);
									if (style != nullptr && style->Rule.Declarations.Find(token) != nullptr)
										authoredRule = true;
								});
								if (authoredRule) unsafeInheritance = true;
								engine::gui::StyleSet direct;
								engine::gui::CollectDirectStyleValues(store, entity, direct);
								if (direct.Find(token) == nullptr) unsafeInheritance = true;
							}
							if (unsafeInheritance) {
								UiDocumentStatus = "a descendant would inherit the extracted theme value";
								return;
							}
							extractions.push_back({token, candidate, property});
							break;
						}
					}
					if (extractions.empty()) {
						UiDocumentStatus = "no safe repeated direct values to extract";
						return;
					}
					engine::gui::UITheme changedTheme = *theme;
					for (const auto &extraction : extractions)
						if (!changedTheme.Tokens.Set({extraction.Token, extraction.Value})) return;
					bool recordFailed = false;
					const auto record = [&](Entity entity, const auto &before, const auto &after) {
						using Component = std::decay_t<decltype(before)>;
						const auto id = engine::ecs::Components::Of<Component>();
						const auto &type = engine::ecs::Components::Describe(id);
						if (!type.Serialisable || type.Write == nullptr) {
							recordFailed = true;
							return false;
						}
						engine::core::ByteWriter beforeBytes;
						engine::core::ByteWriter afterBytes;
						type.Write(beforeBytes, &before, 1);
						type.Write(afterBytes, &after, 1);
						if (beforeBytes.Bytes().size() == afterBytes.Bytes().size() &&
							std::equal(
								beforeBytes.Bytes().begin(),
								beforeBytes.Bytes().end(),
								afterBytes.Bytes().begin()
							))
							return true;
						store.Set(entity, after);
						Commands->RecordComponent(
							SelectionWorld,
							entity,
							type.Name,
							{beforeBytes.Bytes().begin(), beforeBytes.Bytes().end()},
							{afterBytes.Bytes().begin(), afterBytes.Bytes().end()},
							"Extract repeated UI style values"
						);
						return true;
					};
					if (!record(binding->Theme, *theme, changedTheme)) return;
					for (const Entity entity : descendants) {
						engine::gui::StyleSet direct;
						engine::gui::CollectDirectStyleValues(store, entity, direct);
						engine::gui::StyleDirect mask = store.Get<engine::gui::StyleDirect>(entity) != nullptr
															? *store.Get<engine::gui::StyleDirect>(entity)
															: engine::gui::StyleDirect{};
						bool changed = false;
						for (const auto &extraction : extractions) {
							const auto *value = direct.Find(extraction.Token);
							if (value == nullptr || !SameStyleValue(*value, extraction.Value)) continue;
							mask.Properties &= ~static_cast<uint8_t>(extraction.Property);
							changed = true;
							if (extraction.Property == engine::gui::StyleDirectProperty::BackgroundColor) {
								if (const auto *before = store.Get<engine::gui::Background>(entity)) {
									auto after = *before;
									after.Color = engine::gui::Background{}.Color;
									record(entity, *before, after);
								}
							} else if (extraction.Property ==
									   engine::gui::StyleDirectProperty::BackgroundTransparency) {
								if (const auto *before = store.Get<engine::gui::Background>(entity)) {
									auto after = *before;
									after.Transparency = engine::gui::Background{}.Transparency;
									record(entity, *before, after);
								}
							} else if (extraction.Property == engine::gui::StyleDirectProperty::TextColor) {
								if (const auto *before = store.Get<engine::gui::Label>(entity)) {
									auto after = *before;
									after.Color = engine::gui::Label{}.Color;
									record(entity, *before, after);
								}
							} else if (extraction.Property ==
									   engine::gui::StyleDirectProperty::TextTransparency) {
								if (const auto *before = store.Get<engine::gui::Label>(entity)) {
									auto after = *before;
									after.Transparency = engine::gui::Label{}.Transparency;
									record(entity, *before, after);
								}
							} else if (extraction.Property == engine::gui::StyleDirectProperty::ImageColor) {
								if (const auto *before = store.Get<engine::gui::Picture>(entity)) {
									auto after = *before;
									after.Color = engine::gui::Picture{}.Color;
									record(entity, *before, after);
								}
							} else if (const auto *before = store.Get<engine::gui::Picture>(entity)) {
								auto after = *before;
								after.Transparency = engine::gui::Picture{}.Transparency;
								record(entity, *before, after);
							}
						}
						if (changed) {
							if (store.Get<engine::gui::StyleDirect>(entity) != nullptr)
								record(entity, *store.Get<engine::gui::StyleDirect>(entity), mask);
						}
					}
					if (recordFailed) {
						UiDocumentStatus = "could not record every extracted style change";
						return;
					}
					wrote = true;
					UiDocumentStatus = "repeated direct values extracted into the assigned theme";
				});
				Commands->FinishRecording(
					*recording, wrote ? FinishOperation::Commit : FinishOperation::Cancel
				);
				if (wrote) MarkModified();
			}
		}
		ImGui::End();
	}
}
