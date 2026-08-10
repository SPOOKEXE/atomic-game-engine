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

TEST_CASE("a colour survives a round trip through text", "[ui][theme]") {
	// The text form is the only place the byte order differs from `IM_COL32`,
	// and it is a boundary two files write and two read — the layout ini and
	// `preferences.json`. A swap that is wrong in one direction only would show
	// up as colours that drift a little further from what somebody chose every
	// time the editor is restarted, which is the worst kind of bug to find.
	const unsigned int packed = IM_COL32(0x2E, 0x34, 0x40, 0xC0);

	CHECK(engine::ui::ColourText(packed) == "2E3440C0");
	CHECK(engine::ui::ParseColourText("2E3440C0") == packed);
	CHECK(engine::ui::ParseColourText("#2e3440c0") == packed);

	// Six digits is opaque, because that is what somebody pastes out of a
	// palette and appending `FF` is a step they will forget once.
	CHECK(engine::ui::ParseColourText("#2E3440") == IM_COL32(0x2E, 0x34, 0x40, 0xFF));

	// **Refused rather than read as zero.** A typo would otherwise turn a panel
	// black and look like a bug in the theme rather than one in the file.
	CHECK(!engine::ui::ParseColourText("2E344"));
	CHECK(!engine::ui::ParseColourText("2E3440GG"));
	CHECK(!engine::ui::ParseColourText(""));
}

TEST_CASE("every colour has a name that parses back", "[ui][theme]") {
	// The names are the key in two files and the argument a plugin passes to
	// `SetWidgetColour`, so a name that does not parse back is a setting that
	// writes and never reads.
	for (size_t index = 0; index < engine::ui::THEME_COLOUR_COUNT; index++) {
		const auto colour = static_cast<engine::ui::ThemeColour>(index);
		INFO("colour: " << engine::ui::Describe(colour));
		CHECK(engine::ui::ParseThemeColour(engine::ui::Describe(colour)) == colour);
	}

	CHECK(!engine::ui::ParseThemeColour("Surfaces"));
	CHECK(!engine::ui::ParseThemeColour(""));
}

TEST_CASE("an override rides over the palette rather than replacing it", "[ui][theme]") {
	// **The claim the whole design rests on.** An override that was a full copy
	// of a palette would pin all seven colours the moment anybody touched one,
	// and choosing a theme afterwards would appear to do nothing — which is
	// exactly the complaint a customisable theme usually earns.
	const Context context;

	engine::ui::SetPalette(engine::ui::Palette::Dark);
	engine::ui::ApplyEditorTheme();
	const ImVec4 darkSurface = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

	engine::ui::ThemeColours chosen;
	chosen[engine::ui::ThemeColour::Accent] = IM_COL32(0xFF, 0x00, 0x80, 0xFF);
	engine::ui::SetGlobalColours(chosen);

	CHECK(engine::ui::AccentColour() == IM_COL32(0xFF, 0x00, 0x80, 0xFF));

	// The surface is still the palette's, because nobody chose one.
	const ImVec4 afterAccent = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	CHECK(afterAccent.x == darkSurface.x);

	// And switching palette moves everything except the accent that was pinned.
	engine::ui::SetPalette(engine::ui::Palette::Green);
	CHECK(engine::ui::AccentColour() == IM_COL32(0xFF, 0x00, 0x80, 0xFF));
	CHECK(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].y != darkSurface.y);

	// A surface override reaches the whole ladder, not just the window: this is
	// what makes seven knobs enough, and what a per-slot editor would lose.
	chosen[engine::ui::ThemeColour::Surface] = IM_COL32(0x40, 0x20, 0x10, 0xFF);
	engine::ui::SetGlobalColours(chosen);

	const ImGuiStyle &style = ImGui::GetStyle();
	CHECK(style.Colors[ImGuiCol_WindowBg].x > style.Colors[ImGuiCol_WindowBg].z);
	CHECK(style.Colors[ImGuiCol_Button].x > style.Colors[ImGuiCol_WindowBg].x);
	CHECK(style.Colors[ImGuiCol_FrameBg].x < style.Colors[ImGuiCol_WindowBg].x);

	// Left as it was found, so the case order cannot change another's result.
	engine::ui::SetGlobalColours(engine::ui::ThemeColours{});
	engine::ui::SetPalette(engine::ui::Palette::Dark);
}

TEST_CASE("a panel's colours are pushed and popped", "[ui][theme]") {
	// `ScopedColours` is the per-widget half, and the two things that can go
	// wrong with it are both silent: pushing nothing when something was asked
	// for, and leaving the stack unbalanced — which imgui reports a frame later
	// as an assertion in an unrelated window.
	const Context context;
	engine::ui::SetPalette(engine::ui::Palette::Dark);
	engine::ui::ApplyEditorTheme();

	const ImVec4 before = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];

	{
		// Empty is a no-op, which is what every panel nobody recoloured gets.
		const engine::ui::ScopedColours none{engine::ui::ThemeColours{}};
		CHECK(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == before.x);
	}

	engine::ui::ThemeColours chosen;
	chosen[engine::ui::ThemeColour::Surface] = IM_COL32(0x10, 0x40, 0x20, 0xFF);

	{
		const engine::ui::ScopedColours skin(chosen);

		// The style imgui reads is the pushed one, and the whole ladder moved
		// with it rather than the window alone.
		const ImGuiStyle &style = ImGui::GetStyle();
		CHECK(style.Colors[ImGuiCol_WindowBg].y > style.Colors[ImGuiCol_WindowBg].x);
		CHECK(style.Colors[ImGuiCol_Button].y > style.Colors[ImGuiCol_WindowBg].y);
	}

	// And it is back, exactly, on the way out.
	const ImVec4 after = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	CHECK(after.x == before.x);
	CHECK(after.y == before.y);
	CHECK(after.z == before.z);
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
