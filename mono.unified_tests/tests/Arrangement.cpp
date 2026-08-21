// The matrix, checked as a matrix.
//
// **These cases are about the axes and not about what they wire up.** An
// arrangement is a name a person types and a name a report prints, so what has
// to hold is that those are the same name and that the product is the whole
// product - a matrix quietly missing a combination is a matrix that reports
// twelve passes having run eleven things.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <unified/Arrangement.hpp>

TEST_SUITE_ID("unified.arrangement")

using unified::AllArrangements;
using unified::Arrangement;
using unified::Content;
using unified::Discovery;
using unified::ParseArrangement;
using unified::Transport;

TEST_CASE("every arrangement's name reads back as that arrangement", "[unified][matrix]") {
	// **The name is the interface.** It is what `--arrangement` takes, what a
	// report prints and what a failing row in the summary is identified by, so
	// a name that does not round-trip is a run somebody cannot reproduce from
	// the output that reported it.
	for (const Arrangement &arrangement : AllArrangements()) {
		const std::string name = arrangement.Name();
		INFO("arrangement " << name);

		const std::optional<Arrangement> read = ParseArrangement(name);
		REQUIRE(read.has_value());
		CHECK(*read == arrangement);
	}
}

TEST_CASE("the matrix is the whole cross product and holds no duplicates", "[unified][matrix]") {
	const std::vector<Arrangement> all = AllArrangements();

	// Three transports, two content modes, two discovery modes.
	REQUIRE(all.size() == 12);

	std::set<std::string> names;
	for (const Arrangement &arrangement : all) {
		names.insert(arrangement.Name());
	}
	CHECK(names.size() == all.size());

	// The bisection is first, because it is where a person starts.
	CHECK(all.front() == Arrangement{});
	CHECK(all.front().Name() == "direct");
}

TEST_CASE("a name that describes no arrangement is refused", "[unified][matrix]") {
	// **Refused rather than defaulted**, because this parses a command line: a
	// typo that ran something else would be a matrix silently testing the
	// wrong point, and the report would name the point it did not run.
	CHECK_FALSE(ParseArrangement("").has_value());
	CHECK_FALSE(ParseArrangement("wibble").has_value());

	// Half an arrangement. `relayed` names no transport, and guessing at
	// `direct` would be this parser choosing a run the caller did not ask for.
	CHECK_FALSE(ParseArrangement("relayed").has_value());

	// One axis named twice, and a trailing separator.
	CHECK_FALSE(ParseArrangement("direct+loopback").has_value());
	CHECK_FALSE(ParseArrangement("direct+relayed+relayed").has_value());
	CHECK_FALSE(ParseArrangement("direct+").has_value());
	CHECK_FALSE(ParseArrangement("direct++relayed").has_value());
}

TEST_CASE("the axes are named independently of their order", "[unified][matrix]") {
	// A person types what they remember, and the two optional axes have no
	// natural order between them.
	const std::optional<Arrangement> one = ParseArrangement("lossy+relayed+advertised");
	const std::optional<Arrangement> other = ParseArrangement("advertised+lossy+relayed");
	REQUIRE(one.has_value());
	REQUIRE(other.has_value());
	CHECK(*one == *other);

	CHECK(one->Carrying == Transport::Lossy);
	CHECK(one->Serving == Content::Relayed);
	CHECK(one->Finding == Discovery::Advertised);

	// And what comes back out is the canonical spelling, whichever went in.
	CHECK(other->Name() == "lossy+relayed+advertised");
}
