#include <engine/core/Arguments.hpp>
#include <engine/core/Config.hpp>
#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.core.flags")

using Catch::Approx;
using engine::core::Arguments;
using engine::core::Config;
using engine::core::ConfigReport;
using engine::core::Flag;
using engine::core::FlagDescription;
using engine::core::FlagKind;
using engine::core::Flags;
using engine::core::FlagSource;
using engine::core::FlagStatus;

namespace {
	// One table, used by every case, so the cases test the store rather than a
	// per-case vocabulary.
	constexpr std::array<FlagDescription, 4> TABLE{{
		{"test.switch", FlagKind::Boolean, "false", "A boolean"},
		{"test.count", FlagKind::Integer, "7", "A whole number"},
		{"test.rate", FlagKind::Number, "1.5", "A real number"},
		{"test.label", FlagKind::Text, "plain", "Some text"},
	}};

	// Every case starts from an empty table, because the store is process-wide
	// and a leftover freeze would make the next case pass for the wrong reason.
	void Fresh() {
		Flags::Reset();
		REQUIRE(Flags::Declare(TABLE));
	}

	// A command line, built the way a program's `main` gets one.
	class Line {
	  public:
		explicit Line(std::vector<std::string> words) {
			Words.emplace_back("program");
			for (std::string &word : words) {
				Words.push_back(std::move(word));
			}
			Pointers.reserve(Words.size());
			for (std::string &word : Words) {
				Pointers.push_back(word.data());
			}
		}

		int Count() const {
			return static_cast<int>(Pointers.size());
		}

		char **Argv() {
			return Pointers.data();
		}

	  private:
		std::vector<std::string> Words;
		std::vector<char *> Pointers;
	};

	// The two spellings of "put this in the environment", because there is no
	// portable one and this suite needs both platforms to run the case rather
	// than one of them to skip it.
	void SetVariable(const char *name, const char *value) {
#ifdef _WIN32
		REQUIRE(_putenv_s(name, value) == 0);
#else
		REQUIRE(setenv(name, value, 1) == 0);
#endif
	}

	void ClearVariable(const char *name) {
#ifdef _WIN32
		_putenv_s(name, "");
#else
		unsetenv(name);
#endif
	}

	std::filesystem::path Scratch(const std::string &name, const std::string &contents) {
		const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::ofstream file(path, std::ios::trunc);
		file << contents;
		file.close();
		return path;
	}
}

TEST_CASE("a declared flag reads its default until something says otherwise", "[flags]") {
	Fresh();

	CHECK_FALSE(Flag("test.switch").Boolean());
	CHECK(Flag("test.count").Integer() == 7);
	CHECK(Flag("test.rate").Number() == Approx(1.5));
	CHECK(Flag("test.label").Text() == "plain");
	CHECK(Flag("test.switch").Source() == FlagSource::Default);
}

TEST_CASE("a name nothing declares is a dead handle rather than a crash", "[flags]") {
	Fresh();

	const Flag missing("test.nothing");
	CHECK_FALSE(missing.IsValid());
	CHECK_FALSE(missing.Boolean());
	CHECK(missing.Integer() == 0);
	CHECK(missing.Text().empty());

	// And setting one says so, which is what a config file's key check is.
	CHECK(Flags::Set("test.nothing", "true", FlagSource::ConfigFile) == FlagStatus::NoSuchFlag);
}

TEST_CASE("declaring one name twice is refused and named", "[flags]") {
	Fresh();
	CHECK_FALSE(Flags::Declare(TABLE));
}

