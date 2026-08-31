#include "ExternalEditor.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/script/Instances.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/ui/Fonts.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <studio/CodeMetrics.hpp>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	using engine::core::Name;
	using engine::ecs::Store;

	namespace {
		void DrawScriptSource(std::string_view text, const CodeEdit &edit, ImVec2 fieldMin, ImVec2 fieldSize);
	}

	void Editor::DrawScripts() {
		if (!ShowScripts) {
			return;
		}

		if (!ImGui::Begin("Script Editor", &ShowScripts)) {
			ImGui::End();
			return;
		}

		if (Scripts.empty()) {
			ImGui::TextDisabled("no script open");
			ImGui::TextDisabled("double-click a Script in the explorer, or insert one");
			ImGui::End();
			return;
		}

		size_t closing = Scripts.size();

		if (ImGui::BeginTabBar(
				"##scripts", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_TabListPopupButton
			)) {
			for (size_t index = 0; index < Scripts.size(); index++) {
				OpenScript &tab = Scripts[index];

				// The instance's *current* name, read every frame rather than
				// cached. A cached one is wrong for one frame after a rename,
				// and one frame is enough to be seen - `ui/AGENTS.md` states
				// that rule and this is the panel most tempted to break it.
				std::string label = "(deleted)";
				bool alive = false;

				if (tab.World.IsValid()) {
					Universe->Enter(tab.World, [&](Store &store) {
						if (!store.Alive(tab.Instance)) {
							return;
						}
						alive = true;
						const Name name = store.InstanceNameOf(tab.Instance);
						label = name.IsValid() ? std::string(Label(name)) : std::string("Script");
					});
				}

				if (tab.Modified) {
					label += " *";
				}

				// **`###` pins the tab's id to the instance, and this is not a
				// nicety.** imgui derives a widget's id from its label, so appending
				// the modified marker made the tab a *different tab* the instant
				// anybody typed - which closed the old one, rebuilt the field inside
				// it, and dropped keyboard focus after exactly one character. The
				// symptom was a script editor that took the first keystroke and
				// ignored every one after it, and it took a screenshot to see.
				label += "###";
				label += std::to_string(tab.Instance.Id);

				ImGui::PushID(static_cast<int>(index));

				bool open = true;
				if (ImGui::BeginTabItem(label.c_str(), &open)) {
					ActiveScript = static_cast<int>(index);

					if (!alive) {
						ImGui::TextDisabled("the script this tab was editing has been deleted");
					} else {
						RefreshExternalScript(tab);
						ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
						ImGui::Text(
							"%s   in %s",
							tab.Shader ? "GLSL fragment shader"
									   : (tab.Path.IsValid() ? Label(tab.Path)
															 : "(unsaved - a path is chosen on save)"),
							Label(Universe->NameOf(tab.World))
						);
						ImGui::PopStyleColor();

						ImGui::SameLine();
						if (ImGui::SmallButton("Save")) {
							SaveScriptTab(tab);
						}

						ImGui::SameLine();
						if (ImGui::SmallButton(tab.External.Active() ? "External Editor" : "Open External")) {
							OpenScriptExternally(tab);
						}

						// This tab's own scene: a script in a world being edited is
						// just text, whatever another scene is doing.
						if (ModeOf(tab.World) != RunMode::Edit) {
							ImGui::SameLine();
							ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
							// **Said plainly, because the alternative is a
							// mystery.** A running game already started its
							// scripts; editing the text now changes what runs
							// the *next* time, exactly as it does in Roblox, and
							// an editor that let somebody type into a live
							// script without saying so produces "my change did
							// nothing".
							ImGui::TextUnformatted("edits apply on the next run");
							ImGui::PopStyleColor();
						}

						// **The same control the output panel has**, rather
						// than a second copy of the step and the clamp - two
						// panels that disagreed about what a zoom level is
						// would disagree the first time either was tuned.
						ImGui::SameLine();
						DrawZoomControl(ScriptZoom, "code");

						ImGui::SameLine();
						if (ImGui::SmallButton(ShowFind ? "Hide Find" : "Find")) {
							ShowFind = !ShowFind;
						}

						ImGui::Separator();

						// **Find and Replace All, and deliberately not Find
						// Next.** Jumping the caret to a match means setting the
						// selection inside `InputTextMultiline`, which imgui
						// does not expose - reaching into `ImGuiInputTextState`
						// to do it would tie the script editor to a private
						// layout that changes between imgui releases. A match
						// count and a whole-file replace are the two thirds of
						// this that can be built honestly.
						if (ShowFind) {
							ImGui::SetNextItemWidth(180.0f * Settings.Scale);
							if (FocusFind) {
								ImGui::SetKeyboardFocusHere();
								FocusFind = false;
							}
							TextField("##find", FindText, "find");

							ImGui::SameLine();
							ImGui::SetNextItemWidth(180.0f * Settings.Scale);
							TextField("##replace", ReplaceText, "replace with");

							size_t matches = 0;
							if (!FindText.empty()) {
								for (size_t at = tab.Text.find(FindText); at != std::string::npos;
									 at = tab.Text.find(FindText, at + FindText.size())) {
									matches++;
								}
							}

							ImGui::SameLine();
							ImGui::BeginDisabled(matches == 0);
							if (ImGui::SmallButton("Replace All")) {
								std::string rebuilt;
								rebuilt.reserve(tab.Text.size());

								// Built once into a new string rather than
								// replaced in place: replacing in place while
								// scanning re-finds the replacement when it
								// contains the needle, which is an editor that
								// hangs on "a" -> "aa".
								size_t at = 0;
								for (size_t found = tab.Text.find(FindText, at); found != std::string::npos;
									 found = tab.Text.find(FindText, at)) {
									rebuilt.append(tab.Text, at, found - at);
									rebuilt.append(ReplaceText);
									at = found + FindText.size();
								}
								rebuilt.append(tab.Text, at, std::string::npos);

								tab.Text = rebuilt;
								tab.Modified = true;
								Say("replaced " + std::to_string(matches) + " occurrence(s) in " +
									std::string(Label(tab.Path)));
							}
							ImGui::EndDisabled();

							ImGui::SameLine();
							ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
							if (FindText.empty()) {
								ImGui::TextUnformatted("type something to find");
							} else {
								ImGui::Text("%zu match(es)", matches);
							}
							ImGui::PopStyleColor();

							ImGui::Separator();
						}

						if (tab.External.Conflict) {
							ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
							ImGui::TextUnformatted("Studio and the external editor changed this file");
							ImGui::PopStyleColor();
							ImGui::SameLine();
							if (ImGui::SmallButton("Use External")) {
								std::string error;
								if (AcceptExternalDocument(tab.External, tab.Text, error)) {
									tab.Modified = true;
								} else {
									Say("external editor: " + error, engine::core::LogLevel::Warning);
								}
							}
							ImGui::SameLine();
							if (ImGui::SmallButton("Keep Studio")) {
								std::string error;
								if (!KeepStudioDocument(tab.External, tab.Text, error)) {
									Say("external editor: " + error, engine::core::LogLevel::Warning);
								}
							}
							ImGui::Separator();
						}

						// **The monospace face, which is what makes this a code
						// editor rather than a text box.** Columns line up, an
						// `l` is not an `I`, and indentation is a width rather
						// than a guess. `mono.studio/AGENTS.md` listed the
						// absence of one as a deferred gap with the reason being
						// a font this repository did not have; it has four now.
						//
						// **Zoom is the size this is pushed at, rather than a
						// window scale laid over it.** `Options::Scale` rebuilds
						// every metric in the editor and needs a restart to
						// rasterise the faces at the new size; this is one
						// panel's text, and wanting bigger code is not wanting a
						// bigger properties panel.
						//
						// It has to be the pushed size specifically:
						// `SetWindowFontScale` scales the window it is called
						// on, and both the code and the gutter draw into child
						// windows - which imgui begins at scale 1 whatever their
						// parent was set to. That zoomed the frame and the row
						// spacing measured out here, and left every glyph inside
						// them exactly the size it started at.
						//
						// Everything below draws inside this scope, so the
						// gutter's row height and the field's line height come
						// from one number and stay in step.
						const engine::ui::ScopedFont code(
							engine::ui::Typeface::Monospace, engine::ui::TextSize::Body, ScriptZoom
						);

						// **The gutter, and it is a sibling of the code rather
						// than part of it.** `CodeField` is an
						// `InputTextMultiline`, which owns its own scrolling
						// child - so a breakpoint column has to be drawn beside
						// it and told where that child has scrolled to.
						//
						// See `DrawScriptGutter` for why reading one window's
						// scroll is a different kind of reach into imgui from
						// the one this file refuses two hundred lines up.
						const float gutter = DrawScriptGutter(tab);

						ImGui::SameLine(0.0f, 0.0f);

						// **The navigation keys are claimed before the field is
						// submitted, not after.** The text widget polls them
						// during its own submission, so an owner set afterwards
						// would arrive a frame late - Down would move the caret
						// a line *and* the highlighted row, and Enter would
						// insert a newline before accepting. `LockThisFrame` is
						// what makes the widget's owner-agnostic poll fail
						// rather than win.
						// **Tab is on the list, and it can be.** The field
						// polls Tab through `Shortcut` against its own id -
						// the `AllowTabInput`/`CallbackCompletion` assert is
						// about the *flags*, not the key - so the same
						// `LockThisFrame` that keeps Enter out of the text
						// keeps Tab out of the indentation, and Tab accepts
						// exactly while the popup is up.
						const ImGuiID popupId = ImGui::GetID("##completion");
						if (ScriptPopupOpen) {
							for (const ImGuiKey key :
								 {ImGuiKey_UpArrow,
								  ImGuiKey_DownArrow,
								  ImGuiKey_Enter,
								  ImGuiKey_KeypadEnter,
								  ImGuiKey_Tab,
								  ImGuiKey_Escape}) {
								ImGui::SetKeyOwner(key, popupId, ImGuiInputFlags_LockThisFrame);
							}
						}

						const ImVec2 fieldMin = ImGui::GetCursorScreenPos();

						// The minimap's column, taken off the field's width
						// up front. Fixed rather than zoomed: it is an
						// overview of the file, not text somebody reads, so
						// only the interface scale sizes it - and a panel too
						// narrow to share simply keeps the whole width for
						// the code.
						const float minimapWidth = 72.0f * Settings.Scale;
						const float available = ImGui::GetContentRegionAvail().x;
						const bool minimapFits = Prefs.ScriptMinimap && available > minimapWidth * 3.0f;

						if (Prefs.ScriptBackground.has_value()) {
							ImGui::PushStyleColor(ImGuiCol_FrameBg, *Prefs.ScriptBackground);
						}
						ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 0));
						const bool changed = CodeField(
							"##text",
							tab.Text,
							&tab.Edit,
							minimapFits ? available - minimapWidth : -1.0f,
							-1.0f
						);
						ImGui::PopStyleColor();
						if (Prefs.ScriptBackground.has_value()) {
							ImGui::PopStyleColor();
						}
						DrawScriptSource(tab.Text, tab.Edit, fieldMin, ImGui::GetItemRectSize());

						// Read here, while the field is still the last item,
						// so imgui's own rest delay decides when a tooltip is
						// wanted; the drawing happens after the completion
						// has had its turn.
						const bool resting = ImGui::IsItemHovered(
							ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary
						);

						if (changed) {
							tab.Modified = true;
						}
						(void)gutter;

						if (minimapFits) {
							ImGui::SameLine(0.0f, 0.0f);
							DrawScriptMinimap(tab, minimapWidth);
						}

						// Ctrl+Space asks for the list whatever is under the
						// caret, which is the binding every editor has and the
						// way through when the automatic rules decline.
						const bool asked =
							tab.Edit.Active && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Space);

						UpdateScriptCompletion(tab, changed, asked);

						if (ScriptPopupOpen) {
							DrawScriptCompletion(tab, fieldMin, popupId);
						}

						DrawScriptHover(tab, fieldMin, resting);

						// Ctrl+wheel over the text, which is what every editor
						// binds it to.
						ApplyZoomWheel(ScriptZoom);

						// Ctrl+S inside the editor saves the *script*, not the
						// game. The menu bar's shortcut is guarded on
						// `WantTextInput`, so the two do not both fire.
						if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
							ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) {
							SaveScriptTab(tab);
						}
					}

					ImGui::EndTabItem();
				}

				ImGui::PopID();

				if (!open) {
					closing = index;
				}
			}
			ImGui::EndTabBar();
		}

		ImGui::End();

		if (closing < Scripts.size()) {
			// Saved on the way out rather than dropped. An editor that lost
			// what you typed because you closed the tab is an editor nobody
			// trusts twice.
			if (Scripts[closing].Modified) {
				SaveScriptTab(Scripts[closing]);
			}
			CloseScriptTab(closing);
		}
	}

	namespace {

		// What a VM of one language installs, walked once.
		//
		// **Built from a throwaway runtime rather than written down.** A list of
		// globals kept in the editor would be a copy of a surface that lives in
		// fifteen source files, and `script/Vocabulary.hpp` records the two
		// times this engine has shipped exactly that copy out of step. Making a
		// VM costs a few milliseconds and answers correctly forever after.
		//
		// **On first use rather than at start-up.** Somebody who never opens a
		// script never pays for it, which is the same rule the editor learned
		// the expensive way when fetching every asset by kind put half a minute
		// in front of the first frame.
		const engine::script::ScriptSurface &SurfaceFor(const engine::script::Language language) {
			const auto walk = [](const engine::script::Language which) {
				// The store is local: nothing is created in it and it is gone
				// before this returns. `ScriptSurface` is plain strings.
				engine::ecs::Store store("completion");
				const std::unique_ptr<engine::script::Runtime> runtime =
					engine::script::MakeRuntime(store, which);

				return runtime != nullptr ? runtime->Surface() : engine::script::ScriptSurface{};
			};

			static const engine::script::ScriptSurface luau = walk(engine::script::Language::Luau);
			static const engine::script::ScriptSurface javascript =
				walk(engine::script::Language::JavaScript);

			return language == engine::script::Language::Luau ? luau : javascript;
		}

		enum class ScriptToken : uint8_t {
			Plain,
			Keyword,
			String,
			Comment,
			Number,
			Type,
		};

		bool IsScriptKeyword(const std::string_view word) {
			static constexpr std::array<std::string_view, 31> KEYWORDS{
				"and",	 "break",  "continue", "do",	 "else", "elseif", "end",	"export",
				"false", "for",	   "function", "if",	 "in",	 "local",  "nil",	"not",
				"or",	 "repeat", "return",   "then",	 "true", "type",   "until", "while",
				"class", "const",  "extends",  "import", "new",	 "this",   "throw",
			};
			return std::find(KEYWORDS.begin(), KEYWORDS.end(), word) != KEYWORDS.end();
		}

		bool IsScriptType(const std::string_view word) {
			return word == "any" || word == "boolean" || word == "number" || word == "string" ||
				   word == "table" || word == "thread" || word == "userdata" || word == "Vector2" ||
				   word == "Vector3" || word == "CFrame" || word == "Color3" || word == "Instance" ||
				   word == "Enum" || word == "UDim2" || word == "BrickColor";
		}

		ScriptToken TokenFor(const std::string_view word) {
			if (IsScriptKeyword(word)) {
				return ScriptToken::Keyword;
			}
			if (IsScriptType(word)) {
				return ScriptToken::Type;
			}
			return ScriptToken::Plain;
		}

		unsigned int ColourFor(const ScriptToken token) {
			switch (token) {
			case ScriptToken::Keyword:
				return engine::ui::ColourOf(engine::ui::ThemeColour::ScriptKeyword);
			case ScriptToken::String:
				return engine::ui::ColourOf(engine::ui::ThemeColour::ScriptString);
			case ScriptToken::Comment:
				return engine::ui::ColourOf(engine::ui::ThemeColour::ScriptComment);
			case ScriptToken::Number:
				return engine::ui::ColourOf(engine::ui::ThemeColour::ScriptNumber);
			case ScriptToken::Type:
				return engine::ui::ColourOf(engine::ui::ThemeColour::ScriptType);
			case ScriptToken::Plain:
				return engine::ui::ColourOf(engine::ui::ThemeColour::Text);
			}
			return engine::ui::ColourOf(engine::ui::ThemeColour::Text);
		}

		void DrawScriptSource(
			const std::string_view text, const CodeEdit &edit, const ImVec2 fieldMin, const ImVec2 fieldSize
		) {
			ImGuiWindow *code = ImGui::FindWindowByName("##text");
			if (code == nullptr) {
				return;
			}

			const float rowHeight = ImGui::GetTextLineHeight();
			const float glyph = ImGui::CalcTextSize("0").x;
			const ImVec2 padding = ImGui::GetStyle().FramePadding;
			const ImVec2 origin(
				fieldMin.x + padding.x - code->Scroll.x, fieldMin.y + padding.y - code->Scroll.y
			);
			// Append to the multiline field's child window. Drawing on the parent
			// puts the child background over the syntax layer when ImGui submits
			// its windows.
			ImDrawList *draw = code->DrawList;
			draw->PushClipRect(fieldMin, ImVec2(fieldMin.x + fieldSize.x, fieldMin.y + fieldSize.y), true);

			size_t lineStart = 0;
			size_t line = 0;
			while (lineStart <= text.size()) {
				const size_t lineEnd = text.find('\n', lineStart);
				const size_t end = lineEnd == std::string_view::npos ? text.size() : lineEnd;
				size_t at = lineStart;
				while (at < end) {
					const char character = text[at];
					ScriptToken token = ScriptToken::Plain;
					size_t tokenEnd = at + 1;

					if ((character == '-' && at + 1 < end && text[at + 1] == '-') ||
						(character == '/' && at + 1 < end && text[at + 1] == '/')) {
						token = ScriptToken::Comment;
						tokenEnd = end;
					} else if (character == '\'' || character == '"' || character == '`') {
						token = ScriptToken::String;
						const char quote = character;
						while (tokenEnd < end) {
							if (text[tokenEnd] == '\\') {
								tokenEnd = std::min(end, tokenEnd + 2);
								continue;
							}
							if (text[tokenEnd++] == quote) {
								break;
							}
						}
					} else if (std::isdigit(static_cast<unsigned char>(character))) {
						token = ScriptToken::Number;
						while (tokenEnd < end && (std::isalnum(static_cast<unsigned char>(text[tokenEnd])) ||
												  text[tokenEnd] == '.')) {
							tokenEnd++;
						}
					} else if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
						while (tokenEnd < end && (std::isalnum(static_cast<unsigned char>(text[tokenEnd])) ||
												  text[tokenEnd] == '_')) {
							tokenEnd++;
						}
						token = TokenFor(text.substr(at, tokenEnd - at));
					}

					const size_t column = ColumnAt(text, at);
					const ImVec2 position(
						origin.x + static_cast<float>(column) * glyph,
						origin.y + static_cast<float>(line) * rowHeight
					);
					draw->AddText(
						ImGui::GetFont(),
						ImGui::GetFontSize(),
						position,
						ColourFor(token),
						text.data() + at,
						text.data() + tokenEnd
					);
					at = tokenEnd;
				}

				if (lineEnd == std::string_view::npos) {
					break;
				}
				lineStart = lineEnd + 1;
				line++;
			}

			if (edit.Active && edit.SelectionStart == edit.SelectionEnd) {
				const size_t caret =
					static_cast<size_t>(std::clamp(edit.Caret, 0, static_cast<int>(text.size())));
				const std::string_view before = text.substr(0, caret);
				const float caretLine = static_cast<float>(std::count(before.begin(), before.end(), '\n'));
				const float caretColumn = static_cast<float>(ColumnAt(text, caret));
				const float x = origin.x + caretColumn * glyph;
				const float y = origin.y + caretLine * rowHeight;
				draw->AddLine(ImVec2(x, y), ImVec2(x, y + rowHeight), ColourFor(ScriptToken::Plain), 1.0f);
			}

			draw->PopClipRect();
		}

	}

	std::vector<std::string> Editor::ScriptSiblings(const OpenScript &tab) {
		std::vector<std::string> names;

		if (!tab.World.IsValid()) {
			return names;
		}

		Universe->Enter(tab.World, [&](Store &store) {
			if (!store.Alive(tab.Instance)) {
				return;
			}

			const auto record = [&](const engine::ecs::Entity child) {
				if (const Name name = store.InstanceNameOf(child); name.IsValid()) {
					names.emplace_back(Label(name));
				}
			};

			// The script's own siblings, which is what `script.Parent.` reaches.
			if (const engine::ecs::Entity parent = store.ParentOf(tab.Instance);
				parent != engine::ecs::NULL_ENTITY) {
				store.EachChild(parent, record);
			} else {
				store.EachRoot(record);
			}

			// The world's roots as well, because `workspace.` is the other
			// spelling somebody reaches for and it is the same list.
			store.EachRoot(record);
		});

		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		return names;
	}

	void Editor::UpdateScriptCompletion(OpenScript &tab, const bool textChanged, const bool asked) {
		// A field nobody is typing in has no caret worth reading, and a popup
		// left up over an unfocused editor is a panel that looks stuck.
		if (!tab.Edit.Active) {
			ScriptPopupOpen = false;
			ScriptCompletions.clear();
			return;
		}

		const bool moved = tab.Edit.Caret != ScriptPopupCaret;
		if (!moved && !textChanged && !asked) {
			return;
		}

		ScriptPopupCaret = tab.Edit.Caret;

		const auto caret = static_cast<size_t>(std::max(0, tab.Edit.Caret));
		const CompletionQuery query = ScanBackwards(tab.Text, caret);

		// **Opened on a separator, on one character, or on being asked.** A
		// separator is unambiguous - nobody types `part.` meaning to stop there.
		//
		// **One and not two, which was a deliberate reversal at v0.15.** The
		// argument for two was that one character offers every identifier in the
		// file at once and covers the line being written. That is true and it is
		// not what an author feels: what they feel is a keystroke of lag between
		// starting a name and the editor admitting it knows any, which reads as
		// the completion being broken rather than as it being polite. The list
		// filters down on the very next character anyway, so the cost is one
		// crowded frame and the benefit is that "as you type" means what it says.
		//
		// The escape from a popup that is in the way is Escape, and it already
		// works - `DrawScriptCompletion` owns that key while the popup is up.
		const bool worthOpening =
			asked || ScriptPopupOpen || query.Separator != '\0' || !query.Prefix.empty();

		if (!worthOpening) {
			ScriptPopupOpen = false;
			ScriptCompletions.clear();
			return;
		}

		CompletionSources sources;
		std::vector<std::string> children;
		if (tab.Shader) {
			sources.Domain = CompletionDomain::Shader;
			ScriptPopupDomain = CompletionDomain::Shader;
		} else {
			// **The language the script will actually run as**, which is the
			// selector's answer and not the file extension's.
			engine::script::Language language = engine::script::LanguageOf(tab.Path.Text());
			if (tab.World.IsValid()) {
				Universe->Enter(tab.World, [&](Store &store) {
					if (store.Alive(tab.Instance)) {
						language = engine::script::ActiveLanguageOf(store, tab.Instance);
					}
				});
			}

			children = ScriptSiblings(tab);
			sources.Language = language;
			sources.Surface = &SurfaceFor(language);
			sources.Children = children;
			ScriptPopupLanguage = language;
			ScriptPopupDomain = CompletionDomain::Script;
		}

		ScriptCompletions = CompleteAt(tab.Text, caret, sources);

		// Nothing to say is a closed popup rather than an empty box.
		ScriptPopupOpen = !ScriptCompletions.empty();
		ScriptPopupAnchor = static_cast<int>(caret - query.Prefix.size());

		// **Back to the top whenever the list itself changed, and clamped only
		// when it did not.** A highlight is a position in a list, so carrying the
		// *index* across a re-query points it at whatever now happens to sit
		// there. Typing another letter narrows the offers, so an author who had
		// arrowed down to the sixth entry and kept typing was left highlighting
		// the sixth entry of a different list - and Enter inserted it without
		// anything on screen having looked wrong.
		//
		// The clamp is still what a caret move wants: moving along a line
		// re-queries with the same prefix and the same offers, and resetting
		// there would fight the arrow keys.
		if (textChanged || asked) {
			ScriptPopupChoice = 0;
		} else {
			ScriptPopupChoice =
				std::clamp(ScriptPopupChoice, 0, static_cast<int>(ScriptCompletions.size()) - 1);
		}
	}

	void Editor::DrawScriptCompletion(OpenScript &tab, const ImVec2 fieldMin, const unsigned int popupId) {
		const auto count = static_cast<int>(ScriptCompletions.size());
		if (count == 0) {
			ScriptPopupOpen = false;
			return;
		}

		// **Read with the popup's own id**, because the keys were locked to it
		// before the field ran. Polling them the ordinary way would be polling
		// as "anybody", which `LockThisFrame` is precisely what refuses.
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, popupId)) {
			ScriptPopupOpen = false;
			return;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, popupId)) {
			ScriptPopupChoice = (ScriptPopupChoice + 1) % count;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, popupId)) {
			ScriptPopupChoice = (ScriptPopupChoice + count - 1) % count;
		}

		// Tab accepts as well, as it does in VS Code. It only reaches here
		// while the popup owns it - `DrawScripts` locks it beside Enter - so
		// with the popup closed Tab still indents.
		bool accept = ImGui::IsKeyPressed(ImGuiKey_Enter, ImGuiInputFlags_None, popupId) ||
					  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, ImGuiInputFlags_None, popupId) ||
					  ImGui::IsKeyPressed(ImGuiKey_Tab, ImGuiInputFlags_None, popupId);

		// **Where the caret is, computed rather than asked for, and exact
		// because the face is monospace.** Every glyph is one advance wide, so a
		// column is a multiplication - `ScriptEditor` pushes
		// `Typeface::Monospace` a few lines above this and that is what makes it
		// true. A proportional face would need the run measured, which is the
		// sort of thing that is nearly right and drifts a character per line.
		// `ColumnAt` counts a tab as the four columns imgui draws it as, so an
		// indented line places the popup under the caret rather than left of it.
		const auto caret = static_cast<size_t>(std::max(0, tab.Edit.Caret));
		const std::string_view before =
			std::string_view(tab.Text).substr(0, std::min(caret, tab.Text.size()));

		const auto line = static_cast<float>(std::count(before.begin(), before.end(), '\n'));
		const auto column = static_cast<float>(ColumnAt(tab.Text, caret));

		const float rowHeight = ImGui::GetTextLineHeight();
		const float glyph = ImGui::CalcTextSize("0").x;

		// The field's own scroll, from the window it made - the same lookup and
		// the same justification `DrawScriptGutter` gives above.
		ImVec2 scroll(0.0f, 0.0f);
		if (const ImGuiWindow *code = ImGui::FindWindowByName("##text"); code != nullptr) {
			scroll = ImVec2(code->Scroll.x, code->Scroll.y);
		}

		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		const ImVec2 caretTop(
			fieldMin.x + padding.x + (column * glyph) - scroll.x,
			fieldMin.y + padding.y + (line * rowHeight) - scroll.y
		);
		ImVec2 position(caretTop.x, caretTop.y + rowHeight);

		// One footer row under the list: what kind of thing is chosen, and a
		// keyword's doc line when it has one. The size accounts for it up
		// front because the window's size is set, not measured.
		const int rows = std::min(count, 10);
		const float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 1.0f;
		const ImVec2 size(
			320.0f * Settings.Scale,
			(static_cast<float>(rows) * ImGui::GetTextLineHeightWithSpacing()) + (padding.y * 2.0f) + footer
		);

		// Kept on screen. A popup near the right edge or the last line would
		// otherwise open where nobody can read it, which is the state an author
		// hits on the longest line in the file.
		const ImGuiViewport *viewport = ImGui::GetMainViewport();
		const ImVec2 workMin = viewport->WorkPos;
		const ImVec2 workMax(
			viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y
		);
		const float belowY = caretTop.y + rowHeight;
		const float aboveY = caretTop.y - size.y;
		const bool fitsBelow = belowY + size.y <= workMax.y;
		const bool fitsAbove = aboveY >= workMin.y;

		// Prefer below the caret, then above it when the editor is near the
		// bottom edge. Clamping both axes also handles a narrow or scrolled
		// viewport where neither side has the full popup height available.
		position.y = !fitsBelow && fitsAbove ? aboveY : belowY;
		position.x = std::clamp(position.x, workMin.x, std::max(workMin.x, workMax.x - size.x));
		position.y = std::clamp(position.y, workMin.y, std::max(workMin.y, workMax.y - size.y));

		ImGui::SetNextWindowPos(position);
		ImGui::SetNextWindowSize(size);

		// **`NoFocusOnAppearing` and no `Begin` flags that take input.** The
		// text field must keep the keyboard: a window that focused itself would
		// stop the typing that opened it, which is the whole reason this is not
		// `BeginPopup`.
		constexpr ImGuiWindowFlags FLAGS = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
										   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
										   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavInputs;

		// What the author has typed of the word being completed, for the
		// matched-prefix highlight. The anchor is where the word began.
		std::string_view typed;
		if (ScriptPopupAnchor >= 0 && caret >= static_cast<size_t>(ScriptPopupAnchor)) {
			typed = std::string_view(tab.Text).substr(
				static_cast<size_t>(ScriptPopupAnchor), caret - static_cast<size_t>(ScriptPopupAnchor)
			);
		}

		if (ImGui::Begin("##completion", nullptr, FLAGS)) {
			// The rows in their own child so the footer under them stays put
			// while a long list scrolls.
			const float listHeight = static_cast<float>(rows) * ImGui::GetTextLineHeightWithSpacing();
			ImGui::BeginChild("##rows", ImVec2(0.0f, listHeight), false);

			for (int index = 0; index < count; index++) {
				const Completion &entry = ScriptCompletions[static_cast<size_t>(index)];

				ImGui::PushID(index);
				const ImVec2 rowPosition = ImGui::GetCursorScreenPos();
				if (ImGui::Selectable(entry.Text.c_str(), index == ScriptPopupChoice)) {
					ScriptPopupChoice = index;
					accept = true;
				}

				// **The typed prefix re-drawn in the accent colour over the
				// label**, glyph for glyph - exact because the face is
				// monospace and the label starts at the cursor the row was
				// submitted at. Only a real prefix is marked: `FuzzyMatch`
				// also admits subsequences, and marking those would need the
				// match positions it deliberately does not report.
				if (!typed.empty() && entry.Text.size() >= typed.size()) {
					const auto samePrefix = std::equal(
						typed.begin(),
						typed.end(),
						entry.Text.begin(),
						[](const char left, const char right) {
							return std::tolower(static_cast<unsigned char>(left)) ==
								   std::tolower(static_cast<unsigned char>(right));
						}
					);
					if (samePrefix) {
						ImGui::GetWindowDrawList()->AddText(
							rowPosition,
							engine::ui::AccentColour(),
							entry.Text.c_str(),
							entry.Text.c_str() + typed.size()
						);
					}
				}

				// Scrolled to rather than merely highlighted, so arrowing past
				// the tenth row still shows what is chosen.
				if (index == ScriptPopupChoice && (ImGui::IsWindowAppearing() || !ImGui::IsItemVisible())) {
					ImGui::SetScrollHereY(0.5f);
				}

				if (!entry.Detail.empty()) {
					ImGui::SameLine();
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
					ImGui::TextUnformatted(entry.Detail.c_str());
					ImGui::PopStyleColor();
				}
				ImGui::PopID();
			}

			ImGui::EndChild();

			// The footer: what the chosen row is, in the vocabulary the rest
			// of the editor uses, plus a keyword's one-line doc. A property's
			// type and owner are already its `Detail`, so the footer says the
			// kind rather than repeating them.
			const Completion &picked = ScriptCompletions[static_cast<size_t>(ScriptPopupChoice)];
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());

			const std::string_view doc = picked.Kind != CompletionKind::Keyword
											 ? std::string_view{}
											 : (ScriptPopupDomain == CompletionDomain::Shader
													? ShaderKeywordDoc(picked.Text)
													: KeywordDoc(ScriptPopupLanguage, picked.Text));
			if (!doc.empty()) {
				ImGui::TextUnformatted(doc.data(), doc.data() + doc.size());
			} else {
				const std::string_view kind = Describe(picked.Kind);
				ImGui::TextUnformatted(kind.data(), kind.data() + kind.size());
			}
			ImGui::PopStyleColor();
		}
		ImGui::End();

		if (!accept) {
			return;
		}

		// **Requested rather than written.** `CodeEdit::Insert` is applied
		// inside the field's own callback next frame, which keeps imgui's undo
		// stack describing text that is actually there - see `Widgets.hpp`.
		const Completion &chosen = ScriptCompletions[static_cast<size_t>(ScriptPopupChoice)];
		tab.Edit.Insert = chosen.Text;
		tab.Edit.ReplaceFrom = ScriptPopupAnchor;
		tab.Modified = true;

		ScriptPopupOpen = false;
		ScriptPopupChoice = 0;
		ScriptCompletions.clear();
	}

	float Editor::DrawScriptGutter(const OpenScript &tab) {
		// **One column of line numbers, and a click on one toggles a
		// breakpoint.** That is where every editor puts it, and it is the
		// difference between a debugger somebody uses and one they read about:
		// typing a path and a line number into a panel is a thing you do once to
		// see if it works.
		const ImVec2 area = ImGui::GetContentRegionAvail();
		const float rowHeight = ImGui::GetTextLineHeight();

		// Wide enough for the largest line number this file has, plus room for
		// the dot. Measured rather than guessed, so a four-thousand-line script
		// does not push its numbers into the code.
		size_t lines = 1;
		for (const char character : tab.Text) {
			lines += character == '\n' ? 1 : 0;
		}

		const std::string widest = std::to_string(lines);
		const float width = ImGui::CalcTextSize(widest.c_str()).x + rowHeight + 8.0f;

		ImGui::BeginChild("##gutter", ImVec2(width, area.y), false, ImGuiWindowFlags_NoScrollbar);

		// **The code field's own scroll, read from the window it made.**
		//
		// This is `imgui_internal.h`, and it is worth saying why it is allowed
		// here when this file refuses `ImGuiInputTextState` a couple of hundred
		// lines up. That struct is a private *layout* - its fields move between
		// releases and reading one is reading whatever happens to be at an
		// offset. A window's `Scroll` is the same value `GetScrollY` returns for
		// the current window in public API; what is internal is only *looking
		// one up by name*, and a name imgui derives from a label it was given.
		//
		// The failure mode is also benign in a way the other is not: a lookup
		// that stops working answers null, and the gutter draws from the top
		// rather than misreading memory.
		float scroll = 0.0f;
		if (const ImGuiWindow *code = ImGui::FindWindowByName("##text"); code != nullptr) {
			scroll = code->Scroll.y;
		}

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImDrawList *draw = ImGui::GetWindowDrawList();

		// Whether this script could carry one at all. Read once rather than per
		// row, because it is a property of the file.
		const bool breakable = !tab.Shader && engine::script::BreakpointsRefused(tab.Path.Text()).empty();

		// Only the rows on screen. A script of ten thousand lines would
		// otherwise cost ten thousand hit-tests a frame to draw forty of them.
		const auto first = static_cast<size_t>(std::max(0.0f, scroll) / rowHeight);
		const auto visible = static_cast<size_t>(area.y / rowHeight) + 2;

		for (size_t row = first; row < std::min(lines, first + visible); row++) {
			const auto line = static_cast<int>(row) + 1;
			const float y = origin.y + (static_cast<float>(row) * rowHeight) - scroll;

			// **An invisible button per row rather than one hit-test over the
			// column**, so imgui owns the hover and the click and this does not
			// have to reimplement either.
			ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
			ImGui::PushID(line);

			const bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, rowHeight));
			const bool hovered = ImGui::IsItemHovered();

			ImGui::PopID();

			const engine::script::Breakpoint *point = BreakpointAt(tab.Path, line);

			if (pressed) {
				ToggleBreakpoint(tab.Path, line);
			}

			if (point != nullptr) {
				// Filled when it is armed and hollow when it is not, which is
				// what every editor draws and what makes "switched off" visible
				// rather than absent.
				const ImVec2 centre(origin.x + (rowHeight * 0.5f), y + (rowHeight * 0.5f));
				const ImU32 colour = point->Action == engine::script::BreakAction::Stop
										 ? IM_COL32(220, 90, 90, 255)
										 : IM_COL32(220, 160, 60, 255);

				if (point->Enabled) {
					draw->AddCircleFilled(centre, rowHeight * 0.32f, colour);
				} else {
					draw->AddCircle(centre, rowHeight * 0.32f, colour, 0, 1.5f);
				}
			} else if (hovered && breakable) {
				// A hollow mark under the cursor, so the column reads as
				// clickable before anything has been clicked - and not offered
				// at all on a script no breakpoint could fire in, because an
				// affordance that refuses when used is worse than none.
				const ImVec2 centre(origin.x + (rowHeight * 0.5f), y + (rowHeight * 0.5f));
				draw->AddCircle(centre, rowHeight * 0.32f, engine::ui::MutedColour(), 0, 1.0f);
			}

			const std::string label = std::to_string(line);
			const float text = ImGui::CalcTextSize(label.c_str()).x;

			draw->AddText(
				ImVec2(origin.x + width - text - 4.0f, y), engine::ui::MutedColour(), label.c_str()
			);
		}

		ImGui::EndChild();
		return width;
	}

	void Editor::DrawScriptMinimap(const OpenScript &tab, const float width) {
		const ImVec2 area = ImGui::GetContentRegionAvail();

		ImGui::BeginChild("##minimap", ImVec2(width, area.y), false, ImGuiWindowFlags_NoScrollbar);

		size_t lines = 1;
		for (const char character : tab.Text) {
			lines += character == '\n' ? 1 : 0;
		}

		const float rowHeight = ImGui::GetTextLineHeight();

		// The code field's scroll, read from the window it made - the same
		// lookup, the same justification and the same benign fallback
		// `DrawScriptGutter` documents above.
		float scroll = 0.0f;
		ImGuiWindow *code = ImGui::FindWindowByName("##text");
		if (code != nullptr) {
			scroll = code->Scroll.y;
		}

		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float mapRow = MinimapRowHeight(lines, area.y, 2.0f * Settings.Scale);

		// The first hundred columns of the file, spread across the width.
		// Longer lines are cut rather than squeezed: a minimap answers "where
		// is the shape I remember", and shapes stop being recognisable when
		// their scale depends on the longest line in the file.
		constexpr float MAP_COLUMNS = 100.0f;
		const float mapGlyph = width / MAP_COLUMNS;

		// One invisible button over the whole column, so imgui owns the
		// hover, the click and the drag capture. Scroll-to-click while it is
		// held is what makes the map a scrollbar as well as a picture.
		ImGui::InvisibleButton("##map", ImVec2(width, std::max(area.y, 1.0f)));
		if (ImGui::IsItemActive() && code != nullptr && rowHeight > 0.0f) {
			const float picked = (ImGui::GetMousePos().y - origin.y) / mapRow;

			// Writing the field's scroll is the same reach as reading it:
			// internal `SetScrollY(window, ...)` sets what public `SetScrollY`
			// sets on the current window, and the only internal part is
			// addressing the window by name - which already answered non-null
			// on the line above, or this does nothing.
			ImGui::SetScrollY(code, MinimapScrollFor(picked, lines, rowHeight, area.y));
		}

		ImDrawList *draw = ImGui::GetWindowDrawList();

		// Two intensities and not a highlighter. The code field tints
		// nothing, so the honest minimap is a density map: identifier runs
		// bright, punctuation dim, whitespace empty.
		const unsigned int wordColour = engine::ui::MutedColour();
		const unsigned int dimColour = (wordColour & ~IM_COL32_A_MASK) | (0x50u << IM_COL32_A_SHIFT);

		// When the file is compressed below a pixel per line, lines are
		// stepped over rather than overdrawn - the work stays proportional to
		// the map's pixels, never to the file.
		const auto stride = std::max<size_t>(1, static_cast<size_t>(1.0f / mapRow));
		const float runHeight = std::max(mapRow * 0.7f, 1.0f);

		const std::string_view text(tab.Text);
		size_t lineStart = 0;
		for (size_t row = 0; lineStart <= text.size(); row++) {
			const size_t newline = text.find('\n', lineStart);
			const size_t lineEnd = newline == std::string_view::npos ? text.size() : newline;

			const float y = origin.y + (static_cast<float>(row) * mapRow);
			if (y > origin.y + area.y) {
				break;
			}

			if (row % stride == 0) {
				MinimapRunsOf(
					text.substr(lineStart, lineEnd - lineStart),
					static_cast<size_t>(MAP_COLUMNS),
					ScriptMinimapRuns
				);
				for (const MinimapRun &run : ScriptMinimapRuns) {
					const float x = origin.x + (static_cast<float>(run.Column) * mapGlyph);
					draw->AddRectFilled(
						ImVec2(x, y),
						ImVec2(x + (static_cast<float>(run.Columns) * mapGlyph), y + runHeight),
						run.Word ? wordColour : dimColour
					);
				}
			}

			if (newline == std::string_view::npos) {
				break;
			}
			lineStart = newline + 1;
		}

		// The visible region, drawn over the runs so what the code field
		// shows is findable in the picture of the whole.
		if (rowHeight > 0.0f) {
			const float viewTop = origin.y + ((scroll / rowHeight) * mapRow);
			const float viewHeight = std::max((area.y / rowHeight) * mapRow, 2.0f);
			const unsigned int viewColour =
				(engine::ui::BrightColour() & ~IM_COL32_A_MASK) | (0x20u << IM_COL32_A_SHIFT);
			draw->AddRectFilled(
				ImVec2(origin.x, viewTop),
				ImVec2(origin.x + width, std::min(viewTop + viewHeight, origin.y + area.y)),
				viewColour
			);
		}

		ImGui::EndChild();
	}

	void Editor::DrawScriptHover(OpenScript &tab, const ImVec2 fieldMin, const bool hovered) {
		// The popup wins outright: two floating answers at the caret would
		// fight for the same spot and the one being typed against matters.
		if (!hovered || ScriptPopupOpen) {
			ScriptHoverWord.clear();
			ScriptHoverText.clear();
			return;
		}

		const float rowHeight = ImGui::GetTextLineHeight();
		const float glyph = ImGui::CalcTextSize("0").x;
		if (rowHeight <= 0.0f || glyph <= 0.0f) {
			return;
		}

		// The same scroll lookup and the same fallback as the gutter; from
		// the top-left when it fails, which at worst hovers the wrong word
		// for a frame.
		ImVec2 scroll(0.0f, 0.0f);
		if (const ImGuiWindow *code = ImGui::FindWindowByName("##text"); code != nullptr) {
			scroll = ImVec2(code->Scroll.x, code->Scroll.y);
		}

		// Pixels to cell: the inverse of the arithmetic that places the
		// completion popup, with the same monospace guarantee making it
		// exact.
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		const ImVec2 mouse = ImGui::GetMousePos();
		const float x = mouse.x - fieldMin.x - padding.x + scroll.x;
		const float y = mouse.y - fieldMin.y - padding.y + scroll.y;
		if (x < 0.0f || y < 0.0f) {
			ScriptHoverWord.clear();
			ScriptHoverText.clear();
			return;
		}

		// `OffsetAtCell` answers nothing for empty space - past a line's end,
		// past the file's last line - which is what keeps the tooltip off
		// text that is not there.
		const size_t offset =
			OffsetAtCell(tab.Text, static_cast<size_t>(y / rowHeight), static_cast<size_t>(x / glyph));
		if (offset == std::string_view::npos) {
			ScriptHoverWord.clear();
			ScriptHoverText.clear();
			return;
		}

		const std::string_view word = WordAt(tab.Text, offset);
		if (word.empty()) {
			ScriptHoverWord.clear();
			ScriptHoverText.clear();
			return;
		}

		// Rebuilt only when the mouse reaches a different occurrence, because
		// building it resolves the tab's language and walks its siblings
		// inside `Universe::Enter` - fine once per word, wasteful per frame.
		const auto anchor = static_cast<int>(word.data() - tab.Text.data());
		if (word != ScriptHoverWord || anchor != ScriptHoverAnchor || tab.Instance != ScriptHoverInstance) {
			ScriptHoverWord.assign(word);
			ScriptHoverAnchor = anchor;
			ScriptHoverInstance = tab.Instance;

			CompletionSources sources;
			std::vector<std::string> children;
			if (tab.Shader) {
				sources.Domain = CompletionDomain::Shader;
			} else {
				// The language the script will actually run as, exactly as
				// `UpdateScriptCompletion` resolves it.
				engine::script::Language language = engine::script::LanguageOf(tab.Path.Text());
				if (tab.World.IsValid()) {
					Universe->Enter(tab.World, [&](Store &store) {
						if (store.Alive(tab.Instance)) {
							language = engine::script::ActiveLanguageOf(store, tab.Instance);
						}
					});
				}

				children = ScriptSiblings(tab);
				sources.Language = language;
				sources.Surface = &SurfaceFor(language);
				sources.Children = children;
			}

			ScriptHoverText = HoverText(tab.Text, offset, sources);
		}

		// A word the editor knows nothing about is no tooltip at all, and the
		// cached empty answer is what keeps that silence cheap.
		if (ScriptHoverText.empty()) {
			return;
		}

		ImGui::SetTooltip("%s", ScriptHoverText.c_str());
	}

	const engine::script::Breakpoint *Editor::BreakpointAt(engine::core::Name path, int line) const {
		for (const engine::script::Breakpoint &point : Breakpoints.Breakpoints()) {
			if (point.Line == line && point.Source == path.Text()) {
				return &point;
			}
		}
		return nullptr;
	}

	void Editor::ToggleBreakpoint(engine::core::Name path, int line) {
		const std::string source(path.Text());

		// **The editor's list is the one that survives a Stop**, and a runtime's
		// is a copy it was handed at `BeginRun` - so both are written, in that
		// order. Writing only the live ones would make a breakpoint disappear
		// the next time somebody pressed Stop.
		if (BreakpointAt(path, line) != nullptr) {
			Breakpoints.Remove(source, line);

			for (WorldRun &run : Runs) {
				if (run.Runtime != nullptr) {
					run.Runtime->Debug().Remove(source, line);
				}
			}
			return;
		}

		// **Said once, where they clicked.** A gutter dot that appeared and never
		// fired would read as the debugger being broken rather than as the
		// language not being supported, and those are not the same thing to go
		// and fix.
		if (const std::string_view refused = engine::script::BreakpointsRefused(source); !refused.empty()) {
			Say("cannot break in " + source + ": " + std::string(refused), engine::core::LogLevel::Warning);
			return;
		}

		const engine::script::BreakAction action =
			BreakStops ? engine::script::BreakAction::Stop : engine::script::BreakAction::Capture;

		Breakpoints.Add(source, line, action);

		for (WorldRun &run : Runs) {
			if (run.Runtime != nullptr) {
				run.Runtime->Debug().Add(source, line, action);
			}
		}
	}
}
