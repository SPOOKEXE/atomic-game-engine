#include <engine/ecs/Store.hpp>
#include <engine/scene/Accessories.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/PortalTransfer.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.scene.accessories")
TEST_DEPENDS("engine.scene.characters")

using namespace engine;
using namespace engine::scene;
using core::CFrame;
using core::Vector3;

namespace {
	struct Rig {
		ecs::Store Store{"accessories"};
		ecs::Entity Player, Model, Root, Head, Hat, Handle, HatPoint, HeadPoint;
		Rig() {
			RegisterSceneClasses();
			InstallServices(Store);
			Player = AddPlayer(Store, "player");
			Model = LoadCharacter(Store, Player);
			Root = Store.Get<Character>(Model)->Root;
			Head = Store.FindFirstChild(Model, "Head");
			REQUIRE(Head != ecs::NULL_ENTITY);
			Hat = Store.CreateInstance(AccessoryClass(), "hat");
			Handle = Store.CreateInstance(PartClass(), "Handle");
			Store.SetParent(Handle, Hat);
			Store.Set(Handle, Motion{{3, 4, 5}, {1, 2, 3}});
			Store.Set(Handle, Simulated{});
			HatPoint = Point(Handle, CFrame({0, -.25f, .5f}) * CFrame::Angles(.2f, .3f, .4f));
			HeadPoint = Point(Head, CFrame({.2f, .5f, .1f}) * CFrame::Angles(-.3f, .1f, .2f));
		}
		ecs::Entity Point(ecs::Entity parent, CFrame frame) {
			const auto point = Store.CreateInstance(AttachmentClass(), "HatAttachment");
			Store.SetParent(point, parent);
			Store.Set(point, Attachment{frame, {}});
			return point;
		}
	};
	void SameFrame(const CFrame &a, const CFrame &b) {
		CHECK((a.Position - b.Position).Magnitude() < .0001f);
		CHECK((a.LookVector() - b.LookVector()).Magnitude() < .0001f);
		CHECK((a.UpVector() - b.UpVector()).Magnitude() < .0001f);
	}
	void Matched(ecs::Store &store, ecs::Entity hat) {
		PoseCharacters(store);
		ResolveAttachments(store);
		const auto points = *store.Get<Accessory>(hat);
		REQUIRE(points.HandleAttachment != ecs::NULL_ENTITY);
		REQUIRE(points.CharacterAttachment != ecs::NULL_ENTITY);
		SameFrame(
			ResolveAttachment(store, points.HandleAttachment),
			ResolveAttachment(store, points.CharacterAttachment)
		);
	}
}

TEST_CASE(
	"accessory equip matches full local attachment frames and follows the current rig", "[scene][accessory]"
) {
	Rig rig;
	const auto local = rig.Store.Get<Attachment>(rig.HatPoint)->Frame;
	const Collider collider = *rig.Store.Get<Collider>(rig.Handle);
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	REQUIRE(rig.Store.IsA(rig.Hat, ModelClass()));
	REQUIRE(rig.Store.ParentOf(rig.Hat) == rig.Model);
	REQUIRE_FALSE(rig.Store.Has<Motion>(rig.Handle));
	REQUIRE(rig.Store.Get<CharacterLimb>(rig.Handle)->Root == rig.Root);
	Matched(rig.Store, rig.Hat);
	SameFrame(local, rig.Store.Get<Attachment>(rig.HatPoint)->Frame);
	REQUIRE(rig.Store.Get<Collider>(rig.Handle)->CanQuery == collider.CanQuery);
	rig.Store.Set(rig.Root, Transform{CFrame({20, 30, 40}) * CFrame::Angles(.4f, .5f, .6f)});
	Matched(rig.Store, rig.Hat);
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	Matched(rig.Store, rig.Hat);
}

