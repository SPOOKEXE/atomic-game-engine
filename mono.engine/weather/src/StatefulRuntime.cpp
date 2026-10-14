#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/WeatherClock.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

namespace engine::weather {
	namespace {
		constexpr uint32_t MAXIMUM_CATCHUP_TICKS = 32;
		constexpr uint32_t CHECKPOINT_INTERVAL_SECONDS = 5;
		constexpr uint32_t COLUMN_TEMPERATURE_SAMPLES = 16;
		constexpr float MELTING_DEGREE_METRES = 1000;
		constexpr float FOG_HEIGHT_METRES = 64;
		constexpr float SURFACE_COOLING_CELSIUS = 3;

		std::optional<scene::WeatherClock> NextPhysicsEpoch(const scene::WeatherClock &clock) {
			const auto time = WeatherTimeOf(clock);
			if (!time || clock.Epoch == UINT64_MAX) return std::nullopt;
			auto next = clock;
			++next.Epoch;
			next.Sequence = clock.Sequence == UINT64_MAX ? 1 : clock.Sequence + 1;
			next.AnchorTick = clock.Tick;
			next.PhaseTick = time->PhaseTick;
			next.PhaseFractionQ16 = time->PhaseFractionQ16;
			next.DisplacementQ8 = time->DisplacementQ8;
			next.DisplacementRemainderQ12 = time->DisplacementRemainderQ12;
			return next;
		}

		bool BeginRetiredPhysicsEpoch(ecs::Store &store, ecs::Entity provider) {
			const auto *clock = store.Get<scene::WeatherClock>(provider);
			if (!clock) return true;
			auto next = NextPhysicsEpoch(*clock);
			const auto epoch = next
								   ? ClaimWeatherEpoch(store, WeatherLayerName(store, provider), clock->Epoch)
								   : std::optional<uint64_t>{};
			if (!epoch) return false;
			next->Epoch = *epoch;
			*store.GetMutable<scene::WeatherClock>(provider) = *next;
			store.MarkChanged<scene::WeatherClock>(provider);
			return true;
		}

		bool CompatibleDefinition(
			const SimulationDefinition &definition,
			const scene::WeatherCompute &controls,
			std::string layerName,
			const scene::WeatherClock *clock,
			bool ignoreRevision = false
		) {
			auto normalized = controls;
			normalized.Enabled = true;
			normalized.Paused = false;
			normalized.TimeScale = 1;
			auto expected = MakeSimulationDefinition(normalized, std::move(layerName));
			if (!expected) return false;
			expected->WorldEpoch = clock ? clock->Epoch : definition.WorldEpoch;
			if (ignoreRevision) expected->Revision = definition.Revision;
			return *expected == definition;
		}

		void IndexCheckpoint(const StatefulWeatherCheckpoint &checkpoint, const SimulationState &validated) {
			core::ByteReader digestReader(std::span<const std::byte>(checkpoint.Bytes).last(8));
			checkpoint.Queries = StatefulWeatherCheckpoint::QueryIndex{
				validated.Definition,
				digestReader.ReadUInt64(),
				validated.Tick,
				0,
				static_cast<uint32_t>(
					checkpoint.Bytes.size() - 8 - validated.Cells.size() * sizeof(SolverCell)
				),
				static_cast<uint32_t>(
					checkpoint.Bytes.size() - 8 -
					validated.Cells.size() * (sizeof(SolverCell) + sizeof(float)) - 4
				),
				validated.SolidMask,
				validated.GroundHeightMetres
			};
		}

		bool EnsureCheckpointIndex(const StatefulWeatherCheckpoint &checkpoint, uint64_t writeVersion) {
			if (checkpoint.Bytes.size() < 16 || checkpoint.Bytes.size() > CheckpointLimits{}.MaximumBytes)
				return false;
			core::ByteReader digestReader(std::span<const std::byte>(checkpoint.Bytes).last(8));
			const uint64_t digest = digestReader.ReadUInt64();
			if (checkpoint.Queries && checkpoint.Queries->Digest == digest &&
				checkpoint.Queries->Tick == checkpoint.Tick &&
				checkpoint.Queries->SourceWriteVersion == writeVersion &&
				checkpoint.Queries->Definition.WorldEpoch == checkpoint.Epoch &&
				checkpoint.Queries->Definition.Revision == checkpoint.Revision)
				return true;
			SimulationState validated;
			if (!DecodeCheckpoint(checkpoint.Bytes, validated) || validated.Tick != checkpoint.Tick ||
				validated.Definition.WorldEpoch != checkpoint.Epoch ||
				validated.Definition.Revision != checkpoint.Revision)
				return false;
			IndexCheckpoint(checkpoint, validated);
			checkpoint.Queries->SourceWriteVersion = writeVersion;
			return true;
		}

		SolverCell IndexedCell(const StatefulWeatherCheckpoint &checkpoint, size_t index) {
			core::ByteReader reader(
				std::span<const std::byte>(checkpoint.Bytes)
					.subspan(checkpoint.Queries->CellOffset + index * sizeof(SolverCell), sizeof(SolverCell))
			);
			SolverCell cell;
			cell.VapourKilogramsPerKilogram = reader.ReadFloat();
			cell.CondensateKilogramsPerCubicMetre = reader.ReadFloat();
			cell.TemperatureCelsius = reader.ReadFloat();
			cell.AtmosphericPressurePascals = reader.ReadFloat();
			for (float &velocity : cell.VelocityMetresPerSecond)
				velocity = reader.ReadFloat();
			cell.ProjectionPressurePascals = reader.ReadFloat();
			return cell;
		}

		void WritePhysical(core::ByteWriter &writer, const PhysicalSample &sample) {
			writer.WriteFloat(sample.CondensateKilogramsPerCubicMetre);
			writer.WriteFloat(sample.VapourKilogramsPerKilogram);
			writer.WriteFloat(sample.TemperatureCelsius);
			writer.WriteFloat(sample.PressureHectopascals);
			writer.WriteFloat(sample.WindMetresPerSecond.X);
			writer.WriteFloat(sample.WindMetresPerSecond.Y);
			writer.WriteFloat(sample.WindMetresPerSecond.Z);
			writer.WriteFloat(sample.IceFraction);
		}

		void ReadPhysical(core::ByteReader &reader, PhysicalSample &sample) {
			sample.CondensateKilogramsPerCubicMetre = reader.ReadFloat();
			sample.VapourKilogramsPerKilogram = reader.ReadFloat();
			sample.TemperatureCelsius = reader.ReadFloat();
			sample.PressureHectopascals = reader.ReadFloat();
			sample.WindMetresPerSecond = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
			sample.IceFraction = reader.ReadFloat();
		}

