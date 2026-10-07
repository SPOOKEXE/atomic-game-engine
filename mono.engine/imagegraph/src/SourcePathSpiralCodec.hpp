#pragma once

#include "SourcePathSequentialCodec.hpp"

#include <cmath>
#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteSourcePathSpiral(std::ostream &stream, const SourcePathSpiralData2D &spiral) {
		stream << std::setprecision(17) << spiral.Range.X << ' ' << spiral.Range.Y << ' '
			   << spiral.DirectionRange.X << ' ' << spiral.DirectionRange.Y << ' ' << spiral.WeightRange.X
			   << ' ' << spiral.WeightRange.Y << ' ' << spiral.Frequency << ' ' << spiral.Amplitude << ' '
			   << spiral.Spiral << ' ' << spiral.Phase << ' ' << spiral.ClampCurve << ' ' << spiral.Loop
			   << ' ' << spiral.UseWeight << ' ' << unsigned(spiral.Direction) << ' '
			   << unsigned(spiral.WeightMode) << ' ' << spiral.AmplitudeCurve.size();
		for (double sample : spiral.AmplitudeCurve)
			stream << ' ' << sample;
		stream << ' ' << spiral.DirectionCurve.size();
		for (double sample : spiral.DirectionCurve)
			stream << ' ' << sample;
		for (const auto &buffer : spiral.Buffers) {
			stream << ' ';
			WriteSourceSequentialBuffer(stream, buffer);
		}
		stream << ' ' << spiral.Cache.size();
		for (const auto &sample : spiral.Cache)
			stream << ' ' << sample.Coordinate << ' ' << sample.Line << ' ' << sample.Point.Position.X << ' '
				   << sample.Point.Position.Y << ' ' << sample.Point.Weight;
	}
	// Caller admits the descriptor. Sample counts have source precision and are admitted before resize.
	template <class Admit>
	bool ReadSourcePathSpiral(std::istream &stream, SourcePathSpiralData2D &spiral, Admit admit) {
		unsigned clamp = 0, loop = 0, weight = 0, direction = 0, mode = 0;
		if (!(stream >> spiral.Range.X >> spiral.Range.Y >> spiral.DirectionRange.X >>
			  spiral.DirectionRange.Y >> spiral.WeightRange.X >> spiral.WeightRange.Y >> spiral.Frequency >>
			  spiral.Amplitude >> spiral.Spiral >> spiral.Phase >> clamp >> loop >> weight >> direction >>
			  mode) ||
			clamp > 1 || loop > 1 || weight > 1 || direction > 1 || mode > 2)
			return false;
		for (double value :
			 {spiral.Range.X,
			  spiral.Range.Y,
			  spiral.DirectionRange.X,
			  spiral.DirectionRange.Y,
			  spiral.WeightRange.X,
			  spiral.WeightRange.Y,
			  spiral.Frequency,
			  spiral.Amplitude,
			  spiral.Spiral,
			  spiral.Phase})
			if (!std::isfinite(value)) return false;
		spiral.ClampCurve = clamp != 0;
		spiral.Loop = loop != 0;
		spiral.UseWeight = weight != 0;
		spiral.Direction = uint8_t(direction);
		spiral.WeightMode = uint8_t(mode);
		const auto readCurve = [&](std::vector<double> &curve, size_t sourceCount) {
			size_t count = 0;
			if (!(stream >> count) || (count != 0 && count != sourceCount) || !admit(count * sizeof(double)))
				return false;
			curve.resize(count);
			for (double &sample : curve)
				if (!(stream >> sample) || !std::isfinite(sample)) return false;
			return true;
		};
		if (!readCurve(spiral.AmplitudeCurve, 129) || !readCurve(spiral.DirectionCurve, 33)) return false;
		for (auto &buffer : spiral.Buffers)
			if (!ReadSourceSequentialBuffer(stream, buffer)) return false;
		size_t count = 0;
		const size_t tables = spiral.AmplitudeCurve.size() + spiral.DirectionCurve.size();
		if (!(stream >> count) || count > Limits::MaximumArrayElements - tables ||
			!admit(count * sizeof(SourcePathSequentialCachePoint)))
			return false;
		spiral.Cache.resize(count);
		for (auto &sample : spiral.Cache)
			if (!(stream >> sample.Coordinate >> sample.Line >> sample.Point.Position.X >>
				  sample.Point.Position.Y >> sample.Point.Weight) ||
				!std::isfinite(sample.Coordinate) || sample.Line > Limits::MaximumArrayElements ||
				!std::isfinite(sample.Point.Position.X) || !std::isfinite(sample.Point.Position.Y) ||
				!std::isfinite(sample.Point.Weight))
				return false;
		return true;
	}
}
