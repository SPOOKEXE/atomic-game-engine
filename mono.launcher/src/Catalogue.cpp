#include <launcher/Catalogue.hpp>

namespace launcher {

	std::vector<Mode> Modes() {
		std::vector<Mode> modes;

		modes.push_back(
			Mode{
				.Id = "play",
				.Label = "Play",
				.Blurb = "Run a game on this machine, with no server and no network.",
				.Program = "client",
				.Pinned = {"game", "worlds", "entities", "tick-rate", "width", "height", "sound"},
				.Presets = {},
				.After = Lifetime::HandOver,
			}
		);

		modes.push_back(
			Mode{
				.Id = "join",
				.Label = "Join",
				.Blurb = "Play on somebody else's server - by address, or by looking on this subnet.",
				.Program = "client",

				// **`browse` above `connect`, which is the reverse of how the
				// client declares them.** Looking is what somebody does when they
				// do not have an address, and not having an address is the state
				// this screen is opened in.
				.Pinned =
					{"browse",
					 "connect",
					 "session-name",
					 "session-key",
					 "server-key",
					 "cdn",
					 "width",
					 "height"},
				.Presets = {},
				.After = Lifetime::HandOver,
			}
		);

		modes.push_back(
			Mode{
				.Id = "host",
				.Label = "Host",
				.Blurb = "Run a server other people connect to. Stays here so you can watch it.",
				.Program = "server",

				// `listen` first because a server with no port serves nobody, and
				// `advertise` second because a server nobody can find is the next
				// thing to go wrong.
				.Pinned =
					{"listen",
					 "advertise",
					 "session-name",
					 "game",
					 "max-clients",
					 "tick-rate",
					 "content-store"},
				.Presets =
					{
						// A port rather than nothing: `--listen` is what turns a
						// simulating server into a reachable one, and a Host screen
						// that starts with it blank is a screen whose one required
						// field is the one nobody notices.
						ModePreset{.Option = "listen", .Value = "7777"},
						ModePreset{.Option = "advertise", .Value = {}},
					},
				.After = Lifetime::Supervise,
			}
		);

		modes.push_back(
			Mode{
				.Id = "studio",
				.Label = "Studio",
				.Blurb = "Open the editor, on a game file or on a new place.",
				.Program = "studio",
				.Pinned = {"game", "rojo", "viewports", "width", "height", "scale"},
				.Presets = {},
				.After = Lifetime::HandOver,
			}
		);

		modes.push_back(
			Mode{
				.Id = "cdn",
				.Label = "Serve content",
				.Blurb = "Run a content origin over a folder. Stays here so you can watch it.",
				.Program = "cdn",
				.Pinned =
					{"store", "port", "advertise", "upstream", "allow-upstream", "rendezvous", "stream-name"},
				.Presets =
					{
						ModePreset{.Option = "port", .Value = "9080"},
					},
				.After = Lifetime::Supervise,
			}
		);

		return modes;
	}

	std::vector<std::string> ProgramsOf(const std::vector<Mode> &modes) {
		std::vector<std::string> programs;
		for (const Mode &mode : modes) {
			bool seen = false;
			for (const std::string &program : programs) {
				seen = seen || program == mode.Program;
			}
			if (!seen) {
				programs.push_back(mode.Program);
			}
		}
		return programs;
	}

	const Mode *FindMode(const std::vector<Mode> &modes, std::string_view id) {
		for (const Mode &mode : modes) {
			if (mode.Id == id) {
				return &mode;
			}
		}
		return nullptr;
	}
}
