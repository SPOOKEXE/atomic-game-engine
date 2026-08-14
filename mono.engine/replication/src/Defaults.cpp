#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/replication/Defaults.hpp>

#include <string>
#include <vector>

namespace engine::replication {

	namespace {
		// The prefix a world's shared state lives under.
		//
		// **`scene.` is the game's state.** Nothing else is included by prefix:
		// `physics.` is derived from the shared state every tick and
		// reconstructed on the far side, `script.` is a server's own, and a
		// module that wants its components on the wire says so rather than
		// inheriting it from a naming convention.
		constexpr std::string_view SHARED_PREFIX = "scene.";

		// What an instance *is*, and all three of them named rather than a
		// prefix.
		//
		// **`ecs.` used to be a prefix with one exception written into it, and
		// the exception is how two components went missing.** The rule read
		// "`scene.` or `ecs.Hierarchy`", so `ecs.InstanceName` and
		// `ecs.InstanceClass` crossed in a join snapshot — `Store::Save` carries
		// every component — and never in a delta. An entity the world already
		// held arrived named, an entity created while a client was connected
		// arrived with no name and no class, and nothing said so: a client's own
		// `Player` had four children whose names were empty strings.
		//
		// A prefix cannot fail loudly. This list can: `engine.replication.defaults`
		// walks every registered `ecs.` component and requires each one to be
		// either here or in the exclusions it names, so the next component added
		// to `ecs` is a red test rather than a silence. `AGENTS.md` rule 6.
		//
		// **Once per change, not once per creation, and the difference is the
		// v0.7 bug again.** Saying a name in the `Structure` message that creates
		// the entity would be one reliable copy and nothing per tick after it —
		// but `.Name` is a writable property a script sets whenever it likes, and
		// a fact that crosses only at birth is exactly the shape of the part that
		// was recoloured at runtime and kept its old colour on every client for
		// ever. So they are signed like every other write-once component, which
		// costs a hash of two four-byte columns per tick and **zero bytes on a
		// tick where nothing was renamed**. The recovery walk is what guarantees
		// arrival: a value stays in `Unconfirmed` and is re-offered every tick
		// until the client acknowledges a tick it was in, which is the same
		// promise every other replicated value gets and needs no second one.
		//
		// That is also the answer to what a repeated class name costs. It is
		// text on a wire — `ecs.InstanceClass` crosses as `Classes::Describe`'s
		// name, because a `ClassId` is a registration index and rule 4 forbids
		// one — but it crosses on the tick an instance is created and on no
		// other, so a per-connection string table would be a dictionary
		// negotiated to compress a message that is already only sent once. The
		// steady-state saving would be zero.
		constexpr std::string_view INSTANCE_COMPONENTS[] = {
			"ecs.Hierarchy",
			"ecs.InstanceName",
			"ecs.InstanceClass",
		};

		bool PartOfAnInstance(std::string_view component) {
			for (const std::string_view named : INSTANCE_COMPONENTS) {
				if (component == named) {
					return true;
				}
			}
			return false;
		}

		// The two written by a system every tick, so the dirty bits already
		// know.
		//
		// Hashing either would be a pass over the world to learn what was free.
		// Everything else is written once by a script and then never — observing
		// those buys a dirty column paid every tick and read never, and *not*
		// signing them is the v0.7 bug where a part recoloured at runtime kept
		// its old colour on every client for ever.
		bool WrittenEveryTick(std::string_view component) {
			return component == "scene.Transform" || component == "scene.Motion";
		}
	}

