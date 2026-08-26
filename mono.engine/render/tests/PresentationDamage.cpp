// The presentation layers are retained and invalidated independently.

#include <engine/render/PresentationDamage.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.render.presentation-damage")

using engine::render::ParticleLayerVisibility;
using engine::render::PresentationCacheApplicability;
using engine::render::PresentationCacheLayer;
using engine::render::PresentationDamage;
using engine::render::PresentationDamageTracker;
using engine::render::PresentationSignatures;

namespace {
	PresentationSignatures Settled() {
		return {
			.Scene = {.Objects = 11, .Particles = 12, .Environment = 13, .Portals = 14},
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
	changed.Scene.Objects++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK(damage.Scene);
	CHECK(damage.Objects);
	CHECK_FALSE(damage.Particles);
	CHECK_FALSE(damage.Environment);
	CHECK_FALSE(damage.Portals);
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
	changed.Scene.Objects++;

	CHECK(tracker.Inspect(changed).Scene);
	CHECK(tracker.Inspect(changed).Scene);

	tracker.Commit(changed);
	CHECK_FALSE(tracker.Inspect(changed).Any());
}

TEST_CASE(
	"scene source causes remain separate before their image cascades", "[render][presentation][cache]"
) {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationSignatures changed = Settled();
	changed.Scene.Environment++;

	const PresentationDamage damage = tracker.Inspect(changed);
	CHECK(damage.Scene);
	CHECK_FALSE(damage.Objects);
	CHECK_FALSE(damage.Particles);
	CHECK(damage.Environment);
	CHECK_FALSE(damage.Portals);
}

TEST_CASE("cache accounting follows the retained presentation cascade", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());

	PresentationDamage damage = tracker.Inspect(Settled());
	tracker.CacheProfile().Record(damage, true);
	damage.GameInterface = true;
	tracker.CacheProfile().Record(damage, true);

	const auto activities = tracker.CacheProfile().Activities();
	const auto row = [&](PresentationCacheLayer layer) -> const engine::render::PresentationCacheActivity & {
		return activities[static_cast<size_t>(layer)];
	};

	CHECK(row(PresentationCacheLayer::Objects).Hits == 2);
	CHECK(row(PresentationCacheLayer::Objects).Writes == 0);
	CHECK(row(PresentationCacheLayer::SceneImage).Hits == 2);
	CHECK(row(PresentationCacheLayer::GameInterface).Hits == 1);
	CHECK(row(PresentationCacheLayer::GameInterface).Writes == 1);
	CHECK(row(PresentationCacheLayer::ViewportGeometry).Hits == 2);
	CHECK(row(PresentationCacheLayer::ViewportOverlay).Hits == 2);
	CHECK(row(PresentationCacheLayer::GameComposition).Writes == 1);
	CHECK(row(PresentationCacheLayer::StudioComposition).Writes == 1);
	CHECK(row(PresentationCacheLayer::FinalImage).Writes == 1);
}

TEST_CASE("viewport causes stay visible in cache accounting", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationDamage damage = tracker.Inspect(Settled());
	damage.Viewport = true;
	damage.Overlay = true;
	tracker.CacheProfile().Record(damage, true);

	const auto activities = tracker.CacheProfile().Activities();
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::ViewportGeometry)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::ViewportOverlay)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::GameComposition)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::FinalImage)].Wrote());
}

TEST_CASE("client cache accounting omits the Studio composition", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());
	PresentationDamage damage = tracker.Inspect(Settled());
	damage.HostInterface = true;
	tracker.CacheProfile().Record(damage, false);

	const auto activities = tracker.CacheProfile().Activities();
	CHECK_FALSE(activities[static_cast<size_t>(PresentationCacheLayer::FinalImage)].Wrote());
	CHECK_FALSE(activities[static_cast<size_t>(PresentationCacheLayer::StudioComposition)].Wrote());
}

TEST_CASE(
	"portal inputs and portal history writes are reported separately", "[render][presentation][cache]"
) {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());

	PresentationDamage damage = tracker.Inspect(Settled());
	damage.Portals = true;
	damage.Scene = true;
	tracker.CacheProfile().Record(damage, true, false);

	auto activities = tracker.CacheProfile().Activities();
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::PortalInputs)].Wrote());
	CHECK_FALSE(activities[static_cast<size_t>(PresentationCacheLayer::PortalHistory)].Wrote());

	tracker.CacheProfile().Record(PresentationDamage{}, true, true);
	activities = tracker.CacheProfile().Activities();
	CHECK_FALSE(activities[static_cast<size_t>(PresentationCacheLayer::PortalInputs)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::PortalHistory)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::SceneImage)].Wrote());
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::FinalImage)].Wrote());
}

