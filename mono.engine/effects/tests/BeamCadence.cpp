#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Registration.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.effects.beam-cadence")
TEST_DEPENDS("engine.effects.ribbon")
TEST_DEPENDS("engine.scene.update-cadence")

namespace {
	using namespace engine;

	struct BeamScene {
		ecs::Store World{"beam.cadence"};
		ecs::Entity Start;
		ecs::Entity Finish;
		ecs::Entity Beam;

		explicit BeamScene(bool cadence = true) {
			effects::RegisterEffectClasses();
			Start = World.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "Start");
			Finish = World.CreateInstance(ecs::Classes::Find(core::Name("Attachment")), "Finish");
			scene::Attachment first, last;
			first.Frame.Position = {-2, 0, -3};
			last.Frame.Position = {2, 0, -3};
			World.Set(Start, first);
			World.Set(Finish, last);
			Beam = World.CreateInstance(ecs::Classes::Find(core::Name("Beam")), "Beam");
			effects::Beam shape;
			shape.Attachment0 = Start;
			shape.Attachment1 = Finish;
			shape.CurveSize0 = 1.25f;
			shape.CurveSize1 = -0.75f;
			shape.Width0 = 0.3f;
			shape.Width1 = 3;
			shape.TextureSpeed = 0.25f;
			shape.Colour = core::ColorSequence{core::Color3{0.2f, 0.4f, 1}, core::Color3{1, 0.3f, 0.1f}};
			shape.Transparency = core::NumberSequence{0.1f, 0.7f};
			World.Set(Beam, shape);
			if (cadence) {
				scene::UpdateCadence policy;
				policy.Identity = core::Name("town.forge.cosmetic-beam");
				policy.Domains[static_cast<size_t>(scene::UpdateDomain::CosmeticEffects)] = {8, 12};
				policy.DemotionDelayTicks = 2;
				World.Set(Beam, policy);
			}
		}
	};

	void SameOutput(const effects::RibbonBuffer &cached, const effects::RibbonBuffer &reference) {
		REQUIRE(cached.Runs.size() == reference.Runs.size());
		REQUIRE(cached.Vertices.size() == reference.Vertices.size());
		for (size_t index = 0; index < cached.Vertices.size(); ++index) {
			CHECK(cached.Vertices[index].Position == reference.Vertices[index].Position);
			CHECK(cached.Vertices[index].Coordinate == reference.Vertices[index].Coordinate);
			CHECK(cached.Vertices[index].Colour == reference.Vertices[index].Colour);
		}
	}
}

