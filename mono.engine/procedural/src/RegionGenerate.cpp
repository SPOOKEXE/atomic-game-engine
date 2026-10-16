#include "Encoding.hpp"
#include "RegionInternal.hpp"
#include "Stages.hpp"

#include <engine/procedural/Random.hpp>
#include <engine/procedural/Region.hpp>

#include <algorithm>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace engine::procedural {

	namespace {
		struct Candidate {
			std::string Id{};
			std::string SourceName{};
			Point Position{};
			uint64_t Priority = 0;
			bool Active = false;
		};

		bool Spend(uint32_t &used, uint32_t maximum, uint32_t amount = 1) {
			if (amount > maximum || used > maximum - amount) return false;
			used += amount;
			return true;
		}

		bool PollControl(
			GenerationControl *control,
			const RegionResult &result,
			uint32_t completedCandidates,
			Diagnostic &diagnostic
		) {
			if (control == nullptr) return true;
			control->CandidateCells.store(completedCandidates, std::memory_order_release);
			control->WorkUnits.store(result.Facts.WorkUnits, std::memory_order_release);
			control->SpatialTests.store(result.Facts.SpatialTests, std::memory_order_release);
			control->Placements.store(
				static_cast<uint32_t>(result.Placements.size()), std::memory_order_release
			);
			control->Roads.store(static_cast<uint32_t>(result.Roads.size()), std::memory_order_release);
			if (!control->StopRequested.load(std::memory_order_acquire)) return true;
			detail::Refuse(diagnostic, Status::Canceled, "region.cancel");
			return false;
		}

		int64_t Square(int64_t value) {
			return value * value;
		}

		int64_t DistanceSquared(Point left, Point right) {
			return Square(left.X - right.X) + Square(left.Y - right.Y);
		}

		int64_t SegmentDistanceSquared(Point point, Point start, Point end) {
			const int64_t dx = end.X - start.X;
			const int64_t dy = end.Y - start.Y;
			const int64_t length = Square(dx) + Square(dy);
			if (length == 0) return DistanceSquared(point, start);
			const int64_t projection = (point.X - start.X) * dx + (point.Y - start.Y) * dy;
			if (projection <= 0) return DistanceSquared(point, start);
			if (projection >= length) return DistanceSquared(point, end);
			const Point nearest{start.X + dx * projection / length, start.Y + dy * projection / length};
			return DistanceSquared(point, nearest);
		}

		Point CurvePoint(const RoadSpline &road, int64_t step) {
			constexpr int64_t SEGMENTS = 16;
			const int64_t remaining = SEGMENTS - step;
			const auto coordinate = [&](int64_t first, int64_t control, int64_t last) {
				return (remaining * remaining * first + 2 * step * remaining * control + step * step * last) /
					   (SEGMENTS * SEGMENTS);
			};
			return {
				coordinate(road.Controls[0].X, road.Controls[1].X, road.Controls[2].X),
				coordinate(road.Controls[0].Y, road.Controls[1].Y, road.Controls[2].Y)
			};
		}

		bool NearRoad(Point point, const RoadSpline &road, int64_t clearance) {
			// Sixteen chords deviate by less than 23 subcells for the bounded bend.
			// Rounding and integer projection fit inside the 32-subcell margin.
			const int64_t safeClearance = clearance + 32;
			const int64_t threshold = Square(safeClearance);
			Point previous = road.Controls[0];
			for (int64_t step = 1; step <= 16; ++step) {
				const Point current = CurvePoint(road, step);
				if (SegmentDistanceSquared(point, previous, current) < threshold) return true;
				previous = current;
			}
			return false;
		}

		bool AppendPlacement(RegionResult &result, Placement placement, const GenerationLimits &limits) {
			const uint64_t bytes = static_cast<uint64_t>(result.Facts.OutputBytes) + 48 +
								   placement.Id.size() + placement.SourceName.size();
			if (result.Placements.size() >= limits.MaximumPlacements || bytes > limits.MaximumOutputBytes)
				return false;
			result.Facts.OutputBytes = static_cast<uint32_t>(bytes);
			result.Placements.push_back(std::move(placement));
			return true;
		}

		bool AppendRoad(RegionResult &result, RoadSpline road, const GenerationLimits &limits) {
			const uint64_t bytes = static_cast<uint64_t>(result.Facts.OutputBytes) + 64 + road.Id.size() +
								   road.EntranceName.size() + road.VillageName.size();
			if (result.Roads.size() >= limits.MaximumRoads || bytes > limits.MaximumOutputBytes) return false;
			result.Facts.OutputBytes = static_cast<uint32_t>(bytes);
			result.Roads.push_back(std::move(road));
			return true;
		}

		int64_t FloorCell(int64_t subcell) {
			const int64_t quotient = subcell / SUBCELLS_PER_CELL;
			return subcell < 0 && subcell % SUBCELLS_PER_CELL != 0 ? quotient - 1 : quotient;
		}

		int64_t Sign(int64_t value) {
			return (value > 0) - (value < 0);
		}

		const SpatialLeaf *FindVillage(const detail::QueryView &queries, std::string_view name) {
			const auto found = std::lower_bound(
				queries.VillageLeaves.begin(),
				queries.VillageLeaves.end(),
				name,
				[](const SpatialLeaf *leaf, std::string_view key) { return StableNameLess(leaf->Name, key); }
			);
			return found != queries.VillageLeaves.end() && (*found)->Name == name ? *found : nullptr;
		}

		Status BuildRoads(
			const RegionRequest &request,
			const detail::QueryView &queries,
			const ForestVillageSettings &settings,
			uint64_t seed,
			Bounds owner,
			RegionResult &result,
			std::vector<RoadSpline> &localRoads,
			GenerationControl *control,
			uint32_t completedCandidates,
			Diagnostic &diagnostic
		) {
			const int64_t competitorMargin =
				std::max(settings.TreeMinimumDistanceCells, settings.HouseMinimumDistanceCells) + 2;
			const int64_t influence = (competitorMargin + settings.MaximumRoadReachCells +
									   settings.RoadBendCells + settings.RoadClearanceCells + 2) *
									  SUBCELLS_PER_CELL;
			const Bounds influenced{
				{owner.Min.X - influence, owner.Min.Y - influence},
				{owner.Max.X + influence, owner.Max.Y + influence}
			};
			const int64_t maximumReach =
				static_cast<int64_t>(settings.MaximumRoadReachCells) * SUBCELLS_PER_CELL;
			size_t examined = 0;
			for (const SpatialLeaf *entrance : queries.EntranceLeaves) {
				if ((examined++ & 15) == 0 && !PollControl(control, result, completedCandidates, diagnostic))
					return Status::Canceled;
				if (!detail::Contains(influenced, entrance->Anchor)) continue;
				if (!Spend(result.Facts.WorkUnits, request.Limits.MaximumWorkUnits))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "region.work");
				const SpatialLeaf *village = FindVillage(queries, entrance->TargetVillage);
				if (village == nullptr)
					return detail::Refuse(diagnostic, Status::MissingSource, entrance->TargetVillage);
				if (entrance->Anchor == village->Anchor ||
					DistanceSquared(entrance->Anchor, village->Anchor) > Square(maximumReach))
					return detail::Refuse(diagnostic, Status::InvalidInput, entrance->Name);
				const uint64_t choice = CounterWord(
					seed,
					std::string("road.bend/") + entrance->Name,
					FloorCell(entrance->Anchor.X),
					FloorCell(entrance->Anchor.Y)
				);
				const uint64_t choices = static_cast<uint64_t>(settings.RoadBendCells) * 2 + 1;
				const int64_t bend = static_cast<int64_t>(choice % choices) - settings.RoadBendCells;
				const int64_t dx = village->Anchor.X - entrance->Anchor.X;
				const int64_t dy = village->Anchor.Y - entrance->Anchor.Y;
				const Point control{
					entrance->Anchor.X + dx / 2 - Sign(dy) * bend * SUBCELLS_PER_CELL,
					entrance->Anchor.Y + dy / 2 + Sign(dx) * bend * SUBCELLS_PER_CELL
				};
				RoadSpline road{
					"road/" + entrance->Name,
					entrance->Name,
					village->Name,
					{entrance->Anchor, control, village->Anchor}
				};
				if (localRoads.size() >= request.Limits.MaximumRoads)
					return detail::Refuse(diagnostic, Status::LimitExceeded, "region.roads");
				localRoads.push_back(road);
				if (detail::Contains(owner, entrance->Anchor) &&
					!AppendRoad(result, std::move(road), request.Limits))
					return detail::Refuse(diagnostic, Status::LimitExceeded, "region.output");
			}
			return Status::Ok;
		}

		Status GenerateCandidates(
			bool houses,
			const RegionRequest &request,
			const detail::QueryView &queries,
			const ForestVillageSettings &settings,
			uint64_t seed,
			Bounds owner,
			const std::vector<RoadSpline> &roads,
			RegionResult &result,
			GenerationControl *control,
			uint32_t &completedCandidates,
			Diagnostic &diagnostic
		) {
			if (!PollControl(control, result, completedCandidates, diagnostic)) return Status::Canceled;
			const uint16_t minimumCells =
				houses ? settings.HouseMinimumDistanceCells : settings.TreeMinimumDistanceCells;
			const int64_t margin = minimumCells + 2;
			const int64_t left = owner.Min.X / SUBCELLS_PER_CELL - margin;
			const int64_t top = owner.Min.Y / SUBCELLS_PER_CELL - margin;
			const int64_t width = request.Scope.CellsPerSide + margin * 2;
			const int64_t cells = width * width;
			if (cells > request.Limits.MaximumCandidateCells - result.Facts.CandidateCells)
				return detail::Refuse(diagnostic, Status::LimitExceeded, "region.candidates");
			result.Facts.CandidateCells += static_cast<uint32_t>(cells);
			std::vector<Candidate> candidates(static_cast<size_t>(cells));
			// The clear ring separates forest trees from houses at the village edge.
			const int64_t villageRadius =
				(static_cast<int64_t>(settings.VillageRadiusCells) +
				 (houses ? 0
						 : std::max(settings.TreeMinimumDistanceCells, settings.HouseMinimumDistanceCells))) *
				SUBCELLS_PER_CELL;
			const int64_t entranceRadius =
				static_cast<int64_t>(settings.EntranceClearanceCells) * SUBCELLS_PER_CELL;
			const int64_t roadRadius = static_cast<int64_t>(settings.RoadClearanceCells) * SUBCELLS_PER_CELL;
			const uint32_t density = houses ? settings.HouseDensityPpm : settings.ForestDensityPpm;
			const char *prefix = houses ? "house/" : "tree/";
			for (int64_t row = 0; row < width; ++row) {
				for (int64_t column = 0; column < width; ++column) {
					if (!Spend(result.Facts.WorkUnits, request.Limits.MaximumWorkUnits))
						return detail::Refuse(diagnostic, Status::LimitExceeded, "region.work");
					++completedCandidates;
					if ((completedCandidates & 63) == 0 &&
						!PollControl(control, result, completedCandidates, diagnostic))
						return Status::Canceled;
					const int64_t cellX = left + column;
					const int64_t cellY = top + row;
					Candidate &candidate = candidates[static_cast<size_t>(row * width + column)];
					candidate.Id = std::string(prefix) + std::to_string(cellX) + "/" + std::to_string(cellY);
					candidate.Position = {
						cellX * SUBCELLS_PER_CELL +
							static_cast<int64_t>(
								CounterWord(
									seed, houses ? "house.position.x" : "tree.position.x", cellX, cellY
								) %
								SUBCELLS_PER_CELL
							),
						cellY * SUBCELLS_PER_CELL +
							static_cast<int64_t>(
								CounterWord(
									seed, houses ? "house.position.y" : "tree.position.y", cellX, cellY
								) %
								SUBCELLS_PER_CELL
							)
					};
					const SpatialLeaf *nearestVillage = nullptr;
					int64_t nearestDistance = Square(villageRadius);
					for (const SpatialLeaf *village : queries.VillageLeaves) {
						if (!Spend(result.Facts.SpatialTests, request.Limits.MaximumSpatialTests))
							return detail::Refuse(diagnostic, Status::LimitExceeded, "region.spatial_tests");
						if ((result.Facts.SpatialTests & 63) == 0 &&
							!PollControl(control, result, completedCandidates, diagnostic))
							return Status::Canceled;
						const int64_t distance = DistanceSquared(candidate.Position, village->Anchor);
						if (distance < nearestDistance ||
							(distance == nearestDistance && nearestVillage != nullptr &&
							 StableNameLess(village->Name, nearestVillage->Name))) {
							nearestVillage = village;
							nearestDistance = distance;
						}
					}
					if (houses != (nearestVillage != nullptr)) continue;
					if (houses) candidate.SourceName = nearestVillage->Name;
					if (CounterWord(seed, houses ? "house.presence" : "tree.presence", cellX, cellY) %
							1000000 >=
						density)
						continue;
					bool clear = true;
					for (const SpatialLeaf *entrance : queries.EntranceLeaves) {
						if (!Spend(result.Facts.SpatialTests, request.Limits.MaximumSpatialTests))
							return detail::Refuse(diagnostic, Status::LimitExceeded, "region.spatial_tests");
						if ((result.Facts.SpatialTests & 63) == 0 &&
							!PollControl(control, result, completedCandidates, diagnostic))
							return Status::Canceled;
						if (DistanceSquared(candidate.Position, entrance->Anchor) < Square(entranceRadius)) {
							clear = false;
							break;
						}
					}
					if (!clear) continue;
					for (const RoadSpline &road : roads) {
						if (!Spend(result.Facts.SpatialTests, request.Limits.MaximumSpatialTests, 16))
							return detail::Refuse(diagnostic, Status::LimitExceeded, "region.spatial_tests");
						if ((result.Facts.SpatialTests & 63) == 0 &&
							!PollControl(control, result, completedCandidates, diagnostic))
							return Status::Canceled;
						if (NearRoad(candidate.Position, road, roadRadius)) {
							clear = false;
							break;
						}
					}
					if (!clear) continue;
					candidate.Priority =
						CounterWord(seed, houses ? "house.priority" : "tree.priority", cellX, cellY);
					candidate.Active = true;
				}
			}
			const int64_t minimumDistance = static_cast<int64_t>(minimumCells) * SUBCELLS_PER_CELL;
			for (int64_t row = margin; row < margin + request.Scope.CellsPerSide; ++row) {
				for (int64_t column = margin; column < margin + request.Scope.CellsPerSide; ++column) {
					const Candidate &candidate = candidates[static_cast<size_t>(row * width + column)];
					if (!candidate.Active) continue;
					bool accepted = true;
					for (int64_t dy = -margin; dy <= margin && accepted; ++dy) {
						for (int64_t dx = -margin; dx <= margin; ++dx) {
							if (dx == 0 && dy == 0) continue;
							if (!Spend(result.Facts.WorkUnits, request.Limits.MaximumWorkUnits))
								return detail::Refuse(diagnostic, Status::LimitExceeded, "region.work");
							if ((result.Facts.WorkUnits & 63) == 0 &&
								!PollControl(control, result, completedCandidates, diagnostic))
								return Status::Canceled;
							const Candidate &other =
								candidates[static_cast<size_t>((row + dy) * width + column + dx)];
							if (!other.Active || DistanceSquared(candidate.Position, other.Position) >=
													 Square(minimumDistance))
								continue;
							if (other.Priority < candidate.Priority ||
								(other.Priority == candidate.Priority &&
								 StableNameLess(other.Id, candidate.Id))) {
								accepted = false;
								break;
							}
						}
					}
					if (!accepted) continue;
					const int64_t cellX = left + column;
					const int64_t cellY = top + row;
					Placement placed;
					placed.Id = candidate.Id;
					placed.Kind = houses ? PlacementKind::House : PlacementKind::Tree;
					placed.SourceName = candidate.SourceName;
					placed.Position = candidate.Position;
					placed.RotationQuarterTurns = static_cast<uint8_t>(
						CounterWord(seed, houses ? "house.rotation" : "tree.rotation", cellX, cellY) % 4
					);
					placed.Variant = static_cast<uint16_t>(
						CounterWord(seed, houses ? "house.variant" : "tree.variant", cellX, cellY) %
						(houses ? settings.HouseVariants : settings.TreeVariants)
					);
					placed.Material = static_cast<uint16_t>(
						CounterWord(seed, houses ? "house.material" : "tree.material", cellX, cellY) %
						(houses ? settings.HouseMaterials : settings.TreeMaterials)
					);
					if (houses) {
						Placement lot = placed;
						lot.Id = "lot/" + candidate.Id.substr(6);
						lot.Kind = PlacementKind::VillageLot;
						lot.Variant = 0;
						if (!AppendPlacement(result, std::move(lot), request.Limits))
							return detail::Refuse(diagnostic, Status::LimitExceeded, "region.output");
					}
					if (!AppendPlacement(result, std::move(placed), request.Limits))
						return detail::Refuse(diagnostic, Status::LimitExceeded, "region.output");
				}
			}
			if (!PollControl(control, result, completedCandidates, diagnostic)) return Status::Canceled;
			return Status::Ok;
		}
	}

	Status GenerateRegion(
		const CompiledRecipe &recipe,
		const RegionRequest &request,
		RegionResult &out,
		Diagnostic &diagnostic,
		GenerationControl *control
	) {
		try {
			RegionResult candidate;
			uint32_t completedCandidates = 0;
			if (!PollControl(control, candidate, completedCandidates, diagnostic)) return Status::Canceled;
			const Status key = BuildRegionKey(recipe, request, candidate.KeyBytes, diagnostic);
			if (key != Status::Ok) return key;
			if (candidate.KeyBytes.size() > request.Limits.MaximumOutputBytes)
				return detail::Refuse(diagnostic, Status::LimitExceeded, "region.output");
			candidate.Facts.OutputBytes = static_cast<uint32_t>(candidate.KeyBytes.size());
			Bounds owner;
			Bounds required;
			const Status bounds = RequiredRegionBounds(recipe, request.Scope, owner, required, diagnostic);
			if (bounds != Status::Ok) return bounds;
			detail::QueryView queries;
			const Status coverage = detail::ValidateQueries(request, required, queries, diagnostic);
			if (coverage != Status::Ok) return coverage;
			if (!PollControl(control, candidate, completedCandidates, diagnostic)) return Status::Canceled;
			const detail::QueryView *sources = nullptr;
			uint64_t seed = 0;
			std::vector<RoadSpline> localRoads;
			for (uint8_t stage : recipe.Stages_) {
				if (!PollControl(control, candidate, completedCandidates, diagnostic))
					return Status::Canceled;
				switch (static_cast<detail::Stage>(stage)) {
				case detail::Stage::Seed:
					seed = recipe.Definition_.RootSeed;
					break;
				case detail::Stage::VillageSources:
					sources = &queries;
					break;
				case detail::Stage::Entrance:
					for (const SpatialLeaf *entrance : sources->EntranceLeaves) {
						if (!detail::Contains(owner, entrance->Anchor)) continue;
						Placement placed;
						placed.Id = "entrance/" + entrance->Name;
						placed.Kind = PlacementKind::Entrance;
						placed.SourceName = entrance->TargetVillage;
						placed.Position = entrance->Anchor;
						if (!AppendPlacement(candidate, std::move(placed), request.Limits))
							return detail::Refuse(diagnostic, Status::LimitExceeded, "region.output");
					}
					break;
				case detail::Stage::Road: {
					const Status status = BuildRoads(
						request,
						*sources,
						recipe.Settings_,
						seed,
						owner,
						candidate,
						localRoads,
						control,
						completedCandidates,
						diagnostic
					);
					if (status != Status::Ok) return status;
					break;
				}
				case detail::Stage::VillageLots:
				case detail::Stage::Forest: {
					const Status status = GenerateCandidates(
						stage == static_cast<uint8_t>(detail::Stage::VillageLots),
						request,
						*sources,
						recipe.Settings_,
						seed,
						owner,
						localRoads,
						candidate,
						control,
						completedCandidates,
						diagnostic
					);
					if (status != Status::Ok) return status;
					break;
				}
				default:
					return detail::Refuse(diagnostic, Status::InvalidRecipe, "recipe.stages");
				}
			}
			std::sort(
				candidate.Placements.begin(),
				candidate.Placements.end(),
				[](const Placement &a, const Placement &b) { return StableNameLess(a.Id, b.Id); }
			);
			std::sort(
				candidate.Roads.begin(), candidate.Roads.end(), [](const RoadSpline &a, const RoadSpline &b) {
					return StableNameLess(a.Id, b.Id);
				}
			);
			if (!PollControl(control, candidate, completedCandidates, diagnostic)) return Status::Canceled;
			out = std::move(candidate);
			diagnostic = {};
			return Status::Ok;
		} catch (const std::bad_alloc &) {
			return detail::Refuse(diagnostic, Status::AllocationFailed);
		}
	}
}
