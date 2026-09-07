#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Tools.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

TEST_SUITE_ID("engine.scene.portaltransfer")
TEST_DEPENDS("engine.scene.surfacecameras")

using namespace engine::scene;
using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;

TEST_CASE(
	"portal sweep selects the nearest local or foreign seam and keeps the full remainder",
	"[scene][portal-transfer]"
) {
	std::array<PortalSeam, 2> seams;
	for (auto &seam : seams) {
		seam.Normal = {0, 0, 1};
		seam.First = {3, 0, 0};
		seam.Second = {0, 4, 0};
		seam.Destination = CFrame::LookAt({20, 5, 10}, {21, 5, 10});
		seam.Scale = 2;
	}
	seams[0].Centre = {0, 0, -5};
	seams[1].Crosses = true;
	seams[1].DestinationWorld = engine::core::Name("destination");
	PortalHop hop;
	size_t index = 99;
	REQUIRE(NearestPortalCrossing(seams, {1, 2, 10}, {1, 2, -10}, true, hop, index));
	REQUIRE(index == 1);
	REQUIRE(hop.Share == Approx(0.5f));
	const Vector3 hit{1, 2, 0};
	const Vector3 end{1, 2, -10};
	REQUIRE(
		(hop.Through.Point(hit) + hop.Through.Carry(end - hit) - hop.Through.Point(end)).Magnitude() < 1e-5f
	);
	REQUIRE(hop.Through.Carry({0, 0, -10}).Magnitude() == Approx(20));
	REQUIRE(hop.Through.Rotate({0, 0, -10}).Magnitude() == Approx(10));
	REQUIRE(NearestPortalCrossing(seams, {1, 2, 10}, {1, 2, -10}, false, hop, index));
	REQUIRE(index == 0);
	REQUIRE(hop.Share == Approx(0.75f));
}

TEST_CASE(
	"portal sweep refuses a rim miss backward one-way and malformed segment", "[scene][portal-transfer]"
) {
	PortalSeam seam;
	seam.Normal = {0, 0, 1};
	seam.First = {1, 0, 0};
	seam.Second = {0, 1, 0};
	seam.Bidirectional = false;
	PortalHop hop;
	hop.Share = 0.123f;
	size_t index = 55;
	const auto test = [&](Vector3 from, Vector3 to) {
		REQUIRE_FALSE(NearestPortalCrossing(std::span(&seam, 1), from, to, true, hop, index));
		REQUIRE(index == 55);
		REQUIRE(hop.Share == Approx(0.123f));
	};
	test({2, 0, 1}, {2, 0, -1});
	test({0, 0, -1}, {0, 0, 1});
	test({0, 0, 1}, {0, 0, 1});
	test({std::numeric_limits<float>::quiet_NaN(), 0, 1}, {0, 0, -1});
	seam.First = {};
	test({0, 0, 1}, {0, 0, -1});
}

