#include "../SourceFlipAngle.hpp"
#include "FlipNodes.hpp"
#include "SourceFlipDistribution.hpp"

#include <engine/imagegraph/FlipReplay.hpp>
namespace engine::imagegraph::detail {
	namespace {
		uint32_t FlipSeed(double value) {
			double wrapped = std::fmod(std::trunc(value), 4294967296.);
			if (wrapped < 0) wrapped += 4294967296.;
			return uint32_t(wrapped);
		}
	} // namespace
	bool FlipSpawner(NodeContext &context) {
		if (context.Request.NegativeFrame || context.Request.Subframe != 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP fixed-step spawner requires an integral "
				"nonnegative source frame"
			);
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP spawner domain is invalid", "domain");
		if (context.Request.ReuseSimulationFrame) return ReuseFlipDomain(context, *input);
		const auto shape = context.Integer("spawn_shape", 0), type = context.Integer("spawn_type", 0);
		if (shape < 0 || shape > 2 || type < 0 || type > 1)
			return context.Fail(Status::InvalidValue, "FLIP spawn mode is invalid");
		Vector2 position = context.Vec2("spawn_position", {.5, .25}),
				size = context.Vec2("spawn_size", {2, 2}), velocity = context.Vec2("spawn_velocity", {});
		if (!context.IsLinked("spawn_position") && context.Integer("spawn_position_unit", 1) == 1) {
			position.X *= input->Data->Settings.Width;
			position.Y *= input->Data->Settings.Height;
		}
		const double radius = context.Scalar("spawn_radius", 2),
					 requested = context.Scalar("spawn_amount", 8),
					 inherit = context.Scalar("inherit_velocity", 0), seed = context.Scalar("seed", 0);
		for (const double value :
			 {position.X,
			  position.Y,
			  size.X,
			  size.Y,
			  velocity.X,
			  velocity.Y,
			  radius,
			  requested,
			  inherit,
			  seed})
			if (!std::isfinite(value))
				return context.Fail(Status::InvalidValue, "FLIP spawn control is nonfinite");
		std::array<double, 6> direction{};
		size_t directionSize = 0;
		const Value *directionInput = context.Find("spawn_direction");
		if (const auto *array = directionInput ? std::get_if<ArrayValue>(directionInput) : nullptr) {
			if (!array->Items.empty() || !array->Nested.empty() || array->Elements.size() > direction.size())
				return context.Fail(
					Status::UnsupportedExecution,
					"FLIP rotation range requires one source scalar row",
					"spawn_direction"
				);
			for (const auto &element : array->Elements) {
				const auto number = std::visit(
					[](const auto &value) -> std::optional<double> {
						using T = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t>)
							return double(value);
						else
							return std::nullopt;
					},
					element
				);
				if (!number || !std::isfinite(*number))
					return context.Fail(
						Status::InvalidValue, "FLIP rotation range is nonfinite", "spawn_direction"
					);
				direction[directionSize++] = *number;
			}
		} else
			return context.Fail(Status::InvalidValue, "FLIP rotation range is absent", "spawn_direction");
		FluidDomainData::SpawnerState state;
		state.NodeId = context.Authored.Id;
		state.ProcessorRow = context.ProcessorRow;
		bool priorFound = false;
		auto consider = [&](const FluidDomainData &data) {
			for (const auto &item : data.Spawners)
				if (item.NodeId == state.NodeId && item.ProcessorRow == state.ProcessorRow &&
					(!priorFound || item.Tick > state.Tick)) {
					state = item;
					priorFound = true;
				}
		};
		consider(*input->Data);
		if (context.CurrentSimulation)
			for (const auto &entry : context.CurrentSimulation->Entries)
				if (entry.Fluid.Data &&
					entry.State.AuthoringRevision == context.Request.SimulationAuthoringRevision)
					consider(*entry.Fluid.Data);
		if (context.Request.Tick == 0 || !priorFound) state.Accumulator = 0;
		const double amount = std::min(
			requested, double(input->Data->Settings.MaximumParticles) - SourceFluidParticleCount(*input->Data)
		);
		state.Accumulator += amount;
		state.Tick = context.Request.Tick;
		if (!std::isfinite(state.Accumulator) || std::abs(state.Accumulator) > FluidDomainLimits::MaximumWork)
			return context.Fail(
				Status::LimitExceeded, "FLIP spawn accumulator exceeds bounded work", "spawn_amount"
			);
		const auto start = context.Integer("spawn_frame", 0), duration = context.Integer("spawn_duration", 1);
		bool spawn = state.Accumulator >= 1;
		if (type == 1 && (double(context.Request.Tick) < double(start) ||
						  double(context.Request.Tick) >= double(start) + double(duration)))
			spawn = false;
		const Image *surface = shape == 2 ? context.Input("spawn_surface") : nullptr;
		if (shape == 2 && !surface) spawn = false;
		size_t count = spawn ? size_t(std::floor(state.Accumulator)) : 0;
		if (count > FluidDomainLimits::MaximumParticles)
			return context.Fail(
				Status::LimitExceeded,
				"FLIP source spawn buffer exceeds bounded particle work",
				"spawn_amount"
			);
		if (spawn) state.Accumulator -= double(count);
		const auto found =
			std::find_if(input->Data->Spawners.begin(), input->Data->Spawners.end(), [&](const auto &item) {
				return item.NodeId == state.NodeId && item.ProcessorRow == state.ProcessorRow;
			});
		const bool newSlot = found == input->Data->Spawners.end();
		if (newSlot && input->Data->Spawners.size() >= Limits::MaximumNodes)
			return context.Fail(
				Status::LimitExceeded, "FLIP spawner state slots exceed bounded node identities"
			);
		uint64_t bytes = FluidStorageBytes<true>(*input);
		if (newSlot)
			bytes =
				MeshAddBytes(bytes, sizeof(state) + std::max(state.NodeId.size(), std::string{}.capacity()));
		if (!context.ReserveOutput(bytes, "domain")) return false;
		auto scratch = context.ReserveWorkspace(
			bytes + count * (sizeof(FluidSpawnParticle) + sizeof(Vector2)), "domain"
		);
		if (!scratch) return false;
		std::vector<Vector2> distributed;
		if (shape == 2 && count) {
			if (!SourceFlipDistribution(
					context,
					*surface,
					count,
					context.Integer("attempt", 8),
					seed + std::ceil(amount) * double(context.Request.Tick),
					distributed
				))
				return false;
			count = distributed.size();
		}
		std::vector<FluidSpawnParticle> particles;
		particles.reserve(count);
		const double initialSeed = seed + (std::ceil(amount) + 10) * double(context.Request.Tick);
		if (!std::isfinite(initialSeed))
			return context.Fail(Status::InvalidValue, "FLIP spawn seed is nonfinite", "seed");
		SourceFlipRandom random(FlipSeed(initialSeed));
		for (size_t index = 0; index < count; ++index) {
			double x = position.X, y = position.Y;
			if (shape == 0) {
				const double angle = random.Stream.Unit() * 2 * std::numbers::pi,
							 distance = std::sqrt(random.Stream.Unit()) * radius;
				x += std::cos(angle) * distance;
				y -= std::sin(angle) * distance;
			} else if (shape == 1) {
				x += random.Stream.Range(-size.X, size.X);
				y += random.Stream.Range(-size.Y, size.Y);
			} else {
				x = position.X - double(surface->Width) / 2 + distributed[index].X * surface->Width;
				y = position.Y - double(surface->Height) / 2 + distributed[index].Y * surface->Height;
			}
			const double speed = random.Stream.Range(velocity.X, velocity.Y);
			const auto angle = random.Angle(std::span(direction).first(directionSize), index);
			if (!angle)
				return context.Fail(
					Status::UnsupportedExecution, "FLIP source rotation mode is undefined", "spawn_direction"
				);
			const double radians = *angle * std::numbers::pi / 180;
			particles.push_back(
				{{std::clamp(x + input->Data->Settings.Spacing, 0., input->Data->Settings.Width),
				  std::clamp(y + input->Data->Settings.Spacing, 0., input->Data->Settings.Height)},
				 {std::cos(radians) * speed +
					  (context.Request.Tick ? (position.X - state.PreviousPosition[0]) * inherit : 0),
				  -std::sin(radians) * speed +
					  (context.Request.Tick ? (position.Y - state.PreviousPosition[1]) * inherit : 0)}}
			);
		}
		FluidDomainValue output;
		Diagnostic diagnostic;
		const auto status =
			SpawnFlipReplay(*input, particles, Limits::MaximumEvaluationBytes, output, diagnostic);
		if (status != Status::Ok) return context.Fail(status, diagnostic.Message, "domain");
		if (count) {
			output.Data->SourceParticleCount = input->Data->SourceParticleCount;
			if (!AddSourceFluidParticles(*output.Data, double(count)))
				return context.Fail(
					Status::LimitExceeded, "FLIP source spawner count exceeds bounded work", "spawn_amount"
				);
			state.PreviousPosition = {position.X, position.Y};
		}
		auto &states = output.Data->Spawners;
		if (newSlot) {
			states.reserve(states.size() + 1);
			states.push_back(std::move(state));
			std::sort(states.begin(), states.end(), [](const auto &a, const auto &b) {
				return std::tie(a.NodeId, a.ProcessorRow) < std::tie(b.NodeId, b.ProcessorRow);
			});
		} else
			states[size_t(found - input->Data->Spawners.begin())] = std::move(state);
		return PublishFlipDomain(context, std::move(output));
	}
} // namespace engine::imagegraph::detail
