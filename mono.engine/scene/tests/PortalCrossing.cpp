#include <engine/core/Bytes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/PortalCrossing.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scene.portalcrossing")
TEST_DEPENDS("engine.scene.surfacecameras")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using namespace engine::scene;

namespace {
	PortalSeam Seam(Entity pane, Entity far) {
		PortalSeam seam;
		seam.Pane = pane;
		seam.Far = far;
		seam.Normal = {0, 0, 1};
		seam.First = {3, 0, 0};
		seam.Second = {0, 3, 0};
		return seam;
	}

	Entity Body(Store &store, Vector3 at) {
		const Entity body = store.Create();
		store.Set(body, Transform{CFrame(at)});
		store.Set(body, PortalCrossingState{});
		return body;
	}
}

TEST_CASE("body identity has a stable codec and refuses a live key collision", "[scene][portal-crossing]") {
	RegisterSceneClasses();
	Store store("portal crossing identity");
	const Entity first = store.Create();
	const Entity second = store.Create();
	const BodyIdentity identity{{17, 23}, 4};
	REQUIRE(AssignBodyIdentity(store, first, identity));
	CHECK_FALSE(AssignBodyIdentity(store, second, identity));

	engine::core::ByteWriter writer;
	WriteBodyIdentities(writer, &identity, 1);
	BodyIdentity decoded{};
	engine::core::ByteReader reader(writer.Bytes());
	ReadBodyIdentities(reader, &decoded, 1);
	CHECK_FALSE(reader.Failed());
	CHECK(decoded.Key == identity.Key);
	CHECK(decoded.Generation == identity.Generation);
}

TEST_CASE(
	"authority identity allocation is deterministic, monotone, and persisted in the store",
	"[scene][portal-crossing]"
) {
	RegisterSceneClasses();
	Store store("portal crossing authority");
	REQUIRE(ConfigureBodyIdentityAuthority(store, 0x1234, 41));
	const BodyIdentity first = MintBodyIdentity(store);
	const BodyIdentity second = MintBodyIdentity(store);
	CHECK(first.Key == BodyKey{0x1234, 41});
	CHECK(second.Key == BodyKey{0x1234, 42});
	CHECK(first.Generation == 1);
	CHECK_FALSE(ConfigureBodyIdentityAuthority(store, 0x9876, 1));
}

TEST_CASE(
	"a newly spawned or cloned character root receives one unique body identity", "[scene][portal-crossing]"
) {
	RegisterSceneClasses();
	Store store("portal crossing characters");
	InstallServices(store);
	const Entity first = MakeCharacter(store, CharacterDesc{});
	const Entity second = MakeCharacter(store, CharacterDesc{});
	REQUIRE(first != engine::ecs::NULL_ENTITY);
	REQUIRE(second != engine::ecs::NULL_ENTITY);
	const Entity firstRoot = store.Get<Character>(first)->Root;
	const Entity secondRoot = store.Get<Character>(second)->Root;
	const auto *firstIdentity = store.Get<BodyIdentity>(firstRoot);
	const auto *secondIdentity = store.Get<BodyIdentity>(secondRoot);
	REQUIRE(firstIdentity != nullptr);
	REQUIRE(secondIdentity != nullptr);
	CHECK(firstIdentity->Key != secondIdentity->Key);

	const Entity copiedRoot = store.CloneInstance(firstRoot);
	REQUIRE(copiedRoot != engine::ecs::NULL_ENTITY);
	BodyIdentity copyIdentity;
	REQUIRE(EnsureBodyIdentity(store, copiedRoot, copyIdentity));
	CHECK(copyIdentity.Key != firstIdentity->Key);
}

TEST_CASE("a body can stand halfway through one seam without retirement", "[scene][portal-crossing]") {
	Store store("portal crossing halfway");
	const Entity body = Body(store, {0, 0, 0});
	const Entity pane = store.Create();
	const Entity far = store.Create();
	const PortalSeam seam = Seam(pane, far);

	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	const PortalCrossingState *crossing = store.Get<PortalCrossingState>(body);
	REQUIRE(crossing != nullptr);
	CHECK(crossing->Phase == PortalCrossingPhase::Overlapping);
	CHECK(crossing->Pane == pane);
	const uint64_t revision = crossing->PresentationRevision;
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->PresentationRevision == revision);
}

TEST_CASE("body reach includes canonical bounds around its reference", "[scene][portal-crossing]") {
	Store store("portal crossing reach");
	const Entity body = Body(store, {0, 0, 0});
	store.Set(body, Bounds{{3, 4, 0}});
	CHECK(PortalBodyReach(store, body) == 5.0f);
}

TEST_CASE(
	"epsilon jitter keeps the established side and does not prepare a handoff", "[scene][portal-crossing]"
) {
	Store store("portal crossing jitter");
	const Entity body = Body(store, {0, 0, .5f});
	const Entity pane = store.Create();
	const Entity far = store.Create();
	const PortalSeam seam = Seam(pane, far);
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));

	for (const float offset : {-.0005f, .0005f, -.0009f, .0009f}) {
		store.GetMutable<Transform>(body)->Frame.Position.Z = offset;
		REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
		CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Overlapping);
		CHECK(store.Get<PortalCrossingState>(body)->StableSide == 1);
	}
}

