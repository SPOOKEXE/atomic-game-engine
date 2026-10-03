#pragma once

#include "SourcePathSequentialMath.hpp"
#include "SourcePathSequentialPayload.hpp"

#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteSourceSequentialBuffer(std::ostream &stream, const SourcePathPointBuffer &point) {
		stream << ' ' << (point.Class == SourcePathPointClass::Spatial ? "spatial" : "planar") << ' '
			   << point.Position.X << ' ' << point.Position.Y << ' ' << point.Weight << ' ' << bool(point.Z)
			   << ' ' << point.Z.value_or(0);
	}
	inline bool ReadSourceSequentialFlag(std::istream &stream, bool &flag) {
		unsigned value = 0;
		if (!(stream >> value) || value > 1) return false;
		flag = value != 0;
		return true;
	}
	inline bool ReadSourceSequentialBuffer(std::istream &stream, SourcePathPointBuffer &point) {
		// The class tag has a fixed six/seven character bound.
		std::array<char, 7> tag{};
		stream >> std::ws;
		size_t length = 0;
		while (stream.peek() != std::char_traits<char>::eof() &&
			   !std::isspace(static_cast<unsigned char>(stream.peek()))) {
			if (length == tag.size()) return false;
			tag[length++] = char(stream.get());
		}
		const std::string_view name(tag.data(), length);
		if (name != "planar" && name != "spatial") return false;
		point.Class = name == "spatial" ? SourcePathPointClass::Spatial : SourcePathPointClass::Planar;
		bool present = false;
		double z = 0;
		if (!(stream >> point.Position.X >> point.Position.Y >> point.Weight) ||
			!ReadSourceSequentialFlag(stream, present) || !(stream >> z) || !std::isfinite(z) ||
			(!present && z != 0))
			return false;
		if (present) point.Z = z;
		return SourceSequentialFinitePoint(point);
	}
	inline void WriteSourceSequential(std::ostream &stream, const SourcePathSequentialData2D &s) {
		stream << ' ' << std::setprecision(17) << unsigned(s.ExtendSide) << ' ' << s.ExtendLength << ' '
			   << s.FlattenReverse << ' ' << s.FlattenPingPong << ' ' << s.SmoothRange.X << ' '
			   << s.SmoothRange.Y << ' ' << s.SmoothClampCurve << ' ' << s.SmoothLoop << ' ' << s.SmoothSpan
			   << ' ' << s.SmoothBlend << ' ' << s.SmoothSteps << ' ' << s.CachedLength << ' '
			   << s.CachedSegments << ' ' << s.CachedBounds.X << ' ' << s.CachedBounds.Y << ' '
			   << s.CachedBounds.Z << ' ' << s.CachedBounds.W << ' ' << s.ExtendStartPoint.Position.X << ' '
			   << s.ExtendStartPoint.Position.Y << ' ' << s.ExtendStartPoint.Weight << ' '
			   << s.ExtendEndPoint.Position.X << ' ' << s.ExtendEndPoint.Position.Y << ' '
			   << s.ExtendEndPoint.Weight << ' ' << s.ExtendStartDirection << ' ' << s.ExtendEndDirection;
		WriteSourceSequentialBuffer(stream, s.SmoothPoint);
		WriteSourceSequentialBuffer(stream, s.SmoothProbe);
		stream << ' ' << s.Accumulated.size() << ' ' << s.FlattenLengths.size() << ' ' << s.Cache.size();
		for (double n : s.Accumulated)
			stream << ' ' << n;
		for (size_t i = 0; i < s.FlattenLengths.size(); ++i)
			stream << ' ' << s.FlattenLengths[i] << ' ' << s.FlattenOwners[i];
		for (const auto &sample : s.Cache)
			stream << ' ' << sample.Coordinate << ' ' << sample.Line << ' ' << sample.Point.Position.X << ' '
				   << sample.Point.Position.Y << ' ' << sample.Point.Weight;
	}
	template <class Admit>
	bool ReadSourceSequential(
		std::istream &stream, SourcePathSequentialData2D &s, SourcePathOperationKind kind, Admit &&admit
	) {
		unsigned side = 0;
		if (!(stream >> side >> s.ExtendLength) || side > 1 ||
			!ReadSourceSequentialFlag(stream, s.FlattenReverse) ||
			!ReadSourceSequentialFlag(stream, s.FlattenPingPong) ||
			!(stream >> s.SmoothRange.X >> s.SmoothRange.Y) ||
			!ReadSourceSequentialFlag(stream, s.SmoothClampCurve) ||
			!ReadSourceSequentialFlag(stream, s.SmoothLoop) ||
			!(stream >> s.SmoothSpan >> s.SmoothBlend >> s.SmoothSteps >> s.CachedLength >>
			  s.CachedSegments >> s.CachedBounds.X >> s.CachedBounds.Y >> s.CachedBounds.Z >>
			  s.CachedBounds.W >> s.ExtendStartPoint.Position.X >> s.ExtendStartPoint.Position.Y >>
			  s.ExtendStartPoint.Weight >> s.ExtendEndPoint.Position.X >> s.ExtendEndPoint.Position.Y >>
			  s.ExtendEndPoint.Weight >> s.ExtendStartDirection >> s.ExtendEndDirection) ||
			!ReadSourceSequentialBuffer(stream, s.SmoothPoint) ||
			!ReadSourceSequentialBuffer(stream, s.SmoothProbe))
			return false;
		s.ExtendSide = uint8_t(side);
		size_t accumulated = 0, lines = 0, cache = 0;
		if (!(stream >> accumulated >> lines >> cache) || accumulated > Limits::MaximumArrayElements ||
			lines > (Limits::MaximumArrayElements - accumulated) / 2 ||
			cache > Limits::MaximumArrayElements - accumulated - lines * 2 ||
			!admit(
				accumulated * sizeof(double) + lines * (sizeof(double) + sizeof(uint32_t)) +
				cache * sizeof(SourcePathSequentialCachePoint)
			))
			return false;
		s.Accumulated.resize(accumulated);
		s.FlattenLengths.resize(lines);
		s.FlattenOwners.resize(lines);
		s.Cache.resize(cache);
		for (double &n : s.Accumulated)
			if (!(stream >> n)) return false;
		for (size_t i = 0; i < lines; ++i)
			if (!(stream >> s.FlattenLengths[i] >> s.FlattenOwners[i])) return false;
		for (auto &sample : s.Cache)
			if (!(stream >> sample.Coordinate >> sample.Line >> sample.Point.Position.X >>
				  sample.Point.Position.Y >> sample.Point.Weight))
				return false;
		size_t count = 0;
		return ValidSourceSequential(s, kind, &count);
	}
}
