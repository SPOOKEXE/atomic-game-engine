#include <engine/ui/Metrics.hpp>
#include <engine/ui/PerCallSite.hpp>
#include <engine/ui/Theme.hpp>

#include <cctype>
#include <imgui.h>
#include <imgui_internal.h>
#include <studio/Widgets.hpp>

namespace studio {

	const char *Label(const engine::core::Name &name, const char *fallback) {
		return name.IsValid() ? name.Text().data() : fallback;
	}

	namespace {
		// What a field's callback is given.
		//
		// **A carrier rather than the string itself**, because the code field
		// needs a second thing out of the same callback - where the caret is -
		// and imgui offers exactly one `UserData` pointer. `Edit` is null for
		// every field that only wants to grow.
		struct FieldCallback {
			std::string *Text = nullptr;
			CodeEdit *Edit = nullptr;
			TextSelection *Before = nullptr;
			bool CapturedBefore = false;
		};

		// imgui's callback, which is how a text field grows a `std::string`
		// instead of truncating into a fixed buffer - and, for the code field,
		// how the caret gets out and an insertion gets in.
		//
		// **`resize` and then `data()` and not the other way round.** The
		// callback hands back a pointer imgui goes on writing into, so it has
		// to be the pointer the string owns *after* the reallocation - reading
		// it first hands imgui memory that has just been freed, and the
		// corruption shows up in whatever allocated next.
		int Grow(ImGuiInputTextCallbackData *data) {
			auto *carrier = static_cast<FieldCallback *>(data->UserData);

			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
				carrier->Text->resize(static_cast<size_t>(data->BufTextLen));
				data->Buf = carrier->Text->data();
				return 0;
			}

			if (data->EventFlag == ImGuiInputTextFlags_CallbackBeforeInput && carrier->Before != nullptr &&
				!carrier->CapturedBefore) {
				TextSelection &before = *carrier->Before;
				before = {
					static_cast<size_t>(std::max(data->CursorPos, 0)),
					static_cast<size_t>(std::max(data->SelectionStart, 0)),
					static_cast<size_t>(std::max(data->SelectionEnd, 0))
				};
				if (before.Start == before.End) before.Start = before.End = before.Cursor;
				carrier->CapturedBefore = true;
				return 0;
			}

			// Completion edits and caret reporting use ImGui's callback data.
			if (data->EventFlag != ImGuiInputTextFlags_CallbackAlways || carrier->Edit == nullptr) {
				return 0;
			}

			CodeEdit &edit = *carrier->Edit;
			edit.Active = true;

			// **Applied here rather than by rewriting the string.** Editing the
			// buffer behind imgui's back leaves its undo stack describing text
			// that is no longer there, so accepting a completion would make the
			// next Ctrl+Z do something nobody asked for.
			if (edit.ReplaceFrom >= 0) {
				const int end = edit.ReplaceTo >= 0 ? edit.ReplaceTo : data->CursorPos;
				const bool matches = edit.ReplaceTo < 0 ||
									 (edit.ReplaceExpected == *carrier->Text &&
									  std::string_view(data->Buf, static_cast<size_t>(data->BufTextLen)) ==
										  edit.ReplaceExpected);
				if (matches && edit.ReplaceFrom <= end && end <= data->BufTextLen) {
					data->DeleteChars(edit.ReplaceFrom, end - edit.ReplaceFrom);
					if (!edit.Insert.empty()) data->InsertChars(edit.ReplaceFrom, edit.Insert.c_str());
				}
				edit.Insert.clear();
				edit.ReplaceFrom = -1;
				edit.ReplaceTo = -1;
				edit.ReplaceExpected.clear();
			}

			edit.Caret = data->CursorPos;
			edit.SelectionStart = data->SelectionStart;
			edit.SelectionEnd = data->SelectionEnd;
			return 0;
		}