TEST_CASE(
	"portal body copy preserves the actual renamed rig and maps pose and both velocities",
	"[scene][portal-transfer]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "same display name", false, 12345);
	const auto character = LoadCharacter(source, player);
	const Character rig = *source.Get<Character>(character);
	source.SetInstanceName(rig.Root, "custom root");
	source.SetInstanceName(rig.Humanoid, "custom steering");
	source.GetMutable<Humanoid>(rig.Humanoid)->Health = 37;
	source.GetMutable<Humanoid>(rig.Humanoid)->MoveDirection = {1, 0, 0};
	source.Set(rig.Root, Motion{{3, 4, 5}, {1, 2, 3}});
	const CFrame before = source.Get<Transform>(rig.Root)->Frame;
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalBody(source, player, body, failure);
	INFO(failure);
	REQUIRE(captured);
	engine::core::ByteWriter writer;
	REQUIRE(WritePortalBody(writer, body));
	engine::core::ByteReader reader(writer.Bytes());
	PortalBodyCopy decoded;
	REQUIRE(ReadPortalBody(reader, decoded));
	REQUIRE(reader.AtEnd());
	const SeamTransform through{CFrame::LookAt({20, 30, 40}, {21, 30, 40}), {}, 2};
	REQUIRE(MapPortalBody(decoded, through, failure));
	PortalBodyArrival arrived;
	const bool admitted = AdmitPortalBody(destination, decoded, arrived, failure);
	INFO(failure);
	REQUIRE(admitted);
	REQUIRE(destination.InstanceNameOf(arrived.Root).Text() == "custom root");
	REQUIRE(destination.InstanceNameOf(arrived.Humanoid).Text() == "custom steering");
	REQUIRE(destination.Get<Humanoid>(arrived.Humanoid)->Health == 37);
	REQUIRE(destination.Get<Humanoid>(arrived.Humanoid)->RootPart == arrived.Root);
	REQUIRE(destination.Get<Character>(arrived.Character)->Owner == arrived.Player);
	REQUIRE(destination.Get<PlayerCharacter>(arrived.Player)->Model == arrived.Character);
	REQUIRE(NetworkOwnerOf(destination, arrived.Root) == arrived.Player);
	REQUIRE(
		(destination.Get<Transform>(arrived.Root)->Frame.Position - through.Place(before).Position)
			.Magnitude() < 1e-5f
	);
	REQUIRE((destination.Get<Motion>(arrived.Root)->Linear - through.Carry({3, 4, 5})).Magnitude() < 1e-5f);
	REQUIRE((destination.Get<Motion>(arrived.Root)->Angular - through.Rotate({1, 2, 3})).Magnitude() < 1e-5f);
	REQUIRE(destination.Get<PlayerIdentity>(arrived.Player)->UserId == 12345);
	REQUIRE(source.Get<Humanoid>(rig.Humanoid)->Health == 37);

	for (size_t size = 0; size < writer.Size(); ++size) {
		engine::core::ByteReader truncated(writer.Bytes().first(size));
		PortalBodyCopy untouched;
		untouched.Player = "sentinel";
		REQUIRE_FALSE(ReadPortalBody(truncated, untouched));
		REQUIRE(untouched.Player == "sentinel");
	}
}

TEST_CASE(
	"portal body refuses unsupported or external rig state without touching the source",
	"[scene][portal-transfer]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source");
	InstallServices(source);
	const auto player = AddPlayer(source, "player");
	const auto model = LoadCharacter(source, player);
	const Character rig = *source.Get<Character>(model);
	PortalBodyCopy body;
	std::string failure;
	source.Set(rig.Root, Camera{});
	REQUIRE_FALSE(CapturePortalBody(source, player, body, failure));
	REQUIRE(failure.find("scene.Camera") != std::string::npos);
	REQUIRE(source.Alive(rig.Root));
	const engine::ecs::ComponentId retained[]{engine::ecs::Components::Assigned<Camera>()};
	REQUIRE(CapturePortalBody(source, player, body, failure, retained));
	REQUIRE(source.Has<Camera>(rig.Root));
	for (const auto &node : body.Nodes)
		for (const auto &component : node.Components)
			CHECK(component.Type != "scene.Camera");
	source.Remove<Camera>(rig.Root);
	source.GetMutable<Humanoid>(rig.Humanoid)->RootPart = WorkspaceOf(source);
	REQUIRE_FALSE(CapturePortalBody(source, player, body, failure));
	REQUIRE(failure.find("scene.Humanoid") != std::string::npos);
	REQUIRE(source.Alive(rig.Root));
}

TEST_CASE(
	"portal body rejects missing class state and inconsistent roles before admission",
	"[scene][portal-transfer]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "player");
	LoadCharacter(source, player);
	PortalBodyCopy original;
	std::string failure;
	REQUIRE(CapturePortalBody(source, player, original, failure));
	const auto reject = [&](const PortalBodyCopy &body) {
		PortalBodyArrival arrived;
		REQUIRE_FALSE(ValidatePortalBody(body, failure));
		REQUIRE_FALSE(AdmitPortalBody(destination, body, arrived, failure));
		REQUIRE(PlayerCount(destination) == 0);
	};
	{
		auto body = original;
		body.Root = body.Player;
		reject(body);
	}
	{
		auto body = original;
		for (auto &node : body.Nodes)
			if (node.Key == body.Humanoid) node.Components.clear();
		reject(body);
	}
	{
		auto body = original;
		for (auto &node : body.Nodes)
			if (node.Key == body.Character) {
				for (auto &component : node.Components)
					if (component.Type == "scene.Character") component.References[0] = body.Player;
			}
		reject(body);
	}
	{
		const auto names = engine::core::Name::Count();
		auto body = original;
		body.Nodes[0].Class = "rejected.portal.class.never.interned";
		reject(body);
		REQUIRE(engine::core::Name::Count() == names);
	}
}

