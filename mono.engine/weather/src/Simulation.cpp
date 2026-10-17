#include <engine/core/Profiling.hpp>
#include <engine/weather/Simulation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace engine::weather {
	namespace {
		constexpr uint32_t MAXIMUM_CELLS = 65536;
		constexpr uint32_t MAXIMUM_FORCING = 64;
		constexpr double KELVIN_OFFSET = 273.15;
		constexpr uint32_t BLOCKED_NEIGHBOR = UINT32_MAX - 1;

		uint64_t CellCount(const SimulationGrid &grid) {
			uint64_t count = 1;
			for (uint32_t extent : grid.Cells) {
				if (extent < 2 || extent > 128) return 0;
				count *= extent;
			}
			return count <= MAXIMUM_CELLS ? count : 0;
		}

		bool Bounded(float number, float minimum, float maximum) {
			return std::isfinite(number) && number >= minimum && number <= maximum;
		}

		bool ValidCell(const SolverCell &cell, float maximumVelocity) {
			if (!Bounded(cell.VapourKilogramsPerKilogram, 0, 1) ||
				!Bounded(cell.CondensateKilogramsPerCubicMetre, 0, 100) ||
				!Bounded(cell.TemperatureCelsius, -100, 100) ||
				!Bounded(cell.AtmosphericPressurePascals, 1000, 200000) ||
				!Bounded(cell.ProjectionPressurePascals, -1e9f, 1e9f))
				return false;
			for (float velocity : cell.VelocityMetresPerSecond)
				if (!Bounded(velocity, -maximumVelocity, maximumVelocity)) return false;
			return true;
		}

		bool ValidDefinition(const SimulationDefinition &definition) {
			if (definition.AlgorithmName != "constant-density-moist-air" ||
				definition.AlgorithmVersion != 1 || definition.LayerName.empty() ||
				definition.LayerName.size() > 256 || definition.Revision == 0 || definition.WorldEpoch == 0 ||
				!CellCount(definition.Grid) || definition.TickRate < 1 || definition.TickRate > 1000 ||
				definition.MaximumSubsteps < 1 || definition.MaximumSubsteps > 128 ||
				definition.ProjectionIterations < 1 || definition.ProjectionIterations > 4096 ||
				!Bounded(definition.Grid.CellSizeMetres, 0.25f, 4096) ||
				!Bounded(definition.DryAirKilogramsPerCubicMetre, 0.1f, 10) ||
				!Bounded(definition.DiffusionSquareMetresPerSecond, 0, 1000) ||
				!Bounded(definition.ReferenceTemperatureCelsius, -100, 100) ||
				!Bounded(definition.SaturationVapourRatio, 0.000001f, 0.2f) ||
				!Bounded(definition.SaturationSlopePerCelsius, 0, 0.1f) ||
				!Bounded(definition.PhaseRelaxationPerSecond, 0, 100) ||
				!Bounded(definition.LatentHeatJoulesPerKilogram, 1, 3000000) ||
				!Bounded(definition.HeatCapacityJoulesPerKilogramCelsius, 100, 10000) ||
				!Bounded(definition.BuoyancyMetresPerSecondSquaredPerCelsius, 0, 1) ||
				!Bounded(definition.PrecipitationThresholdKilogramsPerCubicMetre, 0, 100) ||
				!Bounded(definition.PrecipitationPerSecond, 0, 100) ||
				!Bounded(definition.MaximumVelocityMetresPerSecond, 0.01f, 4096) ||
				!Bounded(definition.MaximumPressureGradientPascalsPerMetre, 0, 10000) ||
				!Bounded(definition.MaximumDivergencePerSecond, 0.000001f, 0.1f) ||
				definition.TerrainRevision == 0 ||
				!Bounded(definition.GroundVapourFluxKilogramsPerSquareMetreSecond, 0, 1) ||
				!Bounded(definition.RainFallSpeedMetresPerSecond, 0, 64))
				return false;
			for (SolverBoundary boundary : definition.Grid.Boundaries)
				if (boundary != SolverBoundary::Periodic && boundary != SolverBoundary::Closed &&
					boundary != SolverBoundary::Open && boundary != SolverBoundary::GroundOpen)
					return false;
			if (definition.Grid.Boundaries[0] == SolverBoundary::GroundOpen ||
				definition.Grid.Boundaries[2] == SolverBoundary::GroundOpen)
				return false;
			// Four physical buffers and three scalar buffers include transactional stepping and projection
			// scratch.
			return definition.MaximumStateBytes >= CellCount(definition.Grid) * 200;
		}

		bool ValidForcing(const SimulationForcing &forcing, const SimulationDefinition &definition) {
			if (!ValidCell(forcing.Inflow, definition.MaximumVelocityMetresPerSecond) ||
				!Bounded(forcing.VapourSourcePerSecond, 0, 1) ||
				!Bounded(forcing.CondensateSourceKilogramsPerCubicMetrePerSecond, 0, 100) ||
				!Bounded(forcing.CoolingCelsiusPerSecond, -100, 100) ||
				!Bounded(forcing.WindRelaxationPerSecond, 0, 10))
				return false;
			for (size_t axis = 0; axis < 3; ++axis)
				if (!Bounded(forcing.AccelerationMetresPerSecondSquared[axis], -256, 256) ||
					!Bounded(
						forcing.OpenVelocityMetresPerSecond[axis],
						-definition.MaximumVelocityMetresPerSecond,
						definition.MaximumVelocityMetresPerSecond
					) ||
					!Bounded(
						forcing.WindTargetMetresPerSecond[axis],
						-definition.MaximumVelocityMetresPerSecond,
						definition.MaximumVelocityMetresPerSecond
					))
					return false;
			return true;
		}

		std::array<uint32_t, 3> Coordinate(size_t index, const SimulationGrid &grid) {
			return {
				static_cast<uint32_t>(index % grid.Cells[0]),
				static_cast<uint32_t>((index / grid.Cells[0]) % grid.Cells[1]),
				static_cast<uint32_t>(index / (grid.Cells[0] * grid.Cells[1]))
			};
		}

		std::optional<size_t> Neighbor(size_t index, size_t axis, int direction, const SimulationGrid &grid) {
			auto coordinate = Coordinate(index, grid);
			const int shifted = static_cast<int>(coordinate[axis]) + direction;
			if (shifted < 0 || shifted >= static_cast<int>(grid.Cells[axis])) {
				if (grid.Boundaries[axis] != SolverBoundary::Periodic) return std::nullopt;
				coordinate[axis] = shifted < 0 ? grid.Cells[axis] - 1 : 0;
			} else
				coordinate[axis] = static_cast<uint32_t>(shifted);
			return coordinate[0] + grid.Cells[0] * (coordinate[1] + grid.Cells[1] * coordinate[2]);
		}

		std::optional<size_t>
		Neighbor(size_t index, size_t axis, int direction, const SimulationWorkspace &workspace) {
			const uint32_t neighbor = workspace.Neighbors[index][axis * 2 + (direction > 0 ? 1 : 0)];
			return neighbor >= BLOCKED_NEIGHBOR ? std::nullopt : std::optional<size_t>(neighbor);
		}
		bool Exterior(size_t index, size_t axis, int direction, const SimulationWorkspace &workspace) {
			return workspace.Neighbors[index][axis * 2 + (direction > 0 ? 1 : 0)] == UINT32_MAX;
		}
		bool OpenFace(const SimulationGrid &grid, size_t axis, int direction) {
			return grid.Boundaries[axis] == SolverBoundary::Open ||
				   (grid.Boundaries[axis] == SolverBoundary::GroundOpen && axis == 1 && direction > 0);
		}
		void PrepareWorkspace(const SimulationState &state, SimulationWorkspace &workspace) {
			if (workspace.Grid != state.Definition.Grid || workspace.Neighbors.size() != state.Cells.size() ||
				workspace.SolidMask != state.SolidMask ||
				workspace.GroundHeightMetres != state.GroundHeightMetres) {
				// Releasing old capacities keeps a reused workspace inside the new domain's admission budget.
				workspace = {};
				workspace.Grid = state.Definition.Grid;
				workspace.SolidMask = state.SolidMask;
				workspace.GroundHeightMetres = state.GroundHeightMetres;
				workspace.TerrainFlags.resize(state.Cells.size());
				workspace.PressureNeighborCounts.assign(state.Cells.size(), 0);
				workspace.Neighbors.resize(state.Cells.size());
				for (size_t index = 0; index < state.Cells.size(); ++index) {
					workspace.TerrainFlags[index] = static_cast<uint8_t>(SimulationCellFlags(state, index));
					for (size_t axis = 0; axis < 3; ++axis)
						for (int direction : {-1, 1}) {
							const auto neighbor = Neighbor(index, axis, direction, workspace.Grid);
							const bool blocked = (SimulationCellFlags(state, index) & 1) ||
												 (neighbor && (SimulationCellFlags(state, *neighbor) & 1));
							workspace.Neighbors[index][axis * 2 + (direction > 0 ? 1 : 0)] =
								blocked	   ? BLOCKED_NEIGHBOR
								: neighbor ? static_cast<uint32_t>(*neighbor)
										   : UINT32_MAX;
							if (!blocked && (neighbor || OpenFace(workspace.Grid, axis, direction)))
								++workspace.PressureNeighborCounts[index];
						}
				}
			}
			workspace.ForcedCells.resize(state.Cells.size());
			workspace.NextCells.resize(state.Cells.size());
			workspace.Pressure.resize(state.Cells.size());
			workspace.NextPressure.resize(state.Cells.size());
			workspace.Divergence.resize(state.Cells.size());
			workspace.NextFallingCondensate.resize(state.Cells.size());
		}

		float LowVelocity(
			size_t index,
			size_t axis,
			const std::vector<SolverCell> &cells,
			const SimulationWorkspace &workspace,
			const SimulationForcing &forcing,
			float projectionScale = 0
		) {
			const auto &grid = workspace.Grid;
			const auto low = Neighbor(index, axis, -1, workspace);
			if (low) return cells[*low].VelocityMetresPerSecond[axis];
			return Exterior(index, axis, -1, workspace) && OpenFace(grid, axis, -1)
					   ? forcing.OpenVelocityMetresPerSecond[axis] -
							 projectionScale * cells[index].ProjectionPressurePascals
					   : 0;
		}

		float Divergence(
			size_t index,
			const std::vector<SolverCell> &cells,
			const SimulationWorkspace &workspace,
			const SimulationForcing &forcing,
			float projectionScale = 0
		) {
			const auto &grid = workspace.Grid;
			float divergence = 0;
			for (size_t axis = 0; axis < 3; ++axis)
				divergence += cells[index].VelocityMetresPerSecond[axis] -
							  LowVelocity(index, axis, cells, workspace, forcing, projectionScale);
			return divergence / grid.CellSizeMetres;
		}

		bool Project(
			std::vector<SolverCell> &cells,
			const SimulationDefinition &definition,
			const SimulationForcing &forcing,
			float seconds,
			const SimulationWorkspace &workspace,
			std::vector<float> &pressure,
			std::vector<float> &nextPressure,
			std::vector<float> &divergence,
			float &maximumDivergence
		) {
			const auto &grid = definition.Grid;
			const size_t cellCount = cells.size();
			const auto *adjacency = workspace.Neighbors.data();
			const auto *counts = workspace.PressureNeighborCounts.data();
			const auto *flags = workspace.TerrainFlags.data();
			auto *physical = cells.data();
			auto *currentPressure = pressure.data();
			auto *outputPressure = nextPressure.data();
			auto *rhs = divergence.data();
			const bool open = std::any_of(grid.Boundaries.begin(), grid.Boundaries.end(), [](auto boundary) {
				return boundary == SolverBoundary::Open || boundary == SolverBoundary::GroundOpen;
			});
			for (size_t index = 0; index < cellCount; ++index) {
				currentPressure[index] = flags[index] & 1 ? 0 : physical[index].ProjectionPressurePascals;
				rhs[index] = flags[index] & 1 ? 0 : Divergence(index, cells, workspace, forcing);
			}
			const float pressureScale =
				definition.DryAirKilogramsPerCubicMetre * grid.CellSizeMetres * grid.CellSizeMetres / seconds;
			// Typed raw rows remove optional/wrapper churn without changing the six-term summation order.
			for (uint32_t iteration = 0; iteration < definition.ProjectionIterations; ++iteration) {
				for (size_t index = 0; index < cellCount; ++index) {
					float neighbors = 0;
					const uint32_t *row = adjacency[index].data();
					for (size_t face = 0; face < 6; ++face) {
						const uint32_t neighbor = row[face];
						if (neighbor < cellCount) neighbors += currentPressure[neighbor];
					}
					const uint32_t count = counts[index];
					if (!count) {
						outputPressure[index] = 0;
						continue;
					}
					const float solved = (neighbors - pressureScale * rhs[index]) / static_cast<float>(count);
					outputPressure[index] = 0.25f * currentPressure[index] + 0.75f * solved;
				}
				pressure.swap(nextPressure);
				std::swap(currentPressure, outputPressure);
			}
			const float velocityScale =
				seconds / (definition.DryAirKilogramsPerCubicMetre * grid.CellSizeMetres);
			const float gauge = open ? 0 : currentPressure[0];
			for (size_t index = 0; index < cellCount; ++index) {
				physical[index].ProjectionPressurePascals =
					flags[index] & 1 ? 0 : currentPressure[index] - gauge;
				const uint32_t *row = adjacency[index].data();
				for (size_t axis = 0; axis < 3; ++axis) {
					const uint32_t high = row[axis * 2 + 1];
					if (high < cellCount)
						physical[index].VelocityMetresPerSecond[axis] -=
							velocityScale * (currentPressure[high] - currentPressure[index]);
					else if (high == UINT32_MAX && OpenFace(grid, axis, 1))
						physical[index].VelocityMetresPerSecond[axis] +=
							velocityScale * currentPressure[index];
					else
						physical[index].VelocityMetresPerSecond[axis] = 0;
				}
			}
			maximumDivergence = 0;
			for (size_t index = 0; index < cellCount; ++index) {
				if (flags[index] & 1) continue;
				maximumDivergence = std::max(
					maximumDivergence,
					std::abs(Divergence(index, cells, workspace, forcing, open ? velocityScale : 0))
				);
				for (size_t axis = 0; axis < 3; ++axis)
					if (std::abs(
							LowVelocity(index, axis, cells, workspace, forcing, open ? velocityScale : 0)
						) > definition.MaximumVelocityMetresPerSecond)
						return false;
			}
			return std::isfinite(maximumDivergence) &&
				   maximumDivergence <= definition.MaximumDivergencePerSecond;
		}

		float Scalar(const SolverCell &cell, size_t quantity) {
			if (quantity == 0) return cell.VapourKilogramsPerKilogram;
			if (quantity == 1) return cell.CondensateKilogramsPerCubicMetre;
			return cell.TemperatureCelsius + static_cast<float>(KELVIN_OFFSET);
		}

		float Flux(
			const SolverCell &left,
			const SolverCell &right,
			float velocity,
			size_t quantity,
			const SimulationDefinition &definition
		) {
			const float leftScalar = Scalar(left, quantity);
			const float rightScalar = Scalar(right, quantity);
			return velocity * (velocity >= 0 ? leftScalar : rightScalar) -
				   definition.DiffusionSquareMetresPerSecond * (rightScalar - leftScalar) /
					   definition.Grid.CellSizeMetres;
		}

		float Saturation(float temperature, const SimulationDefinition &definition) {
			// This version declares an exponential saturation proxy, rather than a compressible equation of
			// state.
			return definition.SaturationVapourRatio *
				   std::exp(
					   definition.SaturationSlopePerCelsius *
					   (temperature - definition.ReferenceTemperatureCelsius)
				   );
		}

		bool ValidProfile(const EncodingProfile &profile) {
			return Bounded(profile.CondensateKilogramsPerCubicMetre, 0.000001f, 100) &&
				   Bounded(profile.VapourKilogramsPerKilogram, 0.000001f, 1) &&
				   Bounded(profile.TemperatureAnomalyCelsius, 0.000001f, 200) &&
				   Bounded(profile.PressureAnomalyHectopascals, 0.000001f, 2000) &&
				   Bounded(profile.CellSizeMetres, 0.25f, 4096) &&
				   Bounded(profile.BaseTemperatureCelsius, -100, 100) &&
				   Bounded(profile.BasePressureHectopascals, 10, 2000) &&
				   Bounded(profile.TemperatureLapseCelsiusPerKilometre, 0, 20) &&
				   Bounded(profile.WindScaleMetresPerSecond.X, 0.000001f, 4096) &&
				   Bounded(profile.WindScaleMetresPerSecond.Y, 0.000001f, 4096) &&
				   Bounded(profile.WindScaleMetresPerSecond.Z, 0.000001f, 4096);
		}

		void ConvertPhase(SolverCell &cell, const SimulationDefinition &definition, float seconds) {
			if (definition.PhaseRelaxationPerSecond == 0) return;
			float low = -cell.CondensateKilogramsPerCubicMetre / definition.DryAirKilogramsPerCubicMetre;
			float high = cell.VapourKilogramsPerKilogram;
			const float heatScale =
				definition.LatentHeatJoulesPerKilogram / definition.HeatCapacityJoulesPerKilogramCelsius;
			if (cell.VapourKilogramsPerKilogram - low -
					Saturation(cell.TemperatureCelsius + heatScale * low, definition) <=
				0)
				high = low;
			else
				for (uint32_t iteration = 0; iteration < 24; ++iteration) {
					const float conversion = 0.5f * (low + high);
					const float difference =
						cell.VapourKilogramsPerKilogram - conversion -
						Saturation(cell.TemperatureCelsius + heatScale * conversion, definition);
					if (difference > 0)
						low = conversion;
					else
						high = conversion;
				}
			float conversion =
				(0.5f * (low + high)) * std::min(1.0f, seconds * definition.PhaseRelaxationPerSecond);
			if (conversion < 0) {
				const float mass = std::min(
					cell.CondensateKilogramsPerCubicMetre,
					-definition.DryAirKilogramsPerCubicMetre * conversion
				);
				conversion = -mass / definition.DryAirKilogramsPerCubicMetre;
				cell.CondensateKilogramsPerCubicMetre -= mass;
			} else
				cell.CondensateKilogramsPerCubicMetre += definition.DryAirKilogramsPerCubicMetre * conversion;
			cell.VapourKilogramsPerKilogram -= conversion;
			cell.TemperatureCelsius += heatScale * conversion;
		}

		bool TransportRain(
			SimulationState &state,
			const std::vector<SolverCell> &projected,
			const SimulationForcing &forcing,
			float seconds,
			SimulationWorkspace &workspace
		) {
			const auto &definition = state.Definition;
			const auto &grid = definition.Grid;
			const float h = grid.CellSizeMetres;
			const double volume = std::pow(static_cast<double>(h), 3);
			const float pressureScale = seconds / (definition.DryAirKilogramsPerCubicMetre * h);
			const auto &old = state.FallingCondensateKilogramsPerCubicMetre;
			auto &next = workspace.NextFallingCondensate;
			for (size_t index = 0; index < old.size(); ++index) {
				if (workspace.TerrainFlags[index] & 1) {
					next[index] = 0;
					continue;
				}
				float change = 0, outgoing = 0;
				for (size_t axis = 0; axis < 3; ++axis) {
					const auto low = Neighbor(index, axis, -1, workspace);
					const auto high = Neighbor(index, axis, 1, workspace);
					const bool lowExterior = Exterior(index, axis, -1, workspace);
					const bool highExterior = Exterior(index, axis, 1, workspace);
					const bool highOpen = OpenFace(grid, axis, 1);
					const bool lowOpen = OpenFace(grid, axis, -1);
					const bool groundSink = !low && axis == 1;
					const float fall = axis == 1 ? definition.RainFallSpeedMetresPerSecond : 0;
					const bool highActive = high || (highExterior && highOpen);
					const bool lowActive = low || groundSink || (lowExterior && lowOpen);
					const float highVelocity = projected[index].VelocityMetresPerSecond[axis] - fall;
					const float lowVelocity =
						low || (lowExterior && lowOpen)
							? LowVelocity(index, axis, projected, workspace, forcing, pressureScale) - fall
							: -fall;
					const float highFlux = highActive ? highVelocity * (highVelocity >= 0 ? old[index]
																		: high			  ? old[*high]
																						  : 0)
													  : 0;
					const float lowFlux =
						lowActive ? lowVelocity * (lowVelocity >= 0 ? low ? old[*low] : 0 : old[index]) : 0;
					change += seconds * (lowFlux - highFlux) / h;
					outgoing += (highActive ? std::max(highVelocity, 0.0f) : 0) +
								(lowActive ? std::max(-lowVelocity, 0.0f) : 0);
					if (groundSink)
						state.Budget.PrecipitationWaterKilograms -= volume * seconds * lowFlux / h;
					else if (lowExterior && lowOpen)
						state.Budget.BoundaryWaterKilograms += volume * seconds * lowFlux / h;
					if (highExterior && highOpen)
						state.Budget.BoundaryWaterKilograms -= volume * seconds * highFlux / h;
				}
				if (seconds * outgoing / h > 1) return false;
				next[index] = old[index] + change;
				if (!Bounded(next[index], 0, 100)) return false;
			}
			return true;
		}

		SimulationStatus Evolve(SimulationState &state, SimulationWorkspace &workspace) {
			const auto &definition = state.Definition;
			const auto &grid = definition.Grid;
			const auto found = std::upper_bound(
				state.ForcingHistory.begin(),
				state.ForcingHistory.end(),
				state.Tick,
				[](uint64_t tick, const auto &forcing) { return tick < forcing.Tick; }
			);
			const auto &forcing = *std::prev(found);
			const float tickSeconds = 1.0f / static_cast<float>(definition.TickRate);
			const auto schedule = SimulationSubsteps(definition, forcing);
			if (!schedule) return SimulationStatus::Unstable;
			const uint32_t substeps = *schedule;
			const float seconds = tickSeconds / static_cast<float>(substeps);
			const double volume = std::pow(static_cast<double>(grid.CellSizeMetres), 3);
			auto &next = workspace.ForcedCells;
			auto &pressure = workspace.Pressure;
			auto &nextPressure = workspace.NextPressure;
			auto &divergence = workspace.Divergence;
			for (uint32_t substep = 0; substep < substeps; ++substep) {
				next = state.Cells;
				for (size_t index = 0; index < state.Cells.size(); ++index)
					for (size_t axis = 0; axis < 3; ++axis) {
						const auto high = Neighbor(index, axis, 1, workspace);
						if (!high) {
							next[index].VelocityMetresPerSecond[axis] =
								Exterior(index, axis, 1, workspace) && OpenFace(grid, axis, 1)
									? forcing.OpenVelocityMetresPerSecond[axis]
									: 0;
							continue;
						}
						const float gradient =
							(state.Cells[*high].AtmosphericPressurePascals -
							 state.Cells[index].AtmosphericPressurePascals) /
							(grid.CellSizeMetres * definition.DryAirKilogramsPerCubicMetre);
						const float buoyancy = axis == 1
												   ? definition.BuoyancyMetresPerSecondSquaredPerCelsius *
														 (0.5f * (state.Cells[index].TemperatureCelsius +
																  state.Cells[*high].TemperatureCelsius) -
														  definition.ReferenceTemperatureCelsius)
												   : 0;
						next[index].VelocityMetresPerSecond[axis] +=
							seconds *
							(forcing.AccelerationMetresPerSecondSquared[axis] + buoyancy - gradient +
							 forcing.WindRelaxationPerSecond *
								 (forcing.WindTargetMetresPerSecond[axis] -
								  state.Cells[index].VelocityMetresPerSecond[axis]));
					}
				float maximumDivergence = 0;
				if (!Project(
						next,
						definition,
						forcing,
						seconds,
						workspace,
						pressure,
						nextPressure,
						divergence,
						maximumDivergence
					))
					return SimulationStatus::ProjectionFailed;
				state.LastMaximumDivergencePerSecond = maximumDivergence;
				const float projectionScale =
					seconds / (definition.DryAirKilogramsPerCubicMetre * grid.CellSizeMetres);
				if (!TransportRain(state, next, forcing, seconds, workspace))
					return SimulationStatus::Unstable;
				for (size_t index = 0; index < state.Cells.size(); ++index) {
					if (workspace.TerrainFlags[index] & 1) continue;
					float outgoingRate = 0;
					for (size_t axis = 0; axis < 3; ++axis)
						outgoingRate +=
							std::max(0.0f, next[index].VelocityMetresPerSecond[axis]) +
							std::max(
								0.0f, -LowVelocity(index, axis, next, workspace, forcing, projectionScale)
							);
					if (seconds * (outgoingRate / grid.CellSizeMetres +
								   6 * definition.DiffusionSquareMetresPerSecond /
									   (grid.CellSizeMetres * grid.CellSizeMetres)) >
						1)
						return SimulationStatus::Unstable;
				}
				// Physical scalars read the old state while fluxes use projected face velocities.
				auto &transported = workspace.NextCells;
				transported = state.Cells;
				for (size_t index = 0; index < state.Cells.size(); ++index) {
					if (workspace.TerrainFlags[index] & 1) continue;
					float scalarChange[3]{};
					for (size_t axis = 0; axis < 3; ++axis) {
						const auto low = Neighbor(index, axis, -1, workspace);
						const auto high = Neighbor(index, axis, 1, workspace);
						for (size_t quantity = 0; quantity < 3; ++quantity) {
							const bool lowOpen =
								Exterior(index, axis, -1, workspace) && OpenFace(grid, axis, -1);
							const bool highOpen =
								Exterior(index, axis, 1, workspace) && OpenFace(grid, axis, 1);
							const float highFlux = high || highOpen
													   ? Flux(
															 state.Cells[index],
															 high ? state.Cells[*high] : forcing.Inflow,
															 next[index].VelocityMetresPerSecond[axis],
															 quantity,
															 definition
														 )
													   : 0;
							const float lowFlux =
								low || lowOpen
									? Flux(
										  low ? state.Cells[*low] : forcing.Inflow,
										  state.Cells[index],
										  LowVelocity(index, axis, next, workspace, forcing, projectionScale),
										  quantity,
										  definition
									  )
									: 0;
							scalarChange[quantity] += seconds * (lowFlux - highFlux) / grid.CellSizeMetres;
							const double boundaryChange = seconds *
														  ((!low ? lowFlux : 0) - (!high ? highFlux : 0)) /
														  grid.CellSizeMetres;
							if (quantity < 2)
								state.Budget.BoundaryWaterKilograms +=
									boundaryChange * volume *
									(quantity == 0 ? definition.DryAirKilogramsPerCubicMetre : 1);
							if (quantity == 0)
								state.Budget.ExternalEnthalpyJoules +=
									boundaryChange * volume * definition.DryAirKilogramsPerCubicMetre *
									definition.LatentHeatJoulesPerKilogram;
							if (quantity == 2)
								state.Budget.ExternalEnthalpyJoules +=
									boundaryChange * volume * definition.DryAirKilogramsPerCubicMetre *
									definition.HeatCapacityJoulesPerKilogramCelsius;
						}
					}
					SolverCell &cell = transported[index];
					const float groundVapour =
						workspace.TerrainFlags[index] & 2
							? definition.GroundVapourFluxKilogramsPerSquareMetreSecond /
								  (definition.DryAirKilogramsPerCubicMetre * grid.CellSizeMetres)
							: 0;
					cell.VapourKilogramsPerKilogram +=
						scalarChange[0] + seconds * (forcing.VapourSourcePerSecond + groundVapour);
					cell.CondensateKilogramsPerCubicMetre +=
						scalarChange[1] + seconds * forcing.CondensateSourceKilogramsPerCubicMetrePerSecond;
					cell.TemperatureCelsius += scalarChange[2] - seconds * forcing.CoolingCelsiusPerSecond;
					cell.VelocityMetresPerSecond = next[index].VelocityMetresPerSecond;
					cell.ProjectionPressurePascals = next[index].ProjectionPressurePascals;
					ConvertPhase(cell, definition, seconds);
					const float precipitation = std::min(
						100 - workspace.NextFallingCondensate[index],
						std::max(
							0.0f,
							cell.CondensateKilogramsPerCubicMetre -
								definition.PrecipitationThresholdKilogramsPerCubicMetre
						) * std::min(1.0f, seconds * definition.PrecipitationPerSecond)
					);
					cell.CondensateKilogramsPerCubicMetre -= precipitation;
					workspace.NextFallingCondensate[index] += precipitation;
					state.Budget.SourceWaterKilograms +=
						volume * seconds *
						(definition.DryAirKilogramsPerCubicMetre *
							 (forcing.VapourSourcePerSecond + groundVapour) +
						 forcing.CondensateSourceKilogramsPerCubicMetrePerSecond);
					state.Budget.ExternalEnthalpyJoules +=
						volume * seconds * definition.DryAirKilogramsPerCubicMetre *
						(definition.LatentHeatJoulesPerKilogram *
							 (forcing.VapourSourcePerSecond + groundVapour) -
						 definition.HeatCapacityJoulesPerKilogramCelsius * forcing.CoolingCelsiusPerSecond);
					if (!ValidCell(cell, definition.MaximumVelocityMetresPerSecond))
						return SimulationStatus::Unstable;
				}
				state.Cells.swap(transported);
				state.FallingCondensateKilogramsPerCubicMetre.swap(workspace.NextFallingCondensate);
			}
			++state.Tick;
			state.LastSubsteps = substeps;
			state.Budget.WaterResidualKilograms =
				SimulationWaterKilograms(state) - state.Budget.InitialWaterKilograms -
				state.Budget.SourceWaterKilograms - state.Budget.BoundaryWaterKilograms +
				state.Budget.PrecipitationWaterKilograms;
			state.Budget.EnthalpyResidualJoules = SimulationEnthalpyJoules(state) -
												  state.Budget.InitialEnthalpyJoules -
												  state.Budget.ExternalEnthalpyJoules;
			return SimulationStatus::Success;
		}
	}

	uint32_t SimulationCellFlags(const SimulationState &state, size_t index) {
		if (index >= state.Cells.size() || state.SolidMask.size() != (state.Cells.size() + 7) / 8) return 0;
		if (state.SolidMask[index / 8] & (1u << (index & 7))) return 1;
		const auto &grid = state.Definition.Grid;
		const auto position = Coordinate(index, grid);
		const size_t column = position[0] + grid.Cells[0] * position[2];
		if (column >= state.GroundHeightMetres.size()) return 0;
		const float height = state.GroundHeightMetres[column];
		if (height == NO_TERRAIN_HEIGHT) return 0;
		if (position[1] > 0) {
			const size_t below = index - grid.Cells[0];
			return state.SolidMask[below / 8] & (1u << (below & 7)) ? 2 : 0;
		}
		const float bottom = grid.Origin[1] * grid.CellSizeMetres;
		return height >= bottom && height < bottom + 0.5f * grid.CellSizeMetres ? 2 : 0;
	}

	bool ValidateSimulation(const SimulationState &state) {
		if (!ValidDefinition(state.Definition) || state.Cells.size() != CellCount(state.Definition.Grid) ||
			state.ForcingHistory.empty() || state.ForcingHistory.size() > MAXIMUM_FORCING ||
			state.ForcingHistory.front().Tick > state.Tick ||
			state.LastSubsteps > state.Definition.MaximumSubsteps ||
			!Bounded(state.LastMaximumDivergencePerSecond, 0, state.Definition.MaximumDivergencePerSecond))
			return false;
		const auto &grid = state.Definition.Grid;
		if (state.SolidMask.size() != (state.Cells.size() + 7) / 8 ||
			state.GroundHeightMetres.size() != static_cast<size_t>(grid.Cells[0]) * grid.Cells[2] ||
			state.FallingCondensateKilogramsPerCubicMetre.size() != state.Cells.size())
			return false;
		if ((state.Cells.size() & 7) && (state.SolidMask.back() >> (state.Cells.size() & 7))) return false;
		for (float height : state.GroundHeightMetres)
			if (height != NO_TERRAIN_HEIGHT && !Bounded(height, -1e9f, 1e9f)) return false;
		for (size_t index = 0; index < state.ForcingHistory.size(); ++index)
			if (!ValidForcing(state.ForcingHistory[index], state.Definition) ||
				(index && state.ForcingHistory[index - 1].Tick >= state.ForcingHistory[index].Tick))
				return false;
		for (size_t index = 0; index < state.Cells.size(); ++index) {
			if (!ValidCell(state.Cells[index], state.Definition.MaximumVelocityMetresPerSecond)) return false;
			if (!Bounded(state.FallingCondensateKilogramsPerCubicMetre[index], 0, 100)) return false;
			const auto position = Coordinate(index, grid);
			const float height = state.GroundHeightMetres[position[0] + grid.Cells[0] * position[2]];
			const bool expectedSolid =
				height != NO_TERRAIN_HEIGHT &&
				(grid.Origin[1] + static_cast<float>(position[1]) + 0.5f) * grid.CellSizeMetres <= height;
			if (expectedSolid != ((SimulationCellFlags(state, index) & 1) != 0)) return false;
			if (expectedSolid) {
				const auto &cell = state.Cells[index];
				if (cell.VapourKilogramsPerKilogram != 0 || cell.CondensateKilogramsPerCubicMetre != 0 ||
					state.FallingCondensateKilogramsPerCubicMetre[index] != 0 ||
					cell.VelocityMetresPerSecond != std::array<float, 3>{})
					return false;
				continue;
			}
			for (size_t axis = 0; axis < 3; ++axis) {
				const auto high = Neighbor(index, axis, 1, state.Definition.Grid);
				if (high && !(SimulationCellFlags(state, *high) & 1) &&
					std::abs(
						state.Cells[*high].AtmosphericPressurePascals -
						state.Cells[index].AtmosphericPressurePascals
					) / state.Definition.Grid.CellSizeMetres >
						state.Definition.MaximumPressureGradientPascalsPerMetre)
					return false;
			}
		}
		const auto &budget = state.Budget;
		return std::isfinite(budget.InitialWaterKilograms) && budget.InitialWaterKilograms >= 0 &&
			   std::isfinite(budget.SourceWaterKilograms) && budget.SourceWaterKilograms >= 0 &&
			   std::isfinite(budget.BoundaryWaterKilograms) &&
			   std::isfinite(budget.PrecipitationWaterKilograms) && budget.PrecipitationWaterKilograms >= 0 &&
			   std::isfinite(budget.WaterResidualKilograms) && std::isfinite(budget.InitialEnthalpyJoules) &&
			   std::isfinite(budget.ExternalEnthalpyJoules) && std::isfinite(budget.EnthalpyResidualJoules);
	}

	double SimulationWaterKilograms(const SimulationState &state) {
		if (state.SolidMask.size() != (state.Cells.size() + 7) / 8 ||
			state.FallingCondensateKilogramsPerCubicMetre.size() != state.Cells.size())
			return std::numeric_limits<double>::quiet_NaN();
		double water = 0;
		const auto *mask = state.SolidMask.data();
		for (size_t index = 0; index < state.Cells.size(); ++index) {
			if (mask[index / 8] & (1u << (index & 7))) continue;
			const auto &cell = state.Cells[index];
			water += state.Definition.DryAirKilogramsPerCubicMetre *
						 static_cast<double>(cell.VapourKilogramsPerKilogram) +
					 cell.CondensateKilogramsPerCubicMetre +
					 state.FallingCondensateKilogramsPerCubicMetre[index];
		}
		return water * std::pow(static_cast<double>(state.Definition.Grid.CellSizeMetres), 3);
	}

	double SimulationEnthalpyJoules(const SimulationState &state) {
		if (state.SolidMask.size() != (state.Cells.size() + 7) / 8)
			return std::numeric_limits<double>::quiet_NaN();
		double enthalpy = 0;
		const auto *mask = state.SolidMask.data();
		for (size_t index = 0; index < state.Cells.size(); ++index) {
			if (mask[index / 8] & (1u << (index & 7))) continue;
			const auto &cell = state.Cells[index];
			enthalpy += state.Definition.HeatCapacityJoulesPerKilogramCelsius *
							(static_cast<double>(cell.TemperatureCelsius) + KELVIN_OFFSET) +
						state.Definition.LatentHeatJoulesPerKilogram *
							static_cast<double>(cell.VapourKilogramsPerKilogram);
		}
		return enthalpy * state.Definition.DryAirKilogramsPerCubicMetre *
			   std::pow(static_cast<double>(state.Definition.Grid.CellSizeMetres), 3);
	}

	std::optional<SimulationState> CreateSimulation(
		const SimulationDefinition &definition,
		const SolverCell &initial,
		const SimulationForcing &forcing,
		std::span<const float> groundHeightMetres
	) {
		if (!ValidDefinition(definition) || !ValidCell(initial, definition.MaximumVelocityMetresPerSecond) ||
			forcing.Tick != 0 || !ValidForcing(forcing, definition))
			return std::nullopt;
		try {
			SimulationState state;
			state.Definition = definition;
			state.Cells.assign(CellCount(definition.Grid), initial);
			const size_t columns = static_cast<size_t>(definition.Grid.Cells[0]) * definition.Grid.Cells[2];
			if (!groundHeightMetres.empty() && groundHeightMetres.size() != columns) return std::nullopt;
			state.GroundHeightMetres.assign(columns, NO_TERRAIN_HEIGHT);
			if (!groundHeightMetres.empty())
				std::copy(
					groundHeightMetres.begin(), groundHeightMetres.end(), state.GroundHeightMetres.begin()
				);
			for (float height : state.GroundHeightMetres)
				if (height != NO_TERRAIN_HEIGHT && !Bounded(height, -1e9f, 1e9f)) return std::nullopt;
			state.SolidMask.assign((state.Cells.size() + 7) / 8, 0);
			state.FallingCondensateKilogramsPerCubicMetre.assign(state.Cells.size(), 0);
			for (size_t index = 0; index < state.Cells.size(); ++index) {
				const auto position = Coordinate(index, definition.Grid);
				const float height =
					state.GroundHeightMetres[position[0] + definition.Grid.Cells[0] * position[2]];
				if (height != NO_TERRAIN_HEIGHT &&
					(definition.Grid.Origin[1] + static_cast<float>(position[1]) + 0.5f) *
							definition.Grid.CellSizeMetres <=
						height) {
					state.SolidMask[index / 8] |= static_cast<uint8_t>(1u << (index & 7));
					state.Cells[index].VapourKilogramsPerKilogram = 0;
					state.Cells[index].CondensateKilogramsPerCubicMetre = 0;
					state.Cells[index].VelocityMetresPerSecond = {};
					state.Cells[index].ProjectionPressurePascals = 0;
				}
			}
			state.ForcingHistory.push_back(forcing);
			state.Budget.InitialWaterKilograms = SimulationWaterKilograms(state);
			state.Budget.InitialEnthalpyJoules = SimulationEnthalpyJoules(state);
			return state;
		} catch (const std::bad_alloc &) {
			return std::nullopt;
		}
	}

	std::optional<SimulationDefinition>
	MakeSimulationDefinition(const scene::WeatherCompute &controls, std::string layerName) {
		if (!scene::ValidWeatherCompute(controls) || controls.Mode != scene::WeatherMode::Stateful ||
			!controls.Enabled || controls.Paused || controls.TimeScale != 1 ||
			!Bounded(controls.CellSizeMetres, 0.25f, 4096))
			return std::nullopt;
		SimulationDefinition definition;
		definition.LayerName = std::move(layerName);
		definition.Revision = controls.DefinitionRevision;
		definition.Seed = controls.Seed;
		definition.InitializationRecipe = {
			controls.AlgorithmVersion,
			controls.FeatureCells,
			std::bit_cast<uint32_t>(controls.Coverage),
			std::bit_cast<uint32_t>(controls.CondensateScale),
			std::bit_cast<uint32_t>(controls.VapourMixingRatio),
			std::bit_cast<uint32_t>(controls.VapourScale),
			std::bit_cast<uint32_t>(controls.BaseTemperatureCelsius),
			std::bit_cast<uint32_t>(controls.BasePressureHectopascals),
			std::bit_cast<uint32_t>(controls.TemperatureLapseCelsiusPerKilometre),
			std::bit_cast<uint32_t>(controls.TemperatureVariationCelsius),
			std::bit_cast<uint32_t>(controls.PressureVariationHectopascals),
			std::bit_cast<uint32_t>(controls.TurbulenceMetresPerSecond),
			std::bit_cast<uint32_t>(controls.ShearMetresPerSecond),
			std::bit_cast<uint32_t>(controls.LiftMetresPerSecond),
			std::bit_cast<uint32_t>(controls.FrontContrast),
			std::bit_cast<uint32_t>(controls.GrowthRate)
		};
		definition.TickRate = controls.TickRate;
		definition.TerrainRevision = controls.TerrainRevision;
		definition.GroundVapourFluxKilogramsPerSquareMetreSecond =
			controls.GroundVapourFluxKilogramsPerSquareMetreSecond;
		definition.RainFallSpeedMetresPerSecond = controls.RainFallSpeedMetresPerSecond;
		const auto boundary = [](scene::WeatherBoundary value) {
			return value == scene::WeatherBoundary::Periodic ? SolverBoundary::Periodic
				   : value == scene::WeatherBoundary::Closed ? SolverBoundary::Closed
															 : SolverBoundary::Open;
		};
		definition.Grid.Boundaries = {
			boundary(controls.BoundaryX),
			controls.TopBoundary == scene::WeatherTopBoundary::Escape ? SolverBoundary::GroundOpen
																	  : SolverBoundary::Closed,
			boundary(controls.BoundaryZ)
		};
		definition.Grid.CellSizeMetres = controls.CellSizeMetres;
		const std::array<float, 3> extents{
			controls.ExtentMetres.X, controls.ThicknessMetres, controls.ExtentMetres.Z
		};
		for (size_t axis = 0; axis < 3; ++axis) {
			const float cells = extents[axis] / controls.CellSizeMetres;
			if (!Bounded(cells, 2, 128) || std::floor(cells) != cells) return std::nullopt;
			definition.Grid.Cells[axis] = static_cast<uint32_t>(cells);
			definition.Grid.Origin[axis] = -static_cast<int32_t>(cells / 2);
		}
		const float base = controls.BaseHeightMetres / controls.CellSizeMetres;
		if (!Bounded(base, -1000000, 1000000) || std::floor(base) != base) return std::nullopt;
		definition.Grid.Origin[1] = static_cast<int32_t>(base);
		definition.ReferenceTemperatureCelsius = controls.BaseTemperatureCelsius;
		// Authored mm/hour becomes a relaxation rate at the declared reference column condensate.
		const float excessReference =
			controls.CondensateScale - definition.PrecipitationThresholdKilogramsPerCubicMetre;
		if (controls.PrecipitationRate > 0 && excessReference <= 0) return std::nullopt;
		definition.PrecipitationPerSecond =
			controls.PrecipitationRate > 0
				? controls.PrecipitationRate / (3600 * controls.ThicknessMetres * excessReference)
				: 0;
		return ValidDefinition(definition) ? std::optional(definition) : std::nullopt;
	}

	bool AddSimulationForcing(SimulationState &state, const SimulationForcing &forcing, bool replacePending) {
		if (!ValidateSimulation(state) || !ValidForcing(forcing, state.Definition) ||
			forcing.Tick < state.Tick || forcing.Tick < state.ForcingHistory.back().Tick)
			return false;
		if (forcing.Tick == state.ForcingHistory.back().Tick) {
			if (forcing.Tick != state.Tick && !replacePending) return false;
			state.ForcingHistory.back() = forcing;
			return true;
		}
		try {
			auto history = state.ForcingHistory;
			const auto active = std::upper_bound(
				history.begin(), history.end(), state.Tick, [](uint64_t tick, const auto &record) {
					return tick < record.Tick;
				}
			);
			if (active != history.begin()) history.erase(history.begin(), std::prev(active));
			if (history.size() >= MAXIMUM_FORCING) return false;
			history.push_back(forcing);
			state.ForcingHistory = std::move(history);
		} catch (const std::bad_alloc &) {
			return false;
		}
		return true;
	}

	SimulationStatus StepSimulation(SimulationState &state) {
		SimulationWorkspace workspace;
		return StepSimulation(state, workspace);
	}
	SimulationStatus StepSimulation(SimulationState &state, SimulationWorkspace &workspace) {
		ENGINE_PROFILE("weather solver tick");
		if (&state == &workspace.Trial || !ValidateSimulation(state) ||
			state.Tick == std::numeric_limits<uint64_t>::max())
			return SimulationStatus::Invalid;
		try {
			PrepareWorkspace(state, workspace);
			workspace.Trial = state;
			const auto status = Evolve(workspace.Trial, workspace);
			if (status == SimulationStatus::Success) std::swap(state, workspace.Trial);
			return status;
		} catch (const std::bad_alloc &) {
			return SimulationStatus::BudgetExceeded;
		}
	}

	SimulationStatus ReplaySimulation(
		const SimulationState &checkpoint,
		uint64_t targetTick,
		uint32_t maximumTicks,
		SimulationState &destination
	) {
		ENGINE_PROFILE("weather checkpoint replay");
		if (!ValidateSimulation(checkpoint) || targetTick < checkpoint.Tick) return SimulationStatus::Invalid;
		if (maximumTicks > 128 || targetTick - checkpoint.Tick > maximumTicks)
			return SimulationStatus::BudgetExceeded;
		try {
			SimulationState replay = checkpoint;
			SimulationWorkspace workspace;
			while (replay.Tick < targetTick) {
				const auto status = StepSimulation(replay, workspace);
				if (status != SimulationStatus::Success) return status;
			}
			destination = std::move(replay);
			return SimulationStatus::Success;
		} catch (const std::bad_alloc &) {
			return SimulationStatus::BudgetExceeded;
		}
	}

	std::optional<std::vector<EncodedCell>>
	PackSimulation(const SimulationState &state, const EncodingProfile &profile) {
		if (!ValidateSimulation(state) || !ValidProfile(profile)) return std::nullopt;
		const std::array<float, 3> windScale{
			profile.WindScaleMetresPerSecond.X,
			profile.WindScaleMetresPerSecond.Y,
			profile.WindScaleMetresPerSecond.Z
		};
		std::vector<EncodedCell> packed;
		packed.reserve(state.Cells.size());
		for (size_t index = 0; index < state.Cells.size(); ++index) {
			if (SimulationCellFlags(state, index) & 1) {
				packed.push_back(*Encode(CellValues{}));
				continue;
			}
			const auto &cell = state.Cells[index];
			const float altitude = (static_cast<float>(state.Definition.Grid.Origin[1]) +
									Coordinate(index, state.Definition.Grid)[1] + 0.5f) *
								   profile.CellSizeMetres;
			const auto unsignedCode = [](float normalized) {
				return static_cast<uint8_t>(std::lround(std::clamp(normalized, 0.0f, 1.0f) * 255));
			};
			const auto signedCode = [](float normalized) {
				return static_cast<int16_t>(std::lround(std::clamp(normalized, -1.0f, 1.0f) * 127));
			};
			CellValues codes;
			codes.Condensate = unsignedCode(
				cell.CondensateKilogramsPerCubicMetre / profile.CondensateKilogramsPerCubicMetre
			);
			codes.Vapour = unsignedCode(cell.VapourKilogramsPerKilogram / profile.VapourKilogramsPerKilogram);
			codes.TemperatureAnomaly = signedCode(
				(cell.TemperatureCelsius - profile.BaseTemperatureCelsius +
				 profile.TemperatureLapseCelsiusPerKilometre * altitude / 1000) /
				profile.TemperatureAnomalyCelsius
			);
			codes.PressureAnomaly = signedCode(
				(cell.AtmosphericPressurePascals / 100 - profile.BasePressureHectopascals) /
				profile.PressureAnomalyHectopascals
			);
			for (size_t axis = 0; axis < 3; ++axis)
				codes.Wind[axis] = signedCode(cell.VelocityMetresPerSecond[axis] / windScale[axis]);
			codes.Ice = unsignedCode(std::clamp(-cell.TemperatureCelsius / 20, 0.0f, 1.0f));
			packed.push_back(*Encode(codes));
		}
		return packed;
	}

	std::optional<uint32_t>
	SimulationSubsteps(const SimulationDefinition &definition, const SimulationForcing &forcing) {
		if (!ValidDefinition(definition) || !ValidForcing(forcing, definition)) return std::nullopt;
		const float seconds = 1.0f / static_cast<float>(definition.TickRate);
		float acceleration = 0;
		for (float component : forcing.AccelerationMetresPerSecondSquared)
			acceleration = std::max(acceleration, std::abs(component));
		acceleration +=
			definition.MaximumPressureGradientPascalsPerMetre / definition.DryAirKilogramsPerCubicMetre +
			200 * definition.BuoyancyMetresPerSecondSquaredPerCelsius +
			2 * definition.MaximumVelocityMetresPerSecond * forcing.WindRelaxationPerSecond;
		const float speed = 2 * (definition.MaximumVelocityMetresPerSecond + seconds * acceleration);
		const float stability =
			seconds *
			((3 * speed + definition.RainFallSpeedMetresPerSecond) / definition.Grid.CellSizeMetres +
			 6 * definition.DiffusionSquareMetresPerSecond /
				 (definition.Grid.CellSizeMetres * definition.Grid.CellSizeMetres) +
			 definition.PrecipitationPerSecond + forcing.WindRelaxationPerSecond);
		if (!std::isfinite(stability) || stability > 0.8f * static_cast<float>(definition.MaximumSubsteps))
			return std::nullopt;
		return std::max(1u, static_cast<uint32_t>(std::ceil(stability / 0.8f)));
	}
}
