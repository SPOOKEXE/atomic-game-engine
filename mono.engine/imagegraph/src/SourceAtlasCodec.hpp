#pragma once
#include "AtlasPayload.hpp"

#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>

namespace engine::imagegraph::detail {
	inline void WriteAtlasImage(std::ostream &stream, const Image &image) {
		stream << image.Width << ' ' << image.Height << ' ' << DescribeSurfaceFormat(image.Format)->Name
			   << ' ' << image.Hash << ' ' << image.Pixels.size() << ' ';
		constexpr char hex[] = "0123456789abcdef";
		for (uint8_t byte : image.Pixels)
			stream << hex[byte >> 4] << hex[byte & 15];
		stream << ' ';
	}
	template <class Admit> bool ReadAtlasImage(std::istream &stream, Image &image, Admit &admit) {
		Image candidate;
		if (!(stream >> candidate.Width >> candidate.Height)) return false;
		std::array<char, 32> name{};
		size_t length = 0;
		stream >> std::ws;
		while (stream && stream.peek() != std::char_traits<char>::eof() &&
			   !std::isspace(static_cast<unsigned char>(stream.peek()))) {
			if (length == name.size()) return false;
			name[length++] = static_cast<char>(stream.get());
		}
		bool found = false;
		for (uint8_t format = 0; format <= static_cast<uint8_t>(SurfaceFormat::R32Float); ++format) {
			const auto info = DescribeSurfaceFormat(static_cast<SurfaceFormat>(format));
			if (info && info->Name == std::string_view(name.data(), length)) {
				candidate.Format = static_cast<SurfaceFormat>(format);
				found = true;
			}
		}
		size_t bytes = 0;
		if (!found || !(stream >> candidate.Hash >> bytes)) return false;
		const bool empty = candidate.Width == 0 && candidate.Height == 0 && bytes == 0;
		if (!empty) {
			const auto layout = CheckedSurfaceLayout(
				candidate.Width, candidate.Height, candidate.Format, Limits::MaximumArrayBytes
			);
			if (!layout || candidate.Width > Limits::MaximumDimension ||
				candidate.Height > Limits::MaximumDimension || bytes != layout->Bytes || !admit(bytes))
				return false;
		}
		candidate.Pixels.resize(bytes);
		const auto hex = [](int value) {
			if (value >= '0' && value <= '9') return value - '0';
			if (value >= 'a' && value <= 'f') return value - 'a' + 10;
			if (value >= 'A' && value <= 'F') return value - 'A' + 10;
			return -1;
		};
		stream >> std::ws;
		for (auto &byte : candidate.Pixels) {
			const int a = hex(stream.get()), b = hex(stream.get());
			if (a < 0 || b < 0) return false;
			byte = static_cast<uint8_t>((a << 4) | b);
		}
		if (!empty && !FiniteSurfaceSamples(candidate)) return false;
		image = std::move(candidate);
		return true;
	}
	inline void WriteAtlasValue(std::ostream &stream, const AtlasValue &value) {
		stream << std::setprecision(17) << bool(value.Data) << ' ';
		if (!value.Data) return;
		const auto &data = *value.Data;
		stream << (data.Kind == AtlasKind::Atlas ? "atlas" : "surface_atlas") << ' ' << data.Position.X << ' '
			   << data.Position.Y << ' ' << data.Scale.X << ' ' << data.Scale.Y << ' ' << data.Dimension.X
			   << ' ' << data.Dimension.Y << ' ' << data.RotationDegrees << ' ' << data.Alpha << ' '
			   << unsigned(data.Blend.Red) << ' ' << unsigned(data.Blend.Green) << ' '
			   << unsigned(data.Blend.Blue) << ' ' << unsigned(data.Blend.Alpha) << ' ';
		WriteAtlasImage(stream, data.Surface.Data);
		stream << bool(data.OriginalSurface) << ' ';
		if (data.OriginalSurface) WriteAtlasImage(stream, data.OriginalSurface->Data);
		stream << data.OriginalDimension.X << ' ' << data.OriginalDimension.Y << ' ';
	}
	template <class Admit> bool ReadAtlasValue(std::istream &stream, AtlasValue &value, Admit &&admit) {
		unsigned present = 0;
		if (!(stream >> present) || present > 1) return false;
		if (!present) {
			value = {};
			return true;
		}
		if (!admit(sizeof(AtlasData))) return false;
		AtlasData data;
		std::array<char, 14> kind{};
		stream >> std::ws;
		size_t kindSize = 0;
		while (stream && stream.peek() != std::char_traits<char>::eof() &&
			   !std::isspace(static_cast<unsigned char>(stream.peek()))) {
			if (kindSize == kind.size()) return false;
			kind[kindSize++] = static_cast<char>(stream.get());
		}
		const std::string_view kindName(kind.data(), kindSize);
		if (kindName == "surface_atlas")
			data.Kind = AtlasKind::SurfaceAtlas;
		else if (kindName != "atlas")
			return false;
		unsigned r = 0, g = 0, b = 0, a = 0, original = 0;
		if (!(stream >> data.Position.X >> data.Position.Y >> data.Scale.X >> data.Scale.Y >>
			  data.Dimension.X >> data.Dimension.Y >> data.RotationDegrees >> data.Alpha >> r >> g >> b >>
			  a) ||
			r > 255 || g > 255 || b > 255 || a > 255)
			return false;
		data.Blend = {
			static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), static_cast<uint8_t>(a)
		};
		if (!ReadAtlasImage(stream, data.Surface.Data, admit) || !(stream >> original) || original > 1)
			return false;
		if (original && !ReadAtlasImage(stream, data.OriginalSurface.emplace().Data, admit)) return false;
		if (!(stream >> data.OriginalDimension.X >> data.OriginalDimension.Y)) return false;
		AtlasValue candidate;
		candidate.Data.emplace() = std::move(data);
		if (!ValidAtlasPayload(candidate)) return false;
		value = std::move(candidate);
		return true;
	}
}