TEST_CASE(
	"beam cadence retains samples while every view and scroll matches full geometry",
	"[effects][beam-cadence]"
) {
	using namespace engine;
	BeamScene cached, reference(false);
	effects::RibbonBuffer output, full;
	REQUIRE(effects::BuildRibbons(cached.World, {0, 100, -3}, 0.25f, output) == 1);
	REQUIRE(effects::BuildRibbons(reference.World, {0, 100, -3}, 0.25f, full) == 1);
	SameOutput(output, full);
	const auto midpoint = (output.Vertices[10].Position + output.Vertices[11].Position) * 0.5f;
	CHECK(midpoint.X == Catch::Approx(0).margin(1e-6f));
	CHECK(midpoint.Z == Catch::Approx(-3.1875f));
	CHECK(output.Vertices[10].Coordinate.X == 2.0625f);
	CHECK(output.Vertices[10].Colour == 0x998c5999u);
	const auto *history = cached.World.Resource<effects::BeamCadenceCache>();
	REQUIRE(history != nullptr);
	CHECK(history->Rebuilt == 1);
	CHECK(history->Skipped == 0);
	const auto originalPosition = output.Vertices.front().Position;
	const float originalCoordinate = output.Vertices.front().Coordinate.X;
	const auto originalSample = history->Beams.at(cached.Beam).Samples[3].Centre;
	const auto generation = history->Beams.at(cached.Beam).State.Generation;
	const auto *allocation = output.Vertices.data();
	REQUIRE(effects::BuildRibbons(cached.World, {1, 0, 0}, 0.75f, output) == 1);
	REQUIRE(effects::BuildRibbons(reference.World, {1, 0, 0}, 0.75f, full) == 1);
	SameOutput(output, full);
	history = cached.World.Resource<effects::BeamCadenceCache>();
	CHECK(history->Rebuilt == 0);
	CHECK(history->Skipped == 1);
	CHECK(history->Beams.at(cached.Beam).State.Generation == generation);
	CHECK(history->Beams.at(cached.Beam).Samples[3].Centre == originalSample);
	CHECK(output.Vertices.front().Position != originalPosition);
	CHECK(output.Vertices.front().Coordinate.X == Catch::Approx(originalCoordinate + 0.125f));
	CHECK(output.Vertices.data() == allocation);
	uint64_t skipped = 0;
	for (uint64_t tick = 1; tick <= 24; ++tick) {
		cached.World.AdvanceTick(1.0f / 60);
		reference.World.AdvanceTick(1.0f / 60);
		const float elapsed = static_cast<float>(tick) * 0.125f;
		REQUIRE(effects::BuildRibbons(cached.World, {1, 0, 0}, elapsed, output) == 1);
		REQUIRE(effects::BuildRibbons(reference.World, {1, 0, 0}, elapsed, full) == 1);
		SameOutput(output, full);
		history = cached.World.Resource<effects::BeamCadenceCache>();
		CHECK(history->Rebuilt + history->Skipped == 1);
		CHECK(tick - history->Beams.at(cached.Beam).State.LastCommittedTick <= 12);
		skipped += history->Skipped;
	}
	CHECK(skipped >= 18);
}

TEST_CASE(
	"beam source changes refresh immediately including cleared observation windows", "[effects][beam-cadence]"
) {
	using namespace engine;
	BeamScene cached, reference(false);
	effects::RibbonBuffer output, full;
	REQUIRE(effects::BuildRibbons(cached.World, {1, 0, 0}, 0, output) == 1);
	const auto firstGeneration =
		cached.World.Resource<effects::BeamCadenceCache>()->Beams.at(cached.Beam).State.Generation;
	for (auto *fixture : {&cached, &reference})
		fixture->World.GetMutable<scene::Attachment>(fixture->Finish)->Frame.Position.Y = 2;
	REQUIRE(effects::BuildRibbons(cached.World, {1, 0, 0}, 0, output) == 1);
	REQUIRE(effects::BuildRibbons(reference.World, {1, 0, 0}, 0, full) == 1);
	SameOutput(output, full);
	CHECK(cached.World.Resource<effects::BeamCadenceCache>()->Rebuilt == 1);
	CHECK(
		cached.World.Resource<effects::BeamCadenceCache>()->Beams.at(cached.Beam).State.Generation ==
		firstGeneration + 1
	);
	for (auto *fixture : {&cached, &reference})
		fixture->World.GetMutable<effects::Beam>(fixture->Beam)->Colour =
			core::ColorSequence{core::Color3{0.8f, 0.4f, 0.2f}};
	cached.World.ClearChanges();
	REQUIRE(effects::BuildRibbons(cached.World, {1, 0, 0}, 0, output) == 1);
	REQUIRE(effects::BuildRibbons(reference.World, {1, 0, 0}, 0, full) == 1);
	SameOutput(output, full);
	CHECK(cached.World.Resource<effects::BeamCadenceCache>()->Rebuilt == 1);
	CHECK(cached.World.Resource<effects::BeamCadenceCache>()->Skipped == 0);
	for (auto *fixture : {&cached, &reference}) {
		auto *beam = fixture->World.GetMutable<effects::Beam>(fixture->Beam);
		beam->FaceCamera = false;
		beam->Width0 = 2;
	}
	REQUIRE(effects::BuildRibbons(cached.World, {0, 100, -3}, 0, output) == 1);
	REQUIRE(effects::BuildRibbons(reference.World, {0, 100, -3}, 0, full) == 1);
	SameOutput(output, full);
	CHECK(cached.World.Resource<effects::BeamCadenceCache>()->Rebuilt == 1);
}

