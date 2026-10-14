#include <engine/core/Profiling.hpp>
#include <engine/weather/Checkpoint.hpp>

#include <bit>
#include <limits>
#include <new>
#include <type_traits>

namespace engine::weather {
	namespace {
		constexpr uint32_t CHECKPOINT_MAGIC = 0x31524857;
		constexpr uint32_t CHECKPOINT_VERSION = 3;

		uint64_t Digest(std::span<const std::byte> bytes) {
			uint64_t digest = 14695981039346656037ull;
			for (std::byte byte : bytes) {
				digest ^= std::to_integer<uint8_t>(byte);
				digest *= 1099511628211ull;
			}
			return digest;
		}

		struct Writer {
			std::vector<std::byte> Bytes;
			bool Valid = true;

			template <class Number> void Field(const Number &number) {
				using Word = std::conditional_t<
					sizeof(Number) == 8,
					uint64_t,
					std::conditional_t<
						sizeof(Number) == 4,
						uint32_t,
						std::conditional_t<sizeof(Number) == 2, uint16_t, uint8_t>>>;
				const Word bits = std::bit_cast<Word>(number);
				for (size_t index = 0; index < sizeof(Number); ++index)
					Bytes.push_back(static_cast<std::byte>((bits >> (8 * index)) & 255));
			}

			void Field(const std::string &text) {
				Field(static_cast<uint16_t>(text.size()));
				for (unsigned char character : text)
					Bytes.push_back(static_cast<std::byte>(character));
			}

			void Boundary(const SolverBoundary &boundary) {
				Field(
					std::string(
						boundary == SolverBoundary::Periodic	 ? "periodic"
						: boundary == SolverBoundary::Closed	 ? "closed"
						: boundary == SolverBoundary::GroundOpen ? "ground-open"
																 : "open"
					)
				);
			}
		};

		struct Reader {
			std::span<const std::byte> Bytes;
			size_t Position = 0;
			bool Valid = true;

			template <class Number> void Field(Number &number) {
				if (!Valid || Bytes.size() - Position < sizeof(Number)) {
					Valid = false;
					return;
				}
				using Word = std::conditional_t<
					sizeof(Number) == 8,
					uint64_t,
					std::conditional_t<
						sizeof(Number) == 4,
						uint32_t,
						std::conditional_t<sizeof(Number) == 2, uint16_t, uint8_t>>>;
				Word bits = 0;
				for (size_t index = 0; index < sizeof(Number); ++index)
					bits |= static_cast<Word>(std::to_integer<uint8_t>(Bytes[Position++])) << (8 * index);
				number = std::bit_cast<Number>(bits);
			}

			void Field(std::string &text) {
				uint16_t length = 0;
				Field(length);
				if (!Valid || length > 256 || Bytes.size() - Position < length) {
					Valid = false;
					return;
				}
				text.resize(length);
				for (char &character : text)
					character = static_cast<char>(std::to_integer<uint8_t>(Bytes[Position++]));
				if (text.find('\0') != std::string::npos) Valid = false;
			}

			void Boundary(SolverBoundary &boundary) {
				std::string name;
				Field(name);
				if (name == "periodic")
					boundary = SolverBoundary::Periodic;
				else if (name == "closed")
					boundary = SolverBoundary::Closed;
				else if (name == "open")
					boundary = SolverBoundary::Open;
				else if (name == "ground-open")
					boundary = SolverBoundary::GroundOpen;
				else
					Valid = false;
			}
		};

		template <class Codec, class Cell> void CellFields(Codec &codec, Cell &cell) {
			codec.Field(cell.VapourKilogramsPerKilogram);
			codec.Field(cell.CondensateKilogramsPerCubicMetre);
			codec.Field(cell.TemperatureCelsius);
			codec.Field(cell.AtmosphericPressurePascals);
			for (auto &velocity : cell.VelocityMetresPerSecond)
				codec.Field(velocity);
			codec.Field(cell.ProjectionPressurePascals);
		}

