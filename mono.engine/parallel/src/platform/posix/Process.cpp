#include <engine/core/Log.hpp>
#include <engine/parallel/Process.hpp>

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace engine::parallel {

	namespace {
		// Reaps a child if it has ended, without waiting when `block` is false.
		ProcessStatus Reap(uint64_t identifier, bool block) {
			ProcessStatus status;
			if (identifier == 0) {
				status.Reason = ExitReason::Gone;
				return status;
			}

			int state = 0;
			const pid_t result = ::waitpid(static_cast<pid_t>(identifier), &state, block ? 0 : WNOHANG);

			if (result == 0) {
				status.Reason = ExitReason::Running;
				return status;
			}
			if (result < 0) {
				// Already reaped, or never ours. Either way there is nothing
				// left to observe.
				status.Reason = ExitReason::Gone;
				return status;
			}

			if (WIFEXITED(state)) {
				status.Reason = ExitReason::Exited;
				status.Code = WEXITSTATUS(state);
			} else if (WIFSIGNALED(state)) {
				// This is what a hard fault looks like from outside: an abort
				// from the affinity check, a segfault, or the OOM killer.
				status.Reason = ExitReason::Signalled;
				status.Signal = WTERMSIG(state);
			} else {
				status.Reason = ExitReason::Gone;
			}

			return status;
		}
	}

	Process::~Process() {
		// A supervisor that goes away must not leave hosts behind: an orphaned
		// host holds its worlds, its memory and its port, and nothing is left
		// that knows to stop it.
		if (Identifier != 0 && Poll().Alive()) {
			Kill();
			Wait();
		}
	}

	Process::Process(Process &&other) noexcept : Identifier(other.Identifier), Last(other.Last) {
		other.Identifier = 0;
		other.Last = ProcessStatus{};
	}

	Process &Process::operator=(Process &&other) noexcept {
		if (this == &other) {
			return *this;
		}

		if (Identifier != 0 && Poll().Alive()) {
			Kill();
			Wait();
		}

		Identifier = other.Identifier;
		Last = other.Last;
		other.Identifier = 0;
		other.Last = ProcessStatus{};
		return *this;
	}

	bool Process::Start(const std::filesystem::path &program, const std::vector<std::string> &arguments) {
		return Start(program, arguments, ChannelEnd{});
	}

	bool Process::Start(
		const std::filesystem::path &program, const std::vector<std::string> &arguments, ChannelEnd endpoint
	) {
		if (Identifier != 0) {
			return false;
		}

		// posix_spawn rather than fork+exec: a fork from a process that already
		// has a job pool duplicates every thread's view of the heap and is
		// famously unsafe between the fork and the exec. posix_spawn does the
		// whole thing without that window.
		const std::string path = program.string();

		std::vector<std::string> owned;
		owned.reserve(arguments.size() + 1);
		owned.push_back(path);
		owned.insert(owned.end(), arguments.begin(), arguments.end());

		std::vector<char *> argv;
		argv.reserve(owned.size() + 1);
		for (std::string &argument : owned) {
			argv.push_back(argument.data());
		}
		argv.push_back(nullptr);

		// The slot a child finds its channel at. Kept in step with `INHERITED`
		// in the platform channel, and there is one caller of each.
		constexpr int INHERITED = 3;

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_t *actionsPointer = nullptr;
		int spare = -1;

		if (endpoint.Valid()) {
			auto handle = static_cast<int>(endpoint.Raw());

			// `dup2(3, 3)` is defined to do nothing at all — including not
			// clearing close-on-exec — so a handle that already landed on the
			// slot has to move before it can be placed there.
			if (handle == INHERITED) {
				spare = ::dup(handle);
				if (spare < 0) {
					ENGINE_ERROR("could not prepare a channel for '{}': {}", path, std::strerror(errno));
					return false;
				}
				handle = spare;
			}

			if (::posix_spawn_file_actions_init(&actions) != 0) {
				if (spare >= 0) {
					::close(spare);
				}
				return false;
			}
			actionsPointer = &actions;

			// The duplicate lands without close-on-exec, which is what lets it
			// survive the exec while every other handle this process holds —
			// including the driver's own end of this very channel — does not.
			::posix_spawn_file_actions_adddup2(&actions, handle, INHERITED);
		}

		pid_t child = 0;
		const int failure =
			::posix_spawn(&child, path.c_str(), actionsPointer, nullptr, argv.data(), environ);

		if (actionsPointer != nullptr) {
			::posix_spawn_file_actions_destroy(actionsPointer);
		}
		if (spare >= 0) {
			::close(spare);
		}

		// Closed here, on both paths. While this process holds a copy the child
		// can never see the channel end, because the kernel counts references
		// rather than intentions.
		endpoint.Close();

		if (failure != 0) {
			ENGINE_ERROR("could not start '{}': {}", path, std::strerror(failure));
			return false;
		}

		Identifier = static_cast<uint64_t>(child);
		Last = ProcessStatus{};
		Last.Reason = ExitReason::Running;
		return true;
	}

	ProcessStatus Process::Poll() {
		if (Identifier == 0) {
			return Last;
		}

		const ProcessStatus status = Reap(Identifier, false);
		if (!status.Alive()) {
			// Reaped, so nothing accumulates for a supervisor polling every
			// barrier. The last status is kept so a caller can still ask how it
			// ended after the fact.
			Last = status;
			Identifier = 0;
		}
		return status;
	}

	ProcessStatus Process::Wait() {
		if (Identifier == 0) {
			return Last;
		}

		Last = Reap(Identifier, true);
		Identifier = 0;
		return Last;
	}

	bool Process::RequestStop() {
		if (Identifier == 0) {
			return false;
		}
		return ::kill(static_cast<pid_t>(Identifier), SIGTERM) == 0;
	}

	bool Process::Kill() {
		if (Identifier == 0) {
			return false;
		}
		return ::kill(static_cast<pid_t>(Identifier), SIGKILL) == 0;
	}
}