TEST_CASE(
	"portal object copy preserves authored shape and subtree through a scaled seam",
	"[scene][portal-transfer][portal-object]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto object =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "cargo");
	source.SetParent(object, WorkspaceOf(source));
	source.Set(object, Transform{CFrame::LookAt({1, 2, 3}, {2, 2, 3})});
	source.Set(object, PreviousTransform{CFrame({1, 2, 30})});
	source.Set(object, Motion{{3, 4, 5}, {1, 2, 3}});
	source.Set(object, Simulated{});
	Collider collider;
	collider.Shape = ShapeKind::Sphere;
	collider.Extent = {2, 0, 0};
	source.Set(object, collider);
	const auto child =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Folder")), "contents");
	source.SetParent(child, object);
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalObject(source, object, body, failure);
	INFO(failure);
	REQUIRE(captured);
	REQUIRE(body.Kind == PortalBodyKind::Object);
	REQUIRE(body.Player.empty());
	const SeamTransform seam{CFrame::LookAt({20, 30, 40}, {21, 30, 40}), {}, 2};
	REQUIRE(MapPortalBody(body, seam, failure));
	engine::core::ByteWriter writer;
	REQUIRE(WritePortalBody(writer, body));
	engine::core::ByteReader reader(writer.Bytes());
	PortalBodyCopy decoded;
	REQUIRE(ReadPortalBody(reader, decoded));
	REQUIRE(reader.AtEnd());
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, decoded, arrival, failure));
	REQUIRE(arrival.Player == engine::ecs::NULL_ENTITY);
	REQUIRE(arrival.Humanoid == engine::ecs::NULL_ENTITY);
	REQUIRE(destination.InstanceNameOf(arrival.Root).Text() == "cargo");
	REQUIRE(destination.Get<Collider>(arrival.Root)->Shape == ShapeKind::Sphere);
	REQUIRE(
		destination.Get<PreviousTransform>(arrival.Root)->Frame.Position ==
		destination.Get<Transform>(arrival.Root)->Frame.Position
	);
	REQUIRE(destination.Get<Collider>(arrival.Root)->Extent.X == Approx(4));
	REQUIRE((destination.Get<Motion>(arrival.Root)->Linear - seam.Carry({3, 4, 5})).Magnitude() < 1e-5f);
	REQUIRE((destination.Get<Motion>(arrival.Root)->Angular - seam.Rotate({1, 2, 3})).Magnitude() < 1e-5f);
	REQUIRE(
		(destination.Get<Transform>(arrival.Root)->Frame.Position - seam.Point({1, 2, 3})).Magnitude() < 1e-5f
	);
	int children = 0;
	destination.EachChild(arrival.Root, [&](engine::ecs::Entity entity) {
		REQUIRE(destination.InstanceNameOf(entity).Text() == "contents");
		++children;
	});
	REQUIRE(children == 1);
	REQUIRE(PlayerCount(destination) == 0);
}

TEST_CASE(
	"portal object rejects external ownership and independently moving child bodies",
	"[scene][portal-transfer][portal-object]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source");
	InstallServices(source);
	const auto object =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "cargo");
	source.Set(object, Motion{{1, 2, 3}, {}});
	source.Set(object, Simulated{});
	PortalBodyCopy body;
	std::string failure;
	SECTION("external player owner") {
		const auto player = AddPlayer(source, "owner");
		source.Set(object, NetworkOwner{player});
		REQUIRE_FALSE(CapturePortalObject(source, object, body, failure));
		REQUIRE(failure.find("scene.NetworkOwner") != std::string::npos);
	}
	SECTION("compound assembly requires explicit support") {
		const auto child =
			source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "second body");
		source.SetParent(child, object);
		source.Set(child, Motion{});
		REQUIRE_FALSE(CapturePortalObject(source, object, body, failure));
	}
	REQUIRE(source.Get<Motion>(object)->Linear == Vector3{1, 2, 3});
	REQUIRE(source.Has<Simulated>(object));
}

