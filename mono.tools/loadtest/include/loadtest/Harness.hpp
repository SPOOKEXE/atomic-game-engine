#pragma once

// The loop that drives every virtual client.
//
// **One thread and one poll loop over the sessions.** A transport is not
// thread-safe and `net/AGENTS.md` says so in as many words - one owner, one
// thread - so the shapes available were a thread per session, a pool with each
// session pinned to a worker, or one loop. Two hundred threads is a measurement
// of the scheduler; a pool is honest but only pays once the loop is the
// bottleneck, and whether it is is a question this tool answers rather than
// assumes. `Summary::ApplyMeanMicroseconds` and the harness's own profile are
// what say when that has changed.
//
// @tier shared

#include <loadtest/Options.hpp>
#include <loadtest/Session.hpp>
#include <loadtest/Statistics.hpp>
#include <memory>
#include <vector>

namespace loadtest {

	// Opens the sessions, ticks them, and reports.
	class Harness {
	  public:
		// Builds a harness for one run.
		//
		// @param options How the run is shaped. Copied, so the caller's may go
		//        out of scope.
		explicit Harness(const Options &options);

		// Runs to the tick or time budget.
		//
		// @return What every session did.
		Summary Run();

		// How many sockets could not be opened.
		//
		// A machine's file-descriptor limit is the ordinary cause, and a run that
		// silently opened half the clients it was asked for is a run whose
		// numbers mean something else.
		size_t Unopened() const {
			return Unopened_;
		}

	  private:
		Options Settings;
		std::vector<std::unique_ptr<Session>> Sessions;
		size_t Unopened_ = 0;
	};
}
