#include <engine/core/Name.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Accessibility.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace engine::gui {
	namespace {
		using ecs::Entity;
		using ecs::Store;
		constexpr size_t MAXIMUM_SEMANTIC_SOURCES = 65536;
		constexpr size_t MAXIMUM_SEMANTIC_LANGUAGE_BYTES = 64;

		bool Has(SemanticActionSet actions, SemanticActionSet action) {
			return (static_cast<uint8_t>(actions) & static_cast<uint8_t>(action)) != 0;
		}

		bool VisibleBounds(const DrawCommand &command, core::Rect &bounds) {
			bounds.Min.X = std::max(command.Bounds.Min.X, command.Clip.Min.X);
			bounds.Min.Y = std::max(command.Bounds.Min.Y, command.Clip.Min.Y);
			bounds.Max.X = std::min(command.Bounds.Max.X, command.Clip.Max.X);
			bounds.Max.Y = std::min(command.Bounds.Max.Y, command.Clip.Max.Y);
			return bounds.Min.X < bounds.Max.X && bounds.Min.Y < bounds.Max.Y;
		}

	}

	SemanticAudit AuditSemantics(const SemanticSnapshot &snapshot, float minimumTargetPixels) {
		constexpr size_t MAXIMUM_ISSUES = 4096;
		SemanticAudit audit;
		audit.Truncated = snapshot.Truncated;
		if (!std::isfinite(minimumTargetPixels) || minimumTargetPixels < 0.0f) minimumTargetPixels = 44.0f;
		std::unordered_map<uint64_t, std::unordered_set<std::string>> namesByCollector;
		for (const SemanticNode &node : snapshot.Nodes) {
			const bool control = Has(node.Actions, SemanticActionSet::Activate) ||
								 Has(node.Actions, SemanticActionSet::Focus) ||
								 node.Role == SemanticRole::Button || node.Role == SemanticRole::TextField ||
								 (node.Role == SemanticRole::Group && node.Enabled);
			if (!control || !node.Enabled) continue;
			const auto issue = [&](SemanticIssueKind kind) {
				if (audit.Issues.size() == MAXIMUM_ISSUES) {
					audit.Truncated = true;
					return;
				}
				audit.Issues.push_back({kind, node.Instance, node.Collector});
			};
			if (node.Name.empty()) issue(SemanticIssueKind::MissingName);
			const float width = node.Bounds.Max.X - node.Bounds.Min.X;
			const float height = node.Bounds.Max.Y - node.Bounds.Min.Y;
			if (!std::isfinite(width) || !std::isfinite(height) || width < minimumTargetPixels ||
				height < minimumTargetPixels)
				issue(SemanticIssueKind::SmallTarget);
			if (!node.Name.empty() && !namesByCollector[node.Collector.Id].insert(node.Name).second)
				issue(SemanticIssueKind::DuplicateControlName);
		}
		return audit;
	}

	SemanticSnapshot CompileSemantics(const Store &store, const DrawList &list, std::string_view language) {
		RegisterGuiClasses();
		const ecs::ClassId button = GuiClass("GuiButton");
		const ecs::ClassId image = GuiClass("ImageLabel");
		const ecs::ClassId imageButton = GuiClass("ImageButton");
		const Entity service = GuiServiceOf(store);
		const GuiServiceState *state = store.Get<GuiServiceState>(service);
		const Entity selected = state != nullptr ? state->SelectedObject : ecs::NULL_ENTITY;
		const Entity focused = FocusedTextBox(store);

		SemanticSnapshot snapshot;
		std::vector<SemanticNode> &nodes = snapshot.Nodes;
		std::unordered_map<uint64_t, std::string_view> paintedText;
		std::unordered_map<uint64_t, const DrawCommand *> shapedText;
		for (const DrawCommand &command : list.Commands) {
			core::Rect bounds;
			if (command.Kind == DrawKind::Text && VisibleBounds(command, bounds) &&
				(paintedText.size() < MAXIMUM_SEMANTIC_SOURCES || paintedText.contains(command.Source.Id))) {
				paintedText.try_emplace(command.Source.Id, command.Text);
				shapedText.try_emplace(command.Source.Id, &command);
			}
		}
		std::unordered_set<uint64_t> seen;
		for (const DrawCommand &command : list.Commands) {
			core::Rect bounds;
			if (!VisibleBounds(command, bounds)) {
				continue;
			}
			if (!seen.contains(command.Source.Id) && seen.size() == MAXIMUM_SEMANTIC_SOURCES) {
				snapshot.Truncated = true;
				break;
			}
			if (!seen.insert(command.Source.Id).second) {
				continue;
			}
			const Element *element = store.Get<Element>(command.Source);
			if (element == nullptr || !element->Visible) {
				continue;
			}

			const Label *label = store.Get<Label>(command.Source);
			const Entry *entry = store.Get<Entry>(command.Source);
			SemanticRole role = SemanticRole::Group;
			if (entry != nullptr) {
				role = SemanticRole::TextField;
			} else if (store.IsA(command.Source, button)) {
				role = SemanticRole::Button;
			} else if (store.IsA(command.Source, image) || store.IsA(command.Source, imageButton)) {
				role = SemanticRole::Image;
			} else if (label != nullptr) {
				role = SemanticRole::Text;
			} else if (!element->Active && !element->Selectable) {
				continue;
			}

			SemanticNode node;
			node.Instance = command.Source;
			node.Collector = command.Collector;
			node.Role = role;
			node.ReadingOrder = static_cast<uint32_t>(nodes.size());
			if (language.size() <= MAXIMUM_SEMANTIC_LANGUAGE_BYTES) node.Language = language;
			const std::string instanceName(store.InstanceNameOf(command.Source).Text());
			if (role == SemanticRole::TextField) {
				node.Name = instanceName;
				node.Description = entry->PlaceholderText;
				node.Editable = entry->TextEditable;
				node.Password = entry->Password;
				node.Actions = SemanticActionSet::Focus;
				if (entry->TextEditable)
					node.Actions = static_cast<SemanticActionSet>(
						static_cast<uint8_t>(node.Actions) | static_cast<uint8_t>(SemanticActionSet::Edit)
					);
				if (!entry->Password && label != nullptr) node.Value = label->Text;
			} else {
				const auto text = paintedText.find(command.Source.Id);
				node.Name = text != paintedText.end() && !text->second.empty() ? std::string(text->second)
																			   : instanceName;
			}
			if (role == SemanticRole::Button) node.Actions = SemanticActionSet::Activate;
			if (role == SemanticRole::Group && element->Selectable) node.Actions = SemanticActionSet::Focus;
			if (const auto text = shapedText.find(command.Source.Id);
				text != shapedText.end() && !text->second->Shaping.Runs.empty()) {
				node.Direction = text->second->Shaping.Runs.front().RightToLeft
									 ? SemanticTextDirection::RightToLeft
									 : SemanticTextDirection::LeftToRight;
			}
			node.Bounds = bounds;
			node.Enabled = element->Interactable;
			node.Selected = command.Source == selected;
			node.Focused = command.Source == focused;
			nodes.push_back(std::move(node));
		}

		std::unordered_map<uint64_t, size_t> semanticIndex;
		for (size_t index = 0; index < nodes.size(); ++index) {
			semanticIndex.emplace(nodes[index].Instance.Id, index);
		}
		for (SemanticNode &node : nodes) {
			Entity ancestor = store.ParentOf(node.Instance);
			for (size_t depth = 0; depth < 256 && ancestor != ecs::NULL_ENTITY && ancestor != node.Collector;
				 ++depth, ancestor = store.ParentOf(ancestor)) {
				if (semanticIndex.contains(ancestor.Id)) {
					node.Parent = ancestor;
					break;
				}
			}
		}
		return snapshot;
	}
}
