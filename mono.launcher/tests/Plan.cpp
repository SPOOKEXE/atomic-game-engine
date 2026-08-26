// The form, the grouping and the command line.
//
// **This is the half of the launcher that decides anything.** A form generated
// from another program's `--describe` is a form nobody can read the source of
// and predict, so what it produces has to be assertable: which rows exist, how
// forty of them group, and exactly what argv reaches the child.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iterator>
#include <launcher/Plan.hpp>
#include <set>

TEST_SUITE_ID("launcher.plan")

using namespace launcher;

namespace {
	DescribedOption Flag(std::string name) {
		return DescribedOption{
			.Name = std::move(name), .TakesValue = false, .ValueName = {}, .Description = "a flag"
		};
	}

	DescribedOption Value(std::string name, std::string valueName) {
		return DescribedOption{
			.Name = std::move(name),
			.TakesValue = true,
			.ValueName = std::move(valueName),
			.Description = "takes a value",
		};
	}

	// A description shaped like the client's: the three the launcher owns, a
	// couple of pinnable ones, and a run of options sharing a prefix.
	Description Client() {
		Description description;
		description.Program = "client";
		description.Version = "0.18.0";
		description.Options = {
			Flag("help"),
			Flag("version"),
			Flag("describe"),
			Value("flag", "NAME=VALUE"),
			Flag("flags"),
			Value("game", "PATH"),
			Value("connect", "HOST:PORT"),
			Flag("browse"),
			Value("session-name", "NAME"),
			Value("session-id", "HEX"),
			Value("session-key", "SECRET"),
			Value("cdn", "HOST:PORT"),
			Value("width", "PX"),
			Value("content-cache", "DIR"),
		};
		description.Settings = {
			DescribedSetting{
				.Name = "content.gif", .Kind = "boolean", .Default = "false", .Description = "GIFs"
			},
			DescribedSetting{
				.Name = "content.svg", .Kind = "boolean", .Default = "true", .Description = "SVGs"
			},
			DescribedSetting{
				.Name = "engine.serial-compute",
				.Kind = "boolean",
				.Default = "false",
				.Description = "serial"
			},
		};
		return description;
	}

	Mode Join() {
		return Mode{
			.Id = "join",
			.Label = "Join",
			.Blurb = {},
			.Program = "client",
			.Pinned = {"browse", "connect"},
			.Presets = {ModePreset{.Option = "browse", .Value = {}}},
			.After = Lifetime::HandOver,
		};
	}

	const FieldState *Row(const Form &form, std::string_view option) {
		for (const FieldState &field : form.Options) {
			if (field.Option == option) {
				return &field;
			}
		}
		return nullptr;
	}

	bool HasGroup(const std::vector<OptionGroup> &groups, std::string_view title) {
		return std::any_of(groups.begin(), groups.end(), [&](const OptionGroup &group) {
			return group.Title == title;
		});
	}
}

TEST_CASE("the launcher's own options never become fields", "[launcher]") {
	// `--help`, `--version`, `--describe` and `--flags` print and exit, so a row
	// for one is a Launch button that starts nothing. `--flag` is how the
	// settings tab is emitted, and a second hand-typed one would be two places
	// writing the same argument with no rule about which wins.
	CHECK(IsLauncherOwnedOption("help"));
	CHECK(IsLauncherOwnedOption("version"));
	CHECK(IsLauncherOwnedOption("describe"));
	CHECK(IsLauncherOwnedOption("flags"));
	CHECK(IsLauncherOwnedOption("flag"));
	CHECK_FALSE(IsLauncherOwnedOption("game"));

	// `--config PATH` stays: naming a settings file is an ordinary thing to
	// want, and nothing here writes one.
	CHECK_FALSE(IsLauncherOwnedOption("config"));

	const Form form = NewForm(Join(), Client());
	CHECK(Row(form, "help") == nullptr);
	CHECK(Row(form, "flag") == nullptr);
	CHECK(Row(form, "game") != nullptr);
}

