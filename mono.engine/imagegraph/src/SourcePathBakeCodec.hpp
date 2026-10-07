#pragma once

#include "SourcePathBakePayload.hpp"

#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteSourceBaked(std::ostream &stream, const SourcePathBakedData2D &data) {
		stream << data.Lines.size();
		for (const auto &line : data.Lines) {
			stream << ' ' << line.size();
			for (const auto &point : line)
				stream << ' ' << std::setprecision(17) << point.X << ' ' << point.Y << ' ' << point.Z;
		}
	}
	template <class Admit>
	bool ReadSourceBaked(std::istream &stream, SourcePathBakedData2D &data, Admit admit) {
		size_t lines = 0, count = 1;
		if (!(stream >> lines) || lines > Limits::MaximumArrayElements - count ||
			!admit(lines * sizeof(std::vector<Vector3>)))
			return false;
		count += lines;
		data.Lines.reserve(lines);
		for (size_t index = 0; index < lines; ++index) {
			size_t points = 0;
			if (!(stream >> points) || points > (Limits::MaximumArrayElements - count) / 4 ||
				!admit(points * sizeof(Vector3)))
				return false;
			count += points * 4;
			auto &line = data.Lines.emplace_back();
			line.resize(points);
			for (auto &point : line)
				if (!(stream >> point.X >> point.Y >> point.Z)) return false;
		}
		count = 1;
		return ValidSourceBaked(data, count);
	}
}
