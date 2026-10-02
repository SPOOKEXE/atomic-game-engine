#include "FluidPayload.hpp"
#include "SourceFlipDomain.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FlipReplay.hpp>

namespace engine::imagegraph {
	namespace {
		Status Refuse(Diagnostic &diagnostic, Status status, const char *message) {
			diagnostic = {};
			diagnostic.Code = status;
			diagnostic.Message = message;
			return status;
		}
		uint64_t
		Overlap(const FluidDomainValue &previous, const FluidDomainValue &result, uint64_t temporary = 0) {
			uint64_t bytes = detail::MeshAddBytes(
				detail::FluidStorageBytes<true>(previous), detail::FluidStorageBytes<true>(previous)
			);
			if (&previous != &result)
				bytes = detail::MeshAddBytes(bytes, detail::FluidStorageBytes<true>(result));
			return detail::MeshAddBytes(bytes, temporary);
		}
		bool SafePositions(const FluidDomainData &data, const detail::FluidLayout &layout) {
			const auto &positions = data.Buffers[size_t(FluidBuffer::ParticlePosition)];
			const double inv = std::max(layout.InverseCellSize, layout.InverseHashSize);
			for (size_t index = 0; index < size_t(data.ParticleCount) * 2; ++index)
				if (!std::isfinite(positions[index]) ||
					std::abs(positions[index]) * inv > double(std::numeric_limits<int>::max() / 4))
					return false;
			return true;
		}
		bool FiniteBuffers(const FluidDomainData &data) {
			for (const auto &buffer : data.Buffers)
				for (const auto value : buffer)
					if (!std::isfinite(value)) return false;
			return true;
		}
		detail::source_flip::SourceDomain Bind(
			FluidDomainData &data,
			const detail::FluidLayout &layout,
			std::span<const detail::source_flip::SourceObstacle> obstacles,
			uint64_t &work
		) {
			using namespace detail::source_flip;
			SourceDomain domain{};
			const auto &settings = data.Settings;
			domain.width = settings.Width;
			domain.height = settings.Height;
			domain.spacing = settings.Spacing;
			domain.density = settings.Density;
			domain.viscosity = settings.Viscosity;
			domain.friction = settings.Friction;
			domain.fNumX = layout.Columns;
			domain.fNumY = layout.Rows;
			domain.fNumX1 = layout.Columns - 1;
			domain.fNumY1 = layout.Rows - 1;
			domain.fNumCells = size_t(layout.Columns) * layout.Rows;
			domain.h = layout.CellSize;
			domain.fInvSpacing = layout.InverseCellSize;
			domain.collideWall = int(settings.Wall);
			domain.wallElasticity = settings.WallElasticity;
			domain.u = {
				data.Buffers[size_t(FluidBuffer::U)].data(),
				data.Buffers[size_t(FluidBuffer::U)].size(),
				&work
			};
			domain.v = {
				data.Buffers[size_t(FluidBuffer::V)].data(),
				data.Buffers[size_t(FluidBuffer::V)].size(),
				&work
			};
			domain.du = {
				data.Buffers[size_t(FluidBuffer::DU)].data(),
				data.Buffers[size_t(FluidBuffer::DU)].size(),
				&work
			};
			domain.dv = {
				data.Buffers[size_t(FluidBuffer::DV)].data(),
				data.Buffers[size_t(FluidBuffer::DV)].size(),
				&work
			};
			domain.prevU = {
				data.Buffers[size_t(FluidBuffer::PreviousU)].data(),
				data.Buffers[size_t(FluidBuffer::PreviousU)].size(),
				&work
			};
			domain.prevV = {
				data.Buffers[size_t(FluidBuffer::PreviousV)].data(),
				data.Buffers[size_t(FluidBuffer::PreviousV)].size(),
				&work
			};
			domain.p = {
				data.Buffers[size_t(FluidBuffer::Pressure)].data(),
				data.Buffers[size_t(FluidBuffer::Pressure)].size(),
				&work
			};
			domain.s = {
				data.Buffers[size_t(FluidBuffer::Open)].data(),
				data.Buffers[size_t(FluidBuffer::Open)].size(),
				&work
			};
			domain.solidMap = {
				data.Buffers[size_t(FluidBuffer::Solid)].data(),
				data.Buffers[size_t(FluidBuffer::Solid)].size(),
				&work
			};
			domain.cellType = {
				data.Buffers[size_t(FluidBuffer::CellType)].data(),
				data.Buffers[size_t(FluidBuffer::CellType)].size(),
				&work
			};
			domain.particlePos = {
				data.Buffers[size_t(FluidBuffer::ParticlePosition)].data(),
				data.Buffers[size_t(FluidBuffer::ParticlePosition)].size(),
				&work
			};
			domain.particleVel = {
				data.Buffers[size_t(FluidBuffer::ParticleVelocity)].data(),
				data.Buffers[size_t(FluidBuffer::ParticleVelocity)].size(),
				&work
			};
			domain.particleDensity = {
				data.Buffers[size_t(FluidBuffer::ParticleDensity)].data(),
				data.Buffers[size_t(FluidBuffer::ParticleDensity)].size(),
				&work
			};
			domain.particleLife = {
				data.Buffers[size_t(FluidBuffer::ParticleLife)].data(),
				data.Buffers[size_t(FluidBuffer::ParticleLife)].size(),
				&work
			};
			domain.numCellParticles = {
				data.Buffers[size_t(FluidBuffer::CellParticleCount)].data(),
				data.Buffers[size_t(FluidBuffer::CellParticleCount)].size(),
				&work
			};
			domain.firstCellParticle = {
				data.Buffers[size_t(FluidBuffer::FirstCellParticle)].data(),
				data.Buffers[size_t(FluidBuffer::FirstCellParticle)].size(),
				&work
			};
			domain.cellParticleIds = {
				data.Buffers[size_t(FluidBuffer::CellParticleIds)].data(),
				data.Buffers[size_t(FluidBuffer::CellParticleIds)].size(),
				&work
			};
			domain.particleRestDensity = data.ParticleRestDensity;
			domain.particleRadius = layout.ParticleRadius;
			domain.pInvSpacing = layout.InverseHashSize;
			domain.pNumX = layout.HashColumns;
			domain.pNumY = layout.HashRows;
			domain.pNumCells = size_t(layout.HashColumns) * layout.HashRows;
			domain.velocityDamping = settings.VelocityDamping;
			domain.maxParticles = settings.MaximumParticles;
			domain.numParticles = data.ParticleCount;
			domain.dt = settings.TimeStep;
			domain.globalIteration = settings.GlobalIterations;
			domain.gravity = settings.Gravity;
			domain.gravityDirection = settings.GravityDirection;
			domain.flipRatio = settings.FlipRatio;
			domain.numPressureIterations = settings.PressureIterations;
			domain.numParticleIterations = settings.ParticleIterations;
			domain.overRelaxation = settings.OverRelaxation;
			domain.obstacles = obstacles;
			return domain;
		}
	}
	Status ResetFlipReplay(
		const FluidDomainSettings &settings,
		uint64_t tick,
		uint64_t revision,
		uint64_t maximumBytes,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.fluid.reset");
		const auto layout = detail::FluidDomainLayout(settings);
		if (!layout || tick > Limits::MaximumTick)
			return Refuse(
				diagnostic, Status::InvalidValue, "FLIP reset requires bounded valid settings and tick"
			);
		uint64_t bytes = sizeof(FluidDomainData) + std::string{}.capacity();
		for (const auto count : layout->Sizes)
			bytes = detail::MeshAddBytes(bytes, count * sizeof(double));
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			detail::MeshAddBytes(bytes, detail::FluidStorageBytes<true>(result)) > maximumBytes)
			return Refuse(
				diagnostic, Status::LimitExceeded, "FLIP reset replacement overlap exceeds byte budget"
			);
		FluidDomainValue candidate;
		auto &data = candidate.Data.emplace();
		data.Settings = settings;
		data.Tick = tick;
		data.AuthoringRevision = revision;
		data.Initialized = true;
		for (size_t index = 0; index < data.Buffers.size(); ++index)
			data.Buffers[index].resize(layout->Sizes[index]);
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return Refuse(diagnostic, Status::LimitExceeded, "FLIP reset allocation refused");
	}
	Status SpawnFlipReplay(
		const FluidDomainValue &previous,
		std::span<const FluidSpawnParticle> particles,
		uint64_t maximumBytes,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.fluid.spawn");
		if (!previous.Data || !detail::ValidFluidPayload(previous))
			return Refuse(
				diagnostic, Status::InvalidValue, "FLIP spawn needs initialized valid caller state"
			);
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			Overlap(previous, result) > maximumBytes)
			return Refuse(
				diagnostic, Status::LimitExceeded, "FLIP spawn replacement overlap exceeds byte budget"
			);
		const size_t count = std::min(
			particles.size(), size_t(previous.Data->Settings.MaximumParticles - previous.Data->ParticleCount)
		);
		for (size_t index = 0; index < count; ++index)
			if (!detail::MeshFinite(particles[index].Position) ||
				!detail::MeshFinite(particles[index].Velocity))
				return Refuse(diagnostic, Status::InvalidValue, "FLIP spawn coordinates must be finite");
		FluidDomainValue candidate = previous;
		auto &data = *candidate.Data;
		auto &positions = data.Buffers[size_t(FluidBuffer::ParticlePosition)],
			 &velocities = data.Buffers[size_t(FluidBuffer::ParticleVelocity)];
		for (size_t index = 0; index < count; ++index) {
			const size_t destination = (data.ParticleCount + index) * 2;
			positions[destination] = particles[index].Position.X;
			positions[destination + 1] = particles[index].Position.Y;
			velocities[destination] = particles[index].Velocity.X;
			velocities[destination + 1] = particles[index].Velocity.Y;
		}
		data.ParticleCount += uint32_t(count);
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		return Refuse(diagnostic, Status::LimitExceeded, "FLIP spawn allocation refused");
	}
	Status StepFlipReplay(
		const FluidDomainValue &previous,
		uint64_t tick,
		uint64_t revision,
		uint64_t maximumBytes,
		uint64_t maximumWork,
		FluidDomainValue &result,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraph.fluid.step");
		if (!previous.Data || !detail::ValidFluidPayload(previous))
			return Refuse(diagnostic, Status::InvalidValue, "FLIP step needs initialized valid caller state");
		const auto &old = *previous.Data;
		if (old.Tick >= Limits::MaximumTick || tick != old.Tick + 1 || revision != old.AuthoringRevision)
			return Refuse(
				diagnostic,
				Status::InvalidValue,
				"FLIP requires contiguous tick and matching authoring revision"
			);
		if (old.Settings.TimeStep == 0 && !old.Settings.SkipIncompressible && old.Settings.PressureIterations)
			return Refuse(
				diagnostic,
				Status::UnsupportedExecution,
				"source FLIP pressure division at zero time step needs a reference capture"
			);
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			Overlap(previous, result, old.Obstacles.size() * sizeof(detail::source_flip::SourceObstacle)) >
				maximumBytes)
			return Refuse(
				diagnostic, Status::LimitExceeded, "FLIP step replacement overlap exceeds byte budget"
			);
		if (!maximumWork || maximumWork > FluidDomainLimits::MaximumWork ||
			uint64_t(old.Obstacles.size()) * old.ParticleCount * old.Settings.GlobalIterations > maximumWork)
			return Refuse(
				diagnostic, Status::LimitExceeded, "FLIP source work exceeds bounded kernel budget"
			);
		const auto layout = *detail::FluidDomainLayout(old.Settings);
		if (!SafePositions(old, layout))
			return Refuse(
				diagnostic,
				Status::UnsupportedExecution,
				"source FLIP spatial indices exceed safe integer range"
			);
		FluidDomainValue candidate = previous;
		auto &data = *candidate.Data;
		std::vector<detail::source_flip::SourceObstacle> obstacles;
		obstacles.reserve(data.Obstacles.size());
		for (const auto &obstacle : data.Obstacles)
			obstacles.push_back(
				{obstacle.X,
				 obstacle.Y,
				 obstacle.VelocityX,
				 obstacle.VelocityY,
				 detail::source_flip::COLLISION_SHAPE(obstacle.Shape),
				 obstacle.Radius,
				 obstacle.Width,
				 obstacle.Height}
			);
		auto domain = Bind(data, layout, obstacles, maximumWork);
		for (uint32_t x = 1; x < layout.Columns - 1; ++x)
			for (uint32_t y = 1; y < layout.Rows - 1; ++y)
				domain.s[size_t(x) * layout.Rows + y] = 1;
		for (uint32_t iteration = 0; iteration < data.Settings.GlobalIterations; ++iteration) {
			detail::source_flip::integrateParticles(domain);
			if (!SafePositions(data, layout))
				return Refuse(
					diagnostic,
					Status::UnsupportedExecution,
					"source FLIP integration produced unsafe spatial indices"
				);
			detail::source_flip::pushParticlesApart(domain);
			detail::source_flip::handleParticleCollisions(domain);
			if (!FiniteBuffers(data) || !SafePositions(data, layout))
				return Refuse(
					diagnostic,
					Status::InvalidValue,
					"source FLIP collisions produced nonfinite or unsafe state"
				);
			if (!data.Settings.SkipIncompressible) {
				detail::source_flip::transferVelocities(domain, true);
				detail::source_flip::updateParticleDensity(domain);
				detail::source_flip::solveIncompressibility(domain);
				detail::source_flip::transferVelocities(domain, false);
				if (!FiniteBuffers(data))
					return Refuse(
						diagnostic, Status::InvalidValue, "source FLIP grid solve produced nonfinite state"
					);
			}
		}
		data.Tick = tick;
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const detail::source_flip::BoundsViolation &) {
		return Refuse(
			diagnostic, Status::UnsupportedExecution, "source FLIP solver indexes outside a recorded array"
		);
	} catch (const detail::source_flip::WorkViolation &) {
		return Refuse(
			diagnostic, Status::LimitExceeded, "FLIP source buffer accesses exceed kernel work budget"
		);
	} catch (const std::bad_alloc &) {
		return Refuse(diagnostic, Status::LimitExceeded, "FLIP step allocation refused");
	}
}
