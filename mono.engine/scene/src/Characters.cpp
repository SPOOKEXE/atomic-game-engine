#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Color3;
		using core::Vector3;

		// One limb of the rig: what it is called, how big it is, where it sits
		// relative to the root, and which of the three colours it takes.
		//
		// **A table rather than six blocks of code**, because every number here
		// has to add up to `CHARACTER_HEIGHT` and a reader has to be able to
		// check that by looking. The offsets are from the *centre* of the
		// character, which is where the root part sits.
		enum class Palette : uint8_t { Skin, Shirt, Trousers };

		struct LimbDesc {
			std::string_view Name;
			Vector3 Size;
			Vector3 Offset;
			Palette Colour;
		};

		// Feet at -2.5, head crown at +2.5. Legs 2 tall, torso 2, head 1.
		constexpr std::array<LimbDesc, 6> LIMBS{{
			{"Head", {1.5f, 1.0f, 1.5f}, {0.0f, 2.0f, 0.0f}, Palette::Skin},
			{"Torso", {2.0f, 2.0f, 1.0f}, {0.0f, 0.5f, 0.0f}, Palette::Shirt},
			{"Left Arm", {1.0f, 2.0f, 1.0f}, {-1.5f, 0.5f, 0.0f}, Palette::Skin},
			{"Right Arm", {1.0f, 2.0f, 1.0f}, {1.5f, 0.5f, 0.0f}, Palette::Skin},
			{"Left Leg", {1.0f, 2.0f, 1.0f}, {-0.5f, -1.5f, 0.0f}, Palette::Trousers},
			{"Right Leg", {1.0f, 2.0f, 1.0f}, {0.5f, -1.5f, 0.0f}, Palette::Trousers},
		}};

		Color3 ColourOf(const CharacterDesc &desc, Palette which) {
			switch (which) {
			case Palette::Skin:
				return desc.SkinColour;
			case Palette::Shirt:
				return desc.TorsoColour;
			case Palette::Trousers:
				return desc.LegColour;
			}
			return desc.SkinColour;
		}

		// Takes a limb out of the broad phase entirely.
		//
		// **Both masks cleared, rather than `Collider::Trigger`.** A trigger is
		// still a pair the solver considers and still produces an event per
		// frame per limb per contact — five limbs standing inside their own root
		// would report thirty overlaps a tick that nothing reads. An empty layer
		// is never a candidate, which is the ECS-native form of the same wish.
		void MakeIntangible(ecs::Store &store, ecs::Entity part) {
			Collider *collider = store.GetMutable<Collider>(part);
			if (collider == nullptr) {
				return;
			}
			collider->Layer = spatial::LayerMask::None();
			collider->Mask = spatial::LayerMask::None();
		}

		// Finds the humanoid steering a model and the part physics moves for it.
		//
		// **Both arrangements this engine allows, resolved in one place.** A
		// character rig puts the humanoid beside the parts and names one of them;
		// a scripted NPC puts the humanoid on the part itself. `StepCharacters`
		// and `GroundCharacters` both branch on `Humanoid::RootPart` for exactly
		// this, and the whole point of resolving it here is that they never have
		// to guess again — `SetPlayerCharacter` writes the field back.
		bool
		ResolveRig(const ecs::Store &store, ecs::Entity model, ecs::Entity &root, ecs::Entity &humanoid) {
			humanoid = store.Has<Humanoid>(model)
						   ? model
						   : store.FindFirstChildWhichIsA(model, HumanoidClass(), true);

			const Humanoid *steering = humanoid == ecs::NULL_ENTITY ? nullptr : store.Get<Humanoid>(humanoid);
			if (steering == nullptr) {
				return false;
			}

			// **The humanoid's own answer first**, so a rig that already knows
			// its body keeps it — a model with two parts one of which happens to
			// be called `HumanoidRootPart` should not overrule the field.
			root = steering->RootPart;
			if (root == ecs::NULL_ENTITY || !store.Alive(root)) {
				root = store.FindFirstChild(model, "HumanoidRootPart", true);
			}

			// **The model itself is the last resort and a legitimate one.** A
			// scripted character is often one part with a humanoid on it, and
			// refusing that would make the simple case the unsupported one.
			if (root == ecs::NULL_ENTITY && store.Has<Transform>(model)) {
				root = model;
			}

			return root != ecs::NULL_ENTITY;
		}

		// Takes the intent off a body that is no longer anybody's.
		//
		// Without this a character released mid-stride keeps its last
		// `MoveDirection` for ever: `UpdateCharacterControl` only writes the
		// humanoid it is allowed to drive, so once there is no owner there is
		// nothing left to write a zero.
		void StopCharacter(ecs::Store &store, ecs::Entity model) {
			const Character *rig = model == ecs::NULL_ENTITY ? nullptr : store.Get<Character>(model);
			if (rig == nullptr) {
				return;
			}

			if (Humanoid *humanoid = store.GetMutable<Humanoid>(rig->Humanoid); humanoid != nullptr) {
				humanoid->MoveDirection = Vector3{};
				humanoid->JumpRequested = false;
			}
		}
	}

	ecs::Entity MakeCharacter(ecs::Store &store, const CharacterDesc &desc) {
		const ecs::Entity workspace = WorkspaceOf(store);
		if (workspace == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		// **`Frame` is the feet and the root is the centre.** The conversion
		// happens here and nowhere else — `CharacterDesc::Frame` says why the
		// argument is the feet, and a second place that added half a height is
		// the place the two would disagree.
		const CFrame centre = desc.Frame * CFrame(Vector3{0.0f, CHARACTER_HEIGHT * 0.5f, 0.0f});

		const ecs::Entity model = store.CreateInstance(ModelClass(), desc.Name);
		if (model == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}
		store.SetParent(model, workspace);
		store.Set(model, Transform{centre});

		// The whole capsule as one box. **Not a capsule shape**: the ground
		// query is a downward ray and the walls a character meets are boxes, so
		// a box collider is what the solver already does well and a rounded one
		// would buy a slide over a step edge that nothing here asks for.
		PartDesc rootDesc;
		rootDesc.Frame = centre;
		rootDesc.Size = Vector3{2.0f, CHARACTER_HEIGHT, 1.0f};
		rootDesc.Anchored = false;

		const ecs::Entity root = MakePart(store, rootDesc);
		store.SetInstanceName(root, "HumanoidRootPart");
		store.SetParent(root, model);

		// **Invisible, because the six visible parts are the character.** A
		// seventh box the size of all of them would be a coffin around it.
		if (Visual *visual = store.GetMutable<Visual>(root)) {
			visual->Visible = false;
		}

		for (const LimbDesc &limb : LIMBS) {
			PartDesc part;
			part.Frame = centre * CFrame(limb.Offset);
			part.Size = limb.Size;

			// **Anchored, so they carry no `RigidBody` and no `Motion`.** They
			// are placed by `PoseCharacters` and integrating them would be the
			// solver fighting that pass every tick.
			part.Anchored = true;

			const ecs::Entity entity = MakePart(store, part);
			store.SetInstanceName(entity, limb.Name);
			store.SetParent(entity, model);

			if (Visual *visual = store.GetMutable<Visual>(entity)) {
				visual->Tint = ColourOf(desc, limb.Colour);
			}

			MakeIntangible(store, entity);
			store.Set(entity, CharacterLimb{CFrame(limb.Offset), root, 0});
		}

		// The model follows the root like everything else does — see
		// `CharacterLimb` for why it is a row here rather than a case in the
		// pass.
		store.Set(model, CharacterLimb{CFrame(), root, 0});

		// **A sibling of the parts, which is Roblox's arrangement and the one
		// `Part.cpp` registered the class for.** A humanoid on the root part
		// would make `character.Humanoid` a lie.
		const ecs::Entity humanoid = store.CreateInstance(HumanoidClass(), "Humanoid");
		store.SetParent(humanoid, model);

		Humanoid steering;
		steering.Height = CHARACTER_HEIGHT;
		steering.Radius = 1.0f;

		// **What makes the humanoid steer a part it is not on.** `Humanoid::
		// RootPart` carries the argument; without it `StepCharacters` would look
		// for a `Motion` on the humanoid instance, which has no place in the
		// world at all.
		steering.RootPart = root;
		store.Set(humanoid, steering);

		store.Set(model, Character{root, humanoid, ecs::NULL_ENTITY});
		return model;
	}

	ecs::Entity LoadCharacter(ecs::Store &store, ecs::Entity player, const CFrame &frame) {
		if (!store.Alive(player) || !store.IsA(player, PlayerClass())) {
			return ecs::NULL_ENTITY;
		}

		RemoveCharacter(store, player);

		CharacterDesc desc;
		desc.Frame = frame;

		// **Named after the player**, so `workspace:FindFirstChild(player.Name)`
		// is the lookup a game script already knows how to write.
		const core::Name name = store.InstanceNameOf(player);
		const std::string label = name.IsValid() ? std::string(name.Text()) : std::string("Character");
		desc.Name = label;

		const ecs::Entity model = MakeCharacter(store, desc);
		if (model == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		// **Through the same door a script uses**, which is the whole reason
		// `SetPlayerCharacter` exists: ownership, the `Character::Owner` back
		// reference and the `PlayerCharacter` row are one decision, and a second
		// copy of it here is the copy that would drift.
		if (!SetPlayerCharacter(store, player, model)) {
			return ecs::NULL_ENTITY;
		}

		// **The camera is not aimed here.** `FollowOwnCharacter` is the one rule
		// and it runs every frame — see its header for why the moment of
		// spawning is the wrong place to decide what a viewer is looking at.
		return model;
	}

	ecs::Entity LoadCharacter(ecs::Store &store, ecs::Entity player) {
		return LoadCharacter(store, player, FindSpawn(store));
	}

	bool RemoveCharacter(ecs::Store &store, ecs::Entity player) {
		const ecs::Entity model = CharacterOf(store, player);
		if (model == ecs::NULL_ENTITY) {
			return false;
		}

		// **Released before it is destroyed**, so the release can still read the
		// rig it is stopping. Reversed, `SetPlayerCharacter` would be handed a
		// dead model and the humanoid's last `MoveDirection` would go with it —
		// harmless for a body about to vanish, and exactly the ordering mistake
		// that stops being harmless the first time somebody reuses the model.
		(void)SetPlayerCharacter(store, player, ecs::NULL_ENTITY);

		// **The model, which takes its children with it.** Destroying the root
		// alone would leave five limbs following an entity that is not alive —
		// which `PoseCharacters` handles by leaving them where they fell, and
		// which is still a pile of limbs nobody asked for.
		store.DestroyInstance(model);
		return true;
	}

	bool SetPlayerCharacter(ecs::Store &store, ecs::Entity player, ecs::Entity model) {
		if (!store.Alive(player) || !store.IsA(player, PlayerClass())) {
			return false;
		}

		const ecs::Entity previous = CharacterOf(store, player);

		if (model == ecs::NULL_ENTITY) {
			StopCharacter(store, previous);
			if (Character *rig = store.GetMutable<Character>(previous); rig != nullptr) {
				rig->Owner = ecs::NULL_ENTITY;
			}
			store.Set(player, PlayerCharacter{ecs::NULL_ENTITY});
			return true;
		}

		if (!store.Alive(model)) {
			return false;
		}

		ecs::Entity root = ecs::NULL_ENTITY;
		ecs::Entity humanoid = ecs::NULL_ENTITY;
		if (!ResolveRig(store, model, root, humanoid)) {
			return false;
		}

		// **The old body is released before the new one is taken**, so a respawn
		// does not leave the previous character walking. Skipped when the model
		// is the one already held, which is what makes re-assignment idempotent
		// and `LinkPlayerCharacters` cheap to run every tick.
		if (previous != ecs::NULL_ENTITY && previous != model) {
			StopCharacter(store, previous);
			if (Character *rig = store.GetMutable<Character>(previous); rig != nullptr) {
				rig->Owner = ecs::NULL_ENTITY;
			}
		}

		// **Written back, so every later pass resolves the body this did.** A
		// humanoid that *is* the body keeps a null field — that is how
		// `StepCharacters` spells "the row itself", and setting it to the row
		// would be a second spelling of one arrangement.
		if (Humanoid *steering = store.GetMutable<Humanoid>(humanoid); steering != nullptr) {
			steering->RootPart = root == humanoid ? ecs::NULL_ENTITY : root;
		}

		store.Set(model, Character{root, humanoid, player});
		store.Set(player, PlayerCharacter{model});

		// **The client may move its own body and nobody else's**, which is the
		// whole of what ownership buys a character. The limbs are anchored and
		// have nothing to own.
		(void)SetNetworkOwner(store, root, player);
		return true;
	}

	size_t ReclaimOrphanedCharacters(ecs::Store &store) {
		// **Gathered before anything is destroyed**, for the reason
		// `LinkPlayerCharacters` gives about its own list: `DestroyInstance`
		// walks the model's children and removes rows, and doing that under an
		// `Each` is the iteration invalidating itself. On a settled world the
		// vector stays empty, which is every tick but the one somebody left on.
		std::vector<ecs::Entity> orphaned;

		store.Each<const Character>([&](ecs::Entity model, const Character &rig) {
			// An owner that is null is an NPC and never had a player to lose.
			if (rig.Owner == ecs::NULL_ENTITY || store.Alive(rig.Owner)) {
				return;
			}
			orphaned.push_back(model);
		});

		size_t removed = 0;
		for (const ecs::Entity model : orphaned) {
			// Alive again on the way out: two `Character` rows naming one dead
			// owner is not a shape this builds, but a destroy that took a
			// second model with it as a child would make the handle stale here.
			if (!store.Alive(model)) {
				continue;
			}
			store.DestroyInstance(model);
			removed++;
		}

		return removed;
	}

	size_t LinkPlayerCharacters(ecs::Store &store) {
		// **Gathered before anything is written.** `SetPlayerCharacter` adds a
		// `Character` to a model that had none, and an archetype move under an
		// `Each` is the iteration invalidating itself. The vector holds only the
		// players whose link is *wrong*, which on a settled world is none of
		// them — the allocation is the respawn frame's and no other.
		std::vector<std::pair<ecs::Entity, ecs::Entity>> pending;

		store.Each<const PlayerCharacter>([&](ecs::Entity player, const PlayerCharacter &held) {
			// A model destroyed under its player releases the player rather than
			// leaving the reference dangling. `CharacterOf` already answers null
			// for it; this is what stops the body it left behind.
			if (held.Model != ecs::NULL_ENTITY && !store.Alive(held.Model)) {
				pending.emplace_back(player, ecs::NULL_ENTITY);
				return;
			}

			if (held.Model == ecs::NULL_ENTITY) {
				return;
			}

			// The link is already good when the model names this player back and
			// the humanoid it names is still there. That is the every-tick case
			// and it costs two component reads.
			const Character *rig = store.Get<Character>(held.Model);
			if (rig != nullptr && rig->Owner == player && store.Alive(rig->Humanoid)) {
				return;
			}

			pending.emplace_back(player, held.Model);
		});

		size_t linked = 0;
		for (const auto &[player, model] : pending) {
			if (SetPlayerCharacter(store, player, model)) {
				linked++;
				continue;
			}

			// **A model that cannot be a character releases the player.** An
			// assignment that never resolved is a scene mistake, and a player
			// stuck pointing at a model nothing can drive would retry it every
			// tick for the life of the world.
			(void)SetPlayerCharacter(store, player, ecs::NULL_ENTITY);
		}

		return linked;
	}

	ecs::Entity CharacterOf(const ecs::Store &store, ecs::Entity player) {
		const PlayerCharacter *held = store.Get<PlayerCharacter>(player);
		if (held == nullptr || !store.Alive(held->Model)) {
			return ecs::NULL_ENTITY;
		}
		return held->Model;
	}

	ecs::Entity PlayerOf(const ecs::Store &store, ecs::Entity character) {
		const Character *body = store.Get<Character>(character);
		if (body == nullptr || !store.Alive(body->Owner)) {
			// **The liveness check is the half worth having**, and it is the same
			// one `CharacterOf` makes for the same reason: a model outliving its
			// player by a frame is an ordinary state during a disconnect, and
			// handing back a dead handle would be a script holding something it
			// can index and cannot use.
			return ecs::NULL_ENTITY;
		}
		return body->Owner;
	}

	CFrame FindSpawn(const ecs::Store &store) {
		const ecs::Entity workspace = WorkspaceOf(store);
		if (workspace == ecs::NULL_ENTITY) {
			return {};
		}

		const ecs::Entity spawn = store.FindFirstChild(workspace, "SpawnLocation", true);
		if (spawn == ecs::NULL_ENTITY) {
			return {};
		}

		const Transform *placement = store.Get<Transform>(spawn);
		if (placement == nullptr) {
			return {};
		}

		// **The top face, so a character stands on the pad rather than in it.**
		// A spawn with no `Bounds` is a bare point in space and is taken as it
		// is, which is what an `Attachment` used as a spawn would be.
		const Bounds *bounds = store.Get<Bounds>(spawn);
		const float lift = bounds == nullptr ? 0.0f : bounds->HalfExtent.Y;
		return placement->Frame * CFrame(Vector3{0.0f, lift, 0.0f});
	}

	bool FollowOwnCharacter(ecs::Store &store) {
		const LocalPlayer *local = store.Resource<LocalPlayer>();
		if (local == nullptr) {
			return false;
		}

		const Character *character = store.Get<Character>(CharacterOf(store, local->Instance));
		const ecs::Entity wanted = character == nullptr ? ecs::NULL_ENTITY : character->Root;

		auto *camera = store.ResourceMutable<CameraController>();
		if (camera == nullptr || camera->Subject == wanted) {
			return false;
		}

		// **A `Scriptable` camera is left alone**, because a cutscene that took
		// the camera must keep it across a respawn — the same refusal
		// `UpdateCameraControl` and `PlaceCamera` already make, and for the same
		// reason: two writers and the last one wins.
		if (camera->Mode == CameraMode::Scriptable) {
			return false;
		}

		camera->Subject = wanted;
		return true;
	}

	size_t PoseCharacters(ecs::Store &store) {
		size_t placed = 0;

		store.Each<const CharacterLimb, Transform>(
			[&](ecs::Entity, const CharacterLimb &limb, Transform &placement) {
				const Transform *root = store.Get<Transform>(limb.Root);
				if (root == nullptr) {
					// A dead root is a character being torn down. Left where it is
					// — see the header for why that beats the origin.
					return;
				}

				placement.Frame = root->Frame * limb.Offset;
				placed++;
			}
		);

		return placed;
	}

	ecs::ClassId ModelClass() {
		static const ecs::ClassId model = (EnsureClassTree(), ecs::Classes::Find(core::Name("Model")));
		return model;
	}
}
