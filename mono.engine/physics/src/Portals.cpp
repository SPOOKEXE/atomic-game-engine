#include "WorldResource.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Portals.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace engine::physics {

	namespace {
		// How many far-side colliders one body's proxy pass will take.
		//
		// **A ceiling rather than a budget**, and it is generous on purpose: what
		// is being copied is whatever stands within a body's own reach of the far
		// pane, which in a room is a floor, a wall or two and whatever is on
		// them. A doorway that overflows this is a doorway with a crowd of
		// scenery in it, and the overflow is reported rather than silently
		// dropped - a body held up by *some* of a floor is worse than one held up
		// by none, because it looks like physics rather than like a missing
		// feature.
		constexpr size_t MAX_PROXIES_PER_BODY = 32;

		// The map from the far side back to this one.
		//
		// **The far pane's own seam, which is this one's exact inverse.**
		// `scene::SeamMapping` states one map per pane, and a pair's two maps
		// compose to the identity - so there is no inverse to derive and no
		// second arithmetic to get wrong. A pane whose partner is missing from
		// the gathered set has no inverse and no far room to reach into, which is
		// the cross-world case and one this pass has nothing to say about.
		const scene::PortalSeam *
		PartnerOf(const std::vector<scene::PortalSeam> &seams, const scene::PortalSeam &seam) {
			for (const scene::PortalSeam &other : seams) {
				if (other.Pane == seam.Far && !other.Crosses) {
					return &other;
				}
			}
			return nullptr;
		}
	}

	size_t GhostPortalBodies(ecs::Store &store) {
		ENGINE_PROFILE("portal proxies");

		// **A world with no physics in it is not a world with a bug in it.** The
		// character systems are installed by hosts that never call
		// `PreparePhysicsWorld` - a scene of anchored parts and a camera needs no
		// solver - and the overlap below reports a missing world as an error
		// once per body per tick, which is a log nobody can read past.
		//
		// The registration check comes first because `HasResource` registers the
		// type under the compiler's spelling to answer, and this is reached by
		// exactly the hosts that never registered it - `WorldResource.hpp` names
		// the abort that follows.
		if (!PhysicsWorldRegistered() || !store.HasResource<PhysicsWorld>()) {
			return 0;
		}

		static thread_local std::vector<scene::PortalSeam> seams;
		if (scene::GatherPortalSeams(store, seams) == 0) {
			return 0;
		}

		// **Gathered before anything is created**, because creating an entity
		// moves rows and a walk that saw its own output would proxy the proxies -
		// a proxy straddles the pane it was mapped through by construction.
		struct Straddler {
			ecs::Entity Body;
			core::Vector3 At;
			float Reach = 0.0f;
		};

		static thread_local std::vector<Straddler> standing;
		standing.clear();

		store.Each<const scene::Motion, const scene::Transform, const scene::Bounds, const scene::Collider>(
			[](ecs::Entity body,
			   const scene::Motion &,
			   const scene::Transform &placement,
			   const scene::Bounds &bounds,
			   const scene::Collider &) {
				standing.push_back(Straddler{body, placement.Frame.Position, bounds.HalfExtent.Magnitude()});
			}
		);

		if (standing.empty()) {
			return 0;
		}

		size_t placed = 0;
		std::array<ecs::Entity, MAX_PROXIES_PER_BODY> found{};

		for (const Straddler &straddler : standing) {
			for (const scene::PortalSeam &seam : seams) {
				// A cross-world pane's destination is a camera stand-in in this
				// world rather than a room, so there is nothing on its far side
				// to stand on. The same rule the copy pass has.
				if (seam.Crosses || straddler.Body == seam.Pane || straddler.Body == seam.Far) {
					continue;
				}

				if (!scene::SeamStraddled(seam, straddler.At, straddler.Reach)) {
					continue;
				}

				const scene::PortalSeam *partner = PartnerOf(seams, seam);
				if (partner == nullptr) {
					continue;
				}

				// This side to the far side, and back again. The body's far half
				// is at `there`, so what has to be copied here is whatever stands
				// within its reach of *that*.
				const scene::SeamTransform there = scene::SeamMapping(seam);
				const scene::SeamTransform back = scene::SeamMapping(*partner);

				const core::Vector3 at = there.Point(straddler.At);
				const float radius = there.Length(straddler.Reach);

				const spatial::QueryResult result =
					OverlapSphere(store, at, radius, spatial::LayerMask::All(), found);

				if (result.Overflowed) {
					ENGINE_WARN(
						"portal proxy: more than {} colliders within reach of a body in a seam; the "
						"rest are not holding it up",
						MAX_PROXIES_PER_BODY
					);
				}

				for (size_t index = 0; index < result.Written; index++) {
					const ecs::Entity far = found[index];

					// **The panes themselves are never copied.** A pane is a
					// trigger by `scene::OpenPortals` and would solve nothing, and
					// a copy of the far pane lands exactly on the near one.
					if (far == seam.Pane || far == seam.Far || far == straddler.Body) {
						continue;
					}

					// **Static geometry only.** A dynamic body on the far side is
					// simulated there; copying it here would be a second copy of
					// one body with its own momentum, and the two would fight
					// through the wall between them.
					if (store.Has<scene::Motion>(far) || store.Has<scene::PortalProxy>(far)) {
						continue;
					}

					const auto *placement = store.Get<scene::Transform>(far);
					const auto *shape = store.Get<scene::Collider>(far);
					if (placement == nullptr || shape == nullptr) {
						continue;
					}

					// **Parented to nothing**, which is what keeps it out of every
					// pass that walks the tree: `SyncRendered` never marks it, the
					// serialiser never reaches it, and an explorer cannot show it.
					const ecs::Entity proxy = store.Create();

					store.Set(proxy, scene::Transform{back.Place(placement->Frame)});
					store.Set(proxy, scene::Bounds{shape->Extent * back.Scale});

					scene::Collider copy = *shape;
					copy.Extent = shape->Extent * back.Scale;
					store.Set(proxy, copy);

					store.Set(proxy, scene::PortalProxy{straddler.Body});
					placed++;
				}

				// One seam per body, for `CutAndCloneSeams`' reason: a body inside
				// two panes at once is at the line where two holes meet, and two
				// far rooms copied into one would be two floors at one height.
				break;
			}
		}

		return placed;
	}

	size_t RetirePortalProxies(ecs::Store &store) {
		static thread_local std::vector<ecs::Entity> spent;
		spent.clear();

		store.Each<const scene::PortalProxy>([](ecs::Entity proxy, const scene::PortalProxy &) {
			spent.push_back(proxy);
		});

		for (const ecs::Entity proxy : spent) {
			store.Destroy(proxy);
		}

		return spent.size();
	}
}