		template <class Codec, class Definition> void DefinitionFields(Codec &codec, Definition &definition) {
			codec.Field(definition.AlgorithmName);
			codec.Field(definition.LayerName);
			codec.Field(definition.AlgorithmVersion);
			codec.Field(definition.Revision);
			codec.Field(definition.Seed);
			for (auto &field : definition.InitializationRecipe)
				codec.Field(field);
			codec.Field(definition.WorldEpoch);
			for (auto &extent : definition.Grid.Cells)
				codec.Field(extent);
			for (auto &origin : definition.Grid.Origin)
				codec.Field(origin);
			codec.Field(definition.Grid.CellSizeMetres);
			for (auto &boundary : definition.Grid.Boundaries)
				codec.Boundary(boundary);
			codec.Field(definition.TickRate);
			codec.Field(definition.MaximumSubsteps);
			codec.Field(definition.ProjectionIterations);
			codec.Field(definition.MaximumStateBytes);
			codec.Field(definition.DryAirKilogramsPerCubicMetre);
			codec.Field(definition.DiffusionSquareMetresPerSecond);
			codec.Field(definition.ReferenceTemperatureCelsius);
			codec.Field(definition.SaturationVapourRatio);
			codec.Field(definition.SaturationSlopePerCelsius);
			codec.Field(definition.PhaseRelaxationPerSecond);
			codec.Field(definition.LatentHeatJoulesPerKilogram);
			codec.Field(definition.HeatCapacityJoulesPerKilogramCelsius);
			codec.Field(definition.BuoyancyMetresPerSecondSquaredPerCelsius);
			codec.Field(definition.PrecipitationThresholdKilogramsPerCubicMetre);
			codec.Field(definition.PrecipitationPerSecond);
			codec.Field(definition.MaximumVelocityMetresPerSecond);
			codec.Field(definition.MaximumPressureGradientPascalsPerMetre);
			codec.Field(definition.MaximumDivergencePerSecond);
			codec.Field(definition.TerrainRevision);
			codec.Field(definition.GroundVapourFluxKilogramsPerSquareMetreSecond);
			codec.Field(definition.RainFallSpeedMetresPerSecond);
		}

		template <class Codec, class Forcing> void ForcingFields(Codec &codec, Forcing &forcing) {
			codec.Field(forcing.Tick);
			for (auto &acceleration : forcing.AccelerationMetresPerSecondSquared)
				codec.Field(acceleration);
			for (auto &velocity : forcing.OpenVelocityMetresPerSecond)
				codec.Field(velocity);
			for (auto &velocity : forcing.WindTargetMetresPerSecond)
				codec.Field(velocity);
			codec.Field(forcing.WindRelaxationPerSecond);
			codec.Field(forcing.VapourSourcePerSecond);
			codec.Field(forcing.CondensateSourceKilogramsPerCubicMetrePerSecond);
			codec.Field(forcing.CoolingCelsiusPerSecond);
			CellFields(codec, forcing.Inflow);
		}

		template <class Codec, class Budget> void BudgetFields(Codec &codec, Budget &budget) {
			codec.Field(budget.InitialWaterKilograms);
			codec.Field(budget.SourceWaterKilograms);
			codec.Field(budget.BoundaryWaterKilograms);
			codec.Field(budget.PrecipitationWaterKilograms);
			codec.Field(budget.WaterResidualKilograms);
			codec.Field(budget.InitialEnthalpyJoules);
			codec.Field(budget.ExternalEnthalpyJoules);
			codec.Field(budget.EnthalpyResidualJoules);
		}
	}

	std::optional<std::vector<std::byte>>
	EncodeCheckpoint(const SimulationState &state, CheckpointLimits limits) {
		ENGINE_PROFILE("weather checkpoint encode");
		if (!ValidateSimulation(state) || state.Cells.size() > limits.MaximumCells ||
			state.ForcingHistory.size() > limits.MaximumForcingRecords ||
			state.Definition.LayerName.find('\0') != std::string::npos)
			return std::nullopt;
		const uint64_t maximumEncoded =
			1024 + state.Cells.size() * (sizeof(SolverCell) + sizeof(float)) + state.SolidMask.size() +
			state.GroundHeightMetres.size() * sizeof(float) + state.ForcingHistory.size() * 128;
		if (maximumEncoded > limits.MaximumBytes) return std::nullopt;
		try {
			Writer writer;
			writer.Bytes.reserve(static_cast<size_t>(maximumEncoded));
			writer.Field(CHECKPOINT_MAGIC);
			writer.Field(CHECKPOINT_VERSION);
			DefinitionFields(writer, state.Definition);
			writer.Field(state.Tick);
			BudgetFields(writer, state.Budget);
			writer.Field(state.LastSubsteps);
			writer.Field(state.LastMaximumDivergencePerSecond);
			writer.Field(static_cast<uint32_t>(state.ForcingHistory.size()));
			for (const auto &forcing : state.ForcingHistory)
				ForcingFields(writer, forcing);
			writer.Field(static_cast<uint32_t>(state.GroundHeightMetres.size()));
			for (float height : state.GroundHeightMetres)
				writer.Field(height);
			writer.Field(static_cast<uint32_t>(state.SolidMask.size()));
			for (uint8_t mask : state.SolidMask)
				writer.Field(mask);
			writer.Field(static_cast<uint32_t>(state.FallingCondensateKilogramsPerCubicMetre.size()));
			for (float rain : state.FallingCondensateKilogramsPerCubicMetre)
				writer.Field(rain);
			writer.Field(static_cast<uint32_t>(state.Cells.size()));
			for (const auto &cell : state.Cells)
				CellFields(writer, cell);
			writer.Field(Digest(writer.Bytes));
			return std::move(writer.Bytes);
		} catch (const std::bad_alloc &) {
			return std::nullopt;
		}
	}