	bool LocalToTheClient(std::string_view component) {
		// **The client makes its own main camera, and the component that says
		// *which* camera that is, is the one to keep local.** `ActiveCamera`
		// names the live one and `client::AimReplicaViewer` mints a predicted
		// camera and points it there — a replica may not mint an authoritative
		// entity — so a replicated `ActiveCamera` would be a second answer to
		// which eye the world is seen through, and the two would fight every
		// frame. `CameraController` is how a machine drives its own.
		//
		// **`scene.Camera` itself must cross, and that is not a hedge.** It is a
		// *lens*, not a viewpoint: a `SurfaceCamera` carries one, so a mirror
		// with no replicated `Camera` cannot be aimed at all —
		// `AimSurfaceCameras` finds nothing, the pane samples nothing, and the
		// mirror is a flat grey rectangle on every client. That is not
		// hypothetical: excluding it broke `studio.playlink`'s "a mirror arrives
		// on the client whole", which is the case that exists to catch exactly
		// this. An authored `Camera` instance is scene content like any other.
		// **A portal proxy is a piece of another room, made and unmade inside one
		// tick.** It exists so a body standing in a hole has the far room's floor
		// under it — `physics/Portals.hpp` — and it is never the same entity two
		// ticks running, so replicating one would be a create and a destroy per
		// tick per proxy on the wire, describing geometry the client already has
		// on the other side of the pane.
		if (component == "scene.PortalProxy") {
			return true;
		}

		if (component == "scene.ActiveCamera" || component == "scene.CameraController") {
			return true;
		}

		// A client's own input and its own identity. Sending the server's copy
		// would tell every client what some other machine is pressing.
		if (component == "scene.InputState" || component == "scene.LocalPlayer") {
			return true;
		}

		// **The authority's bookkeeping about a life that has not started yet.**
		// `scene.PlayerRespawn` is a deadline `scene::UpdateRespawns` computes
		// and only the authority runs that pass, so a replicated one is a row a
		// client can do nothing with — and it is added and removed on every
		// death and spawn, which is an archetype move and a structural message
		// per respawn per player for a number nobody reads.
		//
		// `scene.PlayersService` and `scene.PlayerIdentity` are deliberately not
		// here: `MaxPlayers`, `UserId` and `DisplayName` are what a game's own
		// interface shows, and Roblox puts all three on the client too.
		//
		// **`scene.CharacterChanges` is a *queue*, and the whole of it is
		// per-machine.** Each side records its own transitions and drains them
		// into its own VM — a client learns it has a character by receiving
		// `PlayerCharacter` and rebuilding the link locally — so shipping the
		// authority's list would fire every client's `CharacterAdded` twice, once
		// for its own record and once for a copy of somebody else's.
		if (component == "scene.PlayerRespawn" || component == "scene.CharacterChanges") {
			return true;
		}

		// **A statement about hosting, not about what the world looks like.**
		// `scene.AwakeWorld` is how a game tells its host that a world with
		// nobody in it still has to tick — NPCs, an economy, a round timer. A
		// client neither needs it nor has any business setting it, and a
		// replicated one would be a client asking a server to keep a machine
		// running.
		if (component == "scene.AwakeWorld") {
			return true;
		}

		// **Derived every frame on whichever machine draws.** A previous
		// transform is what interpolation is measured from and the client builds
		// its own in `replication::SnapshotBuffer`; the render gate is computed
		// by the client's own presentation pass; the hash exists to notice a
		// change and means nothing to a receiver. Sending any of them is paying
		// wire for something the far side is about to overwrite.
		if (component == "scene.PreviousTransform" || component == "scene.Rendered" ||
			component == "scene.QuickHash") {
			return true;
		}

		// **The same argument as `scene.Camera`'s, arriving one step further
		// on.** A `SurfaceCamera` crosses because it is authored scene content;
		// the *frustum fitted to its pane* does not, because that fit is made
		// from where the local eye is standing. The authority's answer is
		// correct for the authority's camera and wrong for every client
		// watching — which is the rule `client/Replicated.hpp` states for the
		// placement, and a lens is the placement's other half.
		//
		// Both ends run `AimSurfaceCameras` and recompute it, so what crosses is
		// the mirror and never the aim. Replicating it would pay wire to send
		// every client a frustum aimed at somebody else's eye, which the
		// receiver then overwrites — wrong *and* wasteful, and wrong in a way
		// that would only show on a second machine.
		//
		// `scene.Portal` is deliberately not here: which part a portal leads to
		// is a fact about the scene, not about the viewer.
		if (component == "scene.SurfaceLens") {
			return true;
		}

		// Marked as not outliving the run that made it, so replicating it would
		// be shipping a thing whose whole point is that it is not kept.
		return component == "scene.Transient";
	}

	std::span<const ReplicatedComponent> DefaultReplicatedComponents() {
		// **Built on first use rather than at static-initialisation time**,
		// because it walks the component registry and that registry is filled by
		// `RegisterSceneComponents` during start-up. A table built before that
		// would be empty and nothing would say so.
		static const std::vector<ReplicatedComponent> table = [] {
			std::vector<ReplicatedComponent> found;

			for (size_t index = 0; index < ecs::Components::Count(); index++) {
				const ecs::TypeDescriptor &type =
					ecs::Components::Describe(ecs::ComponentId{static_cast<uint32_t>(index)});

				const std::string_view name = type.Name.Text();
				const bool shared = name.starts_with(SHARED_PREFIX) || PartOfAnInstance(name);
				if (!shared || LocalToTheClient(name)) {
					continue;
				}

				// **A type with no serialisation cannot cross and is skipped
				// rather than declared.** Declaring one would have the authority
				// refuse it per tick — a component that looks replicated,
				// reports nothing and costs a check for ever.
				if (!type.Serialisable) {
					continue;
				}

				// **And a type that is not trivially copyable cannot be
				// *signed*, which is what everything here but the two above
				// uses.** `Authority` warns and declines it, so declaring one is
				// a warning per host per run describing a component nobody meant
				// to send: the catalogues are the case — `scene.TextureCatalogue`
				// and its two siblings are resources holding maps, they hang off
				// no entity, and they arrived here only because they share the
				// prefix.
				//
				// A non-trivial component that genuinely should cross needs
				// `Observed` and a matching `Store::Observe`, which is a decision
				// per component rather than something a prefix can infer — so it
				// is named in the host that wants it rather than defaulted here.
				if (!type.Trivial && !WrittenEveryTick(name)) {
					continue;
				}

				// **A limb's transform is derived on whichever machine draws**, so
				// sending it every tick spends wire on a row `scene::PoseCharacters`
				// overwrites the moment it lands: five extra rows per character per
				// tick, each a ten-byte quantised `CFrame`, against roughly ten
				// bytes for the root alone.
				//
				// `scene.CharacterLimb` is the tag because it already means exactly
				// this — an entity carrying one *is* an entity whose frame is a
				// product of its root and its rest offset. Nothing new had to be
				// declared, which is also why this needed no second consumer to
				// check the idea against: the second consumer would have wanted the
				// same tag it already has.
				//
				// The offsets still cross, because `scene.CharacterLimb` is itself
				// replicated and is not what this filters. Only `scene.Transform`
				// rows for those entities stop, and only as deltas — the baseline a
				// newly admitted client receives still carries one copy, so its
				// first frame is right before any derivation has run. `D00115`.
				const bool derived = name == "scene.Transform";

				found.push_back(
					ReplicatedComponent{
						name,
						WrittenEveryTick(name) ? ChangeDetection::Observed : ChangeDetection::Signature,
						derived ? std::string_view("scene.CharacterLimb") : std::string_view(),
					}
				);
			}
			return found;
		}();
		return table;
	}
}
