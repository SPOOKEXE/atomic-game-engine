#include <engine/ui/Theme.hpp>

#include <imgui.h>

namespace engine::ui {

	namespace {
		// The palette, as linear-ish sRGB floats. Named rather than inlined so
		// that the style table below reads as "which surface" instead of "which
		// number", which is the difference between a theme you can edit and a
		// theme you can only replace.
		constexpr ImVec4 BACKGROUND{0.086f, 0.090f, 0.106f, 1.00f};
		constexpr ImVec4 SURFACE{0.125f, 0.133f, 0.153f, 1.00f};
		constexpr ImVec4 RAISED{0.169f, 0.180f, 0.204f, 1.00f};
		constexpr ImVec4 SUNKEN{0.063f, 0.067f, 0.078f, 1.00f};
		constexpr ImVec4 BORDER{0.220f, 0.235f, 0.267f, 1.00f};

		constexpr ImVec4 TEXT{0.878f, 0.894f, 0.925f, 1.00f};
		constexpr ImVec4 TEXT_MUTED{0.518f, 0.549f, 0.604f, 1.00f};

		constexpr ImVec4 ACCENT{0.259f, 0.522f, 0.957f, 1.00f};
		constexpr ImVec4 ACCENT_DIM{0.259f, 0.522f, 0.957f, 0.35f};
		constexpr ImVec4 ACCENT_HOT{0.365f, 0.612f, 1.000f, 1.00f};

		// Packs a float colour the way `IM_COL32` packs a byte one, so a
		// drawn thing and a styled thing can be given the same constant.
		unsigned int Pack(const ImVec4 &colour) {
			return IM_COL32(
				static_cast<int>(colour.x * 255.0f + 0.5f),
				static_cast<int>(colour.y * 255.0f + 0.5f),
				static_cast<int>(colour.z * 255.0f + 0.5f),
				static_cast<int>(colour.w * 255.0f + 0.5f)
			);
		}
	}

	void ApplyEditorTheme(float scale) {
		const float unit = scale > 0.0f ? scale : 1.0f;

		ImGuiStyle &style = ImGui::GetStyle();
		ImVec4 *colours = style.Colors;

		colours[ImGuiCol_Text] = TEXT;
		colours[ImGuiCol_TextDisabled] = TEXT_MUTED;

		colours[ImGuiCol_WindowBg] = SURFACE;
		colours[ImGuiCol_ChildBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_PopupBg] = RAISED;
		colours[ImGuiCol_MenuBarBg] = BACKGROUND;

		colours[ImGuiCol_Border] = BORDER;
		colours[ImGuiCol_BorderShadow] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};

		colours[ImGuiCol_FrameBg] = SUNKEN;
		colours[ImGuiCol_FrameBgHovered] = RAISED;
		colours[ImGuiCol_FrameBgActive] = ACCENT_DIM;

		colours[ImGuiCol_TitleBg] = BACKGROUND;
		colours[ImGuiCol_TitleBgActive] = BACKGROUND;
		colours[ImGuiCol_TitleBgCollapsed] = BACKGROUND;

		colours[ImGuiCol_ScrollbarBg] = SUNKEN;
		colours[ImGuiCol_ScrollbarGrab] = RAISED;
		colours[ImGuiCol_ScrollbarGrabHovered] = BORDER;
		colours[ImGuiCol_ScrollbarGrabActive] = ACCENT;

		colours[ImGuiCol_CheckMark] = ACCENT_HOT;
		colours[ImGuiCol_SliderGrab] = ACCENT;
		colours[ImGuiCol_SliderGrabActive] = ACCENT_HOT;

		colours[ImGuiCol_Button] = RAISED;
		colours[ImGuiCol_ButtonHovered] = BORDER;
		colours[ImGuiCol_ButtonActive] = ACCENT;

		colours[ImGuiCol_Header] = ACCENT_DIM;
		colours[ImGuiCol_HeaderHovered] = BORDER;
		colours[ImGuiCol_HeaderActive] = ACCENT;