		char Lower(char c) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}

	bool CodeField(const char *label, std::string &text, CodeEdit *edit, float width, float height) {
		if (text.capacity() < 1024) {
			text.reserve(1024);
		}

		// `AllowTabInput`, because this is where code is written and Tab moving
		// focus to the next widget would make the editor unusable for the one
		// thing it is for.
		//
		// **`CallbackCompletion` stays off**: `imgui_widgets.cpp` asserts that
		// it and `AllowTabInput` are never both set, because both want the key.
		// The completion popup does not need it - the field polls Tab and
		// Enter through `Shortcut` against its own id, so the popup claims
		// both with `SetKeyOwner` while it is open, and Tab indents again the
		// moment it closes.
		auto flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput;
		bool replayed = false;
		bool requested = false;
		TextSelection before;
		const auto selectionOf = [](const CodeEdit &state) {
			TextSelection result{
				static_cast<size_t>(std::max(state.Caret, 0)),
				static_cast<size_t>(std::max(state.SelectionStart, 0)),
				static_cast<size_t>(std::max(state.SelectionEnd, 0))
			};
			if (result.Start == result.End) result.Start = result.End = result.Cursor;
			return result;
		};
		if (edit != nullptr) {
			flags |= ImGuiInputTextFlags_CallbackAlways;
			if (edit->ManagedUndo) {
				flags |= ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_CallbackBeforeInput;
				const ImGuiID id = ImGui::GetID(label);
				if (!edit->History.Observe(text)) {
					edit->Insert.clear();
					edit->ReplaceFrom = edit->ReplaceTo = -1;
					edit->ReplaceExpected.clear();
					if (ImGuiInputTextState *state = ImGui::GetInputTextState(id))
						state->ReloadUserBufAndKeepSelection();
				}
				before = selectionOf(*edit);
				if (ImGui::GetActiveID() != id) edit->History.BreakCoalescing();
				if (ImGui::GetActiveID() == id) {
					const bool undo = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z);
					const bool redo = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
									  ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
					if (undo || redo) {
						// The history action owns this frame; never apply an older queued edit to its result.
						edit->Insert.clear();
						edit->ReplaceFrom = edit->ReplaceTo = -1;
						edit->ReplaceExpected.clear();
						TextSelection selection;
						replayed =
							undo ? edit->History.Undo(text, selection) : edit->History.Redo(text, selection);
						if (replayed) {
							before = selection;
							if (ImGuiInputTextState *state = ImGui::GetInputTextState(id)) {
								const int start = static_cast<int>(selection.Start);
								const int end = static_cast<int>(selection.End);
								if (selection.Cursor == selection.Start && start != end)
									state->SetSelection(end, start);
								else
									state->SetSelection(start, end);
								state->ReloadUserBufAndKeepSelection();
							}
						}
					}
				}
			}
			edit->Active = false;
		}

		if (edit != nullptr) requested = edit->ReplaceFrom >= 0;
		const ImGuiIO &io = ImGui::GetIO();
		const bool typing = !replayed && !requested && io.InputQueueCharacters.Size > 0 && !io.KeyCtrl &&
							!io.KeyAlt && !io.KeySuper;
		FieldCallback carrier{&text, edit, &before};
		const bool changed = ImGui::InputTextMultiline(
			label, text.data(), text.capacity() + 1, ImVec2(width, height), flags, Grow, &carrier
		);
		if (changed && edit != nullptr && edit->ManagedUndo) {
			const TextSelection after = selectionOf(*edit);
			edit->History.Record(text, before, after, typing, ImGui::GetTime());
		}
		return changed || replayed;
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

	bool PathPrompt(
		const char *title,
		const char *label,
		std::string &buffer,
		const char *accept,
		std::string *error,
		const std::function<std::string_view()> &submit
	) {
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
		// be - which is off the edge, and a modal is the one window somebody
		// cannot scroll to or drag back because it has taken the input. So the
		// frame the viewport changes size re-centres every prompt, and every
		// other frame leaves it where it is. Re-centring unconditionally would
		// have been simpler and would have taken away dragging the prompt out
		// of the way of what it is asking about.
		// **Latched per frame, not per call.** `DrawDialogs` submits every
		// prompt every frame and only one of them can be open, so comparing and
		// then storing the size would let the first call consume the change and
		// the other seven - including the open one - see nothing. The frame
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
		// again - a few pixels a frame until the modal had deflated to the
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

		if (error != nullptr && ImGui::IsItemEdited()) {
			error->clear();
		}
		if (error != nullptr && !error->empty()) {
			ImGui::TextUnformatted(error->c_str());
		}

		ImGui::Separator();

		// Scaled like everything else. These were the one pair of raw pixel
		// sizes left in the editor, which is why they were also the floor the
		// deflating modal settled on.
		const ImVec2 button(engine::ui::Scaled(120.0f), 0.0f);

		if (ImGui::Button(accept, button) || entered) {
			const std::string_view refusal = submit ? submit() : std::string_view{};
			if (error != nullptr) {
				*error = refusal;
			}
			if (refusal.empty()) {
				confirmed = true;
				ImGui::CloseCurrentPopup();
			}
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
