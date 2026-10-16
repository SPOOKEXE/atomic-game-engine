#include <engine/assets/Animation.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Animation.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/UpdateCadence.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

TEST_SUITE_ID("engine.render.animation")
TEST_DEPENDS("engine.scene.animation")
TEST_DEPENDS("engine.scene.update-cadence")

using engine::core::Name;

namespace {
	struct CadencedRig {
		engine::ecs::Entity Rig;
		engine::ecs::Entity Bone;
		engine::ecs::Entity Animator;
		engine::ecs::Entity Track;
		engine::ecs::Entity Clip;
	};

	CadencedRig MakeCadencedRig(engine::ecs::Store &store) {
		using namespace engine;
		CadencedRig made;
		made.Rig = store.CreateInstance(ecs::Classes::Find(Name("MeshPart")), "CadencedRig");
		store.Set(made.Rig, scene::Skeleton{Name("cadence.rig"), 1, {}});
		made.Bone = store.CreateInstance(scene::BoneClass(), "Root");
		REQUIRE(store.SetParent(made.Bone, made.Rig));
		made.Animator = store.CreateInstance(scene::AnimatorClass(), "Animator");
		REQUIRE(store.SetParent(made.Animator, made.Rig));
		made.Clip = store.CreateInstance(ecs::Classes::Find(Name("Animation")), "Move");
		store.Set(made.Clip, scene::AnimationClip{Name("cadence.move"), Name("cadence.rig")});
		made.Track = store.CreateInstance(ecs::Classes::Find(Name("AnimationTrack")), "Track");
		REQUIRE(store.SetParent(made.Track, made.Animator));
		scene::AnimationTrack playing;
		playing.Clip = made.Clip;
		playing.Weight = 1;
		playing.FadeTime = 0;
		playing.Playing = true;
		store.Set(made.Track, playing);
		assets::AnimationData clip;
		clip.Duration = 32;
		clip.Channels = {{0, {{0, {}}, {32, core::CFrame(core::Vector3{128, 0, 0})}}}};
		REQUIRE(render::RecordAnimation(store, Name("cadence.move"), clip));
		return made;
	}

	engine::scene::UpdateCadence PosePolicy() {
		engine::scene::UpdateCadence policy;
		policy.Identity = Name("town.citizen.baker");
		policy.Domains[static_cast<size_t>(engine::scene::UpdateDomain::VisualPose)] = {4, 8};
		policy.DemotionDelayTicks = 2;
		return policy;
	}
}

