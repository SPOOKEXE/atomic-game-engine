#include "ExternalEditor.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Paths.hpp>

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>
#include <studio/Keybinds.hpp>

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
		// first use - `HomeDirectory` reads the environment, and doing that
		// during static initialisation is doing it before `main` had a chance to
		// set anything.
		std::filesystem::path &Root() {
			static std::filesystem::path root =
				HomeDirectory() / "Documents" / "atomic-game-engine" / "studio";
			return root;
		}

		bool &RootOverridden() {
			static bool overridden = false;
			return overridden;
		}

		// Reads a whole file, or reports that there was none.
		bool ReadFile(const std::filesystem::path &path, std::string &out, std::string &error) {
			std::error_code failed;
			if (!std::filesystem::is_regular_file(path, failed)) {
				// **Not an error.** A fresh install has none of these documents,
				// and a caller has to be able to tell that from a file it could
				// not read - which is what an empty `error` says.
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
		// when the key is *absent* - a key holding a string still comes back as
		// a throw. These files are hand-editable by design, so every read has to
		// survive somebody typing the wrong thing.
		float JsonNumber(const json &document, const char *key, float fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_number() ? found->get<float>() : fallback;
		}

		int Integer(const json &document, const char *key, int fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_number_integer() ? found->get<int>() : fallback;
		}

		std::string Words(const json &document, const char *key, const std::string &fallback) {
			const auto found = document.find(key);
			return found != document.end() && found->is_string() ? found->get<std::string>() : fallback;
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
		RootOverridden() = !root.empty();
		Root() = root.empty() ? HomeDirectory() / "Documents" / "atomic-game-engine" / "studio" : root;
	}

	std::filesystem::path ConfigPath(std::string_view leaf) {
		return Root() / leaf;
	}

	bool EnsureConfigRoot() {
		std::error_code failed;

		// `create_directories` reports "already there" as `false` with no error,
		// which is why the error code rather than the return value decides -
		// the same reading `engine::assets::EnsureLocalStore` makes.
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
		// fix by hand - which is most of the reason for JSON over a binary form.
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
		// keeping the earliest of each - a file somebody edited into holding
		// seven paths or a repeat comes back obeying the same rules a session
		// would have produced.
		//
		// Not through `Remember`: that one prepends, so replaying the file
		// through it reverses the list *and* truncates the wrong end - the
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

	const char *Describe(ScaleSide side) {
		switch (side) {
		case ScaleSide::Side:
			return "Side";
		case ScaleSide::Both:
			return "Both";
		case ScaleSide::BothHalf:
			return "Both Half";
		}
		// No default label, so adding a mode is a warning here.
		return "Side";
	}

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
		Scale = JsonNumber(document, "scale", Scale);
		ShowGrid = Flag(document, "showGrid", ShowGrid);
		ShowParticleEmitters = Flag(document, "showParticleEmitters", ShowParticleEmitters);
		if (const auto dataStore = document.find("dataStore");
			dataStore != document.end() && dataStore->is_object()) {
			DataStoreEnabled = Flag(*dataStore, "enabled", DataStoreEnabled);
			DataStoreRoot = Words(*dataStore, "root", DataStoreRoot);
			if (const auto provider = engine::datastore::ProviderOf(
					Words(*dataStore, "provider", engine::datastore::Describe(DataStoreProvider))
				)) {
				DataStoreProvider = *provider;
			}
			if (const auto environment = engine::world::SharedStoreEnvironmentOf(
					Words(*dataStore, "environment", engine::world::Describe(DataStoreEnvironment))
				)) {
				DataStoreEnvironment = *environment;
			}
			DataStoreHttpEndpoint = Words(*dataStore, "httpEndpoint", DataStoreHttpEndpoint);
			DataStoreHttpHost = Words(*dataStore, "httpHost", DataStoreHttpHost);
			DataStoreHttpPrefix = Words(*dataStore, "httpPrefix", DataStoreHttpPrefix);
			DataStoreHttpAuthorization = Words(*dataStore, "httpAuthorization", DataStoreHttpAuthorization);
		}
		if (const auto sourceEditor = document.find("scriptEditor");
			sourceEditor != document.end() && sourceEditor->is_object()) {
			if (const auto kind = ExternalEditorKindOf(
					Words(*sourceEditor, "externalEditor", Describe(SourceEditor.Kind))
				)) {
				SourceEditor.Kind = *kind;
			}
			SourceEditor.Executable = Words(*sourceEditor, "executable", SourceEditor.Executable);
			ScriptMinimap = Flag(*sourceEditor, "minimap", ScriptMinimap);
			if (const auto background = sourceEditor->find("background");
				background != sourceEditor->end() && background->is_string()) {
				if (const std::optional<unsigned int> parsed =
						engine::ui::ParseColourText(background->get<std::string>())) {
					ScriptBackground = *parsed;
				}
			}
		}
		SnapEnabled = Flag(document, "snap", SnapEnabled);
		SnapDistance = JsonNumber(document, "gridStep", SnapDistance);
		SnapDegrees = JsonNumber(document, "rotationStep", SnapDegrees);
		PivotEditing = Flag(document, "pivotEditing", PivotEditing);
		DragAligns = Flag(document, "dragAligns", DragAligns);
		ShowFacing = Flag(document, "showFacing", ShowFacing);

		// **Written as its name rather than its index.** An index would make
		// reordering `ScaleSide` a silent change to how everybody's scale drag
		// behaves, and this is a file a person reads.
		if (const auto sides = document.find("scaleSides"); sides != document.end() && sides->is_string()) {
			const std::string wanted = sides->get<std::string>();
			for (size_t index = 0; index < SCALE_SIDE_COUNT; index++) {
				const auto side = static_cast<ScaleSide>(index);
				if (wanted == Describe(side)) {
					Sides = side;
					break;
				}
			}
		}
		ControlPort = Integer(document, "controlPort", ControlPort);

		if (const auto rates = document.find("frameRates"); rates != document.end() && rates->is_object()) {
			Uncapped = Flag(*rates, "uncapped", Uncapped);
			InterfaceActiveHz = JsonNumber(*rates, "interfaceActive", InterfaceActiveHz);
			InterfaceIdleHz = JsonNumber(*rates, "interfaceIdle", InterfaceIdleHz);
			RendererFocusedHz = JsonNumber(*rates, "rendererFocused", RendererFocusedHz);
			RendererUnfocusedHz = JsonNumber(*rates, "rendererUnfocused", RendererUnfocusedHz);
		}

		if (const auto panels = document.find("panels"); panels != document.end() && panels->is_object()) {
			ShowStatistics = Flag(*panels, "statistics", ShowStatistics);
			ShowFrameGraph = Flag(*panels, "frameGraph", ShowFrameGraph);
			ShowHeap = Flag(*panels, "heap", ShowHeap);
			ShowAssets = Flag(*panels, "assets", ShowAssets);
			ShowControl = Flag(*panels, "control", ShowControl);
		}

		// **Words rather than numbers, and matched against the same names the
		// panel shows.** A rule written as `{"subject": 2}` is a file nobody can
		// read or hand-edit, and an enum that gains a value in the middle would
		// silently turn every saved rule into a different one.
		//
		// A rule naming a subject this build does not know is dropped rather
		// than defaulted: an unrecognised word means the file came from a
		// different version, and arming a rule that watches something other
		// than what it says is worse than not arming it.
		if (const auto rules = document.find("frameGraphRules");
			rules != document.end() && rules->is_array()) {
			FrameGraphRules.clear();
			for (const json &entry : *rules) {
				if (!entry.is_object()) {
					continue;
				}

				engine::core::FrameTrigger rule;
				rule.Name = Words(entry, "span", "");
				rule.Threshold = JsonNumber(entry, "threshold", 0.0f);
				rule.Enabled = Flag(entry, "enabled", true);

				const std::string subject = Words(entry, "subject", "");
				bool known = false;
				for (size_t at = 0; at <= static_cast<size_t>(engine::core::TriggerSubject::Dropped); at++) {
					const auto option = static_cast<engine::core::TriggerSubject>(at);
					if (subject == engine::core::GetTriggerSubjectName(option)) {
						rule.Subject = option;
						known = true;
						break;
					}
				}
				if (!known) {
					continue;
				}

				// The comparison defaults rather than drops: a missing or
				// misspelled `test` still leaves a rule that says what it
				// watches, and "over" is what every rule anybody writes means.
				const std::string test = Words(entry, "test", "");
				if (test == engine::core::GetTriggerTestName(engine::core::TriggerTest::Below)) {
					rule.Test = engine::core::TriggerTest::Below;
				}

				const std::string category = Words(entry, "category", "");
				for (size_t at = 0; at < static_cast<size_t>(engine::core::ProfileCategory::Count); at++) {
					const auto option = static_cast<engine::core::ProfileCategory>(at);
					if (category == engine::core::GetCategoryName(option)) {
						rule.Category = option;
						break;
					}
				}

				FrameGraphRules.push_back(std::move(rule));
			}
		}

		// **A missing array and an empty one are different answers.** Absent is
		// a file written before the preference existed and means "use the
		// catalogue's defaults"; present and empty is somebody who unticked
		// every row and means an empty new game, which is a thing they are
		// allowed to want. `DefaultWorlds` stays empty in the first case and the
		// editor decides; the flag below is what tells the two apart.
		if (const auto worlds = document.find("defaultWorlds");
			worlds != document.end() && worlds->is_array()) {
			DefaultWorldsChosen = true;
			DefaultWorlds.clear();
			for (const auto &entry : *worlds) {
				if (entry.is_string()) {
					DefaultWorlds.push_back(entry.get<std::string>());
				}
			}
		}

		// **Absent means "never configured", and every field falls back to the
		// built-in.** An install that has never opened the Discord Presence
		// page has no object here at all, and what that has to mean is "report
		// nothing" rather than "report with an empty application id".
		if (const auto presence = document.find("discord");
			presence != document.end() && presence->is_object()) {
			Discord.Enabled = Flag(*presence, "enabled", Discord.Enabled);
			Discord.ApplicationId = Words(*presence, "applicationId", Discord.ApplicationId);
			Discord.Details = Words(*presence, "details", Discord.Details);
			Discord.State = Words(*presence, "state", Discord.State);
			Discord.LargeImage = Words(*presence, "largeImage", Discord.LargeImage);
			Discord.LargeText = Words(*presence, "largeText", Discord.LargeText);
			Discord.ButtonLabel = Words(*presence, "buttonLabel", Discord.ButtonLabel);
			Discord.ButtonUrl = Words(*presence, "buttonUrl", Discord.ButtonUrl);
			Discord.ShowElapsed = Flag(*presence, "showElapsed", Discord.ShowElapsed);
			Discord.HideNames = Flag(*presence, "hideNames", Discord.HideNames);
			Discord.JoinSecrets = Flag(*presence, "joinSecrets", Discord.JoinSecrets);
		}

		// **A colour nobody recognises is skipped, not an error.** This document
		// is one a person edits by hand and one an older build wrote, and the
		// same rule holds for both: read what is understood, leave the rest, and
		// never refuse the whole file over one line. A panel that no longer
		// exists keeps its entry rather than being pruned - renaming a panel
		// back would otherwise silently lose the colours somebody chose.
		if (const auto colours = document.find("panelColours");
			colours != document.end() && colours->is_object()) {
			for (const auto &[panel, chosen] : colours->items()) {
				if (panel.empty() || !chosen.is_object()) {
					continue;
				}

				engine::ui::ThemeColours entry;
				for (const auto &[name, value] : chosen.items()) {
					const std::optional<engine::ui::ThemeColour> which = engine::ui::ParseThemeColour(name);
					if (!which || !value.is_string()) {
						continue;
					}

					// Written as `RRGGBBAA` text rather than as a number,
					// because a colour in a file somebody reads is `"2E3440FF"`
					// and not `4281545792`. `ParseColourText` takes a leading
					// `#` and a six-digit form, so what somebody pastes out of a
					// palette works.
					if (const std::optional<unsigned int> packed =
							engine::ui::ParseColourText(value.get<std::string>())) {
						entry[*which] = *packed;
					}
				}

				if (entry.Any()) {
					PanelColours[panel] = entry;
				}
			}
		}

		// **Clamped on the way in, not on the way out.** A scale of zero is a
		// window nobody can read and a port past 65535 is a bind that fails with
		// a message about the port rather than about the file - and both are one
		// hand edit away.
		Scale = std::clamp(Scale, 0.5f, 4.0f);
		// A step of zero would round every drag onto one point. Snapping is
		// turned *off* rather than set to nothing, which is what the checkbox
		// beside the field already means.
		SnapDistance = std::max(0.001f, SnapDistance);
		SnapDegrees = std::max(0.001f, SnapDegrees);
		ControlPort = std::clamp(ControlPort, 0, 65535);

		// **Clamped rather than refused.** 361 means "this one imposes no
		// ceiling"; a negative is not, and a file with one in it is a typo rather
		// than a document to reject.
		InterfaceActiveHz = std::clamp(InterfaceActiveHz, 0.0f, 361.0f);
		InterfaceIdleHz = std::clamp(InterfaceIdleHz, 0.0f, 361.0f);
		RendererFocusedHz = std::clamp(RendererFocusedHz, 0.0f, 361.0f);
		RendererUnfocusedHz = std::clamp(RendererUnfocusedHz, 0.0f, 361.0f);
		return true;
	}

	bool Preferences::Save() const {
		// **Only the panels that carry a colour, and only the colours they
		// carry.** A document listing every panel with every slot would be four
		// hundred lines describing the default, and the one panel somebody
		// actually recoloured would be impossible to find in it.
		json panelColours = json::object();
		for (const auto &[panel, chosen] : PanelColours) {
			json entry = json::object();
			for (size_t index = 0; index < engine::ui::THEME_COLOUR_COUNT; index++) {
				if (!chosen.Values[index]) {
					continue;
				}

				entry[engine::ui::Describe(static_cast<engine::ui::ThemeColour>(index))] =
					engine::ui::ColourText(*chosen.Values[index]);
			}
			if (!entry.empty()) {
				panelColours[panel] = std::move(entry);
			}
		}

		// A rule per line, spelled the way the panel spells it. `span` and
		// `category` are written whatever the subject is: a rule switched from
		// a span to a frame keeps the name it had, so switching back does not
		// mean typing it again.
		json frameGraphRules = json::array();
		for (const engine::core::FrameTrigger &rule : FrameGraphRules) {
			frameGraphRules.push_back(
				json{
					{"subject", engine::core::GetTriggerSubjectName(rule.Subject)},
					{"span", rule.Name},
					{"category", engine::core::GetCategoryName(rule.Category)},
					{"test", engine::core::GetTriggerTestName(rule.Test)},
					{"threshold", rule.Threshold},
					{"enabled", rule.Enabled},
				}
			);
		}

		json document{
			{"scale", Scale},
			{"showGrid", ShowGrid},
			{"showParticleEmitters", ShowParticleEmitters},
			{"dataStore",
			 json{
				 {"enabled", DataStoreEnabled},
				 {"provider", engine::datastore::Describe(DataStoreProvider)},
				 {"root", DataStoreRoot},
				 {"environment", engine::world::Describe(DataStoreEnvironment)},
				 {"httpEndpoint", DataStoreHttpEndpoint},
				 {"httpHost", DataStoreHttpHost},
				 {"httpPrefix", DataStoreHttpPrefix},
				 {"httpAuthorization", DataStoreHttpAuthorization},
			 }},
			{"scriptEditor",
			 json{
				 {"externalEditor", Describe(SourceEditor.Kind)},
				 {"executable", SourceEditor.Executable},
				 {"minimap", ScriptMinimap},
			 }},
			{"snap", SnapEnabled},
			{"gridStep", SnapDistance},
			{"rotationStep", SnapDegrees},
			{"pivotEditing", PivotEditing},
			{"scaleSides", Describe(Sides)},
			{"dragAligns", DragAligns},
			{"showFacing", ShowFacing},
			{"controlPort", ControlPort},
			{"panels",
			 json{
				 {"statistics", ShowStatistics},
				 {"frameGraph", ShowFrameGraph},
				 {"heap", ShowHeap},
				 {"assets", ShowAssets},
				 {"control", ShowControl},
			 }},
			{"frameGraphRules", std::move(frameGraphRules)},
			{"panelColours", std::move(panelColours)},
			{"discord",
			 json{
				 {"enabled", Discord.Enabled},
				 {"applicationId", Discord.ApplicationId},
				 {"details", Discord.Details},
				 {"state", Discord.State},
				 {"largeImage", Discord.LargeImage},
				 {"largeText", Discord.LargeText},
				 {"buttonLabel", Discord.ButtonLabel},
				 {"buttonUrl", Discord.ButtonUrl},
				 {"showElapsed", Discord.ShowElapsed},
				 {"hideNames", Discord.HideNames},
				 {"joinSecrets", Discord.JoinSecrets},
			 }},
			{"frameRates",
			 json{
				 {"uncapped", Uncapped},
				 {"interfaceActive", InterfaceActiveHz},
				 {"interfaceIdle", InterfaceIdleHz},
				 {"rendererFocused", RendererFocusedHz},
				 {"rendererUnfocused", RendererUnfocusedHz},
			 }},
		};
		if (ScriptBackground.has_value()) {
			document["scriptEditor"]["background"] = engine::ui::ColourText(*ScriptBackground);
		}

		if (DefaultWorldsChosen) {
			document["defaultWorlds"] = DefaultWorlds;
		}

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
		// old path again - `SaveConfiguration` only ever writes the config
		// folder - so the first run after this change reads the old file and
		// every run after it reads the new one.
		template <class Load>
		bool
		LoadWithLegacy(const std::filesystem::path &wanted, const std::filesystem::path &legacy, Load load) {
			if (load(wanted)) {
				return true;
			}

			// An explicit root is an isolation boundary. Falling back to files
			// beside the installed binary would make a test run read a person's
			// keybinds and content origins despite being pointed at a scratch root.
			if (RootOverridden()) {
				return false;
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
		// **This program's presence wording, before the file is read over it.**
		// `discord::Settings` cannot carry these: the four programs that report
		// say different things, so a default written in the module would be one
		// of them pretending to be all four. `Preferences::Load` leaves alone
		// anything the document does not mention, which is what makes setting
		// them here the same as declaring them.
		Prefs.Discord.Details = "Editing {place}";
		Prefs.Discord.State = "{instances} instances in {world}";

		// **The display's own scale as the default, set the same way and for the
		// same reason as the two lines above.** A high-density screen reports two
		// or more, and an editor that came up at one drew every panel at
		// framebuffer resolution with one-times metrics - which is the whole
		// interface at half size, on the machine most likely to be somebody's
		// main one. `Preferences::Load` leaves alone what the document mentions,
		// so anybody who has moved the slider keeps their number.
		if (Window != nullptr) {
			if (const float displayScale = SDL_GetWindowDisplayScale(Window); displayScale > 0.0f) {
				Prefs.Scale = displayScale;
			}
		}

		// Read before anything is applied, so a broken file leaves every default
		// in place rather than half of them.
		Prefs.Load();
		Recent.Load();

		ShowGrid = Prefs.ShowGrid;
		ShowParticleEmitters = Prefs.ShowParticleEmitters;
		ShowControl = Prefs.ShowControl;

		// Armed here rather than when the panel first draws, because the panel
		// may never be opened and a rule is worth arming either way - the point
		// of one is catching a frame nobody was watching for.
		FrameGraphState.Triggers = Prefs.FrameGraphRules;
		engine::core::FrameGraph::SetTriggers(FrameGraphState.Triggers);
		ControlPortField = Prefs.ControlPort;
		SnapEnabled = Prefs.SnapEnabled;
		SnapDistance = Prefs.SnapDistance;
		SnapDegrees = Prefs.SnapDegrees;
		PivotEditing = Prefs.PivotEditing;
		ScaleSides = Prefs.Sides;
		DragAligns = Prefs.DragAligns;
		ShowFacing = Prefs.ShowFacing;

		// **Read back, which it never was.** Every other field on this page is
		// applied here and the scale was only ever *written* - so moving the
		// slider changed the editor for that session, was saved, and was
		// silently discarded on the next start. `--scale` still wins, in the
		// same way and for the same reason the panel flags below do.
		if (!Settings.ScaleAuthored) {
			Settings.Scale = Prefs.Scale;
		}

		InterfaceActiveHz = Prefs.InterfaceActiveHz;
		InterfaceIdleHz = Prefs.InterfaceIdleHz;
		RendererFocusedHz = Prefs.RendererFocusedHz;
		RendererUnfocusedHz = Prefs.RendererUnfocusedHz;
		Uncapped = Prefs.Uncapped;

		// **The panel flags are ORed rather than assigned**, because `Options`
		// has already reconciled a command-line flag against this same file -
		// see `app/main.cpp`. Assigning here would undo a `--stats` given for
		// one run.
		ShowStatistics = ShowStatistics || Prefs.ShowStatistics;
		ShowFrameGraph = ShowFrameGraph || Prefs.ShowFrameGraph;
		ShowHeap = ShowHeap || Prefs.ShowHeap;
		ShowAssets = ShowAssets || Prefs.ShowAssets;

		ContentSourcesPath = ConfigPath("cdn.json");
		const bool loaded = LoadWithLegacy(
			ContentSourcesPath,
			engine::core::Paths::Base() / "studio-content.ini",
			[this](const std::filesystem::path &path) { return Content.Load(path); }
		);

		if (!loaded) {
			// A fresh install gets one origin on this machine, which is what
			// works with nothing else running - `DeliverySettings::Default`.
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
		Prefs.ShowParticleEmitters = ShowParticleEmitters;
		Prefs.ShowControl = ShowControl;
		Prefs.FrameGraphRules = FrameGraphState.Triggers;
		Prefs.ShowStatistics = ShowStatistics;
		Prefs.ShowFrameGraph = ShowFrameGraph;
		Prefs.ShowHeap = ShowHeap;
		Prefs.ShowAssets = ShowAssets;
		Prefs.ControlPort = ControlPortField;
		Prefs.SnapEnabled = SnapEnabled;
		Prefs.SnapDistance = SnapDistance;
		Prefs.SnapDegrees = SnapDegrees;
		Prefs.PivotEditing = PivotEditing;
		Prefs.Sides = ScaleSides;
		Prefs.DragAligns = DragAligns;
		Prefs.ShowFacing = ShowFacing;
		Prefs.Scale = Settings.Scale;
		Prefs.InterfaceActiveHz = InterfaceActiveHz;
		Prefs.InterfaceIdleHz = InterfaceIdleHz;
		Prefs.RendererFocusedHz = RendererFocusedHz;
		Prefs.RendererUnfocusedHz = RendererUnfocusedHz;
		Prefs.Uncapped = Uncapped;

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
