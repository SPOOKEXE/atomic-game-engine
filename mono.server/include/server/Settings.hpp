#pragma once

// The server's own settings, and the one place its defaults are written down.
//
// The client's `client/Settings.hpp` carries the whole argument; this is the
// same shape one program along. A flag is the *default* for the matching
// command-line option rather than a second copy of it, and the built-in values
// are read off a default-constructed `Options` so `Server.hpp`'s member
// initialisers stay the one statement of what a server does with nothing
// configured.
//
// ## What is deliberately not here
//
// Per-run choices: `--ticks`, `--seconds`, `--record`, `--replay`, `--game`,
// `--host`, `--world`, `--remote-world` and `--graph`. A recording path and a
// tick budget belong to one run; a host's granted worlds are handed to it by
// whoever spawned it, and reading them out of a shared config file would give
// every host on a machine the same set.
//
// **`--listen` is here and it is the one that needed thought.** The port and
// "whether to listen at all" are two settings on purpose - `Options::Listening`
// says why zero is a real port rather than a way of saying no - so they are two
// flags, and `--listen PORT` sets both.
//
// @since v0.15

#include <server/Server.hpp>

namespace server {

	// Declares the server's own settings.
	//
	// @return `false` when a name collided, which is a bug in a table.
	bool DeclareFlags();

	// An `Options` filled from the flags.
	//
	// Called after `core::Config::Apply`, and before the command line is read
	// over the top of it.
	//
	// @return The options the settings describe.
	Options OptionsFromFlags();
}