		void WriteTickSample(core::ByteWriter &writer, const StatefulTickSample &sample) {
			writer.WriteUInt64(sample.Tick);
			writer.WriteFloat(sample.PositionMetres.X);
			writer.WriteFloat(sample.PositionMetres.Y);
			writer.WriteFloat(sample.PositionMetres.Z);
			WritePhysical(writer, sample.Sample.Air);
			writer.WriteFloat(sample.Sample.Precipitation.LiquidMillimetresPerHour);
			writer.WriteFloat(sample.Sample.Precipitation.SnowMillimetresPerHour);
			writer.WriteFloat(sample.Sample.Precipitation.HailMillimetresPerHour);
			writer.WriteFloat(sample.Sample.Precipitation.MeltingFraction);
			writer.WriteUInt32(sample.Sample.Precipitation.SpawnSeed);
			writer.WriteFloat(sample.Sample.FogExtinctionPerMetre);
			writer.WriteFloat(sample.Sample.ChargeSeparation);
			writer.WriteFloat(sample.Sample.HailGrowth);
		}

		bool ReadTickSample(core::ByteReader &reader, StatefulTickSample &sample) {
			sample.Tick = reader.ReadUInt64();
			sample.PositionMetres = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
			ReadPhysical(reader, sample.Sample.Air);
			sample.Sample.Precipitation.LiquidMillimetresPerHour = reader.ReadFloat();
			sample.Sample.Precipitation.SnowMillimetresPerHour = reader.ReadFloat();
			sample.Sample.Precipitation.HailMillimetresPerHour = reader.ReadFloat();
			sample.Sample.Precipitation.MeltingFraction = reader.ReadFloat();
			sample.Sample.Precipitation.SpawnSeed = reader.ReadUInt32();
			sample.Sample.FogExtinctionPerMetre = reader.ReadFloat();
			sample.Sample.ChargeSeparation = reader.ReadFloat();
			sample.Sample.HailGrowth = reader.ReadFloat();
			const float scalars[]{
				sample.PositionMetres.X,
				sample.PositionMetres.Y,
				sample.PositionMetres.Z,
				sample.Sample.Air.CondensateKilogramsPerCubicMetre,
				sample.Sample.Air.VapourKilogramsPerKilogram,
				sample.Sample.Air.TemperatureCelsius,
				sample.Sample.Air.PressureHectopascals,
				sample.Sample.Air.WindMetresPerSecond.X,
				sample.Sample.Air.WindMetresPerSecond.Y,
				sample.Sample.Air.WindMetresPerSecond.Z,
				sample.Sample.Air.IceFraction,
				sample.Sample.Precipitation.LiquidMillimetresPerHour,
				sample.Sample.Precipitation.SnowMillimetresPerHour,
				sample.Sample.Precipitation.HailMillimetresPerHour,
				sample.Sample.Precipitation.MeltingFraction,
				sample.Sample.FogExtinctionPerMetre,
				sample.Sample.ChargeSeparation,
				sample.Sample.HailGrowth
			};
			for (float scalar : scalars)
				if (!std::isfinite(scalar)) return false;
			return !reader.Failed() && sample.Sample.Air.VapourKilogramsPerKilogram >= 0 &&
				   sample.Sample.Air.CondensateKilogramsPerCubicMetre >= 0 &&
				   sample.Sample.ChargeSeparation >= 0 && sample.Sample.ChargeSeparation <= 1 &&
				   sample.Sample.HailGrowth >= 0 && sample.Sample.HailGrowth <= 1;
		}

		void WriteCheckpoint(core::ByteWriter &writer, const StatefulWeatherCheckpoint &checkpoint) {
			writer.WriteUInt32(1);
			writer.WriteUInt64(checkpoint.Tick);
			writer.WriteUInt64(checkpoint.Epoch);
			writer.WriteUInt32(checkpoint.Revision);
			writer.WriteUInt32(static_cast<uint32_t>(checkpoint.Bytes.size()));
			writer.WriteRaw(checkpoint.Bytes.data(), checkpoint.Bytes.size());
		}

		bool ReadCheckpoint(core::ByteReader &reader, StatefulWeatherCheckpoint &checkpoint) {
			if (reader.ReadUInt32() != 1) {
				reader.Fail();
				return false;
			}
			checkpoint.Tick = reader.ReadUInt64();
			checkpoint.Epoch = reader.ReadUInt64();
			checkpoint.Revision = reader.ReadUInt32();
			const uint32_t length = reader.ReadUInt32();
			if (length > CheckpointLimits{}.MaximumBytes) {
				reader.Fail();
				return false;
			}
			const auto bytes = reader.ReadRawView(length);
			SimulationState validated;
			if (reader.Failed() || !DecodeCheckpoint(bytes, validated) || validated.Tick != checkpoint.Tick ||
				validated.Definition.WorldEpoch != checkpoint.Epoch ||
				validated.Definition.Revision != checkpoint.Revision) {
				reader.Fail();
				return false;
			}
			checkpoint.Bytes.assign(bytes.begin(), bytes.end());
			IndexCheckpoint(checkpoint, validated);
			return true;
		}

		void WriteStates(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *states = static_cast<const StatefulWeatherState *>(source);
			for (size_t index = 0; index < count; ++index) {
				const auto &state = states[index];
				writer.WriteUInt32(2);
				const auto checkpoint = EncodeCheckpoint(state.Simulation);
				if (!checkpoint) throw std::runtime_error("invalid stateful weather save");
				WriteCheckpoint(
					writer,
					{state.Simulation.Tick,
					 state.Simulation.Definition.WorldEpoch,
					 state.Simulation.Definition.Revision,
					 *checkpoint}
				);
				writer.WriteUInt32(state.Seed);
				writer.WriteUInt32(state.LayerKey);
				writer.WriteUInt32(state.AcceptedRevision);
				writer.WriteUInt32(static_cast<uint32_t>(state.TickSamples.size()));
				for (const auto &sample : state.TickSamples)
					WriteTickSample(writer, sample);
			}
		}