TEST_CASE("a mode's presets arrive switched on", "[launcher]") {
	const Form form = NewForm(Join(), Client());

	REQUIRE(Row(form, "browse") != nullptr);
	CHECK(Row(form, "browse")->Enabled);

	// Everything else is off. A launcher that quietly passed an option nobody
	// asked for is a launcher whose displayed command line is a lie.
	REQUIRE(Row(form, "connect") != nullptr);
	CHECK_FALSE(Row(form, "connect")->Enabled);
}

TEST_CASE("a preset naming an option the program lacks is ignored", "[launcher]") {
	Mode mode = Join();
	mode.Presets.push_back(ModePreset{.Option = "no-such-option", .Value = "1"});

	// The catalogue is written against the programs in this tree; a launcher
	// run beside an older staged client should offer the options that client
	// has rather than refuse the mode outright.
	const Form form = NewForm(mode, Client());
	CHECK(Row(form, "no-such-option") == nullptr);
	CHECK(Row(form, "browse")->Enabled);
}

TEST_CASE("settings start at their declared defaults and switched off", "[launcher]") {
	const Form form = NewForm(Join(), Client());

	REQUIRE(form.Settings.size() == 3);
	CHECK(form.Settings[0].Name == "content.gif");
	CHECK(form.Settings[0].Value == "false");
	CHECK_FALSE(form.Settings[0].Enabled);
}

TEST_CASE("options group by the prefix their names already carry", "[launcher]") {
	const auto groups = GroupOptions(Client());

	// Three options share `session-`, so they get a header of their own.
	CHECK(HasGroup(groups, "session"));

	// **A prefix with one user is not a header.** `width` and `game` share
	// nothing, and a collapsing header containing a single row is worse than
	// the row on its own.
	CHECK_FALSE(HasGroup(groups, "width"));
	CHECK(HasGroup(groups, "Everything else"));

	// And that group is last, because it is the one with no theme.
	CHECK(groups.back().Title == "Everything else");
}

TEST_CASE("the all-options grouping holds every option, pinned included", "[launcher]") {
	const Description description = Client();
	const auto groups = GroupOptions(description);

	// The tab is called "All options", so it shows all of them. Both tabs edit
	// the same `FieldState`, so a row appearing on each is still one row.
	std::set<std::string> grouped;
	for (const OptionGroup &group : groups) {
		grouped.insert(group.Options.begin(), group.Options.end());
	}

	for (const DescribedOption &option : description.Options) {
		if (IsLauncherOwnedOption(option.Name)) {
			// `--help` and friends never become a row anywhere.
			CHECK(grouped.count(option.Name) == 0);
			continue;
		}
		CHECK(grouped.count(option.Name) == 1);
	}

	// Including the ones Join pins, which the pre-v0.18 grouping left out.
	CHECK(grouped.count("browse") == 1);
	CHECK(grouped.count("connect") == 1);
}

TEST_CASE("plus adds a row for the same option, switched on", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);

	const size_t game = std::distance(
		form.Options.begin(),
		std::find_if(form.Options.begin(), form.Options.end(), [](const FieldState &field) {
			return field.Option == "game";
		})
	);

	REQUIRE(RowsFor(form, "game") == 1);
	const size_t added = AddRow(form, game);

	CHECK(added == game + 1);
	CHECK(RowsFor(form, "game") == 2);

	// On, because the reason to press `+` is to give the option another value.
	// An added row that arrived off is an empty row somebody has to notice.
	CHECK(form.Options[added].Enabled);
	CHECK(form.Options[added].Option == "game");
}

