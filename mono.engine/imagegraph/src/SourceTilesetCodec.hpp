#pragma once
#include "SourceTilesetPayload.hpp"

#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>
namespace engine::imagegraph::detail {
	inline void WriteTilesetString(std::ostream &stream, const std::string &text) {
		stream << text.size() << ' ' << std::quoted(text) << ' ';
	}
	inline void WriteTilesetValue(std::ostream &stream, const TilesetValue &value) {
		stream << std::setprecision(17) << bool(value.Data) << ' ';
		if (!value.Data) return;
		const auto &data = *value.Data;
		stream << data.TileSize.X << ' ' << data.TileSize.Y << ' ';
		WriteTilesetString(stream, data.DisplayName);
		stream << data.Texture.Width << ' ' << data.Texture.Height << ' '
			   << DescribeSurfaceFormat(data.Texture.Format)->Name << ' ' << data.Texture.Hash << ' '
			   << data.Texture.Pixels.size() << ' ';
		constexpr char hex[] = "0123456789abcdef";
		for (uint8_t byte : data.Texture.Pixels)
			stream << hex[byte >> 4] << hex[byte & 15];
		stream << ' ';
		stream << data.Animations.size() << ' ';
		for (const auto &animation : data.Animations) {
			WriteTilesetString(stream, animation.Name);
			stream << animation.Length << ' ' << animation.Indices.size() << ' ';
			for (int32_t index : animation.Indices)
				stream << index << ' ';
		}
		stream << data.Terrains.size() << ' ';
		for (const auto &terrain : data.Terrains) {
			WriteTilesetString(stream, terrain.Name);
			stream << terrain.Type << ' ' << terrain.PreviewIndex << ' ' << terrain.Indices.size() << ' ';
			for (int32_t index : terrain.Indices)
				stream << index << ' ';
		}
		stream << data.Rules.size() << ' ';
		for (const auto &rule : data.Rules) {
			WriteTilesetString(stream, rule.Name);
			stream << rule.Active << ' ' << rule.Range << ' ' << rule.Size.X << ' ' << rule.Size.Y << ' '
				   << rule.Probability << ' ' << rule.Selection.size() << ' ';
			for (const auto &entry : rule.Selection)
				stream << entry.Terrain << ' ' << entry.Index << ' ';
			stream << rule.Replacements.size() << ' ';
			for (const auto &row : rule.Replacements) {
				stream << row.size() << ' ';
				for (double number : row)
					stream << number << ' ';
			}
		}
	}
	inline bool ReadTilesetBoolean(std::istream &stream, bool &result) {
		unsigned number;
		if (!(stream >> number) || number > 1) return false;
		result = number != 0;
		return true;
	}
	inline int TilesetHex(int value) {
		if (value >= '0' && value <= '9') return value - '0';
		if (value >= 'a' && value <= 'f') return value - 'a' + 10;
		if (value >= 'A' && value <= 'F') return value - 'A' + 10;
		return -1;
	}
	template <class Admit> bool ReadTilesetString(std::istream &stream, std::string &result, Admit &admit) {
		size_t length = 0;
		if (!(stream >> length) || length > Limits::MaximumTextBytes ||
			!admit(std::max(length, std::string{}.capacity())))
			return false;
		stream >> std::ws;
		if (stream.get() != '"') return false;
		std::string candidate(length, '\0');
		size_t index = 0;
		for (;;) {
			int character = stream.get();
			if (character == std::char_traits<char>::eof()) return false;
			if (character == '"') break;
			if (character == '\\') {
				character = stream.get();
				if (character == std::char_traits<char>::eof()) return false;
			}
			if (index >= length) return false;
			candidate[index++] = char(character);
		}
		if (index != length) return false;
		result = std::move(candidate);
		return true;
	}
	template <class Admit> bool ReadTilesetValue(std::istream &stream, TilesetValue &result, Admit &&admit) {
		bool present;
		if (!ReadTilesetBoolean(stream, present)) return false;
		TilesetValue candidate;
		if (!present) {
			result = std::move(candidate);
			return true;
		}
		if (!admit(sizeof(TilesetData))) return false;
		auto &data = candidate.Data.emplace();
		if (!(stream >> data.TileSize.X >> data.TileSize.Y) || !MeshFinite(data.TileSize) ||
			!ReadTilesetString(stream, data.DisplayName, admit))
			return false;
		std::array<char, 32> format{};
		size_t formatLength = 0;
		size_t bytes = 0;
		if (!(stream >> data.Texture.Width >> data.Texture.Height)) return false;
		stream >> std::ws;
		for (;;) {
			const int character = stream.peek();
			if (character == std::char_traits<char>::eof()) return false;
			if (std::isspace(static_cast<unsigned char>(character))) break;
			if (formatLength == format.size()) return false;
			format[formatLength++] = char(stream.get());
		}
		constexpr std::array formats{
			SurfaceFormat::RGBA8Unorm,
			SurfaceFormat::RGBA4Unorm,
			SurfaceFormat::RGBA16Float,
			SurfaceFormat::RGBA32Float,
			SurfaceFormat::R8Unorm,
			SurfaceFormat::R16Float,
			SurfaceFormat::R32Float
		};
		std::optional<SurfaceFormat> surface;
		for (const auto candidateFormat : formats)
			if (DescribeSurfaceFormat(candidateFormat)->Name == std::string_view(format.data(), formatLength))
				surface = candidateFormat;
		if (!surface) return false;
		data.Texture.Format = *surface;
		if (!(stream >> data.Texture.Hash >> bytes)) return false;
		const auto layout = CheckedSurfaceLayout(
			data.Texture.Width, data.Texture.Height, *surface, Limits::MaximumOutputBytes
		);
		if (!layout || bytes != layout->Bytes || data.Texture.Width > Limits::MaximumDimension ||
			data.Texture.Height > Limits::MaximumDimension || !admit(bytes))
			return false;
		data.Texture.Pixels.resize(bytes);
		stream >> std::ws;
		for (uint8_t &byte : data.Texture.Pixels) {
			const int hi = TilesetHex(stream.get()), lo = TilesetHex(stream.get());
			if (hi < 0 || lo < 0) return false;
			byte = uint8_t(hi * 16 + lo);
		}
		const int delimiter = stream.peek();
		if (delimiter != std::char_traits<char>::eof() &&
			!std::isspace(static_cast<unsigned char>(delimiter)))
			return false;
		size_t count = 0, frames = 0, total = 0;
		if (!(stream >> count) || count > 128 || !admit(count * sizeof(TileAnimationData))) return false;
		data.Animations.resize(count);
		for (auto &animation : data.Animations) {
			size_t size = 0;
			if (!ReadTilesetString(stream, animation.Name, admit) || !(stream >> animation.Length >> size) ||
				animation.Length > 256 || size > 256 - frames || !admit(size * sizeof(int32_t)))
				return false;
			frames += size;
			animation.Indices.resize(size);
			for (auto &index : animation.Indices)
				if (!(stream >> index)) return false;
		}
		if (!(stream >> count) || count > Limits::MaximumArrayElements ||
			!admit(count * sizeof(TileTerrainData)))
			return false;
		data.Terrains.resize(count);
		for (auto &terrain : data.Terrains) {
			size_t size = 0;
			if (!ReadTilesetString(stream, terrain.Name, admit) ||
				!(stream >> terrain.Type >> terrain.PreviewIndex >> size) || size > 63 ||
				size > Limits::MaximumArrayElements - total || !admit(size * sizeof(int32_t)))
				return false;
			total += size;
			terrain.Indices.resize(size);
			for (auto &index : terrain.Indices)
				if (!(stream >> index)) return false;
		}
		if (!(stream >> count) || count > Limits::MaximumArrayElements ||
			!admit(count * sizeof(TileRuleData)))
			return false;
		// RuleData has a nine-entry default selection, whose allocation also precedes parsing.
		if (!admit(count * 9 * sizeof(TileSelection))) return false;
		data.Rules.resize(count);
		for (auto &rule : data.Rules) {
			size_t size = 0;
			if (!ReadTilesetString(stream, rule.Name, admit) || !ReadTilesetBoolean(stream, rule.Active) ||
				!(stream >> rule.Range >> rule.Size.X >> rule.Size.Y >> rule.Probability >> size) ||
				size > 64 || size > Limits::MaximumArrayElements - total ||
				!admit(size * sizeof(TileSelection)))
				return false;
			total += size;
			rule.Selection = std::vector<TileSelection>(size);
			for (auto &selection : rule.Selection)
				if (!ReadTilesetBoolean(stream, selection.Terrain) || !(stream >> selection.Index) ||
					!std::isfinite(selection.Index))
					return false;
			if (!(stream >> size) || size > 256 || !admit(size * sizeof(std::vector<double>))) return false;
			rule.Replacements.resize(size);
			for (auto &row : rule.Replacements) {
				size_t elements = 0;
				if (!(stream >> elements) || elements > 256 ||
					elements > Limits::MaximumArrayElements - total || !admit(elements * sizeof(double)))
					return false;
				total += elements;
				row.resize(elements);
				for (double &number : row)
					if (!(stream >> number) || !std::isfinite(number)) return false;
			}
		}
		if (!ValidTilesetPayload(candidate)) return false;
		result = std::move(candidate);
		return true;
	}
}