		void ReadStates(core::ByteReader &reader, void *destination, size_t count) {
			if (count > 32) {
				reader.Fail();
				return;
			}
			std::vector<StatefulWeatherState> decoded(count);
			for (auto &state : decoded) {
				if (reader.ReadUInt32() != 2) {
					reader.Fail();
					return;
				}
				StatefulWeatherCheckpoint checkpoint;
				if (!ReadCheckpoint(reader, checkpoint) ||
					!DecodeCheckpoint(checkpoint.Bytes, state.Simulation))
					return;
				state.Seed = reader.ReadUInt32();
				state.LayerKey = reader.ReadUInt32();
				state.AcceptedRevision = reader.ReadUInt32();
				const uint32_t samples = reader.ReadUInt32();
				if (samples > MAXIMUM_CATCHUP_TICKS || state.Seed != state.Simulation.Definition.Seed ||
					state.AcceptedRevision != state.Simulation.Definition.Revision) {
					reader.Fail();
					return;
				}
				state.TickSamples.resize(samples);
				for (size_t index = 0; index < samples; ++index)
					if (!ReadTickSample(reader, state.TickSamples[index]) ||
						state.TickSamples[index].Tick > state.Simulation.Tick ||
						(index && state.TickSamples[index - 1].Tick >= state.TickSamples[index].Tick)) {
						reader.Fail();
						return;
					}
			}
			if (reader.Failed()) return;
			auto *states = static_cast<StatefulWeatherState *>(destination);
			for (size_t index = 0; index < count; ++index)
				states[index] = std::move(decoded[index]);
		}

		void WriteCheckpoints(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *checkpoints = static_cast<const StatefulWeatherCheckpoint *>(source);
			for (size_t index = 0; index < count; ++index)
				WriteCheckpoint(writer, checkpoints[index]);
		}

		void ReadCheckpoints(core::ByteReader &reader, void *destination, size_t count) {
			if (count > 32) {
				reader.Fail();
				return;
			}
			std::vector<StatefulWeatherCheckpoint> decoded(count);
			for (auto &checkpoint : decoded)
				if (!ReadCheckpoint(reader, checkpoint)) return;
			auto *checkpoints = static_cast<StatefulWeatherCheckpoint *>(destination);
			for (size_t index = 0; index < count; ++index)
				checkpoints[index] = std::move(decoded[index]);
		}

		void WriteInitializationFailures(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *failures = static_cast<const StatefulWeatherInitializationFailure *>(source);
			for (size_t index = 0; index < count; ++index) {
				writer.WriteUInt32(1);
				writer.WriteUInt32(failures[index].DefinitionRevision);
			}
		}

		void ReadInitializationFailures(core::ByteReader &reader, void *destination, size_t count) {
			if (count > 32) {
				reader.Fail();
				return;
			}
			std::array<StatefulWeatherInitializationFailure, 32> decoded{};
			for (size_t index = 0; index < count; ++index) {
				if (reader.ReadUInt32() != 1) {
					reader.Fail();
					return;
				}
				decoded[index].DefinitionRevision = reader.ReadUInt32();
				if (reader.Failed() || !decoded[index].DefinitionRevision) {
					reader.Fail();
					return;
				}
			}
			std::copy_n(
				decoded.begin(), count, static_cast<StatefulWeatherInitializationFailure *>(destination)
			);
		}

