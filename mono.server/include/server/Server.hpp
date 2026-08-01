#pragma once

// The server program's own code — an attachment on top of the engine, not a
// layer of it.
//
// It owns the fixed tick, the world set and the shutdown path. It contains no
// renderer: `mono.server` links no `client`-tier target, so `render`, `ui`,
// `audio`, `input`, `text` and `vfx` are absent from the link line entirely. A
// server-side script that reaches for input will fail because the class was
// never registered into this binary, not because a check rejected it.
//
// There is no window, no swapchain and no SDL here. That is checkable rather
// than aspirational: the `server` preset configures with no graphics stack and
// the staged `server/` directory has no `shaders/` folder.

#include <engine/core/Clock.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace server {

	struct Options {
		// Ticks per second. A world ticks at its own rate; this is the rate the
		// one world this version hosts runs at.
		double TickRate = 30.0;

		// Entities in the placeholder world, until there is a game file to load
		// one from.
		uint32_t Entities = 4096;

		// -1 runs until stopped. A tick budget is what makes the server usable
		// from a test or a CI job.
		int64_t MaximumTicks = -1;

		// Run for this long, then drain and exit. Zero means run until stopped.
		double Seconds = 0.0;

		// Run every tick back to back instead of pacing to TickRate. For
		// measuring the simulation rather than the sleep between ticks.
		bool Unpaced = false;

		// TODO(v0.2+): deserialize and host this game file. Parsed and reported
		// now so that a command line written against the roadmap fails with a
		// clear message rather than being silently ignored — `gamefile` arrives
		// with the game file format.
		std::string GamePath;

		std::filesystem::path AssetsDirectory;
	};

	// What the run produced. Returned rather than logged only, so a test can
	// assert on it.
	struct RunSummary {
		uint64_t Ticks = 0;
		double Seconds = 0.0;
		float SlowestTickMilliseconds = 0.0f;
		float MeanTickMilliseconds = 0.0f;
		// Ticks that overran their budget. The number that says whether the
		// tick rate was actually held.
		uint64_t Overruns = 0;
	};

	class Server {
	  public:
		bool Initialise(const Options &options);
		void Shutdown();

		// Runs the fixed-tick loop until the budget is spent or Stop is called.
		RunSummary Run();

		// Safe from another thread: the loop reads it between ticks.
		void Stop();

		engine::ecs::Store &World() {
			return Store;
		}

	  private:
		void Tick(float delta);

		Options Settings;
		engine::ecs::Store Store{"server"};
		engine::ecs::Scheduler Scheduler;

		bool Running = false;

		// There is no tick counter here. The world keeps one — `Store.Time()`
		// — and a second copy on the host is a fact that can disagree with
		// itself the first time one of them is advanced in a branch.
	};
}
