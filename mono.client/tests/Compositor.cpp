// N views in, one frame out.
//
// The compositor is the client's side of `world::ViewChannel`, so what is worth
// checking here is only what it adds: several views composed into one list, a
// producer that stalled keeping its last frame, and the placement that stops
// two worlds being drawn inside each other.
//
// The channel's own behaviour — triple buffering, drop counting, the atomic
// publish index — is `world`'s and is tested there.

#include <engine/core/Name.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <client/Compositor.hpp>
#include <vector>

TEST_SUITE_ID("client.compositor")

using client::Compositor;
using engine::core::Name;
using engine::render::Camera;
using engine::render::Instance;
using engine::world::WorldId;

namespace compositor_test {
	// A draw list of `count` instances, each carrying its index in the X
	// translation so a composed list can be read back.
	std::vector<Instance> ListOf(size_t count, float base) {
		std::vector<Instance> list(count);
		for (size_t index = 0; index < count; index++) {
			list[index].Model[3][0] = base + static_cast<float>(index);
		}
		return list;
	}

	Camera At(float x) {
		Camera camera;
		camera.Frame.Position.X = x;
		return camera;
	}
}

using namespace compositor_test;

TEST_CASE("one view composes to what it published", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("one"), 64);

	const std::vector<Instance> list = ListOf(8, 100.0f);
	REQUIRE(views.Publish(WorldId{0}, At(5.0f), list, 3, 0.25f));

	views.Compose(0.0f);

	REQUIRE(views.Instances().size() == 8);
	REQUIRE(views.Instances()[0].Model[3][0] == 100.0f);
	REQUIRE(views.Instances()[7].Model[3][0] == 107.0f);
	REQUIRE(views.Camera().Frame.Position.X == 5.0f);
	REQUIRE(views.Views()[0].Header.SourceTick == 3);
	REQUIRE(views.Views()[0].Header.Alpha == 0.25f);
}

TEST_CASE("several views compose into one list", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 64);
	views.Track(WorldId{1}, Name("b"), 64);
	views.Track(WorldId{2}, Name("c"), 64);
	REQUIRE(views.Count() == 3);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(2, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), ListOf(3, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{2}, At(0.0f), ListOf(4, 0.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 9);
}

TEST_CASE("views after the first are placed rather than overlaid", "[client]") {
	// Two worlds' coordinates do not mean the same thing. Overlaying them would
	// draw two scenes inside each other and call it one.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(1, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), ListOf(1, 0.0f), 1, 0.0f));

	views.Compose(50.0f);

	REQUIRE(views.Instances().size() == 2);
	REQUIRE(views.Instances()[0].Model[3][0] == 0.0f);
	REQUIRE(views.Instances()[1].Model[3][0] == 50.0f);
}

TEST_CASE("zero spacing overlays them, which is what one view wants", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(1, 7.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), ListOf(1, 7.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Instances()[0].Model[3][0] == 7.0f);
	REQUIRE(views.Instances()[1].Model[3][0] == 7.0f);
}

TEST_CASE("a stalled producer keeps its last frame and is counted", "[client]") {
	// A world flickering out of existence for one frame is worse than a world
	// one frame behind. A compositor running faster than its producers is the
	// normal case, not a fault — but a producer that has *stopped* looks
	// identical to a still scene unless somebody counts it.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(2, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), ListOf(3, 0.0f), 1, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 5);
	REQUIRE(views.StaleViews() == 0);

	// Only the first publishes again.
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(2, 0.0f), 2, 0.0f));
	views.Compose(0.0f);

	// Still five: the second view redrew what it had.
	REQUIRE(views.Instances().size() == 5);
	REQUIRE(views.StaleViews() == 1);
	REQUIRE(views.Views()[0].Fresh);
	REQUIRE_FALSE(views.Views()[1].Fresh);
	REQUIRE(views.Views()[1].Stale == 1);

	// And the count climbs while it stays quiet.
	views.Compose(0.0f);
	views.Compose(0.0f);
	REQUIRE(views.Views()[1].Stale == 3);
}

