#include "PerCallSite.hpp"

#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <cctype>
#include <imgui.h>
#include <studio/Browse.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	const char *Label(const engine::core::Name &name, const char *fallback) {
		return name.IsValid() ? name.Text().data() : fallback;
	}

	namespace {
		// What a field's callback is given.
		//
		// **A carrier rather than the string itself**, because the code field
		// needs a second thing out of the same callback — where the caret is —
		// and imgui offers exactly one `UserData` pointer. `Edit` is null for
		// every field that only wants to grow.
		struct FieldCallback {
			std::string *Text = nullptr;
			CodeEdit *Edit = nullptr;
		};

		// imgui's callback, which is how a text field grows a `std::string`
		// instead of truncating into a fixed buffer — and, for the code field,
		// how the caret gets out and an insertion gets in.
		//
		// **`resize` and then `data()` and not the other way round.** The
		// callback hands back a pointer imgui goes on writing into, so it has
		// to be the pointer the string owns *after* the reallocation — reading
		// it first hands imgui memory that has just been freed, and the
		// corruption shows up in whatever allocated next.
		int Grow(ImGuiInputTextCallbackData *data) {
			auto *carrier = static_cast<FieldCallback *>(data->UserData);

			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
				carrier->Text->resize(static_cast<size_t>(data->BufTextLen));
				data->Buf = carrier->Text->data();
				return 0;
			}

			// **`CallbackAlways` is the whole of the completion seam, and it is
			// public API.** `ScriptEditor.cpp` refuses `ImGuiInputTextState` to
			// move the caret, and that refusal stands — that struct is a private
			// *layout* whose fields shift between releases. This is the
			// supported way to the same fact: `CursorPos`, `InsertChars` and
			// `DeleteChars` are documented members of
			// `ImGuiInputTextCallbackData`, and a popup cannot be placed or
			// applied without them.
			if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways || carrier->Edit == nullptr) {
				return 0;
			}

			CodeEdit &edit = *carrier->Edit;
			edit.Active = true;

			// **Applied here rather than by rewriting the string.** Editing the
			// buffer behind imgui's back leaves its undo stack describing text
			// that is no longer there, so accepting a completion would make the
			// next Ctrl+Z do something nobody asked for.
			if (edit.ReplaceFrom >= 0 && edit.ReplaceFrom <= data->CursorPos) {
				data->DeleteChars(edit.ReplaceFrom, data->CursorPos - edit.ReplaceFrom);
				if (!edit.Insert.empty()) {
					data->InsertChars(edit.ReplaceFrom, edit.Insert.c_str());
				}
				edit.Insert.clear();
				edit.ReplaceFrom = -1;
			}

			edit.Caret = data->CursorPos;
			return 0;
		}

		char Lower(char c) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}

	bool TextField(const char *label, std::string &text, const char *hint, bool secret) {
		// The capacity is what imgui writes into, so a string with none is a
		// field that cannot be typed in.
		if (text.capacity() < 64) {
			text.reserve(64);
		}

		const auto flags = secret ? (ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_Password)
								  : ImGuiInputTextFlags_CallbackResize;

		FieldCallback carrier{&text, nullptr};

		if (hint != nullptr) {
			return ImGui::InputTextWithHint(
				label, hint, text.data(), text.capacity() + 1, flags, Grow, &carrier
			);
		}
		return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, Grow, &carrier);
	}

	bool CodeField(const char *label, std::string &text, CodeEdit *edit, float width, float height) {
		if (text.capacity() < 1024) {
			text.reserve(1024);
		}

		// `AllowTabInput`, because this is where code is written and Tab moving
		// focus to the next widget would make the editor unusable for the one
		// thing it is for.
		//
		// **`AllowTabInput` is also why Tab cannot accept a completion.**
		// `imgui_widgets.cpp` asserts that it and `CallbackCompletion` are never
		// both set, because both want the key — so the popup takes Enter, and
		// claims it with `SetKeyOwner` while it is open.
		auto flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput;
		if (edit != nullptr) {
			flags |= ImGuiInputTextFlags_CallbackAlways;
			edit->Active = false;
		}

		FieldCallback carrier{&text, edit};

		return ImGui::InputTextMultiline(
			label, text.data(), text.capacity() + 1, ImVec2(width, height), flags, Grow, &carrier
		);
	}

	bool RunButton(const char *label, bool active, unsigned int colour) {
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, colour);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colour);
		}

		const bool clicked = ImGui::Button(label);

		if (active) {
			ImGui::PopStyleColor(2);
		}
		return clicked;
	}

	bool PathPrompt(const char *title, const char *label, std::string &buffer, const char *accept) {
		bool confirmed = false;

		// Centred, because a modal that opens wherever the mouse last was is a
		// modal people miss.
		//
		// **No `SetNextWindowSize` to go with it.** The window measures itself
		// from its contents below; asking for a width as well meant the two
		// disagreed on the appearing frame and the auto-size won every frame
		// after, which is a starting width that lasted exactly one frame.
		//
		// **`Appearing` alone was not enough, and the failure needs a resize to
		// see.** A position set once is in absolute coordinates, so shrinking
		// the window afterwards leaves the modal where the old centre used to
		// be — which is off the edge, and a modal is the one window somebody
		// cannot scroll to or drag back because it has taken the input. So the
		// frame the viewport changes size re-centres every prompt, and every
		// other frame leaves it where it is. Re-centring unconditionally would
		// have been simpler and would have taken away dragging the prompt out
		// of the way of what it is asking about.
		// **Latched per frame, not per call.** `DrawDialogs` submits every
		// prompt every frame and only one of them can be open, so comparing and
		// then storing the size would let the first call consume the change and
		// the other seven — including the open one — see nothing. The frame
		// number is what makes "did the viewport resize" a fact about the frame
		// rather than about the call order within it.
		static int measuredFrame = -1;
		static ImVec2 lastViewport = ImVec2(0.0f, 0.0f);
		static bool resized = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		if (const int frame = ImGui::GetFrameCount(); frame != measuredFrame) {
			measuredFrame = frame;
			resized = main->Size.x != lastViewport.x || main->Size.y != lastViewport.y;
			lastViewport = main->Size;
		}

		ImGui::SetNextWindowPos(
			main->GetCenter(), resized ? ImGuiCond_Always : ImGuiCond_Appearing, ImVec2(0.5f, 0.5f)
		);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return false;
		}

		ImGui::TextUnformatted(label);

		// Focused on the frame the popup appears, so the first keystroke lands
		// in the field rather than being swallowed.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		// **An explicit width, because this window auto-resizes.** `-1` is
		// "fill the content region", and the content region of an
		// `AlwaysAutoResize` window comes from the width it measured last
		// frame. So the field asked for one pixel less than the window, the
		// window resized to fit the field, and the field asked for one less
		// again — a few pixels a frame until the modal had deflated to the
		// widest thing that was not elastic, which is the two buttons at the
		// bottom. It looked like an animation and was a feedback loop.
		//
		// One fixed number breaks the cycle: the field decides the width and
		// the window follows it, rather than each following the other.
		ImGui::SetNextItemWidth(engine::ui::Scaled(engine::ui::Size::Prompt));

		// **`Grow` rather than a lambda repeating it.** This was a second copy
		// of the resize callback, which meant a change to how a field grows had
		// two places to land and one of them would have been forgotten.
		FieldCallback promptCarrier{&buffer, nullptr};

		const bool entered = ImGui::InputText(
			"##value",
			buffer.data(),
			buffer.capacity() + 1,
			ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue,
			Grow,
			&promptCarrier
		);

		ImGui::Separator();

		// Scaled like everything else. These were the one pair of raw pixel
		// sizes left in the editor, which is why they were also the floor the
		// deflating modal settled on.
		const ImVec2 button(engine::ui::Scaled(120.0f), 0.0f);

		if (ImGui::Button(accept, button) || entered) {
			confirmed = true;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	bool FilePrompt(
		const char *title,
		std::string &path,
		const char *accept,
		const std::vector<std::string> &extensions,
		bool mustExist
	) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(
			ImVec2(engine::ui::Scaled(640.0f), engine::ui::Scaled(460.0f)), ImGuiCond_Appearing
		);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// **Where each dialog is looking, kept per title.** Six dialogs share
		// this function and they are not looking at the same place: Open starts
		// where the game is, Export starts wherever the last export went. One
		// shared directory would make each of them jump to whichever was used
		// last.
		//
		// `PerCallSite` is that pattern, written once — this was the third
		// transcription of it and the comment here used to say so.
		struct Browsing {
			std::filesystem::path Where;
			std::string Name;
		};

		Browsing *const state = &PerCallSite<Browsing>(title);

		// Opened fresh: start from whatever path the caller had, which is the
		// game's own folder far more often than not.
		if (ImGui::IsWindowAppearing()) {
			const std::filesystem::path given(path);
			state->Where = given;
			state->Name = given.has_filename() ? given.filename().string() : std::string();
		}

		const Listing listing = BrowseDirectory(state->Where, extensions);
		state->Where = listing.Directory;

		// The path bar. Not editable — the field at the bottom is where a path
		// is typed, and two places to type one would be two places for them to
		// disagree.
		ImGui::TextDisabled("%s", listing.Directory.string().c_str());
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (!listing.Error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::TextWrapped("%s", listing.Error.c_str());
				ImGui::PopStyleColor();
			}

			if (!listing.Parent.empty()) {
				if (ImGui::Selectable("..", false, ImGuiSelectableFlags_AllowDoubleClick)) {
					state->Where = listing.Parent;
				}
			}

			for (const BrowseEntry &entry : listing.Entries) {
				// The id is the path rather than the name, so two folders with
				// the same name in different places are two rows.
				ImGui::PushID(entry.Path.string().c_str());

				const bool selected = !entry.Directory && entry.Name == state->Name;

				if (entry.Directory) {
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
				}

				if (ImGui::Selectable(
						(entry.Directory ? entry.Name + "/" : entry.Name).c_str(),
						selected,
						ImGuiSelectableFlags_AllowDoubleClick
					)) {
					if (entry.Directory) {
						// Single click descends. A folder is not a thing this
						// dialog can return, so there is nothing else a click
						// on one could mean.
						state->Where = entry.Path;
						state->Name.clear();
					} else {
						state->Name = entry.Name;

						// Double-click is confirm, which is what every file
						// dialog does and what a person tries first.
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
							confirmed = true;
						}
					}
				}

				if (entry.Directory) {
					ImGui::PopStyleColor();
				}

				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::SetNextItemWidth(-1.0f);
		const bool entered = TextField("##name", state->Name, "file name");

		const std::filesystem::path chosen =
			state->Name.empty() ? std::filesystem::path{} : listing.Directory / state->Name;

		std::error_code code;
		const bool exists = !chosen.empty() && std::filesystem::exists(chosen, code);

		// **Refused rather than allowed to fail later.** Open on a path that is
		// not there used to be discovered by pressing the button and reading a
		// log line; a disabled button with the reason beside it is the same
		// information before the click rather than after.
		const bool usable = !chosen.empty() && (!mustExist || exists);

		const ImVec2 button(engine::ui::Scaled(120.0f), 0.0f);

		ImGui::BeginDisabled(!usable);
		if (ImGui::Button(accept, button) || (entered && usable)) {
			confirmed = true;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		if (!chosen.empty() && mustExist && !exists) {
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted("no such file");
			ImGui::PopStyleColor();
		}

		if (confirmed) {
			path = chosen.string();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	bool FolderPrompt(const char *title, std::string &path, const char *accept) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(
			ImVec2(engine::ui::Scaled(640.0f), engine::ui::Scaled(460.0f)), ImGuiCond_Appearing
		);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// Where this dialog is looking, kept per title — `FilePrompt`'s reason
		// and its pattern.
		std::filesystem::path *const where = &PerCallSite<std::filesystem::path>(title);

		if (ImGui::IsWindowAppearing()) {
			*where = std::filesystem::path(path);
		}

		// **No extension filter, and the files are listed anyway.** A folder
		// browser that showed only directories makes an empty folder and the
		// wrong folder look identical, which is exactly the mistake somebody is
		// about to make when they import a hundred files from the wrong place.
		const Listing listing = BrowseDirectory(*where, {});
		*where = listing.Directory;

		ImGui::TextDisabled("%s", listing.Directory.string().c_str());
		ImGui::Separator();

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (!listing.Error.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::TextWrapped("%s", listing.Error.c_str());
				ImGui::PopStyleColor();
			}

			if (!listing.Parent.empty()) {
				if (ImGui::Selectable("..")) {
					*where = listing.Parent;
				}
			}

			size_t files = 0;
			for (const BrowseEntry &entry : listing.Entries) {
				ImGui::PushID(entry.Path.string().c_str());

				if (entry.Directory) {
					ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
					if (ImGui::Selectable((entry.Name + "/").c_str())) {
						*where = entry.Path;
					}
					ImGui::PopStyleColor();
				} else {
					// Shown and not selectable: this dialog returns the folder,
					// and a row that highlighted but did nothing would be worse
					// than one that plainly cannot be picked.
					ImGui::TextDisabled("%s", entry.Name.c_str());
					files++;
				}

				ImGui::PopID();
			}

			if (files == 0 && listing.Entries.empty()) {
				ImGui::TextDisabled("(empty)");
			}
		}
		ImGui::EndChild();

		const ImVec2 button(engine::ui::Scaled(140.0f), 0.0f);

		if (ImGui::Button(accept, button)) {
			confirmed = true;
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		if (confirmed) {
			path = listing.Directory.string();
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	bool FuzzyMatch(std::string_view query, std::string_view candidate, int &score) {
		score = 0;

		if (query.empty()) {
			return true;
		}

		size_t at = 0;
		int run = 0;

		for (const char c : candidate) {
			if (at < query.size() && Lower(c) == Lower(query[at])) {
				at++;
				run++;
				// Consecutive matches score more than scattered ones, so "bp"
				// prefers "BasePart" over "BrickPlacement" only because of
				// where the letters fall rather than because of a rule about
				// capitals.
				score += run;
			} else {
				run = 0;
			}
		}

		if (at != query.size()) {
			return false;
		}

		// An exact or prefix hit beats everything a subsequence can score, so
		// typing a full class name puts it first however many other classes
		// happen to contain those letters.
		if (candidate.size() == query.size()) {
			score += 1000;
		} else if (candidate.size() > query.size()) {
			bool prefix = true;
			for (size_t index = 0; index < query.size(); index++) {
				if (Lower(candidate[index]) != Lower(query[index])) {
					prefix = false;
					break;
				}
			}
			if (prefix) {
				score += 100;
			}
		}

		return true;
	}
}
