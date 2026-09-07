#include <engine/core/Paths.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.examples.noneuclidean")
TEST_DEPENDS("engine.examples.scene")
TEST_DEPENDS("engine.physics.query")

using namespace engine;

namespace {

	struct Exhibit {
		std::filesystem::path PreviousAssets = core::Paths::Assets();
		ecs::Store Store{"non-euclidean"};
		ecs::Scheduler Systems;
		std::vector<scene::PortalSeam> Seams;
		std::shared_ptr<script::Runtime> Runtime;

		Exhibit() {
			core::Paths::SetAssetsOverride(core::Paths::Base().parent_path() / "assets");
			std::string failure;
			const bool loaded = examples::LoadScene(
				Store, Systems, examples::ExamplePath("NonEuclidean.luau"), failure, &Runtime
			);
			INFO(failure);
			REQUIRE(loaded);
			scene::GatherPortalSeams(Store, Seams);
			REQUIRE(Seams.size() >= 12);
			(void)scene::OpenPortals(Store);
			physics::PreparePhysicsWorld(Store);
		}

		~Exhibit() {
			core::Paths::SetAssetsOverride(PreviousAssets);
		}
	};
}

TEST_CASE(
	"non-euclidean exhibits provide an unambiguous return through every opening", "[examples][non-euclidean]"
) {
	Exhibit exhibit;
	for (const auto &seam : exhibit.Seams) {
		INFO(exhibit.Store.InstanceNameOf(seam.Pane).Text());
		const auto returning =
			std::find_if(exhibit.Seams.begin(), exhibit.Seams.end(), [&](const auto &other) {
				return other.Pane == seam.Far && other.Far == seam.Pane;
			});
		CHECK(returning != exhibit.Seams.end());
		if (returning == exhibit.Seams.end()) continue;
		const auto outward = scene::SeamMapping(seam);
		const auto inward = scene::SeamMapping(*returning);
		for (const float side : {-1.0f, 1.0f}) {
			const auto start = seam.Centre + seam.Normal * side * 2;
			const auto finish = seam.Centre - seam.Normal * side * 2;
			scene::PortalHop hop;
			size_t crossed = 0;
			REQUIRE(scene::NearestPortalCrossing(exhibit.Seams, start, finish, false, hop, crossed));
			CHECK(exhibit.Seams[crossed].Pane == seam.Pane);
			REQUIRE(
				scene::NearestPortalCrossing(
					exhibit.Seams, outward.Point(finish), outward.Point(start), false, hop, crossed
				)
			);
			CHECK(exhibit.Seams[crossed].Pane == seam.Far);
			CHECK((inward.Point(outward.Point(start)) - start).Magnitude() < .001f);
			CHECK(
				(inward.Rotate(outward.Rotate(core::Vector3::XAxis)) - core::Vector3::XAxis).Magnitude() <
				.001f
			);
		}
	}
}

TEST_CASE(
	"non-euclidean authored walls let a humanoid reach each portal plane", "[examples][non-euclidean]"
) {
	Exhibit exhibit;
	const auto model = scene::MakeCharacter(exhibit.Store, {});
	REQUIRE(model != ecs::NULL_ENTITY);
	const auto rig = *exhibit.Store.Get<scene::Character>(model);
	const auto collider = *exhibit.Store.Get<scene::Collider>(rig.Root);
	for (const auto &seam : exhibit.Seams) {
		INFO(exhibit.Store.InstanceNameOf(seam.Pane).Text());
		const auto from = core::CFrame::LookAt(seam.Centre + seam.Normal * 2, seam.Centre);
		exhibit.Store.Set(rig.Root, scene::Transform{from});
		(void)scene::PoseCharacters(exhibit.Store);
		physics::SyncBroadphase(exhibit.Store);
		physics::BroadPhase(exhibit.Store);
		const auto sweep =
			physics::SweepPlacement(exhibit.Store, collider, from, -seam.Normal * 2, {}, rig.Root);
		REQUIRE(sweep.Complete);
		INFO(
			"blocking part " << exhibit.Store.InstanceNameOf(sweep.Owner).Text() << " at " << sweep.Fraction
		);
		CHECK_FALSE(sweep.Hit);
	}
}

