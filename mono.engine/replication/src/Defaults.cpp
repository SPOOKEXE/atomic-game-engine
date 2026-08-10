#include <engine/replication/Defaults.hpp>

namespace engine::replication {

	std::span<const ReplicatedComponent> DefaultReplicatedComponents() {
		// **Ordered by what a client cannot draw without.** Position first,
		// then what gives a part a shape, then what gives it a surface, then the
		// tree and the cameras — so a reader can see at a glance what a replica
		// would be missing if the list were truncated.
		static constexpr ReplicatedComponent TABLE[] = {
			// Written every tick by a system, so the dirty bits already know.
			// Hashing these would be a pass over the world to learn what was
			// free.
			{"scene.Transform", ChangeDetection::Observed},
			{"scene.Motion", ChangeDetection::Observed},

			// Written once by a script and then never. Observing them would buy
			// a dirty column paid every tick and read never — and *not* signing
			// them is the v0.7 bug where a part recoloured at runtime kept its
			// old colour on every client for ever.
			//
			// They cross at all because a client that received a position and no
			// size has nothing to draw.
			{"scene.Bounds", ChangeDetection::Signature},
			{"scene.Visual", ChangeDetection::Signature},

			// What an imported mesh is drawn with. A client that received a mesh
			// name and no texture name has half a model.
			{"scene.SurfaceAppearance", ChangeDetection::Signature},

			// **The masks cross and the names do not**, and that is a stated gap
			// rather than a bug: `scene::TagTable` is a resource and resources
			// have no wire form, so a replica's table stays empty and
			// `HasTag(name)` there answers false. Mask-against-mask filtering is
			// unaffected, because both sides of that comparison come from this
			// authority.
			{"scene.Tags", ChangeDetection::Signature},

			// The mirror and the lens it is aimed with. Without the first a
			// surface camera does not exist on a replica at all, so a pane
			// samples nothing and the mirror is a flat grey rectangle.
			{"scene.SurfaceCamera", ChangeDetection::Signature},
			{"scene.Camera", ChangeDetection::Signature},

			// The tree, because a surface camera is aimed off the part it is
			// parented to and a replica with no parent link cannot find it.
			{"ecs.Hierarchy", ChangeDetection::Signature},
		};
		return TABLE;
	}
}