TEST_CASE("animation tracks sample clip channels into bone transforms", "[render][animation]") {
	engine::scene::RegisterSceneClasses();
	engine::render::RegisterPresentationComponents();
	engine::ecs::Store store("animation-presentation");

	const engine::ecs::Entity rig = store.CreateInstance(engine::ecs::Classes::Find(Name("MeshPart")), "Rig");
	store.Set(rig, engine::scene::Skeleton{Name("animation.Rig"), 1, {}});
	const engine::ecs::Entity bone = store.CreateInstance(engine::ecs::Classes::Find(Name("Bone")), "Root");
	REQUIRE(store.SetParent(bone, rig));
	store.Set(bone, engine::scene::Bone{});

	const engine::ecs::Entity animator = store.CreateInstance(engine::scene::AnimatorClass(), "Animator");
	REQUIRE(store.SetParent(animator, rig));
	store.Set(animator, engine::scene::Animator{});
	const engine::ecs::Entity clip =
		store.CreateInstance(engine::ecs::Classes::Find(Name("Animation")), "Move");
	store.Set(clip, engine::scene::AnimationClip{Name("animation.Move"), Name("animation.Rig")});
	const engine::ecs::Entity track =
		store.CreateInstance(engine::ecs::Classes::Find(Name("AnimationTrack")), "Track");
	REQUIRE(store.SetParent(track, animator));
	engine::scene::AnimationTrack playing;
	playing.Clip = clip;
	playing.TimePosition = 0.5f;
	playing.Weight = 1.0f;
	playing.Playing = true;
	store.Set(track, playing);

	engine::assets::AnimationData data;
	data.Duration = 1.0f;
	data.Channels = {{0, {{0.0f, {}}, {1.0f, engine::core::CFrame(engine::core::Vector3{4, 0, 0})}}}};
	REQUIRE(engine::render::RecordAnimation(store, Name("animation.Move"), data));

	CHECK(engine::render::EvaluateAnimations(store) == 1);
	const engine::scene::Bone *posed = store.Get<engine::scene::Bone>(bone);
	REQUIRE(posed != nullptr);
	CHECK(posed->Transform.Position.X == Catch::Approx(2.0f));

	const engine::ecs::Entity buffer =
		store.CreateInstance(engine::ecs::Classes::Find(Name("AnimationBuffer")), "ProceduralMove");
	auto baked = [](float distance) {
		engine::assets::AnimationData animation;
		animation.Duration = 1.0f;
		animation.Channels = {{
			0,
			{{0.0f, {}}, {1.0f, engine::core::CFrame(engine::core::Vector3{distance, 0, 0})}},
		}};
		engine::core::ByteWriter writer;
		REQUIRE(engine::assets::Animation::Write(writer, animation));
		return std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end());
	};

	REQUIRE(engine::scene::SetAnimationBuffer(store, buffer, baked(8.0f)));
	store.GetMutable<engine::scene::AnimationClip>(clip)->Buffer = buffer;
	CHECK(engine::render::EvaluateAnimations(store) == 1);
	CHECK(store.Get<engine::scene::Bone>(bone)->Transform.Position.X == Catch::Approx(4.0f));

	// A changed revision is decoded once and replaces the cached pose source.
	REQUIRE(engine::scene::SetAnimationBuffer(store, buffer, baked(12.0f)));
	CHECK(engine::render::EvaluateAnimations(store) == 1);
	CHECK(store.Get<engine::scene::Bone>(bone)->Transform.Position.X == Catch::Approx(6.0f));
}

