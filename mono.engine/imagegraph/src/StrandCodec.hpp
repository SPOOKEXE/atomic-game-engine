#pragma once
#include "StrandPayload.hpp"

#include <iomanip>
#include <istream>
#include <ostream>
namespace engine::imagegraph::detail {
	inline void WriteStrandValue(std::ostream &stream, const StrandValue &value) {
		stream << bool(value.Data) << ' ';
		if (!value.Data) return;
		const auto &data = *value.Data;
		stream << data.OriginProcessorRow << ' ' << data.AuthoringRevision << ' ' << data.OriginNodeId.size()
			   << ' ' << std::quoted(data.OriginNodeId) << ' ' << data.State.Loop << ' '
			   << data.State.Hairs.size() << ' ';
		for (const auto &hair : data.State.Hairs) {
			stream << hair.SourceId << ' ' << hair.Free << ' ' << hair.Direction << ' ' << hair.CurlFrequency
				   << ' ' << hair.CurlSize << ' ' << hair.Tension << ' ' << hair.Spring << ' '
				   << hair.AngularTension << ' ' << hair.RootStrength << ' ' << hair.RootForce << ' '
				   << hair.Restitution << ' ' << hair.Points.size() << ' ';
			for (const auto &point : hair.Points) {
				for (const auto &pair : {point.Position, point.Previous, point.PreviousPrevious, point.Delta})
					for (double v : pair)
						stream << v << ' ';
				stream << bool(point.IkX) << ' ' << point.IkX.value_or(0) << ' ' << bool(point.IkY) << ' '
					   << point.IkY.value_or(0) << ' ' << point.AirResistance << ' ';
			}
			for (double v : hair.Lengths)
				stream << v << ' ';
			for (double v : hair.RestAngles)
				stream << v << ' ';
		}
	}
	inline bool ReadStrandBoolean(std::istream &stream, bool &result) {
		unsigned stored;
		if (!(stream >> stored) || stored > 1) return false;
		result = stored != 0;
		return true;
	}
	template <class Admit> bool ReadStrandValue(std::istream &stream, StrandValue &result, Admit &&admit) {
		bool present;
		if (!ReadStrandBoolean(stream, present)) return false;
		StrandValue candidate;
		if (!present) {
			result = std::move(candidate);
			return true;
		}
		if (!admit(sizeof(StrandData2D))) return false;
		auto &data = candidate.Data.emplace();
		size_t nameLength, hairCount;
		if (!(stream >> data.OriginProcessorRow >> data.AuthoringRevision >> nameLength) ||
			data.OriginProcessorRow >= Limits::MaximumArrayElements ||
			nameLength > Limits::MaximumTextBytes || !admit(std::max(nameLength, std::string{}.capacity())))
			return false;
		stream >> std::ws;
		if (stream.get() != '"') return false;
		data.OriginNodeId.assign(nameLength, '\0');
		size_t index = 0;
		for (;;) {
			int character = stream.get();
			if (character == std::char_traits<char>::eof()) return false;
			if (character == '"') break;
			if (character == '\\') {
				character = stream.get();
				if (character == std::char_traits<char>::eof()) return false;
			}
			if (index >= nameLength) return false;
			data.OriginNodeId[index++] = char(character);
		}
		if (index != nameLength || !ReadStrandBoolean(stream, data.State.Loop) || !(stream >> hairCount) ||
			hairCount > Limits::MaximumArrayElements || !admit(hairCount * sizeof(SourceStrandHair)))
			return false;
		data.State.Hairs.reserve(hairCount);
		uint64_t points = 0;
		for (size_t h = 0; h < hairCount; ++h) {
			SourceStrandHair hair;
			size_t count;
			if (!(stream >> hair.SourceId) || !ReadStrandBoolean(stream, hair.Free) ||
				!(stream >> hair.Direction >> hair.CurlFrequency >> hair.CurlSize >> hair.Tension >>
				  hair.Spring >> hair.AngularTension >> hair.RootStrength >> hair.RootForce >>
				  hair.Restitution >> count) ||
				count == 0 || count > MAXIMUM_SOURCE_STRAND_POINTS - points)
				return false;
			points += count;
			if (!admit(count * (sizeof(SourceStrandPoint) + 2 * sizeof(double)))) return false;
			hair.Points.resize(count);
			hair.Lengths.resize(count);
			hair.RestAngles.resize(count);
			for (auto &point : hair.Points) {
				for (auto *pair : {&point.Position, &point.Previous, &point.PreviousPrevious, &point.Delta})
					for (double &v : *pair)
						if (!(stream >> v)) return false;
				bool hasX, hasY;
				double x, y;
				if (!ReadStrandBoolean(stream, hasX) || !(stream >> x) || !ReadStrandBoolean(stream, hasY) ||
					!(stream >> y >> point.AirResistance) || !std::isfinite(x) || !std::isfinite(y))
					return false;
				if (hasX) point.IkX = x;
				if (hasY) point.IkY = y;
			}
			for (double &v : hair.Lengths)
				if (!(stream >> v)) return false;
			for (double &v : hair.RestAngles)
				if (!(stream >> v)) return false;
			data.State.Hairs.push_back(std::move(hair));
		}
		if (!ValidStrandPayload(candidate)) return false;
		result = std::move(candidate);
		return true;
	}
} // namespace engine::imagegraph::detail
