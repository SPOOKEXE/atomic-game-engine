// Real-device proof that startup progress joins native compilation before
// cleanup, and cancellation leaves the same renderer usable for a fresh retry.

#include "RenderFixture.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <thread>

TEST_SUITE_ID("engine.render.startupcompilationgpu")
TEST_DEPENDS("engine.render.startupcompilation")

namespace {
	using namespace engine::render;

	struct ProgressObservation {
		std::thread::id Owner = std::this_thread::get_id();
		size_t Total = 0;
		size_t Completed = 0;
		bool OwnerCallbacks = true;
		bool StableTotal = true;
		bool Ordered = true;
		bool ActiveOperation = false;
		bool Completion = false;
		bool Setup = false;
		bool Finishing = false;

		void Observe(const StartupProgress &progress) {
			OwnerCallbacks &= std::this_thread::get_id() == Owner;
			if (progress.Total == 0) {
				Setup = true;
				Finishing |= Completion;
				return;
			}
			if (Total == 0) Total = progress.Total;
			StableTotal &= Total == progress.Total;
			Ordered &= progress.Completed >= Completed && progress.Completed <= progress.Total;
			Completed = progress.Completed;
			ActiveOperation |= !progress.ShaderName.empty() && progress.Completed < progress.Total;
			Completion |= progress.Completed == progress.Total;
		}
	};

	void CheckProgress(const ProgressObservation &observed) {
		CHECK(observed.OwnerCallbacks);
		CHECK(observed.StableTotal);
		CHECK(observed.Ordered);
		CHECK(observed.Total > 0);
		CHECK(observed.Setup);
	}

	void CheckReleased(const Renderer &renderer) {
		CHECK(renderer.Backend().Device == nullptr);
		const auto memory = renderer.MemoryStatistics();
		CHECK(memory.LiveBytes == 0);
		CHECK(memory.Buffers == 0);
		CHECK(memory.TransferBuffers == 0);
		CHECK(memory.Textures == 0);
	}
}

TEST_CASE(
	"native startup reports owner-thread progress and releases its device", "[render][gpu][startup][.]"
) {
	engine::render::test::FixtureDevice fixture;
	REQUIRE(fixture.VideoReady);
	ProgressObservation observed;
	const bool ready = fixture.Render.Initialise(nullptr, 1, false, 0, [&](const StartupProgress &progress) {
		observed.Observe(progress);
		return true;
	});
	INFO(SDL_GetError());
	REQUIRE(ready);
	CheckProgress(observed);
	CHECK(observed.Completion);
	CHECK(observed.Finishing);
	CHECK(observed.Completed == observed.Total);
	CHECK(fixture.Render.Backend().Device != nullptr);
	CHECK(fixture.Render.MemoryStatistics().LiveBytes > 0);
	fixture.Render.Shutdown();
	CheckReleased(fixture.Render);
}

TEST_CASE(
	"native startup cancellation releases partial pipelines before a same-renderer retry",
	"[render][gpu][startup][.]"
) {
	engine::render::test::FixtureDevice fixture;
	REQUIRE(fixture.VideoReady);
	ProgressObservation cancelled;
	bool requestedCancellation = false;
	bool cancelledWhileActive = false;
	const bool ready = fixture.Render.Initialise(nullptr, 1, false, 0, [&](const StartupProgress &progress) {
		cancelled.Observe(progress);
		// Cached startup can finish before the first planned progress snapshot.
		// Cancellation still owns cleanup even when native compilation already ended.
		if (!requestedCancellation && progress.Total > 0) {
			cancelledWhileActive = progress.Completed < progress.Total;
			requestedCancellation = true;
		}
		return !requestedCancellation;
	});
	REQUIRE(requestedCancellation);
	CHECK_FALSE(ready);
	CheckProgress(cancelled);
	if (cancelledWhileActive) CHECK_FALSE(cancelled.Completion);
	CheckReleased(fixture.Render);

	ProgressObservation retried;
	const bool retryReady =
		fixture.Render.Initialise(nullptr, 1, false, 0, [&](const StartupProgress &progress) {
			retried.Observe(progress);
			return true;
		});
	INFO(SDL_GetError());
	REQUIRE(retryReady);
	CheckProgress(retried);
	CHECK(retried.Completion);
	CHECK(retried.Finishing);
	CHECK(retried.Completed == retried.Total);
	CHECK(fixture.Render.Backend().Device != nullptr);
	CHECK(fixture.Render.MemoryStatistics().LiveBytes > 0);
	fixture.Render.Shutdown();
	CheckReleased(fixture.Render);
}