TEST_CASE("absent portal layers are not reported as cache hits", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	PresentationCacheApplicability applicable;
	applicable.Portals = false;

	tracker.CacheProfile().Record(PresentationDamage{}, true, false, applicable);

	const auto activities = tracker.CacheProfile().Activities();
	for (const PresentationCacheLayer layer :
		 {PresentationCacheLayer::PortalInputs, PresentationCacheLayer::PortalHistory}) {
		const auto &activity = activities[static_cast<size_t>(layer)];
		CHECK(activity.Hits == 0);
		CHECK(activity.Writes == 0);
		CHECK(activity.Last == engine::render::PresentationCacheActivity::Decision::NotApplicable);
	}
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::SceneImage)].Hits == 1);
}

TEST_CASE("absent source layers never inflate hit rates", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	PresentationCacheApplicability applicable;
	applicable.Objects = false;
	applicable.Particles = false;
	applicable.Environment = false;
	applicable.Portals = false;
	applicable.GameInterface = false;
	applicable.HostInterface = false;

	for (size_t frame = 0; frame < 128; frame++) {
		tracker.CacheProfile().Record(PresentationDamage{}, true, false, applicable);
	}

	const auto activities = tracker.CacheProfile().Activities();
	for (const PresentationCacheLayer layer : {
			 PresentationCacheLayer::Objects,
			 PresentationCacheLayer::Particles,
			 PresentationCacheLayer::Environment,
			 PresentationCacheLayer::PortalInputs,
			 PresentationCacheLayer::PortalHistory,
			 PresentationCacheLayer::GameInterface,
			 PresentationCacheLayer::HostInterface,
		 }) {
		const auto &activity = activities[static_cast<size_t>(layer)];
		CHECK(activity.Hits == 0);
		CHECK(activity.Writes == 0);
		CHECK(activity.Last == engine::render::PresentationCacheActivity::Decision::NotApplicable);
	}
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::FinalImage)].Hits == 128);
}

TEST_CASE("steady Studio presentation writes no retained layer", "[render][presentation][cache]") {
	PresentationDamageTracker tracker;
	tracker.Commit(Settled());

	for (size_t opportunity = 0; opportunity < 41'247; opportunity++) {
		tracker.CacheProfile().Record(tracker.Inspect(Settled()), true);
	}

	auto activities = tracker.CacheProfile().Activities();
	for (const engine::render::PresentationCacheActivity &activity : activities) {
		CHECK(activity.Writes == 0);
		CHECK(activity.Hits == 41'247);
	}

	PresentationSignatures statusChanged = Settled();
	statusChanged.HostInterface++;
	tracker.CacheProfile().Record(tracker.Inspect(statusChanged), true);
	tracker.Commit(statusChanged);

	activities = tracker.CacheProfile().Activities();
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::HostInterface)].Writes == 1);
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::StudioComposition)].Writes == 1);
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::FinalImage)].Writes == 1);
	CHECK(activities[static_cast<size_t>(PresentationCacheLayer::SceneImage)].Writes == 0);
}

TEST_CASE(
	"resetting cache diagnostics clears decisions and their last state", "[render][presentation][cache]"
) {
	PresentationDamageTracker tracker;
	PresentationDamage damage;
	damage.Objects = true;
	tracker.CacheProfile().Record(damage, true);
	tracker.CacheProfile().Reset();

	for (const engine::render::PresentationCacheActivity &activity : tracker.CacheProfile().Activities()) {
		CHECK(activity.Hits == 0);
		CHECK(activity.Writes == 0);
		CHECK_FALSE(activity.Wrote());
		CHECK(activity.Last == engine::render::PresentationCacheActivity::Decision::NotObserved);
	}
}

TEST_CASE("an invisible resident particle layer keeps its retained image", "[render][presentation][cache]") {
	ParticleLayerVisibility visibility;
	CHECK(visibility.RequiresImage(true, false, 41));

	visibility.Commit(41, false);
	CHECK_FALSE(visibility.RequiresImage(true, false, 41));
	PresentationDamage simulationOnly;
	simulationOnly.Scene = true;
	simulationOnly.Particles = true;
	visibility.Refine(simulationOnly, true, false, 41);
	CHECK_FALSE(simulationOnly.Particles);
	CHECK_FALSE(simulationOnly.Scene);
	CHECK(visibility.RequiresImage(true, false, 42));
	CHECK(visibility.RequiresImage(true, true, 41));

	visibility.Commit(42, true);
	CHECK(visibility.RequiresImage(true, false, 42));
	visibility.Reset();
	CHECK(visibility.RequiresImage(true, false, 42));
}
