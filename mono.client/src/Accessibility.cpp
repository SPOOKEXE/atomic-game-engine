#include <SDL3/SDL.h>

#include <accesskit.h>
#include <algorithm>
#include <client/Accessibility.hpp>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace client {
	namespace {
		using engine::ecs::Entity;
		using engine::gui::SemanticAction;
		using engine::gui::SemanticActionSet;
		using engine::gui::SemanticNode;
		using engine::gui::SemanticRole;

		constexpr accesskit_node_id ROOT = 0;

		bool Has(SemanticActionSet actions, SemanticActionSet action) {
			return (static_cast<uint8_t>(actions) & static_cast<uint8_t>(action)) != 0;
		}

		accesskit_node_id NodeId(uint64_t epoch, Entity collector, Entity instance) {
			uint64_t value = epoch ^ (collector.Id + 0x9e3779b97f4a7c15ULL + (epoch << 6U) + (epoch >> 2U));
			value ^= instance.Id + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
			return value == ROOT ? 1 : value;
		}

		accesskit_role Role(const SemanticNode &node) {
			switch (node.Role) {
			case SemanticRole::Text:
				return ACCESSKIT_ROLE_LABEL;
			case SemanticRole::Button:
				return ACCESSKIT_ROLE_BUTTON;
			case SemanticRole::TextField:
				return node.Password ? ACCESSKIT_ROLE_PASSWORD_INPUT : ACCESSKIT_ROLE_TEXT_INPUT;
			case SemanticRole::Image:
				return ACCESSKIT_ROLE_IMAGE;
			case SemanticRole::Group:
				return ACCESSKIT_ROLE_GROUP;
			}
			return ACCESSKIT_ROLE_GROUP;
		}
	}

	struct AccessibilityAdapter::State {
		std::mutex Mutex;
		engine::gui::SemanticSnapshot Snapshot;
		uint64_t Epoch = 0;
		struct Target {
			Entity Instance;
			Entity Collector;
		};
		std::unordered_map<accesskit_node_id, Target> Targets;
		std::vector<accesskit_node_id> NodeIds;
		std::vector<AccessibilityAction> Pending;
#if defined(__linux__)
		accesskit_unix_adapter *Adapter = nullptr;
#elif defined(_WIN32)
		accesskit_windows_subclassing_adapter *Adapter = nullptr;
#elif defined(__APPLE__)
		accesskit_macos_subclassing_adapter *Adapter = nullptr;
#endif
	};

	namespace {
		accesskit_tree_update *BuildTree(void *userdata) {
			auto &state = *static_cast<AccessibilityAdapter::State *>(userdata);
			std::scoped_lock lock(state.Mutex);
			const size_t capacity = state.Snapshot.Nodes.size() + 1;
			accesskit_node_id focus = ROOT;
			for (size_t index = 0; index < state.Snapshot.Nodes.size(); ++index) {
				if (state.Snapshot.Nodes[index].Focused) focus = state.NodeIds[index];
			}
			auto *update = accesskit_tree_update_with_capacity_and_focus(capacity, focus);
			auto *tree = accesskit_tree_info_new(ROOT);
			accesskit_tree_info_set_toolkit_name(tree, "Atomic Game Engine");
			accesskit_tree_update_set_tree_info(update, tree);
			auto *root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
			std::unordered_map<uint64_t, size_t> indexes;
			indexes.reserve(state.Snapshot.Nodes.size());
			std::vector<std::vector<size_t>> children(state.Snapshot.Nodes.size());
			for (size_t index = 0; index < state.Snapshot.Nodes.size(); ++index) {
				indexes.emplace(state.Snapshot.Nodes[index].Instance.Id, index);
			}
			for (size_t index = 0; index < state.Snapshot.Nodes.size(); ++index) {
				const SemanticNode &node = state.Snapshot.Nodes[index];
				const auto parent = indexes.find(node.Parent.Id);
				if (parent == indexes.end()) {
					accesskit_node_push_child(root, state.NodeIds[index]);
				} else {
					children[parent->second].push_back(index);
				}
			}
			accesskit_tree_update_push_node(update, ROOT, root);
			for (size_t index = 0; index < state.Snapshot.Nodes.size(); ++index) {
				const SemanticNode &semantic = state.Snapshot.Nodes[index];
				const accesskit_node_id id = state.NodeIds[index];
				auto *node = accesskit_node_new(Role(semantic));
				accesskit_node_set_bounds(
					node,
					{semantic.Bounds.Min.X,
					 semantic.Bounds.Min.Y,
					 semantic.Bounds.Max.X,
					 semantic.Bounds.Max.Y}
				);
				if (!semantic.Name.empty())
					accesskit_node_set_label_with_length(node, semantic.Name.data(), semantic.Name.size());
				if (!semantic.Value.empty())
					accesskit_node_set_value_with_length(node, semantic.Value.data(), semantic.Value.size());
				if (!semantic.Description.empty())
					accesskit_node_set_description_with_length(
						node, semantic.Description.data(), semantic.Description.size()
					);
				if (!semantic.Language.empty())
					accesskit_node_set_language_with_length(
						node, semantic.Language.data(), semantic.Language.size()
					);
				if (!semantic.Enabled) accesskit_node_set_disabled(node);
				if (semantic.Selected) accesskit_node_set_selected(node, true);
				if (semantic.Editable) accesskit_node_add_action(node, ACCESSKIT_ACTION_FOCUS);
				if (Has(semantic.Actions, SemanticActionSet::Activate))
					accesskit_node_add_action(node, ACCESSKIT_ACTION_CLICK);
				for (const size_t child : children[index]) {
					accesskit_node_push_child(node, state.NodeIds[child]);
				}
				accesskit_tree_update_push_node(update, id, node);
			}
			return update;
		}

		accesskit_tree_update *Activated(void *userdata) {
			return BuildTree(userdata);
		}

		void Action(accesskit_action_request *request, void *userdata) {
			auto &state = *static_cast<AccessibilityAdapter::State *>(userdata);
			if (request->action == ACCESSKIT_ACTION_CLICK || request->action == ACCESSKIT_ACTION_FOCUS) {
				std::scoped_lock lock(state.Mutex);
				const auto found = state.Targets.find(request->target_node);
				if (found != state.Targets.end() && state.Pending.size() < 1024) {
					state.Pending.push_back({
						.Target = found->second.Instance,
						.Collector = found->second.Collector,
						.WorldEpoch = state.Epoch,
						.Action = request->action == ACCESSKIT_ACTION_CLICK ? SemanticAction::Activate
																			: SemanticAction::Focus,
					});
				}
			}
			accesskit_action_request_free(request);
		}

		void Deactivated(void *) {}
	}

	AccessibilityAdapter::AccessibilityAdapter(SDL_Window *window) : Data(std::make_unique<State>()) {
#if defined(__linux__)
		(void)window;
		Data->Adapter =
			accesskit_unix_adapter_new(Activated, Data.get(), Action, Data.get(), Deactivated, Data.get());
#elif defined(_WIN32)
		const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
		HWND hwnd = static_cast<HWND>(
			SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
		);
		if (hwnd != nullptr) {
			Data->Adapter =
				accesskit_windows_subclassing_adapter_new(hwnd, Activated, Data.get(), Action, Data.get());
		}
#elif defined(__APPLE__)
		const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
		void *nativeWindow =
			SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
		if (nativeWindow != nullptr) {
			accesskit_macos_add_focus_forwarder_to_window_class("SDLWindow");
			Data->Adapter = accesskit_macos_subclassing_adapter_for_window(
				nativeWindow, Activated, Data.get(), Action, Data.get()
			);
		}
#endif
	}

	AccessibilityAdapter::~AccessibilityAdapter() {
#if defined(__linux__)
		if (Data->Adapter != nullptr) accesskit_unix_adapter_free(Data->Adapter);
#elif defined(_WIN32)
		if (Data->Adapter != nullptr) accesskit_windows_subclassing_adapter_free(Data->Adapter);
#elif defined(__APPLE__)
		if (Data->Adapter != nullptr) accesskit_macos_subclassing_adapter_free(Data->Adapter);
#endif
	}

	void AccessibilityAdapter::Update(const engine::gui::SemanticSnapshot &snapshot, uint64_t worldEpoch) {
		{
			std::scoped_lock lock(Data->Mutex);
			if (Data->Epoch != worldEpoch) Data->Pending.clear();
			Data->Snapshot = snapshot;
			if (Data->Snapshot.Nodes.size() > 65536) Data->Snapshot.Nodes.resize(65536);
			Data->Epoch = worldEpoch;
			Data->Targets.clear();
			Data->NodeIds.clear();
			Data->NodeIds.reserve(Data->Snapshot.Nodes.size());
			std::vector<SemanticNode> accepted;
			accepted.reserve(Data->Snapshot.Nodes.size());
			for (const SemanticNode &node : Data->Snapshot.Nodes) {
				accesskit_node_id id = NodeId(worldEpoch, node.Collector, node.Instance);
				// Linear probing is bounded and deterministic for this snapshot. A hash
				// collision can change an id when the semantic order changes, but never
				// routes a callback to a different current collector or world.
				for (size_t attempts = 0; Data->Targets.contains(id) && attempts < 65536; ++attempts) {
					id = id == UINT64_MAX ? 1 : id + 1;
				}
				if (Data->Targets.contains(id)) {
					Data->Snapshot.Truncated = true;
					break;
				}
				Data->Targets.emplace(id, State::Target{node.Instance, node.Collector});
				Data->NodeIds.push_back(id);
				accepted.push_back(node);
			}
			Data->Snapshot.Nodes = std::move(accepted);
		}
#if defined(__linux__)
		if (Data->Adapter != nullptr)
			accesskit_unix_adapter_update_if_active(Data->Adapter, BuildTree, Data.get());
#elif defined(_WIN32)
		if (Data->Adapter != nullptr) {
			if (auto *events = accesskit_windows_subclassing_adapter_update_if_active(
					Data->Adapter, BuildTree, Data.get()
				);
				events != nullptr)
				accesskit_windows_queued_events_raise(events);
		}
#elif defined(__APPLE__)
		if (Data->Adapter != nullptr) {
			if (auto *events = accesskit_macos_subclassing_adapter_update_if_active(
					Data->Adapter, BuildTree, Data.get()
				);
				events != nullptr)
				accesskit_macos_queued_events_raise(events);
		}
#endif
	}

	void AccessibilityAdapter::SetWindowFocused(bool focused) {
#if defined(__linux__)
		if (Data->Adapter != nullptr)
			accesskit_unix_adapter_update_window_focus_state(Data->Adapter, focused);
#elif defined(__APPLE__)
		if (Data->Adapter != nullptr) {
			if (auto *events =
					accesskit_macos_subclassing_adapter_update_view_focus_state(Data->Adapter, focused);
				events != nullptr)
				accesskit_macos_queued_events_raise(events);
		}
#else
		(void)focused;
#endif
	}

	std::vector<AccessibilityAction> AccessibilityAdapter::Drain() {
		std::scoped_lock lock(Data->Mutex);
		return std::exchange(Data->Pending, {});
	}
}