TEST_CASE("minus removes a duplicate row and clears the last one", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);

	const size_t game = std::distance(
		form.Options.begin(),
		std::find_if(form.Options.begin(), form.Options.end(), [](const FieldState &field) {
			return field.Option == "game";
		})
	);
	form.Options[game].Enabled = true;
	form.Options[game].Value = "one";

	AddRow(form, game);
	REQUIRE(RowsFor(form, "game") == 2);

	CHECK(RemoveRow(form, game + 1));
	CHECK(RowsFor(form, "game") == 1);

	// **The last row stays and is cleared instead.** Every row is generated
	// from a declared option, so removing the last one would leave the form
	// unable to express an option the program has - with no way back short of
	// reopening the mode and losing everything else.
	CHECK_FALSE(RemoveRow(form, game));
	CHECK(RowsFor(form, "game") == 1);
	CHECK_FALSE(form.Options[game].Enabled);
	CHECK(form.Options[game].Value.empty());
}

TEST_CASE("several folders fill one row each, in the order picked", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);

	const size_t cdn = std::distance(
		form.Options.begin(),
		std::find_if(form.Options.begin(), form.Options.end(), [](const FieldState &field) {
			return field.Option == "cdn";
		})
	);

	SetRows(form, cdn, {"/a", "/b", "/c"});

	REQUIRE(RowsFor(form, "cdn") == 3);
	CHECK(form.Options[cdn].Value == "/a");
	CHECK(form.Options[cdn + 1].Value == "/b");
	CHECK(form.Options[cdn + 2].Value == "/c");
	CHECK(form.Options[cdn + 2].Enabled);

	// And they reach the command line in that order, as three pairs. Read by
	// position from the first `--cdn` rather than from the start, because the
	// mode's own `--browse` preset is on the line too.
	const auto line = CommandLine(form, description);
	const auto first = std::find(line.begin(), line.end(), "--cdn");

	REQUIRE(std::distance(first, line.end()) == 6);
	CHECK(*(first + 1) == "/a");
	CHECK(*(first + 2) == "--cdn");
	CHECK(*(first + 3) == "/b");
	CHECK(*(first + 4) == "--cdn");
	CHECK(*(first + 5) == "/c");
}

TEST_CASE("a cancelled multi-pick does not blank the row it came from", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);

	const size_t cdn = std::distance(
		form.Options.begin(),
		std::find_if(form.Options.begin(), form.Options.end(), [](const FieldState &field) {
			return field.Option == "cdn";
		})
	);
	form.Options[cdn].Enabled = true;
	form.Options[cdn].Value = "already typed";

	// An empty list is a confirmed dialog with nothing ticked. Wiping the row
	// would throw away a value somebody had typed by hand.
	SetRows(form, cdn, {});
	CHECK(form.Options[cdn].Value == "already typed");
	CHECK(RowsFor(form, "cdn") == 1);
}

TEST_CASE("row operations on an index that is not there do nothing", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);
	const size_t before = form.Options.size();

	CHECK(AddRow(form, before + 10) == before + 10);
	CHECK_FALSE(RemoveRow(form, before + 10));
	SetRows(form, before + 10, {"/a"});

	CHECK(form.Options.size() == before);
}

TEST_CASE("settings group by the part before the first dot", "[launcher]") {
	const auto groups = GroupSettings(Client());

	REQUIRE(groups.size() == 2);
	CHECK(groups[0].Title == "content");
	CHECK(groups[0].Options.size() == 2);
	CHECK(groups[1].Title == "engine");
}

TEST_CASE("search matches a name or what it does", "[launcher]") {
	CHECK(Matches("", "anything", "at all"));
	CHECK(Matches("SESS", "session-name", "what to call it"));

	// Half of what somebody is looking for they remember by what it does.
	CHECK(Matches("call", "session-name", "What to call this session"));
	CHECK_FALSE(Matches("physics", "session-name", "What to call this session"));
}