TEST_CASE(
	"portal rig retains owned text values without interning their contents", "[scene][portal-transfer]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "player");
	LoadCharacter(source, player);
	const auto text =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("StringValue")), "state");
	source.SetParent(text, player);
	const std::string value("owned\0value", 11);
	source.Set(text, TextContent{value});
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalBody(source, player, body, failure);
	INFO(failure);
	REQUIRE(captured);
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, body, arrival, failure));
	bool found = false;
	destination.EachChild(arrival.Player, [&](engine::ecs::Entity child) {
		if (const auto *content = destination.Get<TextContent>(child)) {
			REQUIRE(content->Value == value);
			found = true;
		}
	});
	REQUIRE(found);
}

TEST_CASE(
	"portal rig copies active animation state and owned external clip definitions",
	"[scene][portal-transfer][portal-animation]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "animated");
	const auto character = LoadCharacter(source, player);
	const auto rig = *source.Get<Character>(character);
	source.Set(rig.Root, Skeleton{engine::core::Name("transfer.rig"), 1, {}});
	const auto bone = source.CreateInstance(BoneClass(), "joint");
	source.SetParent(bone, rig.Root);
	Bone joint;
	joint.Rest.Position = {0, 1, 0};
	joint.Transform.Position = {0, 2, 0};
	joint.InverseBind.Position = {0, -1, 0};
	source.Set(bone, joint);
	const auto animator = source.CreateInstance(AnimatorClass(), "animator");
	source.SetParent(animator, rig.Humanoid);
	Animator controller;
	controller.Rig = rig.Root;
	source.Set(animator, controller);
	const auto clip =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Animation")), "shared clip");
	const auto buffer = source.CreateInstance(
		engine::ecs::Classes::Find(engine::core::Name("AnimationBuffer")), "shared buffer"
	);
	source.SetParent(clip, WorkspaceOf(source));
	source.SetParent(buffer, WorkspaceOf(source));
	AnimationBuffer content;
	content.Data = {std::byte{1}, std::byte{2}, std::byte{3}};
	content.Revision = 17;
	source.Set(buffer, content);
	source.Set(clip, AnimationClip{{}, engine::core::Name("transfer.rig"), buffer});
	const auto track =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("AnimationTrack")), "playing");
	source.SetParent(track, animator);
	AnimationTrack playing;
	playing.Clip = clip;
	playing.TimePosition = 2.25f;
	playing.Speed = .75f;
	playing.Weight = .4f;
	playing.WeightTarget = .8f;
	playing.FadeTime = .3f;
	playing.Priority = AnimationPriority::Action;
	playing.Playing = true;
	playing.Looped = true;
	source.Set(track, playing);
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalBody(source, player, body, failure);
	INFO(failure);
	REQUIRE(captured);
	REQUIRE(MapPortalBody(body, {CFrame({20, 30, 40}), {}, 2}, failure));
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, body, arrival, failure));
	const auto arrivedAnimator = AnimatorFor(destination, arrival.Root);
	REQUIRE(arrivedAnimator != engine::ecs::NULL_ENTITY);
	REQUIRE(RigFor(destination, arrivedAnimator) == arrival.Root);
	bool found = false;
	destination.EachDescendant(arrivedAnimator, [&](engine::ecs::Entity entity) {
		const auto *state = destination.Get<AnimationTrack>(entity);
		if (!state) return;
		CHECK(state->TimePosition == playing.TimePosition);
		CHECK(state->Speed == playing.Speed);
		CHECK(state->Weight == playing.Weight);
		CHECK(state->WeightTarget == playing.WeightTarget);
		CHECK(state->FadeTime == playing.FadeTime);
		CHECK(state->Priority == playing.Priority);
		CHECK(state->Playing);
		CHECK(state->Looped);
		const auto *definition = destination.Get<AnimationClip>(state->Clip);
		REQUIRE(definition != nullptr);
		REQUIRE(definition->Rig == engine::core::Name("transfer.rig"));
		const auto *copied = destination.Get<AnimationBuffer>(definition->Buffer);
		REQUIRE(copied != nullptr);
		CHECK(copied->Data == content.Data);
		CHECK(copied->Revision == 17);
		found = true;
	});
	REQUIRE(found);
	for (const auto type : {"scene.Animator", "scene.AnimationTrack", "scene.AnimationClip"}) {
		auto malformed = body;
		for (auto &node : malformed.Nodes)
			for (auto &component : node.Components)
				if (component.Type == type) component.References[0] = body.Player;
		CHECK_FALSE(ValidatePortalBody(malformed, failure));
	}
	const auto originalBody = body.Root;
	source.GetMutable<AnimationBuffer>(buffer)->Data.resize(MAXIMUM_PORTAL_ANIMATION_BYTES + 1);
	REQUIRE_FALSE(CapturePortalBody(source, player, body, failure));
	CHECK(body.Root == originalBody);
	source.GetMutable<AnimationBuffer>(buffer)->Data.resize(MAXIMUM_PORTAL_ANIMATION_BYTES);
	REQUIRE(CapturePortalBody(source, player, body, failure));
	engine::core::ByteWriter bounded;
	REQUIRE(WritePortalBody(bounded, body));
	engine::core::ByteReader boundedReader(bounded.Bytes());
	PortalBodyCopy decoded;
	REQUIRE(ReadPortalBody(boundedReader, decoded));
	REQUIRE(boundedReader.AtEnd());
	source.Set(buffer, content);
	source.DestroyInstance(player);
	source.DestroyInstance(character);
	REQUIRE(source.Alive(clip));
	REQUIRE(source.Alive(buffer));
	CHECK(source.Get<AnimationBuffer>(buffer)->Data == content.Data);
}