TEST_CASE("combat and player-owned beam sources keep full geometry fidelity", "[effects][beam-cadence]") {
	using namespace engine;
	BeamScene beamScene;
	effects::RibbonBuffer output;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Skipped == 1);
	beamScene.World.GetMutable<scene::UpdateCadence>(beamScene.Beam)->CriticalReasons =
		static_cast<uint32_t>(scene::UpdateCriticality::Combat);
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Rebuilt == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->CriticalRebuilds == 1);
	beamScene.World.GetMutable<scene::UpdateCadence>(beamScene.Beam)->CriticalReasons = 0;
	scene::InstallServices(beamScene.World);
	const auto player = scene::AddPlayer(beamScene.World, "Beam owner");
	const auto character = scene::LoadCharacter(beamScene.World, player);
	REQUIRE(character != ecs::NULL_ENTITY);
	const auto root = beamScene.World.Get<scene::Character>(character)->Root;
	REQUIRE(beamScene.World.SetParent(beamScene.Start, root));
	for (int tick = 0; tick < 4; ++tick) {
		beamScene.World.AdvanceTick(1.0f / 60);
		REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
		CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Rebuilt == 1);
		CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->CriticalRebuilds == 1);
		CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Skipped == 0);
	}
}

TEST_CASE(
	"beam cache admission falls back and missing or removed sources leave no stale ribbon",
	"[effects][beam-cadence]"
) {
	using namespace engine;
	BeamScene beamScene;
	effects::RibbonBuffer output;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	const auto original = output;
	beamScene.World.ResourceMutable<effects::BeamCadenceCache>()->MaximumBeams = 0;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	SameOutput(output, original);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->CapacityRefused == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
	beamScene.World.ResourceMutable<effects::BeamCadenceCache>()->MaximumBeams = 1;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	beamScene.World.GetMutable<effects::Beam>(beamScene.Beam)->Enabled = false;
	CHECK(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 0);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
	beamScene.World.GetMutable<effects::Beam>(beamScene.Beam)->Enabled = true;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	beamScene.World.DestroyInstance(beamScene.Finish);
	CHECK(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 0);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->InvalidSources == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
	CHECK(output.Vertices.empty());
	beamScene.World.DestroyInstance(beamScene.Beam);
	CHECK(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 0);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->InvalidSources == 0);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
}

TEST_CASE("beam cache is derived and invalid inputs never commit sampled state", "[effects][beam-cadence]") {
	using namespace engine;
	BeamScene beamScene;
	effects::RibbonBuffer output;
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	const auto &descriptor = ecs::Components::Describe(ecs::Components::Of<effects::BeamCadenceCache>());
	core::ByteWriter bytes;
	descriptor.Write(bytes, beamScene.World.Resource<effects::BeamCadenceCache>(), 1);
	CHECK(bytes.Size() == 0);
	effects::BeamCadenceCache restored = *beamScene.World.Resource<effects::BeamCadenceCache>();
	core::ByteReader reader(bytes.Bytes());
	descriptor.Read(reader, &restored, 1);
	CHECK_FALSE(reader.Failed());
	CHECK(restored.Beams.empty());
	const auto policy = *beamScene.World.Get<scene::UpdateCadence>(beamScene.Beam);
	beamScene.World.Remove<scene::UpdateCadence>(beamScene.Beam);
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
	beamScene.World.Set(beamScene.Beam, policy);
	REQUIRE(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 1);
	beamScene.World.GetMutable<scene::Attachment>(beamScene.Start)->Frame.Position.X =
		std::numeric_limits<float>::infinity();
	CHECK(effects::BuildRibbons(beamScene.World, {0, 0, 0}, 0, output) == 0);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->InvalidSources == 1);
	CHECK(beamScene.World.Resource<effects::BeamCadenceCache>()->Beams.empty());
}