TEST_CASE(
	"non-euclidean humanoid walks through each opening under fixed-tick physics",
	"[examples][non-euclidean-walk]"
) {
	Exhibit exhibit;
	physics::RegisterPhysicsSystems(exhibit.Systems);
	physics::RegisterCharacterSystems(exhibit.Systems);
	const auto model = scene::MakeCharacter(exhibit.Store, {});
	REQUIRE(model != ecs::NULL_ENTITY);
	const auto rig = *exhibit.Store.Get<scene::Character>(model);
	for (const auto &seam : exhibit.Seams) {
		INFO(exhibit.Store.InstanceNameOf(seam.Pane).Text());
		auto position = seam.Centre + seam.Normal * 2;
		position.Y = seam.Centre.Y - std::abs(seam.Second.Y) + scene::CHARACTER_HEIGHT / 2 + .01f;
		exhibit.Store.Set(rig.Root, scene::Transform{core::CFrame::LookAt(position, position - seam.Normal)});
		exhibit.Store.Set(rig.Root, scene::Motion{});
		(void)scene::PoseCharacters(exhibit.Store);
		auto *humanoid = exhibit.Store.GetMutable<scene::Humanoid>(rig.Humanoid);
		humanoid->MoveDirection = -seam.Normal;
		humanoid->WalkSpeed = 4;
		const auto *previous = exhibit.Store.Get<scene::PortalTransit>(rig.Root);
		const uint32_t serial = previous == nullptr ? 0 : previous->Serial;
		bool crossed = false;
		for (int tick = 0; tick < 120; ++tick) {
			exhibit.Systems.Tick(exhibit.Store, 1.0f / 60);
			const auto *transit = exhibit.Store.Get<scene::PortalTransit>(rig.Root);
			if (transit != nullptr && transit->Serial != serial) {
				CHECK(transit->Serial == serial + 1);
				crossed = true;
				break;
			}
		}
		const auto landed = exhibit.Store.Get<scene::Transform>(rig.Root)->Frame.Position;
		INFO("landed " << landed.X << ", " << landed.Y << ", " << landed.Z);
		CHECK(crossed);
		if (crossed) CHECK((landed - seam.Destination.Position).Magnitude() < 3);
	}
}