TEST_CASE("accessory mismatch ambiguity and replicas refuse before changing state", "[scene][accessory]") {
	Rig rig;
	SECTION("missing match") {
		rig.Store.SetInstanceName(rig.HeadPoint, "Other");
	}
	SECTION("ambiguous match") {
		rig.Point(rig.Head, CFrame{});
	}
	SECTION("nonfinite local frame") {
		rig.Store.GetMutable<Attachment>(rig.HatPoint)->Frame.Position.X =
			std::numeric_limits<float>::quiet_NaN();
	}
	SECTION("replica") {
		rig.Store.SetAdoptOnly(true);
	}
	const Motion before = *rig.Store.Get<Motion>(rig.Handle);
	REQUIRE_FALSE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	REQUIRE(rig.Store.ParentOf(rig.Hat) == ecs::NULL_ENTITY);
	REQUIRE_FALSE(rig.Store.Has<CharacterLimb>(rig.Handle));
	REQUIRE(rig.Store.Get<Motion>(rig.Handle)->Linear == before.Linear);
}

TEST_CASE(
	"accessory detach and removed targets release carried ownership at the last pose", "[scene][accessory]"
) {
	Rig rig;
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	Matched(rig.Store, rig.Hat);
	const CFrame before = rig.Store.Get<Transform>(rig.Handle)->Frame;
	SECTION("reparent") {
		REQUIRE(rig.Store.SetParent(rig.Hat, WorkspaceOf(rig.Store)));
	}
	SECTION("reparent handle") {
		REQUIRE(rig.Store.SetParent(rig.Handle, WorkspaceOf(rig.Store)));
	}
	SECTION("rename handle") {
		rig.Store.SetInstanceName(rig.Handle, "renamed");
	}
	SECTION("destroy target") {
		rig.Store.DestroyInstance(rig.HeadPoint);
	}
	SECTION("destroy handle point") {
		rig.Store.DestroyInstance(rig.HatPoint);
	}
	REQUIRE(UpdateAccessoryAttachments(rig.Store) == 0);
	REQUIRE_FALSE(rig.Store.Has<CharacterLimb>(rig.Handle));
	REQUIRE(rig.Store.Has<Motion>(rig.Handle));
	SameFrame(rig.Store.Get<Transform>(rig.Handle)->Frame, before);
	REQUIRE(rig.Store.Get<Accessory>(rig.Hat)->CharacterAttachment == ecs::NULL_ENTITY);
	rig.Store.Set(rig.Handle, Motion{{7, 8, 9}, {}});
	UpdateAccessoryAttachments(rig.Store);
	REQUIRE(rig.Store.Get<Motion>(rig.Handle)->Linear == Vector3(7, 8, 9));
}

TEST_CASE(
	"accessory portal copy remaps both points preserves scale and refuses dangling references",
	"[scene][accessory][portal-transfer]"
) {
	Rig rig;
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	Matched(rig.Store, rig.Hat);
	const auto original = rig.Store.Get<Transform>(rig.Handle)->Frame;
	const auto local = rig.Store.Get<Attachment>(rig.HatPoint)->Frame;
	PortalBodyCopy body;
	std::string failure;
	REQUIRE(CapturePortalBody(rig.Store, rig.Player, body, failure));

	for (int malformed = 0; malformed < 3; ++malformed) {
		auto invalid = body;
		for (auto &node : invalid.Nodes)
			for (auto &component : node.Components) {
				if (component.Type != "scene.Accessory") continue;
				if (malformed == 0) component.References[0] = component.References[1];
				if (malformed == 1) component.References[1] = invalid.Root;
				if (malformed == 2) node.Parent = invalid.Player;
			}
		REQUIRE_FALSE(ValidatePortalBody(invalid, failure));
	}
	const SeamTransform through{CFrame({10, 20, 30}) * CFrame::Angles(.3f, .4f, .2f), {}, 2};
	REQUIRE(MapPortalBody(body, through, failure));
	ecs::Store destination("accessory-destination");
	InstallServices(destination);
	PortalBodyArrival arrival;
	REQUIRE(AdmitPortalBody(destination, body, arrival, failure));
	const auto hat = destination.FindFirstChild(arrival.Character, "hat");
	Matched(destination, hat);
	const auto points = *destination.Get<Accessory>(hat);
	const auto handle = destination.ParentOf(points.HandleAttachment);
	SameFrame(destination.Get<Transform>(handle)->Frame, through.Place(original));
	CHECK(
		(destination.Get<Attachment>(points.HandleAttachment)->Frame.Position - local.Position * 2)
			.Magnitude() < .0001f
	);
	REQUIRE(destination.Get<CharacterLimb>(handle)->Root == arrival.Root);
	PortalBodyCopy back;
	REQUIRE(CapturePortalBody(destination, arrival.Player, back, failure));
	const SeamTransform reverse{
		CFrame(through.Frame.Inverse().Position * .5f, through.Frame.Inverse().Rotation()), {}, .5f
	};
	REQUIRE(MapPortalBody(back, reverse, failure));
	ecs::Store returned("accessory-returned");
	InstallServices(returned);
	PortalBodyArrival again;
	REQUIRE(AdmitPortalBody(returned, back, again, failure));
	const auto restored = returned.FindFirstChild(again.Character, "hat");
	Matched(returned, restored);
	SameFrame(
		returned.Get<Transform>(returned.ParentOf(returned.Get<Accessory>(restored)->HandleAttachment))
			->Frame,
		original
	);
	rig.Store.DestroyInstance(rig.HeadPoint);
	REQUIRE_FALSE(CapturePortalBody(rig.Store, rig.Player, body, failure));
	REQUIRE(rig.Store.Has<Motion>(rig.Root));
	REQUIRE(rig.Store.Alive(rig.Hat));
}

