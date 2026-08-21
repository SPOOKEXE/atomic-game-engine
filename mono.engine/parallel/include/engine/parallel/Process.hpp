#pragma once

// A child process, and enough control over one to supervise it.
//
// This is the sub-area `parallel/AGENTS.md` reserves for "separate OS
// processes, supervision, crash isolation". It exists for one reason, and the
// reason is worth stating because the usual one is wrong: **processes are for
// crash isolation, not for speed.** A world that aborts on an affinity
// violation should take down one host rather than the server. Two processes
// simulating two worlds are not faster than two threads doing the same; they
// are more survivable.
//
// **What a process boundary can and cannot isolate.** A soft fault - a system
// throwing, a script erroring, a tick budget overrun - is caught at the
// world-tick boundary and never needs a process. A hard fault - `abort()`, a
// segfault, an out-of-memory kill - takes the address space, and no amount of
// care inside a process can arrange otherwise. Catching `SIGSEGV` and carrying
// on means carrying on with a heap that may already be corrupt, which makes the
// *neighbours* suspect too. So the only honest isolation from a hard fault is a
// separate address space, and that is what this buys.
//
// Deliberately small. There is no process pool, no message passing here, and no
// lifecycle beyond spawn, observe, ask to stop, insist. Everything that crosses
// between two processes is a `Channel`; this only starts them and notices when
// they die.
//
// @tier L2 · shared

#include <engine/parallel/ProcessChannel.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine::parallel {

	// How a child ended.
	//
	// @since v0.2
	enum class ExitReason : uint8_t {
		// Still running.
		Running,

		// Returned from main, or called exit.
		Exited,

		// Killed by a signal, which on this engine's own children means a hard
		// fault: an abort from the affinity check, a segfault, or the
		// out-of-memory killer.
		Signalled,

		// The process was never started, or has already been reaped.
		Gone,
	};

	// What a child has done so far.
	//
	// @since v0.2
	struct ProcessStatus {
		// Whether it is running, and how it stopped if not.
		ExitReason Reason = ExitReason::Gone;

		// The exit code, meaningful only when Reason is Exited.
		int Code = 0;

		// The signal number, meaningful only when Reason is Signalled.
		int Signal = 0;

		// Whether the child is still running.
		//
		// @return `true` while it lives.
		bool Alive() const {
			return Reason == ExitReason::Running;
		}

		// Whether the child died the way a hard fault dies.
		//
		// A supervisor treats this differently from a clean exit: a host that
		// returned zero was told to stop, and one that took a signal did not
		// get the chance.
		//
		// @return `true` for a signal death or a non-zero exit.
		bool Faulted() const {
			return Reason == ExitReason::Signalled || (Reason == ExitReason::Exited && Code != 0);
		}
	};

	// Returns a stable, human-readable name for an exit reason.
	//
	// @param reason The reason to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ExitReason reason);

	// One child process.
	//
	// Move-only, because two owners of one child would both try to reap it and
	// one of them would find it already gone.
	//
	// @since v0.2
	class Process {
	  public:
		// Creates a handle that owns no child.
		Process() = default;

		// Terminates and reaps the child if it is still running.
		//
		// A supervisor that goes away must not leave hosts behind - an
		// orphaned host holds its worlds, its memory and its port, and nothing
		// is left that knows to stop it.
		~Process();

		// A child has one owner.
		Process(const Process &) = delete;

		// A child has one owner.
		Process &operator=(const Process &) = delete;

		// Transfers ownership of the child.
		Process(Process &&other) noexcept;

		// Transfers ownership of the child, reaping whatever this held.
		Process &operator=(Process &&other) noexcept;

		// Starts a program.
		//
		// The child inherits this process's standard streams, so a host's log
		// lands where the supervisor's does. That is deliberate: a crash
		// message that went somewhere nobody reads is a crash nobody explains.
		//
		// @param program   The executable to run.
		// @param arguments The arguments, not including the program name.
		// @return `false` when the child could not be started.
		bool Start(const std::filesystem::path &program, const std::vector<std::string> &arguments = {});

		// Starts a program holding one end of a channel.
		//
		// The child adopts it with `AdoptInheritedChannel`. There is no name or
		// number to agree on: a child has exactly one channel to whoever
		// started it, so the slot is fixed and the command line does not have
		// to carry something only two layers understand.
		//
		// **The endpoint is consumed either way.** On success it belongs to the
		// child and this process closes its copy - which is what makes the
		// child see the channel close when this process dies, and what makes
		// this process see it close when the child does. Holding a spare copy
		// would mean neither end ever noticed the other going away.
		//
		// @param program   The executable to run.
		// @param arguments The arguments, not including the program name.
		// @param endpoint  The end the child receives.
		// @return `false` when the child could not be started.
		bool Start(
			const std::filesystem::path &program,
			const std::vector<std::string> &arguments,
			ChannelEnd endpoint
		);

		// Whether this handle owns a child, running or not yet reaped.
		//
		// @return `true` when there is something to observe.
		bool Started() const {
			return Identifier != 0;
		}

		// The child's process id.
		//
		// For logs and for a person with a debugger. Nothing in the engine
		// addresses a child by number.
		//
		// @return The id, or zero when nothing was started.
		uint64_t Id() const {
			return Identifier;
		}

		// Checks on the child without waiting for it.
		//
		// Reaps it if it has ended, so a supervisor polling this every barrier
		// does not accumulate zombies.
		//
		// @return What the child has done so far.
		ProcessStatus Poll();

		// Waits for the child to end.
		//
		// @return How it ended.
		ProcessStatus Wait();

		// Asks the child to stop.
		//
		// Polite: the child gets the chance to flush a snapshot and close its
		// channel. A supervisor follows this with a deadline and then `Kill`.
		//
		// @return `false` when there was nothing to ask.
		bool RequestStop();

		// Stops the child immediately.
		//
		// The child gets no chance to do anything. For the case where a host
		// has stopped answering its heartbeat, which is the case where asking
		// nicely has already failed.
		//
		// @return `false` when there was nothing to stop.
		bool Kill();

	  private:
		// Opaque so that no public header names an operating system -
		// `MonoLibrary.cmake` says the build is the only place that does.
		uint64_t Identifier = 0;

		// A second handle, for the platforms whose process id is not enough to
		// act on. Unused where it is: an id that can be waited on and signalled
		// directly needs nothing beside it.
		//
		// It exists rather than the id being made to mean two things, because
		// `Id()` promises the number a person types into a debugger, and a
		// handle is not that number.
		//
		// `[[maybe_unused]]` because "unused where it is" is a fact about the
		// platform and not an oversight: Clang reports an unread private field
		// and GCC does not, so without this the macOS build warns about a
		// member the comment above already explains.
		[[maybe_unused]] uint64_t Native = 0;

		ProcessStatus Last;
	};

	// The number of workers a host should start, given how many hosts share the
	// machine.
	//
	// **Every host calling `Jobs::Start(0)` is the bug this exists to prevent.**
	// That asks for one worker per hardware thread; eight hosts on a twenty-four
	// core machine then run a hundred and ninety threads over twenty-four cores,
	// and every one of them is slower than it would have been alone.
	//
	// The calling thread of each host drains its own batches, so a host's share
	// is one fewer worker than its slice of the logical processors available to
	// this process. Operating-system affinity restrictions are part of that count.
	//
	// @param hosts The number of hosts sharing this machine, at least one.
	// @return The worker count for one host, possibly zero.
	// @since v0.2
	unsigned WorkersPerHost(unsigned hosts);
}
