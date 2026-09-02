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
		return {};
	}

	bool PinCurrentThread(Processor) {
		return false;
	}

	bool PinCurrentProcess(Processor) {
		return false;
	}

	Processor CurrentProcessor() {
		return {};
	}
}