		template <class CellReader, class RainReader>
		std::optional<ConsumerSample> SamplePhysicalField(
			const SimulationDefinition &definition,
			uint64_t tick,
			uint32_t seed,
			uint32_t layerKey,
			const CellReader &cellAt,
			const RainReader &rainAt,
			std::span<const uint8_t> solidMask,
			std::span<const float> groundHeights,
			const scene::WeatherCompute &controls,
			core::Vector3 position,
			float groundHeight
		) {
			const auto &grid = definition.Grid;
			const float coordinates[]{position.X, position.Y, position.Z};
			std::array<uint32_t, 3> local{};
			bool insideVertical = true;
			for (size_t axis = 0; axis < 3; ++axis) {
				if (!std::isfinite(coordinates[axis]) || std::abs(coordinates[axis]) > 1e9f ||
					!std::isfinite(groundHeight))
					return std::nullopt;
				const double lattice =
					std::floor(static_cast<double>(coordinates[axis]) / grid.CellSizeMetres) -
					grid.Origin[axis];
				if (axis == 1) {
					insideVertical = lattice >= 0 && lattice < grid.Cells[axis];
					local[axis] = static_cast<uint32_t>(
						std::clamp(lattice, 0.0, static_cast<double>(grid.Cells[axis] - 1))
					);
				} else if (grid.Boundaries[axis] == SolverBoundary::Periodic) {
					const auto integral = static_cast<int64_t>(lattice),
							   extent = static_cast<int64_t>(grid.Cells[axis]);
					local[axis] = static_cast<uint32_t>((integral % extent + extent) % extent);
				} else {
					if (lattice < 0 || lattice >= grid.Cells[axis]) return std::nullopt;
					local[axis] = static_cast<uint32_t>(lattice);
				}
			}
			const float terrainHeight = groundHeights[local[0] + grid.Cells[0] * local[2]];
			if (terrainHeight != NO_TERRAIN_HEIGHT) {
				groundHeight = terrainHeight;
				if (position.Y < terrainHeight) return std::nullopt;
			}
			const auto solid = [&](size_t cellIndex) {
				return (solidMask[cellIndex / 8] & (1u << (cellIndex & 7))) != 0;
			};
			size_t index = local[0] + grid.Cells[0] * (local[1] + grid.Cells[1] * local[2]);
			while (solid(index) && local[1] + 1 < grid.Cells[1]) {
				++local[1];
				index += grid.Cells[0];
			}
			if (solid(index)) return std::nullopt;
			const auto &cell = cellAt(index);
			ConsumerSample sample;
			sample.Air.CondensateKilogramsPerCubicMetre =
				insideVertical ? cell.CondensateKilogramsPerCubicMetre : 0;
			sample.Air.VapourKilogramsPerKilogram = cell.VapourKilogramsPerKilogram;
			const float sampledHeight =
				(grid.Origin[1] + static_cast<float>(local[1]) + 0.5f) * grid.CellSizeMetres;
			// Outside the simulated layer, consumers use an explicit lapse continuation, not invented cell
			// history.
			sample.Air.TemperatureCelsius =
				cell.TemperatureCelsius - (insideVertical ? 0
														  : controls.TemperatureLapseCelsiusPerKilometre *
																(position.Y - sampledHeight) / 1000);
			sample.Air.PressureHectopascals = cell.AtmosphericPressurePascals / 100;
			sample.Air.WindMetresPerSecond = {
				cell.VelocityMetresPerSecond[0],
				cell.VelocityMetresPerSecond[1],
				cell.VelocityMetresPerSecond[2]
			};
			sample.Air.IceFraction = std::clamp(-sample.Air.TemperatureCelsius / 20, 0.0f, 1.0f);
			float precipitation = 0, frozen = 0, charge = 0, hail = 0;
			for (uint32_t height = 0; height < grid.Cells[1]; ++height) {
				const auto &column = cellAt(local[0] + grid.Cells[0] * (height + grid.Cells[1] * local[2]));
				if (solid(local[0] + grid.Cells[0] * (height + grid.Cells[1] * local[2]))) continue;
				const float ice = std::clamp(-column.TemperatureCelsius / 20, 0.0f, 1.0f);
				const float updraft = std::clamp(column.VelocityMetresPerSecond[1] / 10, 0.0f, 1.0f);
				const float wet = std::clamp(column.CondensateKilogramsPerCubicMetre / 0.003f, 0.0f, 1.0f);
				charge = std::max(charge, wet * updraft * (0.25f + 0.75f * ice) * controls.StormIntensity);
				hail = std::max(hail, wet * updraft * ice * controls.HailIntensity);
			}
			// Completed falling-water concentration determines precipitation flux; cloud conversion is not
			// a ground sink. The closed ground face has no air velocity.
			const bool surface = local[1] == 0 || solid(index - grid.Cells[0]);
			precipitation = rainAt(index) *
							std::max(
								0.0f,
								definition.RainFallSpeedMetresPerSecond -
									(surface ? 0 : cellAt(index - grid.Cells[0]).VelocityMetresPerSecond[1])
							) *
							3600;
			frozen = precipitation * sample.Air.IceFraction;
			const float topHeight =
				(grid.Origin[1] + static_cast<float>(grid.Cells[1])) * grid.CellSizeMetres;
			const float columnLength = std::max(0.0f, topHeight - position.Y);
			float warmDegreeMetres = 0;
			for (uint32_t heightSample = 0; heightSample < COLUMN_TEMPERATURE_SAMPLES; ++heightSample) {
				const float altitude = position.Y + columnLength * (static_cast<float>(heightSample) + 0.5f) /
														COLUMN_TEMPERATURE_SAMPLES;
				const float lattice = std::floor(altitude / grid.CellSizeMetres) - grid.Origin[1];
				const uint32_t height = static_cast<uint32_t>(
					std::clamp(lattice, static_cast<float>(local[1]), static_cast<float>(grid.Cells[1] - 1))
				);
				const auto &column = cellAt(local[0] + grid.Cells[0] * (height + grid.Cells[1] * local[2]));
				const float cellHeight =
					(grid.Origin[1] + static_cast<float>(height) + 0.5f) * grid.CellSizeMetres;
				const float temperature =
					column.TemperatureCelsius -
					(lattice < 0 || lattice >= grid.Cells[1]
						 ? controls.TemperatureLapseCelsiusPerKilometre * (altitude - cellHeight) / 1000
						 : 0);
				warmDegreeMetres += std::max(0.0f, temperature) * columnLength / COLUMN_TEMPERATURE_SAMPLES;
			}
			sample.Precipitation.MeltingFraction = 1 - std::exp(-warmDegreeMetres / MELTING_DEGREE_METRES);
			hail *= 1 - sample.Precipitation.MeltingFraction;
			const float frozenAtQuery = frozen * (1 - sample.Precipitation.MeltingFraction);
			const float hailRate = std::min(frozenAtQuery, precipitation * hail);
			const float snow = std::max(0.0f, frozenAtQuery - hailRate);
			sample.Precipitation.SnowMillimetresPerHour = snow;
			sample.Precipitation.HailMillimetresPerHour = hailRate;
			sample.Precipitation.LiquidMillimetresPerHour = precipitation - snow - hailRate;
			sample.Precipitation.SpawnSeed = Hash(
				seed ^ layerKey ^ Hash(static_cast<uint32_t>(tick)) ^ Hash(static_cast<uint32_t>(tick >> 32))
			);
			if (position.Y < groundHeight || position.Y >= topHeight) sample.Precipitation = {};
			const float groundDistance = position.Y - groundHeight;
			if (groundDistance >= 0 && groundDistance <= FOG_HEIGHT_METRES) {
				const float suspension = 1 - groundDistance / FOG_HEIGHT_METRES;
				const auto saturation = SaturationMixingRatio(
					sample.Air.TemperatureCelsius - SURFACE_COOLING_CELSIUS * suspension,
					sample.Air.PressureHectopascals
				);
				if (saturation) {
					const float excess = std::max(0.0f, sample.Air.VapourKilogramsPerKilogram - *saturation);
					sample.FogExtinctionPerMetre =
						ExtinctionPerMetre(excess * definition.DryAirKilogramsPerCubicMetre) *
						controls.FogStrength * suspension;
				}
			}
			sample.ChargeSeparation = std::clamp(charge, 0.0f, 1.0f);
			sample.HailGrowth = std::clamp(hail, 0.0f, 1.0f);
			return sample;
		}

		std::optional<ConsumerSample> SampleState(
			const StatefulWeatherState &state,
			const scene::WeatherCompute &controls,
			core::Vector3 position,
			float groundHeight
		) {
			return SamplePhysicalField(
				state.Simulation.Definition,
				state.Simulation.Tick,
				state.Seed,
				state.LayerKey,
				[&](size_t index) -> const SolverCell & { return state.Simulation.Cells[index]; },
				[&](size_t index) { return state.Simulation.FallingCondensateKilogramsPerCubicMetre[index]; },
				state.Simulation.SolidMask,
				state.Simulation.GroundHeightMetres,
				controls,
				position,
				groundHeight
			);
		}

