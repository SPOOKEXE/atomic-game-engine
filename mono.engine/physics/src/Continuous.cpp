#include "ConvexQuery.hpp"
#include "PipelineInternals.hpp"
#include "WorldResource.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Clock.hpp>
#include <engine/physics/Continuous.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Shapes.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::physics {

	namespace {
		void DispatchGridRanges(
			void *, size_t count, spatial::HashGrid::RangeDispatcher::Body body, void *bodyContext
		) {
			parallel::Jobs::For(
				count, 1, [body, bodyContext](size_t begin, size_t end) { body(bodyContext, begin, end); }, 1
			);
		}

		const spatial::HashGrid::RangeDispatcher GRID_DISPATCHER{nullptr, &DispatchGridRanges};

		// The smallest half-extent a shape actually has, in its own axes.
		//
		// `ShapeHalfExtent` reads `Collider::Extent` according to the shape kind,
		// which is the one place that table lives - so a sphere reports its
		// radius on all three axes rather than whatever its unused components
		// were left at.
		float ThinnestHalfExtent(const scene::Collider &collider) {
			const core::Vector3 half = ShapeHalfExtent(collider.Shape, collider.Extent);
			return std::min({half.X, half.Y, half.Z});
		}
	}

	void SweepFastBodies(ecs::Store &store) {
		ENGINE_PROFILE_CAT("physics.continuous", core::ProfileCategory::Physics);

		PhysicsWorld *world = PreparedWorldMutable(store);
		if (world == nullptr) {
			return;
		}

		const float delta = PhysicsStepSeconds(store);
		if (!(delta > 0.0f)) {
			return;
		}

		const scene::CollisionShapes *baked = scene::CollisionShapesOf(store);
		std::vector<spatial::Proxy> &proxies = PipelineInternals::ContinuousProxies(*world);
		std::vector<ColliderRecord> &records = PipelineInternals::ContinuousRecords(*world);
		std::vector<PlacedCollider> &shapes = PipelineInternals::ContinuousShapes(*world);
		std::vector<float> &fractions = PipelineInternals::ContinuousFractions(*world);
		std::vector<float> &thresholds = PipelineInternals::ContinuousThresholds(*world);
		std::vector<float> &reaches = PipelineInternals::ContinuousReaches(*world);
		const size_t bodyCount =
			store.Query<const scene::Transform, const scene::Motion, const scene::Collider>()
				.With<scene::Simulated>()
				.Count();
		proxies.resize(bodyCount);
		records.clear();
		shapes.clear();
		records.reserve(bodyCount);
		shapes.reserve(bodyCount);
		fractions.assign(bodyCount, 1.0f);
		thresholds.resize(bodyCount);
		reaches.resize(bodyCount);

		size_t written = 0;
		store.Query<const scene::Transform, const scene::Motion, const scene::Collider>()
			.With<scene::Simulated>()
			.Each([&](ecs::Entity entity,
					  const scene::Transform &transform,
					  const scene::Motion &motion,
					  const scene::Collider &collider) {
				const core::CFrame from = Advanced(transform.Frame, -motion.Linear, -motion.Angular, delta);
				const collision::ConvexHull *movingHull =
					baked != nullptr && collider.Shape == scene::ShapeKind::Hull
						? baked->FindHull(collider.Geometry)
						: nullptr;
				const ShapeInstance moving{from, collider.Extent, collider.Shape, movingHull, nullptr};
				const core::AABB startBounds = ShapeReach(moving);
				const ShapeInstance ended{
					transform.Frame, collider.Extent, collider.Shape, movingHull, nullptr
				};
				const core::AABB endBounds = ShapeReach(ended);
				const core::Vector3 low = startBounds.Minimum - from.Position;
				const core::Vector3 high = startBounds.Maximum - from.Position;
				const float radius = std::max(low.Magnitude(), high.Magnitude());
				const float angularReach = motion.Angular.Magnitude() * radius * delta;
				const core::Vector3 margin{angularReach, angularReach, angularReach};
				const core::AABB envelope = startBounds.Union(endBounds);
				proxies[written] = spatial::Proxy{
					static_cast<uint64_t>(written),
					core::AABB{envelope.Minimum - margin, envelope.Maximum + margin},
					collider.Layer,
				};
				records.push_back(ColliderRecord{entity, collider.Layer, collider.Mask});
				shapes.push_back(
					PlacedCollider{moving, collider.Trigger, motion.Linear, motion.Angular, radius}
				);
				thresholds[written] = ThinnestHalfExtent(collider) * CONTINUOUS_MOTION_RATIO;
				reaches[written] = (motion.Linear.Magnitude() + motion.Angular.Magnitude() * radius) * delta;
				written++;
			});

		float widestReach = 0.0f;
		float nextReach = 0.0f;
		float narrowestThreshold = bodyCount == 0 ? 0.0f : thresholds[0];
		bool staticSweep = false;
		for (size_t body = 0; body < bodyCount; body++) {
			if (reaches[body] > widestReach) {
				nextReach = widestReach;
				widestReach = reaches[body];
			} else if (reaches[body] > nextReach) {
				nextReach = reaches[body];
			}
			narrowestThreshold = std::min(narrowestThreshold, thresholds[body]);
			staticSweep = staticSweep || reaches[body] > thresholds[body];
		}
		const bool dynamicSweep = bodyCount > 1 && widestReach + nextReach > narrowestThreshold;
		const std::vector<ColliderRecord> &staticRecords = PipelineInternals::StaticRecords(*world);
		const std::vector<PlacedCollider> &staticShapes = PipelineInternals::StaticShapes(*world);
		staticSweep = staticSweep && !staticRecords.empty() && staticShapes.size() == staticRecords.size();
		if (!staticSweep && !dynamicSweep) {
			return;
		}

		spatial::HashGrid &movingIndex = PipelineInternals::ContinuousIndex(*world);
		if (dynamicSweep) {
			movingIndex.RebuildParallel(proxies, GRID_DISPATCHER);
		}
		const spatial::HashGrid &staticIndex = PipelineInternals::StaticIndex(*world);
		std::vector<uint64_t> &candidates = PipelineInternals::CandidateBuffer(*world);
		const size_t widest = std::max(bodyCount, staticRecords.size());
		if (candidates.size() < widest) {
			candidates.resize(widest);
		}

		std::vector<ContinuousImpactEvent> &events = PipelineInternals::ContinuousEvents(*world);
		events.clear();
		events.reserve(bodyCount);
		uint64_t advanceFallbacks = 0;
		uint64_t resweeps = 0;

		const auto addEvent = [&](const ConvexSweep &hit,
								  float fraction,
								  size_t first,
								  size_t second,
								  bool dynamic,
								  bool reswept = false) {
			advanceFallbacks += hit.ConservativeFallback ? 1 : 0;
			if (!hit.Hit) {
				return;
			}
			const float bite = hit.ConservativeFallback
								   ? 0.0f
								   : CONTINUOUS_BITE / std::max(hit.ClosingSpeed * delta, CONTINUOUS_BITE);
			events.push_back(
				ContinuousImpactEvent{
					fraction,
					bite,
					static_cast<uint32_t>(first),
					static_cast<uint32_t>(second),
					dynamic,
					reswept,
				}
			);
		};

		for (size_t first = 0; first < bodyCount; first++) {
			const PlacedCollider &moving = shapes[first];
			if (staticSweep && reaches[first] > thresholds[first]) {
				const spatial::QueryResult found =
					spatial::OverlapBox(staticIndex, proxies[first].Bounds, records[first].Mask, candidates);
				for (size_t at = 0; at < found.Written; at++) {
					const size_t fixedIndex = static_cast<size_t>(candidates[at]);
					const PlacedCollider &fixed = staticShapes[fixedIndex];
					if (fixed.Trigger || !PairAdmitted(records[first], staticRecords[fixedIndex])) {
						continue;
					}
					const ConvexSweep hit = SweepConvexMotion(
						moving.Shape,
						moving.LinearVelocity,
						moving.AngularVelocity,
						fixed.Shape,
						core::Vector3::Zero,
						core::Vector3::Zero,
						delta
					);
					addEvent(hit, hit.Fraction, first, fixedIndex, false);
				}
			}

			if (!dynamicSweep || !(reaches[first] > 0.0f)) {
				continue;
			}
			const spatial::QueryResult found =
				spatial::OverlapBox(movingIndex, proxies[first].Bounds, records[first].Mask, candidates);
			for (size_t at = 0; at < found.Written; at++) {
				const size_t second = static_cast<size_t>(candidates[at]);
				if (second <= first || shapes[first].Trigger || shapes[second].Trigger ||
					!PairAdmitted(records[first], records[second]) ||
					world->RigidlyConnected(records[first].Owner, records[second].Owner)) {
					continue;
				}
				const PlacedCollider &other = shapes[second];
				const float relativeReach = ((moving.LinearVelocity - other.LinearVelocity).Magnitude() +
											 moving.AngularVelocity.Magnitude() * moving.MaximumRadius +
											 other.AngularVelocity.Magnitude() * other.MaximumRadius) *
											delta;
				if (relativeReach <= std::min(thresholds[first], thresholds[second])) {
					continue;
				}
				const ConvexSweep hit = SweepConvexMotion(
					moving.Shape,
					moving.LinearVelocity,
					moving.AngularVelocity,
					other.Shape,
					other.LinearVelocity,
					other.AngularVelocity,
					delta
				);
				addEvent(hit, hit.Fraction, first, second, true);
			}
		}

		const auto beforeEvent = [&](const ContinuousImpactEvent &first,
									 const ContinuousImpactEvent &second) {
			if (first.Fraction != second.Fraction) {
				return first.Fraction < second.Fraction;
			}
			const uint64_t firstA = records[first.First].Owner.Id;
			const uint64_t firstB =
				first.Dynamic ? records[first.Second].Owner.Id : staticRecords[first.Second].Owner.Id;
			const uint64_t secondA = records[second.First].Owner.Id;
			const uint64_t secondB =
				second.Dynamic ? records[second.Second].Owner.Id : staticRecords[second.Second].Owner.Id;
			return std::pair{std::min(firstA, firstB), std::max(firstA, firstB)} <
				   std::pair{std::min(secondA, secondB), std::max(secondA, secondB)};
		};
		const auto laterEvent = [&](const ContinuousImpactEvent &first, const ContinuousImpactEvent &second) {
			return beforeEvent(second, first);
		};
		const size_t initialEvents = events.size();
		std::make_heap(events.begin(), events.end(), laterEvent);
		const auto scheduleAgainstFrozen = [&](size_t fixedIndex) {
			if (!dynamicSweep || fractions[fixedIndex] >= 1.0f) {
				return;
			}
			const spatial::QueryResult found = spatial::OverlapBox(
				movingIndex, proxies[fixedIndex].Bounds, records[fixedIndex].Mask, candidates
			);
			for (size_t at = 0; at < found.Written; at++) {
				const size_t movingIndexValue = static_cast<size_t>(candidates[at]);
				if (movingIndexValue == fixedIndex || fractions[movingIndexValue] < 1.0f ||
					!(reaches[movingIndexValue] > 0.0f) || shapes[movingIndexValue].Trigger ||
					!PairAdmitted(records[movingIndexValue], records[fixedIndex]) ||
					world->RigidlyConnected(records[movingIndexValue].Owner, records[fixedIndex].Owner)) {
					continue;
				}

				const float start = fractions[fixedIndex];
				const PlacedCollider &moving = shapes[movingIndexValue];
				const PlacedCollider &fixed = shapes[fixedIndex];
				const ShapeInstance movingAt{
					Advanced(
						moving.Shape.Frame, moving.LinearVelocity, moving.AngularVelocity, delta * start
					),
					moving.Shape.Extent,
					moving.Shape.Shape,
					moving.Shape.Hull,
					moving.Shape.Mesh,
				};
				const ShapeInstance fixedAt{
					Advanced(fixed.Shape.Frame, fixed.LinearVelocity, fixed.AngularVelocity, delta * start),
					fixed.Shape.Extent,
					fixed.Shape.Shape,
					fixed.Shape.Hull,
					fixed.Shape.Mesh,
				};
				const float remaining = delta * (1.0f - start);
				const ConvexSweep hit = SweepConvexMotion(
					movingAt,
					moving.LinearVelocity,
					moving.AngularVelocity,
					fixedAt,
					core::Vector3::Zero,
					core::Vector3::Zero,
					remaining
				);
				resweeps++;
				const size_t oldSize = events.size();
				addEvent(
					hit, start + hit.Fraction * (1.0f - start), movingIndexValue, fixedIndex, true, true
				);
				if (events.size() != oldSize) {
					std::push_heap(events.begin(), events.end(), laterEvent);
				}
			}
		};

		while (!events.empty()) {
			std::pop_heap(events.begin(), events.end(), laterEvent);
			const ContinuousImpactEvent event = events.back();
			events.pop_back();

			const size_t first = event.First;
			if (!event.Dynamic) {
				if (fractions[first] >= 1.0f) {
					fractions[first] = std::clamp(event.Fraction + event.BiteFraction, 0.0f, 1.0f);
					scheduleAgainstFrozen(first);
				}
				continue;
			}

			const size_t second = event.Second;
			const bool firstMoving = fractions[first] >= 1.0f;
			const bool secondMoving = fractions[second] >= 1.0f;
			if (!firstMoving && !secondMoving) {
				continue;
			}
			if (firstMoving && secondMoving) {
				const float stop = std::clamp(event.Fraction + event.BiteFraction, 0.0f, 1.0f);
				fractions[first] = stop;
				fractions[second] = stop;
				scheduleAgainstFrozen(first);
				scheduleAgainstFrozen(second);
				continue;
			}

			const size_t movingIndexValue = firstMoving ? first : second;
			const size_t fixedIndex = firstMoving ? second : first;
			if (event.Reswept) {
				fractions[movingIndexValue] = std::clamp(event.Fraction + event.BiteFraction, 0.0f, 1.0f);
				scheduleAgainstFrozen(movingIndexValue);
				continue;
			}
			const float start = fractions[fixedIndex];
			const PlacedCollider &moving = shapes[movingIndexValue];
			const PlacedCollider &fixed = shapes[fixedIndex];
			const ShapeInstance movingAt{
				Advanced(moving.Shape.Frame, moving.LinearVelocity, moving.AngularVelocity, delta * start),
				moving.Shape.Extent,
				moving.Shape.Shape,
				moving.Shape.Hull,
				moving.Shape.Mesh,
			};
			const ShapeInstance fixedAt{
				Advanced(fixed.Shape.Frame, fixed.LinearVelocity, fixed.AngularVelocity, delta * start),
				fixed.Shape.Extent,
				fixed.Shape.Shape,
				fixed.Shape.Hull,
				fixed.Shape.Mesh,
			};
			const float remaining = delta * (1.0f - start);
			const ConvexSweep hit = SweepConvexMotion(
				movingAt,
				moving.LinearVelocity,
				moving.AngularVelocity,
				fixedAt,
				core::Vector3::Zero,
				core::Vector3::Zero,
				remaining
			);
			const size_t oldSize = events.size();
			addEvent(hit, start + hit.Fraction * (1.0f - start), movingIndexValue, fixedIndex, true, true);
			if (events.size() != oldSize) {
				std::push_heap(events.begin(), events.end(), laterEvent);
			}
		}
		uint64_t swept = 0;
		size_t body = 0;
		store.Query<scene::Transform, const scene::Motion, const scene::Collider>()
			.With<scene::Simulated>()
			.Each([&](ecs::Entity entity,
					  scene::Transform &transform,
					  const scene::Motion &motion,
					  const scene::Collider &) {
				if (body < records.size() && records[body].Owner == entity && fractions[body] < 1.0f) {
					transform.Frame = Advanced(
						shapes[body].Shape.Frame, motion.Linear, motion.Angular, delta * fractions[body]
					);
					swept++;
				}
				body++;
			});

		PipelineInternals::SweptBodyCount(*world) += swept;
		core::Metrics::Count("physics.continuous.advance-fallback", static_cast<double>(advanceFallbacks));
		core::Metrics::Count("physics.continuous.initial-events", static_cast<double>(initialEvents));
		core::Metrics::Count("physics.continuous.resweeps", static_cast<double>(resweeps));
		core::Metrics::Count("physics.continuous.swept-bodies", static_cast<double>(swept));
	}
}
