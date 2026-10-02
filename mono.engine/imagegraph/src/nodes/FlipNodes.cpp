#include "FlipNodes.hpp"

#include "../FluidPayload.hpp"
#include "../SourceFlipDomain.hpp"
#include "VerletNodes.hpp"

#include <engine/imagegraph/FlipReplay.hpp>

namespace engine::imagegraph::detail {
	namespace {
		bool Count(
			NodeContext &context, std::string_view port, double fallback, uint32_t maximum, uint32_t &result
		) {
			const double value = context.Scalar(port, fallback);
			if (!std::isfinite(value) || value < 0 || value != std::trunc(value) || value > maximum)
				return context.Fail(
					Status::InvalidValue, "FLIP solver count requires a bounded nonnegative integer", port
				);
			result = uint32_t(value);
			return true;
		}
	}
	bool FlipDomain(NodeContext &context) {
		const auto *previous = FindFluidSimulationOrigin(context, context.Authored.Id, context.ProcessorRow);
		FluidDomainValue output;
		if (previous) {
			if (previous->State.Tick != context.Request.Tick &&
				(previous->State.Tick >= Limits::MaximumTick ||
				 previous->State.Tick + 1 != context.Request.Tick))
				return context.Fail(
					Status::InvalidValue, "FLIP domain requires a contiguous tick or reset", "domain"
				);
			if (previous->State.AuthoringRevision != context.Request.SimulationAuthoringRevision)
				return context.Fail(Status::InvalidValue, "FLIP domain revision requires reset", "domain");
			if (!context.ReserveOutput(FluidStorageBytes<true>(previous->Fluid), "domain")) return false;
			output = previous->Fluid;
		} else {
			FluidDomainSettings settings;
			Vector2 dimension = context.Vec2("dimension", {1, 1});
			if (!context.IsLinked("dimension")) {
				const auto unit = context.Integer("dimension_unit", 1);
				if (unit == 1) {
					dimension.X *= context.Project.SurfaceWidth;
					dimension.Y *= context.Project.SurfaceHeight;
				} else if (unit == 2) {
					const Image *image = context.Input("dimension");
					if (!image)
						return context.Fail(
							Status::UnsupportedExecution,
							"FLIP mask dimension requires a resolved surface",
							"dimension"
						);
					dimension = {double(image->Width), double(image->Height)};
				} else if (unit != 0)
					return context.Fail(
						Status::InvalidValue, "FLIP dimension unit is invalid", "dimension_unit"
					);
			}
			settings.Spacing = std::max(1., context.Scalar("particle_size", 1));
			settings.Width = dimension.X + settings.Spacing * 2;
			settings.Height = dimension.Y + settings.Spacing * 2;
			settings.Density = context.Scalar("particle_density", 10);
			if (!Count(
					context,
					"attribute_max_particles",
					10000,
					FluidDomainLimits::MaximumParticles,
					settings.MaximumParticles
				))
				return false;
			const auto layout = FluidDomainLayout(settings);
			if (!layout)
				return context.Fail(
					Status::InvalidValue, "FLIP domain grid exceeds its native limits", "dimension"
				);
			uint64_t bytes =
				sizeof(FluidDomainData) + std::max(context.Authored.Id.size(), std::string{}.capacity());
			for (const auto count : layout->Sizes)
				bytes = MeshAddBytes(bytes, count * sizeof(double));
			if (!context.ReserveOutput(bytes, "domain")) return false;
			Diagnostic diagnostic;
			const auto status = ResetFlipReplay(
				settings,
				context.Request.Tick,
				context.Request.SimulationAuthoringRevision,
				Limits::MaximumEvaluationBytes,
				output,
				diagnostic
			);
			if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
			output.Data->OriginNodeId = context.Authored.Id;
			output.Data->OriginProcessorRow = context.ProcessorRow;
		}
		auto &settings = output.Data->Settings;
		settings.VelocityDamping = context.Scalar("damping", .8);
		settings.TimeStep = context.Scalar("time_step", .05);
		settings.Gravity = context.Scalar("gravity", 5);
		settings.GravityDirection = context.Scalar("gravity_direction", -90);
		settings.FlipRatio = std::pow(context.Scalar("flip_ratio", .8), .1);
		settings.Friction = std::pow(1 - context.Scalar("friction", 0), .025);
		settings.Viscosity = context.Scalar("viscosity", 0);
		settings.OverRelaxation = context.Scalar("attribute_overrelax", 1.5);
		settings.WallElasticity = context.Scalar("wall_elasticity", 0);
		settings.SkipIncompressible = context.Boolean("attribute_skip_incompressible");
		if (!Count(context, "wall", 15, 15, settings.Wall) ||
			!Count(
				context,
				"attribute_iteration",
				8,
				FluidDomainLimits::MaximumIterations,
				settings.GlobalIterations
			) ||
			!Count(
				context,
				"attribute_iteration_pressure",
				2,
				FluidDomainLimits::MaximumIterations,
				settings.PressureIterations
			) ||
			!Count(
				context,
				"attribute_iteration_particle",
				2,
				FluidDomainLimits::MaximumIterations,
				settings.ParticleIterations
			))
			return false;
		NormalizeFlipFrameTick(*output.Data, context.Request.Tick);
		return PublishFlipDomain(context, std::move(output));
	}
	bool FlipUpdate(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP update input is invalid", "domain");
		const uint64_t bytes = FluidStorageBytes<true>(*input);
		if (!context.ReserveOutput(bytes, "domain")) return false;
		FluidDomainValue output = *input;
		if (context.Boolean("override")) output.Data->Settings.TimeStep = context.Scalar("timestep", .01);
		if (context.Boolean("update", true)) {
			auto temporary = context.ReserveWorkspace(
				bytes + FlipStepGrowthBytes(*input->Data) +
					input->Data->Obstacles.size() * sizeof(source_flip::SourceObstacle),
				"domain"
			);
			if (!temporary) return false;
			const auto target = context.Request.Tick == 0 ? 1 : context.Request.Tick;
			output.Data->Tick = target - 1;
			Diagnostic diagnostic;
			const auto status = StepFlipReplay(
				output,
				target,
				context.Request.SimulationAuthoringRevision,
				Limits::MaximumEvaluationBytes,
				FluidDomainLimits::MaximumWork,
				output,
				diagnostic
			);
			if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
		}
		NormalizeFlipFrameTick(*output.Data, context.Request.Tick);
		return PublishFlipDomain(context, std::move(output));
	}
	bool FlipFill(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP fill input is invalid", "domain");
		const uint64_t bytes = FluidStorageBytes<true>(*input);
		// Source first-frame checks include subframes; the native timeline starts at zero.
		if (context.Request.Tick != 0 || context.Request.Subframe != 0 || context.Request.NegativeFrame) {
			if (!context.ReserveOutput(bytes, "domain")) return false;
			return PublishFlipDomain(context, *input);
		}
		const auto shape = context.Integer("spawn_shape", 0);
		if (shape == 1) return FlipFillSurface(context, *input);
		if (shape != 0)
			return context.Fail(Status::InvalidValue, "FLIP fill source shape is invalid", "spawn_shape");
		if (!context.ReserveOutput(bytes, "domain")) return false;
		Area area = context.Get<Area>(
			"spawn_area", {.CenterX = .5, .CenterY = .5, .HalfWidth = .5, .HalfHeight = .5}
		);
		if (!context.IsLinked("spawn_area") && context.Integer("spawn_area_unit", 1) == 1) {
			area.CenterX *= input->Data->Settings.Width;
			area.HalfWidth *= input->Data->Settings.Width;
			area.CenterY *= input->Data->Settings.Height;
			area.HalfHeight *= input->Data->Settings.Height;
		}
		const auto &settings = input->Data->Settings;
		const double density = std::max(.001, context.Scalar("density", .5) / settings.Spacing);
		const double rows = std::ceil(area.HalfHeight * 2 * density),
					 columns = std::ceil(area.HalfWidth * 2 * density);
		if (!std::isfinite(rows) || !std::isfinite(columns) || rows < 0 || columns < 0 ||
			rows * columns > FluidDomainLimits::MaximumWork)
			return context.Fail(
				Status::LimitExceeded, "FLIP fill point traversal exceeds bounded work", "spawn_area"
			);
		if (rows == 1 || columns == 1)
			return context.Fail(
				Status::UnsupportedExecution,
				"source FLIP fill divides by zero for a single row or column",
				"spawn_area"
			);
		const size_t count =
			std::min(size_t(rows * columns), size_t(settings.MaximumParticles - input->Data->ParticleCount));
		auto scratch = context.ReserveWorkspace(bytes + count * sizeof(FluidSpawnParticle), "domain");
		if (!scratch) return false;
		std::vector<FluidSpawnParticle> particles;
		particles.reserve(count);
		for (size_t row = 0; row < size_t(rows) && particles.size() < count; ++row)
			for (size_t column = 0; column < size_t(columns) && particles.size() < count; ++column) {
				const double x =
					area.CenterX - area.HalfWidth + area.HalfWidth * 2 * double(column) / (columns - 1);
				const double y =
					area.CenterY - area.HalfHeight + area.HalfHeight * 2 * double(row) / (rows - 1);
				particles.push_back(
					{{std::clamp(x + settings.Spacing, 0., settings.Width),
					  std::clamp(y + settings.Spacing, 0., settings.Height)},
					 {}}
				);
			}
		FluidDomainValue output;
		Diagnostic diagnostic;
		const auto status =
			SpawnFlipReplay(*input, particles, Limits::MaximumEvaluationBytes, output, diagnostic);
		if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
		output.Data->SourceParticleCount = input->Data->SourceParticleCount;
		if (!AddSourceFluidParticles(*output.Data, rows * columns))
			return context.Fail(
				Status::LimitExceeded,
				"FLIP fill object particle count exceeds its finite bound",
				"spawn_area"
			);
		return PublishFlipDomain(context, std::move(output));
	}
}
