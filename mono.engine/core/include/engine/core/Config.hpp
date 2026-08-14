#pragma once

// The three adapters that turn a source of settings into `core::Flags`.
//
// **An adapter each, rather than three parsers inside `Flags`.** `Flags` is a
// table with a precedence rule and knows nothing about files, environments or
// `argv`; this is the only place any of those three are read. That is what lets
// a program add a source without the store learning a fourth format, and it is
// why `Flags::Set` takes a `FlagSource` rather than working one out.
//
// Every adapter reports what it applied and what it did not, because a settings
// file that is silently ignored is the failure mode all three of these have.
//
// **A key that names no declared flag is an error.** `core::Arguments` already
// takes that position for a command line and the argument is the same one: a
// typo in a config file that produces silence fails at the behaviour, days
// later, in a place that has nothing to do with the file.
//
// ## The file
//
// Sectioned `key = value`, where the section is the flag name's prefix:
//
//     # a comment
//     [content]
//     gif = false        ; content.gif
//     svg = true         ; content.svg
//
//     [server]
//     tick-rate = 30     ; server.tick-rate
//
// A key outside any section names a flag with no prefix. `#` and `;` start a
// comment wherever they appear outside a value's quotes; a value may be quoted
// with `"` to keep leading, trailing or comment-like characters.
//
// ## The environment
//
// `ATOMIC_CONTENT_GIF=false` sets `content.gif`. The prefix marks the variables
// that are meant for this engine, the rest of the name is lowercased, and `_`
// becomes `.` — except that a flag name's own words are joined with `-`, so the
// mapping is against the **declared table** rather than by string surgery. That
// is deliberate: guessing that `SERVER_TICK_RATE` means `server.tick-rate`
// rather than `server.tick.rate` is not something a rule can do, and looking it
// up is.
//
// @tier L0 · shared
// @since v0.15

#include <engine/core/Flags.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

	class Arguments;

	// What one adapter did.
	struct ConfigReport {
		// Whether every key named a declared flag and carried a usable value.
		bool Ok = true;

		// The first thing that was wrong, with a file and line where there is
		// one. Empty when `Ok`.
		std::string Error;

		// Keys that set a flag.
		size_t Applied = 0;

		// Keys that named a declared flag already set by something of higher
		// precedence. Not an error — it is the precedence rule working — but
		// worth reporting, because "my config file does nothing" is otherwise a
		// mystery.
		size_t Outranked = 0;
	};

	// Reads settings out of a file, the environment and a command line.
	class Config {
	  public:
		// Applies every key in `path` at `FlagSource::ConfigFile`.
		//
		// **A missing file is not an error and an unreadable one is.** A program
		// with no config file is the ordinary case; a config file that exists
		// and cannot be opened is a deployment that meant something by it.
		//
		// @param path Where the file is.
		// @return What was applied, and the first fault.
		static ConfigReport ApplyFile(const std::filesystem::path &path);

		// Applies every `PREFIX`-named environment variable at
		// `FlagSource::Environment`.
		//
		// A variable whose name matches no declared flag is **ignored rather
		// than refused**, unlike a config key: an environment is shared with
		// every other program on the machine and this engine does not own the
		// namespace it is looking in.
		//
		// @param prefix What marks a variable as this engine's.
		// @return What was applied.
		static ConfigReport ApplyEnvironment(std::string_view prefix = "ATOMIC_");

		// Applies every `--flag name=value` at `FlagSource::CommandLine`.
		//
		// The option is declared by `DeclareOption` so a program's `--help`
		// carries it, and it may repeat.
		//
		// @param arguments A parsed command line.
		// @return What was applied, and the first fault.
		static ConfigReport ApplyArguments(const Arguments &arguments);

		// Declares the settings `core` itself owns, and applies them.
		//
		// `engine.log-level` today, which is what `--verbose` was the only way
		// to reach. Applied by `Apply` because both live in this module — a
		// setting whose owner is above `core` declares and applies its own,
		// which is what `parallel::DeclareFlags` does.
		//
		// @return `false` when a name collided, which is a bug in a table.
		static bool DeclareEngineFlags();

		// Declares the options this adapter reads, on a program's parser.
		//
		// **One call rather than four copies**, because four programs take the
		// same three options and a fifth spelling `--conifg` would be found by
		// somebody months later.
		//
		// @param arguments The parser to declare on.
		static void DeclareOptions(Arguments &arguments);

		// Runs all three adapters in precedence order and freezes.
		//
		// The whole of what a program's startup owes the flag layer: the file
		// `--config` names or `ATOMIC_CONFIG` points at, then the environment,
		// then `--flag`, then `Flags::Freeze`. A program calls this once, after
		// declaring the flag tables it wants and parsing its command line, and
		// before it starts anything.
		//
		// **Frozen even when something was wrong**, because a program that
		// carries on after a bad config file must not also be one whose flags
		// can still move.
		//
		// @param arguments A parsed command line.
		// @return What was applied, and the first fault. A caller that treats a
		//         fault as fatal is doing the right thing; one that logs and
		//         continues gets the built-in defaults for whatever failed.
		static ConfigReport Apply(const Arguments &arguments);

		// Whether `--flags` was given.
		//
		// **Asked rather than acted on**, because printing the settings and
		// stopping is the caller's decision: the studio wants to carry on and a
		// dedicated server run from a deployment script does not.
		//
		// @param arguments A parsed command line.
		// @return `true` when the listing was asked for.
		static bool ListingWanted(const Arguments &arguments);

		// The name of the flag a section and key spell.
		//
		// Exposed because the file format's one rule is worth testing directly
		// and worth reusing: `cdn`'s dashboard prints a key the way a file would
		// have to write it.
		//
		// @param section The `[section]` in force, or empty.
		// @param key     The key.
		// @return `section.key`, or `key` with no section.
		static std::string FlagNameOf(std::string_view section, std::string_view key);
	};
}
