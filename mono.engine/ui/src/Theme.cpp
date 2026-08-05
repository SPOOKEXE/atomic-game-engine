#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <cstdio>
#include <imgui.h>
#include <imgui_internal.h>

namespace engine::ui {

	namespace {
		// What a palette declares. Everything else is derived.
		//
		// **Five colours, and the other ten are shades of the first.** A theme
		// that spelled out its button face, its hovered button face, its input
		// well and its border is a theme with four more chances to put a
		// control at the wrong distance from the panel behind it — and with
		// seven of them, that is twenty-eight. The shade factors below are the
		// design; a palette only chooses where on the scale it sits and what
		// colour the highlights are.
		struct PaletteSpec {
			const char *Name;

			// The panel colour, and the anchor every surface is derived from.
			ImVec4 Surface;

			// The highlight, and the same colour hovered.
			ImVec4 Accent;
			ImVec4 AccentHot;

			// Ordinary text and secondary text.
			ImVec4 Text;
			ImVec4 TextMuted;
		};

		// The shade ladder, relative to a palette's surface.
		//
		// **Multiplicative rather than additive**, so a near-black palette
		// keeps its separations proportional instead of collapsing into one
		// colour. `Shadow` and `Terminal` are why: an additive ladder that
		// looks right at 20% lightness has no room left at 5%.
		constexpr float BACKGROUND_SHADE = 0.72f; // behind panels, and menu bars
		constexpr float RAISED_SHADE = 1.36f;	  // a button's face
		constexpr float RAISED_HOT_SHADE = 1.72f; // hovered
		constexpr float SUNKEN_SHADE = 0.56f;	  // an input's well
		constexpr float VIEWPORT_SHADE = 0.38f;	  // the darkest surface there is
		constexpr float BORDER_SHADE = 0.60f;

		// The greys the four tinted palettes share. Roughly Studio's
		// `MainText`/`DimmedText`, kept neutral so that only the chrome and the
		// accent carry the hue.
		constexpr ImVec4 NEUTRAL_TEXT{0.855f, 0.867f, 0.886f, 1.00f};
		constexpr ImVec4 NEUTRAL_MUTED{0.482f, 0.510f, 0.561f, 1.00f};

		// **The reference's own numbers, first in the list because it is the
		// default.** Deep blue-grey chrome with a bright blue accent: the
		// arrangement everything else here was measured against.
		constexpr PaletteSpec PALETTES[PALETTE_COUNT]{
			// Dark
			{"Dark",
			 {0.106f, 0.122f, 0.153f, 1.00f},
			 {0.184f, 0.435f, 0.925f, 1.00f},
			 {0.322f, 0.549f, 0.976f, 1.00f},
			 NEUTRAL_TEXT,
			 NEUTRAL_MUTED},

			// Shadow — near black, and the accent is a grey rather than a hue.
			{"Shadow",
			 {0.071f, 0.071f, 0.078f, 1.00f},
			 {0.427f, 0.435f, 0.463f, 1.00f},
			 {0.573f, 0.580f, 0.608f, 1.00f},
			 {0.800f, 0.800f, 0.816f, 1.00f},
			 {0.443f, 0.443f, 0.459f, 1.00f}},

			// Blue — the same structure, further into the hue than `Dark`.
			{"Blue",
			 {0.075f, 0.122f, 0.196f, 1.00f},
			 {0.129f, 0.502f, 0.949f, 1.00f},
			 {0.294f, 0.612f, 0.980f, 1.00f},
			 NEUTRAL_TEXT,
			 NEUTRAL_MUTED},

			// Purple
			{"Purple",
			 {0.125f, 0.098f, 0.180f, 1.00f},
			 {0.573f, 0.353f, 0.937f, 1.00f},
			 {0.667f, 0.478f, 0.965f, 1.00f},
			 NEUTRAL_TEXT,
			 NEUTRAL_MUTED},

			// Red
			{"Red",
			 {0.157f, 0.086f, 0.098f, 1.00f},
			 {0.878f, 0.278f, 0.325f, 1.00f},
			 {0.937f, 0.427f, 0.463f, 1.00f},
			 NEUTRAL_TEXT,
			 NEUTRAL_MUTED},

			// Green
			{"Green",
			 {0.078f, 0.141f, 0.106f, 1.00f},
			 {0.204f, 0.729f, 0.400f, 1.00f},
			 {0.353f, 0.812f, 0.518f, 1.00f},
			 NEUTRAL_TEXT,
			 NEUTRAL_MUTED},

			// Terminal — black and phosphor, text included. The one palette
			// whose text is not neutral, and the header says why.
			{"Terminal",
			 {0.027f, 0.043f, 0.031f, 1.00f},
			 {0.200f, 0.925f, 0.361f, 1.00f},
			 {0.373f, 0.980f, 0.510f, 1.00f},
			 {0.639f, 0.937f, 0.678f, 1.00f},
			 {0.361f, 0.616f, 0.404f, 1.00f}},
		};

