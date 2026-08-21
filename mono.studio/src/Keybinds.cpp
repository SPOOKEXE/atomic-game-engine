#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <studio/Keybinds.hpp>

namespace studio {

	namespace {
		// The defaults, and they are Roblox Studio's where Studio has one.
		//
		// **Ordered by what they do rather than alphabetically**, because that
		// is the order somebody scans a shortcut list in: the file, then
		// running, then editing, then the panels. The page sorts by name on
		// request; this is what it sorts *from*.
		// **Every action ships unbound, and that is a decision rather than an
		// oversight.** The editor's commands are all reachable from the menus,
		// and the menus are where a person finds a command they do not already
		// know. Keys are then added deliberately - through the page, or by a
		// later default set chosen once the manager they are managed by exists.
		//
		// Shipping a key nobody asked for is how F5 came to mean Play in one
		// place and Stop in another, spelled out in three files. An unbound
		// table cannot drift, and a binding added from here now costs one line
		// and carries its scope with it.
		//
		// Ordered by what they do rather than alphabetically, because that is
		// the order somebody scans a shortcut list in: the file, then running,
		// then editing, then the panels.
		constexpr std::array<Keybind, static_cast<size_t>(Action::Count)> DEFAULTS{{
			{Action::NewGame, "file.new", "New Game", "Start an empty universe", Scope::Global, {}},
			{Action::OpenGame, "file.open", "Open Game", "Open a .agame file", Scope::Global, {}},
			{Action::Save, "file.save", "Save", "Write the game to its file", Scope::Global, {}},
			{Action::SaveAs, "file.saveas", "Save As", "Write the game somewhere new", Scope::Global, {}},

			{Action::Play, "run.play", "Play", "Run the server and a client in this process",
			 Scope::Global, {}},
			{Action::RunServer, "run.server", "Run", "Run the server's scripts only", Scope::Global, {}},
			{Action::Stop, "run.stop", "Stop", "Stop and restore the scene as it was", Scope::Global, {}},

			// **`Tree`, not `Global`, and the script editor is why.** A plain
			// multiline field has imgui's own text undo on the same chord, and a
			// world undo that fired while somebody was typing would reverse the
			// last thing they built instead of the last thing they typed. The
			// same argument `Delete` is scoped by, which is a character in a
			// field and an action in the tree.
			//
			// §4.6 of `docs/retired/v07v08.md` gives the viewport its own editing, and
			// this wants a second home there when it does - scopes are one per
			// binding today, so that is a decision rather than a line.
			{Action::Undo, "edit.undo", "Undo", "Reverse the last edit", Scope::Tree, {}},
			{Action::Redo, "edit.redo", "Redo", "Reapply the last undone edit", Scope::Tree, {}},

			{Action::Duplicate, "edit.duplicate", "Duplicate", "Copy the selection beside itself",
			 Scope::Tree, {}},
			{Action::Delete, "edit.delete", "Delete", "Delete the selection", Scope::Tree, {}},
			{Action::SelectNone, "edit.selectnone", "Select None", "Clear the selection", Scope::Tree, {}},

			// **Unbound, like everything else here, and F2 was asked for.**
			// `tests/Keybinds.cpp` holds the rule above still: every action
			// ships unbound, because a default nobody asked for is how one key
			// came to mean two things in three files. Shipping F2 would have
			// been a decision to override a tested invariant to save one visit
			// to the Keybinds page - so the action is here, the operator and
			// the tree's context menu make it reachable, and F2 is one binding
			// away rather than baked in.
			{Action::Rename, "edit.rename", "Rename", "Rename the selected instance", Scope::Tree, {}},

			// **`Viewport`, and that scope is the whole reason these can be
			// digits at all.** A plain `1` bound globally is a `1` the script
			// editor never sees and a rename field swallows; scoped here it can
			// only fire while somebody is working in a picture of the world,
			// which is the only place a handle exists to switch between.
			{Action::ToolSelect, "tool.select", "Select Tool", "Click to select, with no handles",
			 Scope::Viewport, {}},
			{Action::ToolMove, "tool.move", "Move Tool", "Drag an axis to move the selection",
			 Scope::Viewport, {}},
			{Action::ToolRotate, "tool.rotate", "Rotate Tool", "Drag a ring to turn the selection",
			 Scope::Viewport, {}},
			{Action::ToolScale, "tool.scale", "Scale Tool", "Drag an axis to resize the selection",
			 Scope::Viewport, {}},

			{Action::ShowStatistics, "panel.statistics", "Statistics", "Show the frame rate panel",
			 Scope::Global, {}},
			{Action::ShowFrameGraph, "panel.framegraph", "Frame Graph", "Show where the frame went",
			 Scope::Global, {}},
			{Action::ShowHeap, "panel.heap", "Heap", "Show where the memory went", Scope::Global, {}},

			{Action::CommandPalette, "panel.palette", "Command Palette", "Find and run any command",
			 Scope::Global, {}},
		}};