TEST_CASE(
	"scaled portal animation preserves sampled vertices and reverses without changing clip bytes",
	"[render][animation][portal-animation]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	ecs::Store source("animated-source"), destination("animated-destination"), returned("animated-return");
	for (auto *store : {&source, &destination, &returned})
		scene::InstallServices(*store);
	const auto player = scene::AddPlayer(source, "animated");
	const auto model = scene::LoadCharacter(source, player);
	const auto root = source.Get<scene::Character>(model)->Root;
	source.Set(root, scene::Skeleton{Name("portal.rig"), 1, {}});
	const auto bone = source.CreateInstance(scene::BoneClass(), "joint");
	source.SetParent(bone, root);
	scene::Bone joint;
	joint.Rest.Position = {0, 1, 0};
	joint.InverseBind.Position = {0, -1, 0};
	source.Set(bone, joint);
	const auto animator = source.CreateInstance(scene::AnimatorClass(), "animator");
	source.SetParent(animator, root);
	scene::Animator driver;
	driver.Rig = root;
	source.Set(animator, driver);
	const auto clip = source.CreateInstance(ecs::Classes::Find(Name("Animation")), "shared clip");
	const auto buffer = source.CreateInstance(ecs::Classes::Find(Name("AnimationBuffer")), "shared buffer");
	assets::AnimationData animation;
	animation.Duration = 1;
	animation.Channels = {{0, {{0, core::CFrame({0, 0, 0})}, {1, core::CFrame({4, 2, -2})}}}};
	core::ByteWriter encoded;
	REQUIRE(assets::Animation::Write(encoded, animation));
	REQUIRE(scene::SetAnimationBuffer(source, buffer, encoded.Bytes()));
	source.Set(clip, scene::AnimationClip{{}, Name("portal.rig"), buffer});
	const auto track = source.CreateInstance(ecs::Classes::Find(Name("AnimationTrack")), "track");
	source.SetParent(track, animator);
	scene::AnimationTrack playing;
	playing.Clip = clip;
	playing.TimePosition = .5f;
	playing.Weight = 1;
	playing.Playing = true;
	source.Set(track, playing);
	const auto vertex = [](ecs::Store &store, ecs::Entity root, core::Vector3 mesh) {
		REQUIRE(render::EvaluateAnimations(store) == 1);
		scene::ResolveBones(store);
		render::DrawList draw;
		scene::DrawInstance instance;
		instance.Source = root.Id;
		instance.Frame = store.Get<scene::Transform>(root)->Frame;
		instance.HalfExtent = store.Get<scene::Bounds>(root)->HalfExtent;
		draw.Instances.push_back(instance);
		render::CollectSkinPalettes(store, draw);
		REQUIRE(draw.JointFrames.size() == 1);
		const auto local = draw.JointFrames[0].PointToWorldSpace(mesh);
		const auto size = instance.HalfExtent * 2;
		return instance.Frame.PointToWorldSpace({local.X * size.X, local.Y * size.Y, local.Z * size.Z});
	};
	const core::Vector3 mesh{.25f, .5f, -.75f};
	const auto original = vertex(source, root, mesh);
	const scene::SeamTransform through{core::CFrame::LookAt({40, 20, 10}, {41, 20, 10}), {}, 2};
	scene::PortalBodyCopy body;
	std::string failure;
	REQUIRE(scene::CapturePortalBody(source, player, body, failure));
	REQUIRE(scene::MapPortalBody(body, through, failure));
	scene::PortalBodyArrival arrived;
	REQUIRE(scene::AdmitPortalBody(destination, body, arrived, failure));
	REQUIRE(destination.Get<scene::Skeleton>(arrived.Root)->PoseScale == Catch::Approx(2));
	// Change the copied sample so evaluation must replace the retained pose.
	destination.Each<scene::Bone>([](ecs::Entity, scene::Bone &value) { value.Transform = {}; });
	const auto mapped = vertex(destination, arrived.Root, mesh);
	CHECK((mapped - through.Point(original)).Magnitude() < 1e-4f);
	REQUIRE(scene::CapturePortalBody(destination, arrived.Player, body, failure));
	const scene::SeamTransform reverse{through.Frame.Inverse(), through.Point(through.Origin), .5f};
	REQUIRE(scene::MapPortalBody(body, reverse, failure));
	scene::PortalBodyArrival back;
	REQUIRE(scene::AdmitPortalBody(returned, body, back, failure));
	REQUIRE(returned.Get<scene::Skeleton>(back.Root)->PoseScale == Catch::Approx(1));
	returned.Each<scene::Bone>([](ecs::Entity, scene::Bone &value) { value.Transform = {}; });
	CHECK((vertex(returned, back.Root, mesh) - original).Magnitude() < 1e-4f);
	returned.Each<const scene::AnimationBuffer>([&](ecs::Entity, const scene::AnimationBuffer &value) {
		CHECK(std::ranges::equal(value.Data, encoded.Bytes()));
	});
}