TEST_CASE("local scaled portal crossing keeps accessory attachment matches", "[scene][accessory]") {
	Rig rig;
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	const auto original = rig.Store.Get<Attachment>(rig.HatPoint)->Frame;
	const auto pane = rig.Store.CreateInstance(PartClass(), "near");
	const auto far = rig.Store.CreateInstance(PartClass(), "far");
	rig.Store.Set(pane, Bounds{{8, 4.5f, .2f}});
	rig.Store.Set(far, Bounds{{16, 9, .2f}});
	rig.Store.Set(far, Transform{CFrame({100, 0, 0})});
	const auto camera = rig.Store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "seam");
	rig.Store.SetParent(camera, pane);
	SurfaceCamera surface;
	surface.Face = NormalId::Front;
	surface.Surface = 0;
	rig.Store.Set(camera, surface);
	rig.Store.Set(camera, Portal{far});
	rig.Store.Set(rig.Root, Transform{CFrame({0, 0, -1})});
	rig.Store.Set(rig.Root, PreviousTransform{CFrame({0, 0, 1})});
	REQUIRE(CrossPortals(rig.Store) == 1);
	Matched(rig.Store, rig.Hat);
	REQUIRE(
		(rig.Store.Get<Attachment>(rig.HatPoint)->Frame.Position - original.Position * 2).Magnitude() < .0001f
	);
	const auto carried = rig.Store.Get<CharacterLimb>(rig.Handle)->Offset;
	PoseCharacters(rig.Store);
	SameFrame(carried, rig.Store.Get<CharacterLimb>(rig.Handle)->Offset);
}

TEST_CASE("detaching an anchored accessory does not invent a simulated body", "[scene][accessory]") {
	Rig rig;
	rig.Store.Remove<Motion>(rig.Handle);
	rig.Store.Remove<Simulated>(rig.Handle);
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	REQUIRE(rig.Store.SetParent(rig.Hat, WorkspaceOf(rig.Store)));
	UpdateAccessoryAttachments(rig.Store);
	REQUIRE_FALSE(rig.Store.Has<Motion>(rig.Handle));
	REQUIRE_FALSE(rig.Store.Has<Simulated>(rig.Handle));
}

TEST_CASE("a detached accessory clone cannot release its original handle", "[scene][accessory]") {
	Rig rig;
	REQUIRE(EquipAccessory(rig.Store, rig.Model, rig.Hat));
	Matched(rig.Store, rig.Hat);
	const auto clone = rig.Store.CloneInstance(rig.Hat);
	REQUIRE(clone != ecs::NULL_ENTITY);
	const auto copiedHandle = rig.Store.FindFirstChild(clone, "Handle");
	REQUIRE(copiedHandle != ecs::NULL_ENTITY);
	UpdateAccessoryAttachments(rig.Store);
	REQUIRE(rig.Store.Has<CharacterLimb>(rig.Handle));
	REQUIRE_FALSE(rig.Store.Has<Motion>(rig.Handle));
	REQUIRE_FALSE(rig.Store.Has<CharacterLimb>(copiedHandle));
	REQUIRE(rig.Store.Has<Motion>(copiedHandle));
	REQUIRE(EquipAccessory(rig.Store, rig.Model, clone));
	Matched(rig.Store, clone);
	Matched(rig.Store, rig.Hat);
}
