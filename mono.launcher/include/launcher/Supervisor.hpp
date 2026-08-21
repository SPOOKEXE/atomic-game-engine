#pragma once

// One child, started and watched.
//
// **A process and not a linked library, which is the decision this whole
// program rests on.** The launcher could have linked the client, the server,
// the origin and the editor and called into them; it would then be the largest
// tier escape in the repository, it could not be built under the `server` or
// `cdn` presets, and a fault anywhere in any of them would take the launcher
// down with it. Spawning the real staged binary costs one `posix_spawn` and
// buys all three back - and it means what the launcher runs is exactly what a
// person would have typed, which is the only way the command line it displays
// can be honest.
//
// **The child inherits the launcher's streams**, which is `parallel::Process`'s
// documented behaviour and the right one here: a server's log belongs in the
// terminal the launcher was started from, where somebody can scroll it. This
// class reports *that the child ended and how*, not what it said.
//
// **Restart is here and stop is polite first.** A supervised origin is a thing
// you fix a folder path on and start again, and a supervised host is a thing
// you stop before closing the launcher - `parallel::Process`'s destructor
// already refuses to leave an orphan holding a port, and this makes the same
// thing a button.
//
// @tier L13 · client
// @since v0.18

#include <engine/parallel/Process.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

	// Where a supervised child is in its life.
	//
	// @since v0.18
	enum class ChildState : uint8_t {
		// Nothing has been started, or the last one was cleared away.
		Idle,

		// Running now.
		Running,

		// Returned zero. A program that was asked to stop, and did.
		Ended,

		// Returned non-zero or took a signal. **One state for both**, because
		// the difference between them is in the summary line and the thing the
		// screen has to do - say so in red and offer Restart - is the same.
		Failed,
	};

	// A started child, and enough about it to report and act on.
	class Supervisor {
	  public:
		// Starts a program, replacing whatever this held.
		//
		// @param program   The staged binary.
		// @param arguments Its argv, not including the program name.
		// @param failure   Filled in with why, when this returns false.
		// @return `false` when the child could not be started at all.
		bool Start(
			const std::filesystem::path &program,
			const std::vector<std::string> &arguments,
			std::string &failure
		);

		// Starts the same program and arguments again.
		//
		// @param failure Filled in with why, when this returns false.
		// @return `false` when it could not be started.
		bool Restart(std::string &failure);

		// Checks on the child without waiting, reaping it if it has ended.
		//
		// Call once a frame. **Reaping is the point**: a launcher that polled
		// only when somebody looked at the panel would leave a zombie for every
		// run of the session.
		//
		// @param now Seconds from a monotonic clock, for the running time.
		void Poll(double now);

		// Where the child is.
		//
		// @return The state.
		ChildState State() const {
			return Current;
		}

		// Whether there is a child to stop.
		//
		// @return `true` while one is running.
		bool Running() const {
			return Current == ChildState::Running;
		}

		// One line about how it ended, or what it is.
		//
		// @return The summary, empty before anything was started.
		const std::string &Summary() const {
			return Line;
		}

		// The program this is supervising, for the panel's heading.
		//
		// @return The path, empty before anything was started.
		const std::filesystem::path &Program() const {
			return Started;
		}

		// The child's process id, for a log line or a debugger.
		//
		// @return The id, or zero.
		uint64_t Id() const {
			return Child.Id();
		}

		// How long it has been running, or ran for.
		//
		// @return Seconds.
		double Seconds() const {
			return Elapsed;
		}

		// Asks the child to stop, letting it flush and close.
		void RequestStop();

		// Stops the child immediately. For one that stopped answering.
		void Kill();

		// Forgets a finished child, so the panel goes back to the form.
		//
		// Does nothing while one is running: a launcher that could dismiss a
		// live server would be a launcher that loses track of a port.
		void Clear();

	  private:
		engine::parallel::Process Child;
		std::filesystem::path Started;
		std::vector<std::string> Arguments;
		std::string Line;
		ChildState Current = ChildState::Idle;
		double StartedAt = 0.0;
		double Elapsed = 0.0;
	};
}
