#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>

#include <launcher/Supervisor.hpp>

namespace launcher {

	bool Supervisor::Start(
		const std::filesystem::path &program, const std::vector<std::string> &arguments, std::string &failure
	) {
		// **Whatever was here is stopped first.** `Process::operator=` reaps
		// what it held, but it kills rather than asks - so a supervised server
		// being replaced gets the chance to close its sockets, which is the
		// difference between a port that is free on the next Start and one that
		// is in TIME_WAIT.
		if (Running()) {
			Child.RequestStop();
			Child.Wait();
		}

		Started = program;
		Arguments = arguments;
		Elapsed = 0.0;

		if (!Child.Start(program, arguments)) {
			failure = "could not start " + program.string();
			Current = ChildState::Failed;
			Line = failure;
			ENGINE_ERROR("launcher: {}", failure);
			return false;
		}

		StartedAt = engine::core::Clock::Seconds();
		Current = ChildState::Running;
		Line = "running as pid " + std::to_string(Child.Id());
		ENGINE_INFO("launcher: started {} as pid {}", program.string(), Child.Id());
		return true;
	}

	bool Supervisor::Restart(std::string &failure) {
		if (Started.empty()) {
			failure = "nothing has been started yet";
			return false;
		}
		return Start(Started, Arguments, failure);
	}

	void Supervisor::Poll(double now) {
		if (Current != ChildState::Running) {
			return;
		}

		Elapsed = now - StartedAt;

		const engine::parallel::ProcessStatus status = Child.Poll();
		if (status.Alive()) {
			return;
		}

		switch (status.Reason) {
		case engine::parallel::ExitReason::Exited:
			Current = status.Code == 0 ? ChildState::Ended : ChildState::Failed;
			Line = "exited " + std::to_string(status.Code);
			break;

		case engine::parallel::ExitReason::Signalled:
			// A signal death is a hard fault - an abort, a segfault or the
			// out-of-memory killer - and `Process.hpp` says so. Naming the
			// signal is what turns "it closed" into something searchable.
			Current = ChildState::Failed;
			Line = "killed by signal " + std::to_string(status.Signal);
			break;

		case engine::parallel::ExitReason::Running:
		case engine::parallel::ExitReason::Gone:
			Current = ChildState::Ended;
			Line = engine::parallel::Describe(status.Reason);
			break;
		}

		ENGINE_INFO("launcher: {} {} after {:.1f}s", Started.string(), Line, Elapsed);
	}

	void Supervisor::RequestStop() {
		if (Running()) {
			ENGINE_INFO("launcher: asking pid {} to stop", Child.Id());
			Child.RequestStop();
		}
	}

	void Supervisor::Kill() {
		if (Running()) {
			ENGINE_WARN("launcher: killing pid {}", Child.Id());
			Child.Kill();
		}
	}

	void Supervisor::Clear() {
		if (Running()) {
			return;
		}
		Current = ChildState::Idle;
		Line.clear();
		Elapsed = 0.0;
	}
}
