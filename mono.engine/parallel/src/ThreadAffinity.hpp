#pragma once

// The operating-system part of the job pool's worker placement.
//
// Private to `parallel`: callers dispatch to worker indices and never need to
// know how an operating system names a processor.

#include <cstdint>
#include <limits>
#include <vector>

namespace engine::parallel::platform {

	struct Processor {
		static constexpr uint32_t INVALID = std::numeric_limits<uint32_t>::max();

		uint32_t Group = INVALID;
		uint32_t Number = INVALID;

		bool Valid() const {
			return Group != INVALID && Number != INVALID;
		}

		friend bool operator==(Processor, Processor) = default;
	};

	// Processors this process is allowed to run on, in stable operating-system
	// order. Empty means the platform could not provide a usable answer.
	std::vector<Processor> AvailableProcessors();

	// One allowed logical processor from each physical core. Empty means the
	// platform could not prove which processors share a core.
	std::vector<Processor> DistinctCoreProcessors();

	// Restricts the calling thread to one processor.
	bool PinCurrentThread(Processor processor);

	// The processor currently executing this thread, or an invalid value when
	// the platform cannot report it.
	Processor CurrentProcessor();
}