		// The scale every metric is multiplied by. Set by `ApplyEditorTheme`,
		// read by `Scaled`, and held here so that no widget has to be handed it.
		float CurrentScale = 1.0f;

		// Which palette is live, and the scale to restyle at when it changes.
		Palette ChosenPalette = Palette::Dark;

		const PaletteSpec &Current() {
			const auto index = static_cast<size_t>(ChosenPalette);
			return PALETTES[index < PALETTE_COUNT ? index : 0];
		}

		// A surface at a point on the shade ladder.
		//
		// Clamped at the top, because a bright palette scaled by 1.72 would
		// otherwise wrap past white into whatever the driver does with an
		// out-of-range float.
		constexpr ImVec4 Shade(const ImVec4 &base, float factor) {
			const auto clamp = [](float value) {
				return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
			};
			return ImVec4{clamp(base.x * factor), clamp(base.y * factor), clamp(base.z * factor), base.w};
		}

		// The accent at an alpha, for the washes: a selected row, a docking
		// preview, a text selection. One colour at three strengths rather than
		// three colours that have to be kept in step.
		constexpr ImVec4 Fade(const ImVec4 &base, float alpha) {
			return ImVec4{base.x, base.y, base.z, alpha};
		}

		// Packs a float colour the way `IM_COL32` packs a byte one, so a drawn
		// thing and a styled thing can be given the same constant.
		unsigned int Pack(const ImVec4 &colour) {
			return IM_COL32(
				static_cast<int>(colour.x * 255.0f + 0.5f),
				static_cast<int>(colour.y * 255.0f + 0.5f),
				static_cast<int>(colour.z * 255.0f + 0.5f),
				static_cast<int>(colour.w * 255.0f + 0.5f)
			);
		}
	}

	void InstallThemeSettings() {
		// **The palette rides in the layout ini rather than in a file of its
		// own.** It is the same kind of fact as where somebody dragged a panel
		// — a per-machine preference nobody would check into a repository —
		// and a second config file is a second thing to find, parse, version
		// and fail to write.
		//
		// **Registered before the first frame**, because imgui loads the ini
		// lazily on the first `NewFrame` and a handler registered after that
		// is a handler whose lines have already been skipped as unknown.
		ImGuiContext *context = ImGui::GetCurrentContext();
		if (context == nullptr) {
			return;
		}

		for (const ImGuiSettingsHandler &existing : context->SettingsHandlers) {
			if (existing.TypeHash == ImHashStr("StudioTheme")) {
				return;
			}
		}

		ImGuiSettingsHandler handler;
		handler.TypeName = "StudioTheme";
		handler.TypeHash = ImHashStr("StudioTheme");

		// One entry, so the open just has to hand back something not null.
		handler.ReadOpenFn = [](ImGuiContext *, ImGuiSettingsHandler *, const char *) -> void * {
			return reinterpret_cast<void *>(1);
		};

		handler.ReadLineFn = [](ImGuiContext *, ImGuiSettingsHandler *, void *, const char *line) {
			int value = 0;
			if (std::sscanf(line, "Palette=%d", &value) != 1) {
				return;
			}

			// **Range-checked, because an ini is a file a person can edit and a
			// file an older build can write.** A palette index out of range
			// would index the table past its end; falling back to the default
			// is what a setting nobody can read should do.
			if (value >= 0 && static_cast<size_t>(value) < PALETTE_COUNT) {
				SetPalette(static_cast<Palette>(value));
			}
		};

		handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
			out->appendf("[%s][Chosen]\n", h->TypeName);
			out->appendf("Palette=%d\n\n", static_cast<int>(CurrentPalette()));
		};