		// The live table. A copy of the defaults until something rebinds.
		std::array<Keybind, static_cast<size_t>(Action::Count)> &Table() {
			static std::array<Keybind, static_cast<size_t>(Action::Count)> table = DEFAULTS;
			return table;
		}

		// Where the keyboard is this frame. Reset by the interface layer before
		// anything asks. See `Keybinds::SetScope`.
		Scope &ActiveScope() {
			static Scope scope = Scope::Global;
			return scope;
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
		// twice - and so a key this table has never heard of still reads as
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
	}

	void Keybinds::Reset() {
		Table() = DEFAULTS;
	}

	void Keybinds::SetScope(Scope scope) {
		ActiveScope() = scope;
	}

	Scope Keybinds::CurrentScope() {
		return ActiveScope();
	}

	Action Keybinds::Holder(Chord chord, Action ignore) {
		if (!chord.IsBound()) {
			return Action::Count;
		}

		for (const Keybind &binding : Table()) {
			if (binding.Bound != ignore && binding.Keys == chord) {
				return binding.Bound;
			}
		}
		return Action::Count;
	}

	bool Keybinds::Fired(Action action) {
		const Keybind &binding = Table()[IndexOf(action)];
		const Chord chord = binding.Keys;
		if (!chord.IsBound()) {
			return false;
		}

		// **Scoped, so a key can mean different things in different panels.**
		// A global binding fires wherever the editor has focus; anything else
		// fires only in the panel it belongs to. Without this a plain `Delete`
		// bound for the tree would delete the selection while somebody was
		// typing in a script. See `Scope`.
		if (binding.Where != Scope::Global && binding.Where != ActiveScope()) {
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

	namespace {
		// **The key's name in the file, not its number.** `ImGuiKey` values move
		// between imgui versions, so a file holding integers would rebind
		// somebody's editor when the vendored library was updated - silently,
		// and to whatever key happened to take the number.
		std::string NameOfKey(ImGuiKey key) {
			const char *name = ImGui::GetKeyName(key);
			return name != nullptr ? std::string(name) : std::string();
		}

		ImGuiKey KeyOfName(std::string_view name) {
			if (name.empty()) {
				return ImGuiKey_None;
			}
			for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++) {
				const char *candidate = ImGui::GetKeyName(static_cast<ImGuiKey>(key));
				if (candidate != nullptr && name == candidate) {
					return static_cast<ImGuiKey>(key);
				}
			}
			return ImGuiKey_None;
		}
	}

	bool Keybinds::Save(const std::filesystem::path &path) {
		std::ofstream out(path, std::ios::trunc);
		if (!out) {
			return false;
		}

		out << "# atomic studio keybinds\n";
		out << "# one line per command: id = Ctrl+Shift+Key, or id = for unbound\n";

		for (const Keybind &binding : Table()) {
			out << binding.Id << " = ";
			if (binding.Keys.IsBound()) {
				if (binding.Keys.Ctrl) {
					out << "Ctrl+";
				}
				if (binding.Keys.Shift) {
					out << "Shift+";
				}
				if (binding.Keys.Alt) {
					out << "Alt+";
				}
				out << NameOfKey(binding.Keys.Key);
			}
			out << "\n";
		}

		return out.good();
	}

	bool Keybinds::Load(const std::filesystem::path &path) {
		std::ifstream in(path);
		if (!in) {
			// **Not an error.** A fresh install has no file and every action
			// keeps its default, which is the behaviour somebody expects the
			// first time they open the editor.
			return false;
		}

		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line.front() == '#') {
				continue;
			}

			const size_t equals = line.find('=');
			if (equals == std::string::npos) {
				continue;
			}

			auto trim = [](std::string_view text) {
				while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
					text.remove_prefix(1);
				}
				while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
					text.remove_suffix(1);
				}
				return text;
			};

			const std::string_view id = trim(std::string_view(line).substr(0, equals));
			const std::string_view value = trim(std::string_view(line).substr(equals + 1));

			// **An id the build has never heard of is skipped, not refused.**
			// A file written by a later version naming a command this one does
			// not have is not corrupt; it is a file from a later version.
			Keybind *found = nullptr;
			for (Keybind &binding : Table()) {
				if (id == binding.Id) {
					found = &binding;
					break;
				}
			}
			if (found == nullptr) {
				continue;
			}

			Chord chord;
			std::string_view rest = value;
			for (;;) {
				if (rest.starts_with("Ctrl+")) {
					chord.Ctrl = true;
					rest.remove_prefix(5);
				} else if (rest.starts_with("Shift+")) {
					chord.Shift = true;
					rest.remove_prefix(6);
				} else if (rest.starts_with("Alt+")) {
					chord.Alt = true;
					rest.remove_prefix(4);
				} else {
					break;
				}
			}

			chord.Key = KeyOfName(rest);

			// An unreadable key name clears the binding rather than leaving the
			// default in place: the file said something about this command, and
			// guessing it meant the default would be inventing an answer.
			if (!chord.Key) {
				chord = Chord{};
			}

			found->Keys = chord;
		}

		return true;
	}
}
