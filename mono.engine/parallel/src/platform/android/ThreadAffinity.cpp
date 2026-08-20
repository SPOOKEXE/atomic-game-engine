#include "ThreadAffinity.hpp"

#include <fstream>
#include <sched.h>
#include <set>
#include <string>

namespace engine::parallel::platform {

	std::vector<Processor> AvailableProcessors() {
		cpu_set_t allowed;
		CPU_ZERO(&allowed);
		if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
			return {};
		}

		std::vector<Processor> processors;
		for (uint32_t number = 0; number < CPU_SETSIZE; number++) {
			if (CPU_ISSET(static_cast<int>(number), &allowed)) {
				processors.push_back(Processor{0, number});
			}
		}
		return processors;
	}

	std::vector<Processor> DistinctCoreProcessors() {
		std::set<std::string> siblingSets;
		std::vector<Processor> processors;
		for (const Processor processor : AvailableProcessors()) {
			const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(processor.Number) +
									 "/topology/thread_siblings_list";
			std::ifstream topology(path);
			std::string siblings;
			if (!std::getline(topology, siblings) || siblings.empty()) {
				return {};
			}
			if (siblingSets.insert(siblings).second) {
				processors.push_back(processor);
			}
		}
		return processors;
	}

	bool PinCurrentThread(Processor processor) {
		if (!processor.Valid() || processor.Group != 0 || processor.Number >= CPU_SETSIZE) {
			return false;
		}

		cpu_set_t selected;
		CPU_ZERO(&selected);
		CPU_SET(static_cast<int>(processor.Number), &selected);
		return ::sched_setaffinity(0, sizeof(selected), &selected) == 0;
	}

	Processor CurrentProcessor() {
		const int number = ::sched_getcpu();
		return number < 0 ? Processor{} : Processor{0, static_cast<uint32_t>(number)};
	}
}
