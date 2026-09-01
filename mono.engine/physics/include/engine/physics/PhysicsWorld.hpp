#pragma once

// One world's physics state: the two indexes, the pair list, and the buffers
// the pipeline reuses instead of reallocating.
//
// **A resource, because rule 2 of the root `AGENTS.md` leaves no alternative.**
// A module does not keep private vectors for data another module also reads, and
// every one of these buffers is read outside the function that filled it - the
// pair list by the narrow phase, the events by game code. Held in the store, it
// is covered by the affinity check, visible to a snapshot, and there is exactly
// one of it per world.
//
// **Everything here is cleared and never freed**, a standing rule for exactly
// these lists: a steady scene stops allocating after its first tick.
//
// **Two indexes, not one.** `spatial::HashGrid` is rebuild-only by design, so
// "only re-insert what moved" is not a call this module can make - it is a
// decision about *what to hand to `Rebuild`*. Static geometry gets its own grid
// and its own rebuild, which happens when the static set changes and not once
// per tick. `spatial/AGENTS.md` names the second grid as the answer to
// re-measuring the world every tick, and this is it.
//
// @tier L8 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/spatial/ChunkMap.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace engine::physics {

	// A direct WeldConstraint's captured relative frame.
	struct WeldPose {
		ecs::Entity Owner;
		ecs::Entity Part0;
		ecs::Entity Part1;
		core::CFrame Part0ToPart1;
	};

	// One active rigid edge and one part in the resolved assembly graph.
	struct RigidEdge {
		ecs::Entity Owner;
		ecs::Entity Part0;
		ecs::Entity Part1;
		core::CFrame Part0ToPart1;
	};

	struct RigidNode {
		ecs::Entity Part;
		ecs::Entity Root;
		core::CFrame Frame;
		bool Placed = false;
	};

	// What the broad phase remembers about one collider, beside its box.
	//
	// **The masks are here rather than looked up per candidate.** A pair needs
	// both colliders' `Layer` and `Mask`, and `spatial::HashGrid` filters with
	// one mask only - so the reverse half of the rule needs the other side's
	// mask in hand. Resolving it through the store per candidate would be one
	// random access per candidate, which is the cost an index exists to remove.
	//
	// It holds nothing else on purpose. `ShapeKind`, `Extent` and `Trigger` are
	// what the *narrow* phase needs, and it reads them off the entity's
	// `scene::Collider` in one go rather than paying to copy three fields per
	// collider per tick for a step that visits a fraction of them.
	//
	// @since v0.4
	struct ColliderRecord {
		// Which entity this collider belongs to.
		ecs::Entity Owner;

		// Which layers the collider is on.
		spatial::LayerMask Layer;

		// Which layers it is tested against.
		spatial::LayerMask Mask;
	};

	// Two colliders whose world boxes overlap, and which the layer masks admit.
	//
	// A *candidate*, not a contact. The boxes touch; the shapes inside them very
	// often do not, and deciding that is the narrow phase's job.
	//
	// @since v0.4
	struct CandidatePair {
		// The collider with the smaller entity id.
		ecs::Entity A;

		// The collider with the larger entity id.
		ecs::Entity B;

		// Reports whether both sides name the same two entities.
		//
		// @param other The pair to compare.
		// @return `true` when both entities match.
		constexpr bool operator==(const CandidatePair &other) const {
			return A == other.A && B == other.B;
		}

		// Orders by the smaller entity id, then the larger.
		//
		// **This ordering is a determinism requirement and not tidiness.** The
		// solver visits contacts in pair order, sequential impulse is
		// order-dependent, and a list assembled in whatever sequence a grid walk
		// produced would put two runs of one scene on different trajectories -
		// which `just determinism` reports a long way from here.
		//
		// @param other The pair to compare.
		// @return `true` when this pair sorts first.
		constexpr bool operator<(const CandidatePair &other) const {
			if (A.Id != other.A.Id) {
				return A.Id < other.A.Id;
			}
			return B.Id < other.B.Id;
		}
	};

	// Which proxy each half of a candidate pair came from.
	//
	// **The broad phase already knows this and used to throw it away.** It
	// generates a pair from two *record indices* and then names the pair by
	// entity, because an entity is what a manifold and a contact event carry.
	// The narrow phase then resolved those entities back into shapes through the
	// store - twenty-five thousand `Store::Get` calls for ten thousand
	// colliders, every one of them re-reading a `Transform` the sync had already
	// read. Carrying the indices forward makes that step two array subscripts.
	//
	// **Private, and it must stay so.** These are indices into `PhysicsWorld`'s
	// own arrays, exactly as `spatial::Proxy::Id` is - `AGENTS.md` explains why
	// publishing one hands a caller a number that is plausible and wrong.
	//
	// **The high bit says which array.** A world has two indexes, and a pair may
	// name one collider in each; a separate flag byte would be two more bytes on
	// a row that is sorted, and a second parallel array would be a third thing
	// to keep in step through that sort.
	//
	// @since v0.17
	struct CandidateSource {
		// The first collider's proxy index, with `STATIC` set when it is in the
		// static index.
		uint32_t First = 0;

		// The second's, the same way.
		uint32_t Second = 0;

		// Set on an index that refers to the static arrays.
		static constexpr uint32_t STATIC = 0x80000000u;

		// Whether each collider is in the static index rather than the moving
		// one, which decides which array `FirstIndex` and `SecondIndex` are
		// positions into.
		//@{
		constexpr bool FirstIsStatic() const {
			return (First & STATIC) != 0;
		}

		constexpr bool SecondIsStatic() const {
			return (Second & STATIC) != 0;
		}
		//@}

		// Each collider's position in whichever array it belongs to, with the
		// static flag masked off.
		//@{
		constexpr size_t FirstIndex() const {
			return static_cast<size_t>(First & ~STATIC);
		}

		constexpr size_t SecondIndex() const {
			return static_cast<size_t>(Second & ~STATIC);
		}
		//@}
	};

	// A candidate pair with the proxies it came from, as the broad phase sorts
	// it.
	//
	// **One row rather than two parallel arrays, and only while sorting.** The
	// pair list has to come out in entity order and the indices have to stay
	// with the pair they belong to; two arrays sorted separately is two chances
	// for them to disagree, and the disagreement is silent - the narrow phase
	// would test two real shapes that are not the ones the pair names.
	//
	// Split into `PairList` and `PairSourceList` once the sort is done, because
	// the first of those is what every consumer outside this module reads and
	// the second is nobody else's business.
	//
	// @since v0.17
	struct SourcedPair {
		// The pair itself, which is what everything outside this module reads.
		CandidatePair Pair;

		// Where it came from, which is nobody else's business once the sort is
		// done.
		CandidateSource Source;

		// Ordered and compared by the pair alone. Two rows naming one pair are
		// one candidate however they were found.
		//@{
		constexpr bool operator<(const SourcedPair &other) const {
			return Pair < other.Pair;
		}

		constexpr bool operator==(const SourcedPair &other) const {
			return Pair == other.Pair;
		}
		//@}
	};

	// One body the solver may push, gathered once per tick.
	//
	// **A compact array and not the components themselves.** A sequential
	// impulse solver reads and writes a body's velocity once per contact per
	// iteration; at sixteen iterations and four points that is sixty-four visits
	// to the same row. Through `Store::Get` every one of them is a sparse-set
	// lookup into a column that is not the one visited before it. Gathered, the
	// whole iteration runs over one array, and `Publish` is the single pass
	// that puts the answers back - which is exactly what the pipeline table
	// means by a `Publish` step that writes back velocities.
	//
	// **The first five fields are the ones a sweep touches, and they are first
	// on purpose.** The iteration reads and writes two bodies per row at random
	// offsets, `SOLVER_ITERATIONS` times over, so what decides whether that hits
	// cache is how many lines a body's *hot* half spans. The three velocities,
	// the inverse mass and the movable flag are forty-one bytes; the
	// sixty-four-byte alignment below puts all of them in one line and
	// everything the sweeps never look at in the next. Scattered - inverse mass
	// sat ninety-six bytes after the velocities - it was two lines per body per
	// access.
	//
	// @since v0.4
	struct alignas(64) SolverBody {
		// Metres per second, in world space.
		core::Vector3 LinearVelocity;

		// Radians per second about each world axis.
		core::Vector3 AngularVelocity;

		// The correction velocity, which moves the body and nothing else.
		//
		// **Split from the real velocity on purpose.** Pushing two overlapping
		// bodies apart through their actual velocity is indistinguishable, one
		// tick later, from them having been thrown apart - the stack bounces,
		// and a box at rest carries a permanent upward velocity that no
		// sleeping threshold can tell from creeping. This is added to the
		// transform by `Publish` and then thrown away.
		//
		// **There is no angular twin, and that is a decision rather than an
		// omission.** A correction that also turns bodies has nothing damping
		// it: the real solve never sees the rotation, so nothing resists it,
		// and a stack acquires a permanent lean that grows every tick until it
		// slides apart. Measured on a six-box tower over four seconds, the
		// angular half costs 326 millimetres of drift against 40 without it.
		// The price is that an overlap which really is rotational - a box
		// resting on one corner, sunk in - is pushed straight out rather than
		// tipped out, which is slower and never wrong.
		core::Vector3 CorrectionLinear;

		// One over the mass, in reciprocal kilograms. **Zero means immovable**
		// - static geometry, a kinematic body, or a sleeping one.
		//
		// Here rather than beside the other scalars because `ApplyImpulse` reads
		// it, so it belongs in the hot line with the velocities it scales.
		float InverseMass = 0.0f;

		// Whether the solver is allowed to change its velocity at all.
		//
		// **In the hot line for `InverseMass`' reason, and it moved here in
		// v0.17.** `ApplyImpulse` reads it once per body per row per sweep to
		// decide whether to write at all - see the branch there, which is what
		// lets two workers share a floor - so a flag left down among the setup
		// fields would pull a second cache line per body on every access and
		// cost more than the writes it was added to avoid.
		//
		// It is always `InverseMass > 0`, and the redundancy is deliberate: a
		// sweep asking "may I write this" should not have to know that the
		// answer happens to be encoded in a reciprocal mass.
		bool Movable = false;

		// --- everything below is set up once and never read by a sweep -------

		// Which entity this is.
		ecs::Entity Owner;

		// Where the body turns about - its transform's position, because
		// nothing in this engine offsets a centre of mass from its origin.
		core::Vector3 Centre;

		// The body's principal axes, in world space.
		//
		// Held as axes and a diagonal rather than as a three-by-three matrix
		// because that is the form the inertia is *derived* in - every shape
		// here is symmetric about its own axes, so the local tensor is
		// diagonal and the world one is `R diag Rt`. Multiplying a vector
		// through the axes costs three dots and three scales and never builds
		// the matrix, and it keeps a vendor matrix type out of a public header.
		core::Vector3 PrincipalAxis[3];

		// One over each principal moment of inertia, in the body's own axes.
		// Zero on every axis for a body the solver may not turn.
		core::Vector3 InverseInertia;

		// Coulomb friction, resolved from the body's `scene::Surface` once per
		// tick. The narrow phase reads the table; the solver reads this.
		float Friction = 0.5f;

		// Restitution, resolved the same way.
		float Restitution = 0.0f;

		// Whether the body is at rest and out of the dynamic archetype.
		//
		// Read by the gather and by the resting merge, never by a sweep - which
		// is why it stayed here when `Movable` moved up. A sleeping body is
		// immovable for the tick, so a sweep asks `Movable` and gets the answer
		// without a second question.
		bool Asleep = false;
	};

	// One contact point's accumulated impulse, kept for the next tick.
	//
	// **The warm start, and it is a reuse structure rather than an
	// optimisation bolted on.** A resting stack converges to the same impulses
	// every tick, so
	// starting from last tick's answer instead of from zero costs a lookup and
	// saves most of the iterations it would otherwise take to find them again.
	//
	// @since v0.4
	struct ContactImpulse {
		// The collider with the smaller entity id.
		ecs::Entity A;

		// The collider with the larger entity id.
		ecs::Entity B;

		// Which contact of that pair, per `ContactPoint::Feature`. The third
		// part of the key the allocation table names.
		uint32_t Feature = 0;

		// The accumulated impulse along the manifold normal, in newton
		// seconds.
		float Normal = 0.0f;

		// The accumulated impulses along the two friction directions.
		float Tangent[2] = {0.0f, 0.0f};

		// Orders by the two entities and then the feature, so the cache is a
		// sorted array a binary search answers rather than a hash map whose
		// walk order is the allocator's.
		//
		// @param other The row to compare.
		// @return `true` when this row sorts first.
		constexpr bool operator<(const ContactImpulse &other) const {
			if (A.Id != other.A.Id) {
				return A.Id < other.A.Id;
			}
			if (B.Id != other.B.Id) {
				return B.Id < other.B.Id;
			}
			return Feature < other.Feature;
		}
	};

	// One direction an impulse may act along at a contact, fully resolved.
	//
	// The three of them - the normal and the two friction directions - differ
	// only in which way they point, so they are one type used three times
	// rather than three sets of parallel fields. Keeping the direction, the
	// response it produces and the impulse accumulated along it in one place is
	// what makes it impossible to apply a magnitude against the wrong response.
	//
	// @since v0.14
	struct ContactAxis {
		// Which way the impulse acts, unit length. For the normal, pointing
		// from the first body toward the second.
		core::Vector3 Direction;

		// The angular velocity each body picks up per unit of impulse: its
		// world inverse inertia applied to `lever` crossed into `Direction`.
		//
		// **Resolved here because no sweep changes it.** A body's inertia and
		// its lever arms are fixed for the tick, so this is three vectors per
		// row - but derived inside the iteration it was two inertia products
		// per body per direction per sweep, which at sixteen sweeps is
		// ninety-six evaluations of three constants and was the largest single
		// cost in the solve.
		//@{
		core::Vector3 FirstAngular;
		core::Vector3 SecondAngular;
		//@}

		// Each body's lever arm crossed into `Direction`.
		//
		// The torque a unit impulse applies, and - by the scalar triple product
		// - also the vector that turns a body's angular velocity into its share
		// of the closing speed along `Direction`. That second reading is what a
		// sweep uses: `(w x lever) . Direction` becomes `w . Torque`, so probing
		// the contact is two dot products instead of two cross products, three
		// times per row per sweep.
		//@{
		core::Vector3 FirstTorque;
		core::Vector3 SecondTorque;
		//@}

		// The mass the pair presents along `Direction` at this point. It is the
		// reciprocal of what the two responses above already add up to, so all
		// three come out of one calculation.
		float Mass = 0.0f;

		// The impulse accumulated along `Direction` so far, in newton seconds.
		float Impulse = 0.0f;
	};

	// One contact point turned into the numbers an impulse iteration needs.
	//
	// **Set up once and read every iteration.** Everything here is constant for
	// the tick apart from the accumulated impulses - the directions, the
	// responses each body gives them, the effective masses, the friction the
	// two `scene::Surface` rows combine to, the target speed the penetration
	// asks for. Recomputing any of it inside the iteration loop would multiply
	// it by `SOLVER_ITERATIONS`, which is the "read the surface once" rule
	// applied where it bites.
	//
	// The lever arms are deliberately **not** here. Everything that needs them
	// folds them into `Along` during setup, and a field the sweeps would reload
	// sixteen times without reading is a field that costs cache and nothing
	// else.
	//
	// There is no public accessor for these. They are the solver's working set
	// and nothing outside the pipeline has a use for one.
	//
	// @since v0.4
	struct ContactRow {
		// Index into `Bodies` of the collider with the smaller entity id.
		size_t First = 0;

		// Index into `Bodies` of the other one.
		size_t Second = 0;

		// The three directions this contact acts along.
		//
		// `NORMAL` is the manifold's normal; `TANGENT` and the slot after it
		// are the two friction directions across it. Two friction directions
		// rather than one because friction resists sliding anywhere in the
		// contact plane, and one direction leaves the perpendicular one free.
		ContactAxis Along[3];

		// The mass for the correction, which is translation only - so it is
		// the two inverse masses and no lever arm at all.
		float CorrectionMass = 0.0f;

		// The separating speed the penetration asks for, in metres per second.
		float Bias = 0.0f;

		// The separating speed the bounce asks for.
		float Bounce = 0.0f;

		// The combined Coulomb coefficient for this pair.
		float Friction = 0.5f;

		// The impulse accumulated against the correction velocities.
		//
		// Never warm-started, unlike the three above: it answers "how far apart
		// do these two have to be pushed *this* tick", and last tick's answer
		// is about a penetration that has already been unwound.
		float CorrectionImpulse = 0.0f;

		// The contact's cache key, carried so the tick's answer can be stored
		// under the same key it was warm-started from.
		uint32_t Feature = 0;

		// Slots in `Along`. The normal first, so the friction pair is the two
		// after it and both can be walked with one loop.
		//@{
		static constexpr size_t NORMAL = 0;
		static constexpr size_t TANGENT = 1;
		//@}
	};

	// A run of contact rows no other run shares a movable body with.
	//
	// **This is what makes a sequential-impulse solve parallel without making it
	// approximate.** Two groups name disjoint sets of bodies the solver may
	// write, so a worker sweeping one group and a worker sweeping another cannot
	// see each other's arithmetic - and the answer is therefore the same however
	// the two were scheduled, which is the property `Solver.hpp` says a parallel
	// solve would have to have. It is the graph colouring that file names, with
	// the colours read off a spatial partition rather than searched for.
	//
	// **Disjoint in *movable* bodies and not in all of them**, which is the
	// distinction that makes the scheme work at all. Every contact in a scene
	// with a floor names that floor, so a rule about all bodies would put every
	// contact in one group. An immovable body is only ever read during a sweep -
	// its inverse mass and inverse inertia are zero, so every impulse applied to
	// it is a no-op - and `ApplyImpulse` skips the write outright, so two workers
	// touching one floor is two reads.
	//
	// @since v0.17
	struct SolverGroup {
		// Where this group's rows start, as an index into `PhysicsWorld::Rows`.
		uint32_t FirstRow = 0;

		// How many there are. The rows are contiguous, which is the point: a
		// sweep over a group is a walk rather than a filtered pass over the
		// whole list.
		uint32_t RowCount = 0;
	};

	// How long one body has been still, and whether it has been put to sleep.
	//
	// Kept per world rather than per row, because the alternative is the field
	// `scene::RigidBody` used to carry - the same state in two places, and only
	// readable by visiting the row it was meant to let the query skip. See
	// `AGENTS.md` in this directory for the whole of that decision.
	//
	// @since v0.4
	struct RestingBody {
		// Which entity this is.
		ecs::Entity Owner;

		// How long its velocity has stayed under the sleep thresholds, in
		// seconds. Reset to zero by any motion above them.
		float RestingSeconds = 0.0f;

		// Whether it is asleep, which is also whether `Publish` has taken its
		// `scene::Motion` away.
		bool Asleep = false;

		// Orders by entity, so the list is a sorted array and carrying it from
		// one tick to the next is a merge rather than a map lookup.
		//
		// @param other The row to compare.
		// @return `true` when this row sorts first.
		constexpr bool operator<(const RestingBody &other) const {
			return Owner.Id < other.Owner.Id;
		}
	};

	// The per-world index, pair list and contact buffers.
	//
	// Constructed once per world by `PreparePhysicsWorld` and written only by
	// this module's systems. Everything a caller may read is a `const` accessor;
	// the storage is private because its layout is the pipeline's business and
	// publishing it would make the layout an API.
	//
	// @since v0.4
	class PhysicsWorld {
	  public:
		// Constructs the buffers empty, with the static index marked stale so
		// that the first `SyncBroadphase` builds it.
		//
		// @param cellSize Grid cell edge in metres. Passed straight to
		//                 `spatial::HashGrid`, which refuses a value at or below
		//                 zero in favour of its own default.
		explicit PhysicsWorld(float cellSize = spatial::HashGrid::DEFAULT_CELL_SIZE);

		// Cell edge length of the dynamic index, in metres.
		//
		// **The two indexes may differ, and since v0.12 they usually do.** A
		// scene's static geometry is walls and floors and its dynamic set is
		// crates and characters; one spacing chosen for the union of them is
		// chosen for neither. This reports the dynamic one because that is the
		// index rebuilt every tick and therefore the one a profile is asking
		// about.
		//
		// @return The cell size in metres.
		float CellSize() const {
			return DynamicIndex.CellSize();
		}

		// Whether the grids size themselves from what they hold.
		//
		// **True unless the caller named a size**, which is the whole of the
		// rule: `PreparePhysicsWorld` with no cell size means "measure it" and
		// with one means "the author decided". A world that overrode a chosen
		// size every tick would make the parameter a suggestion, and a parameter
		// that is silently ignored is worse than one that is not there.
		//
		// @return `true` when `SyncBroadphase` calls `spatial::SuggestCellSize`.
		// @since v0.12
		bool CellSizeMeasured() const {
			return MeasureCells;
		}

		// The candidate pairs the last `BroadPhase` produced, sorted.
		//
		// Sorted by `(min id, max id)` and free of duplicates. Valid until the
		// next `BroadPhase`.
		//
		// @return The pairs, in solver visit order.
		std::span<const CandidatePair> Pairs() const {
			return PairList;
		}

		// The contact manifolds for this tick.
		//
		// Written by `NarrowPhase` and in the same order as `Pairs`, so a
		// consumer walking both walks them together. Valid until the next
		// `NarrowPhase`.
		//
		// **Empty now means nothing touched.** It did not always: before the
		// narrow phase existed it meant nobody had looked, and this accessor
		// carried a warning saying so. The warning is gone because the gap is.
		//
		// @return The manifolds, in solver visit order.
		std::span<const ContactManifold> Manifolds() const {
			return ManifoldList;
		}

		// The contact transitions for this tick.
		//
		// Written by `Publish` - one entry per pair that started touching,
		// kept touching, or stopped. In the same `(min id, max id)` order the
		// pairs are in, so two runs of a scene deliver them identically.
		//
		// @return The events for this tick.
		std::span<const ContactEvent> Events() const {
			return EventList;
		}

		// The bodies the solver gathered this tick.
		//
		// Only the ones a manifold names: a body nothing touches has no
		// constraint to solve and no velocity to write back. Sorted by entity
		// id.
		//
		// @return The solver's compact body array, valid until the next
		//         `Solve`.
		std::span<const SolverBody> Bodies() const {
			return BodyList;
		}

		// Whether a body is asleep, and therefore out of the dynamic set.
		//
		// **The one reader of the sleeping state**, and the reason it is a
		// query rather than a field on `scene::RigidBody`: the state belongs to
		// the solver, and a debug view asking about one body should not make
		// every physics query load a byte it never reads.
		//
		// @param entity The body to ask about.
		// @return `true` when the solver has put it to sleep.
		bool Sleeping(ecs::Entity entity) const;

		// How many bodies are asleep.
		//
		// @return The count, which a settling scene drives up and a disturbance
		//         drives down.
		size_t SleepingBodies() const;

		// Takes a body out of the resting set, so the next tick simulates it.
		//
		// **The verb that was missing, and a character controller is what
		// noticed.** Everything that woke a body until now was a *contact* - the
		// solver's wake pass gives a sleeping body back to the dynamic set when
		// an awake neighbour touches it. That is the whole of the mechanism, and
		// it cannot express "this body has been told to move": a character
		// standing still settles, loses its `scene::Motion` to the archetype
		// move in `Publish`, and from then on has nothing for `scene::
		// StepCharacters` to write a velocity into. It stands there for ever
		// with a perfectly good move direction on it.
		//
		// **It clears the whole resting record rather than only the flag**, so
		// the body starts accumulating rest again from zero. Clearing the flag
		// alone would leave the timer at its threshold, and the body would sleep
		// again on the very next tick it was not being pushed - which is a
		// character that walks in single frames.
		//
		// Costs a binary search and is a no-op for a body that was already
		// awake, so calling it every tick for every character is fine.
		//
		// @param entity The body to wake.
		// @return `true` when it was asleep and is not any more.
		bool Wake(ecs::Entity entity);

		// Records that the static geometry changed and its index must be rebuilt.
		//
		// `SyncBroadphase` detects the ordinary cases itself - a static collider
		// created, destroyed, or written through `Store::Set`. This is for the
		// case it cannot see: a transform written through a raw column pointer
		// in the batch path, which advances no per-row stamp. Calling it when
		// nothing moved costs one rebuild and is always safe; not calling it
		// when something did means a collider that collides where it used to be.
		void MarkStaticDirty() {
			StaticStale = true;
		}

		// Whether the static index will be rebuilt on the next `SyncBroadphase`.
		//
		// @return `true` when the static set is known to be out of date.
		bool StaticDirty() const {
			return StaticStale;
		}

		// Whether two parts belong to the same active rigid assembly.
		bool RigidlyConnected(ecs::Entity first, ecs::Entity second) const;

		// How many colliders the dynamic index held after the last sync.
		//
		// @return The dynamic collider count.
		size_t DynamicColliders() const {
			return DynamicRecords.size();
		}

		// How many colliders the static index held after the last sync.
		//
		// @return The static collider count.
		size_t StaticColliders() const {
			return StaticRecords.size();
		}

		// How many times the dynamic index has been rebuilt.
		//
		// Once per tick with anything moving in the world, which is what makes
		// the static counter beside it worth reading.
		//
		// @return A counter that only increases.
		uint64_t DynamicRebuilds() const {
			return DynamicRebuildCount;
		}

		// How many times the static index has been rebuilt.
		//
		// **The number the second-grid design exists to keep small.** A world
		// whose static rebuild count climbs with its tick count has static
		// geometry being re-measured every tick, which is the cost
		// `spatial/AGENTS.md` says the second grid removes - and it is what the
		// suite for this module asserts against.
		//
		// @return A counter that only increases.
		uint64_t StaticRebuilds() const {
			return StaticRebuildCount;
		}

		// How many contact rows the last solve built.
		//
		// **The length of `Rows()`, which is not `Rows().size()`.** The row
		// array is grown and never shrunk, so its size is the largest this world
		// has ever needed and its tail is whatever a busier tick left there.
		//
		// @return Rows as of the last `Solve`.
		size_t RowCount() const {
			return SolverRowCount;
		}

		// How many bodies have been stopped at a surface they would otherwise
		// have passed through.
		//
		// **The number that says whether continuous collision is doing anything
		// in a given scene**, and the one to read when a world is suspiciously
		// slow: a sweep is a distance query per candidate per fast body, so a
		// scene where this climbs every tick is a scene whose bodies are moving
		// further than their own thickness every tick.
		//
		// @return A counter that only increases.
		uint64_t SweptBodies() const {
			return SweptBodyCount;
		}

		// How many independent groups the last solve split its rows into.
		//
		// **The number that says whether a scene can use the machine.** One
		// group is a solve that runs on one thread whatever the pool holds; a
		// scene wanting every worker busy needs several groups per worker,
		// because they are not the same size. Zero means the solve took the
		// serial path, which it does below `PARALLEL_SOLVE_ROWS`.
		//
		// @return Groups as of the last `Solve`.
		size_t SolverGroupCount() const {
			return SolverGroups.size();
		}

		// How many contact rows the last solve could not put in a group.
		//
		// **The Amdahl term, and the reason it is worth publishing.** These are
		// the contacts whose two movable bodies landed in different chunks, and
		// they are solved on one thread after every group has finished - so a
		// scene where this is a large fraction of `Rows().size()` is a scene
		// whose chunks are too small for what is in them.
		//
		// @return Border rows as of the last `Solve`.
		size_t BorderRowCount() const {
			return BorderRows.RowCount;
		}

		// The chunk edge the last solve partitioned its bodies with, in metres.
		//
		// **Zero means the solve took the serial path**, which it does below
		// `PARALLEL_SOLVE_ROWS`. Reading the map's own size instead would report
		// whatever edge it was last set to - or its default on a world that has
		// never partitioned at all - and a number that looks like an answer is
		// worse than one that says there is none.
		//
		// @return A size in metres, or zero when the solve took the serial path.
		float SolverChunkSize() const {
			return SolverChunkEdge;
		}

	  private:
		// The two indexes and the arrays behind them.
		//
		// `Proxy::Id` in these grids is the **index into the matching record and
		// proxy arrays**, not an `ecs::Entity`. That is what makes resolving a
		// candidate's masks an array subscript rather than a store lookup; the
		// entity is on the record. A caller reaching one of these grids and
		// reading `Proxy::Id` as an entity gets a number that is plausible and
		// wrong, which is why neither grid is reachable from outside.
		spatial::HashGrid DynamicIndex;
		spatial::HashGrid StaticIndex;

		std::vector<spatial::Proxy> DynamicProxies;
		std::vector<ColliderRecord> DynamicRecords;
		std::vector<spatial::Proxy> StaticProxies;
		std::vector<ColliderRecord> StaticRecords;

		// The placed shape of every collider, parallel to the records.
		//
		// **Filled by `SyncBroadphase`, which is already holding the two
		// components it needs.** See `PlacedCollider`: this exists so the narrow
		// phase never touches the store, which is what lets it be dispatched.
		//
		// **Rebuilt on the same schedule as the index beside it**, so a stale
		// static index and a stale static shape go stale together. That is the
		// stronger of the two arrangements: the alternative was a stale index
		// and a *fresh* transform read per pair, which is two descriptions of
		// where a wall is disagreeing inside one step.
		std::vector<PlacedCollider> DynamicShapes;
		std::vector<PlacedCollider> StaticShapes;

		std::vector<CandidatePair> PairList;

		// Which proxy each pair came from, parallel to `PairList` and sorted
		// with it. See `CandidateSource`.
		std::vector<CandidateSource> PairSourceList;

		// Where the two are sorted together before being split. Cleared and
		// refilled, never freed, like every other list here.
		std::vector<SourcedPair> SourcedPairList;
		std::vector<ContactManifold> ManifoldList;
		std::vector<ContactEvent> EventList;

		// One slot per candidate pair, and whether that pair turned out to touch.
		//
		// **What lets the narrow phase be dispatched without a shared cursor.**
		// Workers write the slot their own pair owns and nothing else, and the
		// compaction that follows walks the flags in pair order - so the
		// manifold list is a function of the pair list rather than of which
		// worker finished first, which is what the solver's order-dependence
		// requires.
		//
		// `ManifoldSlots.size()` is a high-water mark, like `RowList`'s: only
		// the slots a flag points at are ever read.
		std::vector<ContactManifold> ManifoldSlots;
		std::vector<uint8_t> ManifoldTouching;

		// What the solver works on, refilled every tick from the manifolds.
		//
		// **`RowList.size()` is a high-water mark and not this tick's row
		// count.** Every byte of every row is written by the set-up pass, so
		// resizing to the exact count would memset megabytes a tick to produce
		// zeroes nothing reads. The vector therefore only ever grows, and
		// `SolverRowCount` is the length anything walking it must take. Reading
		// `size()` instead is a walk over last tick's tail, and a stale row is a
		// contact between two bodies that are no longer touching.
		std::vector<SolverBody> BodyList;
		std::vector<ContactRow> RowList;
		size_t SolverRowCount = 0;

		// The scratch the solver's gather sorts, and where it puts the body
		// indices it resolves per manifold.
		//
		// **Entity ids rather than bodies**, because the gather's sort is over
		// two entries per manifold and a `SolverBody` is fifteen times the size
		// of the id the sort is keyed on. Sorting the bodies moved megabytes to
		// order kilobytes of information.
		//
		// Cleared and refilled, never freed, like every other list here.
		std::vector<ecs::Entity> BodyOwners;
		std::vector<std::pair<uint32_t, uint32_t>> ManifoldBodies;

		// The spatial partition the solve is batched by, and the points it is
		// built from.
		//
		// **Over the solver's bodies rather than over the broad phase's
		// proxies**, and they are not the same set: the bodies are the ones a
		// manifold named, which is a fraction of the colliders in a busy world
		// and includes anchored geometry the dynamic index never held. Reusing
		// the dynamic proxies would put a resting body in no chunk and a body
		// touching a wall in a chunk that does not contain the wall.
		//
		// A point per body rather than its box, because the partition wants one
		// answer per body - see `spatial::ChunkMap`, which explains why binning
		// a box gives a set and a set cannot be split across workers.
		spatial::ChunkMap SolverChunks;
		std::vector<spatial::Proxy> SolverPoints;

		// Which group each manifold's rows went into, and the groups themselves.
		//
		// `GroupOfManifold` is the counting pass's answer and the filling pass's
		// input, kept between them rather than recomputed - the alternative is
		// asking each manifold's two bodies for their chunks twice, which is two
		// random accesses per manifold for a number that is already known.
		//
		// `SolverGroups` holds only the groups that ended up with rows in them,
		// so a dispatch over it never hands a worker an empty range.
		// The chunk edge the last partitioned solve used, or zero for a solve
		// that did not partition. See `SolverChunkSize`.
		float SolverChunkEdge = 0.0f;

		std::vector<uint32_t> GroupOfManifold;
		std::vector<uint32_t> GroupRowStart;
		std::vector<uint32_t> GroupRowCursor;

		// Where each manifold's rows begin, as an index into `RowList`.
		//
		// **Handed out before any row is built, which is what lets the set-up
		// pass be dispatched.** A cursor advanced while the rows are built is a
		// shared write and would have pinned the expensive half of the solve to
		// one thread; this is the cheap walk that turns it into an address per
		// manifold nobody else writes to.
		std::vector<uint32_t> RowStartOfManifold;

		// Where each manifold's impulses begin, as an index into `ImpulseNext`.
		//
		// **Not the same offset as `RowStartOfManifold`, and the difference is
		// the reason both exist.** The rows are grouped by chunk so a sweep can
		// walk one group; the impulse cache is a sorted array a binary search
		// answers, so it has to come out in pair order. The manifolds are
		// already sorted on the pair, so this is a running total over them.
		std::vector<uint32_t> ImpulseStartOfManifold;
		std::vector<SolverGroup> SolverGroups;

		// The rows whose two movable bodies landed in different chunks.
		//
		// **Solved after every group and on one thread**, because they are what
		// the groups are disjoint *despite*: a contact straddling a chunk face
		// names a body in each, so it conflicts with both chunks' interiors. It
		// is a surface-area effect - the fraction of contacts that straddle
		// falls as a chunk holds more bodies - and `PhysicsWorld::BorderRows` is
		// what says whether it has stopped being one in a given scene.
		SolverGroup BorderRows;

		// The impulses, double-buffered: `ImpulseCache` is what this tick's
		// warm start reads and `ImpulseNext` is what it writes for the tick
		// after. Two buffers rather than one, because a tick both reads last
		// tick's answer and records this one, and a single array would have to
		// be searched while it was being rewritten.
		std::vector<ContactImpulse> ImpulseCache;
		std::vector<ContactImpulse> ImpulseNext;

		// Which bodies have been still, and for how long.
		//
		// **The one thing here that outlives a tick's contacts.** A body that
		// fell asleep on top of static geometry produces no candidate pair at
		// all - both sides are anchored as far as the broad phase is concerned
		// - so it is not gathered, and an entry rebuilt from this tick's bodies
		// would lose it and wake it for no reason. Entries for sleepers are
		// carried; entries for awake bodies that stopped touching anything are
		// not, because a body in mid-air is not resting.
		std::vector<RestingBody> RestingList;
		std::vector<RestingBody> RestingNext;

		// Rigid-link state and scratch, all retained so a steady assembly stops
		// allocating. Weld poses outlive a tick; edges and nodes are rebuilt.
		std::vector<WeldPose> WeldPoses;
		std::vector<WeldPose> WeldPosesNext;
		std::vector<RigidEdge> RigidEdges;
		std::vector<RigidNode> RigidNodes;

		// The pairs that were touching last tick, sorted, so `Publish` can say
		// which began, which persisted and which ended without holding a set.
		std::vector<CandidatePair> TouchingLast;
		std::vector<CandidatePair> TouchingNow;

		// Where an overlap query writes its candidate ids. One buffer reused by
		// every query in a tick, grown to the largest answer any of them needed.
		std::vector<uint64_t> CandidateBuffer;

		bool StaticStale = true;

		// Whether the grids size themselves. See `CellSizeMeasured`.
		//
		// **Snapshotted with the cell size**, because it decides what the
		// restored size means: a measured world re-derives one on its first sync
		// and a configured world keeps the author's.
		bool MeasureCells = true;

		// `Store::ChangeVersion()` as of the last time the static set was
		// examined. The counter only moves for a write through `Set` to an
		// observed component, so an unchanged one means nothing authored has
		// happened and the changed-row walk can be skipped entirely - which is
		// what keeps the static index from being rebuilt every tick in a store
		// whose dirty bits nobody clears.
		uint64_t StaticChangeVersion = 0;

		uint64_t DynamicRebuildCount = 0;
		uint64_t StaticRebuildCount = 0;

		// How many bodies the continuous step has clamped short of a surface.
		uint64_t SweptBodyCount = 0;

		// The systems, the suites and the benchmarks all read the arrays above,
		// and not one of them is another module. Publishing the storage to reach
		// it would turn the layout into an API somebody outside could depend on;
		// this keeps it inside `src/`, which is what the private include
		// directory is for. `spatial::HashGrid` does the same with
		// `GridInternals` and for the same reason.
		friend struct PipelineInternals;
	};
}
