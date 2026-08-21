#include <engine/ui/Fields.hpp>

#include <imgui.h>

namespace engine::ui {

	namespace {
		// imgui's callback, which is how a text field grows a `std::string`
		// instead of truncating into a fixed buffer.
		//
		// **`resize` and then `data()` and not the other way round.** The
		// callback hands back a pointer imgui goes on writing into, so it has
		// to be the pointer the string owns *after* the reallocation - reading
		// it first hands imgui memory that has just been freed, and the
		// corruption shows up in whatever allocated next.
		int Grow(ImGuiInputTextCallbackData *data) {
			auto *text = static_cast<std::string *>(data->UserData);

			if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
				text->resize(static_cast<size_t>(data->BufTextLen));
				data->Buf = text->data();
			}
			return 0;
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

		if (hint != nullptr) {
			return ImGui::InputTextWithHint(
				label, hint, text.data(), text.capacity() + 1, flags, Grow, &text
			);
		}
		return ImGui::InputText(label, text.data(), text.capacity() + 1, flags, Grow, &text);
	}
}
