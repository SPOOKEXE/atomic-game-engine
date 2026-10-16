#include <array>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <testrunner/Process.hpp>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace testrunner {
	namespace {
		int MoveAboveStdio(int descriptor) {
			if (descriptor > STDERR_FILENO) return descriptor;
#ifdef F_DUPFD_CLOEXEC
			const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
			const int moved = fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1);
			if (moved >= 0 && fcntl(moved, F_SETFD, FD_CLOEXEC) < 0) {
				close(moved);
				return -1;
			}
#endif
			if (moved >= 0) close(descriptor);
			return moved;
		}
	}

	ProcessResult Run(const std::vector<std::string> &arguments) {
		ProcessResult result;
		if (arguments.empty()) {
			return result;
		}

		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (const auto &argument : arguments) {
			argv.push_back(const_cast<char *>(argument.c_str()));
		}
		argv.push_back(nullptr);

		// The portable pipe() and fcntl() path uses separate calls. Serialize
		// only pipe setup and spawn there, so concurrent Run calls cannot spawn
		// between pipe() and marking both ends close-on-exec.
#ifndef __linux__
		static std::mutex launchMutex;
		std::unique_lock launchLock(launchMutex);
#endif
		int pipes[2] = {-1, -1};
#ifdef __linux__
		if (pipe2(pipes, O_CLOEXEC) != 0) {
#else
		if (pipe(pipes) != 0) {
#endif
			return result;
		}
#ifndef __linux__
		if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipes[1], F_SETFD, FD_CLOEXEC) < 0) {
			close(pipes[0]);
			close(pipes[1]);
			return result;
		}
#endif
		// A caller may have closed stdin/stdout/stderr. Keep the pipe on FDs
		// above them so dup2 never leaves CLOEXEC on captured stdout or closes it.
		for (int &descriptor : pipes) {
			const int moved = MoveAboveStdio(descriptor);
			if (moved < 0) {
				close(pipes[0]);
				close(pipes[1]);
				return result;
			}
			descriptor = moved;
		}

		// posix_spawnp keeps PATH lookup while performing child setup without
		// running library code from this multi-threaded parent after a fork.
		posix_spawn_file_actions_t actions;
		if (posix_spawn_file_actions_init(&actions) != 0) {
			close(pipes[0]);
			close(pipes[1]);
			return result;
		}
		const bool configured = posix_spawn_file_actions_addclose(&actions, pipes[0]) == 0 &&
								posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) == 0 &&
								posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO) == 0 &&
								posix_spawn_file_actions_addclose(&actions, pipes[1]) == 0;
		pid_t child = -1;
		const int spawned =
			configured ? posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ) : -1;
		posix_spawn_file_actions_destroy(&actions);

#ifndef __linux__
		launchLock.unlock();
#endif
		if (spawned != 0) {
			close(pipes[0]);
			close(pipes[1]);
			return result;
		}

		// The parent closes its writer before draining the child's output.
		close(pipes[1]);

		// Read to EOF before waiting. Waiting first deadlocks as soon as the
		// child writes more than a pipe buffer, which for a failing test suite
		// is immediately.
		std::array<char, 4096> buffer{};
		for (;;) {
			const ssize_t read = ::read(pipes[0], buffer.data(), buffer.size());
			if (read > 0) {
				result.Output.append(buffer.data(), static_cast<size_t>(read));
				continue;
			}
			if (read < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		close(pipes[0]);

		int status = 0;
		while (waitpid(child, &status, 0) < 0) {
			if (errno != EINTR) {
				return result;
			}
		}

		result.Started = true;
		if (WIFEXITED(status)) {
			result.ExitCode = WEXITSTATUS(status);
			// 127 is the convention for failed spawn setup or exec. A real
			// program is unlikely to use it, and treating it as "did not start"
			// gives a better message than "failed with 127".
			if (result.ExitCode == 127) {
				result.Started = false;
			}
		} else if (WIFSIGNALED(status)) {
			// A crashed test is a failed test, not a missing one.
			result.ExitCode = 128 + WTERMSIG(status);
		}

		return result;
	}
}
