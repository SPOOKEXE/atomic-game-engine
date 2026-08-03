#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/SurfaceTable.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace engine::physics {

	namespace {
		// One component of a vector by index, so the three principal axes can
		// be walked in a loop rather than written out three times.
		float Component(const core::Vector3 &vector, size_t index) {
			return index == 0 ? vector.X : (index == 1 ? vector.Y : vector.Z);
		}

		// One over each principal moment of inertia, in the shape's own axes.
		//
		// Every shape here is symmetric about its own axes, so the local tensor
		// is diagonal and these three numbers are the whole of it. The formulae
		// are the standard solid ones, written against **half**-extents because
		// that is what `Collider::Extent` is — reading them as full extents
		// makes every body four times harder to turn, which reads as a
		// suspiciously heavy world rather than as a units mistake.
		core::Vector3 InverseInertiaOf(const scene::Collider &collider, float mass) {
			const float extentX = collider.Extent.X;
			const float extentY = collider.Extent.Y;
			const float extentZ = collider.Extent.Z;

			core::Vector3 inertia;
			switch (collider.Shape) {
			case scene::ShapeKind::Box:
				inertia = core::Vector3{
					mass * (extentY * extentY + extentZ * extentZ) / 3.0f,
					mass * (extentX * extentX + extentZ * extentZ) / 3.0f,
					mass * (extentX * extentX + extentY * extentY) / 3.0f,
				};
				break;

			case scene::ShapeKind::Sphere: {
				const float solid = 0.4f * mass * extentX * extentX;
				inertia = core::Vector3{solid, solid, solid};
				break;
			}

			case scene::ShapeKind::Cylinder: {
				// About the barrel it is a disc; across it, a disc plus a rod.
				const float across = mass * (3.0f * extentX * extentX + 4.0f * extentY * extentY) / 12.0f;
				inertia = core::Vector3{across, 0.5f * mass * extentX * extentX, across};
				break;
			}
			}

			return core::Vector3{
				inertia.X > 0.0f ? 1.0f / inertia.X : 0.0f,
				inertia.Y > 0.0f ? 1.0f / inertia.Y : 0.0f,
				inertia.Z > 0.0f ? 1.0f / inertia.Z : 0.0f,
			};
		}

		// The world inverse inertia applied to a torque.
		//
		// `R diag Rt` times a vector, without ever building `R diag Rt`.
		core::Vector3 AngularResponse(const SolverBody &body, const core::Vector3 &torque) {
			core::Vector3 response;
			for (size_t index = 0; index < 3; index++) {
				const core::Vector3 &axis = body.PrincipalAxis[index];
				response = response + axis * (Component(body.InverseInertia, index) * axis.Dot(torque));
			}
			return response;
		}

		core::Vector3 VelocityAt(const SolverBody &body, const core::Vector3 &lever) {
			return body.LinearVelocity + body.AngularVelocity.Cross(lever);
		}

		// Two unit directions across `normal`, spanning the contact plane.
		//
		// Built from whichever world axis the normal is least aligned with, so
		// the cross product is never near-degenerate. The choice is a function
		// of the normal alone, which is what keeps the friction basis — and
		// therefore the warm start that reuses its impulses — the same from one
		// tick to the next.
		void TangentsFor(const core::Vector3 &normal, core::Vector3 &first, core::Vector3 &second) {
			const core::Vector3 seed = std::abs(normal.X) < 0.57735f   ? core::Vector3::XAxis
									   : std::abs(normal.Y) < 0.57735f ? core::Vector3::YAxis
																	   : core::Vector3::ZAxis;
			first = normal.Cross(seed).Unit();
			second = normal.Cross(first);
		}

		// The mass the pair presents along one direction at one contact point.
		float EffectiveMass(
			const SolverBody &first,
			const SolverBody &second,
			const core::Vector3 &firstLever,
			const core::Vector3 &secondLever,
			const core::Vector3 &direction
		) {
			const float linear = first.InverseMass + second.InverseMass;
			const float angular =
				direction.Dot(AngularResponse(first, firstLever.Cross(direction)).Cross(firstLever)) +
				direction.Dot(AngularResponse(second, secondLever.Cross(direction)).Cross(secondLever));
			const float total = linear + angular;
			return total > 0.0f ? 1.0f / total : 0.0f;
		}

		void ApplyImpulse(
			SolverBody &first,
			SolverBody &second,
			const ContactRow &row,
			const core::Vector3 &direction,
			float magnitude
		) {
			// The normal points from the first body toward the second, so a
			// positive impulse pushes the second away and the first back. Every
			// pair function obeys that one convention, which is why this is the
			// only place the sign appears.
			first.LinearVelocity = first.LinearVelocity - direction * (magnitude * first.InverseMass);
			first.AngularVelocity =
				first.AngularVelocity - AngularResponse(first, row.FirstLever.Cross(direction)) * magnitude;
			second.LinearVelocity = second.LinearVelocity + direction * (magnitude * second.InverseMass);
			second.AngularVelocity = second.AngularVelocity +
									 AngularResponse(second, row.SecondLever.Cross(direction)) * magnitude;
		}

		// The same push, against the velocity that only moves positions.
		//
		// Translation only. See `SolverBody::CorrectionLinear` for why there is
		// no angular half: a rotation nothing damps is a lean that grows.
		void ApplyCorrection(SolverBody &first, SolverBody &second, const ContactRow &row, float magnitude) {
			first.CorrectionLinear = first.CorrectionLinear - row.Normal * (magnitude * first.InverseMass);
			second.CorrectionLinear = second.CorrectionLinear + row.Normal * (magnitude * second.InverseMass);
		}

		size_t IndexOf(const std::vector<SolverBody> &bodies, ecs::Entity entity) {
			const auto found = std::lower_bound(
				bodies.begin(), bodies.end(), entity, [](const SolverBody &body, ecs::Entity target) {
					return body.Owner.Id < target.Id;
				}
			);
			return static_cast<size_t>(found - bodies.begin());
		}

		// Last tick's impulses for one contact, or nothing.
		//
		// A binary search over a sorted array rather than a hash lookup. Not
		// for speed: an unordered container's walk order is the allocator's,
		// and §2.4 refuses one anywhere in this path.
		const ContactImpulse *FindImpulse(
			const std::vector<ContactImpulse> &cache, ecs::Entity first, ecs::Entity second, uint32_t feature
		) {
			const ContactImpulse key{first, second, feature, 0.0f, {0.0f, 0.0f}};
			const auto found = std::lower_bound(cache.begin(), cache.end(), key);
			if (found == cache.end() || key < *found) {
				return nullptr;
			}
			return &*found;
		}

		// Everything a body's row contributes, resolved once.
		struct BodyFacts {
			float InverseMass = 0.0f;
			core::Vector3 InverseInertia;
			bool Dynamic = false;
		};

		BodyFacts FactsFor(const scene::RigidBody *body, const scene::Collider *collider) {
			if (body == nullptr || collider == nullptr) {
				// No `RigidBody` is not a static body — `scene::Enums` is
				// explicit that it is not a body at all. It still stops things,
				// which is exactly what an infinite mass does.
				return BodyFacts{};
			}
			if (body->Kind != scene::BodyKind::Dynamic || !(body->Mass > 0.0f)) {
				return BodyFacts{};
			}
			return BodyFacts{1.0f / body->Mass, InverseInertiaOf(*collider, body->Mass), true};
		}
	}

	void Solve(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.solve", core::ProfileCategory::ECS);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		const float delta = store.Time().Delta;
		const std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(*world);
		std::vector<SolverBody> &bodies = PipelineInternals::Bodies(*world);
		std::vector<ContactRow> &rows = PipelineInternals::Rows(*world);
		bodies.clear();
		rows.clear();

		// --- gather ----------------------------------------------------------
		//
		// Only the bodies a manifold names. A body nothing touches has no
		// constraint to solve and no velocity for `Publish` to write back, so
		// gathering every dynamic row would be a pass over the world to find
		// the few that are in contact — the shape `CODE_QUALITY.md` names.
		const auto named = [](ecs::Entity owner) {
			SolverBody body;
			body.Owner = owner;
			return body;
		};
		for (const ContactManifold &manifold : manifolds) {
			bodies.push_back(named(manifold.A));
			bodies.push_back(named(manifold.B));
		}
		std::sort(bodies.begin(), bodies.end(), [](const SolverBody &left, const SolverBody &right) {
			return left.Owner.Id < right.Owner.Id;
		});
		bodies.erase(
			std::unique(
				bodies.begin(),
				bodies.end(),
				[](const SolverBody &left, const SolverBody &right) { return left.Owner == right.Owner; }
			),
			bodies.end()
		);

		// Sorted by entity, which makes every lookup below a binary search and
		// makes the body array a function of the scene rather than of the order
		// rows happen to sit in their archetypes.
		// Safe to reach typed, and only because of the guard at the top of this
		// function. `RegisterPhysicsComponents` registers the `scene` types
		// before its own, so a store that got past `PreparedWorldMutable` has
		// `scene.SurfaceTable` registered under its explicit name — and a store
		// that did not never reaches this line. That is the ordering the guard
		// buys, and moving this above it would reintroduce the hazard
		// `WorldResource.hpp` describes with a different type.
		const scene::SurfaceTable *surfaces = store.Resource<scene::SurfaceTable>();

		for (SolverBody &body : bodies) {
			const scene::Transform *transform = store.Get<scene::Transform>(body.Owner);
			const scene::Motion *motion = store.Get<scene::Motion>(body.Owner);
			const scene::Collider *collider = store.Get<scene::Collider>(body.Owner);
			const scene::RigidBody *rigid = store.Get<scene::RigidBody>(body.Owner);

			if (transform != nullptr) {
				body.Centre = transform->Frame.Position;
				body.PrincipalAxis[0] = transform->Frame.RightVector();
				body.PrincipalAxis[1] = transform->Frame.UpVector();
				body.PrincipalAxis[2] = transform->Frame.VectorToWorldSpace(core::Vector3::ZAxis);
			}
			if (motion != nullptr) {
				body.LinearVelocity = motion->Linear;
				body.AngularVelocity = motion->Angular;
			}

			// **The one `Surface` read.** One row per body per tick, resolved
			// before a single impulse is computed — `v02v03v04.md` §3.2 asks
			// for exactly this and the cost it is avoiding is a name lookup per
			// contact per iteration. An unregistered material takes the
			// defaults rather than logging: `SurfaceTable` refuses a
			// get-or-default so that the caller decides, and the caller's
			// decision here is that a missing row is ordinary rather than an
			// error worth a line per body per tick.
			const scene::Surface *surface = store.Get<scene::Surface>(body.Owner);
			scene::SurfaceProperties properties;
			if (surfaces != nullptr && surface != nullptr) {
				const scene::SurfaceProperties *row = surfaces->Find(surface->Material);
				if (row != nullptr) {
					properties = *row;
				}
			}
			body.Friction = properties.Friction;
			body.Restitution = properties.Restitution;

			const BodyFacts facts = FactsFor(rigid, collider);
			body.InverseMass = facts.InverseMass;
			body.InverseInertia = facts.InverseInertia;
			body.Movable = facts.Dynamic;
			body.Asleep = facts.Dynamic && world->Sleeping(body.Owner);
		}

		// --- wake ------------------------------------------------------------
		//
		// A sleeping body has no `scene::Motion`, so the broad phase has it in
		// the static index and only an *awake* neighbour can produce a pair
		// with it. One pass in pair order, so a stack wakes one layer per tick
		// — bounded, deterministic, and visibly a settling stack rather than a
		// whole scene jumping at once.
		for (const ContactManifold &manifold : manifolds) {
			SolverBody &first = bodies[IndexOf(bodies, manifold.A)];
			SolverBody &second = bodies[IndexOf(bodies, manifold.B)];

			const float firstSpeed = first.LinearVelocity.Magnitude();
			const float secondSpeed = second.LinearVelocity.Magnitude();
			if (first.Asleep && !second.Asleep && secondSpeed > WAKE_SPEED) {
				first.Asleep = false;
			}
			if (second.Asleep && !first.Asleep && firstSpeed > WAKE_SPEED) {
				second.Asleep = false;
			}
		}

		for (SolverBody &body : bodies) {
			if (!body.Asleep) {
				continue;
			}
			// Asleep is immovable for this tick, which is the whole benefit:
			// the impulse arithmetic treats it exactly as it treats a floor.
			body.Movable = false;
			body.InverseMass = 0.0f;
			body.InverseInertia = core::Vector3::Zero;
		}

		// --- set up ----------------------------------------------------------
		for (const ContactManifold &manifold : manifolds) {
			// A trigger reports and never pushes. Skipping it here rather than
			// zeroing its impulse keeps it out of the iteration entirely, so a
			// world made of triggers costs the solver nothing.
			if (manifold.Trigger) {
				continue;
			}

			const size_t firstIndex = IndexOf(bodies, manifold.A);
			const size_t secondIndex = IndexOf(bodies, manifold.B);
			if (!bodies[firstIndex].Movable && !bodies[secondIndex].Movable) {
				continue;
			}

			const SolverBody &first = bodies[firstIndex];
			const SolverBody &second = bodies[secondIndex];

			// Combined the standard way: friction multiplies because two
			// slippery surfaces are slipperier than either, restitution takes
			// the larger because a superball off concrete bounces.
			const float friction = std::sqrt(first.Friction * second.Friction);
			const float restitution =
				first.Restitution > second.Restitution ? first.Restitution : second.Restitution;

			for (size_t point = 0; point < manifold.PointCount; point++) {
				const ContactPoint &contact = manifold.Points[point];

				ContactRow row;
				row.First = firstIndex;
				row.Second = secondIndex;
				row.Normal = manifold.Normal;
				row.FirstLever = contact.Position - first.Centre;
				row.SecondLever = contact.Position - second.Centre;
				row.Friction = friction;
				row.Feature = contact.Feature;
				TangentsFor(row.Normal, row.Tangent[0], row.Tangent[1]);

				row.NormalMass = EffectiveMass(first, second, row.FirstLever, row.SecondLever, row.Normal);

				const float shared = first.InverseMass + second.InverseMass;
				row.CorrectionMass = shared > 0.0f ? 1.0f / shared : 0.0f;

				row.TangentMass[0] =
					EffectiveMass(first, second, row.FirstLever, row.SecondLever, row.Tangent[0]);
				row.TangentMass[1] =
					EffectiveMass(first, second, row.FirstLever, row.SecondLever, row.Tangent[1]);

				const float excess = contact.Penetration - PENETRATION_SLOP;
				const float unwind =
					delta > 0.0f && excess > 0.0f ? (POSITION_CORRECTION / delta) * excess : 0.0f;
				row.Bias = unwind > MAXIMUM_CORRECTION_SPEED ? MAXIMUM_CORRECTION_SPEED : unwind;

				// Restitution is a function of how fast they were closing
				// *before* anything was applied, so it is captured here and not
				// recomputed per iteration — an iteration that recomputed it
				// would keep finding a smaller closing speed and add energy
				// chasing it.
				const float closing =
					-(VelocityAt(second, row.SecondLever) - VelocityAt(first, row.FirstLever))
						 .Dot(row.Normal);
				row.Bounce = closing > BOUNCE_THRESHOLD ? restitution * closing : 0.0f;

				const ContactImpulse *cached = FindImpulse(
					PipelineInternals::ImpulseCache(*world), manifold.A, manifold.B, contact.Feature
				);
				if (cached != nullptr) {
					row.NormalImpulse = cached->Normal;
					row.TangentImpulse[0] = cached->Tangent[0];
					row.TangentImpulse[1] = cached->Tangent[1];
				}

				rows.push_back(row);
			}
		}

		// --- warm start ------------------------------------------------------
		//
		// Last tick's answer applied before the first sweep. A resting stack
		// converges to the same impulses every tick, so starting from them
		// instead of from zero is most of why `SOLVER_ITERATIONS` is sixteen and
		// not forty.
		for (const ContactRow &row : rows) {
			SolverBody &first = bodies[row.First];
			SolverBody &second = bodies[row.Second];
			ApplyImpulse(first, second, row, row.Normal, row.NormalImpulse);
			ApplyImpulse(first, second, row, row.Tangent[0], row.TangentImpulse[0]);
			ApplyImpulse(first, second, row, row.Tangent[1], row.TangentImpulse[1]);
		}

		// --- iterate ---------------------------------------------------------
		//
		// **Serial, and that is the algorithm rather than an omission.** Every
		// row is meant to see the velocities the rows before it left behind;
		// two threads visiting this list produce a different answer every run.
		// See `Solver.hpp` for what a parallel version would actually have to
		// be.
		for (size_t sweep = 0; sweep < SOLVER_ITERATIONS; sweep++) {
			for (ContactRow &row : rows) {
				SolverBody &first = bodies[row.First];
				SolverBody &second = bodies[row.Second];

				// Friction before the normal, using the normal impulse the
				// previous sweep settled on. The other order lets a contact
				// that has just gained its normal impulse apply friction it has
				// not earned yet, which reads as a box that slides less on the
				// tick it lands than on every tick after.
				for (size_t axis = 0; axis < 2; axis++) {
					const float sliding =
						(VelocityAt(second, row.SecondLever) - VelocityAt(first, row.FirstLever))
							.Dot(row.Tangent[axis]);
					const float limit = row.Friction * row.NormalImpulse;
					const float wanted = row.TangentImpulse[axis] - sliding * row.TangentMass[axis];
					const float clamped = wanted < -limit ? -limit : (wanted > limit ? limit : wanted);
					const float applied = clamped - row.TangentImpulse[axis];
					row.TangentImpulse[axis] = clamped;
					ApplyImpulse(first, second, row, row.Tangent[axis], applied);
				}

				const float separating =
					(VelocityAt(second, row.SecondLever) - VelocityAt(first, row.FirstLever)).Dot(row.Normal);

				// **No penetration term here.** The overlap is unwound by the
				// correction sweep below, against velocities that never reach a
				// `scene::Motion` — so a body at rest ends the tick at rest
				// rather than carrying one tick of gravity upward forever.
				const float wanted = row.NormalImpulse + (row.Bounce - separating) * row.NormalMass;

				// Clamped at zero because a contact pushes and never pulls. The
				// accumulated form is what makes the clamp correct across
				// sweeps: clamping the increment instead would let a row that
				// over-pushed early keep the excess forever.
				const float clamped = wanted > 0.0f ? wanted : 0.0f;
				const float applied = clamped - row.NormalImpulse;
				row.NormalImpulse = clamped;
				ApplyImpulse(first, second, row, row.Normal, applied);

				// The correction sweep. Its own effective mass, because it is a
				// translation-only constraint and the row's normal mass carries
				// the lever arms of one that is not.
				const float drifting = (second.CorrectionLinear - first.CorrectionLinear).Dot(row.Normal);
				const float wantedCorrection =
					row.CorrectionImpulse + (row.Bias - drifting) * row.CorrectionMass;
				const float clampedCorrection = wantedCorrection > 0.0f ? wantedCorrection : 0.0f;
				const float appliedCorrection = clampedCorrection - row.CorrectionImpulse;
				row.CorrectionImpulse = clampedCorrection;
				ApplyCorrection(first, second, row, appliedCorrection);
			}
		}

		// --- remember --------------------------------------------------------
		std::vector<ContactImpulse> &next = PipelineInternals::ImpulseNext(*world);
		next.clear();
		for (const ContactRow &row : rows) {
			next.push_back(
				ContactImpulse{
					bodies[row.First].Owner,
					bodies[row.Second].Owner,
					row.Feature,
					row.NormalImpulse,
					{row.TangentImpulse[0], row.TangentImpulse[1]},
				}
			);
		}

		// Sorted, because next tick's warm start binary-searches this. The rows
		// arrive in pair order already, but a manifold's own points do not
		// arrive in feature order — reduction keeps the four that hold the face
		// widest, not the first four — so the last part of the key is the part
		// that needs the sort.
		std::sort(next.begin(), next.end());
		std::swap(PipelineInternals::ImpulseCache(*world), next);

		// --- rest ------------------------------------------------------------
		std::vector<RestingBody> &resting = PipelineInternals::Resting(*world);
		std::vector<RestingBody> &restingNext = PipelineInternals::RestingNext(*world);
		restingNext.clear();

		size_t carried = 0;
		const auto carryUntil = [&](uint64_t limit) {
			while (carried < resting.size() && resting[carried].Owner.Id < limit) {
				const RestingBody &existing = resting[carried];
				if (existing.Asleep && store.Alive(existing.Owner)) {
					restingNext.push_back(existing);
				}
				carried++;
			}
		};

		for (const SolverBody &body : bodies) {
			carryUntil(body.Owner.Id);

			RestingBody entry{body.Owner, 0.0f, false};
			if (carried < resting.size() && resting[carried].Owner == body.Owner) {
				entry = resting[carried];
				carried++;
			}

			// Only a dynamic body sleeps. A kinematic one is moved by whoever
			// owns it, and taking its `scene::Motion` away would stop that
			// owner's writes from moving anything.
			if (!body.Movable && !body.Asleep) {
				continue;
			}

			entry.Asleep = body.Asleep;
			if (!entry.Asleep) {
				const bool still = body.LinearVelocity.Magnitude() < SLEEP_LINEAR_SPEED &&
								   body.AngularVelocity.Magnitude() < SLEEP_ANGULAR_SPEED;
				entry.RestingSeconds = still ? entry.RestingSeconds + delta : 0.0f;
				entry.Asleep = entry.RestingSeconds >= SLEEP_SETTLE_SECONDS;
			}
			restingNext.push_back(entry);
		}
		carryUntil(UINT64_MAX);

		std::swap(resting, restingNext);
	}
}
