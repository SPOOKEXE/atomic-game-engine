#pragma once

// What the load test was asked to do.
//
// Declared through `core::Flags` like every other program here, so
// `loadtest --flags` and `loadtest --help` behave the way `server --flags` and
// `client --help` do. A tool with its own argv parser is a tool whose options
// drift from the repository's.
//
// @tier shared

#include <cstdint>
#include <filesystem>
#include <string>

namespace loadtest {

	// How a run is shaped.
	struct Options {
		// How many virtual clients to open.
		uint32_t Clients = 200;

		// Where the server is. A host and a port.
		std::string Address = "127.0.0.1";
		uint16_t Port = 0;

		// How fast the harness ticks its clients, in hertz. Match the server's
		// tick rate: a client polling slower than the server publishes builds a
		// backlog in the socket that reads as latency it did not have.
		double TickRate = 30.0;

		// How long to run for, in seconds. Zero runs until the tick budget.
		double Seconds = 0.0;

		// How many harness ticks to run. Zero runs until the time budget. With
		// both zero the run has no end and the recipe's `timeout` is what stops
		// it, which is why the tool refuses that combination.
		int64_t Ticks = 0;

		// How many sessions may start dialling on one tick.
		//
		// **Staggered rather than all at once, and it is realism rather than
		// politeness.** Two hundred simultaneous handshakes is a burst no
		// deployment sees, and it would measure the admission path's worst case
		// while saying nothing about the steady state the rest of the run is
		// about. Raise it to measure exactly that burst.
		uint32_t ConnectsPerTick = 8;

		// How often a client submits an input, in harness ticks.
		uint32_t InputEveryTicks = 1;

		// How long a session may make no progress before it is written off.
		double StallSeconds = 20.0;

		// Fold this run's own frame graph into a `.folded` file here.
		//
		// The harness's own cost, which is what proves the harness is not the
		// bottleneck. Empty writes none.
		std::filesystem::path ProfilePath;
	};

	// Declares this tool's flags. Called once, before anything is parsed.
	//
	// @return `false` when a name was already declared.
	bool DeclareFlags();

	// Reads the options out of the frozen flag table.
	//
	// @return The options.
	Options OptionsFromFlags();
}
