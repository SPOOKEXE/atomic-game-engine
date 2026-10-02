#include "FlipNodes.hpp"

#include <numbers>
namespace engine::imagegraph::detail {
	bool FlipParticleForce(NodeContext &context) {
		const auto *input = FindFlipDomainInput(context);
		if (!input || !input->Data) return true;
		if (!ValidFluidPayload(*input))
			return context.Fail(Status::InvalidValue, "FLIP force domain is invalid", "domain");
		const bool repel = context.Authored.Type == "pc.flip_repel",
				   vortex = context.Authored.Type == "pc.flip_vortex";
		const Vector2 position =
			FlipPosition(context, *input->Data, (repel || vortex) ? Vector2{.5, .5} : Vector2{});
		const Vector2 velocity = context.Vec2("velocity"), size = context.Vec2("size", {4, 4});
		const double radius = context.Scalar("radius", 4),
					 strength = context.Scalar("strength", 4) * (repel ? 8 : 1),
					 attraction = context.Scalar("attraction", 0);
		const auto shape = (repel || vortex) ? 0 : context.Integer("shape", 0);
		if (!MeshFinite(position) || !MeshFinite(velocity) || !MeshFinite(size) || !std::isfinite(radius) ||
			!std::isfinite(strength) || !std::isfinite(attraction))
			return context.Fail(Status::InvalidValue, "FLIP force controls must be finite");
		if (shape < 0 || shape > 1)
			return context.Fail(Status::InvalidValue, "FLIP velocity shape is invalid", "shape");
		if (!context.ReserveOutput(FluidStorageBytes<true>(*input), "domain")) return false;
		FluidDomainValue output = *input;
		auto &data = *output.Data;
		const auto &points = data.Buffers[size_t(FluidBuffer::ParticlePosition)];
		auto &velocities = data.Buffers[size_t(FluidBuffer::ParticleVelocity)];
		// The upstream native loops start at one; particle zero receives no affector impulse.
		for (size_t index = 1; index < data.ParticleCount; ++index) {
			const double x = points[index * 2], y = points[index * 2 + 1];
			if (x == 0 && y == 0) continue;
			const double dx = x - position.X, dy = y - position.Y;
			if (repel || vortex) {
				const double distance = std::sqrt(dx * dx + dy * dy);
				if (distance < radius) {
					const double angle = std::atan2(dy, dx);
					if (repel) {
						velocities[index * 2] += (1 - distance / radius) * std::cos(angle) * strength;
						velocities[index * 2 + 1] += (1 - distance / radius) * std::sin(angle) * strength;
					} else {
						velocities[index * 2] +=
							(1 - distance / radius) * std::cos(angle + std::numbers::pi / 2) * strength -
							std::cos(angle) * attraction;
						velocities[index * 2 + 1] +=
							(1 - distance / radius) * std::sin(angle + std::numbers::pi / 2) * strength -
							std::sin(angle) * attraction;
					}
				}
			} else {
				// The source wrapper offsets the rectangle center by its half-size before the native call.
				const bool inside = shape == 0 ? dx * dx + dy * dy < radius * radius
											   : (x > position.X - size.X * 2 && x < position.X &&
												  y > position.Y - size.Y * 2 && y < position.Y);
				if (inside) {
					velocities[index * 2] += velocity.X;
					velocities[index * 2 + 1] += velocity.Y;
				}
			}
		}
		return PublishFlipDomain(context, std::move(output));
	}
}
