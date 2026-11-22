#pragma once

// Startup-only mailbox. Native compilation runs on one joined worker while the
// window owner pumps events. No renderer state is inspected before that join.

#include <engine/render/Renderer.hpp>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <string>

namespace engine::render {
	struct StartupCompilationCancelled {};

	class StartupCompilation {
	  public:
		void Plan(size_t total) {
			std::lock_guard lock(Mutex);
			Total = total;
		}

		void BeginGroup() {
			std::lock_guard lock(Mutex);
			if (Cancelled) throw StartupCompilationCancelled{};
			if (GroupOpen) ++Completed;
			GroupOpen = true;
		}

		void Operation(std::string_view name) {
			std::lock_guard lock(Mutex);
			if (Cancelled) throw StartupCompilationCancelled{};
			Name = name;
		}

		void Complete() {
			std::lock_guard lock(Mutex);
			if (Cancelled) throw StartupCompilationCancelled{};
			if (GroupOpen) ++Completed;
			GroupOpen = false;
		}

		// Work exceptions cross only after joining. A callback exception also
		// cancels and joins before it propagates to the owner.
		bool Run(const std::function<bool()> &work, const StartupProgressCallback &callback) {
			if (!callback) return work();
			std::exception_ptr failure;
			bool result = false;
			auto worker = std::async(std::launch::async, [&] {
				try {
					result = work();
				} catch (const StartupCompilationCancelled &) {
					result = false;
				} catch (...) {
					// Preserve allocation and backend failures for owner-thread cleanup.
					failure = std::current_exception();
				}
				std::lock_guard lock(Mutex);
				Done = true;
				Changed.notify_one();
			});
			std::exception_ptr callbackFailure;
			bool accepted = true;
			for (;;) {
				std::unique_lock lock(Mutex);
				const auto completed = Completed;
				const auto total = Total;
				const auto name = Name;
				const bool done = Done;
				lock.unlock();
				if (!callbackFailure) {
					try {
						accepted = callback({completed, total, name}) && accepted;
					} catch (...) {
						callbackFailure = std::current_exception();
						accepted = false;
					}
				}
				lock.lock();
				if (!accepted) Cancelled = true;
				if (done) break;
				Changed.wait_for(lock, std::chrono::milliseconds(16), [&] { return Done; });
			}
			worker.get();
			if (callbackFailure) std::rethrow_exception(callbackFailure);
			if (failure) std::rethrow_exception(failure);
			return accepted && result;
		}

	  private:
		std::mutex Mutex;
		std::condition_variable Changed;
		size_t Completed = 0;
		size_t Total = 0;
		std::string Name;
		bool GroupOpen = false;
		bool Cancelled = false;
		bool Done = false;
	};
}
