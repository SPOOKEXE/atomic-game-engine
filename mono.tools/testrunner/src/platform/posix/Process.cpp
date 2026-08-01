#include <testrunner/Process.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

namespace testrunner {

	ProcessResult Run(const std::vector<std::string> &arguments) {
		ProcessResult result;
		if (arguments.empty()) {
			return result;
		}

		int pipes[2] = { -1, -1 };
		if (pipe(pipes) != 0) {
			return result;
		}

		const pid_t child = fork();
		if (child < 0) {
			close(pipes[0]);
			close(pipes[1]);
			return result;
		}

		if (child == 0) {
			// The child. Nothing between fork and execvp may allocate or take a
			// lock: in a multi-threaded parent, only the forking thread
			// survives, and a lock another thread happened to hold is held
			// forever. Everything below is a syscall.
			close(pipes[0]);
			dup2(pipes[1], STDOUT_FILENO);
			dup2(pipes[1], STDERR_FILENO);
			close(pipes[1]);

			// Built before the fork, for the reason above.
			std::vector<char *> argv;
			argv.reserve(arguments.size() + 1);
			for (const auto &argument : arguments) {
				argv.push_back(const_cast<char *>(argument.c_str()));
			}
			argv.push_back(nullptr);

			execvp(argv[0], argv.data());

			// Only reachable if exec failed. _exit rather than exit: the
			// child shares the parent's atexit handlers and stdio buffers, and
			// running them here would flush the parent's buffers twice.
			_exit(127);
		}

		close(pipes[1]);

		// Read to EOF before waiting. Waiting first deadlocks as soon as the
		// child writes more than a pipe buffer, which for a failing test suite
		// is immediately.
		std::array<char, 4096> buffer {};
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
			// 127 is the convention execvp failure exits with above. A real
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
