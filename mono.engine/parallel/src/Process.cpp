#include <engine/parallel/Process.hpp>

#include <algorithm>
#include <thread>

namespace engine::parallel {

	const char *Describe(ExitReason reason) {
		switch (reason) {
		case ExitReason::Running:
			return "running";
		case ExitReason::Exited:
			return "exited";
		case ExitReason::Signalled:
			return "signalled";
		case ExitReason::Gone:
			return "gone";
		}
		return "?";
	}

	unsigned WorkersPerHost(unsigned hosts) {
		if (hosts == 0) {
			hosts = 1;
		}

		const unsigned available = std::thread::hardware_concurrency();
		if (available <= 1) {
			return 0;
		}

		// Integer division deliberately rounds down: a host that took its share
		// rounded up would, multiplied by the host count, oversubscribe the very
		// machine this is dividing.
		const unsigned share = available / hosts;

		// One fewer, because a host's calling thread drains its own batches.
		// `Jobs::Start` makes the same subtraction for the same reason.
		return share <= 1 ? 0 : share - 1;
	}
}
