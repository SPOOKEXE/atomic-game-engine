#pragma once
#include "ParticlePayload.hpp"

#include <cctype>
#include <iomanip>
#include <istream>
#include <ostream>
namespace engine::imagegraph::detail {
	inline void WriteParticleValue(std::ostream &stream, const ParticleValue &value) {
		stream << std::setprecision(17) << bool(value.Data) << ' ';
		if (!value.Data) return;
		const auto &data = *value.Data;
		stream << data.OriginProcessorRow << ' ' << data.OriginNodeId.size() << ' '
			   << std::quoted(data.OriginNodeId) << ' ' << data.Sprites.size() << ' ';
		{
			const auto &particle = data.State;
			stream << particle.Active << ' ' << particle.SourceSlot << ' ' << particle.Position[0] << ' '
				   << particle.Position[1] << ' ' << particle.Scale[0] << ' ' << particle.Scale[1] << ' '
				   << particle.RotationDegrees << ' ' << particle.Alpha << ' ' << particle.Blend << ' '
				   << bool(particle.SpriteSlot) << ' ' << particle.SpriteSlot.value_or(0) << ' '
				   << particle.XHistory.size() << ' ';
			for (const auto *history : {&particle.XHistory, &particle.YHistory})
				for (const auto &item : *history)
					stream << bool(item) << ' ' << item.value_or(0) << ' ';
		}
		constexpr char hex[] = "0123456789abcdef";
		for (const auto &sprite : data.Sprites) {
			stream << sprite.Width << ' ' << sprite.Height << ' '
				   << DescribeSurfaceFormat(sprite.Format)->Name << ' ' << sprite.Pixels.size() << ' '
				   << sprite.Hash << ' ';
			for (const auto byte : sprite.Pixels)
				stream << hex[byte >> 4] << hex[byte & 15];
			stream << ' ';
		}
	}
	inline bool ReadParticleBoolean(std::istream &stream, bool &result) {
		unsigned stored;
		if (!(stream >> stored) || stored > 1) return false;
		result = stored != 0;
		return true;
	}
	inline int ParticleHex(int value) {
		if (value >= '0' && value <= '9') return value - '0';
		if (value >= 'a' && value <= 'f') return value - 'a' + 10;
		if (value >= 'A' && value <= 'F') return value - 'A' + 10;
		return -1;
	}
	template <class Admit>
	bool ReadParticleValue(std::istream &stream, ParticleValue &result, Admit &&admit) {
		bool present;
		if (!ReadParticleBoolean(stream, present)) return false;
		ParticleValue candidate;
		if (!present) {
			result = std::move(candidate);
			return true;
		}
		if (!admit(sizeof(ParticleData2D))) return false;
		auto &data = candidate.Data.emplace();
		size_t nameLength, spriteCount;
		if (!(stream >> data.OriginProcessorRow >> nameLength) || nameLength > Limits::MaximumTextBytes ||
			!admit(std::max(nameLength, std::string{}.capacity())))
			return false;
		stream >> std::ws;
		if (stream.get() != '"') return false;
		data.OriginNodeId = std::string(nameLength, '\0');
		size_t nameIndex = 0;
		for (;;) {
			int character = stream.get();
			if (character == std::char_traits<char>::eof()) return false;
			if (character == '"') break;
			if (character == '\\') {
				character = stream.get();
				if (character == std::char_traits<char>::eof()) return false;
			}
			if (nameIndex >= nameLength) return false;
			data.OriginNodeId[nameIndex++] = char(character);
		}
		if (nameIndex != nameLength || !(stream >> spriteCount) ||
			spriteCount > Limits::MaximumArrayElements || !admit(spriteCount * sizeof(Image)))
			return false;
		data.Sprites.resize(spriteCount);
		uint64_t totalHistory = 0, totalPixels = 0;
		{
			auto &particle = data.State;
			bool spritePresent;
			uint32_t spriteSlot;
			size_t historyCount;
			if (!ReadParticleBoolean(stream, particle.Active) ||
				!(stream >> particle.SourceSlot >> particle.Position[0] >> particle.Position[1] >>
				  particle.Scale[0] >> particle.Scale[1] >> particle.RotationDegrees >> particle.Alpha >>
				  particle.Blend) ||
				!ReadParticleBoolean(stream, spritePresent) || !(stream >> spriteSlot >> historyCount) ||
				historyCount > Limits::MaximumRangeFrames)
				return false;
			totalHistory = MeshAddBytes(totalHistory, uint64_t(historyCount) * 2);
			if (totalHistory > Limits::MaximumArrayElements ||
				!admit(uint64_t(historyCount) * 2 * sizeof(std::optional<double>)))
				return false;
			if (spritePresent) particle.SpriteSlot = spriteSlot;
			particle.XHistory.resize(historyCount);
			particle.YHistory.resize(historyCount);
			for (auto *history : {&particle.XHistory, &particle.YHistory})
				for (auto &item : *history) {
					bool itemPresent;
					double number;
					if (!ReadParticleBoolean(stream, itemPresent) || !(stream >> number) ||
						!std::isfinite(number))
						return false;
					if (itemPresent) item = number;
				}
		}
		const std::array formats{
			SurfaceFormat::RGBA8Unorm,
			SurfaceFormat::RGBA4Unorm,
			SurfaceFormat::RGBA16Float,
			SurfaceFormat::RGBA32Float,
			SurfaceFormat::R8Unorm,
			SurfaceFormat::R16Float,
			SurfaceFormat::R32Float
		};
		for (auto &sprite : data.Sprites) {
			size_t byteCount;
			std::array<char, 32> formatName{};
			size_t formatLength = 0;
			if (!(stream >> sprite.Width >> sprite.Height) || sprite.Width > Limits::MaximumDimension ||
				sprite.Height > Limits::MaximumDimension)
				return false;
			stream >> std::ws;
			for (;;) {
				const int character = stream.peek();
				if (character == std::char_traits<char>::eof()) return false;
				if (std::isspace(static_cast<unsigned char>(character))) break;
				if (formatLength >= formatName.size()) return false;
				formatName[formatLength++] = char(stream.get());
			}
			bool known = false;
			for (const auto format : formats)
				if (DescribeSurfaceFormat(format)->Name ==
					std::string_view(formatName.data(), formatLength)) {
					sprite.Format = format;
					known = true;
					break;
				}
			if (!known || !(stream >> byteCount >> sprite.Hash)) return false;
			const auto layout =
				CheckedSurfaceLayout(sprite.Width, sprite.Height, sprite.Format, Limits::MaximumOutputBytes);
			totalPixels = MeshAddBytes(totalPixels, byteCount);
			if (!layout || byteCount != layout->Bytes || totalPixels > Limits::MaximumOutputBytes ||
				!admit(byteCount))
				return false;
			sprite.Pixels.resize(byteCount);
			stream >> std::ws;
			for (auto &byte : sprite.Pixels) {
				const int high = ParticleHex(stream.get()), low = ParticleHex(stream.get());
				if (high < 0 || low < 0) return false;
				byte = uint8_t(high * 16 + low);
			}
			const int delimiter = stream.peek();
			if (delimiter != std::char_traits<char>::eof() &&
				!std::isspace(static_cast<unsigned char>(delimiter)))
				return false;
		}
		if (!ValidParticlePayload(candidate)) return false;
		result = std::move(candidate);
		return true;
	}
} // namespace engine::imagegraph::detail
