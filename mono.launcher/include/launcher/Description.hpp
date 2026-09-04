#pragma once

// arch-waiver public-header: forward launcher API. `Launcher.hpp` builds forms
// from these complete program descriptions.

// What a program says it accepts, read back from `--describe`.
//
// **This is the whole reason the launcher does not carry flag tables of its
// own.** Between them the client, the server, the origin and the studio declare
// about a hundred and twenty options and sixty settings. Transcribed here, they
// would be a hundred and eighty facts with two homes, and the copy that goes
// stale is the one nobody notices - a launcher missing a flag still starts the
// program, so the failure is invisible until somebody needs the flag.
//
// So the launcher asks. `core::Arguments::Describe` generates the answer from
// the same declarations `--help` is generated from, every program in the
// repository answers it because `Arguments` declares the flag itself, and an
// option added next week appears in the launcher with no change here.
//
// **The cost, said out loud, and it is not nothing: four child processes before
// the first frame.** Measured on this machine in a `dev` build, each program
// takes about 0.29 s to start, print its table and exit - so `Load` is roughly
// 1.2 s, and almost all of it is a `-O0` binary being loaded rather than
// anything this does. It is one at a time, in order, deliberately: a thread per
// program would fork from a process that has several, which is the arrangement
// `parallel/Process.hpp` warns about, and 1.2 s once at startup is not worth
// buying that with. There is no cache on disk either - that would be a file
// format, an invalidation rule and a stale-cache failure mode, in exchange for
// a second.
//
// Measure it again in `release` before optimising it. The `dev` preset builds
// first-party code unoptimised and this number is mostly load time.
//
// @tier L13 · client
// @since v0.18

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace launcher {

	// One command-line option a program declared.
	struct DescribedOption {
		// The name, without dashes.
		std::string Name;

		// Whether it is `--name VALUE` rather than a bare switch. **The field
		// the help text only conveys by layout**, and the one a form cannot be
		// built without: it decides checkbox or text box.
		bool TakesValue = false;

		// What the value is called - `PATH`, `N`, `HOST:PORT`. Used as the
		// field's placeholder, so the form says what shape an answer has
		// without the launcher knowing anything about the option.
		std::string ValueName;

		// The one-line description, shown as the field's tooltip.
		std::string Description;
	};

	// One setting from `core::Flags`' declared table.
	//
	// A second surface, because a program has two: options are for one run and
	// settings arrive from a config file, the environment or `--flag`. The
	// launcher writes them as `--flag NAME=VALUE`, which is the spelling that
	// outranks the other two and therefore the only one a launcher can be sure
	// of.
	struct DescribedSetting {
		// Dotted and lowercase - `content.gif`, `server.tick-rate`.
		std::string Name;

		// `boolean`, `integer`, `number`, `text` or `list`. Decides the widget.
		std::string Kind;

		// The built-in value, spelled as a config file would spell it. Shown as
		// the placeholder, so an untouched field says what will happen anyway.
		std::string Default;

		// The one line `--flags` prints beside the name.
		std::string Description;
	};

	// Everything one program said about itself.
	struct Description {
		// The program's own name for itself, which is what it prints in `--help`.
		std::string Program;

		// The summary line, shown under the mode's heading.
		std::string Summary;

		// The engine version it was built from. **Shown, because a launcher
		// beside a stale staged tree is the failure this program could
		// otherwise hide**: two versions in one tree means somebody built one
		// target and not the others.
		std::string Version;

		// Every declared option, in declaration order.
		std::vector<DescribedOption> Options;

		// Every declared setting, in declaration order.
		std::vector<DescribedSetting> Settings;

		// The option by that name, or nothing.
		const DescribedOption *Option(std::string_view name) const;

		// The setting by that name, or nothing.
		const DescribedSetting *Setting(std::string_view name) const;
	};

	// Reads one `--describe` object.
	//
	// @param json    The text the program printed.
	// @param failure Filled in with why, when this returns nothing.
	// @return The description, or nothing when the text was not one.
	std::optional<Description> ParseDescription(const std::string &json, std::string &failure);

	// Runs `<program> --describe` and reads what it printed.
	//
	// **A program that fails to describe itself is reported rather than
	// skipped.** The likely causes are a half-built tree and a binary from
	// before v0.18, and both are things the person in front of the launcher can
	// fix once they are told.
	//
	// @param program The staged binary - `launcher::ProgramPath`.
	// @param failure Filled in with why, when this returns nothing.
	// @return What it said, or nothing.
	std::optional<Description> ReadDescription(const std::filesystem::path &program, std::string &failure);

	// Every program's description, read once and kept.
	//
	// Keyed by the program's staged name rather than its path, because that is
	// what a `Mode` names and what an error message should say.
	class Descriptions {
	  public:
		// Reads the ones that are present, and records why the others are not.
		//
		// @param stageRoot The staged tree.
		// @param programs  The program names to ask.
		void Load(const std::filesystem::path &stageRoot, const std::vector<std::string> &programs);

		// What that program said, or null when it was absent or refused.
		const Description *Find(std::string_view program) const;

		// Why that program has no description, or empty when it has one.
		std::string Failure(std::string_view program) const;

	  private:
		std::map<std::string, Description, std::less<>> Loaded;
		std::map<std::string, std::string, std::less<>> Failures;
	};
}