TEST_CASE(
	"non-euclidean pillar admits repeated continuous humanoid circuits", "[examples][non-euclidean-loop]"
) {
	Exhibit exhibit;
	physics::RegisterPhysicsSystems(exhibit.Systems);
	physics::RegisterCharacterSystems(exhibit.Systems);
	const auto namedSeam = [&](std::string_view name) -> const scene::PortalSeam & {
		const auto found = std::find_if(exhibit.Seams.begin(), exhibit.Seams.end(), [&](const auto &seam) {
			return exhibit.Store.InstanceNameOf(seam.Pane).Text() == name;
		});
		INFO(name);
		REQUIRE(found != exhibit.Seams.end());
		return *found;
	};
	const auto &entrance = namedSeam("PillarWay1");
	const auto &connector = namedSeam("TurnLoop1");
	const auto &exit = namedSeam("TurnWay2");
	const auto model = scene::MakeCharacter(exhibit.Store, {});
	REQUIRE(model != ecs::NULL_ENTITY);
	const auto rig = *exhibit.Store.Get<scene::Character>(model);
	auto start = entrance.Centre + entrance.Normal * 2;
	start.Y = scene::CHARACTER_HEIGHT / 2 + .01f;
	exhibit.Store.Set(rig.Root, scene::Transform{core::CFrame::LookAt(start, start - entrance.Normal)});
	exhibit.Store.Set(rig.Root, scene::Motion{});
	(void)scene::PoseCharacters(exhibit.Store);
	uint32_t serial = 0;
	float walked = 0;
	const auto walk = [&](core::Vector3 target, const scene::PortalSeam *crossing) {
		for (int tick = 0; tick < 600; ++tick) {
			const auto before = exhibit.Store.Get<scene::Transform>(rig.Root)->Frame.Position;
			auto toward = target - before;
			toward.Y = 0;
			const float distance = toward.Magnitude();
			if (crossing == nullptr && distance < .2f) return true;
			if (distance < .001f) return false;
			auto *humanoid = exhibit.Store.GetMutable<scene::Humanoid>(rig.Humanoid);
			humanoid->MoveDirection = toward / distance;
			humanoid->WalkSpeed = 8;
			exhibit.Systems.Tick(exhibit.Store, 1.0f / 60);
			const auto after = exhibit.Store.Get<scene::Transform>(rig.Root)->Frame.Position;
			const auto *transit = exhibit.Store.Get<scene::PortalTransit>(rig.Root);
			if (transit == nullptr || transit->Serial == serial) {
				walked += (after - before).Magnitude();
				continue;
			}
			REQUIRE(crossing != nullptr);
			CHECK(transit->Serial == serial + 1);
			CHECK((after - crossing->Destination.Position).Magnitude() < 3);
			serial = transit->Serial;
			return true;
		}
		return false;
	};
	for (int lap = 0; lap < 2; ++lap) {
		INFO("lap " << lap);
		for (const auto *seam : {&entrance, &connector, &exit}) {
			INFO(exhibit.Store.InstanceNameOf(seam->Pane).Text());
			REQUIRE(walk(seam->Centre - seam->Normal * 2, seam));
		}
		const auto outside = scene::SeamMapping(exit).Point(exit.Centre);
		REQUIRE(walk({outside.X, 0, 2}, nullptr));
		REQUIRE(walk({entrance.Centre.X, 0, 2}, nullptr));
	}
	CHECK(serial == 6);
	CHECK(walked > 100);
}

