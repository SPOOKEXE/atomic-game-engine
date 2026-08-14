#pragma once

// The client's own settings, and the one place its defaults are written down.
//
// **A flag is the *default* for the matching command-line option, not a second
// copy of it.** `main` fills an `Options` from here and then lets `--width` and
// friends override what it found, so the precedence a person expects —
// built-in, then config file, then environment, then what they typed — falls
// out of `core::Flags` rather than being re-derived per option.
//
// **The built-in defaults are read off a default-constructed `Options`**, so
// there is one statement of what a client does with nothing configured and it
// is the member initialiser in `Client.hpp`. A table that spelled `1280` again
// would be a second copy of exactly the fact rule 2 is about, and the first
// place it would drift is a `--help` line.
//
// ## What is deliberately not here
//
// Per-run diagnostics: `--frames`, `--capture`, `--profile-seconds`,
// `--enable-profiler`, `--profiler-tab`, `--script` and `--game`. A setting is
// something a deployment decides once; those are things a person decides for
// one run, and a config file that silently held `frames = 200` would be a
// machine where the client exits for no visible reason.
//
// ## A setting that is several
//
// `client.content-sources` is a list: the key repeats and the order is the
// priority order, because the first origin that answers wins. Nothing is split
// on a separator — a source is `host:port` or `dir:PATH` and a path may contain
// anything somebody chose — so the repeat is what expresses the sequence.
// `core::Flags`' `List` kind carries the append-and-replace rule.
//
// @since v0.15

#include <client/Client.hpp>

namespace client {

	// Declares the client's own settings.
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
