#include "AccessibilityOrigins.hpp"

#include <engine/core/Log.hpp>

#include <SDL3/SDL.h>

#include <accesskit.h>
#include <algorithm>
#include <client/Accessibility.hpp>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace client {
	void ReportAccessibilityRejection(const AccessibilityAction &action, const char *reason) {
		ENGINE_WARN(
			"native accessibility action rejected: {} (status {}, node {}, frame {}, count {}, action {})",
			reason,
			static_cast<unsigned>(action.Status),
			action.NativeNodeId,
			action.FrameSequence,
			action.RejectedCount,
			action.NativeAction
		);
	}

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
		AccessibilityTree Tree;
		AccessibilityRequests Requests;
		// Caller holds Mutex so callbacks cannot observe a partial transition.
		bool Publish(
			const engine::gui::SemanticSnapshot &snapshot,
			uint64_t worldEpoch,
			uint64_t frameSequence,
			bool reuse
		) {
			const bool modeChanged = Tree.ChangesMode(frameSequence);
			if (!Tree.Publish(snapshot, worldEpoch, frameSequence, reuse)) return false;
			if (modeChanged) Requests.Retire();
			return true;
		}
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
			const auto &nodeIds = state.Tree.Origins().CurrentIds();
			const size_t capacity = state.Tree.Snapshot().Nodes.size() + 1;
			accesskit_node_id focus = ROOT;
			for (size_t index = 0; index < state.Tree.Snapshot().Nodes.size(); ++index) {
				if (state.Tree.Snapshot().Nodes[index].Focused) focus = nodeIds[index];
			}
			auto *update = accesskit_tree_update_with_capacity_and_focus(capacity, focus);
			auto *tree = accesskit_tree_info_new(ROOT);
			accesskit_tree_info_set_toolkit_name(tree, "Atomic Game Engine");
			accesskit_tree_update_set_tree_info(update, tree);
			auto *root = accesskit_node_new(ACCESSKIT_ROLE_WINDOW);
			std::unordered_map<uint64_t, size_t> indexes;
			indexes.reserve(state.Tree.Snapshot().Nodes.size());
			std::vector<std::vector<size_t>> children(state.Tree.Snapshot().Nodes.size());
			for (size_t index = 0; index < state.Tree.Snapshot().Nodes.size(); ++index) {
				indexes.emplace(state.Tree.Snapshot().Nodes[index].Instance.Id, index);
			}
			for (size_t index = 0; index < state.Tree.Snapshot().Nodes.size(); ++index) {
				const SemanticNode &node = state.Tree.Snapshot().Nodes[index];
				const auto parent = indexes.find(node.Parent.Id);
				if (parent == indexes.end()) {
					accesskit_node_push_child(root, nodeIds[index]);
				} else {
					children[parent->second].push_back(index);
				}
			}
			accesskit_tree_update_push_node(update, ROOT, root);
			for (size_t index = 0; index < state.Tree.Snapshot().Nodes.size(); ++index) {
				const SemanticNode &semantic = state.Tree.Snapshot().Nodes[index];
				const accesskit_node_id id = nodeIds[index];
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
					accesskit_node_push_child(node, nodeIds[child]);
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
			{
				std::scoped_lock lock(state.Mutex);
				if (request->action == ACCESSKIT_ACTION_CLICK || request->action == ACCESSKIT_ACTION_FOCUS) {
					const auto action = request->action == ACCESSKIT_ACTION_CLICK ? SemanticAction::Activate
																				  : SemanticAction::Focus;
					auto resolved = state.Tree.Origins().Resolve(request->target_node, action);
					resolved.NativeAction = static_cast<uint32_t>(request->action);
					state.Requests.Push(resolved);
				} else {
					AccessibilityAction unsupported;
					unsupported.Status = AccessibilityActionStatus::Unsupported;
					unsupported.NativeNodeId = request->target_node;
					unsupported.NativeAction = static_cast<uint32_t>(request->action);
					state.Requests.Push(unsupported);
				}
			}
			accesskit_action_request_free(request);
		}

		void Deactivated(void *) {}

		void PublishNativeTree(AccessibilityAdapter::State &state) {
#if defined(__linux__)
			if (state.Adapter != nullptr)
				accesskit_unix_adapter_update_if_active(state.Adapter, BuildTree, &state);
#elif defined(_WIN32)
			if (state.Adapter != nullptr) {
				if (auto *events = accesskit_windows_subclassing_adapter_update_if_active(
						state.Adapter, BuildTree, &state
					);
					events != nullptr)
					accesskit_windows_queued_events_raise(events);
			}
#elif defined(__APPLE__)
			if (state.Adapter != nullptr) {
				if (auto *events = accesskit_macos_subclassing_adapter_update_if_active(
						state.Adapter, BuildTree, &state
					);
					events != nullptr)
					accesskit_macos_queued_events_raise(events);
			}
#endif
		}
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
		if (!Update(snapshot, worldEpoch, 0, true)) {
			Retire();
			ENGINE_WARN("native accessibility tree update refused");
		}
	}

	bool AccessibilityAdapter::Update(
		const engine::gui::SemanticSnapshot &snapshot,
		uint64_t worldEpoch,
		uint64_t frameSequence,
		bool reuseProven
	) {
		{
			std::scoped_lock lock(Data->Mutex);
			if (!Data->Publish(snapshot, worldEpoch, frameSequence, reuseProven)) return false;
		}
		PublishNativeTree(*Data);
		return true;
	}

	void AccessibilityAdapter::Retire() {
		{
			std::scoped_lock lock(Data->Mutex);
			Data->Tree.Retire();
			Data->Requests.Retire();
		}
		PublishNativeTree(*Data);
	}

	bool AccessibilityAdapter::HasLegacyTree(uint64_t worldEpoch) const {
		std::scoped_lock lock(Data->Mutex);
		return Data->Tree.HasLegacyTree(worldEpoch);
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
		return Data->Requests.Drain();
	}
}
