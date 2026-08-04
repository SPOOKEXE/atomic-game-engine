#include <array>
#include <studio/Keybinds.hpp>

namespace studio {

	namespace {
		// The defaults, and they are Roblox Studio's where Studio has one.
		//
		// **Ordered by what they do rather than alphabetically**, because that
		// is the order somebody scans a shortcut list in: the file, then
		// running, then editing, then the panels. The page sorts by name on
		// request; this is what it sorts *from*.
		constexpr std::array<Keybind, static_cast<size_t>(Action::Count)> DEFAULTS{{
			{Action::NewGame, "New Game", "Start an empty universe", {ImGuiKey_N, true, false, false}},
			{Action::OpenGame, "Open Game", "Open a .agame file", {ImGuiKey_O, true, false, false}},
			{Action::Save, "Save", "Write the game to its file", {ImGuiKey_S, true, false, false}},
			{Action::SaveAs, "Save As", "Write the game somewhere new", {ImGuiKey_S, true, true, false}},

			{Action::Play, "Play", "Run the server and a client in this process", {ImGuiKey_F5}},
			{Action::RunServer, "Run", "Run the server's scripts only", {ImGuiKey_F6}},
			{Action::Stop,
			 "Stop",
			 "Stop and restore the scene as it was",
			 {ImGuiKey_F5, false, true, false}},

			{Action::Duplicate,
			 "Duplicate",
			 "Copy the selection beside itself",
			 {ImGuiKey_D, true, false, false}},
			{Action::Delete, "Delete", "Delete the selection", {ImGuiKey_Delete}},
			{Action::SelectNone, "Select None", "Clear the selection", {ImGuiKey_Escape}},

			// Unbound on purpose. See `Action::ShowStatistics`.
			{Action::ShowStatistics, "Statistics", "Show the frame rate panel", {}},
			{Action::ShowFrameGraph, "Frame Graph", "Show where the frame went", {}},
		}};

		// The live table. A copy of the defaults until something rebinds.
		std::array<Keybind, static_cast<size_t>(Action::Count)> &Table() {
			static std::array<Keybind, static_cast<size_t>(Action::Count)> table = DEFAULTS;
			return table;
		}

		size_t IndexOf(Action action) {
			const auto index = static_cast<size_t>(action);
			return index < static_cast<size_t>(Action::Count) ? index : 0;
		}
	}

	std::string Chord::Text() const {
		if (!IsBound()) {
			return {};
		}

		std::string text;
		if (Ctrl) {
			text += "Ctrl+";
		}
		if (Shift) {
			text += "Shift+";
		}
		if (Alt) {
			text += "Alt+";
		}

		// imgui's own name for the key, so a name never has to be written down
		// twice — and so a key this table has never heard of still reads as
		// something rather than as a number.
		text += ImGui::GetKeyName(Key);
		return text;
	}

	std::span<Keybind> Keybinds::All() {
		return Table();
	}

	Chord Keybinds::Of(Action action) {
		return Table()[IndexOf(action)].Keys;
	}

	void Keybinds::Set(Action action, Chord chord) {
		// **Cleared from whoever else held it first.** Two actions on one chord
		// is a key that does two things, and which one happens depends on the
		// order `DrawShortcuts` happens to test them in.
		if (chord.IsBound()) {
			for (Keybind &binding : Table()) {
				if (binding.Bound != action && binding.Keys == chord) {
					binding.Keys = Chord{};
				}
			}
		}

		Table()[IndexOf(action)].Keys = chord;

		// TODO: file persistence. See the declaration — this is where a write
		// would go, and the reason it is not here yet is the format rather than
		// the call.
	}

	void Keybinds::Reset() {
		Table() = DEFAULTS;
	}

	bool Keybinds::Fired(Action action) {
		const Chord chord = Of(action);
		if (!chord.IsBound()) {
			return false;
		}

		if (!ImGui::IsKeyPressed(chord.Key, false)) {
			return false;
		}

		// **Exactly, not at least.** `Ctrl+S` firing on `Ctrl+Shift+S` means
		// Save As also saves, which is the kind of thing that silently
		// overwrites the wrong file.
		const ImGuiIO &io = ImGui::GetIO();
		return io.KeyCtrl == chord.Ctrl && io.KeyShift == chord.Shift && io.KeyAlt == chord.Alt;
	}

	Chord Keybinds::Pressed() {
		const ImGuiIO &io = ImGui::GetIO();

		for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
			const auto candidate = static_cast<ImGuiKey>(key);

			// The modifiers themselves are never the key: holding Ctrl on the
			// way to `Ctrl+D` would otherwise bind Ctrl the moment it went
			// down.
			if (candidate == ImGuiKey_LeftCtrl || candidate == ImGuiKey_RightCtrl ||
				candidate == ImGuiKey_LeftShift || candidate == ImGuiKey_RightShift ||
				candidate == ImGuiKey_LeftAlt || candidate == ImGuiKey_RightAlt ||
				candidate == ImGuiKey_LeftSuper || candidate == ImGuiKey_RightSuper) {
				continue;
			}

			if (ImGui::IsKeyPressed(candidate, false)) {
				return Chord{candidate, io.KeyCtrl, io.KeyShift, io.KeyAlt};
			}
		}

		return Chord{};
	}
}
