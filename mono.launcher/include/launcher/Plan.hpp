#pragma once

// arch-waiver public-header: forward launcher API. `Launcher.hpp` owns forms
// and its caller-facing command-line construction uses these value types.

// The form a mode is filled in on, and the command line it turns into.
//
// **Everything here is a value, and none of it draws.** The whole of what the
// launcher decides - which options are on, what they hold, how forty of them
// are ordered into readable groups, and exactly what argv the child receives -
// is a pure function of a `Mode` and a `Description`, so a suite exercises it
// without a window, a GPU or a child process. `Interface.cpp` is the part that
// cannot be tested that way, and it is deliberately thin.
//
// **The command line is shown, always.** A launcher that hides what it ran is a
// launcher whose bug reports say "it did not work" - so `DisplayCommandLine`
// produces the line a person can paste into a terminal and get the identical
// run, and the screen shows it under the Launch button.
//
// @tier L13 · client
// @since v0.18

#include <cstdint>
#include <filesystem>
#include <launcher/Catalogue.hpp>
#include <launcher/Description.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace launcher {

	// One row of the form: an option, whether it is on, and what it holds.
	//
	// **Two rows may name the same option, and that is how a repeatable one is
	// answered.** `--cdn`, `--upstream` and `--world` are declared repeatable
	// and mean different things given twice; a form that could only hold one
	// value each would have quietly made those options half-usable.
	struct FieldState {
		// The option's name, without dashes.
		std::string Option;

		// Whether it reaches the command line at all.
		bool Enabled = false;

		// The value, for an option that takes one. Ignored for a bare flag,
		// which is on or absent.
		std::string Value;
	};

	// One row of the settings tab, emitted as `--flag NAME=VALUE`.
	struct SettingState {
		// The dotted name.
		std::string Name;

		// Whether it reaches the command line.
		bool Enabled = false;

		// The value, spelled as a config file would spell it.
		std::string Value;
	};

	// A mode's form, as it currently stands.
	struct Form {
		// The mode this belongs to.
		std::string ModeId;

		// One row per declared option, in the order `Ordering` puts them, plus
		// any extra rows added for a repeated option.
		std::vector<FieldState> Options;

		// One row per declared setting.
		std::vector<SettingState> Settings;
	};

	// A run of options that share a name prefix, for a readable form.
	//
	// **Because forty rows in one column is a wall nobody reads.** The prefix a
	// name already carries - `session-`, `content-`, `profile-` - is the
	// grouping the declarations were written with, so grouping by it costs no
	// new table and cannot disagree with the names.
	struct OptionGroup {
		// What the collapsing header says.
		std::string Title;

		// The option names in it, in declaration order.
		std::vector<std::string> Options;
	};

	// Which browse dialog an option's value wants, if any.
	enum class BrowseShape : uint8_t {
		// Type it, or paste it. Everything that is not a path.
		None,

		// One existing file.
		File,

		// A directory, or several of them for a repeatable option.
		Folder,
	};

	// Whether an option's value is a path somebody should be able to browse
	// for, and which of the two shapes it wants.
	//
	// **Read off the declared value name rather than a table here.** Every
	// program in the tree already spells a path option's value `PATH` or `DIR` -
	// that is `core::Arguments::Value`'s second argument and what `--help`
	// prints - so a browse button appears on a new path option the day it is
	// declared, with no change to this module.
	//
	// @param option The declared option.
	// @return What to offer beside its field.
	BrowseShape BrowseShapeOf(const DescribedOption &option);

	// Whether any of these options browses, which is what decides how wide the
	// actions column has to be for all of them.
	//
	// @param description What the program accepts.
	// @param names       The option names in one group. Names it does not
	//                    declare are skipped, the way a stale pin is.
	// @return `true` when at least one draws a browse button.
	bool AnyBrowses(const Description &description, const std::vector<std::string> &names);

	// Whether a setting is a two-state value.
	//
	// The declared kind and not the default's spelling: a `text` setting whose
	// default happens to read `false` is still a text setting. What the form
	// does with the answer - a checkbox rather than a field - is `Interface.cpp`'s.
	//
	// @param setting The declared setting.
	// @return `true` for the `boolean` kind and nothing else.
	bool IsBooleanSetting(const DescribedSetting &setting);

	// Options the form never shows, because the launcher answers them itself or
	// they end the program instead of running it.
	//
	// @param name The option's name.
	// @return `true` when it must not become a field.
	bool IsLauncherOwnedOption(std::string_view name);

	// A fresh form for one mode, with the mode's presets applied.
	//
	// @param mode        The mode being opened.
	// @param description What its program said it accepts.
	// @return The form, with every option off except the mode's presets.
	Form NewForm(const Mode &mode, const Description &description);

	// Every option, grouped by shared name prefix.
	//
	// **Every option, including the pinned ones.** Until v0.18 this excluded
	// them, because the only screen was one page and showing a row twice would
	// have been a row that could disagree with itself. The form is tabbed now -
	// Common is the pinned block and All options is this - so a tab called "all"
	// that quietly omitted seven of them would be the lie. Both tabs edit the
	// same `FieldState`, so there is still only one of each row.
	//
	// An option whose prefix is shared with nothing else lands in a final group
	// rather than a group of one.
	//
	// @param description What the program accepts.
	// @return The groups, prefix groups first and the leftovers last.
	std::vector<OptionGroup> GroupOptions(const Description &description);

	// The settings, grouped by the part of the name before the first dot.
	//
	// @param description What the program accepts.
	// @return The groups, in first-appearance order.
	std::vector<OptionGroup> GroupSettings(const Description &description);

	// Whether a name or its description matches a typed query.
	//
	// Case-insensitive substring, over both fields, because half of what
	// somebody is looking for they remember by what it *does* rather than by
	// what it is called.
	//
	// @param query       What was typed. Empty matches everything.
	// @param name        The option or setting name.
	// @param description Its one-line description.
	// @return `true` when the row should stay visible.
	bool Matches(std::string_view query, std::string_view name, std::string_view description);

	// Which of these option names the search leaves on screen, in the order
	// they were given.
	//
	// **One answer, used for both the rows and the count beside the tab.** They
	// were two loops until v0.19 and a tab that said `(3)` over two rows is the
	// failure mode that has: the count and the page have to be the same
	// question asked once.
	//
	// @param description What the program accepts.
	// @param names       The names to filter - a mode's pins, or a group.
	// @param query       What was typed. Empty keeps everything declared.
	// @return The names that survive. A name the program does not declare is
	//         dropped rather than shown empty, which is what lets a pin outlive
	//         the option it names.
	std::vector<std::string> MatchingOptions(
		const Description &description, const std::vector<std::string> &names, std::string_view query
	);

	// The same, over the declared settings.
	//
	// @param description What the program accepts.
	// @param names       The setting names to filter.
	// @param query       What was typed. Empty keeps everything declared.
	// @return The names that survive, in the order they were given.
	std::vector<std::string> MatchingSettings(
		const Description &description, const std::vector<std::string> &names, std::string_view query
	);

	// How many declared options the search matches, across every group.
	//
	// The ones the launcher answers itself are not counted, because they are
	// not on the form to be found - see `IsLauncherOwnedOption`.
	//
	// @param description What the program accepts.
	// @param query       What was typed. Empty counts every option shown.
	// @return The count.
	size_t OptionHits(const Description &description, std::string_view query);

	// How many declared settings the search matches.
	//
	// @param description What the program accepts.
	// @param query       What was typed. Empty counts every setting.
	// @return The count.
	size_t SettingHits(const Description &description, std::string_view query);

	// How many rows name this option.
	//
	// One is the ordinary answer. More than one means somebody used `+` or
	// picked several folders at once, and it is what decides whether a row
	// offers to remove itself - see `RemoveRow`.
	//
	// @param form   The form.
	// @param option The option's name.
	// @return The count.
	size_t RowsFor(const Form &form, std::string_view option);

	// Adds another row for the option at `row`, directly after it.
	//
	// @param form The form to change.
	// @param row  The row to duplicate. Out of range does nothing.
	// @return The index of the new row, or `row` when nothing was added.
	size_t AddRow(Form &form, size_t row);

	// Removes a row, unless it is the only one naming its option.
	//
	// **The last row stays, and is switched off instead.** Every row on the form
	// is generated from a declared option, so removing the last one would leave
	// the form unable to express an option the program has - and the person who
	// pressed the button would have no way back to it short of reopening the
	// mode and losing everything else they typed.
	//
	// @param form The form to change.
	// @param row  The row to remove. Out of range does nothing.
	// @return `true` when a row was actually removed.
	bool RemoveRow(Form &form, size_t row);

	// Puts several values into one option, adding rows for the extras.
	//
	// What a multi-folder pick lands in. The first value replaces `row` and each
	// one after it becomes a new row directly below, so three folders read down
	// the form in the order they were ticked.
	//
	// @param form   The form to change.
	// @param row    Where the first value goes. Out of range does nothing.
	// @param values The values. Empty does nothing - a confirmed dialog with no
	//               ticks must not blank a row somebody had already filled in.
	void SetRows(Form &form, size_t row, const std::vector<std::string> &values);

	// The argv a child receives, not including the program name.
	//
	// Enabled rows only, options before settings, in form order. A value option
	// becomes two entries - `--name` and the value - rather than `--name=value`,
	// so nothing has to be escaped for a parser that splits on the first `=`.
	//
	// @param form        The filled-in form.
	// @param description What the program accepts, which is what says whether a
	//                    row takes a value and which rows are real at all.
	// @return The arguments.
	std::vector<std::string> CommandLine(const Form &form, const Description &description);

	// The same command line, as a person would type it.
	//
	// Quoted for a POSIX shell on every platform. **A display string and not an
	// input**: nothing in this program ever parses it back, and nothing is ever
	// handed to a shell - `parallel::Process` takes the list from
	// `CommandLine`. This exists so a person can reproduce a run by hand and so
	// a bug report can carry it.
	//
	// @param program     The staged binary.
	// @param form        The filled-in form.
	// @param description What the program accepts. See `CommandLine`.
	// @return One line.
	std::string DisplayCommandLine(
		const std::filesystem::path &program, const Form &form, const Description &description
	);
}
