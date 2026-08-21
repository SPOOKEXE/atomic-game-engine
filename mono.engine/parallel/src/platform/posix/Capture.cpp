#include <engine/parallel/Capture.hpp>

#include <array>
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>

namespace engine::parallel {

	CaptureResult Capture(const std::vector<std::string> &arguments) {
		CaptureResult result;
		if (arguments.empty()) {
			return result;
		}

		int pipes[2] = {-1, -1};
		if (::pipe(pipes) != 0) {
			return result;
		}

		// **`fork` here where `Process::Start` uses `posix_spawn`, and the
		// difference is the pipe.** `posix_spawn` can redirect a descriptor
		// through file actions, but the child also has to close the read end it
		// inherited, and a spawn action list cannot close a descriptor it was
		// not told about by number in a way that stays correct as the actions
		// run. The window this opens is the one `Process.cpp` warns about - the
		// gap between fork and exec in a threaded process - so everything below
		// is a syscall and nothing allocates or takes a lock.
		std::vector<char *> argv;
		argv.reserve(arguments.size() + 1);
		for (const auto &argument : arguments) {
			argv.push_back(const_cast<char *>(argument.c_str()));
		}
		argv.push_back(nullptr);

		const pid_t child = ::fork();
		if (child < 0) {
			::close(pipes[0]);
			::close(pipes[1]);
			return result;
		}

		if (child == 0) {
			::close(pipes[0]);
			::dup2(pipes[1], STDOUT_FILENO);
			::dup2(pipes[1], STDERR_FILENO);
			::close(pipes[1]);

			::execvp(argv[0], argv.data());

			// Only reachable if exec failed. `_exit` rather than `exit`: the
			// child shares the parent's atexit handlers and stdio buffers, and
			// running them here would flush the parent's buffers twice.
			::_exit(127);
		}

		::close(pipes[1]);

		// **Read to EOF before waiting.** Waiting first deadlocks as soon as the
		// child writes more than a pipe buffer, and a settings table from a
		// program with forty options is well over one.
		std::array<char, 4096> buffer{};
		for (;;) {
			const ssize_t taken = ::read(pipes[0], buffer.data(), buffer.size());
			if (taken > 0) {
				result.Output.append(buffer.data(), static_cast<size_t>(taken));
				continue;
			}
			if (taken < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		::close(pipes[0]);

		int status = 0;
		while (::waitpid(child, &status, 0) < 0) {
			if (errno != EINTR) {
				return result;
			}
		}

		result.Started = true;
		if (WIFEXITED(status)) {
			result.ExitCode = WEXITSTATUS(status);

			// 127 is the convention the failed `execvp` above exits with. A
			// real program is unlikely to use it, and treating it as "did not
			// start" gives a better message than "failed with 127".
			if (result.ExitCode == 127) {
				result.Started = false;
			}
		} else if (WIFSIGNALED(status)) {
			result.ExitCode = 128 + WTERMSIG(status);
		}

		return result;
	}
}
