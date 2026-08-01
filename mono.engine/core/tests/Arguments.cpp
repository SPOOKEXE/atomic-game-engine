#include <engine/core/Arguments.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.core.arguments")

using engine::core::Arguments;

namespace {
	// Parse takes argv as the OS hands it over, including argv[0].
	struct CommandLine {
		std::vector<std::string> Storage;
		std::vector<char *> Pointers;

		explicit CommandLine(std::initializer_list<const char *> words) {
			Storage.emplace_back("client");
			for (const char *word : words) {
				Storage.emplace_back(word);
			}
			for (auto &word : Storage) {
				Pointers.push_back(word.data());
			}
		}

		int Count() const {
			return static_cast<int>(Pointers.size());
		}
		char **Values() {
			return Pointers.data();
		}
	};

	Arguments Declared() {
		Arguments arguments("client", "Test harness");
		arguments.Flag("stats", "Open the panels");
		arguments.Value("frames", "N", "Exit after N frames");
		arguments.Value("scene", "PATH", "Scene to load");
		return arguments;
	}
}

TEST_CASE("a declared flag is absent until it is given", "[arguments]") {
	auto arguments = Declared();
	CommandLine line {};

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE_FALSE(arguments.Has("stats"));
}

TEST_CASE("a flag is present when given", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--stats" };

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE(arguments.Has("stats"));
}

TEST_CASE("a value is accepted in both spellings", "[arguments]") {
	SECTION("separated") {
		auto arguments = Declared();
		CommandLine line { "--frames", "120" };

		REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
		REQUIRE(arguments.GetInteger("frames", -1) == 120);
	}

	SECTION("joined") {
		auto arguments = Declared();
		CommandLine line { "--frames=120" };

		REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
		REQUIRE(arguments.GetInteger("frames", -1) == 120);
	}
}

TEST_CASE("a single dash is the same option", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "-stats" };

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE(arguments.Has("stats"));
}

TEST_CASE("an unknown option is an error rather than silence", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--stat" };

	const auto result = arguments.Parse(line.Count(), line.Values());
	REQUIRE_FALSE(result.Ok);
	REQUIRE(result.Error.find("--stat") != std::string::npos);
}

TEST_CASE("a missing value is an error", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--frames" };

	REQUIRE_FALSE(arguments.Parse(line.Count(), line.Values()).Ok);
}

TEST_CASE("a forgotten value does not swallow the next option", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--frames", "--stats" };

	const auto result = arguments.Parse(line.Count(), line.Values());
	REQUIRE_FALSE(result.Ok);
	REQUIRE(result.Error.find("--stats") != std::string::npos);
}

TEST_CASE("a value that takes no value is an error", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--stats=1" };

	REQUIRE_FALSE(arguments.Parse(line.Count(), line.Values()).Ok);
}

TEST_CASE("everything after -- is positional", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--stats", "--", "--frames", "file.scene" };

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE(arguments.Has("stats"));
	REQUIRE(arguments.Positional().size() == 2);
	REQUIRE(arguments.Positional()[0] == "--frames");
	REQUIRE_FALSE(arguments.Has("frames"));
}

TEST_CASE("a bare word is positional", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "game.atomic" };

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE(arguments.Positional().size() == 1);
	REQUIRE(arguments.Positional()[0] == "game.atomic");
}

TEST_CASE("an unparseable number falls back rather than throwing", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--frames", "soon" };

	REQUIRE(arguments.Parse(line.Count(), line.Values()).Ok);
	REQUIRE(arguments.GetInteger("frames", 60) == 60);
}

TEST_CASE("help is declared without being asked for", "[arguments]") {
	auto arguments = Declared();
	CommandLine line { "--help" };

	const auto result = arguments.Parse(line.Count(), line.Values());
	REQUIRE(result.Ok);
	REQUIRE(result.HelpRequested);

	// Generated from the declarations, so an option cannot exist and be
	// undocumented at the same time.
	const std::string help = arguments.Help();
	REQUIRE(help.find("--stats") != std::string::npos);
	REQUIRE(help.find("--frames N") != std::string::npos);
}
