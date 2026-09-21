// The phase rule, over combinations a live server would take a minute to reach.
//
// What is deliberately not here is a session against a real socket: that is what
// `just stress` is, and a suite that started a server would be a slow, flaky
// copy of it. What can regress silently is the rule below - a session stuck in
// the wrong phase reports the wrong thing about the server, which is the one
// failure a load test cannot afford.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <iterator>
#include <loadtest/Session.hpp>
#include <string_view>

TEST_SUITE_ID("tools.loadtest.session")

using loadtest::NextStage;
using loadtest::Progress;
using loadtest::Stage;

TEST_CASE("a session that has heard nothing stays where it is", "[loadtest]") {
	REQUIRE(NextStage(Stage::Dialling, Progress{}, 1.0, 20.0) == Stage::Dialling);
}

TEST_CASE("admission moves a session to streaming", "[loadtest]") {
	const Progress admitted{.Admitted = true};
	REQUIRE(NextStage(Stage::Dialling, admitted, 0.5, 20.0) == Stage::Streaming);
}

TEST_CASE("a join reached inside one poll skips streaming", "[loadtest]") {
	// One `Poll` drains the socket and applies everything that was in it, so a
	// small world admits and joins in the same call. A rule that only reached
	// `Playing` from `Streaming` would leave those sessions looking stuck.
	const Progress joined{.Admitted = true, .Joined = true};
	REQUIRE(NextStage(Stage::Dialling, joined, 0.1, 20.0) == Stage::Playing);
}

TEST_CASE("a refusal outranks an admission", "[loadtest]") {
	// A connector that was refused after being admitted is a client the server
	// dropped. Reading the flags the other way round leaves it playing.
	const Progress dropped{.Rejected = true, .Admitted = true, .Joined = true};
	REQUIRE(NextStage(Stage::Playing, dropped, 0.0, 20.0) == Stage::Refused);
}

TEST_CASE("a stage that goes nowhere for long enough times out", "[loadtest]") {
	REQUIRE(NextStage(Stage::Dialling, Progress{}, 21.0, 20.0) == Stage::TimedOut);

	// Admitted and streaming counts as progress, so the deadline only bites on a
	// session that is not moving at all.
	const Progress admitted{.Admitted = true};
	REQUIRE(NextStage(Stage::Streaming, admitted, 900.0, 20.0) == Stage::Streaming);
}

TEST_CASE("a deadline of zero never times anything out", "[loadtest]") {
	// What a run being debugged passes. A harness that wrote sessions off while
	// somebody sat in a debugger on the server would report a server failure.
	REQUIRE(NextStage(Stage::Dialling, Progress{}, 1e6, 0.0) == Stage::Dialling);
}

TEST_CASE("a terminal stage is terminal", "[loadtest]") {
	// The same rule `net::Link`'s lifecycle has: nothing comes back to life, so
	// a late welcome from a server that already refused this client cannot make
	// a written-off session start counting again.
	const Progress joined{.Admitted = true, .Joined = true};
	REQUIRE(NextStage(Stage::Refused, joined, 0.0, 20.0) == Stage::Refused);
	REQUIRE(NextStage(Stage::TimedOut, joined, 0.0, 20.0) == Stage::TimedOut);

	REQUIRE(loadtest::Terminal(Stage::Refused));
	REQUIRE(loadtest::Terminal(Stage::TimedOut));
	REQUIRE_FALSE(loadtest::Terminal(Stage::Playing));
}

TEST_CASE("every stage has a name of its own", "[loadtest]") {
	// The report prints these, and two stages sharing a name is a report that
	// cannot tell a refusal from a timeout - which are opposite diagnoses.
	const Stage every[]{Stage::Dialling, Stage::Streaming, Stage::Playing, Stage::Refused, Stage::TimedOut};
	for (size_t left = 0; left < std::size(every); left++) {
		REQUIRE(std::string_view(loadtest::Describe(every[left])) != "?");
		for (size_t right = left + 1; right < std::size(every); right++) {
			REQUIRE(
				std::string_view(loadtest::Describe(every[left])) !=
				std::string_view(loadtest::Describe(every[right]))
			);
		}
	}
}