TEST_CASE(
	"portal rig retains an equipped tool and scales the grip with its limb",
	"[scene][portal-transfer][portal-animation]"
) {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "equipped");
	const auto character = LoadCharacter(source, player);
	const auto tool = source.CreateInstance(ToolClass(), "carried tool");
	source.Set(tool, Tool{CFrame({.2f, .3f, .4f})});
	const auto handle =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Part")), "Handle");
	source.SetParent(handle, tool);
	REQUIRE(EquipTool(source, character, tool));
	const auto original = *source.Get<CharacterLimb>(handle);
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalBody(source, player, body, failure);
	INFO(failure);
	REQUIRE(captured);
	REQUIRE(MapPortalBody(body, {CFrame({10, 20, 30}), {}, 2}, failure));
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, body, arrival, failure));
	const auto equipped = EquippedTool(destination, arrival.Character);
	REQUIRE(equipped != engine::ecs::NULL_ENTITY);
	CHECK(destination.Get<Tool>(equipped)->Grip.Position == Vector3{.4f, .6f, .8f});
	const auto *limb = destination.Get<CharacterLimb>(ToolHandle(destination, equipped));
	REQUIRE(limb != nullptr);
	CHECK(limb->Root == arrival.Root);
	CHECK(limb->Offset.Position == original.Offset.Position * 2);
}

TEST_CASE("portal body carries authored attachment frames", "[scene][portal-transfer][accessory]") {
	RegisterSceneClasses();
	engine::ecs::Store source("source"), destination("destination");
	InstallServices(source);
	InstallServices(destination);
	const auto player = AddPlayer(source, "player");
	const auto character = LoadCharacter(source, player);
	const auto root = source.Get<Character>(character)->Root;
	const auto attachment =
		source.CreateInstance(engine::ecs::Classes::Find(engine::core::Name("Attachment")));
	REQUIRE(source.SetParent(attachment, root));
	source.Set(attachment, Attachment{CFrame({1, 2, 3}), CFrame({99, 99, 99})});
	PortalBodyCopy body;
	std::string failure;
	const bool captured = CapturePortalBody(source, player, body, failure);
	INFO(failure);
	REQUIRE(captured);
	const SeamTransform through{CFrame({10, 20, 30}), {}, 2};
	REQUIRE(MapPortalBody(body, through, failure));
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, body, arrival, failure));
	const auto copied = destination.FindFirstChildWhichIsA(
		arrival.Root, engine::ecs::Classes::Find(engine::core::Name("Attachment"))
	);
	REQUIRE(copied != engine::ecs::NULL_ENTITY);
	REQUIRE(destination.Get<Attachment>(copied)->Frame.Position == Vector3(2, 4, 6));
	REQUIRE(
		destination.Get<Attachment>(copied)->WorldFrame.Position ==
		(destination.Get<Transform>(arrival.Root)->Frame * CFrame({2, 4, 6})).Position
	);
}
