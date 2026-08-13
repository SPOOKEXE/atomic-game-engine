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
#include <engine/scene/Part.hpp>
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

		// How fast the two bodies are separating along one direction, at the
		// contact point that direction belongs to.
		//
		// Positive means moving apart. The angular halves read the precomputed
		// torques rather than building the point velocities, which is the same
		// scalar triple product written the cheap way round — see
		// `ContactAxis::FirstTorque`.
		float ClosingSpeed(const SolverBody &first, const SolverBody &second, const ContactAxis &axis) {
			return (second.LinearVelocity - first.LinearVelocity).Dot(axis.Direction) +
				   second.AngularVelocity.Dot(axis.SecondTorque) -
				   first.AngularVelocity.Dot(axis.FirstTorque);
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

		// Resolves one impulse direction: the two angular responses and the mass
		// that falls out of them.
		//
		// The effective mass is the reciprocal of the responses projected back
		// onto the direction, so asking for it separately would be computing
		// them twice. `axis.Direction` must already be set.
		void PrepareAxis(
			const SolverBody &first,
			const SolverBody &second,
			const core::Vector3 &firstLever,
			const core::Vector3 &secondLever,
			ContactAxis &axis
		) {
			axis.FirstTorque = firstLever.Cross(axis.Direction);
			axis.SecondTorque = secondLever.Cross(axis.Direction);
			axis.FirstAngular = AngularResponse(first, axis.FirstTorque);
			axis.SecondAngular = AngularResponse(second, axis.SecondTorque);

			const float linear = first.InverseMass + second.InverseMass;
			const float angular = axis.FirstAngular.Dot(axis.FirstTorque) +
								  axis.SecondAngular.Dot(axis.SecondTorque);
			const float total = linear + angular;
			axis.Mass = total > 0.0f ? 1.0f / total : 0.0f;
		}

		void ApplyImpulse(SolverBody &first, SolverBody &second, const ContactAxis &axis, float magnitude) {
			// The normal points from the first body toward the second, so a
			// positive impulse pushes the second away and the first back. Every
			// pair function obeys that one convention, which is why this is the
			// only place the sign appears.
			first.LinearVelocity = first.LinearVelocity - axis.Direction * (magnitude * first.InverseMass);
			first.AngularVelocity = first.AngularVelocity - axis.FirstAngular * magnitude;
			second.LinearVelocity = second.LinearVelocity + axis.Direction * (magnitude * second.InverseMass);
			second.AngularVelocity = second.AngularVelocity + axis.SecondAngular * magnitude;
		}

		// The same push, against the velocity that only moves positions.
		//
		// Translation only. See `SolverBody::CorrectionLinear` for why there is
		// no angular half: a rotation nothing damps is a lean that grows.
		void ApplyCorrection(SolverBody &first, SolverBody &second, const ContactRow &row, float magnitude) {
			const core::Vector3 &normal = row.Along[ContactRow::NORMAL].Direction;
			first.CorrectionLinear = first.CorrectionLinear - normal * (magnitude * first.InverseMass);
			second.CorrectionLinear = second.CorrectionLinear + normal * (magnitude * second.InverseMass);
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

		BodyFacts FactsFor(
			const scene::RigidBody *body,
			const scene::Collider *collider,
			const scene::PhysicsProperties *physical
		) {
			if (body == nullptr || collider == nullptr) {
				// No `RigidBody` is not a static body — `scene::Enums` is
				// explicit that it is not a body at all. It still stops things,
				// which is exactly what an infinite mass does.
				return BodyFacts{};
			}

			// **`scene::MassOf` and not `body->Mass`, because density is a mass
			// too.** A part with `CustomPhysicalProperties` weighs its density
			// times its volume, and the properties panel shows the same number
			// through the same function — a solver with its own arithmetic here
			// would be a part that weighs one thing and reads as another.
			const float mass = scene::MassOf(*collider, *body, physical);
			if (body->Kind != scene::BodyKind::Dynamic || !(mass > 0.0f)) {
				return BodyFacts{};
			}
			return BodyFacts{1.0f / mass, InverseInertiaOf(*collider, mass), true};
		}
	}

	void Solve(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.solve", core::ProfileCategory::Physics);

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
		//
		// **The sort is over entity ids, not over bodies.** Two entries per
		// manifold go in and only `Owner` is meaningful at this point, so
		// sorting `SolverBody` moved a hundred and twenty bytes to order the
		// eight the comparison reads. At ten thousand bodies that was megabytes
		// of moves for kilobytes of information.
		std::vector<ecs::Entity> &owners = PipelineInternals::BodyOwners(*world);
		owners.clear();
		owners.reserve(manifolds.size() * 2);
		for (const ContactManifold &manifold : manifolds) {
			owners.push_back(manifold.A);
			owners.push_back(manifold.B);
		}
		std::sort(owners.begin(), owners.end(), [](ecs::Entity left, ecs::Entity right) {
			return left.Id < right.Id;
		});
		owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

		bodies.resize(owners.size());
		for (size_t index = 0; index < owners.size(); index++) {
			bodies[index] = SolverBody{};
			bodies[index].Owner = owners[index];
		}

		// **Every manifold's two body indices, resolved once.** Three later
		// passes want them, and a binary search per pass per side is four
		// searches per manifold over an array that no longer fits in cache once
		// a scene is large.
		std::vector<std::pair<uint32_t, uint32_t>> &located = PipelineInternals::ManifoldBodies(*world);
		located.clear();
		located.reserve(manifolds.size());
		for (const ContactManifold &manifold : manifolds) {
			located.emplace_back(
				static_cast<uint32_t>(IndexOf(bodies, manifold.A)),
				static_cast<uint32_t>(IndexOf(bodies, manifold.B))
			);
		}

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

			// **The part's own numbers win over its material's.** `Surface`
			// names what a thing is made of and this is the crate that is
			// deliberately slippery — `scene::PhysicsProperties` carries the
			// argument for it being an override rather than a replacement, and
			// `Custom` is what says whether there is one at all.
			//
			// One read per body per tick, beside the `Surface` read above and
			// for the same reason: this is where a body's row is resolved once,
			// before a single impulse is computed.
			const scene::PhysicsProperties *physical = store.Get<scene::PhysicsProperties>(body.Owner);
			if (physical != nullptr && physical->Custom) {
				body.Friction = physical->Friction;
				body.Restitution = physical->Elasticity;
			}

			const BodyFacts facts = FactsFor(rigid, collider, physical);
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
		for (size_t at = 0; at < manifolds.size(); at++) {
			SolverBody &first = bodies[located[at].first];
			SolverBody &second = bodies[located[at].second];

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
		for (size_t at = 0; at < manifolds.size(); at++) {
			const ContactManifold &manifold = manifolds[at];

			// A trigger reports and never pushes. Skipping it here rather than
			// zeroing its impulse keeps it out of the iteration entirely, so a
			// world made of triggers costs the solver nothing.
			if (manifold.Trigger) {
				continue;
			}

			const size_t firstIndex = located[at].first;
			const size_t secondIndex = located[at].second;
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

				// The lever arms are what turn an impulse into a torque, and the
				// reason a four-point manifold holds a box against rotation.
				// They stay local: every use of them is here, folded into the
				// three axes below, and a row the sweeps have to reload is a
				// row that costs sixteen passes over the cache.
				const core::Vector3 firstLever = contact.Position - first.Centre;
				const core::Vector3 secondLever = contact.Position - second.Centre;

				ContactRow row;
				row.First = firstIndex;
				row.Second = secondIndex;
				row.Friction = friction;
				row.Feature = contact.Feature;

				row.Along[ContactRow::NORMAL].Direction = manifold.Normal;
				TangentsFor(
					manifold.Normal,
					row.Along[ContactRow::TANGENT].Direction,
					row.Along[ContactRow::TANGENT + 1].Direction
				);
				for (ContactAxis &axis : row.Along) {
					PrepareAxis(first, second, firstLever, secondLever, axis);
				}

				const float shared = first.InverseMass + second.InverseMass;
				row.CorrectionMass = shared > 0.0f ? 1.0f / shared : 0.0f;

				const float excess = contact.Penetration - PENETRATION_SLOP;
				const float unwind =
					delta > 0.0f && excess > 0.0f ? (POSITION_CORRECTION / delta) * excess : 0.0f;
				row.Bias = unwind > MAXIMUM_CORRECTION_SPEED ? MAXIMUM_CORRECTION_SPEED : unwind;

				// Restitution is a function of how fast they were closing
				// *before* anything was applied, so it is captured here and not
				// recomputed per iteration — an iteration that recomputed it
				// would keep finding a smaller closing speed and add energy
				// chasing it.
				const float closing = -ClosingSpeed(first, second, row.Along[ContactRow::NORMAL]);
				row.Bounce = closing > BOUNCE_THRESHOLD ? restitution * closing : 0.0f;

				const ContactImpulse *cached = FindImpulse(
					PipelineInternals::ImpulseCache(*world), manifold.A, manifold.B, contact.Feature
				);
				if (cached != nullptr) {
					row.Along[ContactRow::NORMAL].Impulse = cached->Normal;
					row.Along[ContactRow::TANGENT].Impulse = cached->Tangent[0];
					row.Along[ContactRow::TANGENT + 1].Impulse = cached->Tangent[1];
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
			for (const ContactAxis &axis : row.Along) {
				ApplyImpulse(first, second, axis, axis.Impulse);
			}
		}

		// --- iterate ---------------------------------------------------------
		//
		// **Serial, and that is the algorithm rather than an omission.** Every
		// row is meant to see the velocities the rows before it left behind;
		// two threads visiting this list produce a different answer every run.
		// See `Solver.hpp` for what a parallel version would actually have to
		// be.
		for (size_t sweep = 0; sweep < SOLVER_ITERATIONS; sweep++) {
			for (size_t at = 0; at < rows.size(); at++) {
				ContactRow &row = rows[at];

				// **The two bodies of the row after this one, fetched early.**
				// The rows stream in order and the prefetcher handles them; the
				// bodies they name are two random offsets into an array that
				// stops fitting in cache somewhere around a few thousand
				// contacts, and the row's arithmetic cannot start until they
				// arrive. Asking for them one row ahead is what turns that wait
				// into work already done. A hint only — it changes no result,
				// and the bounds test costs one predictable branch per row.
				if (at + 1 < rows.size()) {
					__builtin_prefetch(&bodies[rows[at + 1].First]);
					__builtin_prefetch(&bodies[rows[at + 1].Second]);
				}

				SolverBody &first = bodies[row.First];
				SolverBody &second = bodies[row.Second];

				ContactAxis &normal = row.Along[ContactRow::NORMAL];

				// Friction before the normal, using the normal impulse the
				// previous sweep settled on. The other order lets a contact
				// that has just gained its normal impulse apply friction it has
				// not earned yet, which reads as a box that slides less on the
				// tick it lands than on every tick after.
				const float limit = row.Friction * normal.Impulse;
				for (size_t slot = ContactRow::TANGENT; slot < 3; slot++) {
					ContactAxis &tangent = row.Along[slot];
					const float sliding = ClosingSpeed(first, second, tangent);
					const float wanted = tangent.Impulse - sliding * tangent.Mass;
					const float clamped = wanted < -limit ? -limit : (wanted > limit ? limit : wanted);
					const float applied = clamped - tangent.Impulse;
					tangent.Impulse = clamped;
					ApplyImpulse(first, second, tangent, applied);
				}

				const float separating = ClosingSpeed(first, second, normal);

				// **No penetration term here.** The overlap is unwound by the
				// correction sweep below, against velocities that never reach a
				// `scene::Motion` — so a body at rest ends the tick at rest
				// rather than carrying one tick of gravity upward forever.
				const float wanted = normal.Impulse + (row.Bounce - separating) * normal.Mass;

				// Clamped at zero because a contact pushes and never pulls. The
				// accumulated form is what makes the clamp correct across
				// sweeps: clamping the increment instead would let a row that
				// over-pushed early keep the excess forever.
				const float clamped = wanted > 0.0f ? wanted : 0.0f;
				const float applied = clamped - normal.Impulse;
				normal.Impulse = clamped;
				ApplyImpulse(first, second, normal, applied);

				// The correction sweep. Its own effective mass, because it is a
				// translation-only constraint and the row's normal mass carries
				// the lever arms of one that is not.
				const float drifting =
					(second.CorrectionLinear - first.CorrectionLinear).Dot(normal.Direction);
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
					row.Along[ContactRow::NORMAL].Impulse,
					{row.Along[ContactRow::TANGENT].Impulse, row.Along[ContactRow::TANGENT + 1].Impulse},
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
