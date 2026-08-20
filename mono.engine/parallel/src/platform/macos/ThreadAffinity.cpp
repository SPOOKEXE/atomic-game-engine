#include "ThreadAffinity.hpp"

#include <thread>

namespace engine::parallel::platform {

	std::vector<Processor> AvailableProcessors() {
		const unsigned count = std::thread::hardware_concurrency();
		std::vector<Processor> processors;
		processors.reserve(count);
		for (unsigned number = 0; number < count; number++) {
			processors.push_back(Processor{0, number});
		}
		return processors;
	}

	std::vector<Processor> DistinctCoreProcessors() {
		// Darwin does not expose a binding that can enforce the result, so no
		// topology answer is usable for the assigned-worker guarantee.
		return {};
	}

	bool PinCurrentThread(Processor) {
		// Darwin exposes affinity tags as cache-placement hints, not a binding to
		// a processor. Returning false prevents the scheduler from promising a
		// core guarantee the operating system cannot provide.
		return false;
	}

	Processor CurrentProcessor() {
		return {};
	}
}