		colours[ImGuiCol_Separator] = BORDER;
		colours[ImGuiCol_SeparatorHovered] = ACCENT;
		colours[ImGuiCol_SeparatorActive] = ACCENT_HOT;

		colours[ImGuiCol_ResizeGrip] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_ResizeGripHovered] = ACCENT_DIM;
		colours[ImGuiCol_ResizeGripActive] = ACCENT;

		colours[ImGuiCol_Tab] = BACKGROUND;
		colours[ImGuiCol_TabHovered] = BORDER;
		colours[ImGuiCol_TabSelected] = SURFACE;
		colours[ImGuiCol_TabSelectedOverline] = ACCENT;
		colours[ImGuiCol_TabDimmed] = BACKGROUND;
		colours[ImGuiCol_TabDimmedSelected] = SURFACE;
		colours[ImGuiCol_TabDimmedSelectedOverline] = BORDER;

		colours[ImGuiCol_DockingPreview] = ACCENT_DIM;
		colours[ImGuiCol_DockingEmptyBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};

		colours[ImGuiCol_TableHeaderBg] = BACKGROUND;
		colours[ImGuiCol_TableBorderStrong] = BORDER;
		colours[ImGuiCol_TableBorderLight] = ImVec4{BORDER.x, BORDER.y, BORDER.z, 0.45f};
		colours[ImGuiCol_TableRowBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_TableRowBgAlt] = ImVec4{1.0f, 1.0f, 1.0f, 0.020f};

		colours[ImGuiCol_TextSelectedBg] = ACCENT_DIM;
		colours[ImGuiCol_NavCursor] = ACCENT;

		// **`DockingEmptyBg` is transparent and that is load-bearing.** The
		// dockspace covers the whole window, so an opaque one would paint over
		// the world the editor exists to show — the viewport is the hole in the
		// middle of the layout rather than a panel with a texture in it. See
		// `render::Viewport`, which is how the renderer is told where the hole
		// is.

		style.WindowRounding = 0.0f;
		style.ChildRounding = 4.0f * unit;
		style.FrameRounding = 3.0f * unit;
		style.PopupRounding = 4.0f * unit;
		style.ScrollbarRounding = 6.0f * unit;
		style.GrabRounding = 3.0f * unit;
		style.TabRounding = 4.0f * unit;

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBarBorderSize = 1.0f;

		style.WindowPadding = ImVec2{8.0f * unit, 8.0f * unit};
		style.FramePadding = ImVec2{7.0f * unit, 4.0f * unit};
		style.CellPadding = ImVec2{6.0f * unit, 3.0f * unit};
		style.ItemSpacing = ImVec2{7.0f * unit, 5.0f * unit};
		style.ItemInnerSpacing = ImVec2{5.0f * unit, 4.0f * unit};
		style.IndentSpacing = 16.0f * unit;
		style.ScrollbarSize = 12.0f * unit;
		style.GrabMinSize = 10.0f * unit;

		// Titles centred and tabs left, which is Studio's arrangement and not a
		// coincidence: a docked panel's title is a label and a tab is a target,
		// and a target that moves as its neighbours are renamed is harder to
		// hit than one anchored to an edge.
		style.WindowTitleAlign = ImVec2{0.5f, 0.5f};
		style.WindowMenuButtonPosition = ImGuiDir_None;

		style.SeparatorTextBorderSize = 1.0f;
		style.SeparatorTextPadding = ImVec2{16.0f * unit, 2.0f * unit};
	}

	unsigned int AccentColour() {
		return Pack(ACCENT);
	}

	unsigned int SelectionColour() {
		return Pack(ACCENT_DIM);
	}

	unsigned int WarningColour() {
		return IM_COL32(232, 178, 76, 255);
	}

	unsigned int ErrorColour() {
		return IM_COL32(233, 96, 96, 255);
	}

	unsigned int MutedColour() {
		return Pack(TEXT_MUTED);
	}
}
