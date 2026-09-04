#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Solver.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/spatial/ChunkMap.hpp>
#include <engine/spatial/LayerMask.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

// `_mm_prefetch` lives here, and only MSVC needs it.
#if defined(_MSC_VER) && !defined(__clang__)
#include <xmmintrin.h>
#endif

namespace engine::physics {

	namespace {
		// A pair-sorted read over the public touching manifolds and the private
		// speculative ones. The order vector is four words per pair rather than a
		// second copy of every four-point manifold.
		struct SolverManifoldView {
			const std::vector<ContactManifold> &Touching;
			const std::vector<ContactManifold> &Speculative;
			const std::vector<SolverManifoldIndex> &Order;

			size_t size() const {
				return Order.empty() ? Touching.size() : Order.size();
			}

			const ContactManifold &operator[](size_t index) const {
				if (Order.empty()) {
					return Touching[index];
				}
				const SolverManifoldIndex entry = Order[index];
				return entry.Speculative ? Speculative[entry.Index] : Touching[entry.Index];
			}
		};

		// One cache line asked for early. **A hint and nothing else** - it changes
		// no result, so a toolchain with no way to spell it does nothing and the
		// solver is correct and slower.
		//
		// Three spellings because there is no portable one. GCC and Clang have
		// `__builtin_prefetch`, asked for through `__has_builtin` rather than
		// assumed - the nested `#if` is the form `ecs/TypeDescriptor.hpp` uses, and
		// it is nested because `__has_builtin(x)` on a compiler that does not
		// define `__has_builtin` expands to `0(0)` and is a preprocessor error
		// rather than a false. MSVC has `_mm_prefetch` and no builtin, which is
		// what stopped the Windows release build at `error C3861`.
		inline void Prefetch([[maybe_unused]] const void *address) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_prefetch)
			__builtin_prefetch(address);
			return;
#endif
#endif
#if defined(_MSC_VER) && !defined(__clang__)
			_mm_prefetch(static_cast<const char *>(address), _MM_HINT_T0);
#endif
		}
		// The group id for a manifold that contributes no rows at all.
		//
		// A trigger, or a pair the solver could not move either half of. Marked
		// rather than removed, because the pass that decides it and the pass
		// that fills the rows are separated by the partition, and re-deciding it
		// in the second pass is two chances to disagree with the count the
		// offsets were built from - which would not misplace a row, it would
		// write one past the end of a group.
		constexpr uint32_t SKIPPED_MANIFOLD = UINT32_MAX;

		// Reports the shape the scheduling pass produced. `groups` is longest
		// first, so its ends and middle give the exact min, max and nearest-rank
		// median without allocating or changing the scheduler's dispatch order.
		// These are gauges because they describe the latest solve, not a rate.
		void ReportSolverGroupShape(
			const std::vector<SolverGroup> &groups, SolverGroup border, size_t interiorRowCount
		) {
			const double groupCount = static_cast<double>(groups.size());
			const double minimum = groups.empty() ? 0.0 : static_cast<double>(groups.back().RowCount);
			const double median =
				groups.empty() ? 0.0 : static_cast<double>(groups[groups.size() / 2].RowCount);
			const double maximum = groups.empty() ? 0.0 : static_cast<double>(groups.front().RowCount);

			core::Metrics::SetGauge("physics.solve.groups", groupCount);
			core::Metrics::SetGauge("physics.solve.group-rows.min", minimum);
			core::Metrics::SetGauge("physics.solve.group-rows.median", median);
			core::Metrics::SetGauge("physics.solve.group-rows.max", maximum);
			core::Metrics::SetGauge("physics.solve.interior-rows", static_cast<double>(interiorRowCount));
			core::Metrics::SetGauge("physics.solve.border-rows", static_cast<double>(border.RowCount));
		}

		// Colour metrics describe the exact wave plan rather than the worker pool.
		// A scene therefore reports the same plan on a one-thread replay and on a
		// wide machine, which is the determinism boundary this solver needs.
		void ReportSolverColorShape(const std::vector<SolverColor> &colors, size_t retainedBytes) {
			double minimum = 0.0;
			double median = 0.0;
			double maximum = 0.0;
			if (!colors.empty()) {
				std::array<uint32_t, SOLVER_COLOR_MAX_WAVES> rowCounts{};
				for (size_t color = 0; color < colors.size(); color++) {
					rowCounts[color] = colors[color].RowCount;
				}
				std::sort(rowCounts.begin(), rowCounts.begin() + colors.size());
				minimum = static_cast<double>(rowCounts.front());
				median = static_cast<double>(rowCounts[colors.size() / 2]);
				maximum = static_cast<double>(rowCounts[colors.size() - 1]);
			}
			core::Metrics::SetGauge("physics.solve.colors", static_cast<double>(colors.size()));
			core::Metrics::SetGauge("physics.solve.color-rows.min", minimum);
			core::Metrics::SetGauge("physics.solve.color-rows.median", median);
			core::Metrics::SetGauge("physics.solve.color-rows.max", maximum);
			core::Metrics::SetGauge("physics.solve.color.retained-bytes", static_cast<double>(retainedBytes));
		}

		// A coloured group is one manifold and therefore has at most four rows.
		// A fixed histogram reports its actual distribution without sorting the
		// wave-owned dispatch ranges or allocating measurement scratch.
		void ReportColoredGroupShape(const std::vector<SolverGroup> &groups, size_t rowCount) {
			std::array<size_t, ContactManifold::MAXIMUM_POINTS + 1> counts{};
			for (const SolverGroup &group : groups) {
				counts[group.RowCount]++;
			}

			size_t minimum = 0;
			size_t median = 0;
			size_t maximum = 0;
			size_t seen = 0;
			for (size_t rows = 1; rows < counts.size(); rows++) {
				if (counts[rows] == 0) {
					continue;
				}
				if (minimum == 0) {
					minimum = rows;
				}
				seen += counts[rows];
				if (median == 0 && seen > groups.size() / 2) {
					median = rows;
				}
				maximum = rows;
			}

			core::Metrics::SetGauge("physics.solve.groups", static_cast<double>(groups.size()));
			core::Metrics::SetGauge("physics.solve.group-rows.min", static_cast<double>(minimum));
			core::Metrics::SetGauge("physics.solve.group-rows.median", static_cast<double>(median));
			core::Metrics::SetGauge("physics.solve.group-rows.max", static_cast<double>(maximum));
			core::Metrics::SetGauge("physics.solve.interior-rows", static_cast<double>(rowCount));
			core::Metrics::SetGauge("physics.solve.border-rows", 0.0);
		}

		// Island metrics are bounded by one retained scratch array. The row list is
		// canonical-root ordered, so sorting its copy only affects measurement.
		void ReportSolverIslandShape(
			const std::vector<uint32_t> &rows,
			std::vector<uint32_t> &orderScratch,
			bool reused,
			bool selected,
			size_t retainedBytes
		) {
			double minimum = 0.0;
			double median = 0.0;
			double maximum = 0.0;
			double largestShare = 0.0;
			if (!rows.empty()) {
				orderScratch = rows;
				std::sort(orderScratch.begin(), orderScratch.end());
				minimum = static_cast<double>(orderScratch.front());
				median = static_cast<double>(orderScratch[orderScratch.size() / 2]);
				maximum = static_cast<double>(orderScratch.back());
				const size_t total = std::accumulate(rows.begin(), rows.end(), size_t{0});
				largestShare = total == 0 ? 0.0 : maximum / static_cast<double>(total);
			}

			core::Metrics::SetGauge("physics.solve.islands", static_cast<double>(rows.size()));
			core::Metrics::SetGauge("physics.solve.island-rows.min", minimum);
			core::Metrics::SetGauge("physics.solve.island-rows.median", median);
			core::Metrics::SetGauge("physics.solve.island-rows.max", maximum);
			core::Metrics::SetGauge("physics.solve.island-largest-share", largestShare);
			core::Metrics::SetGauge("physics.solve.island.selected", selected ? 1.0 : 0.0);
			core::Metrics::SetGauge("physics.solve.island.reused", reused ? 1.0 : 0.0);
			core::Metrics::SetGauge(
				"physics.solve.island.retained-bytes", static_cast<double>(retainedBytes)
			);
		}

		size_t SolverColorRetainedBytes(PhysicsWorld &world) {
			return PipelineInternals::SolverColorOfManifold(world).capacity() * sizeof(uint32_t) +
				   PipelineInternals::SolverColorClaims(world).capacity() * sizeof(uint64_t) +
				   PipelineInternals::SolverColors(world).capacity() * sizeof(SolverColor);
		}

		uint32_t FindSolverComponent(std::vector<uint32_t> &parents, uint32_t body) {
			uint32_t root = body;
			while (parents[root] != root) {
				root = parents[root];
			}
			while (parents[body] != body) {
				const uint32_t next = parents[body];
				parents[body] = root;
				body = next;
			}
			return root;
		}

		void JoinSolverComponents(std::vector<uint32_t> &parents, uint32_t first, uint32_t second) {
			first = FindSolverComponent(parents, first);
			second = FindSolverComponent(parents, second);
			if (first != second) {
				parents[std::max(first, second)] = std::min(first, second);
			}
		}

		// An entity id has a fixed width, so ordering the compact solver bodies
		// takes six stable passes instead of comparison-sorting every contact's
		// two owners. Eleven-bit buckets fit in L1 and avoid an extra full walk
		// over a large contact set.
		constexpr size_t OWNER_RADIX_BITS = 11;
		constexpr size_t OWNER_RADIX_BUCKETS = size_t{1} << OWNER_RADIX_BITS;
		constexpr size_t OWNER_RADIX_PASSES =
			(sizeof(uint64_t) * 8 + OWNER_RADIX_BITS - 1) / OWNER_RADIX_BITS;

		void SortOwners(std::vector<ecs::Entity> &owners, std::vector<ecs::Entity> &scratch) {
			if (owners.size() < 2) {
				return;
			}

			scratch.resize(owners.size());
			for (size_t pass = 0; pass < OWNER_RADIX_PASSES; pass++) {
				const size_t shift = pass * OWNER_RADIX_BITS;
				std::array<size_t, OWNER_RADIX_BUCKETS> offsets{};
				for (ecs::Entity owner : owners) {
					const size_t bucket =
						static_cast<size_t>((owner.Id >> shift) & (OWNER_RADIX_BUCKETS - 1));
					offsets[bucket]++;
				}

				size_t next = 0;
				for (size_t &offset : offsets) {
					const size_t count = offset;
					offset = next;
					next += count;
				}

				for (ecs::Entity owner : owners) {
					const size_t bucket =
						static_cast<size_t>((owner.Id >> shift) & (OWNER_RADIX_BUCKETS - 1));
					scratch[offsets[bucket]++] = owner;
				}
				owners.swap(scratch);
			}
		}

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
		// that is what `Collider::Extent` is - reading them as full extents
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

			case scene::ShapeKind::Capsule: {
				const float radius = std::max(extentX, 0.0f);
				const float halfSegment = std::max(extentY, 0.0f);
				const float cylinderVolume =
					std::numbers::pi_v<float> * radius * radius * (2.0f * halfSegment);
				const float sphereVolume =
					(4.0f / 3.0f) * std::numbers::pi_v<float> * radius * radius * radius;
				const float totalVolume = cylinderVolume + sphereVolume;
				const float cylinderMass = totalVolume > 0.0f ? mass * cylinderVolume / totalVolume : 0.0f;
				const float sphereMass = mass - cylinderMass;
				const float radiusSquared = radius * radius;
				const float axial = 0.5f * cylinderMass * radiusSquared + 0.4f * sphereMass * radiusSquared;
				const float capCentroid = halfSegment + 3.0f * radius / 8.0f;
				const float across =
					cylinderMass * (3.0f * radiusSquared + 4.0f * halfSegment * halfSegment) / 12.0f +
					sphereMass * (83.0f * radiusSquared / 320.0f + capCentroid * capCentroid);
				inertia = core::Vector3{across, axial, across};
				break;
			}

			case scene::ShapeKind::Hull:
			case scene::ShapeKind::Mesh:
				// **The box tensor of the part's own extent**, which is the same
				// choice `scene::VolumeOf` makes about the mass and for the same
				// reason: the exact tensor of a baked hull is an integral over
				// its tetrahedra, it would have to be recomputed whenever the
				// shape table changed, and it differs from this by a factor a
				// designer will not notice on a hull that is a good fit for its
				// part. A hull that is *not* a good fit is a scene mistake, and
				// an exact tensor would only hide it.
				//
				// A mesh collider is static in practice, so its tensor is read
				// only by a world that made one dynamic - where a box is the
				// answer least likely to be surprising.
				inertia = core::Vector3{
					mass * (extentY * extentY + extentZ * extentZ) / 3.0f,
					mass * (extentX * extentX + extentZ * extentZ) / 3.0f,
					mass * (extentX * extentX + extentY * extentY) / 3.0f,
				};
				break;
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
		// scalar triple product written the cheap way round - see
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
		// of the normal alone, which is what keeps the friction basis - and
		// therefore the warm start that reuses its impulses - the same from one
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
			const float angular =
				axis.FirstAngular.Dot(axis.FirstTorque) + axis.SecondAngular.Dot(axis.SecondTorque);
			const float total = linear + angular;
			axis.Mass = total > 0.0f ? 1.0f / total : 0.0f;
		}

		// **Nothing is written to a body the solver may not move**, and the two
		// branches are what make a parallel sweep possible rather than a
		// micro-optimisation.
		//
		// Every term below is scaled by the body's inverse mass or by an angular
		// response built from its inverse inertia, and both are zero for an
		// immovable body - so the writes were always adding zero and skipping
		// them changes no number anywhere. What it changes is that two workers
		// sweeping two groups that share a floor now only *read* that floor.
		// Without the branch they would both store to it, which is a data race
		// by the letter of the language and a cache line bouncing between cores
		// by the letter of the machine, on the one body every contact in a scene
		// names.
		//
		// The branch is predictable: a body's movability is fixed for the tick.
		void ApplyImpulse(SolverBody &first, SolverBody &second, const ContactAxis &axis, float magnitude) {
			// The normal points from the first body toward the second, so a
			// positive impulse pushes the second away and the first back. Every
			// pair function obeys that one convention, which is why this is the
			// only place the sign appears.
			if (first.Movable) {
				first.LinearVelocity =
					first.LinearVelocity - axis.Direction * (magnitude * first.InverseMass);
				first.AngularVelocity = first.AngularVelocity - axis.FirstAngular * magnitude;
			}
			if (second.Movable) {
				second.LinearVelocity =
					second.LinearVelocity + axis.Direction * (magnitude * second.InverseMass);
				second.AngularVelocity = second.AngularVelocity + axis.SecondAngular * magnitude;
			}
		}

		// The same push, against the velocity that only moves positions.
		//
		// Translation only. See `SolverBody::CorrectionLinear` for why there is
		// no angular half: a rotation nothing damps is a lean that grows.
		void ApplyCorrection(SolverBody &first, SolverBody &second, const ContactRow &row, float magnitude) {
			const core::Vector3 &normal = row.Along[ContactRow::NORMAL].Direction;
			if (first.Movable) {
				first.CorrectionLinear = first.CorrectionLinear - normal * (magnitude * first.InverseMass);
			}
			if (second.Movable) {
				second.CorrectionLinear = second.CorrectionLinear + normal * (magnitude * second.InverseMass);
			}
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

		float FindSpeculativeClosingSpeed(
			const std::vector<ContactImpulse> &cache, ecs::Entity first, ecs::Entity second
		) {
			const ContactImpulse key{first, second, 0, 0.0f, {0.0f, 0.0f}};
			auto found = std::lower_bound(cache.begin(), cache.end(), key);
			float speed = 0.0f;
			while (found != cache.end() && found->A == first && found->B == second) {
				speed = std::max(speed, found->SpeculativeClosingSpeed);
				found++;
			}
			return speed;
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
			const scene::PhysicsProperties *physical,
			bool simulated
		) {
			// **Whether the world may move it is asked first, and it used to be
			// asked by omission.** Until v0.15 an anchored part had no
			// `RigidBody` at all, so a null pointer here meant "the world may
			// not move this" and the infinite mass below was the right answer to
			// it. Every part carries one now - it is what the part weighs, not
			// whether it may be pushed - so the question is asked through
			// `scene::Simulated` instead. Left as it was, every anchored floor
			// in every scene became a dynamic body and the things standing on it
			// fell through.
			//
			// **A sleeping body arrives here with `simulated` true**, and that
			// is deliberate rather than an oversight. It has no `scene::Motion`,
			// so the broad phase has it in the static index, but it is a real
			// body with a real mass and the wake pass below needs it recognised
			// as one - `body.Asleep` is gated on `Movable`, which is what this
			// returns. Test `Motion` here instead and a settled crate becomes a
			// wall that nothing can ever wake.
			if (!simulated || body == nullptr || collider == nullptr) {
				// Not a body at all - `scene::Enums` is explicit that this is
				// not the same as a static one. It still stops things, which is
				// exactly what an infinite mass does.
				return BodyFacts{};
			}

			// **`scene::MassOf` and not `body->Mass`, because density is a mass
			// too.** A part with `CustomPhysicalProperties` weighs its density
			// times its volume, and the properties panel shows the same number
			// through the same function - a solver with its own arithmetic here
			// would be a part that weighs one thing and reads as another.
			const float mass = scene::MassOf(*collider, *body, physical);
			if (body->Kind != scene::BodyKind::Dynamic || !(mass > 0.0f)) {
				return BodyFacts{};
			}
			return BodyFacts{1.0f / mass, InverseInertiaOf(*collider, mass), true};
		}

		// Loads the physics facts shared by the compact body's normal archetype
		// walk and the uncommon fallback. Custom entities may omit BasePart
		// columns, so absence remains a supported input rather than a shortcut.
		void LoadBody(
			SolverBody &body,
			const scene::Transform *transform,
			const scene::Motion *motion,
			const scene::Collider *collider,
			const scene::RigidBody *rigid,
			const scene::Surface *surface,
			const scene::PhysicsProperties *physical,
			bool simulated,
			const scene::SurfaceTable *surfaces
		) {
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

			scene::SurfaceProperties properties;
			if (surfaces != nullptr && surface != nullptr) {
				const scene::SurfaceProperties *row = surfaces->Find(surface->Material);
				if (row != nullptr) {
					properties = *row;
				}
			}
			body.Friction = properties.Friction;
			body.Restitution = properties.Restitution;
			if (physical != nullptr && physical->Custom) {
				body.Friction = physical->Friction;
				body.Restitution = physical->Elasticity;
			}

			const BodyFacts facts = FactsFor(rigid, collider, physical, simulated);
			body.InverseMass = facts.InverseMass;
			body.InverseInertia = facts.InverseInertia;
			body.Movable = facts.Dynamic;
		}

		// One sequential-impulse sweep over one contiguous run of rows.
		//
		// **Extracted so a run can be handed to a worker, and it is still the
		// same serial method inside the run.** Sequential impulse works by
		// letting each row see what the rows before it left behind, and this
		// loop does exactly that for the rows it is given. What changed in v0.17
		// is that the *list* is cut into runs no two of which share a body the
		// solver may write - see `SolverGroup` - so running two of them at once
		// gives the answer running them one after the other would.
		//
		// @param bodies The solver's body array, written through.
		// @param rows   Every row, of which this call touches `[first, first +
		//               count)`.
		void SweepRows(std::vector<SolverBody> &bodies, std::vector<ContactRow> &rows, SolverGroup run) {
			const size_t last = static_cast<size_t>(run.FirstRow) + run.RowCount;

			for (size_t at = run.FirstRow; at < last; at++) {
				ContactRow &row = rows[at];

				// **The two bodies of the row after this one, fetched early.**
				// The rows stream in order and the prefetcher handles them; the
				// bodies they name are two random offsets into an array that
				// stops fitting in cache somewhere around a few thousand
				// contacts, and the row's arithmetic cannot start until they
				// arrive. Asking for them one row ahead is what turns that wait
				// into work already done. A hint only - it changes no result,
				// and the bounds test costs one predictable branch per row.
				//
				// **Bounded by the run and not by the list**, since v0.17. A
				// prefetch past the end of a run is harmless - it is a hint at a
				// row somebody else may be writing, and a hint has no result -
				// but it is also a line pulled in that this worker will not use.
				if (at + 1 < last) {
					Prefetch(&bodies[rows[at + 1].First]);
					Prefetch(&bodies[rows[at + 1].Second]);
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
				// `scene::Motion` - so a body at rest ends the tick at rest
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
	}

	void Solve(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.solve", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		const float delta = PhysicsStepSeconds(store);
		const SolverManifoldView manifolds{
			PipelineInternals::Manifolds(*world),
			PipelineInternals::SpeculativeManifolds(*world),
			PipelineInternals::SolverManifoldOrder(*world),
		};
		std::vector<SolverBody> &bodies = PipelineInternals::Bodies(*world);
		std::vector<ContactRow> &rows = PipelineInternals::Rows(*world);
		bodies.clear();

		// --- gather ----------------------------------------------------------
		//
		// Only the bodies a manifold names. A body nothing touches has no
		// constraint to solve and no velocity for `Publish` to write back, so
		// gathering every dynamic row would be a pass over the world to find
		// the few that are in contact - the shape `CODE_QUALITY.md` names.
		//
		// **The sort is over entity ids, not over bodies.** Two entries per
		// manifold go in and only `Owner` is meaningful at this point, so
		// sorting `SolverBody` moved a hundred and twenty bytes to order the
		// eight the comparison reads. At ten thousand bodies that was megabytes
		// of moves for kilobytes of information.
		std::vector<ecs::Entity> &owners = PipelineInternals::BodyOwners(*world);
		std::unordered_map<uint64_t, size_t> &bodyIndex = PipelineInternals::BodyIndexByOwner(*world);
		{
			ENGINE_PROFILE_CAT("physics.solve-gather", core::ProfileCategory::Physics);
			{
				ENGINE_PROFILE_CAT("physics.solve-gather-owners", core::ProfileCategory::Physics);
				owners.clear();
				owners.reserve(manifolds.size() * 2);
				for (size_t at = 0; at < manifolds.size(); at++) {
					const ContactManifold &manifold = manifolds[at];
					owners.push_back(manifold.A);
					owners.push_back(manifold.B);
				}
			}
			{
				ENGINE_PROFILE_CAT("physics.solve-gather-sort", core::ProfileCategory::Physics);
				SortOwners(owners, PipelineInternals::BodyOwnerSortScratch(*world));
				owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
			}

			bodies.resize(owners.size());
			for (size_t index = 0; index < owners.size(); index++) {
				bodies[index] = SolverBody{};
				bodies[index].Owner = owners[index];
			}

			{
				ENGINE_PROFILE_CAT("physics.solve-gather-index", core::ProfileCategory::Physics);
				bodyIndex.clear();
				bodyIndex.reserve(bodies.size());
				for (size_t index = 0; index < bodies.size(); index++) {
					bodyIndex.emplace(bodies[index].Owner.Id, index);
				}
			}
		}

		// **Every manifold's two body indices, resolved once.** Three later
		// passes want them, so a retained lookup table saves repeatedly searching
		// the sorted body array for the same two known owners.
		// **Dispatched, because the lookups are reads and the answers do not share
		// a slot.** Each manifold owns one result, and the table is complete
		// before workers start reading it.
		std::vector<std::pair<uint32_t, uint32_t>> &located = PipelineInternals::ManifoldBodies(*world);
		located.resize(manifolds.size());
		{
			ENGINE_PROFILE_CAT("physics.solve-locate", core::ProfileCategory::Physics);
			parallel::Jobs::For(
				manifolds.size(),
				LOCATE_GRAIN,
				[&located, &manifolds, &bodyIndex](size_t begin, size_t end) {
					for (size_t at = begin; at < end; at++) {
						located[at] = {
							static_cast<uint32_t>(bodyIndex.at(manifolds[at].A.Id)),
							static_cast<uint32_t>(bodyIndex.at(manifolds[at].B.Id)),
						};
					}
				},
				LOCATE_GRAIN
			);
		}

		// Sorted by entity, which keeps the body array a function of the scene
		// rather than of the order rows happen to sit in their archetypes. The
		// lookup table above answers membership without changing that order.
		// Safe to reach typed, and only because of the guard at the top of this
		// function. `RegisterPhysicsComponents` registers the `scene` types
		// before its own, so a store that got past `PreparedWorldMutable` has
		// `scene.SurfaceTable` registered under its explicit name - and a store
		// that did not never reaches this line. That is the ordering the guard
		// buys, and moving this above it would reintroduce the hazard
		// `WorldResource.hpp` describes with a different type.
		const scene::SurfaceTable *surfaces = store.Resource<scene::SurfaceTable>();

		// **Serial, and it was measured rather than assumed.** Every access below
		// is a read and the rows are disjoint, so dispatching this is safe and
		// is the obvious thing to do - and on ten thousand bodies it is **twice
		// as slow**: 1375 us serial against 2977 us over twenty-three workers,
		// in `engine.physics.bench.solver`'s tallest row. Seven sparse-set
		// lookups per body each chase the entity directory into a column that is
		// not the one before it, and twenty-three threads doing that at once
		// thrash a shared last level cache rather than sharing the work.
		//
		// The manifold-index pass beside it is dispatched because it reads only
		// the retained lookup table. This is a fact about `Store::Get` and not
		// about the gather. Worth retrying if `ecs` ever grows a locate-once-read-
		// many primitive, which would remove six of the seven directory walks and
		// change the shape of the problem.
		const ecs::Store &reader = store;
		{
			ENGINE_PROFILE_CAT("physics.solve-bodies", core::ProfileCategory::Physics);
			std::vector<uint8_t> &loaded = PipelineInternals::BodyLoaded(*world);
			loaded.assign(bodies.size(), 0);
			{
				ENGINE_PROFILE_CAT("physics.solve-bodies-archetype", core::ProfileCategory::Physics);
				const size_t baseParts = store.CountMatching<
					scene::Transform,
					scene::Collider,
					scene::RigidBody,
					scene::Surface,
					scene::PhysicsProperties>();
				if (bodies.size() >= (baseParts + 1) / 2) {
					store.Each<
						const scene::Transform,
						const scene::Collider,
						const scene::RigidBody,
						const scene::Surface,
						const scene::PhysicsProperties>([&bodies, &loaded, &bodyIndex, &reader, surfaces](
															ecs::Entity entity,
															const scene::Transform &transform,
															const scene::Collider &collider,
															const scene::RigidBody &rigid,
															const scene::Surface &surface,
															const scene::PhysicsProperties &physical
														) {
						const auto found = bodyIndex.find(entity.Id);
						if (found == bodyIndex.end()) {
							return;
						}

						const size_t index = found->second;
						LoadBody(
							bodies[index],
							&transform,
							reader.Get<scene::Motion>(entity),
							&collider,
							&rigid,
							&surface,
							&physical,
							reader.Has<scene::Simulated>(entity),
							surfaces
						);
						loaded[index] = true;
					});
				}
			}

			{
				ENGINE_PROFILE_CAT("physics.solve-bodies-fallback", core::ProfileCategory::Physics);
				for (size_t index = 0; index < bodies.size(); index++) {
					if (loaded[index]) {
						continue;
					}
					SolverBody &body = bodies[index];
					const scene::Transform *transform = reader.Get<scene::Transform>(body.Owner);
					const scene::Motion *motion = reader.Get<scene::Motion>(body.Owner);
					const scene::Collider *collider = reader.Get<scene::Collider>(body.Owner);
					const scene::RigidBody *rigid = reader.Get<scene::RigidBody>(body.Owner);

					const scene::Surface *surface = reader.Get<scene::Surface>(body.Owner);
					const scene::PhysicsProperties *physical =
						reader.Get<scene::PhysicsProperties>(body.Owner);
					LoadBody(
						body,
						transform,
						motion,
						collider,
						rigid,
						surface,
						physical,
						reader.Has<scene::Simulated>(body.Owner),
						surfaces
					);
				}
			}
		}

		// **The sleeping flag is resolved on one thread and not in the walk
		// above.** `PhysicsWorld::Sleeping` is a binary search over the resting
		// list, which is a read like any other - but it is a read of the *world*
		// rather than of the store, and keeping every worker's reach limited to
		// the store is what makes the claim above short enough to check.
		{
			ENGINE_PROFILE_CAT("physics.solve-wake", core::ProfileCategory::Physics);
			for (SolverBody &body : bodies) {
				body.Asleep = body.Movable && world->Sleeping(body.Owner);
			}

			// --- wake ------------------------------------------------------------
			//
			// A sleeping body has no `scene::Motion`, so the broad phase has it in
			// the static index and only an *awake* neighbour can produce a pair
			// with it. One pass in pair order, so a stack wakes one layer per tick
			// - bounded, deterministic, and visibly a settling stack rather than a
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
		}

		// --- partition -------------------------------------------------------
		//
		// **Which rows may be solved at the same time as which others**, decided
		// once and read by every sweep. See `SolverGroup` for the argument; what
		// happens here is the three passes that turn it into offsets.
		//
		// Pass one asks which manifolds contribute rows at all and how many. It
		// has to run before anything else because the answer decides whether to
		// partition: below `PARALLEL_SOLVE_ROWS` the whole solve is one run in
		// manifold order, which is exactly what it was before groups existed.
		std::vector<uint32_t> &groupOfManifold = PipelineInternals::GroupOfManifold(*world);
		size_t rowCount = 0;
		{
			ENGINE_PROFILE_CAT("physics.solve-classify", core::ProfileCategory::Physics);
			groupOfManifold.assign(manifolds.size(), SKIPPED_MANIFOLD);

			for (size_t at = 0; at < manifolds.size(); at++) {
				const ContactManifold &manifold = manifolds[at];

				// A trigger reports and never pushes. Skipping it here rather than
				// zeroing its impulse keeps it out of the iteration entirely, so a
				// world made of triggers costs the solver nothing.
				if (manifold.Trigger) {
					continue;
				}
				if (!bodies[located[at].first].Movable && !bodies[located[at].second].Movable) {
					continue;
				}

				// Provisional: the real group is written in pass two, and until then
				// this only says "not skipped".
				groupOfManifold[at] = 0;
				rowCount += manifold.PointCount;
			}
		}

		std::vector<SolverGroup> &groups = PipelineInternals::SolverGroups(*world);
		SolverGroup &border = PipelineInternals::BorderRows(*world);
		std::vector<uint32_t> &groupStart = PipelineInternals::GroupRowStart(*world);
		groups.clear();
		border = SolverGroup{};

		bool partitioned = rowCount >= PARALLEL_SOLVE_ROWS;
		bool usingColoring = false;
		bool usingIslands = false;
		size_t chunkCount = 0;
		PipelineInternals::SolverChunkEdge(*world) = 0.0f;
		std::vector<SolverColor> &colors = PipelineInternals::SolverColors(*world);
		colors.clear();
		PipelineInternals::SolverUsesColoring(*world) = false;
		PipelineInternals::SolverUsesIslands(*world) = false;
		PipelineInternals::SolverIslandCount(*world) = 0;
		bool solverTopologyChanged = false;
		const bool persistentScheduleEligible =
			partitioned && bodies.size() <= SOLVER_ISLAND_DISCOVERY_MAXIMUM_BODIES;

		// Islands are connected through movable endpoints only. A static floor is
		// read by every task but never written, so treating it as an edge would
		// turn unrelated stacks into one serial component.
		if (persistentScheduleEligible) {
			std::vector<SolverTopologyEntry> &topology = PipelineInternals::SolverTopology(*world);
			std::vector<SolverTopologyEntry> &topologyScratch =
				PipelineInternals::SolverTopologyScratch(*world);
			topologyScratch.resize(manifolds.size());
			for (size_t at = 0; at < manifolds.size(); at++) {
				const SolverBody &first = bodies[located[at].first];
				const SolverBody &second = bodies[located[at].second];
				topologyScratch[at] = SolverTopologyEntry{
					manifolds[at].A.Id,
					manifolds[at].B.Id,
					static_cast<uint8_t>(
						(manifolds[at].Trigger ? 1U : 0U) | (first.Movable ? 2U : 0U) |
						(second.Movable ? 4U : 0U) | (manifolds[at].PointCount != 0 ? 8U : 0U)
					),
				};
			}
			solverTopologyChanged = topology != topologyScratch;
			if (solverTopologyChanged) {
				topology = topologyScratch;
				PipelineInternals::SolverColorOfManifold(*world).clear();
				PipelineInternals::SolverColorTopologyAccepted(*world) = false;
				PipelineInternals::SolverColorTopologyKnown(*world) = false;
				PipelineInternals::SolverIslandTopologyKnown(*world) = false;
			}

			std::vector<uint32_t> &islandOfManifold = PipelineInternals::SolverIslandOfManifold(*world);
			std::vector<uint32_t> &islandOfBody = PipelineInternals::SolverIslandOfBody(*world);
			const bool reusedIslands = PipelineInternals::SolverIslandTopologyKnown(*world) &&
									   islandOfManifold.size() == manifolds.size() &&
									   islandOfBody.size() == bodies.size();
			if (!reusedIslands) {
				std::vector<uint32_t> &parents = PipelineInternals::SolverComponentParents(*world);
				parents.resize(bodies.size());
				for (size_t body = 0; body < bodies.size(); body++) {
					parents[body] = static_cast<uint32_t>(body);
				}

				for (size_t at = 0; at < manifolds.size(); at++) {
					if (groupOfManifold[at] == SKIPPED_MANIFOLD || manifolds[at].PointCount == 0) {
						continue;
					}
					const uint32_t first = located[at].first;
					const uint32_t second = located[at].second;
					if (bodies[first].Movable && bodies[second].Movable) {
						JoinSolverComponents(parents, first, second);
					}
				}

				islandOfBody.assign(bodies.size(), SKIPPED_MANIFOLD);
				size_t islandCount = 0;
				for (size_t body = 0; body < bodies.size(); body++) {
					if (!bodies[body].Movable) {
						continue;
					}
					const uint32_t root = FindSolverComponent(parents, static_cast<uint32_t>(body));
					if (islandOfBody[root] == SKIPPED_MANIFOLD) {
						islandOfBody[root] = static_cast<uint32_t>(islandCount++);
					}
					islandOfBody[body] = islandOfBody[root];
				}

				islandOfManifold.assign(manifolds.size(), SKIPPED_MANIFOLD);
				for (size_t at = 0; at < manifolds.size(); at++) {
					if (groupOfManifold[at] == SKIPPED_MANIFOLD || manifolds[at].PointCount == 0) {
						continue;
					}
					const uint32_t first = located[at].first;
					const uint32_t second = located[at].second;
					const uint32_t movable = bodies[first].Movable ? first : second;
					islandOfManifold[at] = islandOfBody[movable];
				}
				PipelineInternals::SolverIslandTopologyKnown(*world) = true;
				core::Metrics::Count("physics.solve.island.topology-rebuild", 1.0);
			} else {
				core::Metrics::Count("physics.solve.island.topology-reuse", 1.0);
			}

			std::vector<uint32_t> &islandRows = PipelineInternals::SolverIslandRows(*world);
			size_t islandCount = 0;
			for (uint32_t island : islandOfManifold) {
				if (island != SKIPPED_MANIFOLD) {
					islandCount = std::max(islandCount, static_cast<size_t>(island) + 1);
				}
			}
			islandRows.assign(islandCount, 0);
			for (size_t at = 0; at < manifolds.size(); at++) {
				const uint32_t island = islandOfManifold[at];
				if (island != SKIPPED_MANIFOLD) {
					islandRows[island] += manifolds[at].PointCount;
				}
			}
			const size_t largestRows =
				islandRows.empty() ? 0 : *std::max_element(islandRows.begin(), islandRows.end());
			usingIslands = islandCount >= SOLVER_ISLAND_MINIMUM && islandCount <= SOLVER_ISLAND_MAXIMUM &&
						   largestRows * SOLVER_ISLAND_LARGEST_SHARE_DENOMINATOR <=
							   rowCount * SOLVER_ISLAND_LARGEST_SHARE_NUMERATOR;
			PipelineInternals::SolverIslandCount(*world) = islandCount;
			PipelineInternals::SolverUsesIslands(*world) = usingIslands;
			std::vector<uint32_t> &islandOrderScratch =
				PipelineInternals::SolverIslandRowOrderScratch(*world);
			islandOrderScratch.reserve(islandRows.size());
			ReportSolverIslandShape(
				islandRows,
				islandOrderScratch,
				reusedIslands,
				usingIslands,
				world->SolverIslandRetainedBytes()
			);
			if (usingIslands) {
				groupOfManifold = islandOfManifold;
				partitioned = false;
			}
		} else {
			ReportSolverIslandShape(
				{},
				PipelineInternals::SolverIslandRowOrderScratch(*world),
				false,
				false,
				world->SolverIslandRetainedBytes()
			);
		}

		// A colour is assigned to a whole manifold before its points become rows.
		// The exact key excludes a positive point-count change because it cannot
		// change conflicts, and the placement pass recomputes every row offset.
		// Zero is included because a manifold with no rows must not claim a colour.
		if (persistentScheduleEligible && rowCount >= SOLVER_COLOR_MIN_ROWS) {
			std::vector<SolverTopologyEntry> &topology = PipelineInternals::SolverTopology(*world);
			std::vector<uint32_t> &colourOfManifold = PipelineInternals::SolverColorOfManifold(*world);
			const std::vector<SolverTopologyEntry> &topologyScratch =
				PipelineInternals::SolverTopologyScratch(*world);

			bool reused = !solverTopologyChanged && PipelineInternals::SolverColorTopologyKnown(*world) &&
						  topology == topologyScratch &&
						  (PipelineInternals::SolverColorTopologyAccepted(*world)
							   ? colourOfManifold.size() == manifolds.size()
							   : colourOfManifold.empty());
			if (!reused) {
				colourOfManifold.assign(manifolds.size(), SKIPPED_MANIFOLD);
				std::vector<uint64_t> &claims = PipelineInternals::SolverColorClaims(*world);
				claims.assign(bodies.size(), 0);
				size_t colourCount = 0;
				bool exceeded = false;
				for (size_t at = 0; at < manifolds.size(); at++) {
					if (groupOfManifold[at] == SKIPPED_MANIFOLD || manifolds[at].PointCount == 0) {
						continue;
					}
					const size_t first = located[at].first;
					const size_t second = located[at].second;
					const uint64_t unavailable = (bodies[first].Movable ? claims[first] : 0) |
												 (bodies[second].Movable ? claims[second] : 0);
					size_t colour = 0;
					while (colour < SOLVER_COLOR_MAX_WAVES && (unavailable & (uint64_t{1} << colour)) != 0) {
						colour++;
					}
					if (colour == SOLVER_COLOR_MAX_WAVES) {
						exceeded = true;
						break;
					}
					colourOfManifold[at] = static_cast<uint32_t>(colour);
					if (bodies[first].Movable) {
						claims[first] |= uint64_t{1} << colour;
					}
					if (bodies[second].Movable) {
						claims[second] |= uint64_t{1} << colour;
					}
					colourCount = std::max(colourCount, colour + 1);
				}

				// A fixed barrier for one or two tiny blocks costs more than it can
				// save. More importantly, the colour path is for one large connected
				// dynamic island, not many independent stacks which only share a floor.
				// Island discovery already assigned each movable component, so reuse that
				// canonical map rather than building a second union-find forest.
				size_t blocks = 0;
				size_t dynamicBlocks = 0;
				const std::vector<uint32_t> &islandOfManifold =
					PipelineInternals::SolverIslandOfManifold(*world);
				std::vector<uint32_t> &componentBlocks = PipelineInternals::SolverComponentParents(*world);
				componentBlocks.assign(PipelineInternals::SolverIslandCount(*world), 0);
				for (size_t at = 0; at < colourOfManifold.size(); at++) {
					if (colourOfManifold[at] == SKIPPED_MANIFOLD) {
						continue;
					}
					blocks++;
					const uint32_t first = located[at].first;
					const uint32_t second = located[at].second;
					if (bodies[first].Movable && bodies[second].Movable) {
						dynamicBlocks++;
						componentBlocks[islandOfManifold[at]]++;
					}
				}
				size_t largestComponentBlocks = 0;
				if (dynamicBlocks != 0) {
					largestComponentBlocks =
						*std::max_element(componentBlocks.begin(), componentBlocks.end());
				}
				if (exceeded || colourCount < SOLVER_COLOR_MIN_WAVES ||
					blocks < colourCount * SOLVER_COLOR_MIN_BLOCKS_PER_WAVE || dynamicBlocks == 0 ||
					largestComponentBlocks * SOLVER_COLOR_MIN_COMPONENT_BLOCK_FRACTION_DENOMINATOR <
						dynamicBlocks) {
					colourOfManifold.clear();
				}
				PipelineInternals::SolverColorTopologyAccepted(*world) = !colourOfManifold.empty();
				PipelineInternals::SolverColorTopologyKnown(*world) = true;
				topology = topologyScratch;
				core::Metrics::Count("physics.solve.color.topology-rebuild", 1.0);
			} else {
				core::Metrics::Count("physics.solve.color.topology-reuse", 1.0);
			}

			if (!colourOfManifold.empty()) {
				size_t colourCount = 0;
				for (uint32_t colour : colourOfManifold) {
					if (colour != SKIPPED_MANIFOLD) {
						colourCount = std::max(colourCount, static_cast<size_t>(colour) + 1);
					}
				}
				if (colourCount != 0) {
					usingColoring = true;
					groupOfManifold = colourOfManifold;
					partitioned = false;
					colors.resize(colourCount);
					for (size_t at = 0; at < manifolds.size(); at++) {
						if (groupOfManifold[at] != SKIPPED_MANIFOLD) {
							colors[groupOfManifold[at]].RowCount += manifolds[at].PointCount;
						}
					}
				}
			}
		}

		if (usingColoring) {
			groupStart.assign(colors.size() + 1, 0);
			for (size_t colour = 0; colour < colors.size(); colour++) {
				groupStart[colour + 1] = colors[colour].RowCount;
			}
		} else if (usingIslands) {
			const std::vector<uint32_t> &islandRows = PipelineInternals::SolverIslandRows(*world);
			groupStart.assign(islandRows.size() + 1, 0);
			for (size_t island = 0; island < islandRows.size(); island++) {
				groupStart[island + 1] = islandRows[island];
			}
		} else if (partitioned) {
			ENGINE_PROFILE_CAT("physics.solve-partition", core::ProfileCategory::Physics);

			// **A point per body, and every body rather than only the movable
			// ones.** The partition is asked for a chunk by body index, so
			// leaving the immovable ones out would mean carrying a second index
			// to map between the two arrays. What they cost instead is a chunk
			// that no interior row ever lands in, which the compaction below
			// drops for free.
			std::vector<spatial::Proxy> &points = PipelineInternals::SolverPoints(*world);
			points.clear();
			points.reserve(bodies.size());
			for (const SolverBody &body : bodies) {
				points.push_back(
					spatial::Proxy{
						static_cast<uint64_t>(points.size()),
						core::AABB{body.Centre, body.Centre},
						spatial::LayerMask::All(),
					}
				);
			}

			spatial::ChunkMap &chunks = PipelineInternals::SolverChunks(*world);
			chunks.SetChunkSize(spatial::SuggestChunkSize(points, SOLVE_GROUP_TARGET));
			chunks.Rebuild(points);
			chunkCount = chunks.ChunkCount();
			PipelineInternals::SolverChunkEdge(*world) = chunks.ChunkSize();

			// Pass two: the group each manifold's rows go in, and how many rows
			// each group ends up with. `chunkCount` is the border's own slot, so
			// the offsets below come out with every interior group first and the
			// border last - which is the order the sweeps want and costs nothing
			// to arrange here.
			groupStart.assign(chunkCount + 2, 0);

			for (size_t at = 0; at < manifolds.size(); at++) {
				if (groupOfManifold[at] == SKIPPED_MANIFOLD) {
					continue;
				}

				const SolverBody &first = bodies[located[at].first];
				const SolverBody &second = bodies[located[at].second];

				// **Only a movable body claims a chunk.** An immovable one is
				// read and never written during a sweep, so it constrains
				// nothing about who may run beside whom - and asking it for a
				// chunk anyway is what would put every contact in a scene with a
				// floor into the border.
				const uint32_t firstChunk = first.Movable ? chunks.ChunkOfProxy(located[at].first)
														  : static_cast<uint32_t>(chunkCount);
				const uint32_t secondChunk = second.Movable ? chunks.ChunkOfProxy(located[at].second)
															: static_cast<uint32_t>(chunkCount);

				uint32_t group;
				if (firstChunk == secondChunk) {
					// Both in one chunk, or one movable body and one wall.
					group = firstChunk;
				} else if (!first.Movable) {
					group = secondChunk;
				} else if (!second.Movable) {
					group = firstChunk;
				} else {
					// Two movable bodies in two chunks. Nothing may run beside
					// this row without checking both, so it waits for the end.
					group = static_cast<uint32_t>(chunkCount);
				}

				groupOfManifold[at] = group;
				groupStart[static_cast<size_t>(group) + 1] += manifolds[at].PointCount;
			}
		} else {
			// One group, filled in manifold order. Every row is in it, so the
			// sweeps below walk exactly the list the solver walked before
			// v0.17, in exactly the order it walked it.
			groupStart.assign(2, 0);
			groupStart[1] = static_cast<uint32_t>(rowCount);
			for (uint32_t &group : groupOfManifold) {
				if (group != SKIPPED_MANIFOLD) {
					group = 0;
				}
			}
		}

		// The prefix sum, in place, one slot to the right of the counts - the
		// shape `spatial::HashGrid::Rebuild` uses, and for the same reason: the
		// starts are what the fill reads and reconstructing them afterwards is a
		// second chance to be wrong.
		for (size_t slot = 0; slot + 1 < groupStart.size(); slot++) {
			groupStart[slot + 1] += groupStart[slot];
		}

		// The dispatch list: the interior groups that ended up with rows, and
		// the border's run. An empty group is dropped rather than dispatched,
		// because a worker handed nothing still costs a range claim.
		if (!usingColoring) {
			const size_t interiorSlots =
				usingIslands ? PipelineInternals::SolverIslandCount(*world) : (partitioned ? chunkCount : 1);
			groups.reserve(interiorSlots);
			for (size_t slot = 0; slot < interiorSlots; slot++) {
				const uint32_t first = groupStart[slot];
				const uint32_t last = groupStart[slot + 1];
				if (last > first) {
					groups.push_back(SolverGroup{first, last - first});
				}
			}
		}

		// **Longest first, which is a scheduling decision and not an ordering
		// one.** `Jobs::For` hands out ranges in index order, so a dispatch
		// finishes when the last group *started* finishes - and a chunk holding
		// a pile has several times the rows of one holding a wall. Started last,
		// the pile is the whole critical path with every other worker already
		// idle; started first, the short groups fill in around it.
		//
		// **Free to do, because the groups are disjoint.** Reordering them
		// changes which worker takes which and changes no arithmetic: no two
		// groups write one body, so no sum inside a body depends on the order
		// the groups ran in. The order *within* a group is untouched, and that
		// is the one that does matter.
		std::sort(groups.begin(), groups.end(), [](const SolverGroup &left, const SolverGroup &right) {
			if (left.RowCount != right.RowCount) {
				return left.RowCount > right.RowCount;
			}
			return left.FirstRow < right.FirstRow;
		});
		if (partitioned) {
			border = SolverGroup{groupStart[chunkCount], groupStart[chunkCount + 1] - groupStart[chunkCount]};
		}

		if (!usingColoring) {
			ReportSolverGroupShape(groups, border, rowCount - static_cast<size_t>(border.RowCount));
		}

		// --- place -----------------------------------------------------------
		//
		// Where each manifold's rows go, handed out before a single one is
		// built.
		//
		// **This pass exists so that the pass after it can be dispatched.** A
		// cursor per group advanced as the rows are built is a shared write, so
		// the expensive half - three prepared axes and a warm-start lookup per
		// contact - would have had to stay on one thread. Handing out the slots
		// first costs a walk over the manifolds that does no arithmetic, and
		// leaves every row with an address nobody else will write to.
		//
		// **Grown and never shrunk, which is the allocation rule one step
		// further.** `std::vector::resize` value-initialises what it adds, and
		// every byte of every row is written by the pass below - so on a steady
		// scene the exact size would memset twelve megabytes a tick to produce
		// zeroes nothing reads. `rowCount` is the logical length from here on
		// and `rows.size()` is only ever the high-water mark; the two loops that
		// used to walk the whole vector take the count instead.
		std::vector<uint32_t> &cursor = PipelineInternals::GroupRowCursor(*world);
		std::vector<uint32_t> &rowStart = PipelineInternals::RowStartOfManifold(*world);
		std::vector<uint32_t> &impulseStart = PipelineInternals::ImpulseStartOfManifold(*world);
		{
			ENGINE_PROFILE_CAT("physics.solve-place", core::ProfileCategory::Physics);
			if (rows.size() < rowCount) {
				rows.resize(rowCount);
			}
			PipelineInternals::RowCount(*world) = rowCount;

			cursor.assign(groupStart.begin(), groupStart.end() - 1);
			rowStart.resize(manifolds.size());
			impulseStart.resize(manifolds.size());

			// **Two offsets per manifold, because the two arrays are ordered
			// differently and that is the whole point of one of them.** A row's slot
			// comes from its group, so the row array is grouped by chunk and is in
			// no entity order at all. The impulse cache has to come out ordered by
			// the pair, because next tick's warm start binary-searches it - so its
			// slot is a plain running total in manifold order, and the manifolds are
			// already sorted on the pair.
			//
			// Writing the cache at row indices instead is the mistake that costs
			// nothing to make and is silent: the cache is still complete, still the
			// right size, and every lookup into it misses.
			uint32_t emitted = 0;
			for (size_t at = 0; at < manifolds.size(); at++) {
				const uint32_t group = groupOfManifold[at];
				if (group == SKIPPED_MANIFOLD) {
					continue;
				}
				rowStart[at] = cursor[group];
				cursor[group] += manifolds[at].PointCount;

				impulseStart[at] = emitted;
				emitted += manifolds[at].PointCount;
			}
		}

		if (usingColoring) {
			// The prefix sums put each colour in one row range. Within that range
			// manifold order is retained, and each group is precisely one manifold
			// block so a multi-point contact is never divided between jobs.
			groups.clear();
			for (size_t colour = 0; colour < colors.size(); colour++) {
				SolverColor &wave = colors[colour];
				wave.FirstGroup = static_cast<uint32_t>(groups.size());
				wave.GroupCount = 0;
				for (size_t at = 0; at < manifolds.size(); at++) {
					if (groupOfManifold[at] != colour || manifolds[at].PointCount == 0) {
						continue;
					}
					groups.push_back(SolverGroup{rowStart[at], manifolds[at].PointCount});
					wave.GroupCount++;
				}
			}
			ReportSolverColorShape(colors, SolverColorRetainedBytes(*world));
			ReportColoredGroupShape(groups, rowCount);
			PipelineInternals::SolverUsesColoring(*world) = true;
		} else {
			ReportSolverColorShape({}, SolverColorRetainedBytes(*world));
		}

		// --- set up ----------------------------------------------------------
		//
		// **Dispatched, and it is the one pass here that may be.** Every
		// manifold reads the bodies and the impulse cache and writes only the
		// rows `rowStart` gave it, so two workers share nothing - which is the
		// same rule the sweeps below obey and is why this needs no partition of
		// its own. The bodies are still read-only at this point: the warm start
		// is what first writes one, and it runs after this.
		//
		// A grain of sixty-four rather than one. A manifold's set-up is six
		// angular responses and a binary search, which is expensive enough to
		// dispatch and far too cheap to claim a range for one at a time.
		{
			ENGINE_PROFILE_CAT("physics.solve-setup", core::ProfileCategory::Physics);
			const auto setUpManifolds = [&](size_t begin, size_t end) {
				for (size_t at = begin; at < end; at++) {
					const ContactManifold &manifold = manifolds[at];
					if (groupOfManifold[at] == SKIPPED_MANIFOLD) {
						continue;
					}

					const size_t firstIndex = located[at].first;
					const size_t secondIndex = located[at].second;

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
						const bool speculative = contact.Separation > 0.0f;

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
						row.Friction = speculative ? 0.0f : friction;
						row.Feature = contact.Feature;
						row.Speculative = speculative;

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
						// recomputed per iteration - an iteration that recomputed it
						// would keep finding a smaller closing speed and add energy
						// chasing it.
						const float closing = -ClosingSpeed(first, second, row.Along[ContactRow::NORMAL]);
						row.SpeculativeClosingSpeed =
							speculative ? closing
										: FindSpeculativeClosingSpeed(
											  PipelineInternals::ImpulseCache(*world), manifold.A, manifold.B
										  );
						// A speculative row permits exactly the speed needed to reach
						// the surface next tick. One slop of bite avoids a float-rounded
						// gap that would otherwise hold the body just outside forever.
						row.Bounce = speculative
										 ? -(contact.Separation + PENETRATION_SLOP) / delta
										 : (std::max(closing, row.SpeculativeClosingSpeed) > BOUNCE_THRESHOLD
												? restitution * std::max(closing, row.SpeculativeClosingSpeed)
												: 0.0f);

						const ContactImpulse *cached = speculative
														   ? nullptr
														   : FindImpulse(
																 PipelineInternals::ImpulseCache(*world),
																 manifold.A,
																 manifold.B,
																 contact.Feature
															 );
						if (cached != nullptr) {
							row.Along[ContactRow::NORMAL].Impulse = cached->Normal;
							row.Along[ContactRow::TANGENT].Impulse = cached->Tangent[0];
							row.Along[ContactRow::TANGENT + 1].Impulse = cached->Tangent[1];
						}

						rows[rowStart[at] + point] = row;
					}
				}
			};

			parallel::Jobs::For(manifolds.size(), SETUP_GRAIN, setUpManifolds, SETUP_GRAIN);
		}

		// --- warm start ------------------------------------------------------
		//
		// Last tick's answer applied before the first sweep. A resting stack
		// converges to the same impulses every tick, so starting from them
		// instead of from zero is most of why `SOLVER_ITERATIONS` is sixteen and
		// not forty.
		//
		// **Over the same groups as a sweep, because it has the same shape.** It
		// writes the two bodies of every row exactly as a sweep does, so the
		// runs that may not overlap are the same runs - and a body's warm start
		// is a sum of impulses that float addition makes order-dependent, which
		// is why it has to be the *same* partition rather than any partition.
		// The border rows follow on the dispatching thread, again as a sweep's
		// do.
		{
			ENGINE_PROFILE_CAT("physics.solve-warm", core::ProfileCategory::Physics);
			const auto warmStart = [&bodies, &rows](SolverGroup run) {
				const size_t last = static_cast<size_t>(run.FirstRow) + run.RowCount;
				for (size_t at = run.FirstRow; at < last; at++) {
					const ContactRow &row = rows[at];
					SolverBody &first = bodies[row.First];
					SolverBody &second = bodies[row.Second];
					for (const ContactAxis &axis : row.Along) {
						ApplyImpulse(first, second, axis, axis.Impulse);
					}
				}
			};

			if (usingColoring) {
				for (const SolverColor &wave : colors) {
					const size_t tasks =
						(wave.GroupCount + SOLVER_COLOR_BLOCKS_PER_TASK - 1) / SOLVER_COLOR_BLOCKS_PER_TASK;
					parallel::Jobs::For(
						tasks,
						1,
						[&groups, &warmStart, wave](size_t begin, size_t end) {
							for (size_t task = begin; task < end; task++) {
								const size_t first = wave.FirstGroup + task * SOLVER_COLOR_BLOCKS_PER_TASK;
								const size_t last = std::min(
									first + SOLVER_COLOR_BLOCKS_PER_TASK,
									static_cast<size_t>(wave.FirstGroup + wave.GroupCount)
								);
								for (size_t group = first; group < last; group++) {
									warmStart(groups[group]);
								}
							}
						},
						1
					);
				}
			} else if (groups.size() > 1) {
				parallel::Jobs::For(
					groups.size(),
					1,
					[&groups, &warmStart](size_t begin, size_t end) {
						for (size_t group = begin; group < end; group++) {
							warmStart(groups[group]);
						}
					},
					2
				);
			} else if (!groups.empty()) {
				warmStart(groups[0]);
			}
			if (!usingColoring) {
				warmStart(border);
			}
		}

		// --- iterate ---------------------------------------------------------
		//
		// **Serial inside a group, and the groups are what may run at once.**
		// Sequential impulse works by letting each row see the velocities the
		// rows before it left behind - that is the method, not an
		// implementation detail - so this is still that method, applied to a
		// list that has been cut into runs no two of which write the same body.
		// `Solver.hpp` names graph colouring with a fixed batch order as the
		// only shape a parallel solve may take; `SolverGroup` is where the
		// colours come from.
		//
		// **The border rows are the ones that conflict with two groups at once**,
		// so they run after every group has finished, on the thread that
		// dispatched. They are a surface-area effect and
		// `PhysicsWorld::BorderRowCount` is what says when they have stopped
		// being one in a given scene.
		//
		// **Several sweeps per handover, because a handover costs more than a
		// sweep does.** Measured on ten thousand boxes, one dispatch of the
		// groups is about 190 microseconds of waking and joining against about
		// 190 of work - so sixteen dispatches spend as long starting as solving.
		// The groups share no body the solver may write, so a worker sweeping
		// its own group has nobody to synchronise with between sweeps and can
		// simply do several; only the border rows couple two groups, and the
		// barrier that matters is the one before them.
		//
		// The cost is that news crosses a chunk face `SOLVE_SWEEPS_PER_BATCH`
		// times more slowly, which is what that constant's measurement is about.
		// The total number of sweeps every row gets is unchanged.
		//
		// **A world that did not partition is bit-for-bit unaffected**: one
		// group and no border rows means the rounds below are sixteen
		// consecutive sweeps of one list, in the order they were always done.
		//
		// **A grain of one, and a minimum of two.** A group is an expensive unit
		// - a run of contacts swept in full - so the default cutoff, which
		// assumes an index is cheap, would refuse to hand any of them over.
		static_assert(
			SOLVER_ITERATIONS % SOLVE_SWEEPS_PER_BATCH == 0,
			"the sweeps have to divide into whole rounds, or a scene silently gets fewer than "
			"SOLVER_ITERATIONS of them"
		);

		{
			ENGINE_PROFILE_CAT("physics.solve-sweeps", core::ProfileCategory::Physics);
			if (usingIslands) {
				// Island tasks own every movable body they touch, so there is no border
				// and no information to exchange between sweeps. One blocking dispatch
				// therefore performs all sixteen local sweeps without queue handovers.
				parallel::Jobs::For(
					groups.size(),
					1,
					[&bodies, &rows, &groups](size_t begin, size_t end) {
						for (size_t group = begin; group < end; group++) {
							for (size_t sweep = 0; sweep < SOLVER_ITERATIONS; sweep++) {
								SweepRows(bodies, rows, groups[group]);
							}
						}
					},
					2
				);
			} else if (usingColoring) {
				// Each Jobs call completes before the next colour begins. That barrier
				// is the graph-colouring contract: every task in one wave is disjoint,
				// while a later wave may read the impulses it left behind.
				for (size_t round = 0; round < SOLVER_ITERATIONS; round++) {
					ENGINE_PROFILE_CAT("physics.solve-round", core::ProfileCategory::Physics);
					for (const SolverColor &wave : colors) {
						ENGINE_PROFILE_CAT("physics.solve-colour", core::ProfileCategory::Physics);
						const size_t tasks = (wave.GroupCount + SOLVER_COLOR_BLOCKS_PER_TASK - 1) /
											 SOLVER_COLOR_BLOCKS_PER_TASK;
						parallel::Jobs::For(
							tasks,
							1,
							[&bodies, &rows, &groups, wave](size_t begin, size_t end) {
								for (size_t task = begin; task < end; task++) {
									const size_t first =
										wave.FirstGroup + task * SOLVER_COLOR_BLOCKS_PER_TASK;
									const size_t last = std::min(
										first + SOLVER_COLOR_BLOCKS_PER_TASK,
										static_cast<size_t>(wave.FirstGroup + wave.GroupCount)
									);
									for (size_t group = first; group < last; group++) {
										SweepRows(bodies, rows, groups[group]);
									}
								}
							},
							1
						);
					}
				}
			} else {
				for (size_t round = 0; round < SOLVER_ITERATIONS / SOLVE_SWEEPS_PER_BATCH; round++) {
					ENGINE_PROFILE_CAT("physics.solve-round", core::ProfileCategory::Physics);
					{
						// `jobs.drain` is the nested span when this dispatch reaches the
						// pool. Keeping it inside an explicit interior span separates queue
						// drain and group work from the serial border that follows.
						ENGINE_PROFILE_CAT("physics.solve-interior", core::ProfileCategory::Physics);
						if (groups.size() > 1) {
							parallel::Jobs::For(
								groups.size(),
								1,
								[&bodies, &rows, &groups](size_t begin, size_t end) {
									for (size_t group = begin; group < end; group++) {
										for (size_t sweep = 0; sweep < SOLVE_SWEEPS_PER_BATCH; sweep++) {
											SweepRows(bodies, rows, groups[group]);
										}
									}
								},
								2
							);
						} else if (!groups.empty()) {
							for (size_t sweep = 0; sweep < SOLVE_SWEEPS_PER_BATCH; sweep++) {
								SweepRows(bodies, rows, groups[0]);
							}
						}
					}
					{
						ENGINE_PROFILE_CAT("physics.solve-border", core::ProfileCategory::Physics);
						for (size_t sweep = 0; sweep < SOLVE_SWEEPS_PER_BATCH; sweep++) {
							SweepRows(bodies, rows, border);
						}
					}
				}
			}
		}

		// --- remember --------------------------------------------------------
		//
		// **Walked by manifold rather than by row, which is what turns a sort of
		// the whole cache into a sort of four entries.** The cache has to come
		// out ordered by the two entities and then the feature, because next
		// tick's warm start binary-searches it. The manifold list is already
		// sorted on the pair, so emitting manifold by manifold gets the first
		// two parts of that key for free - and a manifold's own points do not
		// arrive in feature order, since reduction keeps the four that hold the
		// face widest rather than the first four, so the only thing left to
		// order is at most four entries that are already in the cache line being
		// written.
		//
		// The row order cannot serve here: the partition groups rows by chunk,
		// so the list a sweep walks is in no useful order at all.
		//
		// **Dispatched over the same manifold ranges as the set-up pass**, and
		// for the same reason: `rowStart` gives every manifold an output slot
		// nobody else writes to, and the sort is inside one of those slots.
		{
			ENGINE_PROFILE_CAT("physics.solve-remember", core::ProfileCategory::Physics);
			size_t confirmedSpeculative = 0;
			for (size_t at = 0; at < rowCount; at++) {
				const ContactRow &row = rows[at];
				confirmedSpeculative +=
					row.Speculative && row.Along[ContactRow::NORMAL].Impulse > 0.0f ? 1 : 0;
			}
			core::Metrics::SetGauge(
				"physics.solve.speculative.confirmed", static_cast<double>(confirmedSpeculative)
			);
			std::vector<ContactImpulse> &next = PipelineInternals::ImpulseNext(*world);
			if (next.size() < rowCount) {
				next.resize(rowCount);
			}

			parallel::Jobs::For(
				manifolds.size(),
				SETUP_GRAIN,
				[&next, &rows, &bodies, &manifolds, &groupOfManifold, &rowStart, &impulseStart](
					size_t begin, size_t end
				) {
					for (size_t at = begin; at < end; at++) {
						if (groupOfManifold[at] == SKIPPED_MANIFOLD) {
							continue;
						}

						const uint32_t first = impulseStart[at];
						const uint32_t last = first + manifolds[at].PointCount;
						for (uint32_t offset = 0; offset < manifolds[at].PointCount; offset++) {
							const ContactRow &row = rows[rowStart[at] + offset];
							const float normalImpulse =
								row.Speculative ? 0.0f : row.Along[ContactRow::NORMAL].Impulse;
							next[first + offset] = ContactImpulse{
								bodies[row.First].Owner,
								bodies[row.Second].Owner,
								row.Feature,
								normalImpulse,
								{
									row.Speculative ? 0.0f : row.Along[ContactRow::TANGENT].Impulse,
									row.Speculative ? 0.0f : row.Along[ContactRow::TANGENT + 1].Impulse,
								},
								row.Speculative ? row.SpeculativeClosingSpeed : 0.0f,
							};
						}
						std::sort(next.begin() + first, next.begin() + last);
					}
				},
				SETUP_GRAIN
			);

			// The manifolds contributed their runs in order, but the runs of the
			// ones that contributed nothing were skipped - so the filled slots are
			// `[0, rowCount)` and the tail beyond it is last tick's, which the
			// binary search must not see.
			next.resize(rowCount);
			std::swap(PipelineInternals::ImpulseCache(*world), next);
		}

		// --- rest ------------------------------------------------------------
		{
			ENGINE_PROFILE_CAT("physics.solve-rest", core::ProfileCategory::Physics);
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

			for (SolverBody &body : bodies) {
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
				// Publish runs after this merge. Keep the gathered body in sync with
				// the authoritative entry so it can remove Motion without searching
				// the resting list again.
				body.Asleep = entry.Asleep;
				restingNext.push_back(entry);
			}
			carryUntil(UINT64_MAX);

			std::swap(resting, restingNext);
		}
	}
}
