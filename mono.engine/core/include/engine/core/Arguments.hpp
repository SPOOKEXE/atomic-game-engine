#pragma once

// Command-line parsing, shared by every program.
//
// Options are declared before parsing rather than probed for afterwards. That
// costs three lines per option and buys three things a scan-for-strcmp loop
// cannot: `--help` that is generated from the truth, an unknown option that is
// an error instead of silence, and a typo in a flag name that fails at the
// command line rather than at the behaviour.
//
//     Arguments arguments("client", "Runs a game.");
//     arguments.Flag("stats", "Open the FPS counter and the frame graph");
//     arguments.Value("frames", "N", "Exit after N frames");
//
//     auto result = arguments.Parse(argc, argv);
//     if (!result.Ok) { ... }
//
// @tier L0 · shared

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {

	// A declarative command-line parser that generates help from its options.
	//
	// The parser stores views of declaration text and `argv`; both must outlive
	// the `Arguments` object and any values read from it.
	class Arguments {
	  public:
		// The outcome of parsing one command line.
		struct Result {
			// Whether every option was declared and supplied a value when required.
			bool Ok = true;
			// Set when `--help` or `-help` was given. The caller decides whether that
			// is an exit, because a program with subcommands may not want it
			// to be.
			bool HelpRequested = false;
			// A diagnostic for the first parse error, or empty when `Ok` is true.
			std::string Error;
		};

		// Creates a parser and declares its built-in `help` flag.
		Arguments(std::string_view program, std::string_view summary);

		// A boolean option. Present or absent; it never takes a value.
		Arguments &Flag(std::string_view name, std::string_view description);

		// An option that takes a value, as either `--name value` or
		// `--name=value`.
		Arguments &Value(std::string_view name, std::string_view valueName, std::string_view description);

		// Parses `argv[1]` onward against the declared options.
		//
		// One or two leading dashes name the same option. A bare `--` ends option
		// parsing, and all later arguments become positional. Unknown options,
		// missing values, and values attached to flags return an error.
		Result Parse(int argc, char **argv);

		// Reports whether a declared option appeared on the parsed command line.
		bool Has(std::string_view name) const;

		// The value given, or nothing if the option was absent. An option
		// declared with Flag always reports nothing.
		//
		// The *last* value, when the option was given more than once. That is
		// the conventional reading of a repeated option and the one a caller
		// who was not expecting a repeat wants; `GetAll` is for the callers who
		// were.
		std::optional<std::string_view> Get(std::string_view name) const;

		// Every value given for an option, in the order they appeared.
		//
		// For an option that names one of several things - the worlds a
		// supervised host was granted, say. Order is kept because a caller may
		// depend on it and sorting here would hide that.
		//
		// @param name The option to read.
		// @return The values, empty when the option was absent.
		std::vector<std::string_view> GetAll(std::string_view name) const;

		// Parses a complete signed decimal integer, or returns `fallback` when the
		// option is absent, empty, or not an integer.
		int64_t GetInteger(std::string_view name, int64_t fallback) const;

		// Parses a complete floating-point number, or returns `fallback` when the
		// option is absent, empty, or not a number.
		double GetNumber(std::string_view name, double fallback) const;

		// Everything after `--`, plus anything that did not start with `-`.
		const std::vector<std::string_view> &Positional() const {
			return Positionals;
		}

		// Builds usage text from the program summary and option declarations.
		std::string Help() const;

	  private:
		struct Option {
			std::string_view Name;
			std::string_view ValueName;
			std::string_view Description;
			bool TakesValue = false;

			bool Present = false;
			std::string_view Given;

			// Every value, for the options that may repeat. `Given` stays the
			// last of them so nothing that only ever expected one has to change.
			std::vector<std::string_view> All;
		};

		Option *Find(std::string_view name);
		const Option *Find(std::string_view name) const;

		std::string_view Program;
		std::string_view Summary;
		std::vector<Option> Options;
		std::vector<std::string_view> Positionals;
	};
}