TEST_CASE("a browse button is offered off the declared value name", "[launcher]") {
	const Description description = Client();

	// `PATH` and `DIR` are what every program in the tree already spells a path
	// option's value, so a browse button arrives with the declaration rather
	// than with a table here.
	REQUIRE(description.Option("game") != nullptr);
	CHECK(BrowseShapeOf(*description.Option("game")) == BrowseShape::File);
	REQUIRE(description.Option("content-cache") != nullptr);
	CHECK(BrowseShapeOf(*description.Option("content-cache")) == BrowseShape::Folder);

	// Everything else is typed. `HOST:PORT` is not a path and a file dialog
	// over it would offer to fill it in with something the child refuses.
	CHECK(BrowseShapeOf(*description.Option("connect")) == BrowseShape::None);

	// A bare flag has no value to browse for, whatever its value name says.
	CHECK(BrowseShapeOf(*description.Option("browse")) == BrowseShape::None);
}

TEST_CASE("a group knows whether any of it browses", "[launcher]") {
	const Description description = Client();

	// This is what sizes the actions column for a whole group, so it has to be
	// the same question the row asks - one `PATH` in the group widens all of
	// them, and a group with none keeps the narrow column.
	CHECK(AnyBrowses(description, {"connect", "game"}));
	CHECK(AnyBrowses(description, {"content-cache"}));
	CHECK_FALSE(AnyBrowses(description, {"connect", "width", "browse"}));

	// A name the program does not declare is skipped rather than counted, the
	// way a stale pin is.
	CHECK_FALSE(AnyBrowses(description, {"no-such-option"}));
}

TEST_CASE("a boolean setting is the declared kind, not the default's spelling", "[launcher]") {
	const Description description = Client();

	REQUIRE(description.Setting("content.gif") != nullptr);
	CHECK(IsBooleanSetting(*description.Setting("content.gif")));

	// A text setting whose default happens to read like a boolean is still a
	// text setting, and a checkbox over it would throw away every other value
	// it can hold.
	const DescribedSetting text{
		.Name = "session.mode", .Kind = "text", .Default = "false", .Description = "how to run"
	};
	CHECK_FALSE(IsBooleanSetting(text));
}

TEST_CASE("the search filters a name list to what stays on screen", "[launcher]") {
	const Description description = Client();
	const std::vector<std::string> pinned = Join().Pinned;

	// Empty query is every declared name, in the order they were given - the
	// pinned list is a reading order and filtering must not reorder it.
	CHECK(MatchingOptions(description, pinned, "") == pinned);

	CHECK(MatchingOptions(description, pinned, "connect") == std::vector<std::string>{"connect"});
	CHECK(MatchingOptions(description, pinned, "physics").empty());

	// A pinned name the program no longer declares is silently nothing, which
	// is what lets this launcher work beside an older staged tree.
	CHECK(MatchingOptions(description, {"no-such-option", "game"}, "") == std::vector<std::string>{"game"});
}

TEST_CASE("the settings search filters the same way", "[launcher]") {
	const Description description = Client();
	const std::vector<std::string> group = {"content.gif", "content.svg"};

	CHECK(MatchingSettings(description, group, "") == group);
	CHECK(MatchingSettings(description, group, "svg") == std::vector<std::string>{"content.svg"});
	CHECK(MatchingSettings(description, {"no-such-setting"}, "").empty());
}

TEST_CASE("a tab's count is the number of rows that tab draws", "[launcher]") {
	const Description description = Client();

	// **One function behind both**, because a tab reading `(3)` over two rows
	// is what two loops asking the same question drift into. Until v0.19 the
	// counts and the rows were separate loops in `Interface.cpp`.
	size_t declared = 0;
	for (const DescribedOption &option : description.Options) {
		declared += IsLauncherOwnedOption(option.Name) ? 0 : 1;
	}
	CHECK(OptionHits(description, "") == declared);

	size_t grouped = 0;
	for (const OptionGroup &group : GroupOptions(description)) {
		grouped += MatchingOptions(description, group.Options, "session").size();
	}
	CHECK(OptionHits(description, "session") == grouped);
	CHECK(OptionHits(description, "session") == 3);

	// `--help` and friends are not on the form, so a search that matches one
	// must not claim a hit nobody can scroll to.
	CHECK(OptionHits(description, "help") == 0);

	CHECK(SettingHits(description, "") == description.Settings.size());
	CHECK(SettingHits(description, "content.") == 2);
	CHECK(SettingHits(description, "physics") == 0);
}

