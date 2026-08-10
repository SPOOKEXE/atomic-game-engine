#include <studio/Config.hpp>

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace studio {

	namespace {
		using nlohmann::json;

		// Where a person's own files live.
		//
		// **The same three-step lookup `cdn::LocalStore` makes**, and it is
		// duplicated rather than shared for one reason: `Mono::cdn` is a content
		// origin and this is an editor, and a `client`-tier header exported so
		// that a second module could ask where `$HOME` is would be a strange
		// thing for a content library to offer. Four lines and a comment beats a
		// public function named after somebody else's job.
		std::filesystem::path HomeDirectory() {
			if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0') {
				return std::filesystem::path(home);
			}
			if (const char *profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
				return std::filesystem::path(profile);
			}

			// A container or a service account rather than a person. Neither is
			// a case to abort in, and the working directory is where every other
			// path in that situation already lands.
			return std::filesystem::current_path();
		}

		// The root, resolved once and overridable.
		//
		// A function-local static rather than a namespace one, so it is built on
		// first use — `HomeDirectory` reads the environment, and doing that
		// during static initialisation is doing it before `main` had a chance to
		// set anything.
		std::filesystem::path &Root() {
			static std::filesystem::path root =
				HomeDirectory() / "Documents" / "atomic-game-engine" / "studio";
			return root;
		}

		// Reads a whole file, or reports that there was none.
		bool ReadFile(const std::filesystem::path &path, std::string &out, std::string &error) {
			std::error_code failed;
			if (!std::filesystem::is_regular_file(path, failed)) {
				// **Not an error.** A fresh install has none of these documents,
				// and a caller has to be able to tell that from a file it could
				// not read — which is what an empty `error` says.
				return false;
			}

			std::ifstream in(path, std::ios::binary);
			if (!in) {
				error = "could not open " + path.string();
				return false;
			}

			std::ostringstream buffer;
			buffer << in.rdbuf();
			out = buffer.str();
			return true;
		}

		// A number read defensively.
		//
		// **`value()` alone is not enough**, because it takes the default only
		// when the key is *absent* — a key holding a string still comes back as
		// a throw. These files are hand-editable by design, so every read has to
		// survive somebody typing the wrong thing.
		float Number(const json &document, const char *key, float fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_number() ? found->get<float>() : fallback;
		}

		int Integer(const json &document, const char *key, int fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_number_integer() ? found->get<int>() : fallback;
		}

		bool Flag(const json &document, const char *key, bool fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_boolean() ? found->get<bool>() : fallback;
		}
	}

	const std::filesystem::path &ConfigRoot() {
		return Root();
	}

	void SetConfigRoot(const std::filesystem::path &root) {
		Root() = root.empty() ? HomeDirectory() / "Documents" / "atomic-game-engine" / "studio" : root;
	}

	std::filesystem::path ConfigPath(std::string_view leaf) {
		return Root() / leaf;
	}

	bool EnsureConfigRoot() {
		std::error_code failed;

		// `create_directories` reports "already there" as `false` with no error,
		// which is why the error code rather than the return value decides —
		// the same reading `cdn::EnsureLocalStore` makes.
		std::filesystem::create_directories(Root(), failed);
		if (failed) {
			ENGINE_ERROR("studio config: could not create {}: {}", Root().string(), failed.message());
			return false;
		}
		return true;
	}

	bool ReadConfigDocument(std::string_view leaf, json &out, std::string &error) {
		std::string text;
		if (!ReadFile(ConfigPath(leaf), text, error)) {
			return false;
		}

		json document = json::parse(text, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			// **Named rather than silently defaulted.** A file somebody edited
			// by hand and broke should say so once, because the alternative is
			// an editor that quietly forgot every preference and no line
			// anywhere saying why.
			error = ConfigPath(leaf).string() + " is not a JSON object";
			return false;
		}

		out = std::move(document);
		return true;
	}

	bool WriteConfigDocument(std::string_view leaf, const json &document, std::string &error) {
		if (!EnsureConfigRoot()) {
			error = "could not create " + Root().string();
			return false;
		}

		const std::filesystem::path path = ConfigPath(leaf);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			error = "could not write " + path.string();
			return false;
		}

		// Indented, because every one of these is a file somebody may open and
		// fix by hand — which is most of the reason for JSON over a binary form.
		out << document.dump(2) << "\n";
		return out.good();
	}

	// --- recent projects -----------------------------------------------------

	void RecentProjects::Remember(const std::filesystem::path &path) {
		if (path.empty()) {
			return;
		}

		Forget(path);
		Paths.insert(Paths.begin(), path);

		if (Paths.size() > LIMIT) {
			Paths.resize(LIMIT);
		}
	}

	void RecentProjects::Forget(const std::filesystem::path &path) {
		Paths.erase(std::remove(Paths.begin(), Paths.end(), path), Paths.end());
	}

	bool RecentProjects::Load() {
		json document;
		std::string error;

		if (!ReadConfigDocument("recent.json", document, error)) {
			if (!error.empty()) {
				ENGINE_WARN("studio config: {}", error);
			}
			return false;
		}

		const auto projects = document.find("projects");
		if (projects == document.end() || !projects->is_array()) {
			return false;
		}

		// **In file order, which is already newest first**, and deduplicated
		// keeping the earliest of each — a file somebody edited into holding
		// seven paths or a repeat comes back obeying the same rules a session
		// would have produced.
		//
		// Not through `Remember`: that one prepends, so replaying the file
		// through it reverses the list *and* truncates the wrong end — the
		// oldest five survive instead of the newest five, which is the list
		// backwards and was worth a failing case to find.
		Paths.clear();
		for (const json &entry : *projects) {
			if (!entry.is_string() || Paths.size() >= LIMIT) {
				continue;
			}

			std::filesystem::path path(entry.get<std::string>());
			if (path.empty() || std::find(Paths.begin(), Paths.end(), path) != Paths.end()) {
				continue;
			}
			Paths.push_back(std::move(path));
		}

		return !Paths.empty();
	}

	bool RecentProjects::Save() const {
		json projects = json::array();
		for (const std::filesystem::path &path : Paths) {
			projects.push_back(path.string());
		}

		std::string error;
		if (!WriteConfigDocument("recent.json", json{{"projects", std::move(projects)}}, error)) {
			ENGINE_WARN("studio config: {}", error);
			return false;
		}
		return true;
	}

	// --- preferences ---------------------------------------------------------

	bool Preferences::Load() {
		json document;
		std::string error;

		if (!ReadConfigDocument("preferences.json", document, error)) {
			if (!error.empty()) {
				ENGINE_WARN("studio config: {}", error);
			}
			return false;
		}

		// **Every field defaults to what this object already holds**, so a
		// document written by an older build is read forward rather than
		// clearing whatever it did not mention.
		Scale = Number(document, "scale", Scale);
		ShowGrid = Flag(document, "showGrid", ShowGrid);
		SnapEnabled = Flag(document, "snap", SnapEnabled);
		SnapDistance = Number(document, "gridStep", SnapDistance);
		SnapDegrees = Number(document, "rotationStep", SnapDegrees);
		PivotEditing = Flag(document, "pivotEditing", PivotEditing);
		ControlPort = Integer(document, "controlPort", ControlPort);

		if (const auto panels = document.find("panels");
			panels != document.end() && panels->is_object()) {
			ShowStatistics = Flag(*panels, "statistics", ShowStatistics);
			ShowFrameGraph = Flag(*panels, "frameGraph", ShowFrameGraph);
			ShowAssets = Flag(*panels, "assets", ShowAssets);
			ShowControl = Flag(*panels, "control", ShowControl);
		}

		// **Clamped on the way in, not on the way out.** A scale of zero is a
		// window nobody can read and a port past 65535 is a bind that fails with
		// a message about the port rather than about the file — and both are one
		// hand edit away.
		Scale = std::clamp(Scale, 0.5f, 4.0f);
		// A step of zero would round every drag onto one point. Snapping is
		// turned *off* rather than set to nothing, which is what the checkbox
		// beside the field already means.
		SnapDistance = std::max(0.001f, SnapDistance);
		SnapDegrees = std::max(0.001f, SnapDegrees);
		ControlPort = std::clamp(ControlPort, 0, 65535);
		return true;
	}

	bool Preferences::Save() const {
		const json document{
			{"scale", Scale},
			{"showGrid", ShowGrid},
			{"snap", SnapEnabled},
			{"gridStep", SnapDistance},
			{"rotationStep", SnapDegrees},
			{"pivotEditing", PivotEditing},
			{"controlPort", ControlPort},
			{"panels",
			 json{
				 {"statistics", ShowStatistics},
				 {"frameGraph", ShowFrameGraph},
				 {"assets", ShowAssets},
				 {"control", ShowControl},
			 }},
		};

		std::string error;
		if (!WriteConfigDocument("preferences.json", document, error)) {
			ENGINE_WARN("studio config: {}", error);
			return false;
		}
		return true;
	}

	// --- the editor's own reading of all this --------------------------------

	namespace {
		// Reads a document from the config folder, falling back once to whatever
		// an older build left beside the binary.
		//
		// **The fallback is a move, not a second location.** Nothing writes the
		// old path again — `SaveConfiguration` only ever writes the config
		// folder — so the first run after this change reads the old file and
		// every run after it reads the new one.
		template <class Load>
		bool LoadWithLegacy(
			const std::filesystem::path &wanted, const std::filesystem::path &legacy, Load load
		) {
			if (load(wanted)) {
				return true;
			}

			std::error_code failed;
			if (legacy.empty() || !std::filesystem::is_regular_file(legacy, failed)) {
				return false;
			}

			if (!load(legacy)) {
				return false;
			}

			ENGINE_INFO("studio config: moved {} to {}", legacy.string(), wanted.string());
			return true;
		}
	}

	void Editor::LoadConfiguration() {
		// Read before anything is applied, so a broken file leaves every default
		// in place rather than half of them.
		Prefs.Load();
		Recent.Load();

		ShowGrid = Prefs.ShowGrid;
		ShowControl = Prefs.ShowControl;
		ControlPortField = Prefs.ControlPort;
		SnapEnabled = Prefs.SnapEnabled;
		SnapDistance = Prefs.SnapDistance;
		SnapDegrees = Prefs.SnapDegrees;
		PivotEditing = Prefs.PivotEditing;

		// **The panel flags are ORed rather than assigned**, because `Options`
		// has already reconciled a command-line flag against this same file —
		// see `app/main.cpp`. Assigning here would undo a `--stats` given for
		// one run.
		ShowStatistics = ShowStatistics || Prefs.ShowStatistics;
		ShowFrameGraph = ShowFrameGraph || Prefs.ShowFrameGraph;
		ShowAssets = ShowAssets || Prefs.ShowAssets;

		ContentSourcesPath = ConfigPath("cdn.json");
		const bool loaded = LoadWithLegacy(
			ContentSourcesPath,
			engine::core::Paths::Base() / "studio-content.ini",
			[this](const std::filesystem::path &path) { return Content.Load(path); }
		);

		if (!loaded) {
			// A fresh install gets one origin on this machine, which is what
			// works with nothing else running — `DeliverySettings::Default`.
			Content = ContentSources::Default();
		}

		KeybindPath = ConfigPath("keybinds.json");
		if (LoadWithLegacy(
				KeybindPath,
				engine::core::Paths::Base() / "studio-keybinds.ini",
				[](const std::filesystem::path &path) { return Keybinds::Load(path); }
			)) {
			ENGINE_INFO("keybinds from {}", KeybindPath.string());
		}
	}

	void Editor::SaveConfiguration() {
		// **From the live interface rather than from `Prefs`.** The panel
		// toggles are the editor's own flags and the menu writes them directly;
		// reading them back here is what makes "it remembered what I left open"
		// true without every toggle having to write a file.
		Prefs.ShowGrid = ShowGrid;
		Prefs.ShowControl = ShowControl;
		Prefs.ShowStatistics = ShowStatistics;
		Prefs.ShowFrameGraph = ShowFrameGraph;
		Prefs.ShowAssets = ShowAssets;
		Prefs.ControlPort = ControlPortField;
		Prefs.SnapEnabled = SnapEnabled;
		Prefs.SnapDistance = SnapDistance;
		Prefs.SnapDegrees = SnapDegrees;
		Prefs.PivotEditing = PivotEditing;
		Prefs.Scale = Settings.Scale;

		Prefs.Save();
		Recent.Save();

		if (!KeybindPath.empty() && !Keybinds::Save(KeybindPath)) {
			ENGINE_WARN("could not write {}", KeybindPath.string());
		}
		if (!ContentSourcesPath.empty() && !Content.Save(ContentSourcesPath)) {
			ENGINE_WARN("could not write {}", ContentSourcesPath.string());
		}
	}
}