TEST_CASE("every kind reads what a person would write", "[flags]") {
	Fresh();

	for (const char *spelling : {"true", "on", "yes", "1", "TRUE", "On"}) {
		Flags::Reset();
		REQUIRE(Flags::Declare(TABLE));
		REQUIRE(Flags::Set("test.switch", spelling, FlagSource::ConfigFile) == FlagStatus::Applied);
		CHECK(Flag("test.switch").Boolean());
	}

	for (const char *spelling : {"false", "off", "no", "0"}) {
		Flags::Reset();
		REQUIRE(Flags::Declare(TABLE));
		REQUIRE(Flags::Set("test.switch", spelling, FlagSource::ConfigFile) == FlagStatus::Applied);
		CHECK_FALSE(Flag("test.switch").Boolean());
	}

	Fresh();

	// A bare flag with no value is `true`, which is what a command line hands
	// over — and is not a value at all for the other three kinds.
	CHECK(Flags::Set("test.switch", "", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(Flag("test.switch").Boolean());
	CHECK(Flags::Set("test.count", "", FlagSource::CommandLine) == FlagStatus::NotAValue);

	// **The whole string or nothing.** A number that stopped parsing halfway
	// would take `30fps` as thirty and say nothing about the three characters
	// it ignored.
	CHECK(Flags::Set("test.count", "30fps", FlagSource::CommandLine) == FlagStatus::NotAValue);
	CHECK(Flags::Set("test.rate", "1.5s", FlagSource::CommandLine) == FlagStatus::NotAValue);
	CHECK(Flags::Set("test.switch", "maybe", FlagSource::CommandLine) == FlagStatus::NotAValue);
	CHECK(Flag("test.count").Integer() == 7);

	// Text takes anything, including a spelling that would be a fault anywhere
	// else.
	CHECK(Flags::Set("test.label", "30fps", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(Flag("test.label").Text() == "30fps");

	// An integer answers `Number` and a number answers `Integer`, so a caller
	// wanting a double does not have to test the kind.
	CHECK(Flag("test.count").Number() == Approx(7.0));
	Flags::Set("test.rate", "2.75", FlagSource::CommandLine);
	CHECK(Flag("test.rate").Integer() == 2);
}

TEST_CASE("precedence is a property of the source and not of the call order", "[flags]") {
	Fresh();

	REQUIRE(Flags::Set("test.count", "3", FlagSource::CommandLine) == FlagStatus::Applied);

	// Applied afterwards and still refused, which is the whole point: a program
	// may read its sources in whichever order suits it.
	CHECK(Flags::Set("test.count", "4", FlagSource::ConfigFile) == FlagStatus::Outranked);
	CHECK(Flags::Set("test.count", "5", FlagSource::Environment) == FlagStatus::Outranked);
	CHECK(Flag("test.count").Integer() == 3);
	CHECK(Flag("test.count").Source() == FlagSource::CommandLine);

	// The same source twice is one person saying it twice, and the second is
	// what they meant.
	CHECK(Flags::Set("test.count", "6", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(Flag("test.count").Integer() == 6);

	// And a whole file read after a command line reports what it could not do,
	// which is the counter's reason for existing: "my config file does nothing"
	// is otherwise a mystery.
	const std::filesystem::path path = Scratch(
		"atomic-flags-late.cfg",
		"[test]\n"
		"count = 1\n"
		"label = late\n"
	);
	const ConfigReport late = Config::ApplyFile(path);
	CHECK(late.Ok);
	CHECK(late.Applied == 1);
	CHECK(late.Outranked == 1);
	CHECK(Flag("test.count").Integer() == 6);
	CHECK(Flag("test.label").Text() == "late");

	std::filesystem::remove(path);
}

TEST_CASE("a frozen flag refuses every later set", "[flags]") {
	Fresh();

	REQUIRE(Flags::Set("test.count", "3", FlagSource::ConfigFile) == FlagStatus::Applied);
	Flags::Freeze();
	CHECK(Flags::Frozen());

	CHECK(Flags::Set("test.count", "9", FlagSource::CommandLine) == FlagStatus::Frozen);
	CHECK(Flag("test.count").Integer() == 3);
}

TEST_CASE("a handle resolved before a reset does not answer the wrong flag after one", "[flags]") {
	// The failure this guards is a test's and not a program's — nothing else
	// resets — and it is the kind that reads as the code under test being
	// wrong, because a stale index answers a *plausible* value from the wrong
	// row.
	Fresh();

	const Flag count("test.count");
	REQUIRE(count.Integer() == 7);

	Flags::Reset();
	constexpr std::array<FlagDescription, 2> REORDERED{{
		{"test.label", FlagKind::Text, "second", "Some text"},
		{"test.count", FlagKind::Integer, "99", "A whole number"},
	}};
	REQUIRE(Flags::Declare(REORDERED));

	CHECK(count.Integer() == 99);
}

TEST_CASE("the listing names every flag, its value and where the value came from", "[flags]") {
	Fresh();
	REQUIRE(Flags::Set("test.count", "42", FlagSource::Environment) == FlagStatus::Applied);

	const std::string listing = Flags::Listing();
	CHECK(listing.find("test.count") != std::string::npos);
	CHECK(listing.find("42") != std::string::npos);
	CHECK(listing.find("environment") != std::string::npos);
	CHECK(listing.find("A whole number") != std::string::npos);

	// Every declared flag has a line, including the ones nobody set.
	CHECK(listing.find("test.label") != std::string::npos);
	CHECK(listing.find("(default)") != std::string::npos);
}

TEST_CASE("a config file's section is the flag name's prefix", "[flags][config]") {
	CHECK(Config::FlagNameOf("content", "gif") == "content.gif");
	CHECK(Config::FlagNameOf("", "gif") == "gif");
	CHECK(Config::FlagNameOf("Content", "GIF") == "content.gif");
}

TEST_CASE("a config file sets what it names and reports what it cannot", "[flags][config]") {
	Fresh();

	const std::filesystem::path path = Scratch(
		"atomic-flags-good.cfg",
		"# a comment\n"
		"\n"
		"[test]\n"
		"switch = true    ; a trailing comment\n"
		"count = 12\n"
		"label = \"a value # with a hash\"\n"
	);

	const ConfigReport report = Config::ApplyFile(path);
	CHECK(report.Ok);
	CHECK(report.Applied == 3);
	CHECK(Flag("test.switch").Boolean());
	CHECK(Flag("test.count").Integer() == 12);
	CHECK(Flag("test.label").Text() == "a value # with a hash");
	CHECK(Flag("test.count").Source() == FlagSource::ConfigFile);

	std::filesystem::remove(path);
}

TEST_CASE("a config key naming no flag is an error rather than silence", "[flags][config]") {
	Fresh();

	const std::filesystem::path path = Scratch(
		"atomic-flags-typo.cfg",
		"[test]\n"
		"swich = true\n"
	);

	const ConfigReport report = Config::ApplyFile(path);
	CHECK_FALSE(report.Ok);
	CHECK(report.Error.find("test.swich") != std::string::npos);
	CHECK(report.Error.find(":2") != std::string::npos);

	std::filesystem::remove(path);
}

TEST_CASE("a missing config file is ordinary and a named missing one is not", "[flags][config]") {
	Fresh();

	// Unnamed and absent: every program that was never given one.
	CHECK(Config::ApplyFile(std::filesystem::temp_directory_path() / "atomic-flags-absent.cfg").Ok);

	Arguments arguments("test", "");
	Config::DeclareOptions(arguments);
	Line line({"--config", "/nonexistent/atomic.cfg"});
	REQUIRE(arguments.Parse(line.Count(), line.Argv()).Ok);

	const ConfigReport report = Config::Apply(arguments);
	CHECK_FALSE(report.Ok);
	CHECK(report.Error.find("no such file") != std::string::npos);

	// **Frozen even so.** A program that carries on after a bad config file
	// must not also be one whose flags can still move.
	CHECK(Flags::Frozen());
}

TEST_CASE("--flag sets one setting above everything else", "[flags][config]") {
	Fresh();

	const std::filesystem::path path = Scratch(
		"atomic-flags-beaten.cfg",
		"[test]\n"
		"count = 3\n"
		"label = from-file\n"
	);

	Arguments arguments("test", "");
	Config::DeclareOptions(arguments);
	Line line({"--config", path.string(), "--flag", "test.count=8", "--flag", "test.switch"});
	REQUIRE(arguments.Parse(line.Count(), line.Argv()).Ok);

	const ConfigReport report = Config::Apply(arguments);
	CHECK(report.Ok);

	// The command line wins where they disagree and the file still lands where
	// they do not.
	CHECK(Flag("test.count").Integer() == 8);
	CHECK(Flag("test.label").Text() == "from-file");

	// A bare `--flag NAME` is `true`.
	CHECK(Flag("test.switch").Boolean());

	// **Nothing was outranked, and that is `Apply`'s order rather than luck.**
	// It reads the sources lowest-first, so every later one simply wins; the
	// counter is what a caller applying a second file, or applying them in
	// another order, sees. The guarantee being sold is that the outcome is the
	// same either way, which is what the precedence case above pins.
	CHECK(report.Outranked == 0);
	CHECK(Flags::Frozen());

	std::filesystem::remove(path);
}

TEST_CASE("the environment is read against the declared table", "[flags][config]") {
	Fresh();

	// The mapping is only a function in this direction: `test.count` and
	// `test-count` would both produce `ATOMIC_TEST_COUNT`, so the adapter walks
	// the flags and asks for each rather than parsing what it finds.
	SetVariable("ATOMIC_TEST_COUNT", "21");
	SetVariable("ATOMIC_TEST_LABEL", "from-environment");

	const ConfigReport report = Config::ApplyEnvironment();
	CHECK(report.Ok);
	CHECK(report.Applied == 2);
	CHECK(Flag("test.count").Integer() == 21);
	CHECK(Flag("test.label").Text() == "from-environment");
	CHECK(Flag("test.count").Source() == FlagSource::Environment);

	ClearVariable("ATOMIC_TEST_COUNT");
	ClearVariable("ATOMIC_TEST_LABEL");
}

TEST_CASE("a list appends within a source and is replaced by one that outranks it", "[flags]") {
	// **The rule a scalar cannot express.** Three origins in a config file are
	// three lines and all three are meant; one `--flag` on a command line
	// replaces all three, rather than being appended to something the person
	// running it cannot see.
	Flags::Reset();
	engine::core::FlagTableBuilder built;
	built.List("test.sources", "An origin. Repeat for more");
	REQUIRE(Flags::Declare(built.Rows()));

	const Flag sources("test.sources");
	CHECK(sources.Items().empty());

	REQUIRE(Flags::Set("test.sources", "dir:/one", FlagSource::ConfigFile) == FlagStatus::Applied);
	REQUIRE(Flags::Set("test.sources", "dir:/two", FlagSource::ConfigFile) == FlagStatus::Applied);
	REQUIRE(sources.Items().size() == 2);
	CHECK(sources.Items()[0] == "dir:/one");
	CHECK(sources.Items()[1] == "dir:/two");

	// **Order is kept**, because these are priority orders — the first origin
	// that answers wins — and a set would lose the only thing they carry.
	CHECK(sources.Text() == "dir:/one, dir:/two");

	REQUIRE(Flags::Set("test.sources", "host:9080", FlagSource::CommandLine) == FlagStatus::Applied);
	REQUIRE(sources.Items().size() == 1);
	CHECK(sources.Items()[0] == "host:9080");

	// And the same source again appends to what it just replaced.
	REQUIRE(Flags::Set("test.sources", "host:9081", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(sources.Items().size() == 2);

	// A lower source is refused outright rather than appending underneath.
	CHECK(Flags::Set("test.sources", "dir:/three", FlagSource::ConfigFile) == FlagStatus::Outranked);
	CHECK(sources.Items().size() == 2);
}

TEST_CASE("an empty value is how a list is emptied", "[flags]") {
	Flags::Reset();
	engine::core::FlagTableBuilder built;
	built.List("test.sources", "An origin. Repeat for more");
	REQUIRE(Flags::Declare(built.Rows()));

	REQUIRE(Flags::Set("test.sources", "dir:/one", FlagSource::ConfigFile) == FlagStatus::Applied);
	REQUIRE(Flag("test.sources").Items().size() == 1);

	// **`--flag test.sources=` says "none".** An append of nothing would be
	// indistinguishable from not writing the line at all, so this is the only
	// way a command line can take back what a file named.
	REQUIRE(Flags::Set("test.sources", "", FlagSource::CommandLine) == FlagStatus::Applied);
	CHECK(Flag("test.sources").Items().empty());
	CHECK(Flag("test.sources").Text().empty());
}

TEST_CASE("a repeated key in a config file is a list and not a last-one-wins", "[flags][config]") {
	Flags::Reset();
	engine::core::FlagTableBuilder built;
	built.List("test.sources", "An origin. Repeat for more");
	built.Text("test.label", "plain", "Some text");
	REQUIRE(Flags::Declare(built.Rows()));

	const std::filesystem::path path = Scratch(
		"atomic-flags-list.cfg",
		"[test]\n"
		"sources = dir:/one\n"
		"sources = \"dir:/with, a comma\"\n"
		"sources = host:9080\n"
		"label = first\n"
		"label = second\n"
	);

	const ConfigReport report = Config::ApplyFile(path);
	CHECK(report.Ok);

	const Flag sources("test.sources");
	REQUIRE(sources.Items().size() == 3);
	CHECK(sources.Items()[0] == "dir:/one");

	// **The whole point of not splitting on a separator.** A path somebody chose
	// may contain anything, and a list that split its entries would make that a
	// bug nobody could see — which is exactly why the key repeats instead.
	CHECK(sources.Items()[1] == "dir:/with, a comma");
	CHECK(sources.Items()[2] == "host:9080");

	// A repeated *scalar* is still last-one-wins, which is what a scalar means.
	CHECK(Flag("test.label").Text() == "second");

	std::filesystem::remove(path);
}