TEST_CASE(
	"pose cadence retains bones while semantic animation matches full-fidelity playback",
	"[render][animation][cadence]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	ecs::Store full("pose.full"), throttled("pose.throttled");
	const auto reference = MakeCadencedRig(full);
	const auto reduced = MakeCadencedRig(throttled);
	const auto policy = PosePolicy();
	throttled.Set(reduced.Rig, policy);
	float previousPose = 0;
	uint64_t skipped = 0;
	for (uint64_t tick = 1; tick <= 32; ++tick) {
		full.AdvanceTick(0.125f);
		throttled.AdvanceTick(0.125f);
		REQUIRE(scene::AdvanceAnimationTracks(full) == 1);
		REQUIRE(scene::AdvanceAnimationTracks(throttled) == 1);
		CHECK(
			full.Get<scene::AnimationTrack>(reference.Track)->TimePosition ==
			throttled.Get<scene::AnimationTrack>(reduced.Track)->TimePosition
		);
		CHECK(
			full.Get<scene::AnimationTrack>(reference.Track)->Weight ==
			throttled.Get<scene::AnimationTrack>(reduced.Track)->Weight
		);
		const auto *history = throttled.Resource<scene::PoseCadenceHistory>();
		const scene::DomainCadenceState state =
			history == nullptr ? scene::DomainCadenceState{} : history->Poses.at(reduced.Animator);
		const auto due =
			scene::CadenceDue(policy, state, scene::UpdateDomain::VisualPose, tick, policy.Revision);
		REQUIRE(render::EvaluateAnimations(full) == 1);
		(void)render::EvaluateAnimations(throttled);
		history = throttled.Resource<scene::PoseCadenceHistory>();
		REQUIRE(history != nullptr);
		const auto pose = throttled.Get<scene::Bone>(reduced.Bone)->Transform.Position.X;
		if (due.Due) {
			CHECK(pose == full.Get<scene::Bone>(reference.Bone)->Transform.Position.X);
			CHECK(history->Evaluated == 1);
			CHECK(history->Skipped == 0);
		} else {
			CHECK(pose == previousPose);
			CHECK(history->Evaluated == 0);
			CHECK(history->Skipped == 1);
			skipped++;
		}
		CHECK(history->MaximumAgeTicks <= 8);
		previousPose = pose;
	}
	CHECK(skipped >= 20);
}

TEST_CASE(
	"combat promotion samples current time before the next periodic pose slot", "[render][animation][cadence]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	ecs::Store store("pose.combat");
	const auto rig = MakeCadencedRig(store);
	auto policy = PosePolicy();
	store.Set(rig.Animator, policy);
	store.AdvanceTick(0.125f);
	REQUIRE(scene::AdvanceAnimationTracks(store) == 1);
	REQUIRE(render::EvaluateAnimations(store) == 1);
	uint64_t retainedTick = 0;
	for (uint64_t tick = 2; tick < 8; ++tick) {
		store.AdvanceTick(0.125f);
		REQUIRE(scene::AdvanceAnimationTracks(store) == 1);
		if (render::EvaluateAnimations(store) == 0) {
			retainedTick = tick;
			break;
		}
	}
	REQUIRE(retainedTick != 0);
	const float semanticTime = store.Get<scene::AnimationTrack>(rig.Track)->TimePosition;
	REQUIRE(store.Get<scene::Bone>(rig.Bone)->Transform.Position.X != semanticTime * 4);
	store.GetMutable<scene::UpdateCadence>(rig.Animator)->CriticalReasons =
		static_cast<uint32_t>(scene::UpdateCriticality::Combat);
	REQUIRE(render::EvaluateAnimations(store) == 1);
	CHECK(store.Get<scene::Bone>(rig.Bone)->Transform.Position.X == semanticTime * 4);
	CHECK(store.Get<scene::AnimationTrack>(rig.Track)->TimePosition == semanticTime);
	REQUIRE(store.Resource<scene::PoseCadenceHistory>() != nullptr);
	CHECK(store.Resource<scene::PoseCadenceHistory>()->Promotions == 1);
	CHECK(
		store.Resource<scene::PoseCadenceHistory>()->Poses.at(rig.Animator).LastCommittedTick == retainedTick
	);
	store.GetMutable<scene::UpdateCadence>(rig.Animator)->CriticalReasons = 0;
	for (int held = 0; held < 2; ++held) {
		store.AdvanceTick(0.125f);
		REQUIRE(scene::AdvanceAnimationTracks(store) == 1);
		REQUIRE(render::EvaluateAnimations(store) == 1);
		CHECK(
			store.Get<scene::Bone>(rig.Bone)->Transform.Position.X ==
			store.Get<scene::AnimationTrack>(rig.Track)->TimePosition * 4
		);
	}
}

