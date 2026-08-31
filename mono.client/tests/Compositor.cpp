// N views in, one frame out.
//
// The compositor is the client's side of `world::ViewChannel`, so what is worth
// checking here is only what it adds: several views composed into one list, a
// producer that stalled keeping its last frame, and the placement that stops
// two worlds being drawn inside each other.
//
// The channel's own behaviour - triple buffering, drop counting, the atomic
// publish index - is `world`'s and is tested there.

#include <engine/core/Name.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <client/Compositor.hpp>
#include <vector>

TEST_SUITE_ID("client.compositor")
TEST_DEPENDS("engine.scene.drawinstance")

using client::Compositor;
using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::scene::Camera;
using engine::scene::DrawInstance;
using engine::world::WorldId;

namespace compositor_test {
	// A draw list of `count` instances, each carrying its index in the X
	// position so a composed list can be read back.
	std::vector<DrawInstance> ListOf(size_t count, float base) {
		std::vector<DrawInstance> list(count);
		for (size_t index = 0; index < count; index++) {
			list[index].Frame.Position.X = base + static_cast<float>(index);
		}
		return list;
	}

	// A camera placement. The lens is separate from the placement here for the
	// same reason it is in a world: composing moves where a view is taken from
	// and never what it is taken through.
	CFrame At(float x) {
		return CFrame(Vector3{x, 0.0f, 0.0f});
	}

	const Camera LENS;
}

using namespace compositor_test;

TEST_CASE("one view composes to what it published", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("one"), 64);

	const std::vector<DrawInstance> list = ListOf(8, 100.0f);
	REQUIRE(views.Publish(WorldId{0}, At(5.0f), LENS, list, 3, 0.25f));

	views.Compose(0.0f);

	REQUIRE(views.Instances().size() == 8);
	REQUIRE(views.Instances()[0].Frame.Position.X == 100.0f);
	REQUIRE(views.Instances()[7].Frame.Position.X == 107.0f);
	REQUIRE(views.CameraFrame().Position.X == 5.0f);
	REQUIRE(views.Views()[0].Header.SourceTick == 3);
	REQUIRE(views.Views()[0].Header.Alpha == 0.25f);
}

TEST_CASE("several views compose into one list", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 64);
	views.Track(WorldId{1}, Name("b"), 64);
	views.Track(WorldId{2}, Name("c"), 64);
	REQUIRE(views.Count() == 3);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(2, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), LENS, ListOf(3, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{2}, At(0.0f), LENS, ListOf(4, 0.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 9);
	CHECK(views.Instances()[0].SourceWorld == Name("a"));
	CHECK(views.Instances()[2].SourceWorld == Name("b"));
	CHECK(views.Instances()[5].SourceWorld == Name("c"));
}

TEST_CASE("composed skin palettes keep each world's adjusted offset", "[client][skinning]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 4);
	views.Track(WorldId{1}, Name("b"), 4);

	std::vector<DrawInstance> first = ListOf(1, 0.0f);
	first[0].SkinCount = 1;
	std::vector<DrawInstance> second = ListOf(1, 0.0f);
	second[0].SkinCount = 2;
	const std::array firstJoints{At(1.0f)};
	const std::array secondJoints{At(2.0f), At(3.0f)};

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, first, 1, 0.0f, firstJoints));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), LENS, second, 1, 0.0f, secondJoints));
	views.Compose(0.0f);

	REQUIRE(views.Instances().size() == 2);
	CHECK(views.Instances()[0].SkinFirst == 0);
	CHECK(views.Instances()[1].SkinFirst == 1);
	REQUIRE(views.JointFrames().size() == 3);
	CHECK(views.JointFrames()[2].Position.X == 3.0f);
}

TEST_CASE("views after the first are placed rather than overlaid", "[client]") {
	// Two worlds' coordinates do not mean the same thing. Overlaying them would
	// draw two scenes inside each other and call it one.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(1, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), LENS, ListOf(1, 0.0f), 1, 0.0f));

	views.Compose(50.0f);

	REQUIRE(views.Instances().size() == 2);
	REQUIRE(views.Instances()[0].Frame.Position.X == 0.0f);
	REQUIRE(views.Instances()[1].Frame.Position.X == 50.0f);
}

TEST_CASE("zero spacing overlays them, which is what one view wants", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(1, 7.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), LENS, ListOf(1, 7.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Instances()[0].Frame.Position.X == 7.0f);
	REQUIRE(views.Instances()[1].Frame.Position.X == 7.0f);
}

