// Thin argument-parsing entry point over the client library.

#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/parallel/Settings.hpp>
#include <engine/render/DebugPanels.hpp>

#include <cctype>
#include <cdn/LocalStore.hpp>
#include <client/Client.hpp>
#include <client/Settings.hpp>
#include <cstdio>
#include <string>
#include <vector>

namespace {

	// Matches a tab by name, case-insensitively, so `--profiler-tab systems`
	// does not depend on how the enum happens to be spelled.
	bool ParseProfilerTab(std::string_view given, engine::render::ProfilerTab &out) {
		std::string wanted(given);
		for (char &character : wanted) {
			character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
		}

		for (uint8_t index = 0; index < static_cast<uint8_t>(engine::render::ProfilerTab::Count); index++) {
			const auto candidate = static_cast<engine::render::ProfilerTab>(index);
			if (engine::render::GetProfilerTabName(candidate) == wanted) {
				out = candidate;
				return true;
			}
		}
		return false;
	}
}

int main(int argc, char **argv) {
	engine::core::Log::Initialise("client");

	// **Declared before anything is parsed or read**, because a config key that
	// names a flag no registrar declared is an error and a program that
	// declared its tables late would refuse its own settings.
	engine::core::Config::DeclareEngineFlags();
	engine::parallel::DeclareFlags();
	engine::assets::DeclareContentFlags(engine::assets::ContentVerb::Handle);
	client::DeclareFlags();

	engine::core::Arguments arguments("client", "atomic - runs a game.");
	engine::core::Config::DeclareOptions(arguments);

	arguments.Flag("stats", "Open the F3 statistics panel at startup");
	arguments.Flag("net", "Open the F4 network panel at startup (needs --connect)");
	arguments.Flag("graph", "Open the F5 frame graph at startup");
	arguments.Flag("uncapped", "Present without waiting for vblank");
	arguments.Flag("headless", "Run with no window (needs --frames)");
	arguments.Value("max-fps", "N", "Hold this frame rate. Needs --uncapped; 0 is no limit");
	arguments.Flag("verbose", "Log at trace level");
	arguments.Flag(
		"force-serial-compute",
		"Run every parallel dispatch on one thread, so the frame graph keeps every span"
	);

	arguments.Value("entities", "N", "Cubes in the demo scene, per world (default 2048)");
	arguments.Value("worlds", "N", "Worlds to simulate and composite (default 1)");
	arguments.Value("view-spacing", "UNITS", "World units between composited views (default 40)");
	arguments.Value("tick-rate", "HZ", "Simulation ticks per second (default 60)");
	arguments.Value("frames", "N", "Exit after N presented frames");
	arguments.Value(
		"surface-bounces",
		"N",
		"Levels of mirror-in-mirror per frame, overriding the world (default: measured)"
	);
	arguments.Value("width", "PX", "Window width (default 1280)");
	arguments.Value("height", "PX", "Window height (default 720)");
	arguments.Value("profiler-tab", "NAME", "frame, categories, systems or counters");

	arguments.Value("script", "PATH", "Luau script to run at startup (v0.6)");
	arguments.Value("game", "PATH", "Game file to play single-player (.agame)");
	arguments.Value("enable-profiler", "SECONDS", "Wait for a Tracy profiler before starting");
	arguments.Value("profile-seconds", "SECONDS", "Run for this long, then exit");
	arguments.Value("override-assets-directory", "DIR", "Read shaders and data from here");
	arguments.Value("connect", "HOST:PORT", "Replicate a world from this server, beside the demo");
	arguments.Flag("browse", "Look for a server announcing itself on this subnet instead of naming one");
	arguments.Value("browse-seconds", "N", "How long to look before giving up (default 3)");
	arguments.Value("session-name", "NAME", "Join the session with this name rather than the first found");
	arguments.Value("session-id", "HEX", "Join the session with this id - 32 hex characters");
	arguments.Value(
		"session-key", "SECRET", "The secret for a private session: 64 hex characters, or a passphrase"
	);
	arguments.Value("rendezvous", "HOST:PORT", "Reach a session through this rendezvous point");
	arguments.Value(
		"server-key",
		"HEX",
		"64 hex characters - the server identity to pin. Without it a relay in the path can read "
		"everything"
	);
	arguments.Value(
		"cdn", "HOST:PORT", "A content origin, in priority order. 'dir:PATH' for a local store. Repeatable"
	);
	arguments.Value("content-cache", "DIR", "Keep verified content here between runs");
	arguments.Value("publisher-key", "HEX", "64 hex characters - the key whose manifests this client trusts");
	arguments.Value("sound", "PATH", "Play this .wav or .mp3 on a loop - proves audio runs in-game");
	arguments.Value(
		"click",
		"NAME",
		"Press the interface element with this name once, mid-run. A diagnostic; see Options::ClickElement"
	);
	arguments.Value(
		"type",
		"TEXT",
		"Type this into the focused TextBox once, mid-run. Pairs with --click; see Options::TypedText"
	);
	arguments.Value(
		"capture",
		"PATH",
		"Write a BMP of the scene near the end of the run. Needs --frames; renders offscreen"
	);

	const auto parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}

	// **`--verbose` before the settings, so it reaches the settings' own
	// complaints.** It is the one option that has to act before anything can be
	// read, and it is deliberately kept beside `engine.log-level` rather than
	// replaced by it: every recipe in this repository passes it.
	if (arguments.Has("verbose")) {
		engine::core::Log::SetLevel(engine::core::LogLevel::Trace);
	}
	if (arguments.Has("force-serial-compute")) {
		engine::core::Flags::Set("engine.serial-compute", "true", engine::core::FlagSource::CommandLine);
	}

	const engine::core::ConfigReport settings = engine::core::Config::Apply(arguments);
	if (!settings.Ok) {
		std::fprintf(stderr, "%s\n", settings.Error.c_str());
		return 2;
	}
	if (engine::core::Config::ListingWanted(arguments)) {
		std::fputs(engine::core::Flags::Listing().c_str(), stdout);
		return 0;
	}

	// Set before startup so every dispatch uses the measured serial path.
	engine::parallel::ApplyFlags();

	// **The settings first, the command line over the top.** Every `Get*` below
	// takes what the flags produced as its fallback, so the precedence a person
	// expects - built-in, config file, environment, then what they typed -
	// falls out of one rule rather than being re-derived per option.
	client::Options options = client::OptionsFromFlags();
	options.Width = static_cast<int>(arguments.GetInteger("width", options.Width));
	options.Height = static_cast<int>(arguments.GetInteger("height", options.Height));
	options.Entities = static_cast<uint32_t>(arguments.GetInteger("entities", options.Entities));
	options.Worlds = static_cast<uint32_t>(arguments.GetInteger("worlds", options.Worlds));
	options.ViewSpacing = static_cast<float>(arguments.GetNumber("view-spacing", options.ViewSpacing));
	options.TickRate = arguments.GetNumber("tick-rate", options.TickRate);
	options.MaximumFrames = arguments.GetInteger("frames", -1);
	options.SurfaceBounces =
		static_cast<int>(arguments.GetInteger("surface-bounces", options.SurfaceBounces));

	// **`|=` rather than `=`, because these four are bare flags.** An absent
	// `--stats` is silence and not "off", so assigning would make a command line
	// with no panel flags on it overrule a config file that asked for one.
	options.ShowStatistics = options.ShowStatistics || arguments.Has("stats");
	options.ShowNetwork = options.ShowNetwork || arguments.Has("net");
	options.ShowFrameGraph = options.ShowFrameGraph || arguments.Has("graph");
	options.Uncapped = options.Uncapped || arguments.Has("uncapped");
	options.Headless = arguments.Has("headless");

	// **Refused rather than run**, because a headless client has no window to
	// close: without a frame budget it would render forever with nothing on
	// screen to say so, on a machine somebody has probably walked away from.
	// The studio's `--headless` carries the same requirement.
	if (options.Headless && options.MaximumFrames < 0) {
		std::fprintf(stderr, "--headless needs --frames N: there is no window to close.\n");
		return 2;
	}
	options.MaximumFrameRate =
		static_cast<uint32_t>(arguments.GetInteger("max-fps", options.MaximumFrameRate));
	options.ProfileSeconds = arguments.GetNumber("profile-seconds", 0.0);

	if (arguments.Has("enable-profiler")) {
		options.ProfilerWaitSeconds = arguments.GetNumber("enable-profiler", 10.0);
	}

	if (auto script = arguments.Get("script")) {
		options.ScriptPath = std::string(*script);
	}
	if (auto game = arguments.Get("game")) {
		options.GameFile = std::filesystem::path(*game);

		// Loudly rather than silently. A run given both a game file and a scene
		// script has to choose, and a choice nobody was told about is a run
		// that did something other than what was asked.
		if (!options.ScriptPath.empty()) {
			ENGINE_WARN("--game and --script were both given; playing the game file");
			options.ScriptPath.clear();
		}
	}
	if (auto assets = arguments.Get("override-assets-directory")) {
		options.AssetsDirectory = std::filesystem::path(*assets);
	}
	if (auto server = arguments.Get("connect")) {
		options.ConnectAddress = std::string(*server);
	}
	options.Browse = options.Browse || arguments.Has("browse");
	options.BrowseSeconds = arguments.GetNumber("browse-seconds", options.BrowseSeconds);
	if (auto name = arguments.Get("session-name")) {
		options.SessionName = std::string(*name);
	}
	if (auto session = arguments.Get("session-id")) {
		options.SessionIdText = std::string(*session);
	}
	if (auto secret = arguments.Get("session-key")) {
		options.SessionSecret = std::string(*secret);
	}
	if (auto point = arguments.Get("rendezvous")) {
		options.RendezvousAddress = std::string(*point);
	}
	if (auto key = arguments.Get("server-key")) {
		options.ServerKey = std::string(*key);
	}

	// **Prepended, so a named origin outranks a configured one.** The list is
	// priority order and the first that answers wins, so what somebody typed has
	// to sit in front of what their config file already held - which is the same
	// precedence every other setting here follows, expressed as position.
	if (const std::vector<std::string_view> named = arguments.GetAll("cdn"); !named.empty()) {
		std::vector<std::string> sources(named.begin(), named.end());
		sources.insert(sources.end(), options.ContentSources.begin(), options.ContentSources.end());
		options.ContentSources = std::move(sources);
	}

	// **The local store, when nobody named an origin.** `ROADMAP.md` v0.10 asks
	// for the cdn to be hooked up by default, and this is what that means in
	// practice: content published into `~/Documents/atomic-game-engine/cdn`
	// resolves with no flag at all, where before it took `assetc`, `cdn
	// --publish` and a `--cdn dir:...` on every run.
	//
	// **Appended rather than prepended, and only when the list is empty.** A
	// caller who named an origin meant it, and quietly adding a second one behind
	// theirs would make "which store did that texture come from" a question with
	// no answer in the command line.
	//
	// The folder is created rather than merely looked for, so a first run leaves
	// somewhere to drag files into instead of a path that does not exist.
	if (options.ContentSources.empty()) {
		const cdn::LocalPaths local = cdn::DefaultLocalPaths();
		if (cdn::EnsureLocalStore(local)) {
			options.ContentSources.push_back("dir:" + local.Processed.string());
		}
	}
	if (auto cache = arguments.Get("content-cache")) {
		options.ContentCache = std::filesystem::path(*cache);
	}
	if (auto element = arguments.Get("click")) {
		options.ClickElement = std::string(*element);
	}
	if (auto typed = arguments.Get("type")) {
		options.TypedText = std::string(*typed);
	}
	if (auto capture = arguments.Get("capture")) {
		options.Capture = std::filesystem::path(*capture);
	}
	if (auto sound = arguments.Get("sound")) {
		options.SoundPath = std::filesystem::path(*sound);
	}
	if (auto key = arguments.Get("publisher-key")) {
		options.ContentPublisherKey = std::string(*key);
	} else if (options.ContentSources.size() == 1 &&
			   options.ContentSources.front() == "dir:" + cdn::DefaultLocalPaths().Processed.string()) {
		// **The development key, and only for the store on this machine.** A
		// client with no `--publisher-key` refused to start at all, which is
		// right for an origin across a network and was pure friction for the
		// well-known local folder: the same sixty-four characters had to be
		// typed for `--publish` and again here, and getting one wrong produced a
		// client that refused every asset with no hint which half was wrong.
		//
		// **The condition is the whole safety of it.** This fires only when
		// nobody named a source *and* the only source is the default local
		// store's own directory. Naming any origin - `--cdn`, a remote, even
		// another directory - leaves the key required, because a key that
		// everybody knows is not a trust boundary and must never become one for
		// content somebody else served. `cdn::DevelopmentSigningKey` carries the
		// same argument from the publishing end.
		options.ContentPublisherKey = cdn::DevelopmentPublisher().ToHex();
	}

	if (auto tab = arguments.Get("profiler-tab")) {
		if (!ParseProfilerTab(*tab, options.Tab)) {
			std::fprintf(
				stderr, "--profiler-tab: no tab called '%.*s'\n", static_cast<int>(tab->size()), tab->data()
			);
			return 2;
		}
		options.ShowFrameGraph = true;
	}

	// A profiling run wants the graph collecting, or it measures nothing.
	if (options.ProfileSeconds > 0.0) {
		options.ShowFrameGraph = true;
	}

	client::Client client;
	if (!client.Initialise(options)) {
		ENGINE_ERROR("client failed to start");
		return 1;
	}

	return client.Run();
}
