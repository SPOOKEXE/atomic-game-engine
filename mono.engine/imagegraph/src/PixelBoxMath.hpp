#pragma once

#include <engine/imagegraph/Document.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraph::detail {
	inline std::array<double, 4> PixelBoxBounds(const PixelBoxData &box) {
		if (box.FixedBounds) return *box.FixedBounds;
		const double width = box.BaseBounds[2] - box.BaseBounds[0];
		const double height = box.BaseBounds[3] - box.BaseBounds[1];
		std::array<double, 6> anchors = box.Anchors;
		for (size_t index = 0; index < anchors.size(); ++index)
			if (box.Fractional[index]) anchors[index] *= index % 2 == 0 ? width : height;
		std::array<double, 4> bounds{
			box.BaseBounds[0] + anchors[0],
			box.BaseBounds[1] + anchors[1],
			box.BaseBounds[2] - anchors[2],
			box.BaseBounds[3] - anchors[3]
		};
		for (size_t axis = 0; axis < 2; ++axis) {
			const double center = (bounds[axis] + bounds[axis + 2]) / 2;
			const double extent = std::min(anchors[axis + 4], bounds[axis + 2] - bounds[axis]);
			switch (box.AnchorModes[axis]) {
			case 0:
				bounds[axis] = center - extent / 2;
				bounds[axis + 2] = center + extent / 2;
				break;
			case 1:
				bounds[axis] = bounds[axis + 2] - anchors[axis + 4];
				break;
			case 2:
				bounds[axis + 2] = bounds[axis] + anchors[axis + 4];
				break;
			default:
				break;
			}
			bounds[axis] = std::floor(bounds[axis]);
			bounds[axis + 2] = std::ceil(bounds[axis + 2]);
		}
		return bounds;
	}

	// Absolute left/top setters subtract the base origin; right/bottom are already distances.
	inline void SetPixelBoxAnchor(PixelBoxData &box, size_t index, double value, bool absolute = true) {
		const size_t axis = index % 2;
		if (index < 2 && absolute) value -= box.BaseBounds[axis];
		const double extent = box.BaseBounds[axis + 2] - box.BaseBounds[axis];
		box.Anchors[index] = box.Fractional[index] ? value / extent : value;
	}

	inline void SetPixelBoxBounds(PixelBoxData &box, const std::array<double, 4> &bounds) {
		for (size_t axis = 0; axis < 2; ++axis) {
			const double origin = box.BaseBounds[axis], end = box.BaseBounds[axis + 2];
			const double extent = end - origin;
			const double low = box.Anchors[axis] * (box.Fractional[axis] ? extent : 1);
			const double high = box.Anchors[axis + 2] * (box.Fractional[axis + 2] ? extent : 1);
			const double width = bounds[axis + 2] - bounds[axis];
			switch (box.AnchorModes[axis]) {
			case 2:
				SetPixelBoxAnchor(box, axis, bounds[axis]);
				SetPixelBoxAnchor(box, axis + 4, width);
				break;
			case 1:
				SetPixelBoxAnchor(box, axis + 2, end - bounds[axis + 2]);
				SetPixelBoxAnchor(box, axis + 4, width);
				break;
			case 3:
				SetPixelBoxAnchor(box, axis, bounds[axis]);
				SetPixelBoxAnchor(box, axis + 2, end - bounds[axis + 2]);
				break;
			case 0: {
				SetPixelBoxAnchor(box, axis + 4, width);
				const double center = (bounds[axis] + bounds[axis + 2]) / 2;
				const double previousLow = origin + low, previousHigh = end - high;
				if (box.PreviousAnchorModes[axis] == 2)
					SetPixelBoxAnchor(box, axis, 2 * center - previousHigh - origin, false);
				if (box.PreviousAnchorModes[axis] == 1)
					SetPixelBoxAnchor(box, axis + 2, end - (2 * center - previousLow));
				if (box.PreviousAnchorModes[axis] == 0) {
					const double shift = center - (previousLow + previousHigh) / 2;
					SetPixelBoxAnchor(box, axis, low + shift, false);
					SetPixelBoxAnchor(box, axis + 2, high - shift);
				}
				break;
			}
			}
			box.PreviousAnchorModes[axis] = box.AnchorModes[axis];
		}
	}
}
