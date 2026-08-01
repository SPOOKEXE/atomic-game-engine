#include <engine/core/Log.hpp>
#include <engine/parallel/Jobs.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace engine::parallel {

	namespace {

		// One batch in flight at a time. The pool exists to make a single
		// parallel-for wide, not to overlap several of them — nesting For()
		// inside For() would deadlock a fixed pool, so the design refuses the
		// shape rather than defending against it.
		struct Batch {
			const std::function<void(size_t, size_t)> *Body = nullptr;
			size_t Count = 0;
			size_t Grain = 0;
			std::atomic<size_t> Next { 0 };
			std::atomic<size_t> Outstanding { 0 };

			std::mutex FailureGuard;
			std::exception_ptr Failure;
		};

		struct Pool {
			std::vector<std::thread> Workers;
			std::mutex Guard;
			std::condition_variable Available;
			std::condition_variable Finished;

			Batch *Current = nullptr;
			uint64_t Generation = 0;
			bool Stopping = false;
		};

		Pool &Get() {
			static Pool pool;
			return pool;
		}

		// Claims ranges until there are none left. Run by the workers and by
		// the calling thread, which is why the caller never idles.
		void Drain(Batch &batch) {
			for (;;) {
				const size_t begin = batch.Next.fetch_add(batch.Grain, std::memory_order_relaxed);
				if (begin >= batch.Count) {
					return;
				}

				const size_t end = std::min(begin + batch.Grain, batch.Count);
				try {
					(*batch.Body)(begin, end);
				} catch (...) {
					std::lock_guard lock(batch.FailureGuard);
					if (!batch.Failure) {
						batch.Failure = std::current_exception();
					}
				}
			}
		}

		void WorkerLoop() {
			auto &pool = Get();
			uint64_t seen = 0;

			for (;;) {
				Batch *batch = nullptr;
				{
					std::unique_lock lock(pool.Guard);
					pool.Available.wait(lock, [&] {
						return pool.Stopping || pool.Generation != seen;
					});
					if (pool.Stopping) {
						return;
					}
					seen = pool.Generation;
					batch = pool.Current;
				}

				if (batch) {
					Drain(*batch);

					std::lock_guard lock(pool.Guard);
					if (batch->Outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
						pool.Finished.notify_all();
					}
				}
			}
		}
	}

	void Jobs::Start(unsigned workers) {
		auto &pool = Get();
		std::lock_guard lock(pool.Guard);

		if (!pool.Workers.empty()) {
			return;
		}

		if (workers == 0) {
			const unsigned available = std::thread::hardware_concurrency();
			// The calling thread participates, so one fewer worker keeps the
			// total at the core count rather than one over it.
			workers = available > 1 ? available - 1 : 0;
		}

		pool.Stopping = false;
		pool.Workers.reserve(workers);
		for (unsigned index = 0; index < workers; index++) {
			pool.Workers.emplace_back(WorkerLoop);
		}

		ENGINE_INFO("job system started with {} worker(s)", workers);
	}

	void Jobs::Stop() {
		auto &pool = Get();
		{
			std::lock_guard lock(pool.Guard);
			if (pool.Workers.empty()) {
				return;
			}
			pool.Stopping = true;
		}
		pool.Available.notify_all();

		for (auto &worker : pool.Workers) {
			worker.join();
		}
		pool.Workers.clear();
	}

	unsigned Jobs::WorkerCount() {
		auto &pool = Get();
		std::lock_guard lock(pool.Guard);
		return static_cast<unsigned>(pool.Workers.size());
	}

	void Jobs::For(size_t count, size_t grain, const std::function<void(size_t, size_t)> &body) {
		if (count == 0) {
			return;
		}
		if (grain == 0) {
			grain = DEFAULT_GRAIN;
		}

		auto &pool = Get();

		// Inline when there is nobody to hand work to, or too little work to be
		// worth the handover. Both cases are the common one in a small scene,
		// and taking the lock at all would be the dominant cost.
		size_t workers = 0;
		{
			std::lock_guard lock(pool.Guard);
			workers = pool.Workers.size();
		}
		if (workers == 0 || count <= grain) {
			body(0, count);
			return;
		}

		Batch batch;
		batch.Body = &body;
		batch.Count = count;
		batch.Grain = grain;
		batch.Outstanding.store(workers, std::memory_order_relaxed);

		{
			std::lock_guard lock(pool.Guard);
			pool.Current = &batch;
			pool.Generation++;
		}
		pool.Available.notify_all();

		Drain(batch);

		{
			std::unique_lock lock(pool.Guard);
			pool.Finished.wait(lock, [&] {
				return batch.Outstanding.load(std::memory_order_acquire) == 0;
			});
			pool.Current = nullptr;
		}

		if (batch.Failure) {
			std::rethrow_exception(batch.Failure);
		}
	}
}
