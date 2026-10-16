#include "StartupCompilation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <semaphore>
#include <stdexcept>

TEST_SUITE_ID("engine.render.startupcompilation")

using namespace engine::render;

TEST_CASE("startup compilation keeps callbacks on the owner while native work blocks", "[render][startup]") {
	StartupCompilation compilation;
	const auto owner = std::this_thread::get_id();
	std::atomic_bool workerIsSeparate = false;
	std::binary_semaphore release(0);
	size_t heartbeats = 0;
	size_t lastCompleted = 0;
	bool allCallbacksOnOwner = true;
	bool stableTotal = true;
	bool ordered = true;
	const bool ready = compilation.Run(
		[&] {
			workerIsSeparate = std::this_thread::get_id() != owner;
			compilation.Plan(2);
			compilation.BeginGroup();
			compilation.Operation("opaque.frag");
			if (!release.try_acquire_for(std::chrono::seconds(2))) return false;
			compilation.Operation("OpaquePipeline two-sided");
			compilation.BeginGroup();
			compilation.Operation("weather-clouds.comp");
			compilation.Complete();
			return true;
		},
		[&](const StartupProgress &progress) {
			allCallbacksOnOwner &= std::this_thread::get_id() == owner;
			stableTotal &= progress.Total == 0 || progress.Total == 2;
			ordered &= progress.Completed >= lastCompleted && progress.Completed <= progress.Total;
			lastCompleted = progress.Completed;
			if (progress.ShaderName == "opaque.frag" && ++heartbeats == 3) release.release();
			return true;
		}
	);
	CHECK(ready);
	CHECK(workerIsSeparate);
	CHECK(allCallbacksOnOwner);
	CHECK(stableTotal);
	CHECK(ordered);
	CHECK(heartbeats >= 3);
	CHECK(lastCompleted == 2);
}

TEST_CASE(
	"startup cancellation joins after the current operation and never reports completion", "[render][startup]"
) {
	StartupCompilation compilation;
	std::binary_semaphore release(0);
	bool reachedNextOperation = false;
	bool workerFinished = false;
	size_t lastCompleted = 0;
	bool cancelRequested = false;
	bool releasedAfterCancellation = false;
	const bool ready = compilation.Run(
		[&] {
			struct Finished {
				bool &Flag;
				~Finished() {
					Flag = true;
				}
			} finished{workerFinished};
			compilation.Plan(1);
			compilation.BeginGroup();
			compilation.Operation("opaque.frag");
			if (!release.try_acquire_for(std::chrono::seconds(2))) return false;
			compilation.Operation("OpaquePipeline");
			reachedNextOperation = true;
			compilation.Complete();
			return true;
		},
		[&](const StartupProgress &progress) {
			lastCompleted = progress.Completed;
			if (progress.ShaderName.empty()) return true;
			if (!cancelRequested) {
				cancelRequested = true;
				return false;
			}
			// A second pump means the first callback's cancellation is published.
			if (!releasedAfterCancellation) release.release();
			releasedAfterCancellation = true;
			return false;
		}
	);
	CHECK_FALSE(ready);
	CHECK(workerFinished);
	CHECK(releasedAfterCancellation);
	CHECK_FALSE(reachedNextOperation);
	CHECK(lastCompleted == 0);
}

TEST_CASE("startup failures join and fresh attempts start with empty progress", "[render][startup]") {
	StartupCompilation failed;
	bool workerFinished = false;
	CHECK_THROWS_AS(
		failed.Run(
			[&]() -> bool {
				struct Finished {
					bool &Flag;
					~Finished() {
						Flag = true;
					}
				} finished{workerFinished};
				throw std::runtime_error("compile failed");
			},
			[](const auto &) { return true; }
		),
		std::runtime_error
	);
	CHECK(workerFinished);

	StartupCompilation retry;
	CHECK(retry.Run(
		[&] {
			retry.Plan(1);
			retry.BeginGroup();
			retry.Operation("retry.frag");
			retry.Complete();
			return true;
		},
		[](const StartupProgress &progress) { return progress.Completed <= progress.Total; }
	));
}

TEST_CASE("startup callback exceptions cancel and join before propagating", "[render][startup]") {
	StartupCompilation compilation;
	std::atomic_bool workerFinished = false;
	CHECK_THROWS_AS(
		compilation.Run(
			[&] {
				struct Finished {
					std::atomic_bool &Flag;
					~Finished() {
						Flag = true;
					}
				} finished{workerFinished};
				for (;;) {
					compilation.Operation("blocked.frag");
					std::this_thread::yield();
				}
				return true;
			},
			[](const auto &) -> bool { throw std::runtime_error("popup failed"); }
		),
		std::runtime_error
	);
	CHECK(workerFinished);
}

TEST_CASE("startup without a progress callback stays synchronous", "[render][startup]") {
	StartupCompilation compilation;
	const auto owner = std::this_thread::get_id();
	CHECK(compilation.Run([&] { return std::this_thread::get_id() == owner; }, {}));
}
