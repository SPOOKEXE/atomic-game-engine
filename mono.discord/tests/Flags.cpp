// The `discord` flag table, and the one rule the four-program story rests on.
//
// **Every text row in that table declares empty, and that is load-bearing.**
// The studio says "Editing", a server says "Hosting", an origin says "Serving";
// a default written into the shared table would be one of them pretending to be
// all four. So `SettingsFromFlags` keeps the caller's wording for anything
// nobody set, and answers with the flag for anything somebody did. Those two
// halves are what this suite is for - get the first wrong and every program
// reports a blank card, get the second wrong and a command line does nothing.

#include <engine/core/Flags.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <discord/Settings.hpp>
#include <string>

TEST_SUITE_ID("discord.flags")

using engine::core::Flags;
using engine::core::FlagSource;
using engine::core::FlagStatus;

namespace {
	// Every case starts from an empty table: the store is process-wide, and a
	// value left behind would make the next case pass for the wrong reason.
	void Fresh() {
		Flags::Reset();
		REQUIRE(discord::DeclareFlags());
	}

	// What a program hands in - its own wording, and nothing switched on.
	discord::Settings Program() {
		discord::Settings settings;
		settings.Details = "Hosting {game}";
		settings.State = "{players} of {capacity} players";
		settings.ButtonLabel = "Join this server";
		return settings;
	}

	// A flag set the way `--flag NAME=VALUE` sets one.
	void Typed(std::string_view name, std::string_view value) {
		REQUIRE(Flags::Set(name, value, FlagSource::CommandLine) == FlagStatus::Applied);
	}
}

TEST_CASE("a program that never declared the table keeps its own settings", "[discord][flags]") {
	Flags::Reset();

	// No `DeclareFlags`. A dead flag reads `false` and an empty string, which
	// here would be the right answer for entirely the wrong reason - so the
	// question asked is whether anything declared the table at all.
	const discord::Settings settings = discord::SettingsFromFlags(Program());
	CHECK(settings.Details == "Hosting {game}");
	CHECK(settings.ButtonLabel == "Join this server");
	CHECK_FALSE(settings.Enabled);
}

TEST_CASE("a declared but unset table leaves every default alone", "[discord][flags]") {
	Fresh();

	const discord::Settings settings = discord::SettingsFromFlags(Program());
	CHECK(settings.Details == "Hosting {game}");
	CHECK(settings.State == "{players} of {capacity} players");
	CHECK(settings.ButtonLabel == "Join this server");
	CHECK(settings.LargeImage == "atomic");
	CHECK(settings.LargeText == "Atomic Game Engine");
	CHECK(settings.ShowElapsed);
	CHECK_FALSE(settings.Enabled);
	CHECK_FALSE(settings.HideNames);
	CHECK_FALSE(settings.JoinSecrets);
}

TEST_CASE("what somebody typed wins over what the program shipped", "[discord][flags]") {
	Fresh();

	Typed("discord.enabled", "true");
	Typed("discord.app-id", "1234567890123456789");
	Typed("discord.details", "Running {game}");
	Typed("discord.button-url", "https://example.invalid/join");
	Typed("discord.hide-names", "true");

	const discord::Settings settings = discord::SettingsFromFlags(Program());

	CHECK(settings.Enabled);
	CHECK(settings.ApplicationId == "1234567890123456789");
	CHECK(settings.Details == "Running {game}");
	CHECK(settings.ButtonUrl == "https://example.invalid/join");
	CHECK(settings.HideNames);

	// Untouched rows still carry the program's wording rather than the table's
	// empty declaration, which is the half that is easy to lose.
	CHECK(settings.State == "{players} of {capacity} players");
	CHECK(settings.ButtonLabel == "Join this server");
	CHECK(settings.ShowElapsed);
}

TEST_CASE("a flag can turn a default off as well as on", "[discord][flags]") {
	Fresh();

	// `false` over a `true` default is the case a "did somebody set this"
	// implemented as "is the value non-empty" would silently lose.
	Typed("discord.show-elapsed", "false");

	const discord::Settings settings = discord::SettingsFromFlags(Program());
	CHECK_FALSE(settings.ShowElapsed);
}

TEST_CASE("nothing is configured without an application id", "[discord][flags]") {
	Fresh();

	SECTION("switched on, nothing pasted") {
		Typed("discord.enabled", "true");

		// The shipped default is `-1`, which is not a snowflake and never will
		// be. Reaching Discord with it would produce `Invalid Client ID` and a
		// person wondering what they did wrong.
		const discord::Settings settings = discord::SettingsFromFlags(Program());
		CHECK(settings.ApplicationId == discord::UNSET_APPLICATION_ID);
		CHECK_FALSE(discord::IsConfigured(settings));
	}

	SECTION("switched on and cleared by hand") {
		Typed("discord.enabled", "true");
		Typed("discord.app-id", "");

		// A field somebody emptied and a field nobody touched mean the same
		// thing, so they behave the same way.
		const discord::Settings settings = discord::SettingsFromFlags(Program());
		CHECK_FALSE(discord::IsConfigured(settings));
	}

	SECTION("an id pasted but the switch left off") {
		Typed("discord.app-id", "1234567890123456789");

		const discord::Settings settings = discord::SettingsFromFlags(Program());
		CHECK_FALSE(discord::IsConfigured(settings));
	}

	SECTION("both") {
		Typed("discord.enabled", "true");
		Typed("discord.app-id", "1234567890123456789");

		const discord::Settings settings = discord::SettingsFromFlags(Program());
		CHECK(discord::IsConfigured(settings));
	}

	Flags::Reset();
}
