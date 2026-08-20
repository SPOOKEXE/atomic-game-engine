// Substitution and truncation.
//
// **The UTF-8 case is the one with teeth.** A field cut through the middle of a
// multibyte sequence produces a payload Discord's parser rejects, and it says
// nothing about why - so somebody with an emoji in their place name gets a
// feature that silently does not work, and nothing points at the emoji.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <discord/Activity.hpp>
#include <string>

TEST_SUITE_ID("discord.templates")

using discord::Clamp;
using discord::Facts;
using discord::Fill;

TEST_CASE("a token is replaced by its fact", "[discord][templates]") {
	const Facts facts{{"place", "Portals"}, {"parts", "412"}};
	CHECK(Fill("Editing {place}", facts, 128) == "Editing Portals");
	CHECK(Fill("{parts} parts in {place}", facts, 128) == "412 parts in Portals");
}

TEST_CASE("an unknown token resolves to nothing", "[discord][templates]") {
	// Somebody copied the server's line into the studio's box. Showing them
	// `{capacity}` on their own profile is worse than showing them a gap.
	const Facts facts{{"players", "3"}};
	CHECK(Fill("{players}/{capacity} players", facts, 128) == "3/ players");
}

TEST_CASE("a brace with no partner is literal", "[discord][templates]") {
	const Facts facts{{"world", "Start"}};
	CHECK(Fill("in {world", facts, 128) == "in {world");
	CHECK(Fill("100% {", facts, 128) == "100% {");
}

TEST_CASE("a fact nothing names is ignored", "[discord][templates]") {
	const Facts facts{{"world", "Start"}, {"unused", "nothing"}};
	CHECK(Fill("Editing", facts, 128) == "Editing");
}

TEST_CASE("text under the limit is untouched", "[discord][templates]") {
	CHECK(Clamp("Editing Portals", 128) == "Editing Portals");
	CHECK(Clamp("", 128).empty());
}

TEST_CASE("truncation counts characters and cuts on a boundary", "[discord][templates]") {
	// Four characters, seven bytes: 'a', then a two-byte, a three-byte and a
	// four-byte sequence.
	const std::string mixed = "aé€\U0001F600";
	REQUIRE(mixed.size() == 10);

	CHECK(Clamp(mixed, 4) == mixed);
	CHECK(Clamp(mixed, 10) == mixed);

	// Every prefix is a whole number of characters, never a split sequence.
	CHECK(Clamp(mixed, 3) == "aé€");
	CHECK(Clamp(mixed, 2) == "aé");
	CHECK(Clamp(mixed, 1) == "a");
	CHECK(Clamp(mixed, 0).empty());
}

TEST_CASE("a filled template is clamped like any other text", "[discord][templates]") {
	const Facts facts{{"place", "a name far longer than the limit allows"}};
	const std::string filled = Fill("{place}", facts, 6);
	CHECK(filled == "a name");
}
