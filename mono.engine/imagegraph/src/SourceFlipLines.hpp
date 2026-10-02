#pragma once
#include "FluidPayload.hpp"
#include "NodeExecutors.hpp"
#include "SourceGradient.hpp"
#include "SourceRandom.hpp"
#include "nodes/Sampler.hpp"
namespace engine::imagegraph::detail {
	inline Vector2 SourceFlipHistoryPosition(const FluidDomainData &data, uint64_t frame, size_t particle) {
		const auto found = std::lower_bound(
			data.History.begin(), data.History.end(), frame, [](const auto &entry, uint64_t tick) {
				return entry.Tick < tick;
			}
		);
		if (found == data.History.end() || found->Tick != frame || particle >= found->Positions.size() / 2)
			return {};
		return {found->Positions[particle * 2], found->Positions[particle * 2 + 1]};
	}
	// Missing stored slots represent the source array_create zero-filled history,
	// not interpolated solver poses. Each snapshot holds the previous readback.
	template <class DrawLine>
	bool VisitSourceFlipLines(
		NodeContext &context,
		const FluidDomainData &data,
		const Gradient *gradient,
		Vector2 lifespan,
		Vector2 velocityMap,
		double shift,
		int64_t segments,
		double thickness,
		DrawLine draw
	) {
		if (context.Request.NegativeFrame || context.Request.Subframe != 0)
			return context.Fail(
				Status::UnsupportedExecution,
				"FLIP line history requires integral nonnegative source array indices",
				"segments"
			);
		if (!MeshFinite(lifespan) || !MeshFinite(velocityMap) || !std::isfinite(shift) ||
			!std::isfinite(thickness) || (gradient && gradient->Keys.empty()))
			return context.Fail(Status::InvalidValue, "FLIP line controls are invalid");
		const uint64_t frame = context.Request.Tick;
		const double selectedSegments = std::min(double(segments), double(frame));
		if (selectedSegments > Limits::MaximumRangeFrames)
			return context.Fail(
				Status::LimitExceeded, "FLIP line history exceeds bounded lookback", "segments"
			);
		const double firstFrame = std::max(0., double(frame) - selectedSegments);
		const size_t count =
			std::min(size_t(data.Settings.MaximumParticles - 1), size_t(SourceFluidParticleCount(data)));
		if (count && selectedSegments > 0 &&
			selectedSegments > double(FluidDomainLimits::MaximumWork / count))
			return context.Fail(
				Status::LimitExceeded, "FLIP line history exceeds bounded segment work", "segments"
			);
		SourceRandom random{uint32_t(context.Request.Seed)};
		Colour color = gradient ? gradient->Keys.front().Color : Colour{255, 255, 255, 255};
		for (size_t particle = 0; particle < count; ++particle) {
			const double life = particle < data.ReadbackLife.size() ? data.ReadbackLife[particle] : 0;
			const int64_t duration = random.IntRange(lifespan.X, lifespan.Y);
			const double lastFrame =
				duration ? std::min(double(frame), double(frame) - life + double(duration)) : double(frame);
			if (lastFrame <= firstFrame) continue;
			if (lastFrame != std::trunc(lastFrame) || firstFrame != std::trunc(firstFrame))
				return context.Fail(
					Status::UnsupportedExecution,
					"FLIP line lifetime produces a fractional source history index",
					"lifespan"
				);
			Vector2 old =
				lastFrame == double(frame)
					? (particle < data.ReadbackPositions.size() / 2
						   ? Vector2{data.ReadbackPositions[particle * 2], data.ReadbackPositions[particle * 2 + 1]}
						   : Vector2{})
					: SourceFlipHistoryPosition(data, uint64_t(lastFrame) + 1, particle);
			if (old.X == 0 && old.Y == 0) continue;
			old.X -= data.Settings.Spacing;
			old.Y -= data.Settings.Spacing;
			for (uint64_t j = uint64_t(lastFrame); j > uint64_t(firstFrame); --j) {
				auto next = SourceFlipHistoryPosition(data, j, particle);
				if (next.X == 0 && next.Y == 0) continue;
				if (gradient && gradient->Keys.size() > 1) {
					// The source compares the adjusted old endpoint with the raw new endpoint.
					const double dx = old.X - next.X, dy = old.Y - next.Y;
					const double ratio =
						(std::sqrt(dx * dx + dy * dy) - velocityMap.X) / (velocityMap.Y - velocityMap.X);
					if (!std::isfinite(ratio))
						return context.Fail(
							Status::InvalidValue,
							"FLIP line velocity gradient ratio is undefined",
							"velocity_map"
						);
					const auto mapped = SourceCachedGradient(
						*gradient, Fract(std::pow(std::clamp(ratio, 0., 1.), 5) + shift)
					);
					if (!mapped)
						return context.Fail(
							Status::InvalidValue,
							"FLIP line gradient color is undefined",
							"color_over_velocity"
						);
					color = *mapped;
				}
				next.X -= data.Settings.Spacing;
				next.Y -= data.Settings.Spacing;
				if (!draw(old, next, thickness, color)) return false;
				old = next;
			}
		}
		return true;
	}
}