TEST_CASE("a stalled producer keeps its last frame and is counted", "[client]") {
	// A world flickering out of existence for one frame is worse than a world
	// one frame behind. A compositor running faster than its producers is the
	// normal case, not a fault - but a producer that has *stopped* looks
	// identical to a still scene unless somebody counts it.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(2, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(0.0f), LENS, ListOf(3, 0.0f), 1, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 5);
	REQUIRE(views.StaleViews() == 0);

	// Only the first publishes again.
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(2, 0.0f), 2, 0.0f));
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

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(4, 0.0f), 1, 0.0f));
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

	REQUIRE(views.Publish(WorldId{0}, At(1.0f), LENS, {}, 9, 0.0f));
	views.Compose(0.0f);

	REQUIRE(views.Instances().empty());
	REQUIRE(views.Views()[0].Fresh);
	REQUIRE(views.Views()[0].Header.SourceTick == 9);
}

TEST_CASE("a draw list past the channel's size grows it rather than being refused", "[client]") {
	// **The size passed to `Track` is a guess, not a ceiling.** On the client
	// it is `--entities`, which is the demo scene's cube count and says nothing
	// about what a script builds - so a world that outgrew it used to stop
	// being drawn entirely. That reads as a rendering bug and is not one: the
	// draw list was fine and there was nowhere to put it.
	//
	// Truncating was never the alternative. Half a draw list is a frame with
	// holes in it, and a hole looks like a bug in whatever was meant to fill
	// it.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 4);

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(64, 0.0f), 1, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 64);

	// And it keeps working at the smaller size afterwards: growing is a
	// ceiling moving, not a channel reshaping itself around one frame.
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(4, 0.0f), 2, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 4);
}

TEST_CASE("growth is in steps, so a world that settles stops growing", "[client]") {
	// A channel reserved at exactly what this frame needed would allocate
	// again on the next frame that added one instance - which is the
	// per-frame allocation the reservation exists to avoid.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 4);

	// Past the start, so this one grows.
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(500, 0.0f), 1, 0.0f));
	views.Compose(0.0f);
	REQUIRE(views.Instances().size() == 500);

	// Inside the step it grew to. A frame that fits publishes without the
	// channel moving again.
	for (uint64_t tick = 2; tick < 8; tick++) {
		REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(500 + tick, 0.0f), tick, 0.0f));
		views.Compose(0.0f);
		REQUIRE(views.Instances().size() == 500 + tick);
	}
}

TEST_CASE("publishing for an untracked world is refused", "[client]") {
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);

	REQUIRE_FALSE(views.Publish(WorldId{9}, At(0.0f), LENS, ListOf(1, 0.0f), 1, 0.0f));
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

	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(1, 1.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(1, 2.0f), 2, 0.0f));
	REQUIRE(views.Publish(WorldId{0}, At(0.0f), LENS, ListOf(1, 3.0f), 3, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.Views()[0].Header.SourceTick == 3);
	REQUIRE(views.Instances()[0].Frame.Position.X == 3.0f);
	REQUIRE(views.Dropped() == 2);
}

TEST_CASE("the camera comes from the first view", "[client]") {
	// A compositor with one camera and several worlds has to choose. Choosing
	// the first and framing the row is the choice that shows something rather
	// than the first world and a black gap.
	Compositor views;
	views.Track(WorldId{0}, Name("a"), 8);
	views.Track(WorldId{1}, Name("b"), 8);

	REQUIRE(views.Publish(WorldId{0}, At(11.0f), LENS, ListOf(1, 0.0f), 1, 0.0f));
	REQUIRE(views.Publish(WorldId{1}, At(99.0f), LENS, ListOf(1, 0.0f), 1, 0.0f));

	views.Compose(0.0f);
	REQUIRE(views.CameraFrame().Position.X == 11.0f);
}

TEST_CASE("composing many times over many views stays consistent", "[client][fuzz]") {
	// Publishers and a consumer at different rates, which is the case the whole
	// channel exists for. Every compose has to produce exactly the instances of
	// whatever each view last published - never a mixture, never a partial one.
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
				WorldId{index}, At(0.0f), LENS, ListOf(count, static_cast<float>(index * 1000)), round, 0.0f
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
			REQUIRE(views.Instances()[at].Frame.Position.X == static_cast<float>(index * 1000));
			at += lastPublished[index];
		}
	}
}