	bool DecodeCheckpoint(
		std::span<const std::byte> bytes, SimulationState &destination, CheckpointLimits limits
	) {
		ENGINE_PROFILE("weather checkpoint decode");
		if (bytes.size() < 16 || bytes.size() > limits.MaximumBytes) return false;
		Reader checksum{bytes.last(8)};
		uint64_t expectedDigest = 0;
		checksum.Field(expectedDigest);
		if (Digest(bytes.first(bytes.size() - 8)) != expectedDigest) return false;
		try {
			Reader reader{bytes.first(bytes.size() - 8)};
			uint32_t magic = 0, version = 0;
			reader.Field(magic);
			reader.Field(version);
			if (!reader.Valid || magic != CHECKPOINT_MAGIC || version != CHECKPOINT_VERSION) return false;
			SimulationState decoded;
			DefinitionFields(reader, decoded.Definition);
			uint64_t gridCount = 1;
			for (uint32_t extent : decoded.Definition.Grid.Cells) {
				if (extent < 2 || extent > 128) return false;
				gridCount *= extent;
			}
			if (!reader.Valid || gridCount > limits.MaximumCells || gridCount > 65536 ||
				gridCount * 200 > decoded.Definition.MaximumStateBytes)
				return false;
			reader.Field(decoded.Tick);
			BudgetFields(reader, decoded.Budget);
			reader.Field(decoded.LastSubsteps);
			reader.Field(decoded.LastMaximumDivergencePerSecond);
			uint32_t forcingCount = 0;
			reader.Field(forcingCount);
			if (!reader.Valid || forcingCount == 0 || forcingCount > limits.MaximumForcingRecords ||
				forcingCount > 64 || forcingCount > (reader.Bytes.size() - reader.Position) / 92)
				return false;
			decoded.ForcingHistory.resize(forcingCount);
			for (auto &forcing : decoded.ForcingHistory)
				ForcingFields(reader, forcing);
			uint32_t columns = 0, maskBytes = 0, rainCount = 0;
			reader.Field(columns);
			if (!reader.Valid ||
				columns != static_cast<uint64_t>(decoded.Definition.Grid.Cells[0]) *
							   decoded.Definition.Grid.Cells[2] ||
				columns > (reader.Bytes.size() - reader.Position) / sizeof(float))
				return false;
			decoded.GroundHeightMetres.resize(columns);
			for (float &height : decoded.GroundHeightMetres)
				reader.Field(height);
			reader.Field(maskBytes);
			if (!reader.Valid || maskBytes != (gridCount + 7) / 8 ||
				maskBytes > reader.Bytes.size() - reader.Position)
				return false;
			decoded.SolidMask.resize(maskBytes);
			for (uint8_t &mask : decoded.SolidMask)
				reader.Field(mask);
			reader.Field(rainCount);
			if (!reader.Valid || rainCount != gridCount ||
				rainCount > (reader.Bytes.size() - reader.Position) / sizeof(float))
				return false;
			decoded.FallingCondensateKilogramsPerCubicMetre.resize(rainCount);
			for (float &rain : decoded.FallingCondensateKilogramsPerCubicMetre)
				reader.Field(rain);
			uint32_t cellCount = 0;
			reader.Field(cellCount);
			if (!reader.Valid || cellCount == 0 || cellCount > limits.MaximumCells || cellCount > 65536 ||
				cellCount != gridCount ||
				cellCount > (reader.Bytes.size() - reader.Position) / sizeof(SolverCell) ||
				static_cast<uint64_t>(cellCount) * 200 > decoded.Definition.MaximumStateBytes)
				return false;
			decoded.Cells.resize(cellCount);
			for (auto &cell : decoded.Cells)
				CellFields(reader, cell);
			if (!reader.Valid || reader.Position != reader.Bytes.size() || !ValidateSimulation(decoded))
				return false;
			destination = std::move(decoded);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}
}
