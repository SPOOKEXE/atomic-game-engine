#pragma once

#include "SourcePathSequentialCodec.hpp"
#include "SourcePathWavePayload.hpp"

#include <cmath>
#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteSourcePathWave(std::ostream &stream, const SourcePathWaveData2D &wave) {
		stream << std::setprecision(17) << wave.Range.X << ' ' << wave.Range.Y << ' ' << wave.Frequency.X
			   << ' ' << wave.Frequency.Y << ' ' << wave.Amplitude.X << ' ' << wave.Amplitude.Y << ' '
			   << wave.Phase.X << ' ' << wave.Phase.Y << ' ' << wave.DirectionRange.X << ' '
			   << wave.DirectionRange.Y << ' ' << wave.WiggleAmplitude.X << ' ' << wave.WiggleAmplitude.Y
			   << ' ' << wave.WeightRange.X << ' ' << wave.WeightRange.Y << ' ' << wave.Seed << ' '
			   << wave.AmplitudeShift << ' ' << wave.IterationFrequency << ' ' << wave.IterationAmplitude
			   << ' ' << wave.IterationShift << ' ' << wave.WiggleFrequency << ' ' << wave.Iteration << ' '
			   << wave.ClampCurve << ' ' << wave.Loop << ' ' << wave.Wiggle << ' ' << wave.UseWeight << ' '
			   << unsigned(wave.Direction) << ' ' << unsigned(wave.Mode) << ' ' << unsigned(wave.Post) << ' '
			   << unsigned(wave.WeightMode) << ' ' << wave.AmplitudeCurve.size();
		for (double sample : wave.AmplitudeCurve)
			stream << ' ' << sample;
		stream << ' ' << wave.DirectionCurve.size();
		for (double sample : wave.DirectionCurve)
			stream << ' ' << sample;
		for (const auto &buffer : wave.Buffers) {
			stream << ' ';
			WriteSourceSequentialBuffer(stream, buffer);
		}
		stream << ' ' << wave.Cache.size();
		for (const auto &sample : wave.Cache)
			stream << ' ' << sample.Coordinate << ' ' << sample.Line << ' ' << sample.Point.Position.X << ' '
				   << sample.Point.Position.Y << ' ' << sample.Point.Weight;
	}
	// Every variable-length field is admitted before allocation. Document parsing publishes only after
	// the complete enclosing value has passed its structural validation.
	template <class Admit>
	bool ReadSourcePathWave(std::istream &stream, SourcePathWaveData2D &wave, Admit admit) {
		unsigned clamp = 0, loop = 0, wiggle = 0, weight = 0, direction = 0, mode = 0, post = 0,
				 weightMode = 0;
		if (!(stream >> wave.Range.X >> wave.Range.Y >> wave.Frequency.X >> wave.Frequency.Y >>
			  wave.Amplitude.X >> wave.Amplitude.Y >> wave.Phase.X >> wave.Phase.Y >> wave.DirectionRange.X >>
			  wave.DirectionRange.Y >> wave.WiggleAmplitude.X >> wave.WiggleAmplitude.Y >>
			  wave.WeightRange.X >> wave.WeightRange.Y >> wave.Seed >> wave.AmplitudeShift >>
			  wave.IterationFrequency >> wave.IterationAmplitude >> wave.IterationShift >>
			  wave.WiggleFrequency >> wave.Iteration >> clamp >> loop >> wiggle >> weight >> direction >>
			  mode >> post >> weightMode) ||
			wave.Iteration < 0 || wave.Iteration > 4096 || clamp > 1 || loop > 1 || wiggle > 1 ||
			weight > 1 || direction > 1 || mode > 2 || post > 2 || weightMode > 2)
			return false;
		for (double value :
			 {wave.Range.X,
			  wave.Range.Y,
			  wave.Frequency.X,
			  wave.Frequency.Y,
			  wave.Amplitude.X,
			  wave.Amplitude.Y,
			  wave.Phase.X,
			  wave.Phase.Y,
			  wave.DirectionRange.X,
			  wave.DirectionRange.Y,
			  wave.WiggleAmplitude.X,
			  wave.WiggleAmplitude.Y,
			  wave.WeightRange.X,
			  wave.WeightRange.Y,
			  wave.Seed,
			  wave.AmplitudeShift,
			  wave.IterationFrequency,
			  wave.IterationAmplitude,
			  wave.IterationShift,
			  wave.WiggleFrequency})
			if (!std::isfinite(value)) return false;
		wave.ClampCurve = clamp != 0;
		wave.Loop = loop != 0;
		wave.Wiggle = wiggle != 0;
		wave.UseWeight = weight != 0;
		wave.Direction = uint8_t(direction);
		wave.Mode = uint8_t(mode);
		wave.Post = uint8_t(post);
		wave.WeightMode = uint8_t(weightMode);
		const auto readCurve = [&](std::vector<double> &curve, size_t expected, bool optional) {
			size_t count = 0;
			if (!(stream >> count) || (count != expected && (!optional || count != 0)) ||
				count > Limits::MaximumArrayElements || !admit(count * sizeof(double)))
				return false;
			curve.resize(count);
			for (double &sample : curve)
				if (!(stream >> sample) || !std::isfinite(sample)) return false;
			return true;
		};
		if (!readCurve(wave.AmplitudeCurve, 129, false) || !readCurve(wave.DirectionCurve, 33, true))
			return false;
		for (auto &buffer : wave.Buffers)
			if (!ReadSourceSequentialBuffer(stream, buffer)) return false;
		size_t count = 0;
		const size_t tables = wave.AmplitudeCurve.size() + wave.DirectionCurve.size();
		if (!(stream >> count) || count > Limits::MaximumArrayElements - tables ||
			!admit(count * sizeof(SourcePathSequentialCachePoint)))
			return false;
		wave.Cache.resize(count);
		for (auto &sample : wave.Cache)
			if (!(stream >> sample.Coordinate >> sample.Line >> sample.Point.Position.X >>
				  sample.Point.Position.Y >> sample.Point.Weight) ||
				!std::isfinite(sample.Coordinate) || sample.Line >= Limits::MaximumArrayElements ||
				!std::isfinite(sample.Point.Position.X) || !std::isfinite(sample.Point.Position.Y) ||
				!std::isfinite(sample.Point.Weight))
				return false;
		return true;
	}
} // namespace engine::imagegraph::detail
