// The arithmetic that turns two hundred session reports into the dozen numbers
// somebody compares between two runs.
//
// The failure worth catching is a summary that averages away what a load test is
// looking for, so these are about the distribution and about which sessions
// count towards which figure.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <loadtest/Statistics.hpp>
#include <vector>

TEST_SUITE_ID("tools.loadtest.statistics")

using loadtest::SessionReport;
using loadtest::Stage;
using loadtest::Summarise;

TEST_CASE("a percentile is a reading somebody actually took", "[loadtest]") {
	const std::vector<double> readings{1.0, 2.0, 3.0, 4.0, 100.0};

	// Nearest-rank, not interpolated: an interpolated p99 over five readings
	// reports a number no session measured.
	REQUIRE(loadtest::Percentile(readings, 0.0) == 1.0);
	REQUIRE(loadtest::Percentile(readings, 0.5) == 3.0);
	REQUIRE(loadtest::Percentile(readings, 0.99) == 100.0);
	REQUIRE(loadtest::Percentile({}, 0.5) == 0.0);
}

TEST_CASE("sessions are counted under the stage they ended in", "[loadtest]") {
	std::vector<SessionReport> reports(4);
	reports[0].Final = Stage::Playing;
	reports[1].Final = Stage::Playing;
	reports[2].Final = Stage::TimedOut;
	reports[3].Final = Stage::Refused;

	const loadtest::Summary summary = Summarise(reports, 10.0);
	REQUIRE(summary.Sessions == 4);
	REQUIRE(summary.Playing == 2);
	REQUIRE(summary.TimedOut == 1);
	REQUIRE(summary.Refused == 1);
	REQUIRE(summary.Streaming == 0);
}

TEST_CASE("a session that was admitted and then died still counts as admitted", "[loadtest]") {
	// **The number an operator reads first when a run goes wrong.** Admission is
	// the handshake and joining is the whole world having arrived, so counting
	// admissions by final stage would report a streaming failure as an admission
	// failure - which sends the reader to the wrong half of the server.
	std::vector<SessionReport> reports(2);
	reports[0].Final = Stage::TimedOut;
	reports[0].AdmitSeconds = 0.4;
	reports[1].Final = Stage::Playing;
	reports[1].AdmitSeconds = 0.2;
	reports[1].JoinSeconds = 1.5;

	const loadtest::Summary summary = Summarise(reports, 10.0);
	REQUIRE(summary.Admitted == 2);
	REQUIRE(summary.Playing == 1);
	REQUIRE(summary.JoinP50Seconds == 1.5);
}

TEST_CASE("the join distribution keeps the tail a mean would hide", "[loadtest]") {
	std::vector<SessionReport> reports(100);
	for (size_t index = 0; index < reports.size(); index++) {
		reports[index].Final = Stage::Playing;
		reports[index].AdmitSeconds = 0.1;
		// Ninety-five quick joins and five that took ten seconds. The mean barely
		// moves; p99 is the whole point of carrying the distribution.
		reports[index].JoinSeconds = index >= 95 ? 10.0 : 0.5;
	}

	const loadtest::Summary summary = Summarise(reports, 20.0);
	REQUIRE(summary.JoinMeanSeconds < 1.0);
	REQUIRE(summary.JoinP50Seconds == 0.5);
	REQUIRE(summary.JoinP99Seconds == 10.0);
}

TEST_CASE("traffic totals become a rate against the run's own clock", "[loadtest]") {
	std::vector<SessionReport> reports(2);
	reports[0].BytesSent = 1000;
	reports[0].BytesReceived = 40000;
	reports[1].BytesSent = 1000;
	reports[1].BytesReceived = 40000;

	const loadtest::Summary summary = Summarise(reports, 4.0);
	REQUIRE(summary.BytesSent == 2000);
	REQUIRE(summary.BytesReceived == 80000);
	REQUIRE(summary.SentBytesPerSecond == 500.0);
	REQUIRE(summary.ReceivedBytesPerSecond == 20000.0);
}

TEST_CASE("the smallest replica is a real reading and not a zero", "[loadtest]") {
	// Two numbers because one would hide the case worth catching: clients seeing
	// different amounts of the world is interest management working, and one
	// client seeing none of it is a client that never really joined. A smallest
	// that started at zero would report the first every time.
	std::vector<SessionReport> reports(3);
	reports[0].Entities = 500;
	reports[1].Entities = 480;
	reports[2].Entities = 512;

	const loadtest::Summary summary = Summarise(reports, 1.0);
	REQUIRE(summary.SmallestReplica == 480);
	REQUIRE(summary.LargestReplica == 512);
}

TEST_CASE("the apply cost is a mean over polls and not over sessions", "[loadtest]") {
	// A session that joined late polled fewer times, so dividing by the session
	// count reports a per-poll cost that is really a per-session one - and the
	// question this answers is whether one poll fits in a tick.
	std::vector<SessionReport> reports(2);
	reports[0].ApplyMicroseconds = 1000.0;
	reports[0].Polls = 100;
	reports[1].ApplyMicroseconds = 500.0;
	reports[1].Polls = 25;

	const loadtest::Summary summary = Summarise(reports, 1.0);
	REQUIRE(summary.Polls == 125);
	REQUIRE(summary.ApplyMeanMicroseconds == 12.0);
}

TEST_CASE("a run with no sessions summarises to nothing rather than dividing by zero", "[loadtest]") {
	const loadtest::Summary summary = Summarise({}, 0.0);
	REQUIRE(summary.Sessions == 0);
	REQUIRE(summary.ApplyMeanMicroseconds == 0.0);
	REQUIRE(summary.SentBytesPerSecond == 0.0);
	REQUIRE(summary.SmallestReplica == 0);
	REQUIRE_FALSE(loadtest::Describe(summary).empty());
}
