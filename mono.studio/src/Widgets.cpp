#include <cctype>
#include <imgui.h>
#include <studio/Widgets.hpp>

namespace studio {

	const char *Label(const engine::core::Name &name, const char *fallback) {
		return name.IsValid() ? name.Text().data() : fallback;
	}

	namespace {
		// imgui's resize callback, which is how a text field grows a
		// `std::string` instead of truncating into a fixed buffer.
		//
		// **`resize` and then `data()` and not the other way round.** The
		// callback hands back a pointer imgui goes on writing into, so it has
		// to be the pointer the string owns *after* the reallocation — reading
		// it first hands imgui memory that has just been freed, and the
		// corruption shows up in whatever allocated next.
		int Grow(ImGuiInputTextCallbackData *data) {
			if (data->EventFlag != ImGuiInputTextFlags_CallbackResize) {
				return 0;
			}

			auto *text = static_cast<std::string *>(data->UserData);
			text->resize(static_cast<size_t>(data->BufTextLen));
			data->Buf = text->data();
			return 0;
		}

		char Lower(char c) {
			return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}

	bool TextField(const char *label, std::string &text, const char *hint) {
		// The capacity is what imgui writes into, so a string with none is a
		// field that cannot be typed in.
		if (text.capacity() < 64) {
			text.reserve(64);
		}

		const auto flags = ImGuiInputTextFlags_CallbackResize;

		if (hint != nullptr) {
			return ImGui::InputTextWithHint(
				label, hint, text.data(), text.capacity() + 1, flags, Grow, &text
			);
		}
		return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, Grow, &text);
	}

	bool CodeField(const char *label, std::string &text, float width, float height) {
		if (text.capacity() < 1024) {
			text.reserve(1024);
		}

		// `AllowTabInput`, because this is where code is written and Tab moving
		// focus to the next widget would make the editor unusable for the one
		// thing it is for.
		const auto flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput;

		return ImGui::InputTextMultiline(
			label, text.data(), text.capacity() + 1, ImVec2(width, height), flags, Grow, &text
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
		const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			return false;
		}

		ImGui::TextUnformatted(label);

		// Focused on the frame the popup appears, so the first keystroke lands
		// in the field rather than being swallowed.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}

		ImGui::SetNextItemWidth(-1.0f);
		const bool entered = ImGui::InputText(
			"##value",
			buffer.data(),
			buffer.capacity() + 1,
			ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue,
			[](ImGuiInputTextCallbackData *data) {
				if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
					auto *text = static_cast<std::string *>(data->UserData);
					text->resize(static_cast<size_t>(data->BufTextLen));
					data->Buf = text->data();
				}
				return 0;
			},
			&buffer
		);

		ImGui::Separator();

		if (ImGui::Button(accept, ImVec2(120.0f, 0.0f)) || entered) {
			confirmed = true;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
