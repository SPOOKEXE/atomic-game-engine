// The theme, checked without a GPU.
//
// **An imgui context is not a device.** `ImGui::CreateContext` allocates a
// style table and a font atlas description and touches no driver, so the two
// claims in `Theme.cpp` that would otherwise be assertions in a comment are
// checkable here — and they are the two that break the editor visibly rather
// than subtly.

#include <engine/testing/Suite.hpp>
#include <engine/ui/Theme.hpp>

#include <catch2/catch_test_macros.hpp>

#include <imgui.h>

TEST_SUITE_ID("engine.ui.theme")

namespace {
	// Creates and destroys a context around one body, so a failing assertion
	// does not leave a context behind for the next test case to inherit.
	struct Context {
		ImGuiContext *Handle = nullptr;

		Context() {
			IMGUI_CHECKVERSION();
			Handle = ImGui::CreateContext();
		}

		~Context() {
			ImGui::DestroyContext(Handle);
		}

		Context(const Context &) = delete;
		Context &operator=(const Context &) = delete;
	};
}

TEST_CASE("the dockspace's empty background is transparent", "[ui][theme]") {
	// **The one that makes the editor look broken rather than ugly.** The
	// dockspace host covers the whole window, so an opaque `DockingEmptyBg`
	// paints over the world — and the symptom is a viewport showing the clear
	// colour, which reads as "the renderer stopped drawing" rather than as a
	// style value. The world is the hole in the middle of the layout; see
	// `render::Viewport`, which is how the renderer is told where that hole is.
	const Context context;
	engine::ui::ApplyEditorTheme();

	CHECK(ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg].w == 0.0f);
	CHECK(ImGui::GetStyle().Colors[ImGuiCol_ChildBg].w == 0.0f);
}

TEST_CASE("one scale knob moves every metric", "[ui][theme]") {
	// `InterfaceSettings` offers a single scale, and a UI scaled in one
	// dimension and not another is worse than one that is small: text that
	// outgrows its row overlaps the row below and reads as a corrupt font. So
	// every spacing has to move together, and "every" is what this checks.
	const Context context;

	engine::ui::ApplyEditorTheme(1.0f);
	const ImGuiStyle single = ImGui::GetStyle();

	engine::ui::ApplyEditorTheme(2.0f);
	const ImGuiStyle doubled = ImGui::GetStyle();

	CHECK(doubled.WindowPadding.x == single.WindowPadding.x * 2.0f);
	CHECK(doubled.FramePadding.y == single.FramePadding.y * 2.0f);
	CHECK(doubled.ItemSpacing.x == single.ItemSpacing.x * 2.0f);
	CHECK(doubled.IndentSpacing == single.IndentSpacing * 2.0f);
	CHECK(doubled.ScrollbarSize == single.ScrollbarSize * 2.0f);
	CHECK(doubled.GrabMinSize == single.GrabMinSize * 2.0f);

	// Borders are the deliberate exception and are named so that a reader does
	// not read their absence above as an oversight. A one-pixel border is one
	// pixel at every scale — doubling it draws a frame around every widget
	// heavy enough to read as a selection.
	CHECK(doubled.WindowBorderSize == single.WindowBorderSize);
}

TEST_CASE("the drawn colours are the styled ones", "[ui][theme]") {
	// The accent is both a style entry and a packed constant, because a few
	// things are drawn rather than styled — the explorer's selection band, the
	// toolbar's run-state pill. Two spellings of one colour is the drift
	// `Theme.cpp` exists to prevent, so this checks they still agree.
	const Context context;
	engine::ui::ApplyEditorTheme();

	const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_SliderGrab];
	CHECK(ImGui::ColorConvertFloat4ToU32(accent) == engine::ui::AccentColour());

	const ImVec4 muted = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
	CHECK(ImGui::ColorConvertFloat4ToU32(muted) == engine::ui::MutedColour());
}
