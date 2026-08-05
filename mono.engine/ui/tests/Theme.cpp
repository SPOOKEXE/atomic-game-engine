// The theme, checked without a GPU.
//
// **An imgui context is not a device.** `ImGui::CreateContext` allocates a
// style table and a font atlas description and touches no driver, so the two
// claims in `Theme.cpp` that would otherwise be assertions in a comment are
// checkable here — and they are the two that break the editor visibly rather
// than subtly.

#include <engine/testing/Suite.hpp>
#include <engine/ui/Metrics.hpp>
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

TEST_CASE("the dockspace's empty background is opaque", "[ui][theme]") {
	// **This assertion is the reverse of what it was at v0.7's midpoint, and
	// the flip is the interesting part.** While the world was drawn through a
	// hole in the dockspace, `DockingEmptyBg` had to be transparent or it
	// painted over the frame. That arrangement is gone: `imgui.cpp` only punches
	// the hole while the central node is *empty*, so docking the viewport into
	// it — the only way anybody uses it — filled the whole dockspace with
	// `WindowBg` and the world vanished.
	//
	// The world is a texture in a panel now, so an empty dock area is an
	// ordinary surface. A transparent one would show whatever the swapchain
	// happened to hold, which is last frame's image.
	const Context context;
	engine::ui::ApplyEditorTheme();

	CHECK(ImGui::GetStyle().Colors[ImGuiCol_DockingEmptyBg].w == 1.0f);

	// Child backgrounds stay transparent: a child window is a region of its
	// parent rather than a surface of its own, and painting one draws a box
	// around every scrolling list.
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
	CHECK(engine::ui::Scaled(1.0f) == 2.0f);
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

TEST_CASE("every palette is legible and distinct", "[ui][theme]") {
	// **The check seven hand-written palettes would need and would not get.**
	// Each one declares five colours and derives the rest from a shared ladder,
	// so what can still go wrong is a palette whose *anchor* is wrong: text
	// that does not separate from the surface it sits on, or a control that
	// does not separate from the panel behind it. Both are invisible in a diff
	// and obvious the moment somebody switches to that theme.
	const Context context;

	const auto luminance = [](const ImVec4 &colour) {
		return 0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z;
	};

	for (size_t index = 0; index < engine::ui::PALETTE_COUNT; index++) {
		const auto palette = static_cast<engine::ui::Palette>(index);
		engine::ui::SetPalette(palette);
		engine::ui::ApplyEditorTheme();

		INFO("palette: " << engine::ui::Describe(palette));
		CHECK(engine::ui::CurrentPalette() == palette);

		const ImGuiStyle &style = ImGui::GetStyle();
		const float surface = luminance(style.Colors[ImGuiCol_WindowBg]);
		const float text = luminance(style.Colors[ImGuiCol_Text]);

		// Text has to be well clear of the panel it is on. Every palette here
		// is dark, so "clear" means brighter — a light theme would need this
		// stated as a distance rather than as a direction.
		CHECK(text - surface > 0.35f);

		// A button has to be visible against the panel, and its hovered state
		// against itself. These are the two the shade ladder exists to
		// guarantee, checked because a palette anchored near black is where a
		// multiplicative ladder gets thin.
		const float raised = luminance(style.Colors[ImGuiCol_Button]);
		const float raisedHot = luminance(style.Colors[ImGuiCol_ButtonHovered]);
		CHECK(raised > surface);
		CHECK(raisedHot > raised);

		// An input's well has to read as *below* the panel rather than above.
		CHECK(luminance(style.Colors[ImGuiCol_FrameBg]) < surface);

		// Muted text is dimmer than ordinary text and still above the surface,
		// which is what makes a class name an annotation rather than either a
		// second name or an invisible one.
		const float muted = luminance(style.Colors[ImGuiCol_TextDisabled]);
		CHECK(muted < text);
		CHECK(muted > surface);

		// And the dockspace stays opaque whatever the palette — see the case
		// above for what a transparent one shows.
		CHECK(style.Colors[ImGuiCol_DockingEmptyBg].w == 1.0f);
	}

	// Left as it was found, so the case order cannot change another's result.
	engine::ui::SetPalette(engine::ui::Palette::Dark);
}

TEST_CASE("a palette chosen before a context survives to it", "[ui][theme]") {
	// `SetPalette` restyles immediately when there is something to restyle, and
	// remembers when there is not — which is what lets a setting read out of a
	// file be applied before the interface has been created.
	engine::ui::SetPalette(engine::ui::Palette::Terminal);
	CHECK(engine::ui::CurrentPalette() == engine::ui::Palette::Terminal);

	const Context context;
	engine::ui::ApplyEditorTheme();
	CHECK(
		ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_SliderGrab]) ==
		engine::ui::AccentColour()
	);

	engine::ui::SetPalette(engine::ui::Palette::Dark);
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