TEST_CASE(
	"non-euclidean angle tour seeks every mouth without sweeping its cuts", "[examples][non-euclidean-tour]"
) {
	Exhibit exhibit;
	auto &store = exhibit.Store;
	const auto view = store.FindFirstChild(scene::WorkspaceOf(store), "Viewer");
	REQUIRE(view != ecs::NULL_ENTITY);
	const auto number = [&](const char *name, double value) {
		ecs::AttributeValue attribute;
		attribute.Type = ecs::PropertyType::Double;
		attribute.Double = value;
		REQUIRE(ecs::SetAttribute(store, view, core::Name(name), attribute));
	};
	ecs::AttributeValue mode;
	mode.Type = ecs::PropertyType::String;
	mode.String = "angles";
	REQUIRE(ecs::SetAttribute(store, view, core::Name("TourMode"), mode));
	ecs::AttributeValue count;
	REQUIRE(ecs::GetAttribute(store, view, core::Name("TourCount"), count));
	REQUIRE(count.Type == ecs::PropertyType::Double);
	REQUIRE(count.Double == 45 * exhibit.Seams.size());
	scene::CameraController controller;
	controller.Mode = scene::CameraMode::Scriptable;
	store.SetResource(controller);
	physics::RegisterCharacterSystems(exhibit.Systems);
	const auto tick = [&] {
		exhibit.Systems.Tick(store, 1.0f / 60);
		INFO(exhibit.Runtime->LastError());
		REQUIRE(exhibit.Runtime->LastError().empty());
		return store.Get<scene::Transform>(view)->Frame;
	};
	struct Coverage {
		unsigned Front = 0;
		unsigned Back = 0;
		unsigned Near = 0;
		unsigned Away = 0;
		unsigned Grazing = 0;
	};
	std::map<std::string, Coverage> covered;
	for (int sample = 0; sample < static_cast<int>(count.Double); ++sample) {
		INFO("sample " << sample);
		number("TourSample", sample);
		const auto pose = tick();
		CHECK(store.Resource<scene::ActiveCamera>()->Entity == view);
		CHECK(store.Get<scene::PreviousTransform>(view)->Frame.FuzzyEq(pose, 1e-6f));
		ecs::AttributeValue pane;
		REQUIRE(ecs::GetAttribute(store, view, core::Name("TourPane"), pane));
		const auto found = std::find_if(exhibit.Seams.begin(), exhibit.Seams.end(), [&](const auto &seam) {
			return store.InstanceNameOf(seam.Pane).Text() == pane.String;
		});
		REQUIRE(found != exhibit.Seams.end());
		const auto offset = pose.Position - found->Centre;
		const float distance = offset.Magnitude();
		const float side = offset.Dot(found->Normal);
		REQUIRE(std::isfinite(distance));
		REQUIRE(distance > .01f);
		CHECK(std::abs(side) > .01f);
		const float gaze = pose.LookVector().Dot(offset / distance);
		CHECK(std::abs(gaze) > .999f);
		auto &seen = covered[pane.String];
		if (side > 0)
			++seen.Front;
		else
			++seen.Back;
		if (std::abs(side) < .03f) ++seen.Near;
		if (gaze > 0) ++seen.Away;
		if (std::abs(side / distance) < .1f) ++seen.Grazing;
	}
	CHECK(covered.size() == exhibit.Seams.size());
	for (const auto &[pane, seen] : covered) {
		INFO(pane);
		CHECK(seen.Front >= 18);
		CHECK(seen.Back >= 18);
		CHECK(seen.Near == 2);
		CHECK(seen.Away == 2);
		CHECK(seen.Grazing >= 12);
	}

	number("TourSample", 7);
	const auto sought = tick();
	number("TourSample", 700);
	(void)tick();
	number("TourSample", 7);
	CHECK(tick().FuzzyEq(sought, 1e-6f));
	ecs::AttributeValue pause;
	pause.Type = ecs::PropertyType::Bool;
	pause.Bool = true;
	REQUIRE(ecs::SetAttribute(store, view, core::Name("TourPaused"), pause));
	number("TourSample", -1);
	const auto held = tick();
	for (int frame = 0; frame < 10; ++frame)
		CHECK(tick().FuzzyEq(held, 1e-6f));
	number("TourStep", 1);
	CHECK_FALSE(tick().FuzzyEq(held, 1e-6f));
	for (double invalid :
		 {std::numeric_limits<double>::quiet_NaN(),
		  std::numeric_limits<double>::infinity(),
		  std::numeric_limits<double>::max()}) {
		number("TourSample", invalid);
		number("TourStep", invalid);
		const auto pose = tick();
		CHECK(std::isfinite(pose.Position.X));
		CHECK(std::isfinite(pose.Position.Y));
		CHECK(std::isfinite(pose.Position.Z));
	}

	mode.String = "overview";
	REQUIRE(ecs::SetAttribute(store, view, core::Name("TourMode"), mode));
	const auto overview = tick();
	CHECK(store.Get<scene::PreviousTransform>(view)->Frame.FuzzyEq(overview, 1e-6f));
	CHECK(tick().FuzzyEq(overview, 1e-6f));
	store.SetResource(scene::ActiveCamera{});
	CHECK(tick().FuzzyEq(overview, 1e-6f));

	const auto playerCamera = store.CreateInstance(scene::CameraClass(), "PlayerCamera");
	const auto character = scene::MakeCharacter(store, {});
	REQUIRE(character != ecs::NULL_ENTITY);
	const auto rig = *store.Get<scene::Character>(character);
	REQUIRE(
		store.SetProperty(playerCamera, core::Name("CameraSubject"), &rig.Humanoid, sizeof(rig.Humanoid))
	);
	store.SetResource(scene::ActiveCamera{playerCamera});
	const auto beforePlay = store.Get<scene::Transform>(view)->Frame;
	for (int frame = 0; frame < 10; ++frame)
		CHECK(tick().FuzzyEq(beforePlay, 1e-6f));
	CHECK(store.Resource<scene::ActiveCamera>()->Entity == playerCamera);
	CHECK(store.Get<scene::CameraSubject>(playerCamera)->Target == rig.Humanoid);
}