TEST_CASE(
	"a pre-commit reversal restores the original side and full clearance retires the seam",
	"[scene][portal-crossing]"
) {
	Store store("portal crossing reversal");
	const Entity body = Body(store, {0, 0, .5f});
	const Entity pane = store.Create();
	const Entity far = store.Create();
	const PortalSeam seam = Seam(pane, far);
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	store.GetMutable<Transform>(body)->Frame.Position.Z = -.5f;
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Prepared);
	store.GetMutable<Transform>(body)->Frame.Position.Z = .5f;
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Overlapping);
	store.GetMutable<Transform>(body)->Frame.Position.Z = 2.0f;
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Idle);
}

TEST_CASE(
	"a changed or closed seam keeps its pinned endpoint until full clearance", "[scene][portal-crossing]"
) {
	Store store("portal crossing topology");
	const Entity body = Body(store, {0, 0, .5f});
	const Entity pane = store.Create();
	const Entity far = store.Create();
	PortalSeam first = Seam(pane, far);
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&first, 1), 1.0f));
	const uint64_t revision = store.Get<PortalCrossingState>(body)->TopologyRevision;
	first.Centre.X = 2.0f;
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&first, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->TopologyRevision == revision);
	PortalSeam pinned;
	REQUIRE(PinnedPortalSeam(*store.Get<PortalCrossingState>(body), pinned));
	CHECK(pinned.Centre.X == 0.0f);
	REQUIRE(AdvancePortalCrossing(store, body, {}, 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Overlapping);
	store.GetMutable<Transform>(body)->Frame.Position.Z = 2.0f;
	REQUIRE(AdvancePortalCrossing(store, body, {}, 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Idle);
}

TEST_CASE(
	"an overlapping body follows moving endpoints but keeps its entry pairing and scale",
	"[scene][portal-crossing]"
) {
	Store store("portal crossing moving seam");
	const Entity body = Body(store, {0, 0, .5f});
	const Entity pane = store.Create();
	const Entity far = store.Create();
	PortalSeam entered = Seam(pane, far);
	entered.Scale = 2.0f;
	entered.Destination.Position = {10, 0, 0};
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&entered, 1), 1.0f));

	// The mouth moves under a body that has stopped halfway through it. Its new
	// rectangle and destination pose are live, while a scale edit waits for the
	// body to clear the route it entered.
	PortalSeam moved = entered;
	moved.Centre = {2, 0, 0};
	moved.Destination.Position = {12, 0, 0};
	moved.Scale = .5f;
	store.GetMutable<Transform>(body)->Frame.Position = {2, 0, .5f};
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&moved, 1), 1.0f));
	PortalSeam active;
	REQUIRE(PinnedPortalSeam(*store.Get<PortalCrossingState>(body), std::span(&moved, 1), active));
	CHECK(active.Pane == pane);
	CHECK(active.Far == far);
	CHECK(active.Centre == moved.Centre);
	CHECK(active.Destination.Position == moved.Destination.Position);
	CHECK(active.Scale == entered.Scale);

	// Reversing while the mouth continues to move cancels the pending handoff
	// without replacing the paired route.
	store.GetMutable<Transform>(body)->Frame.Position = {2, 0, -.5f};
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&moved, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Prepared);
	store.GetMutable<Transform>(body)->Frame.Position = {2, 0, .5f};
	REQUIRE(AdvancePortalCrossing(store, body, std::span(&moved, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(body)->Phase == PortalCrossingPhase::Overlapping);
}

TEST_CASE(
	"the declared reference anchor decides root-first and camera-first crossings", "[scene][portal-crossing]"
) {
	Store store("portal crossing anchors");
	const Entity pane = store.Create();
	const Entity far = store.Create();
	const PortalSeam seam = Seam(pane, far);

	const Entity rootFirst = Body(store, {0, 0, .5f});
	const Entity camera = store.Create();
	store.Set(camera, Transform{CFrame({0, 0, .5f})});
	REQUIRE(AdvancePortalCrossing(store, rootFirst, std::span(&seam, 1), 1.0f));
	store.GetMutable<Transform>(rootFirst)->Frame.Position.Z = -.5f;
	REQUIRE(AdvancePortalCrossing(store, rootFirst, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(rootFirst)->Phase == PortalCrossingPhase::Prepared);

	const Entity cameraFirst = Body(store, {0, 0, .5f});
	store.GetMutable<Transform>(camera)->Frame.Position.Z = .5f;
	store.GetMutable<PortalCrossingState>(cameraFirst)->Reference = camera;
	REQUIRE(AdvancePortalCrossing(store, cameraFirst, std::span(&seam, 1), 1.0f));
	store.GetMutable<Transform>(camera)->Frame.Position.Z = -.5f;
	REQUIRE(AdvancePortalCrossing(store, cameraFirst, std::span(&seam, 1), 1.0f));
	CHECK(store.Get<PortalCrossingState>(cameraFirst)->Phase == PortalCrossingPhase::Prepared);
}
