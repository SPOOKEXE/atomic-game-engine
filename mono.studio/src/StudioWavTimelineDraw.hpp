#pragma once
#include <engine/imagegraph/WavTimelinePresentation.hpp>

#include <algorithm>
#include <cmath>
#include <imgui.h>
namespace studio {
	// Clip in double signal coordinates before converting to float ImGui vertices.
	// Scaling the intersection ratio avoids overflow for finite opposite-sign amplitudes.
	inline bool
	ClipWavSegment(engine::imagegraph::Vector2 &a, engine::imagegraph::Vector2 &b, double frames) {
		const auto code = [&](auto point) {
			return (point.X < 0 ? 1 : point.X > frames ? 2 : 0) | (point.Y < -.5 ? 4 : point.Y > .5 ? 8 : 0);
		};
		for (unsigned pass = 0; pass < 8; ++pass) {
			const auto ca = code(a), cb = code(b);
			if (!(ca | cb)) return true;
			if (ca & cb) return false;
			const auto outside = ca ? ca : cb;
			engine::imagegraph::Vector2 p;
			if (outside & 12) {
				p.Y = outside & 4 ? -.5 : .5;
				const double scale = std::max({std::abs(a.Y), std::abs(b.Y), 1.});
				const double t =
					std::clamp((p.Y / scale - a.Y / scale) / (b.Y / scale - a.Y / scale), 0., 1.);
				p.X = a.X + (b.X - a.X) * t;
			} else {
				p.X = outside & 1 ? 0 : frames;
				const double t = std::clamp((p.X - a.X) / (b.X - a.X), 0., 1.);
				p.Y = (1 - t) * a.Y + t * b.Y;
			}
			if (ca)
				a = p;
			else
				b = p;
		}
		return false;
	}
	// Caller provides an observation cached by document/input revisions and exact
	// signed FrameTime.
	inline void DrawWavTimelineObservation(
		const engine::imagegraph::WavTimelinePresentation &observation,
		float pixelsPerFrame,
		float width,
		float height
	) {
		const auto origin = ImGui::GetCursorScreenPos();

		ImGui::InvisibleButton("##wav-timeline-observation", {std::max(1.f, width), height});
		auto *draw = ImGui::GetWindowDrawList();
		draw->PushClipRect(origin, {origin.x + width, origin.y + height}, true);
		for (size_t i = 1; i < observation.Points.size(); ++i) {
			auto a = observation.Points[i - 1], b = observation.Points[i];
			if (!ClipWavSegment(a, b, width / pixelsPerFrame)) continue;
			draw->AddLine(
				{origin.x + float(a.X) * pixelsPerFrame, origin.y + height / 2 + float(a.Y) * height},
				{origin.x + float(b.X) * pixelsPerFrame, origin.y + height / 2 + float(b.Y) * height},
				0xffffffff
			);
		}
		const float x = origin.x + width * float(observation.Progress);
		draw->AddLine({x, origin.y}, {x, origin.y + height}, 0xffffffff);
		draw->PopClipRect();
	}
} // namespace studio