		std::optional<StatefulWeatherState> InitialState(
			ecs::Store &store,
			ecs::Entity provider,
			const scene::WeatherCompute &controls,
			const scene::WeatherClock *initialClock = nullptr
		) try {
			auto accepted = controls;
			accepted.Paused = false;
			accepted.TimeScale = 1;
			const std::string layerName = WeatherLayerName(store, provider);
			auto definition = MakeSimulationDefinition(accepted, layerName);
			if (!definition) return std::nullopt;
			if (const auto *clock = store.Get<scene::WeatherClock>(provider))
				definition->WorldEpoch = clock->Epoch;
			if (initialClock) definition->WorldEpoch = initialClock->Epoch;
			accepted.Mode = scene::WeatherMode::Procedural;
			const auto compiled = Compile(accepted, WeatherLayerKey(controls.LayerName.Text()));
			if (!compiled) return std::nullopt;
			SolverCell uniform;
			uniform.TemperatureCelsius = controls.BaseTemperatureCelsius;
			uniform.AtmosphericPressurePascals = controls.BasePressureHectopascals * 100;
			uniform.VapourKilogramsPerKilogram = controls.VapourMixingRatio;
			SimulationForcing forcing;
			forcing.Inflow = uniform;
			forcing.WindTargetMetresPerSecond = {
				controls.WindMetresPerSecond.X, controls.WindMetresPerSecond.Y, controls.WindMetresPerSecond.Z
			};
			forcing.WindRelaxationPerSecond = 1;
			for (size_t axis = 0; axis < 3; ++axis)
				if (definition->Grid.Boundaries[axis] == SolverBoundary::Open ||
					definition->Grid.Boundaries[axis] == SolverBoundary::GroundOpen)
					forcing.OpenVelocityMetresPerSecond[axis] = forcing.WindTargetMetresPerSecond[axis];
			std::vector<float> terrain(
				static_cast<size_t>(definition->Grid.Cells[0]) * definition->Grid.Cells[2], NO_TERRAIN_HEIGHT
			);
			if (const auto *query = store.Resource<WeatherGroundQuery>(); query && query->Height) {
				const float rayHeight = std::max(
					1000000.0f,
					(definition->Grid.Origin[1] + static_cast<float>(definition->Grid.Cells[1]) + 1) *
						definition->Grid.CellSizeMetres
				);
				const float bottom =
					std::min(-1000000.0f, (definition->Grid.Origin[1] - 1) * definition->Grid.CellSizeMetres);
				for (uint32_t z = 0; z < definition->Grid.Cells[2]; ++z)
					for (uint32_t x = 0; x < definition->Grid.Cells[0]; ++x) {
						const auto height = query->Height(
							store,
							{(definition->Grid.Origin[0] + static_cast<float>(x) + 0.5f) *
								 definition->Grid.CellSizeMetres,
							 rayHeight,
							 (definition->Grid.Origin[2] + static_cast<float>(z) + 0.5f) *
								 definition->Grid.CellSizeMetres},
							rayHeight - bottom
						);
						if (height) {
							if (!std::isfinite(*height) || std::abs(*height) > 1e9f) return std::nullopt;
							terrain[x + definition->Grid.Cells[0] * z] = *height;
						}
					}
			}
			auto simulation = CreateSimulation(*definition, uniform, forcing, terrain);
			if (!simulation) return std::nullopt;
			const auto initialTime = initialClock ? WeatherTimeOf(*initialClock)
												  : ResolveTime(InitialAnchor(compiled->Definition), 0);
			if (!initialTime) return std::nullopt;
			simulation->Tick = initialTime->PhaseTick;
			simulation->ForcingHistory.front().Tick = simulation->Tick;
			for (size_t index = 0; index < simulation->Cells.size(); ++index) {
				if (SimulationCellFlags(*simulation, index) & 1) continue;
				CellCoordinate coordinate{
					definition->Grid.Origin[0] + static_cast<int32_t>(index % definition->Grid.Cells[0]),
					definition->Grid.Origin[1] +
						static_cast<int32_t>((index / definition->Grid.Cells[0]) % definition->Grid.Cells[1]),
					definition->Grid.Origin[2] +
						static_cast<int32_t>(index / (definition->Grid.Cells[0] * definition->Grid.Cells[1]))
				};
				const auto physical = ToPhysical(
					Evaluate(compiled->Definition, *initialTime, coordinate),
					compiled->Profile,
					(coordinate.Y + 0.5f) * definition->Grid.CellSizeMetres
				);
				if (!physical) return std::nullopt;
				auto &cell = simulation->Cells[index];
				cell.VapourKilogramsPerKilogram = physical->VapourKilogramsPerKilogram;
				cell.CondensateKilogramsPerCubicMetre = physical->CondensateKilogramsPerCubicMetre;
				cell.TemperatureCelsius = physical->TemperatureCelsius;
				cell.AtmosphericPressurePascals = physical->PressureHectopascals * 100;
				cell.VelocityMetresPerSecond = {
					physical->WindMetresPerSecond.X,
					physical->WindMetresPerSecond.Y,
					physical->WindMetresPerSecond.Z
				};
			}
			simulation->Budget.InitialWaterKilograms = SimulationWaterKilograms(*simulation);
			simulation->Budget.InitialEnthalpyJoules = SimulationEnthalpyJoules(*simulation);
			if (!ValidateSimulation(*simulation)) return std::nullopt;
			StatefulWeatherState state;
			state.Simulation = std::move(*simulation);
			state.Seed = controls.Seed;
			state.LayerKey = compiled->Definition.LayerKey;
			state.AcceptedRevision = controls.DefinitionRevision;
			state.TickSamples.reserve(MAXIMUM_CATCHUP_TICKS);
			const auto column = StatefulEventColumn(state, state.Simulation.Tick);
			const auto sample = SampleState(state, controls, column, 0);
			if (sample) state.TickSamples.push_back({state.Simulation.Tick, column, *sample});
			return state;
		} catch (const std::bad_alloc &) {
			return std::nullopt;
		}

		bool ClaimInitializedEpoch(
			ecs::Store &store,
			ecs::Entity provider,
			scene::WeatherClock &clock,
			StatefulWeatherState &state,
			const scene::WeatherCompute &controls
		) {
			const auto epoch = ClaimWeatherEpoch(store, WeatherLayerName(store, provider), clock.Epoch - 1);
			if (!epoch) return false;
			clock.Epoch = *epoch;
			state.Simulation.Definition.WorldEpoch = *epoch;
			state.TickSamples.clear();
			const auto column = StatefulEventColumn(state, state.Simulation.Tick);
			const auto sample = SampleState(state, controls, column, 0);
			if (sample) state.TickSamples.push_back({state.Simulation.Tick, column, *sample});
			return true;
		}
	}

	void RegisterStatefulWeatherComponents() {
		ecs::Components::Register<StatefulWeatherInitializationFailure>(
			"weather.StatefulInitializationFailure",
			WriteInitializationFailures,
			ReadInitializationFailures,
			8
		);
		ecs::Components::Register<StatefulWeatherState>(
			"weather.StatefulState", WriteStates, ReadStates, 32 * 1024 * 1024
		);
		ecs::Components::Register<StatefulWeatherCheckpoint>(
			"weather.StatefulCheckpoint", WriteCheckpoints, ReadCheckpoints, 32 * 1024 * 1024
		);
	}

