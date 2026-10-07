#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cmath>

namespace engine::imagegraph::detail {
	inline double SourceBakedLength(const std::vector<Vector3> &line) {
		double length = 0;
		for (size_t index = 1; index < line.size(); ++index)
			length += std::hypot(line[index].X - line[index - 1].X, line[index].Y - line[index - 1].Y);
		return length;
	}
	inline bool ValidSourceBaked(const SourcePathBakedData2D &data, size_t &count) {
		if (data.Lines.size() > Limits::MaximumArrayElements - count) return false;
		count += data.Lines.size();
		for (const auto &line : data.Lines) {
			if (line.size() > (Limits::MaximumArrayElements - count) / 4) return false;
			count += line.size() * 4;
			for (const auto &point : line)
				if (!std::isfinite(point.X) || !std::isfinite(point.Y) || !std::isfinite(point.Z))
					return false;
			if (!std::isfinite(SourceBakedLength(line))) return false;
		}
		return true;
	}
	template <bool Retained> uint64_t SourceBakedBytes(const SourcePathBakedData2D &data) {
		uint64_t bytes = sizeof(data) + (Retained ? data.Lines.capacity() : data.Lines.size()) *
											sizeof(std::vector<Vector3>);
		for (const auto &line : data.Lines)
			bytes += (Retained ? line.capacity() : line.size()) * sizeof(Vector3);
		return bytes;
	}
	inline Vector2 SourceBakedPoint(const std::vector<Vector3> &line, double distance) {
		const double length = SourceBakedLength(line);
		if (length <= 0 || line.size() < 2) return {};
		distance = std::fmod(distance, length);
		double accumulated = 0;
		for (size_t index = 1; index < line.size(); ++index) {
			const auto &from = line[index - 1], &to = line[index];
			const double next = accumulated + std::hypot(to.X - from.X, to.Y - from.Y);
			if (distance < next || index + 1 == line.size()) {
				const double ratio = (distance - accumulated) / (next - accumulated);
				return {from.X + (to.X - from.X) * ratio, from.Y + (to.Y - from.Y) * ratio};
			}
			accumulated = next;
		}
		return {};
	}
}