		ImGui::AddSettingsHandler(&handler);
	}

	const char *Describe(Palette palette) {
		const auto index = static_cast<size_t>(palette);
		return PALETTES[index < PALETTE_COUNT ? index : 0].Name;
	}

	Palette CurrentPalette() {
		return ChosenPalette;
	}

	PaletteSample SampleOf(Palette palette) {
		const auto index = static_cast<size_t>(palette);
		const PaletteSpec &spec = PALETTES[index < PALETTE_COUNT ? index : 0];

		// The same ladder `ApplyEditorTheme` uses, so a swatch is the theme
		// rather than an approximation of it that drifts when a factor changes.
		return PaletteSample{
			Pack(spec.Surface),
			Pack(Shade(spec.Surface, RAISED_SHADE)),
			Pack(spec.Accent),
		};
	}

	void SetPalette(Palette palette) {
		ChosenPalette = palette;

		// **Only when there is something to restyle.** A palette chosen from a
		// config file before the interface exists is a choice, not a draw call;
		// `ApplyEditorTheme` picks it up when the context is made.
		if (ImGui::GetCurrentContext() == nullptr) {
			return;
		}

		ApplyEditorTheme(CurrentScale);

		// **Marked dirty here rather than by the caller**, so that persisting
		// the choice is not something each call site has to remember — and so
		// that no panel needs `imgui_internal.h` to change a colour. imgui
		// saves on a timer and on shutdown, so an editor that was killed rather
		// than closed keeps the choice it would otherwise appear not to have
		// saved.
		ImGui::MarkIniSettingsDirty();
	}

	float Scale() {
		return CurrentScale;
	}

	float Scaled(float value) {
		return value * CurrentScale;
	}

	void ApplyEditorTheme(float scale) {
		CurrentScale = scale > 0.0f ? scale : 1.0f;

		// **Derived here rather than stored.** Fifteen constants per palette
		// times seven palettes is a hundred and five numbers, of which seventy
		// would be the same ladder written out again — and the first one to be
		// mistyped would be a button half a shade off in one theme, which is
		// the kind of thing that gets noticed a year later.
		const PaletteSpec &palette = Current();

		const ImVec4 SURFACE = palette.Surface;
		const ImVec4 BACKGROUND = Shade(SURFACE, BACKGROUND_SHADE);
		const ImVec4 RAISED = Shade(SURFACE, RAISED_SHADE);
		const ImVec4 RAISED_HOT = Shade(SURFACE, RAISED_HOT_SHADE);
		const ImVec4 SUNKEN = Shade(SURFACE, SUNKEN_SHADE);
		const ImVec4 VIEWPORT = Shade(SURFACE, VIEWPORT_SHADE);
		const ImVec4 BORDER = Shade(SURFACE, BORDER_SHADE);

		const ImVec4 TEXT = palette.Text;
		const ImVec4 TEXT_MUTED = palette.TextMuted;

		const ImVec4 ACCENT = palette.Accent;
		const ImVec4 ACCENT_HOT = palette.AccentHot;
		const ImVec4 ACCENT_DIM = Fade(ACCENT, 0.42f);

		// **No `LINK` here**, and its absence is not an oversight: a matched run
		// in a filtered list is *drawn* rather than styled, so imgui's table has
		// no slot for it. `LinkColour` derives it from the same accent — the
		// accent lifted toward white, so it reads as "the accent, brighter"
		// rather than as a sixth colour nobody chose.

		ImGuiStyle &style = ImGui::GetStyle();
		ImVec4 *colours = style.Colors;

		colours[ImGuiCol_Text] = TEXT;
		colours[ImGuiCol_TextDisabled] = TEXT_MUTED;

		colours[ImGuiCol_WindowBg] = SURFACE;
		colours[ImGuiCol_ChildBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_PopupBg] = RAISED;
		colours[ImGuiCol_MenuBarBg] = BACKGROUND;
		colours[ImGuiCol_DockingEmptyBg] = VIEWPORT;

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
		colours[ImGuiCol_ScrollbarGrabHovered] = RAISED_HOT;
		colours[ImGuiCol_ScrollbarGrabActive] = ACCENT;

		colours[ImGuiCol_CheckMark] = ACCENT_HOT;
		colours[ImGuiCol_SliderGrab] = ACCENT;
		colours[ImGuiCol_SliderGrabActive] = ACCENT_HOT;

		colours[ImGuiCol_Button] = RAISED;
		colours[ImGuiCol_ButtonHovered] = RAISED_HOT;
		colours[ImGuiCol_ButtonActive] = ACCENT;

		colours[ImGuiCol_Header] = ACCENT_DIM;
		colours[ImGuiCol_HeaderHovered] = RAISED_HOT;
		colours[ImGuiCol_HeaderActive] = ACCENT;

		colours[ImGuiCol_Separator] = BORDER;
		colours[ImGuiCol_SeparatorHovered] = ACCENT;
		colours[ImGuiCol_SeparatorActive] = ACCENT_HOT;

		colours[ImGuiCol_ResizeGrip] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_ResizeGripHovered] = ACCENT_DIM;
		colours[ImGuiCol_ResizeGripActive] = ACCENT;

		colours[ImGuiCol_Tab] = BACKGROUND;
		colours[ImGuiCol_TabHovered] = RAISED_HOT;
		colours[ImGuiCol_TabSelected] = SURFACE;
		colours[ImGuiCol_TabSelectedOverline] = ACCENT;
		colours[ImGuiCol_TabDimmed] = BACKGROUND;
		colours[ImGuiCol_TabDimmedSelected] = SURFACE;
		colours[ImGuiCol_TabDimmedSelectedOverline] = BORDER;

		colours[ImGuiCol_DockingPreview] = ACCENT_DIM;

		colours[ImGuiCol_TableHeaderBg] = BACKGROUND;
		colours[ImGuiCol_TableBorderStrong] = BORDER;
		colours[ImGuiCol_TableBorderLight] = ImVec4{BORDER.x, BORDER.y, BORDER.z, 0.45f};
		colours[ImGuiCol_TableRowBg] = ImVec4{0.0f, 0.0f, 0.0f, 0.0f};
		colours[ImGuiCol_TableRowBgAlt] = ImVec4{1.0f, 1.0f, 1.0f, 0.020f};

		colours[ImGuiCol_TextSelectedBg] = ACCENT_DIM;
		colours[ImGuiCol_NavCursor] = ACCENT;

		// **`DockingEmptyBg` is opaque now, and that is the v0.7 change.** It was
		// transparent while the world was drawn through a hole in the dockspace
		// — and that arrangement is gone, because `imgui.cpp` only punches the
		// hole while the central node is *empty*, so docking the viewport into
		// it painted over the frame. The world is a texture in a panel now, so
		// an empty dock area is a surface like any other and takes the darkest
		// grey in the palette. See `render::SceneTarget`.

		// **Every number below comes from `ui::Metrics`.** A size between two
		// steps is a mistake rather than a choice, and an interface built from
		// five numbers looks designed where one built from thirty looks like
		// nobody was in charge.
		style.WindowRounding = 0.0f;
		style.ChildRounding = Scaled(Radius::Panel);
		style.PopupRounding = Scaled(Radius::Panel);
		style.FrameRounding = Scaled(Radius::Control);
		style.GrabRounding = Scaled(Radius::Control);
		style.TabRounding = Scaled(Radius::Control);
		style.ScrollbarRounding = Scaled(Radius::Control);

		style.WindowBorderSize = 1.0f;
		style.ChildBorderSize = 1.0f;
		style.PopupBorderSize = 1.0f;
		style.FrameBorderSize = 0.0f;
		style.TabBarBorderSize = 1.0f;

		// Borders are one pixel at every scale. Doubling one draws a frame
		// around every widget heavy enough to read as a selection.
		style.WindowPadding = ImVec2(Scaled(Space::Large), Scaled(Space::Large));
		style.FramePadding = ImVec2(Scaled(Space::Large), Scaled(Space::Small));
		style.CellPadding = ImVec2(Scaled(Space::Medium), Scaled(Space::Tiny));
		style.ItemSpacing = ImVec2(Scaled(Space::Medium), Scaled(Space::Small));
		style.ItemInnerSpacing = ImVec2(Scaled(Space::Small), Scaled(Space::Small));
		style.IndentSpacing = Scaled(Size::Indent);
		style.ScrollbarSize = Scaled(Space::Huge);
		style.GrabMinSize = Scaled(Space::Huge);

		// Titles centred and tabs left, which is Studio's arrangement and not a
		// coincidence: a docked panel's title is a label and a tab is a target,
		// and a target that moves as its neighbours are renamed is harder to hit
		// than one anchored to an edge.
		style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
		style.WindowMenuButtonPosition = ImGuiDir_None;

		style.SeparatorTextBorderSize = 1.0f;
		style.SeparatorTextPadding = ImVec2(Scaled(Space::Huge), Scaled(Space::Tiny));

		// **Antialiased fills off for rectangles is deliberately not set.** The
		// panels are rectangles at integer positions, so the antialiasing costs
		// nothing on them — and the rounded corners the scale asks for are the
		// one thing that would look wrong without it.
		style.WindowMinSize = ImVec2(Scaled(Size::PanelMinimum), Scaled(Size::Bar * 2.0f));
	}

	unsigned int AccentColour() {
		return Pack(Current().Accent);
	}

	unsigned int SelectionColour() {
		return Pack(Fade(Current().Accent, 0.42f));
	}

	unsigned int WarningColour() {
		// WarningText.
		return IM_COL32(255, 191, 77, 255);
	}

	unsigned int ErrorColour() {
		// ErrorText.
		return IM_COL32(255, 89, 89, 255);
	}

	unsigned int MutedColour() {
		return Pack(Current().TextMuted);
	}

	unsigned int LinkColour() {
		return Pack(Shade(Current().AccentHot, 1.18f));
	}

	unsigned int BrightColour() {
		// **White, except where white is wrong.** `Terminal`'s text is
		// phosphor green, and the brightest thing on it has to be a brighter
		// green rather than a colour from a different palette entirely.
		return Pack(Shade(Current().Text, 1.30f));
	}
}
