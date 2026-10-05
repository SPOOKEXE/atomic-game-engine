#pragma once

#include "DockLayout.hpp"

#include <array>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>

namespace studio::detail {
	struct ImageComposerPanel {
		const char *Title;
		const char *Window;
	};

	inline constexpr std::array IMAGE_COMPOSER_PANELS{
		ImageComposerPanel{"Nodes", "Nodes###image-composer-nodes"},
		ImageComposerPanel{"Inspector", "Inspector###image-composer-inspector"},
		ImageComposerPanel{"Project", "Project###image-composer-project"},
		ImageComposerPanel{"Groups", "Groups###image-composer-groups"},
		ImageComposerPanel{"Preview", "Preview###image-composer-preview"},
		ImageComposerPanel{"Assets / Sinks", "Assets / Sinks###image-composer-assets"},
		ImageComposerPanel{"Font inputs", "Font inputs###image-composer-font-inputs"},
		ImageComposerPanel{"Timeline", "Timeline###image-composer-timeline"},
		ImageComposerPanel{"Diagnostics", "Diagnostics###image-composer-diagnostics"},
	};

	struct ImageComposerPanels {
		std::array<bool, IMAGE_COMPOSER_PANELS.size()> Open{
			true, true, false, false, false, false, false, false, false
		};
		bool LayoutInitialized = false;
		bool ResetRequested = false;
	};

	inline constexpr const char *IMAGE_COMPOSER_GRAPH = "Graph###image-composer-graph";

	inline ImGuiID ImageComposerDockspaceId() {
		return ImHashStr("ImageComposerDockspace.v1");
	}

	inline ImGuiWindowClass ImageComposerWindowClass() {
		ImGuiWindowClass windowClass;
		windowClass.ClassId = ImHashStr("ImageComposerWindows");
		windowClass.DockingAllowUnclassed = false;
		return windowClass;
	}

	// The first nested layout brings old Studio-docked panes inside their owner.
	// Detached panes and later saved choices retain their own homes.
	inline void InitializeImageComposerPanels(ImageComposerPanels &panels, ImVec2 position, ImVec2 size) {
		const ImGuiID dockspace = ImageComposerDockspaceId();
		ImGuiDockNode *root = ImGui::DockBuilderGetNode(dockspace);
		const bool placeholder = root != nullptr && root->IsLeafNode() &&
								 ImGui::FindWindowSettingsByID(ImHashStr(IMAGE_COMPOSER_GRAPH)) == nullptr;
		const bool firstNestedLayout = root == nullptr || placeholder;
		const bool rebuild = firstNestedLayout || panels.ResetRequested;
		if (panels.LayoutInitialized && !rebuild) return;
		ImGuiID graphHome = 0;
		ImGuiID panelHome = 0;
		if (rebuild) {
			ImGui::DockBuilderRemoveNode(dockspace);
			ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodePos(dockspace, position);
			ImGui::DockBuilderSetNodeSize(dockspace, size);
			panelHome = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Right, 0.28f, nullptr, &graphHome);
			root = ImGui::DockBuilderGetNode(dockspace);
		} else {
			graphHome = FirstDockLeaf(ImGui::DockBuilderGetCentralNode(dockspace));
			if (graphHome == 0) graphHome = FirstDockLeaf(root);
			for (const auto &panel : IMAGE_COMPOSER_PANELS) {
				ImGuiDockNode *node = ImGui::DockBuilderGetNode(PanelDockId(panel.Window));
				if (node != nullptr && node->IsLeafNode() &&
					ImGui::DockNodeGetRootNode(node)->ID == dockspace)
					panelHome = node->ID;
			}
			if (panelHome == 0)
				panelHome = root->ChildNodes[1] != nullptr ? FirstDockLeaf(root->ChildNodes[1]) : graphHome;
		}
		const auto needsHome = [&](const char *label) {
			if (panels.ResetRequested) return true;
			const ImGuiWindowSettings *saved = ImGui::FindWindowSettingsByID(ImHashStr(label));
			const ImGuiWindow *window = ImGui::FindWindowByName(label);
			if (saved == nullptr && window == nullptr) return true;
			const ImGuiID dock = PanelDockId(label);
			if (dock == 0) return false;
			ImGuiDockNode *node = ImGui::DockBuilderGetNode(dock);
			if (node == nullptr || !node->IsLeafNode()) return true;
			const ImGuiDockNode *savedRoot = ImGui::DockNodeGetRootNode(node);
			return savedRoot->IsDockSpace() && savedRoot->ID != dockspace;
		};
		if (needsHome(IMAGE_COMPOSER_GRAPH)) ImGui::DockBuilderDockWindow(IMAGE_COMPOSER_GRAPH, graphHome);
		for (const auto &panel : IMAGE_COMPOSER_PANELS)
			if (needsHome(panel.Window)) ImGui::DockBuilderDockWindow(panel.Window, panelHome);
		ImGui::DockBuilderFinish(dockspace);
		panels.LayoutInitialized = true;
		panels.ResetRequested = false;
	}

	inline void DrawImageComposerView(ImageComposerPanels &panels) {
		if (ImGui::Button("View")) ImGui::OpenPopup("##image-composer-view");
		if (!ImGui::BeginPopup("##image-composer-view")) return;
		for (size_t index = 0; index < IMAGE_COMPOSER_PANELS.size(); ++index)
			ImGui::MenuItem(IMAGE_COMPOSER_PANELS[index].Title, nullptr, &panels.Open[index]);
		ImGui::EndPopup();
	}
}