TEST_CASE("only enabled rows reach the command line", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);

	for (FieldState &field : form.Options) {
		if (field.Option == "connect") {
			field.Enabled = true;
			field.Value = "10.0.0.4:7777";
		}
	}

	const auto line = CommandLine(form, description);

	// A value option is two entries and not `--name=value`: both parse, and
	// this one has no character in it that a value could also contain.
	//
	// **Declaration order, not the order somebody switched them on in.** The
	// form is built from the description and the description is the program's
	// own table, so the line a launcher shows reads in the same order as that
	// program's `--help` - and two people filling in the same form in different
	// orders get the same command line.
	REQUIRE(line.size() == 3);
	CHECK(line[0] == "--connect");
	CHECK(line[1] == "10.0.0.4:7777");
	CHECK(line[2] == "--browse");
}

TEST_CASE("a repeated option is two rows and two pairs", "[launcher]") {
	const Description description = Client();
	Form form;
	form.Options = {
		FieldState{.Option = "session-name", .Enabled = true, .Value = "one"},
		FieldState{.Option = "session-name", .Enabled = true, .Value = "two"},
	};

	// `--cdn`, `--upstream` and `--world` are declared repeatable and mean
	// different things given twice. A form that could hold only one value each
	// would have made those options half-usable and said nothing about it.
	const auto line = CommandLine(form, description);
	REQUIRE(line.size() == 4);
	CHECK(line[1] == "one");
	CHECK(line[3] == "two");
}

TEST_CASE("settings are emitted as the source that outranks the others", "[launcher]") {
	const Description description = Client();
	Form form = NewForm(Join(), description);
	form.Options.clear();
	form.Settings[0].Enabled = true;
	form.Settings[0].Value = "true";

	// `--flag` is the command-line source, which beats a config file and the
	// environment. It is the only one a launcher can be sure of.
	const auto line = CommandLine(form, description);
	REQUIRE(line.size() == 2);
	CHECK(line[0] == "--flag");
	CHECK(line[1] == "content.gif=true");
}

TEST_CASE("an option the program never declared is dropped", "[launcher]") {
	const Description description = Client();
	Form form;
	form.Options = {FieldState{.Option = "invented", .Enabled = true, .Value = "x"}};

	// `core::Arguments` treats an unknown option as an error rather than as
	// silence, so passing this on would turn a launcher that starts what it
	// understood into one that can start nothing at all.
	CHECK(CommandLine(form, description).empty());
}

TEST_CASE("the displayed line is one a person can paste back", "[launcher]") {
	const Description description = Client();
	Form form;
	form.Options = {FieldState{.Option = "game", .Enabled = true, .Value = "/games/my game.agame"}};

	const std::string line = DisplayCommandLine("/stage/client/client", form, description);

	// Quoted, because a path with a space in it is ordinary and a line somebody
	// pastes has to reproduce the run rather than half of it.
	CHECK(line.find("'/games/my game.agame'") != std::string::npos);
	CHECK(line.rfind("/stage/client/client", 0) == 0);
}

TEST_CASE("a value holding a quote survives being displayed", "[launcher]") {
	const Description description = Client();
	Form form;
	form.Options = {FieldState{.Option = "session-name", .Enabled = true, .Value = "declan's game"}};

	// The one character single quotes cannot contain. Getting this wrong
	// produces a line that looks right and pastes into something else.
	const std::string line = DisplayCommandLine("/stage/client/client", form, description);
	CHECK(line.find(R"('declan'\''s game')") != std::string::npos);
}