	core::Vector3 StatefulEventColumn(const StatefulWeatherState &state, uint64_t tick) {
		const auto &grid = state.Simulation.Definition.Grid;
		auto column = StormColumn(
			state.Seed,
			state.LayerKey,
			tick,
			state.Simulation.Definition.WorldEpoch,
			{grid.Cells[0] * grid.CellSizeMetres,
			 grid.Cells[1] * grid.CellSizeMetres,
			 grid.Cells[2] * grid.CellSizeMetres}
		);
		column.X += (grid.Origin[0] + 0.5f * grid.Cells[0]) * grid.CellSizeMetres;
		column.Z += (grid.Origin[2] + 0.5f * grid.Cells[2]) * grid.CellSizeMetres;
		// The signed Q16 sample includes its positive endpoint; canonical domain bounds do not.
		const float lowerX = grid.Origin[0] * grid.CellSizeMetres;
		const float lowerZ = grid.Origin[2] * grid.CellSizeMetres;
		const float upperX = (grid.Origin[0] + static_cast<float>(grid.Cells[0])) * grid.CellSizeMetres;
		const float upperZ = (grid.Origin[2] + static_cast<float>(grid.Cells[2])) * grid.CellSizeMetres;
		column.X = std::clamp(column.X, lowerX, std::nextafter(upperX, lowerX));
		column.Z = std::clamp(column.Z, lowerZ, std::nextafter(upperZ, lowerZ));
		const uint32_t x = static_cast<uint32_t>(std::clamp(
			std::floor(column.X / grid.CellSizeMetres) - grid.Origin[0],
			0.0f,
			static_cast<float>(grid.Cells[0] - 1)
		));
		const uint32_t z = static_cast<uint32_t>(std::clamp(
			std::floor(column.Z / grid.CellSizeMetres) - grid.Origin[2],
			0.0f,
			static_cast<float>(grid.Cells[2] - 1)
		));
		const float height = state.Simulation.GroundHeightMetres[x + grid.Cells[0] * z];
		if (height != NO_TERRAIN_HEIGHT) column.Y = std::max(column.Y, height);
		return column;
	}

	std::optional<uint64_t> StatefulWeatherTick(const ecs::Store &store, ecs::Entity provider) {
		const auto *state = store.Get<StatefulWeatherState>(provider);
		return state ? std::optional(state->Simulation.Tick) : std::nullopt;
	}

	bool StatefulEventColumnBlocked(const StatefulWeatherState &state, core::Vector3 column) {
		const auto &simulation = state.Simulation;
		const auto &grid = simulation.Definition.Grid;
		if (!std::isfinite(column.X) || !std::isfinite(column.Z) || grid.CellSizeMetres <= 0 ||
			grid.Cells[0] == 0 || grid.Cells[1] == 0 || grid.Cells[2] == 0 ||
			simulation.SolidMask.size() != (simulation.Cells.size() + 7) / 8)
			return false;
		const double x = std::floor(static_cast<double>(column.X) / grid.CellSizeMetres) - grid.Origin[0];
		const double z = std::floor(static_cast<double>(column.Z) / grid.CellSizeMetres) - grid.Origin[2];
		if (x < 0 || x >= grid.Cells[0] || z < 0 || z >= grid.Cells[2]) return false;
		for (uint32_t y = 0; y < grid.Cells[1]; ++y) {
			const size_t index =
				static_cast<size_t>(x) + grid.Cells[0] * (y + grid.Cells[1] * static_cast<size_t>(z));
			if (index >= simulation.Cells.size() || !(simulation.SolidMask[index / 8] & (1u << (index & 7))))
				return false;
		}
		return true;
	}

	std::optional<ConsumerSample> QueryStatefulWeather(
		ecs::Store &store,
		ecs::Entity provider,
		core::Vector3 positionMetres,
		float groundHeightMetres,
		uint64_t tick
	) {
		store.Observe<StatefulWeatherCheckpoint>();
		const auto *state = store.Get<StatefulWeatherState>(provider);
		const auto *controls = scene::WeatherControlsOf(store, provider);
		if (!controls || !scene::ValidWeatherCompute(*controls) || !controls->Enabled ||
			controls->Mode != scene::WeatherMode::Stateful)
			return std::nullopt;
		if (!state) {
			const auto *checkpoint = store.Get<StatefulWeatherCheckpoint>(provider);
			if (!checkpoint ||
				!EnsureCheckpointIndex(
					*checkpoint, store.ComponentChangeVersion<StatefulWeatherCheckpoint>()
				) ||
				(tick != UINT64_MAX && tick != checkpoint->Tick) ||
				checkpoint->Queries->Definition.Seed != controls->Seed ||
				checkpoint->Revision != controls->DefinitionRevision ||
				!CompatibleDefinition(
					checkpoint->Queries->Definition,
					*controls,
					WeatherLayerName(store, provider),
					store.Get<scene::WeatherClock>(provider)
				))
				return std::nullopt;
			return SamplePhysicalField(
				checkpoint->Queries->Definition,
				checkpoint->Tick,
				controls->Seed,
				WeatherLayerKey(controls->LayerName.Text()),
				[&](size_t index) { return IndexedCell(*checkpoint, index); },
				[&](size_t index) {
					core::ByteReader reader(
						std::span<const std::byte>(checkpoint->Bytes)
							.subspan(checkpoint->Queries->RainOffset + index * sizeof(float), sizeof(float))
					);
					return reader.ReadFloat();
				},
				checkpoint->Queries->SolidMask,
				checkpoint->Queries->GroundHeightMetres,
				*controls,
				positionMetres,
				groundHeightMetres
			);
		}
		if (state->AcceptedRevision != controls->DefinitionRevision ||
			state->LayerKey != WeatherLayerKey(controls->LayerName.Text()) ||
			!CompatibleDefinition(
				state->Simulation.Definition,
				*controls,
				WeatherLayerName(store, provider),
				store.Get<scene::WeatherClock>(provider)
			))
			return std::nullopt;
		if (tick != UINT64_MAX && tick != state->Simulation.Tick) {
			if (groundHeightMetres != 0) return std::nullopt;
			const auto found =
				std::find_if(state->TickSamples.begin(), state->TickSamples.end(), [&](const auto &sample) {
					return sample.Tick == tick && sample.PositionMetres == positionMetres;
				});
			return found == state->TickSamples.end() ? std::nullopt : std::optional(found->Sample);
		}
		return SampleState(*state, *controls, positionMetres, groundHeightMetres);
	}

	bool PublishStatefulCheckpoint(ecs::Store &store, ecs::Entity provider) {
		if (store.AdoptOnly()) return false;
		const auto *state = store.Get<StatefulWeatherState>(provider);
		if (!state) return false;
		const auto bytes = EncodeCheckpoint(state->Simulation);
		if (!bytes) return false;
		StatefulWeatherCheckpoint checkpoint{
			state->Simulation.Tick,
			state->Simulation.Definition.WorldEpoch,
			state->Simulation.Definition.Revision,
			*bytes,
			{}
		};
		IndexCheckpoint(checkpoint, state->Simulation);
		store.Set(provider, std::move(checkpoint));
		return true;
	}

