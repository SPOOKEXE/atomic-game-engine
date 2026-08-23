// The presentation layers are retained and invalidated independently.

#include <engine/render/PresentationDamage.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.presentation-damage")

using engine::render::PresentationDamage;
using engine::render::PresentationDamageTracker;
using engine::render::PresentationSignatures;

namespace {
	PresentationSignatures Settled() {
		return {
			.Scene = 11,
			.GameInterface = 22,
			.HostInterface = 33,
			.Viewport = 44,
		};
	}
}

TEST_CASE("the first presentation builds every retained layer", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	const PresentationDamage damage = tracker.Inspect(Settled());

	CHECK(damage.Scene);
	CHECK(damage.GameInterface);
	CHECK(damage.HostInterface);
	CHECK(damage.Viewport);
	CHECK(damage.SceneImage());
	CHECK(damage.Any());
}

TEST_CASE("an unchanged presentation schedules no work", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());

	const PresentationDamage damage = tracker.Inspect(Settled());
	CHECK_FALSE(damage.Scene);
	CHECK_FALSE(damage.GameInterface);
	CHECK_FALSE(damage.HostInterface);
	CHECK_FALSE(damage.Viewport);
	CHECK_FALSE(damage.SceneImage());
	CHECK_FALSE(damage.Any());
}

TEST_CASE("scene pixels invalidate only the scene image", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.Scene++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK(damage.Scene);
	CHECK_FALSE(damage.GameInterface);
	CHECK_FALSE(damage.HostInterface);
	CHECK_FALSE(damage.Viewport);
	CHECK(damage.SceneImage());
}

TEST_CASE("game UI has a signature separate from its scene", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.GameInterface++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK_FALSE(damage.Scene);
	CHECK(damage.GameInterface);
	CHECK_FALSE(damage.HostInterface);
	CHECK_FALSE(damage.Viewport);
	CHECK(damage.SceneImage());
}

TEST_CASE("host UI does not invalidate the game image", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.HostInterface++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK_FALSE(damage.Scene);
	CHECK_FALSE(damage.GameInterface);
	CHECK(damage.HostInterface);
	CHECK_FALSE(damage.Viewport);
	CHECK_FALSE(damage.SceneImage());
}

TEST_CASE(
	"resizing a viewport rebuilds every layer that uses its geometry", "[render][presentation][damage]"
) {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.Viewport++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK(damage.Scene);
	CHECK(damage.GameInterface);
	CHECK(damage.HostInterface);
	CHECK(damage.Viewport);
}

TEST_CASE("failed presentation does not consume its damage", "[render][presentation][damage]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.Scene++;

	CHECK(tracker.Inspect(changed).Scene);
	CHECK(tracker.Inspect(changed).Scene);

	tracker.Commit(changed);
	CHECK_FALSE(tracker.Inspect(changed).Any());
}