TEST_CASE("a view that never published contributes nothing", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("silent"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(4, 0.0f), 1, 0.0f));
	views.Compose(0.0f);

	REQUIRE(views.Instances().size() == 4);
	REQUIRE(views.Views()[1].Instances == 0);
}

TEST_CASE("an empty draw list is a legal frame", "[client]") {
	// A world with nothing visible in it is not a world that failed to publish,
	// and the difference matters: one is a black screen, the other is a
	// producer to go and look at.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(1.0f), {}, 9, 0.0f));
	views.Compose(0.0f);

	REQUIRE(views.Instances().empty());
	REQUIRE(views.Views()[0].Fresh);
	REQUIRE(views.Views()[0].Header.SourceTick == 9);
}

TEST_CASE("a draw list past the channel's maximum is refused whole", "[client]") {
	// Half a draw list is a frame with holes in it, which reads as a rendering
	// bug rather than as the budget overrun it is.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 4);

	REQUIRE_FALSE(views.Publish(WorldId{0}, At(0.0f), ListOf(64, 0.0f), 1, 0.0f));

	// Still usable: a refused frame is not a broken channel.
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(4, 0.0f), 2, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 4);
}

TEST_CASE("publishing for an untracked world is refused", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);

	REQUIRE_FALSE(views.Publish(WorldId{9}, At(0.0f), ListOf(1, 0.0f), 1, 0.0f));
}

TEST_CASE("tracking the same world twice adds one view", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{0}, Name("a"), 8);

	REQUIRE(views.Count() == 1);
}

TEST_CASE("only the newest frame of several is taken", "[client]") {
	// The channel drops rather than queues, which is what stops a slow
	// compositor from throttling a world. Composing after three publishes has
	// to give the third, not the first.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 32);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(1, 1.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(1, 2.0f), 2, 0.0f));
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), ListOf(1, 3.0f), 3, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Views()[0].Header.SourceTick == 3);
	REQUIRE(views.Instances()[0].Model[3][0] == 3.0f);
	REQUIRE(views.Dropped() == 2);
}

TEST_CASE("the camera comes from the first view", "[client]") {
	// A compositor with one camera and several worlds has to choose. Choosing
	// the first and framing the row is the choice that shows something rather
	// than the first world and a black gap.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(11.0f), ListOf(1, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(99.0f), ListOf(1, 0.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Camera().Frame.Position.X == 11.0f);
}

TEST_CASE("composing many times over many views stays consistent", "[client][fuzz]") {
	// Publishers and a consumer at different rates, which is the case the whole
	// channel exists for. Every compose has to produce exactly the instances of
	// whatever each view last published — never a mixture, never a partial one.
	Compositor views;
	constexpr size_t COUNT = 5;
	for (uint32_t index = 0; index < COUNT; index++) {
		views.Track(WorldId{index}, Name("w" + std::to_string(index)), 64);
	}

	std::vector<size_t> lastPublished(COUNT, 0);

	for (uint32_t round = 0; round < 300; round++) {
		for (uint32_t index = 0; index < COUNT; index++) {
			// A different subset publishes each round, so most views are stale
			// most of the time.
			if ((round + index) % 3 != 0) {
				continue;
			}
			const size_t count = 1 + (round + index) % 17;
			REQUIRE(views.Publish(
				WorldId{index}, At(0.0f), ListOf(count, static_cast<float>(index * 1000)), round, 0.0f
			));
			lastPublished[index] = count;
		}

		views.Compose(0.0f);

		size_t expected = 0;
		for (const size_t count : lastPublished) {
			expected += count;
		}
		REQUIRE(views.Instances().size() == expected);

		// And each view's slice still starts where that view's did, which is
		// what proves nothing was mixed between them.
		size_t at = 0;
		for (uint32_t index = 0; index < COUNT; index++) {
			if (lastPublished[index] == 0) {
				continue;
			}
			REQUIRE(views.Instances()[at].Model[3][0] == static_cast<float>(index * 1000));
			at += lastPublished[index];
		}
	}
}
