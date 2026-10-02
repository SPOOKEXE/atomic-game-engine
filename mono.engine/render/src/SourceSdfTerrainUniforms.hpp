#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace engine::render::imagegraph {
	struct SourceSdfTerrainUniforms {
		std::array<std::byte, 256> Bytes{};
		static constexpr size_t atlasUvScale = 240;
		static constexpr size_t useTexture = 0;
		static constexpr size_t shape = 16;
		static constexpr size_t tile = 32;
		static constexpr size_t thickness = 48;
		static constexpr size_t time = 64;
		static constexpr size_t position = 80;
		static constexpr size_t rotation = 96;
		static constexpr size_t objectScale = 112;
		static constexpr size_t fov = 128;
		static constexpr size_t viewRange = 144;
		static constexpr size_t depthInt = 160;
		static constexpr size_t background = 176;
		static constexpr size_t ambient = 192;
		static constexpr size_t sunPosition = 208;
		static constexpr size_t shadow = 224;
		template <class T> void Set(size_t offset, const T &value, size_t index = 0) {
			static_assert(sizeof(T) <= 16);
			std::memcpy(Bytes.data() + offset + index * 16, &value, sizeof(T));
		}
	};
}
