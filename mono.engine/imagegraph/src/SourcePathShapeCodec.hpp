#pragma once
#include "SourcePathShape.hpp"

#include <iomanip>
#include <istream>
#include <ostream>
#include <string>

namespace engine::imagegraph::detail {
	inline void WriteSourcePathShape(std::ostream &stream, const SourcePathShapeData2D &shape) {
		stream << std::quoted(std::string(SourcePathShapeNames[size_t(shape.Kind)])) << ' ' << shape.Loop
			   << ' ' << shape.Points.size() << ' ' << std::setprecision(17) << shape.Position.X << ' '
			   << shape.Position.Y << ' ' << shape.HalfSize.X << ' ' << shape.HalfSize.Y << ' '
			   << shape.AngleRange.X << ' ' << shape.AngleRange.Y << ' ' << shape.Rotation << ' '
			   << shape.Factor;
		for (const auto &point : shape.Points)
			stream << ' ' << point.X << ' ' << point.Y;
	}
	// The operation slot is admitted by the caller; admit sampled geometry before
	// allocating it.
	template <class Admit>
	bool ReadSourcePathShape(std::istream &stream, SourcePathShapeData2D &shape, Admit admit) {
		std::array<char, 16> nameBytes{};
		size_t nameLength = 0;
		int loop = 0;
		size_t count = 0;
		// Names are bounded fixed vocabulary; read their quoted bytes without an
		// unbounded string allocation.
		if (!(stream >> std::ws) || stream.get() != '"') return false;
		for (size_t index = 0; index <= 16; ++index) {
			const int character = stream.get();
			if (character == '"') break;
			if (character == std::char_traits<char>::eof() || index == nameBytes.size()) return false;
			nameBytes[nameLength++] = char(character);
		}
		const auto found = std::find(
			SourcePathShapeNames.begin(),
			SourcePathShapeNames.end(),
			std::string_view{nameBytes.data(), nameLength}
		);
		if (found == SourcePathShapeNames.end() || !(stream >> loop >> count) || (loop != 0 && loop != 1) ||
			count > Limits::MaximumPathAnchors ||
			!(stream >> shape.Position.X >> shape.Position.Y >> shape.HalfSize.X >> shape.HalfSize.Y >>
			  shape.AngleRange.X >> shape.AngleRange.Y >> shape.Rotation >> shape.Factor))
			return false;
		shape.Kind = SourcePathShapeKind2D(found - SourcePathShapeNames.begin());
		shape.Loop = loop != 0;
		if (!ValidSourcePathShape(shape) || !admit(count * sizeof(Vector2))) return false;
		shape.Points.resize(count);
		for (auto &point : shape.Points)
			if (!(stream >> point.X >> point.Y) || !std::isfinite(point.X) || !std::isfinite(point.Y))
				return false;
		return true;
	}
} // namespace engine::imagegraph::detail
