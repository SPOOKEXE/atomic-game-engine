#pragma once

#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <thread>

namespace engine::physics::testing {
	// Keep a caller's existing pool intact while enabling the parallel path in standalone runs.
	struct JobPoolFixture {
		const bool OwnsPool = engine::parallel::Jobs::WorkerCount() == 0;

		JobPoolFixture() {
			if (!OwnsPool) return;
			// Three workers cut the combined dev cell-size checks from 516 s to 336 s
			// on 24 logical processors. Leave a processor for the caller on smaller hosts.
			constexpr unsigned MAXIMUM_WORKERS = 3;
			const unsigned processors = std::thread::hardware_concurrency();
			const unsigned workers = processors > 1 ? std::min(MAXIMUM_WORKERS, processors - 1) : 1;
			engine::parallel::Jobs::Start(workers);
		}

		~JobPoolFixture() {
			if (OwnsPool) engine::parallel::Jobs::Stop();
		}

		JobPoolFixture(const JobPoolFixture &) = delete;
		JobPoolFixture &operator=(const JobPoolFixture &) = delete;
	};

}
