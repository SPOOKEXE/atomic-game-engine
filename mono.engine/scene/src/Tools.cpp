#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Tools.hpp>

#include <cstring>
#include <vector>

namespace engine::scene {

	namespace {
		using core::CFrame;
		using core::Vector3;

		// The character a tool is being held by, or a null entity.
		//
		// **The immediate parent and never an ancestor**, which is Roblox's rule
		// and is also what keeps the answer from being ambiguous: a tool sitting
		// in a `Folder` a script put inside a character is stored there, not
		// held, and a walk up the chain could not tell the two apart.
		ecs::Entity HolderOf(const ecs::Store &store, ecs::Entity tool) {
			const ecs::Entity parent = store.ParentOf(tool);
			return store.Get<Character>(parent) == nullptr ? ecs::NULL_ENTITY : parent;
		}

		// What the handle's limb row should be, for a tool held by `character`.
		CharacterLimb WantedLimb(const ecs::Store &store, ecs::Entity character, const Tool &held) {
			const Character *rig = store.Get<Character>(character);
			return CharacterLimb{rig->Root, ToolGrip(store, character) * held.Grip, 0};
		}

		// Whether a handle already carries exactly the row it should.
		//
		// **A byte comparison, and it is exact only because `CharacterLimb` has
		// no holes.** Its members are ordered widest-first for that reason -
		// see the struct, where the eight bytes this comparison found are
		// written up. A field-by-field comparison would be the weaker test
		// anyway: it silently stops covering the next member somebody adds,
		// which is `WriteVisuals`' documented failure mode one file over.
		//
		// Compared at all rather than written unconditionally, because a `Set`
		// marks the row changed - and an equipped tool would then put a
		// `scene.CharacterLimb` delta on the wire every tick, for ever.
		bool SameLimb(const CharacterLimb &left, const CharacterLimb &right) {
			return std::memcmp(&left, &right, sizeof(CharacterLimb)) == 0;
		}

		// Makes one handle agree with where its tool is parented.
		//
		// Split out because three callers want it for one tool and one wants it
		// for all of them, and a second copy of "what a held handle looks like"
		// is the shape `scene/AGENTS.md` refuses everywhere else.
		void HangHandle(ecs::Store &store, ecs::Entity handle, const CharacterLimb &limb, bool held) {
			if (!held) {
				// **Nothing to release is not the same as a release**, and the
				// difference is a write: a stowed tool nobody ever equipped
				// would otherwise have its handle's `Motion` zeroed on every
				// call, which marks the row changed and puts it on a wire.
				if (!store.Has<CharacterLimb>(handle)) {
					return;
				}
				store.Remove<CharacterLimb>(handle);

				// **Only where there is a body to move.** An anchored handle has
				// no `RigidBody` and must not gain a `Motion` - that pair is what
				// `Anchored` means, and handing one back would silently unanchor
				// a part the author anchored.
				if (store.Has<RigidBody>(handle)) {
					store.Set(handle, Motion{});
				}
				return;
			}

			store.Set(handle, limb);
			store.Remove<Motion>(handle);
		}

		// The one tool's worth of `UpdateToolGrips`, for the callers that have
		// just moved a specific tool and should not pay for a walk of the rest.
		void RegripTool(ecs::Store &store, ecs::Entity tool) {
			const ecs::Entity handle = ToolHandle(store, tool);
			if (handle == ecs::NULL_ENTITY) {
				return;
			}

			const Tool *held = store.Get<Tool>(tool);
			const ecs::Entity character = HolderOf(store, tool);
			if (held == nullptr || character == ecs::NULL_ENTITY) {
				HangHandle(store, handle, CharacterLimb{}, false);
				return;
			}

			HangHandle(store, handle, WantedLimb(store, character, *held), true);
		}
	}

	ecs::ClassId ToolClass() {
		static const ecs::ClassId tool = (EnsureClassTree(), ecs::Classes::Find(core::Name("Tool")));
		return tool;
	}

	ecs::Entity ToolHandle(const ecs::Store &store, ecs::Entity tool) {
		return store.FindFirstChild(tool, TOOL_HANDLE_NAME);
	}

	ecs::Entity EquippedTool(const ecs::Store &store, ecs::Entity character) {
		return store.FindFirstChildWhichIsA(character, ToolClass());
	}

