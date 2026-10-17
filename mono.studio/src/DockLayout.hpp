#pragma once

// Recovers hosted panel slots while retaining panels placed outside Studio.

#include <imgui.h>
#include <imgui_internal.h>
#include <span>

namespace studio::detail {

	inline ImGuiID FirstDockLeaf(ImGuiDockNode *node) {
		if (node == nullptr) return 0;
		while (!node->IsLeafNode()) {
			node = node->ChildNodes[0] != nullptr ? node->ChildNodes[0] : node->ChildNodes[1];
			if (node == nullptr) return 0;
		}
		return node->ID;
	}

	inline ImGuiID PanelDockId(const char *label) {
		if (const ImGuiWindow *window = ImGui::FindWindowByName(label)) return window->DockId;
		if (const ImGuiWindowSettings *settings = ImGui::FindWindowSettingsByID(ImHashStr(label)))
			return settings->DockId;
		return 0;
	}

	// Prefer a surviving peer in this dockspace. Closed peers still have ini entries.
	inline ImGuiID PanelDockHome(ImGuiID dockspace, std::span<const char *const> peers) {
		for (const char *label : peers) {
			ImGuiDockNode *node = ImGui::DockBuilderGetNode(PanelDockId(label));
			if (node != nullptr && ImGui::DockNodeGetRootNode(node)->ID == dockspace)
				return FirstDockLeaf(node);
		}
		if (ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(dockspace))
			return FirstDockLeaf(central);
		return FirstDockLeaf(ImGui::DockBuilderGetNode(dockspace));
	}

	inline bool PanelOutsideHost(
		const ImGuiWindow *window, const ImGuiWindowSettings *settings, const ImGuiDockNode *host
	) {
		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImVec2 position;
		ImVec2 size;
		if (window != nullptr) {
			if (window->Viewport == nullptr || window->Viewport->ID == main->ID) return false;
			position = window->Pos;
			size = window->Size;
		} else {
			if (settings == nullptr || settings->ViewportId == 0 || settings->ViewportId == main->ID)
				return false;
			position = ImVec2(
				static_cast<float>(settings->ViewportPos.x + settings->Pos.x),
				static_cast<float>(settings->ViewportPos.y + settings->Pos.y)
			);
			size = ImVec2(static_cast<float>(settings->Size.x), static_cast<float>(settings->Size.y));
		}
		const ImRect panel(position, ImVec2(position.x + size.x, position.y + size.y));
		const ImRect currentHost(main->Pos, ImVec2(main->Pos.x + main->Size.x, main->Pos.y + main->Size.y));
		// The saved host rectangle also covers a previous desktop position when
		// the window manager places Studio somewhere else on this launch.
		return !panel.Overlaps(currentHost) && !panel.Overlaps(host->Rect());
	}

	inline void RepairPanelDock(const char *label, ImGuiID home) {
		ImGuiDockNode *homeNode = ImGui::DockBuilderGetNode(home);
		if (homeNode == nullptr || !homeNode->IsLeafNode()) return;
		const ImGuiDockNode *homeRoot = ImGui::DockNodeGetRootNode(homeNode);
		const ImGuiWindowSettings *settings = ImGui::FindWindowSettingsByID(ImHashStr(label));
		const ImGuiWindow *window = ImGui::FindWindowByName(label);
		const ImGuiID dock = PanelDockId(label);
		if (PanelOutsideHost(window, settings, homeRoot)) return;
		ImGuiDockNode *node = ImGui::DockBuilderGetNode(dock);
		if (node != nullptr && node->IsLeafNode()) {
			const ImGuiDockNode *root = ImGui::DockNodeGetRootNode(node);
			if (root == homeRoot) return;
		}
		ImGui::DockBuilderDockWindow(label, home);
	}

	inline ImGuiDockNode *OccupiedDockLeaf(ImGuiDockNode *node) {
		if (node == nullptr) return nullptr;
		if (node->IsLeafNode()) {
			for (const ImGuiWindow *window : node->Windows)
				if (window->WasActive) return node;
			return nullptr;
		}
		if (ImGuiDockNode *occupied = OccupiedDockLeaf(node->ChildNodes[0])) return occupied;
		return OccupiedDockLeaf(node->ChildNodes[1]);
	}

	inline ImGuiDockNode *CentralDockLeaf(ImGuiDockNode *node) {
		if (node == nullptr) return nullptr;
		if (node->IsLeafNode()) return node->IsCentralNode() ? node : nullptr;
		if (ImGuiDockNode *central = CentralDockLeaf(node->ChildNodes[0])) return central;
		return CentralDockLeaf(node->ChildNodes[1]);
	}

	inline void FillEmptyCentralDock(ImGuiID dockspace) {
		ImGuiContext &context = *ImGui::GetCurrentContext();
		// Keep drop targets stable during a drag. ImGui applies the dock request
		// on the next frame, after the pointer has been released.
		if (context.MovingWindow != nullptr || context.DragDropActive) return;
		ImGuiDockNode *root = ImGui::DockBuilderGetNode(dockspace);
		if (root == nullptr || root->IsLeafNode()) return;
		// closing a split can free the cached CentralNode before dockspace updates it.
		ImGuiDockNode *central = CentralDockLeaf(root);
		if (central == nullptr || OccupiedDockLeaf(central) != nullptr) return;
		ImGuiDockNode *occupied = OccupiedDockLeaf(root);
		if (occupied == nullptr) return;
		// Central nodes remain visible even without windows. Transfer that role
		// so an empty old viewport slot cannot reserve a permanent blank region.
		central->SetLocalFlags(central->LocalFlags & ~ImGuiDockNodeFlags_CentralNode);
		occupied->SetLocalFlags(occupied->LocalFlags | ImGuiDockNodeFlags_CentralNode);
		root->CentralNode = occupied;
		ImGui::MarkIniSettingsDirty();
	}

	inline void SubmitStudioDockSpace(ImGuiID dockspace, const ImGuiViewport *viewport = nullptr) {
		// snap guides stay on host. a second copy moves inside dragged window.
		ImGui::GetIO().ConfigDockingTransparentPayload = true;
		FillEmptyCentralDock(dockspace);
		ImGui::DockSpaceOverViewport(dockspace, viewport);
	}
}