TEST_CASE(
	"ownership projectiles and animator requirements preserve full-fidelity poses",
	"[render][animation][cadence]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	for (int requirement = 0; requirement < 5; ++requirement) {
		ecs::Store store("pose.required");
		const auto rig = MakeCadencedRig(store);
		auto policy = PosePolicy();
		store.Set(rig.Rig, policy);
		if (requirement == 0) {
			const auto owner = store.CreateInstance(ecs::Classes::Find(Name("Player")), "Owner");
			store.Set(rig.Rig, scene::NetworkOwner{owner});
		}
		if (requirement == 1) {
			const auto owner = store.CreateInstance(ecs::Classes::Find(Name("Player")), "Owner");
			store.Set(rig.Rig, scene::Character{rig.Rig, {}, owner});
		}
		if (requirement == 2) store.GetMutable<scene::Animator>(rig.Animator)->EvaluationThrottled = false;
		if (requirement == 3) store.GetMutable<scene::Animator>(rig.Animator)->RootMotion = true;
		if (requirement == 4)
			store.GetMutable<scene::UpdateCadence>(rig.Rig)->CriticalReasons =
				static_cast<uint32_t>(scene::UpdateCriticality::Projectile);
		for (int tick = 0; tick < 10; ++tick) {
			store.AdvanceTick(0.125f);
			REQUIRE(scene::AdvanceAnimationTracks(store) == 1);
			REQUIRE(render::EvaluateAnimations(store) == 1);
			CHECK(
				store.Get<scene::Bone>(rig.Bone)->Transform.Position.X ==
				store.Get<scene::AnimationTrack>(rig.Track)->TimePosition * 4
			);
		}
		CHECK(store.Resource<scene::PoseCadenceHistory>()->Skipped == 0);
	}
}

TEST_CASE(
	"cadenced poses retry missing content and replicas never advance semantic time",
	"[render][animation][cadence]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	render::RegisterPresentationComponents();
	ecs::Store store("pose.retry");
	const auto rig = MakeCadencedRig(store);
	store.Set(rig.Rig, PosePolicy());
	store.GetMutable<scene::AnimationTrack>(rig.Track)->TimePosition = 0.5f;
	REQUIRE(render::EvaluateAnimations(store) == 1);
	const float retained = store.Get<scene::Bone>(rig.Bone)->Transform.Position.X;
	store.GetMutable<scene::AnimationClip>(rig.Clip)->Asset = Name("cadence.pending");
	for (int tick = 0; tick < 16; ++tick)
		store.AdvanceTick(0.125f);
	store.GetMutable<scene::AnimationTrack>(rig.Track)->TimePosition = 2;
	const auto generation = store.Resource<scene::PoseCadenceHistory>()->Poses.at(rig.Animator).Generation;
	CHECK(render::EvaluateAnimations(store) == 0);
	CHECK(store.Get<scene::Bone>(rig.Bone)->Transform.Position.X == retained);
	CHECK(store.Resource<scene::PoseCadenceHistory>()->Poses.at(rig.Animator).Generation == generation);
	CHECK(store.Resource<scene::PoseCadenceHistory>()->Unavailable == 1);
	assets::AnimationData arrived;
	arrived.Duration = 32;
	arrived.Channels = {{0, {{0, {}}, {32, core::CFrame(core::Vector3{128, 0, 0})}}}};
	REQUIRE(render::RecordAnimation(store, Name("cadence.pending"), arrived));
	store.SetAdoptOnly(true);
	REQUIRE(render::EvaluateAnimations(store) == 1);
	CHECK(store.Get<scene::Bone>(rig.Bone)->Transform.Position.X == 8);
	CHECK(store.Get<scene::AnimationTrack>(rig.Track)->TimePosition == 2);
	CHECK(store.Resource<scene::PoseCadenceHistory>()->Poses.at(rig.Animator).Generation == generation + 1);
	store.SetAdoptOnly(false);
	store.DestroyInstance(rig.Animator);
	(void)render::EvaluateAnimations(store);
	CHECK(store.Resource<scene::PoseCadenceHistory>()->Poses.empty());
}