	CFrame ToolGrip(const ecs::Store &store, ecs::Entity character) {
		const Character *rig = store.Get<Character>(character);
		if (rig == nullptr) {
			return {};
		}

		const ecs::Entity arm = store.FindFirstChild(character, RIGHT_ARM_NAME);
		const CharacterLimb *limb = store.Get<CharacterLimb>(arm);

		// **The limb has to hang off *this* rig's root**, or the offset means
		// nothing: a model with a part called `Right Arm` that is not part of the
		// formation would put the handle wherever that part's own frame implied.
		if (limb == nullptr || limb->Root != rig->Root) {
			return {};
		}

		// The bottom face of the arm, which is the hand. A limb with no `Bounds`
		// is a bare point and is taken as it is - the same reading `FindSpawn`
		// gives a spawn pad with no bounds.
		const Bounds *bounds = store.Get<Bounds>(arm);
		const float drop = bounds == nullptr ? 0.0f : bounds->HalfExtent.Y;
		return limb->Offset * CFrame(Vector3{0.0f, -drop, 0.0f});
	}

	bool EquipTool(ecs::Store &store, ecs::Entity character, ecs::Entity tool) {
		// **Refused in a replica, and this is the authority decision written
		// where it can be enforced.** See the declaration: a client that can
		// reparent its own tool can duplicate it, and the write survives exactly
		// until the next delta.
		if (store.AdoptOnly()) {
			return false;
		}

		if (!store.Alive(tool) || !store.IsA(tool, ToolClass())) {
			return false;
		}

		if (!store.Alive(character) || store.Get<Character>(character) == nullptr) {
			return false;
		}

		const ecs::Entity already = EquippedTool(store, character);
		if (already == tool) {
			// Idempotent, so a caller polling this costs one child walk. The
			// regrip still runs, because the arm may have moved under it.
			RegripTool(store, tool);
			return true;
		}

		// **A hand that cannot be emptied refuses the swap**, rather than ending
		// up with two tools in it. That happens for a character with no owner to
		// have a `Backpack` - see `UnequipTool` - and refusing it here is the
		// mistake caught where it was made, which is `SetPlayerCharacter`'s rule
		// about a model with no humanoid.
		if (already != ecs::NULL_ENTITY && !UnequipTool(store, already)) {
			return false;
		}

		if (!store.SetParent(tool, character)) {
			return false;
		}

		RegripTool(store, tool);
		return true;
	}

	bool UnequipTool(ecs::Store &store, ecs::Entity tool) {
		if (store.AdoptOnly()) {
			return false;
		}

		if (!store.Alive(tool) || !store.IsA(tool, ToolClass())) {
			return false;
		}

		const ecs::Entity character = HolderOf(store, tool);
		if (character == ecs::NULL_ENTITY) {
			return false;
		}

		const Character *rig = store.Get<Character>(character);
		const ecs::Entity backpack = rig->Owner == ecs::NULL_ENTITY
										 ? ecs::NULL_ENTITY
										 : store.FindFirstChild(rig->Owner, BACKPACK_NAME);
		if (backpack == ecs::NULL_ENTITY) {
			// An NPC's tool, or a player built without going through
			// `AddPlayer`. Left in the hand rather than put somewhere this
			// module invented - see the declaration.
			return false;
		}

		if (!store.SetParent(tool, backpack)) {
			return false;
		}

		RegripTool(store, tool);
		return true;
	}

	size_t UpdateToolGrips(ecs::Store &store) {
		// **Gathered before anything is written**, for `LinkPlayerCharacters`'
		// reason: adding or removing a `CharacterLimb` is an archetype move, and
		// an archetype move under a walk is the walk invalidating itself. The
		// vector is empty on a settled world, which is every tick but the one
		// somebody equipped something on.
		struct Pending {
			ecs::Entity Handle;
			CharacterLimb Limb;
			bool Held;
		};
		std::vector<Pending> pending;

		store.Each<const Tool>([&](ecs::Entity tool, const Tool &held) {
			const ecs::Entity handle = ToolHandle(store, tool);
			if (handle == ecs::NULL_ENTITY) {
				return;
			}

			const CharacterLimb *worn = store.Get<CharacterLimb>(handle);
			const ecs::Entity character = HolderOf(store, tool);

			if (character == ecs::NULL_ENTITY) {
				if (worn != nullptr) {
					pending.push_back({handle, CharacterLimb{}, false});
				}
				return;
			}

			const CharacterLimb wanted = WantedLimb(store, character, held);
			if (worn == nullptr || !SameLimb(*worn, wanted)) {
				pending.push_back({handle, wanted, true});
			}
		});

		for (const Pending &one : pending) {
			HangHandle(store, one.Handle, one.Limb, one.Held);
		}
		return pending.size();
	}
}
