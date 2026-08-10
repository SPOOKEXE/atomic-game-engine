#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>
#include <engine/replication/Defaults.hpp>

#include <string>
#include <vector>

namespace engine::replication {

	namespace {
		// The prefixes a world's shared state lives under.
		//
		// **`scene.` is the game's state and `ecs.Hierarchy` is where it hangs
		// from.** Nothing else is included by prefix: `physics.` is derived from
		// the shared state every tick and reconstructed on the far side,
		// `script.` is a server's own, and a module that wants its components on
		// the wire says so rather than inheriting it from a naming convention.
		constexpr std::string_view SHARED_PREFIX = "scene.";
		constexpr std::string_view HIERARCHY = "ecs.Hierarchy";

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
		if (component == "scene.ActiveCamera" || component == "scene.CameraController") {
			return true;
		}

		// A client's own input and its own identity. Sending the server's copy
		// would tell every client what some other machine is pressing.
		if (component == "scene.InputState" || component == "scene.LocalPlayer") {
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
				const bool shared = name.starts_with(SHARED_PREFIX) || name == HIERARCHY;
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

				found.push_back(
					ReplicatedComponent{
						name,
						WrittenEveryTick(name) ? ChangeDetection::Observed : ChangeDetection::Signature,
					}
				);
			}
			return found;
		}();
		return table;
	}
}
