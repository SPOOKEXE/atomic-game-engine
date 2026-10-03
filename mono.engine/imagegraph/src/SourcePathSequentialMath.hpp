#pragma once

#include "SourcePathShape.hpp"

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <span>

namespace engine::imagegraph::detail {
	inline void SourceSequentialResetPoint(SourcePathPointBuffer &out) {
		out.Position = {};
	}
	inline bool SourceSequentialFinitePoint(const SourcePathPointBuffer &point) {
		return std::isfinite(point.Position.X) && std::isfinite(point.Position.Y) &&
			   std::isfinite(point.Weight) && (!point.Z || std::isfinite(*point.Z)) &&
			   (point.Class != SourcePathPointClass::Spatial || point.Z.has_value());
	}
	inline double SourceSequentialWrap(double ratio, bool loop) {
		if (!loop) return std::clamp(ratio, 0., .999);
		const double first = ratio - std::trunc(ratio), sum = first + 1;
		return sum - std::trunc(sum);
	}
	// A sampler mutates its supplied object and may return a replacement of a
	// different source class. Smoothen assigns that replacement to its own buffers.
	template <class Sample>
	void SourceSequentialSmooth(
		double ratio,
		size_t line,
		const SourcePathSequentialData2D &controls,
		SourcePathPointBuffer &point,
		SourcePathPointBuffer &probe,
		SourcePathPointBuffer &out,
		Sample &&sample
	) {
		SourceSequentialResetPoint(out);
		point = sample(ratio, line, point);
		if (ratio < controls.SmoothRange.X || ratio > controls.SmoothRange.Y) {
			out.Position = point.Position;
			out.Weight = point.Weight;
			return;
		}
		double x = 0, y = 0, amplitude = 1, weight = 0, offset = controls.SmoothSpan;
		for (int64_t i = 0; i < controls.SmoothSteps; ++i) {
			probe = sample(SourceSequentialWrap(ratio - offset, controls.SmoothLoop), line, probe);
			x += probe.Position.X * amplitude;
			y += probe.Position.Y * amplitude;
			probe = sample(SourceSequentialWrap(ratio + offset, controls.SmoothLoop), line, probe);
			x += probe.Position.X * amplitude;
			y += probe.Position.Y * amplitude;
			weight += amplitude * 2;
			offset += controls.SmoothSpan;
			amplitude *= .5;
		}
		x /= weight;
		y /= weight;
		out.Position = {
			point.Position.X + (x - point.Position.X) * controls.SmoothBlend,
			point.Position.Y + (y - point.Position.Y) * controls.SmoothBlend
		};
		out.Weight = point.Weight;
	}
	// Extends intentionally ignores a child replacement, as its source wrapper
	// calls getPointRatio without assigning the returned point.
	template <class Sample>
	void SourceSequentialExtend(
		double distance,
		const SourcePathSequentialData2D &controls,
		SourcePathPointBuffer &out,
		Sample &&sample
	) {
		SourceSequentialResetPoint(out);
		if ((controls.ExtendSide == 0 && distance < controls.ExtendLength) ||
			(controls.ExtendSide == 1 && distance > controls.CachedLength)) {
			const double amount = controls.ExtendSide == 0 ? controls.ExtendLength - distance
														   : distance - controls.CachedLength;
			const double angle =
				(controls.ExtendSide == 0 ? controls.ExtendStartDirection : controls.ExtendEndDirection) *
				std::numbers::pi / 180;
			const auto &point =
				controls.ExtendSide == 0 ? controls.ExtendStartPoint : controls.ExtendEndPoint;
			out.Position = {
				point.Position.X + SourceShapeLengthdirComponent(amount * std::cos(angle)),
				point.Position.Y + SourceShapeLengthdirComponent(-amount * std::sin(angle))
			};
		} else {
			const double ratio = controls.ExtendSide == 0
									 ? (distance - controls.ExtendLength) / controls.CachedLength
									 : distance / controls.CachedLength;
			(void)sample(ratio, size_t{0}, out);
		}
	}
	// The flattened entry index, rather than the child-local line index, is
	// forwarded to the original child. Its fallback uses the complete length.
	template <class Sample>
	void SourceSequentialFlatten(
		double distance,
		const SourcePathSequentialData2D &controls,
		size_t originalChildren,
		SourcePathPointBuffer &out,
		Sample &&sample
	) {
		SourceSequentialResetPoint(out);
		double remaining = distance;
		bool reversed = controls.FlattenReverse;
		for (size_t i = 0; i < controls.FlattenLengths.size(); ++i) {
			const double length = controls.FlattenLengths[i];
			if (length >= remaining) {
				if (reversed) remaining = length - remaining;
				(void)sample(controls.FlattenOwners[i], remaining, i, out);
				return;
			}
			remaining -= length;
			if (controls.FlattenPingPong) reversed = !reversed;
		}
		if (originalChildren) (void)sample(originalChildren - 1, controls.CachedLength, size_t{0}, out);
	}
}
