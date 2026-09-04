#pragma once

// arch-waiver public-header: forward launcher API. `Launcher.hpp` stores and
// exposes modes through this complete value contract.

// The modes this launcher offers, and what each one is on top of a program.
//
// **A mode is not a program.** Play and Join are both the client, and the only
// difference between them is which options are filled in and which few are
// worth showing first - `--game` for one, `--connect` for the other. Modelling
// them as two programs would have meant two copies of the client's option
// table; modelling them as one screen with a radio button would have meant the
// person choosing "join a server" first has to know that a server is joined by
// a client.
//
// **The pinned list is a reading order, not a filter.** Every option the
// program declared is reachable - that is the whole point of generating the
// form from `--describe`. What pinning does is put the four or five that decide
// what the run *is* above the fold, so that a mode is usable without reading
// forty rows first. An option that is pinned and an option that is not are the
// same option in the same command line.
//
// @tier L13 · client
// @since v0.18

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

	// What the launcher does once the child is running.
	//
	// @since v0.18
	enum class Lifetime : uint8_t {
		// Start it and get out of the way: hide the window until the child
		// ends, then come back. For the modes that take over the display -
		// there is nothing useful a second window could be showing while
		// somebody is playing.
		HandOver,

		// Stay up, watch it, and say how it ended. For the modes that are a
		// service rather than a session: a host and an origin are things you
		// start, leave running, watch fall over and start again, and a launcher
		// that vanished after starting one would be a launcher you could not
		// use to stop it.
		Supervise,
	};

	// A value the mode fills in before anybody types anything.
	struct ModePreset {
		// The option's name, without dashes.
		std::string Option;

		// Its value, or empty for a bare flag - which is then simply on.
		std::string Value;
	};

	// One entry on the launcher's front screen.
	struct Mode {
		// Stable, lowercase, and what `--mode` names on this launcher's own
		// command line. **A string rather than the enumerator's position**,
		// because rule 4 is about exactly this: reordering this list must not
		// change what `--mode play` means.
		std::string Id;

		// What the button says.
		std::string Label;

		// One line under the button, saying what the mode is for.
		std::string Blurb;

		// The staged program this mode runs - `mono_add_program`'s name.
		std::string Program;

		// The options shown above the fold, in this order.
		std::vector<std::string> Pinned;

		// What is filled in when the mode is first opened.
		std::vector<ModePreset> Presets;

		// What happens after the child starts.
		Lifetime After = Lifetime::HandOver;
	};

	// Every mode, in the order the front screen shows them.
	//
	// @return The catalogue. Built once per call; there is no global.
	std::vector<Mode> Modes();

	// The distinct programs the catalogue needs - what `Descriptions::Load`
	// should be asked for.
	//
	// @param modes The catalogue.
	// @return Each program once, in first-appearance order.
	std::vector<std::string> ProgramsOf(const std::vector<Mode> &modes);

	// The mode with that id, or null.
	//
	// @param modes The catalogue.
	// @param id    The id to find.
	// @return The mode, or null when nothing has that id.
	const Mode *FindMode(const std::vector<Mode> &modes, std::string_view id);
}
