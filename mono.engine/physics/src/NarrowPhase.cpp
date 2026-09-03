#include "ContactPairs.hpp"
#include "FaceManifold.hpp"
#include "PipelineInternals.hpp"
#include "ShapeSupport.hpp"
#include "WorldResource.hpp"

#include <engine/core/FrameGraph.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Contacts.hpp>
#include <engine/physics/NarrowPhase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/scene/Components.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::physics {
	namespace {
		// A cached point is exact only while both feature anchors remain close.
		// One millimetre is the pair algorithms' existing separation tolerance;
		// a tenth of that distance sideways keeps a warm-start feature from
		// following a body across the face that originally produced it. Relative
		// normal motion is held to roughly eight hundredths of a degree for the
		// same reason.
		constexpr float PERSISTENT_NORMAL_DOT = 0.999999f;
		constexpr float PERSISTENT_TANGENT_DISTANCE = CONTACT_TOLERANCE * 0.1f;

		bool Before(const PersistentContactManifold &cached, const CandidatePair &pair) {
			return cached.A.Id < pair.A.Id || (cached.A.Id == pair.A.Id && cached.B.Id < pair.B.Id);
		}

		bool SamePair(const PersistentContactManifold &cached, const CandidatePair &pair) {
			return cached.A == pair.A && cached.B == pair.B;
		}

		bool Before(const PersistentContactCandidate &cached, const CandidatePair &pair) {
			return cached.A.Id < pair.A.Id || (cached.A.Id == pair.A.Id && cached.B.Id < pair.B.Id);
		}

		bool SamePair(const PersistentContactCandidate &cached, const CandidatePair &pair) {
			return cached.A == pair.A && cached.B == pair.B;
		}

		uint64_t MixSignature(uint64_t signature, uint32_t value) {
			return (signature ^ value) * 1099511628211ULL;
		}

		uint64_t GeometrySignature(const PlacedCollider &first, const PlacedCollider &second) {
			uint64_t signature = 1469598103934665603ULL;
			const auto mixVector = [&signature](const core::Vector3 &value) {
				signature = MixSignature(signature, std::bit_cast<uint32_t>(value.X));
				signature = MixSignature(signature, std::bit_cast<uint32_t>(value.Y));
				signature = MixSignature(signature, std::bit_cast<uint32_t>(value.Z));
			};
			// Admission depends on relative geometry. Moving a whole assembly must
			// not make every contact look new when neither local anchor changed.
			mixVector(first.Shape.Frame.PointToObjectSpace(second.Shape.Frame.Position));
			mixVector(first.Shape.Frame.VectorToObjectSpace(second.Shape.Axis[0]));
			mixVector(first.Shape.Frame.VectorToObjectSpace(second.Shape.Axis[1]));
			mixVector(first.Shape.Frame.VectorToObjectSpace(second.Shape.Axis[2]));
			mixVector(first.Shape.Extent);
			mixVector(second.Shape.Extent);
			signature = MixSignature(signature, static_cast<uint32_t>(first.Shape.Shape));
			signature = MixSignature(signature, static_cast<uint32_t>(second.Shape.Shape));
			return signature;
		}

		bool Cacheable(const PlacedCollider &first, const PlacedCollider &second) {
			if (first.Trigger || second.Trigger) {
				return false;
			}
			const auto analytic = [](scene::ShapeKind shape) {
				switch (shape) {
				case scene::ShapeKind::Box:
				case scene::ShapeKind::Sphere:
				case scene::ShapeKind::Cylinder:
				case scene::ShapeKind::Capsule:
					return true;
				case scene::ShapeKind::Hull:
				case scene::ShapeKind::Mesh:
					return false;
				}
				return false;
			};
			if (!analytic(first.Shape.Shape) || !analytic(second.Shape.Shape)) {
				return false;
			}

			// Sphere pairs take 14 to 52 ns in the release benchmark. Refreshing
			// two local anchors costs more than recomputing those contacts and also
			// retains a much larger row. Capsules keep the cache because they use
			// the general convex path rather than the cheap analytic sphere paths.
			const bool hasSphere = first.Shape.Shape == scene::ShapeKind::Sphere ||
								   second.Shape.Shape == scene::ShapeKind::Sphere;
			const bool hasCapsule = first.Shape.Shape == scene::ShapeKind::Capsule ||
									second.Shape.Shape == scene::ShapeKind::Capsule;
			return !hasSphere || hasCapsule;
		}

		bool SameGeometry(
			const PersistentContactManifold &cached, const PlacedCollider &first, const PlacedCollider &second
		) {
			return cached.FirstShape == first.Shape.Shape && cached.SecondShape == second.Shape.Shape &&
				   cached.FirstExtent == first.Shape.Extent && cached.SecondExtent == second.Shape.Extent;
		}

		PersistentContactManifold Retain(
			const CandidatePair &pair,
			const PlacedCollider &first,
			const PlacedCollider &second,
			const ContactSolution &solution
		) {
			PersistentContactManifold cached;
			cached.A = pair.A;
			cached.B = pair.B;
			cached.LocalNormalFirst = first.Shape.Frame.VectorToObjectSpace(solution.Normal);
			cached.LocalNormalSecond = second.Shape.Frame.VectorToObjectSpace(solution.Normal);
			cached.FirstExtent = first.Shape.Extent;
			cached.SecondExtent = second.Shape.Extent;
			cached.FirstShape = first.Shape.Shape;
			cached.SecondShape = second.Shape.Shape;
			cached.PointCount = solution.PointCount;
			for (size_t index = 0; index < solution.PointCount; index++) {
				const core::Vector3 onSecond = solution.Positions[index];
				const core::Vector3 onFirst = onSecond + solution.Normal * solution.Penetrations[index];
				cached.Points[index] = PersistentContactPoint{
					first.Shape.Frame.PointToObjectSpace(onFirst),
					second.Shape.Frame.PointToObjectSpace(onSecond),
					solution.Features[index],
				};
			}
			return cached;
		}

		bool Refresh(
			const PersistentContactManifold &cached,
			const CandidatePair &pair,
			const PlacedCollider &first,
			const PlacedCollider &second,
			ContactManifold &manifold
		) {
			if (!SamePair(cached, pair) || !SameGeometry(cached, first, second) || cached.PointCount == 0) {
				return false;
			}

			const core::Vector3 normal = first.Shape.Frame.VectorToWorldSpace(cached.LocalNormalFirst).Unit();
			const core::Vector3 secondNormal =
				second.Shape.Frame.VectorToWorldSpace(cached.LocalNormalSecond).Unit();
			if (normal.MagnitudeSquared() == 0.0f || normal.Dot(secondNormal) < PERSISTENT_NORMAL_DOT) {
				return false;
			}

			manifold.A = pair.A;
			manifold.B = pair.B;
			manifold.Normal = normal;
			manifold.PointCount = cached.PointCount;
			manifold.Trigger = false;
			for (size_t index = 0; index < cached.PointCount; index++) {
				const PersistentContactPoint &point = cached.Points[index];
				const core::Vector3 onFirst = first.Shape.Frame.PointToWorldSpace(point.LocalFirst);
				const core::Vector3 onSecond = second.Shape.Frame.PointToWorldSpace(point.LocalSecond);
				const core::Vector3 delta = onSecond - onFirst;
				const float separation = delta.Dot(normal);
				const core::Vector3 tangent = delta - normal * separation;
				if (separation > CONTACT_TOLERANCE ||
					tangent.MagnitudeSquared() > PERSISTENT_TANGENT_DISTANCE * PERSISTENT_TANGENT_DISTANCE) {
					return false;
				}
				manifold.Points[index] = ContactPoint{
					onSecond,
					std::max(-separation, 0.0f),
					point.Feature,
				};
			}
			return true;
		}

		void ReportPersistentStats(
			size_t reused,
			size_t rebuilt,
			size_t rejected,
			size_t cached,
			size_t pending,
			size_t retainedBytes
		) {
			core::Metrics::SetGauge("physics.narrowphase.pcm.reused", static_cast<double>(reused));
			core::Metrics::SetGauge("physics.narrowphase.pcm.rebuilt", static_cast<double>(rebuilt));
			core::Metrics::SetGauge("physics.narrowphase.pcm.rejected", static_cast<double>(rejected));
			core::Metrics::SetGauge("physics.narrowphase.pcm.cached", static_cast<double>(cached));
			core::Metrics::SetGauge("physics.narrowphase.pcm.pending", static_cast<double>(pending));
			core::Metrics::SetGauge(
				"physics.narrowphase.pcm.reuse-ratio",
				reused + rebuilt == 0 ? 0.0
									  : static_cast<double>(reused) / static_cast<double>(reused + rebuilt)
			);
			core::Metrics::SetGauge(
				"physics.narrowphase.pcm.retained-bytes", static_cast<double>(retainedBytes)
			);
		}

		size_t PersistentRetainedBytes(PhysicsWorld &world) {
			size_t bytes =
				PipelineInternals::PersistentManifolds(world).capacity() * sizeof(PersistentContactManifold) +
				PipelineInternals::PersistentNext(world).capacity() * sizeof(PersistentContactManifold);
			const auto &batches = PipelineInternals::PersistentManifoldBatches(world);
			bytes += batches.capacity() * sizeof(std::vector<PersistentContactManifold>);
			for (const auto &batch : batches) {
				bytes += batch.capacity() * sizeof(PersistentContactManifold);
			}
			bytes += PipelineInternals::PersistentCandidates(world).capacity() *
						 sizeof(PersistentContactCandidate) +
					 PipelineInternals::PersistentCandidateNext(world).capacity() *
						 sizeof(PersistentContactCandidate);
			const auto &candidateBatches = PipelineInternals::PersistentCandidateBatches(world);
			bytes += candidateBatches.capacity() * sizeof(std::vector<PersistentContactCandidate>);
			for (const auto &batch : candidateBatches) {
				bytes += batch.capacity() * sizeof(PersistentContactCandidate);
			}
			bytes += PipelineInternals::PersistentManifoldBatchStatsOf(world).capacity() *
					 sizeof(PersistentContactBatchStats);
			return bytes;
		}
	}

	void NarrowPhase(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.narrowphase", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		// Cleared, never freed - the allocation table names these two lists
		// beside the pair list, and this is the step that owns clearing them.
		// The event list is cleared here rather than in `Publish` so that a
		// world whose narrow phase ran and whose solver did not cannot hand a
		// reader last tick's events as though they were this tick's.
		//
		// **The manifolds belong to a step and the events belong to a tick**,
		// which only differ on a world stepping physics more than once per
		// tick. A reader asks what touched this tick, and a touch that began on
		// the second step of one is a touch that happened - clearing per step
		// would drop every contact that began and ended inside a tick, and the
		// faster the world was configured the more of them it would drop.
		std::vector<ContactManifold> &manifolds = PipelineInternals::Manifolds(*world);
		manifolds.clear();

		if (FirstPhysicsStepOfTick(store)) {
			PipelineInternals::Events(*world).clear();
		}

		// **Dispatched since v0.17, and the note it replaces said the opposite
		// twice.** A pair function is pure - it reads two placed shapes and
		// writes one manifold - so splitting the pair list across workers gives
		// bit-for-bit the same answer, and the obvious thing to do is dispatch
		// it. Two attempts did, and both lost:
		//
		// - the first wrote a slot per *candidate*, which cost a pass over
		//   several megabytes;
		// - the second wrote a slot only for the pairs that touch, and finished
		//   in 4.07 ms of wall against 4.19 ms serial - having spent **89.5 ms
		//   of worker time** doing it. Twenty-four workers each did twenty-one
		//   times the per-pair work one thread does.
		//
		// The cause was isolated rather than guessed: hoisting the two
		// `Store::Get` calls out of the dispatched body, and changing nothing
		// else, dropped worker time to 3.67 ms and wall time to 349 us.
		// `physics/AGENTS.md` carries the whole finding.
		//
		// **So this step no longer looks anything up.** `SyncBroadphase` reads
		// every collider's `Transform` and `Collider` to build the proxies, and
		// now keeps the placed shape beside each one; `BroadPhase` carries the
		// proxy indices alongside the entities it emits. A pair is two array
		// subscripts.
		const std::span<const CandidatePair> pairs = world->Pairs();
		const std::span<const CandidateSource> sources = PipelineInternals::PairSources(*world);
		if (pairs.empty() || sources.size() != pairs.size()) {
			PipelineInternals::PersistentManifolds(*world).clear();
			PipelineInternals::PersistentNext(*world).clear();
			PipelineInternals::PersistentCandidates(*world).clear();
			PipelineInternals::PersistentCandidateNext(*world).clear();
			ReportPersistentStats(0, 0, 0, 0, 0, PersistentRetainedBytes(*world));
			return;
		}

		const std::vector<PlacedCollider> &dynamicShapes = PipelineInternals::DynamicShapes(*world);
		const std::vector<PlacedCollider> &staticShapes = PipelineInternals::StaticShapes(*world);

		std::vector<std::vector<ContactManifold>> &batches = PipelineInternals::ManifoldBatches(*world);
		const std::vector<PersistentContactManifold> &persistent =
			PipelineInternals::PersistentManifolds(*world);
		const std::vector<PersistentContactCandidate> &candidates =
			PipelineInternals::PersistentCandidates(*world);
		std::vector<std::vector<PersistentContactManifold>> &persistentBatches =
			PipelineInternals::PersistentManifoldBatches(*world);
		std::vector<std::vector<PersistentContactCandidate>> &candidateBatches =
			PipelineInternals::PersistentCandidateBatches(*world);
		std::vector<PersistentContactBatchStats> &batchStats =
			PipelineInternals::PersistentManifoldBatchStatsOf(*world);
		const size_t batchCount = (pairs.size() + NARROW_GRAIN - 1) / NARROW_GRAIN;
		if (batches.size() < batchCount) {
			batches.resize(batchCount);
		}
		if (persistentBatches.size() < batchCount) {
			persistentBatches.resize(batchCount);
		}
		if (candidateBatches.size() < batchCount) {
			candidateBatches.resize(batchCount);
		}
		if (batchStats.size() < batchCount) {
			batchStats.resize(batchCount);
		}

		{
			ENGINE_PROFILE_CAT("physics.contact-measure", core::ProfileCategory::Physics);
			parallel::Jobs::For(
				pairs.size(),
				NARROW_GRAIN,
				[pairs,
				 sources,
				 &dynamicShapes,
				 &staticShapes,
				 &persistent,
				 &candidates,
				 &batches,
				 &persistentBatches,
				 &candidateBatches,
				 &batchStats](size_t begin, size_t end) {
					std::vector<ContactManifold> &output = batches[begin / NARROW_GRAIN];
					std::vector<PersistentContactManifold> &cacheOutput =
						persistentBatches[begin / NARROW_GRAIN];
					std::vector<PersistentContactCandidate> &candidateOutput =
						candidateBatches[begin / NARROW_GRAIN];
					PersistentContactBatchStats &stats = batchStats[begin / NARROW_GRAIN];
					output.clear();
					cacheOutput.clear();
					candidateOutput.clear();
					stats = {};
					output.reserve(end - begin);

					size_t cacheAt = 0;
					if (!persistent.empty()) {
						cacheAt = static_cast<size_t>(
							std::lower_bound(
								persistent.begin(),
								persistent.end(),
								pairs[begin],
								[](const PersistentContactManifold &cached, const CandidatePair &pair) {
									return Before(cached, pair);
								}
							) -
							persistent.begin()
						);
					}
					size_t candidateAt = 0;
					if (!candidates.empty()) {
						candidateAt = static_cast<size_t>(
							std::lower_bound(
								candidates.begin(),
								candidates.end(),
								pairs[begin],
								[](const PersistentContactCandidate &cached, const CandidatePair &pair) {
									return Before(cached, pair);
								}
							) -
							candidates.begin()
						);
					}
					for (size_t at = begin; at < end; at++) {
						const CandidateSource &source = sources[at];

						// **Bounds-checked, because the two index arrays are built
						// one step apart.** A row destroyed between the sync and the
						// broad phase leaves an index into an array that has since
						// been rebuilt shorter - deferred structural changes land at
						// the end of an `Each`, not at the end of the phase, so this
						// is an ordinary outcome and not a diagnostic.
						const std::vector<PlacedCollider> &firstTable =
							source.FirstIsStatic() ? staticShapes : dynamicShapes;
						const std::vector<PlacedCollider> &secondTable =
							source.SecondIsStatic() ? staticShapes : dynamicShapes;

						const size_t firstAt = source.FirstIndex();
						const size_t secondAt = source.SecondIndex();
						if (firstAt >= firstTable.size() || secondAt >= secondTable.size()) {
							continue;
						}

						const PlacedCollider &first = firstTable[firstAt];
						const PlacedCollider &second = secondTable[secondAt];
						const CandidatePair &pair = pairs[at];
						while (cacheAt < persistent.size() && Before(persistent[cacheAt], pair)) {
							cacheAt++;
						}
						while (candidateAt < candidates.size() && Before(candidates[candidateAt], pair)) {
							candidateAt++;
						}

						const bool cacheable = Cacheable(first, second);
						const bool found = cacheAt < persistent.size() && SamePair(persistent[cacheAt], pair);
						if (cacheable && found) {
							ContactManifold manifold;
							if (Refresh(persistent[cacheAt], pair, first, second, manifold)) {
								output.push_back(manifold);
								cacheOutput.push_back(persistent[cacheAt]);
								stats.Reused++;
								continue;
							}
							stats.Rejected++;
						}

						const ContactSolution solution = ContactBetween(first.Shape, second.Shape);
						if (!solution.Touching) {
							continue;
						}

						ContactManifold manifold;
						manifold.A = pair.A;
						manifold.B = pair.B;
						manifold.Normal = solution.Normal;
						manifold.PointCount = solution.PointCount;

						// Either side being a trigger makes the whole manifold one.
						// There is no half-solved contact: a trigger reports and
						// never pushes, and a pair where one side pushed and the
						// other did not would apply an impulse to one body out of
						// two.
						manifold.Trigger = first.Trigger || second.Trigger;

						for (size_t index = 0; index < solution.PointCount; index++) {
							manifold.Points[index] = ContactPoint{
								solution.Positions[index],
								solution.Penetrations[index],
								solution.Features[index],
							};
						}

						output.push_back(manifold);
						if (cacheable) {
							const uint64_t signature = GeometrySignature(first, second);
							const bool candidateFound = candidateAt < candidates.size() &&
														SamePair(candidates[candidateAt], pair) &&
														candidates[candidateAt].Signature == signature;
							if (!found && candidateFound) {
								cacheOutput.push_back(Retain(pair, first, second, solution));
								stats.Rebuilt++;
							} else {
								candidateOutput.push_back(
									PersistentContactCandidate{pair.A, pair.B, signature}
								);
							}
						}
					}
				},
				NARROW_GRAIN
			);
		}

		// **In pair-range order, on one thread.** A worker appending to one
		// manifold list would make solver order depend on when workers finished.
		// Each batch covers one fixed input range, so concatenation carries the
		// pair order through while skipping candidates that did not touch.
		{
			ENGINE_PROFILE_CAT("physics.contact-compact", core::ProfileCategory::Physics);
			manifolds.reserve(pairs.size());
			std::vector<PersistentContactManifold> &next = PipelineInternals::PersistentNext(*world);
			next.clear();
			next.reserve(persistent.size());
			std::vector<PersistentContactCandidate> &candidateNext =
				PipelineInternals::PersistentCandidateNext(*world);
			candidateNext.clear();
			candidateNext.reserve(candidates.size());
			size_t reused = 0;
			size_t rebuilt = 0;
			size_t rejected = 0;
			for (size_t batch = 0; batch < batchCount; batch++) {
				const std::vector<ContactManifold> &output = batches[batch];
				manifolds.insert(manifolds.end(), output.begin(), output.end());
				const std::vector<PersistentContactManifold> &cacheOutput = persistentBatches[batch];
				next.insert(next.end(), cacheOutput.begin(), cacheOutput.end());
				const std::vector<PersistentContactCandidate> &candidateOutput = candidateBatches[batch];
				candidateNext.insert(candidateNext.end(), candidateOutput.begin(), candidateOutput.end());
				reused += batchStats[batch].Reused;
				rebuilt += batchStats[batch].Rebuilt;
				rejected += batchStats[batch].Rejected;
			}
			PipelineInternals::PersistentManifolds(*world).swap(next);
			PipelineInternals::PersistentCandidates(*world).swap(candidateNext);
			ReportPersistentStats(
				reused,
				rebuilt,
				rejected,
				PipelineInternals::PersistentManifolds(*world).size(),
				PipelineInternals::PersistentCandidates(*world).size(),
				PersistentRetainedBytes(*world)
			);
		}
	}
}
