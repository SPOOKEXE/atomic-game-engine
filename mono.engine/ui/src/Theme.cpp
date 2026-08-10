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

		// The two colours no palette declares.
		//
		// **Semantic rather than decorative**, which is why they are here and
		// not in `PaletteSpec`: a warning is amber and an error is red in every
		// theme, because what they mean does not change with the chrome. Seven
		// palettes declaring the same two colours would be fourteen numbers and
		// fourteen chances for one of them to drift.
		constexpr ImVec4 DEFAULT_WARNING{1.000f, 0.749f, 0.302f, 1.00f};
		constexpr ImVec4 DEFAULT_ERROR{1.000f, 0.349f, 0.349f, 1.00f};

		// The scale every metric is multiplied by. Set by `ApplyEditorTheme`,
		// read by `Scaled`, and held here so that no widget has to be handed it.
		float CurrentScale = 1.0f;

		// Which palette is live, and the scale to restyle at when it changes.
		Palette ChosenPalette = Palette::Dark;

		// What has been chosen over it, for the whole editor.
		ThemeColours GlobalOverride;

		const PaletteSpec &Spec(Palette palette) {
			const auto index = static_cast<size_t>(palette);
			return PALETTES[index < PALETTE_COUNT ? index : 0];
		}

		// The seven colours a theme is built from, with nothing left unchosen.
		//
		// **A resolved skin rather than a `PaletteSpec`**, because a spec is what
		// a palette *declares* and this is what is actually being drawn: the
		// declaration, plus whatever the editor and the panel chose over it.
		// Keeping them separate is what lets an override survive a change of
		// palette — see `ThemeColours`.
		struct Skin {
			ImVec4 Values[THEME_COLOUR_COUNT];

			ImVec4 &operator[](ThemeColour colour) {
				return Values[static_cast<size_t>(colour) % THEME_COLOUR_COUNT];
			}

			const ImVec4 &operator[](ThemeColour colour) const {
				return Values[static_cast<size_t>(colour) % THEME_COLOUR_COUNT];
			}
		};

		// Reads a colour packed the way `IM_COL32` packs one.
		constexpr ImVec4 Unpack(unsigned int packed) {
			return ImVec4{
				static_cast<float>(packed & 0xFFu) / 255.0f,
				static_cast<float>((packed >> 8) & 0xFFu) / 255.0f,
				static_cast<float>((packed >> 16) & 0xFFu) / 255.0f,
				static_cast<float>((packed >> 24) & 0xFFu) / 255.0f,
			};
		}

		// A palette with the overrides applied, in the order they win.
		//
		// **Palette, then editor, then panel**, which is the order of how much
		// the person was thinking about the thing they were colouring: a theme
		// is a taste, an editor-wide override is a decision, and a panel's own
		// colour is a decision about that panel.
		Skin Resolve(Palette palette, const ThemeColours *panel) {
			const PaletteSpec &spec = Spec(palette);

			Skin skin;
			skin[ThemeColour::Surface] = spec.Surface;
			skin[ThemeColour::Accent] = spec.Accent;
			skin[ThemeColour::AccentHot] = spec.AccentHot;
			skin[ThemeColour::Text] = spec.Text;
			skin[ThemeColour::TextMuted] = spec.TextMuted;
			skin[ThemeColour::Warning] = DEFAULT_WARNING;
			skin[ThemeColour::Error] = DEFAULT_ERROR;

			const auto apply = [&skin](const ThemeColours &chosen) {
				for (size_t index = 0; index < THEME_COLOUR_COUNT; index++) {
					if (chosen.Values[index]) {
						skin.Values[index] = Unpack(*chosen.Values[index]);
					}
				}
			};

			apply(GlobalOverride);
			if (panel != nullptr) {
				apply(*panel);
			}
			return skin;
		}

		// The theme as it is being drawn right now.
		Skin Live() {
			return Resolve(ChosenPalette, nullptr);
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

		// The forty-odd imgui slots, from the seven.
		//
		// **Written into an array rather than into the style**, because a panel
		// with its own colours needs the same ladder pushed rather than
		// installed — and a second copy of it is the drift the whole file exists
		// to prevent. `ApplyEditorTheme` passes `style.Colors`; `ScopedColours`
		// passes a local and pushes what differs.
		//
		// **Derived rather than stored.** Fifteen constants per palette times
		// seven palettes is a hundred and five numbers, of which seventy would be
		// the same ladder written out again — and the first one to be mistyped
		// would be a button half a shade off in one theme, which is the kind of
		// thing that gets noticed a year later.
		void Derive(const Skin &skin, ImVec4 *colours) {
			const ImVec4 SURFACE = skin[ThemeColour::Surface];
			const ImVec4 BACKGROUND = Shade(SURFACE, BACKGROUND_SHADE);
			const ImVec4 RAISED = Shade(SURFACE, RAISED_SHADE);
			const ImVec4 RAISED_HOT = Shade(SURFACE, RAISED_HOT_SHADE);
			const ImVec4 SUNKEN = Shade(SURFACE, SUNKEN_SHADE);
			const ImVec4 VIEWPORT = Shade(SURFACE, VIEWPORT_SHADE);
			const ImVec4 BORDER = Shade(SURFACE, BORDER_SHADE);

			const ImVec4 TEXT = skin[ThemeColour::Text];
			const ImVec4 TEXT_MUTED = skin[ThemeColour::TextMuted];

			const ImVec4 ACCENT = skin[ThemeColour::Accent];
			const ImVec4 ACCENT_HOT = skin[ThemeColour::AccentHot];
			const ImVec4 ACCENT_DIM = Fade(ACCENT, 0.42f);

			// **No `LINK` here**, and its absence is not an oversight: a matched
			// run in a filtered list is *drawn* rather than styled, so imgui's
			// table has no slot for it. `LinkColour` derives it from the same
			// accent — the accent lifted toward white, so it reads as "the
			// accent, brighter" rather than as a sixth colour nobody chose. The
			// warning and the error are drawn for the same reason.

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

			// **`DockingEmptyBg` is opaque now, and that is the v0.7 change.** It
			// was transparent while the world was drawn through a hole in the
			// dockspace — and that arrangement is gone, because `imgui.cpp` only
			// punches the hole while the central node is *empty*, so docking the
			// viewport into it painted over the frame. The world is a texture in
			// a panel now, so an empty dock area is a surface like any other and
			// takes the darkest grey in the palette. See `render::SceneTarget`.
		}

		// The names a colour goes by, in the settings panel and in the ini.
		//
		// **In `ThemeColour` order and checked against the count**, so adding one
		// and forgetting its name fails to build rather than writing a file whose
		// key is a null pointer.
		constexpr const char *COLOUR_NAMES[THEME_COLOUR_COUNT]{
			"Surface",
			"Accent",
			"AccentHot",
			"Text",
			"TextMuted",
			"Warning",
			"Error",
		};
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
			if (std::sscanf(line, "Palette=%d", &value) == 1) {
				// **Range-checked, because an ini is a file a person can edit
				// and a file an older build can write.** A palette index out of
				// range would index the table past its end; falling back to the
				// default is what a setting nobody can read should do.
				if (value >= 0 && static_cast<size_t>(value) < PALETTE_COUNT) {
					SetPalette(static_cast<Palette>(value));
				}
				return;
			}

			// **The overrides are named rather than numbered**, unlike the
			// palette above: an index would make reordering `ThemeColour` a
			// silent recolour of everybody's editor, and a name that no longer
			// exists is skipped instead. The same names a plugin passes and a
			// settings panel prints — see `COLOUR_NAMES`.
			char name[32] = {};
			char text[16] = {};
			if (std::sscanf(line, "%31[^=]=%15s", name, text) != 2) {
				return;
			}

			const std::optional<ThemeColour> colour = ParseThemeColour(name);
			const std::optional<unsigned int> packed = ParseColourText(text);
			if (colour && packed) {
				GlobalOverride[*colour] = *packed;

				// Restyled per line, like `SetPalette` above. Seven restyles at
				// most, once, on the frame the ini loads — and the alternative
				// is a rule about who restyles after a load that somebody has to
				// remember.
				if (ImGui::GetCurrentContext() != nullptr) {
					ApplyEditorTheme(CurrentScale);
				}
			}
		};

		handler.WriteAllFn = [](ImGuiContext *, ImGuiSettingsHandler *h, ImGuiTextBuffer *out) {
			out->appendf("[%s][Chosen]\n", h->TypeName);
			out->appendf("Palette=%d\n", static_cast<int>(CurrentPalette()));

			// **Only what was chosen.** A file listing all seven every time
			// would make an untouched editor's ini claim seven overrides, and
			// the next palette anybody picked would appear to do nothing.
			for (size_t index = 0; index < THEME_COLOUR_COUNT; index++) {
				if (GlobalOverride.Values[index]) {
					out->appendf(
						"%s=%s\n", COLOUR_NAMES[index], ColourText(*GlobalOverride.Values[index]).c_str()
					);
				}
			}
			out->append("\n");
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
		const PaletteSpec &spec = Spec(palette);

		// **The palette as declared, with no override on top**, which is the
		// question the picker is asking: "what does this theme look like", not
		// "what would the editor look like if I chose it". An override rides
		// over whichever palette is chosen, so folding it into all seven swatches
		// would make them differ by less and less as somebody customised, until
		// seven identical squares offered a choice between nothing.
		return PaletteSample{
			Pack(spec.Surface),
			Pack(Shade(spec.Surface, RAISED_SHADE)),
			Pack(spec.Accent),
		};
	}

	const char *Describe(ThemeColour colour) {
		return COLOUR_NAMES[static_cast<size_t>(colour) % THEME_COLOUR_COUNT];
	}

	std::optional<ThemeColour> ParseThemeColour(std::string_view name) {
		for (size_t index = 0; index < THEME_COLOUR_COUNT; index++) {
			if (name == COLOUR_NAMES[index]) {
				return static_cast<ThemeColour>(index);
			}
		}
		return std::nullopt;
	}

	bool ThemeColours::Any() const {
		for (const std::optional<unsigned int> &value : Values) {
			if (value) {
				return true;
			}
		}
		return false;
	}

	void ThemeColours::Clear() {
		for (std::optional<unsigned int> &value : Values) {
			value.reset();
		}
	}

	std::string ColourText(unsigned int packed) {
		char text[16] = {};
		std::snprintf(
			text,
			sizeof(text),
			"%02X%02X%02X%02X",
			packed & 0xFFu,
			(packed >> 8) & 0xFFu,
			(packed >> 16) & 0xFFu,
			(packed >> 24) & 0xFFu
		);
		return text;
	}

	std::optional<unsigned int> ParseColourText(std::string_view text) {
		if (!text.empty() && text.front() == '#') {
			text.remove_prefix(1);
		}
		if (text.size() != 6 && text.size() != 8) {
			return std::nullopt;
		}

		unsigned int bytes[4] = {0, 0, 0, 0xFFu};
		for (size_t index = 0; index * 2 < text.size(); index++) {
			unsigned int value = 0;
			for (size_t digit = 0; digit < 2; digit++) {
				const char character = text[index * 2 + digit];
				unsigned int nibble = 0;
				if (character >= '0' && character <= '9') {
					nibble = static_cast<unsigned int>(character - '0');
				} else if (character >= 'a' && character <= 'f') {
					nibble = static_cast<unsigned int>(character - 'a') + 10u;
				} else if (character >= 'A' && character <= 'F') {
					nibble = static_cast<unsigned int>(character - 'A') + 10u;
				} else {
					// **Refused rather than treated as zero.** A typo in a
					// colour would otherwise turn a panel black and look like a
					// bug in the theme rather than a bug in the file.
					return std::nullopt;
				}
				value = (value << 4) | nibble;
			}
			bytes[index] = value;
		}

		return IM_COL32(bytes[0], bytes[1], bytes[2], bytes[3]);
	}

	unsigned int ColourOf(ThemeColour colour) {
		return Pack(Live()[colour]);
	}

	unsigned int ColourOf(Palette palette, ThemeColour colour) {
		const PaletteSpec &spec = Spec(palette);
		switch (colour) {
		case ThemeColour::Surface:
			return Pack(spec.Surface);
		case ThemeColour::Accent:
			return Pack(spec.Accent);
		case ThemeColour::AccentHot:
			return Pack(spec.AccentHot);
		case ThemeColour::Text:
			return Pack(spec.Text);
		case ThemeColour::TextMuted:
			return Pack(spec.TextMuted);
		case ThemeColour::Warning:
			return Pack(DEFAULT_WARNING);
		case ThemeColour::Error:
			return Pack(DEFAULT_ERROR);
		}
		// No default label, so adding a colour is a warning here.
		return Pack(spec.Surface);
	}

	const ThemeColours &GlobalColours() {
		return GlobalOverride;
	}

	void SetGlobalColours(const ThemeColours &colours) {
		GlobalOverride = colours;

		// Restyles and persists, for the reasons `SetPalette` does both — and
		// through the same two calls, so that a caller changing a colour and a
		// caller changing a theme have nothing different to remember.
		if (ImGui::GetCurrentContext() == nullptr) {
			return;
		}

		ApplyEditorTheme(CurrentScale);
		ImGui::MarkIniSettingsDirty();
	}

	ScopedColours::ScopedColours(const ThemeColours &colours) {
		if (!colours.Any() || ImGui::GetCurrentContext() == nullptr) {
			return;
		}

		ImVec4 wanted[ImGuiCol_COUNT];
		Derive(Resolve(ChosenPalette, &colours), wanted);

		// **Only what differs from the live style.** A panel that moved its
		// accent shares thirty-odd slots with the editor around it, and pushing
		// those would be forty entries on imgui's colour stack every frame for
		// no visible change. The comparison is exact rather than approximate:
		// both sides came out of the same `Derive` from the same floats, so a
		// slot that is meant to be equal is bit-for-bit equal.
		const ImVec4 *live = ImGui::GetStyle().Colors;
		for (int slot = 0; slot < ImGuiCol_COUNT; slot++) {
			const ImVec4 &want = wanted[slot];
			const ImVec4 &have = live[slot];
			if (want.x == have.x && want.y == have.y && want.z == have.z && want.w == have.w) {
				continue;
			}
			ImGui::PushStyleColor(static_cast<ImGuiCol>(slot), want);
			Pushed++;
		}
	}

	ScopedColours::~ScopedColours() {
		if (Pushed > 0) {
			ImGui::PopStyleColor(Pushed);
		}
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

		// The palette, the editor's overrides, and the ladder over both. See
		// `Derive` for why the body of this lives in the anonymous namespace.
		ImGuiStyle &style = ImGui::GetStyle();
		Derive(Live(), style.Colors);

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

	// **These five read the resolved skin, not the palette.** They are what the
	// things that are *drawn* rather than styled ask for — a selection band, an
	// error line, a matched run in a filter — so a colour overridden for the
	// editor has to reach them too, or half the interface would take a new
	// accent and the other half would keep the old one.

	unsigned int AccentColour() {
		return Pack(Live()[ThemeColour::Accent]);
	}

	unsigned int WarningColour() {
		return Pack(Live()[ThemeColour::Warning]);
	}

	unsigned int ErrorColour() {
		return Pack(Live()[ThemeColour::Error]);
	}

	unsigned int MutedColour() {
		return Pack(Live()[ThemeColour::TextMuted]);
	}

	unsigned int LinkColour() {
		return Pack(Shade(Live()[ThemeColour::AccentHot], 1.18f));
	}

	unsigned int BrightColour() {
		// **White, except where white is wrong.** `Terminal`'s text is
		// phosphor green, and the brightest thing on it has to be a brighter
		// green rather than a colour from a different palette entirely.
		return Pack(Shade(Live()[ThemeColour::Text], 1.30f));
	}
}