	bool RestoreStatefulCheckpoint(
		ecs::Store &store, ecs::Entity provider, const StatefulWeatherCheckpoint &checkpoint
	) {
		if (store.AdoptOnly()) return false;
		const auto *controls = scene::WeatherControlsOf(store, provider);
		if (!controls || controls->Mode != scene::WeatherMode::Stateful) return false;
		SimulationState restored;
		if (!DecodeCheckpoint(checkpoint.Bytes, restored) || restored.Tick != checkpoint.Tick ||
			restored.Definition.WorldEpoch != checkpoint.Epoch ||
			restored.Definition.Revision != checkpoint.Revision ||
			restored.Definition.LayerName != WeatherLayerName(store, provider) ||
			restored.Definition.Seed != controls->Seed ||
			restored.Definition.Revision != controls->DefinitionRevision ||
			!CompatibleDefinition(
				restored.Definition,
				*controls,
				WeatherLayerName(store, provider),
				store.Get<scene::WeatherClock>(provider)
			))
			return false;
		StatefulWeatherState state;
		state.Simulation = std::move(restored);
		state.Seed = controls->Seed;
		state.LayerKey = WeatherLayerKey(controls->LayerName.Text());
		state.AcceptedRevision = state.Simulation.Definition.Revision;
		state.TickSamples.reserve(MAXIMUM_CATCHUP_TICKS);
		const auto checkpointCopy = checkpoint;
		store.Set(provider, std::move(state));
		store.Set(provider, checkpointCopy);
		store.Remove<StatefulWeatherInitializationFailure>(provider);
		return true;
	}

