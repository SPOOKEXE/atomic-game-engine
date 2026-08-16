#pragma once

// Turning a pile of per-session reports into the dozen numbers an operator
// compares between two runs.
//
// **Arithmetic over a span and nothing else**, so the summary is exercised over
// hand-built reports rather than by standing up a server. What it must not do
// is average away the thing a load test is looking for: a mean join time with
// two hundred clients in it hides the twenty that took ten seconds, so the
// distribution is carried and the mean is beside it rather than instead of it.
//
// @tier shared

#include <cstdint>
#include <loadtest/Session.hpp>
#include <span>
#include <string>
#include <vector>

namespace loadtest {

	// What a whole run did.
	struct Summary {
		// How many sessions ended in each stage. They add up to `Sessions`.
		//@{
		size_t Sessions = 0;
		size_t Playing = 0;
		size_t Streaming = 0;
		size_t Dialling = 0;
		size_t Refused = 0;
		size_t TimedOut = 0;
		//@}

		// Sessions that were admitted at any point, whether or not they joined.
		//
		// **Not the same as `Playing`.** Admission is the handshake; joining is
		// the whole world having arrived. A run where every client is admitted
		// and none joins is a streaming problem, and one where admission itself
		// fails is a different problem entirely.
		size_t Admitted = 0;

		// Seconds from dialling to admission, over the sessions that were
		// admitted.
		//@{
		double AdmitMeanSeconds = 0.0;
		double AdmitP50Seconds = 0.0;
		double AdmitP99Seconds = 0.0;
		//@}

		// Seconds from dialling to joined, over the sessions that joined.
		//@{
		double JoinMeanSeconds = 0.0;
		double JoinP50Seconds = 0.0;
		double JoinP95Seconds = 0.0;
		double JoinP99Seconds = 0.0;
		//@}

		// Totals over every session, measured at the socket.
		//@{
		uint64_t BytesSent = 0;
		uint64_t BytesReceived = 0;
		uint64_t PacketsSent = 0;
		uint64_t PacketsReceived = 0;
		uint64_t PacketsLost = 0;
		//@}

		// Totals over every session.
		//@{
		uint64_t InputsSent = 0;
		uint64_t InputsRefused = 0;
		uint64_t Applied = 0;
		uint64_t Deltas = 0;
		uint64_t Incomplete = 0;
		uint64_t Refusals = 0;
		//@}

		// What one `Connector::Poll` cost, in microseconds, over every poll of
		// every session.
		//@{
		double ApplyMeanMicroseconds = 0.0;
		double ApplyTotalMilliseconds = 0.0;
		uint64_t Polls = 0;
		//@}

		// Entities in the replicas, smallest and largest.
		//
		// **Two numbers because one would hide the case worth catching**: every
		// client seeing a different amount of the world is interest management
		// working, and one client seeing none of it while the rest see all of it
		// is a client that never really joined.
		//@{
		uint64_t SmallestReplica = 0;
		uint64_t LargestReplica = 0;
		//@}

		// Wall time the run took, and the bytes per second that follow from it.
		//@{
		double Seconds = 0.0;
		double SentBytesPerSecond = 0.0;
		double ReceivedBytesPerSecond = 0.0;
		//@}
	};

	// Nearest-rank percentile over a copy of `readings`.
	//
	// Not interpolated, for `core::FrameGraph`'s reason: an interpolated p99 can
	// report a figure no session actually measured.
	//
	// @param readings The readings. Copied, so the caller's order is kept.
	// @param fraction Between zero and one.
	// @return The reading at that rank, or zero when there are none.
	double Percentile(std::vector<double> readings, double fraction);

	// Reduces every session's report to one run's numbers.
	//
	// @param reports One per session.
	// @param seconds Wall time the run took, for the per-second figures.
	// @return The summary.
	Summary Summarise(std::span<const SessionReport> reports, double seconds);

	// The summary as the block of text the tool prints.
	//
	// @param summary What to describe.
	// @return The text, newline-terminated.
	std::string Describe(const Summary &summary);
}
