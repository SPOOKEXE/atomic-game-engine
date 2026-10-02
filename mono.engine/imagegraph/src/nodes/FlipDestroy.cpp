#include "FlipNodes.hpp"
#include "SourceBuiltinRandomContext.hpp"

#include <climits>
namespace engine::imagegraph::detail {
	bool FlipDestroy(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) {
			context.SetValue("domain", UndefinedValue{});
			return true;
		}
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP destroy input is invalid", "domain");
		const int64_t shape = context.Integer("shape", 0);
		if (shape < 0 || shape > 2)
			return context.Fail(Status::InvalidValue, "FLIP destroy shape is invalid", "shape");
		if (shape == 2)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP destroy surface requires the bounded source mask raster profile",
				"mask"
			);
		Vector2 position = context.Vec2("position", {}), size = context.Vec2("size", {4, 4});
		if (!context.IsLinked("position") && context.Integer("position_unit", 1) == 1) {
			position.X *= context.Project.SurfaceWidth;
			position.Y *= context.Project.SurfaceHeight;
		}
		const double radius = context.Scalar("radius", 4), chance = context.Scalar("chance", 1),
					 squaredRadius = radius * radius;
		if (!std::isfinite(position.X) || !std::isfinite(position.Y) || !std::isfinite(size.X) ||
			!std::isfinite(size.Y) || !std::isfinite(radius) || !std::isfinite(squaredRadius) ||
			!std::isfinite(chance))
			return context.Fail(Status::InvalidValue, "FLIP destroy controls are nonfinite");
		const SourceBuiltinRandomCapture *capture = nullptr;
		const size_t count = input->Data->ParticleCount;
		if (count) {
			if (!FindSourceBuiltinRandomCapture(context, capture)) return false;
			if (capture->Draws.size() != count)
				return context.Fail(
					Status::InvalidValue,
					"FLIP destroy requires one captured C rand draw per solver particle",
					"chance"
				);
			for (const auto &draw : capture->Draws)
				if (draw.Operation != SourceBuiltinRandomOperation::CRand || draw.Lower != 0 ||
					draw.Upper != 0 || !std::isfinite(draw.Result) || draw.Result < 0 ||
					draw.Result > INT_MAX || draw.Result != std::trunc(draw.Result))
					return context.Fail(
						Status::InvalidValue, "FLIP destroy captured C rand operation is invalid", "chance"
					);
		}
		if (!context.ReserveOutput(FluidStorageBytes<true>(*input), "domain")) return false;
		FluidDomainValue output = *input;
		auto &points = output.Data->Buffers[size_t(FluidBuffer::ParticlePosition)];
		for (size_t index = 0; index < count; ++index) {
			// Source consumes a C rand draw even when chance or geometry excludes this
			// slot.
			if (double(uint32_t(capture->Draws[index].Result) % 100) / 100 > chance) continue;
			const double x = points[index * 2], y = points[index * 2 + 1], dx = x - position.X,
						 dy = y - position.Y;
			const bool inside = shape == 0 ? dx * dx + dy * dy < squaredRadius
										   : x > position.X - size.X && x < position.X + size.X &&
												 y > position.Y - size.Y && y < position.Y + size.Y;
			if (inside) points[index * 2] = points[index * 2 + 1] = 0;
		}
		return PublishFlipDomain(context, std::move(output));
	}
} // namespace engine::imagegraph::detail