	void UpdateStatefulWeather(ecs::Store &store) {
		ENGINE_PROFILE("weather authoritative stateful runtime");
		if (store.AdoptOnly()) return;
		store.Observe<StatefulWeatherCheckpoint>();
		const uint64_t acceptedWrites = store.ComponentChangeVersion<scene::WeatherAcceptedDefinition>();
		const uint64_t checkpointWrites = store.ComponentChangeVersion<StatefulWeatherCheckpoint>();
		const auto refuseInitialization = [&](ecs::Entity provider, uint32_t revision) {
			store.Set(
				provider, StatefulWeatherInitializationFailure{revision, acceptedWrites, checkpointWrites}
			);
		};
		std::vector<ecs::Entity> staleFailures;
		store.Each<const StatefulWeatherInitializationFailure>([&](ecs::Entity provider, const auto &) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			if (!store.Has<scene::WeatherCompute>(provider) || !controls ||
				controls->Mode != scene::WeatherMode::Stateful)
				staleFailures.push_back(provider);
		});
		for (ecs::Entity provider : staleFailures) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			if (controls && controls->Mode == scene::WeatherMode::Procedural &&
				!store.Has<StatefulWeatherState>(provider) && !BeginRetiredPhysicsEpoch(store, provider))
				continue;
			store.Remove<StatefulWeatherInitializationFailure>(provider);
		}
		std::vector<ecs::Entity> retired;
		store.Each<const StatefulWeatherState>([&](ecs::Entity provider, const auto &) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			if (!store.Has<scene::WeatherCompute>(provider) || !controls ||
				controls->Mode != scene::WeatherMode::Stateful)
				retired.push_back(provider);
		});
		for (ecs::Entity provider : retired) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			if (controls && controls->Mode == scene::WeatherMode::Procedural &&
				!BeginRetiredPhysicsEpoch(store, provider)) {
				store.GetMutable<StatefulWeatherState>(provider)->LastStatus = SimulationStatus::Invalid;
				store.MarkChanged<StatefulWeatherState>(provider);
				continue;
			}
			store.Remove<StatefulWeatherState>(provider);
			store.Remove<StatefulWeatherCheckpoint>(provider);
		}
		size_t existingCount = 0;
		store.Each<const StatefulWeatherState>([&](ecs::Entity, const auto &) { ++existingCount; });
		std::vector<ecs::Entity> missing;
		store.Each<const scene::WeatherCompute>([&](ecs::Entity provider, const auto &) {
			const auto *controls = scene::WeatherControlsOf(store, provider);
			const auto *failure = store.Get<StatefulWeatherInitializationFailure>(provider);
			if (failure && controls && failure->DefinitionRevision == controls->DefinitionRevision &&
				failure->AcceptedWrites == acceptedWrites && failure->CheckpointWrites == checkpointWrites)
				return;
			if (controls && controls->Enabled && controls->Mode == scene::WeatherMode::Stateful &&
				!store.Has<StatefulWeatherState>(provider) && existingCount + missing.size() < 32)
				missing.push_back(provider);
		});
		for (ecs::Entity provider : missing) {
			const auto controls = *scene::WeatherControlsOf(store, provider);
			const auto *checkpoint = store.Get<StatefulWeatherCheckpoint>(provider);
			if (checkpoint) {
				if (!RestoreStatefulCheckpoint(store, provider, *checkpoint))
					refuseInitialization(provider, controls.DefinitionRevision);
				continue;
			}
			const auto *clock = store.Get<scene::WeatherClock>(provider);
			auto restarted = clock && store.Has<WeatherRuntimeState>(provider)
								 ? NextPhysicsEpoch(*clock)
								 : std::optional<scene::WeatherClock>{};
			if (clock && store.Has<WeatherRuntimeState>(provider) && !restarted) {
				refuseInitialization(provider, controls.DefinitionRevision);
				continue;
			}
			auto state = InitialState(store, provider, controls, restarted ? &*restarted : nullptr);
			if (state) {
				if (restarted) {
					if (!ClaimInitializedEpoch(store, provider, *restarted, *state, controls)) {
						refuseInitialization(provider, controls.DefinitionRevision);
						continue;
					}
					*store.GetMutable<scene::WeatherClock>(provider) = *restarted;
					store.MarkChanged<scene::WeatherClock>(provider);
					state->LastWorldTick = store.Time().Tick;
				}
				store.Set(provider, std::move(*state));
				store.Remove<StatefulWeatherInitializationFailure>(provider);
				(void)PublishStatefulCheckpoint(store, provider);
			} else
				refuseInitialization(provider, controls.DefinitionRevision);
		}
		std::vector<ecs::Entity> publish;
		store.Each<const scene::WeatherCompute, const scene::WeatherClock, StatefulWeatherState>(
			[&](ecs::Entity provider, const auto &, const auto &clock, StatefulWeatherState &state) {
				const auto *accepted = scene::WeatherControlsOf(store, provider);
				if (!accepted) return;
				const auto &controls = *accepted;
				if (state.LastWorldTick == store.Time().Tick || !controls.Enabled ||
					controls.Mode != scene::WeatherMode::Stateful)
					return;
				state.LastWorldTick = store.Time().Tick;
				const auto resolved = WeatherTimeOf(clock);
				if (!resolved || clock.Epoch < state.Simulation.Definition.WorldEpoch ||
					resolved->PhaseTick < state.Simulation.Tick) {
					state.LastStatus = SimulationStatus::Invalid;
					return;
				}
				const auto layerName = WeatherLayerName(store, provider);
				auto renamedDefinition = state.Simulation.Definition;
				renamedDefinition.LayerName = layerName;
				renamedDefinition.WorldEpoch = clock.Epoch;
				if (!CompatibleDefinition(renamedDefinition, controls, layerName, &clock, true) ||
					state.LayerKey != WeatherLayerKey(controls.LayerName.Text())) {
					// Accepted structural edits explicitly begin a new atmosphere at the current phase.
					// Old water and energy budgets belong to the previous epoch, not this initialization.
					auto nextClock = NextPhysicsEpoch(clock);
					auto initialized = nextClock ? InitialState(store, provider, controls, &*nextClock)
												 : std::optional<StatefulWeatherState>{};
					if (!initialized ||
						!ClaimInitializedEpoch(store, provider, *nextClock, *initialized, controls)) {
						state.LastStatus = SimulationStatus::Invalid;
						store.MarkChanged<StatefulWeatherState>(provider);
						return;
					}
					initialized->LastWorldTick = store.Time().Tick;
					state = std::move(*initialized);
					*store.GetMutable<scene::WeatherClock>(provider) = *nextClock;
					store.MarkChanged<scene::WeatherClock>(provider);
					store.MarkChanged<StatefulWeatherState>(provider);
					publish.push_back(provider);
					return;
				}
				if (state.Simulation.Definition.WorldEpoch != clock.Epoch) {
					// A reused stream name gets a newer event era while compatible physical history survives.
					state.Simulation.Definition.WorldEpoch = clock.Epoch;
					state.TickSamples.clear();
					const auto column = StatefulEventColumn(state, state.Simulation.Tick);
					const auto sample = SampleState(state, controls, column, 0);
					if (sample) state.TickSamples.push_back({state.Simulation.Tick, column, *sample});
					publish.push_back(provider);
				}
				{
					SimulationForcing forcing = state.Simulation.ForcingHistory.back();
					forcing.Tick = clock.PhaseTick;
					forcing.WindTargetMetresPerSecond = {
						controls.WindMetresPerSecond.X,
						controls.WindMetresPerSecond.Y,
						controls.WindMetresPerSecond.Z
					};
					forcing.WindRelaxationPerSecond = 1;
					for (size_t axis = 0; axis < 3; ++axis)
						if (state.Simulation.Definition.Grid.Boundaries[axis] == SolverBoundary::Open ||
							state.Simulation.Definition.Grid.Boundaries[axis] == SolverBoundary::GroundOpen)
							forcing.OpenVelocityMetresPerSecond[axis] =
								forcing.WindTargetMetresPerSecond[axis];
					const bool windChanged = forcing.WindTargetMetresPerSecond !=
											 state.Simulation.ForcingHistory.back().WindTargetMetresPerSecond;
					if (windChanged && !AddSimulationForcing(state.Simulation, forcing, true)) {
						state.LastStatus = SimulationStatus::Invalid;
						return;
					}
					if (windChanged || controls.DefinitionRevision != state.AcceptedRevision) {
						state.AcceptedRevision = controls.DefinitionRevision;
						state.Simulation.Definition.Revision = controls.DefinitionRevision;
						publish.push_back(provider);
					}
				}
				if (state.Simulation.Definition.LayerName != layerName) {
					// Accepted provider renames change checkpoint identity without restarting the atmosphere.
					state.Simulation.Definition.LayerName = layerName;
					publish.push_back(provider);
				}
				uint64_t baseLimit = clock.Tick;
				if (const auto *events = store.Get<WeatherRuntimeState>(provider))
					baseLimit = events->LastTick +
								std::min<uint64_t>(MAXIMUM_CATCHUP_TICKS, UINT64_MAX - events->LastTick);
				const auto boundedTime = ResolveTime(
					WeatherAnchorOf(clock), std::min(clock.Tick, std::max(clock.AnchorTick, baseLimit))
				);
				if (!boundedTime) {
					state.LastStatus = SimulationStatus::Invalid;
					return;
				}
				const uint64_t target = std::min(
					boundedTime->PhaseTick,
					state.Simulation.Tick +
						std::min<uint64_t>(MAXIMUM_CATCHUP_TICKS, UINT64_MAX - state.Simulation.Tick)
				);
				while (state.Simulation.Tick < target) {
					if (!state.Workspace) state.Workspace = std::make_shared<SimulationWorkspace>();
					state.LastStatus = StepSimulation(state.Simulation, *state.Workspace);
					if (state.LastStatus != SimulationStatus::Success) break;
					const auto column = StatefulEventColumn(state, state.Simulation.Tick);
					const auto sample = SampleState(state, controls, column, 0);
					if (sample) {
						if (state.TickSamples.size() == MAXIMUM_CATCHUP_TICKS)
							state.TickSamples.erase(state.TickSamples.begin());
						state.TickSamples.push_back({state.Simulation.Tick, column, *sample});
					}
				}
				const auto *checkpoint = store.Get<StatefulWeatherCheckpoint>(provider);
				if (!checkpoint || checkpoint->Tick > state.Simulation.Tick ||
					state.Simulation.Tick - checkpoint->Tick >=
						std::min<uint64_t>(
							128,
							static_cast<uint64_t>(state.Simulation.Definition.TickRate) *
								CHECKPOINT_INTERVAL_SECONDS
						))
					publish.push_back(provider);
				store.MarkChanged<StatefulWeatherState>(provider);
			}
		);
		for (ecs::Entity provider : publish)
			(void)PublishStatefulCheckpoint(store, provider);
	}

	void RegisterStatefulWeatherSystem(ecs::Scheduler &scheduler) {
		scheduler.Add("weather.stateful", ecs::Phase::PreSimulation, UpdateStatefulWeather);
	}
}
