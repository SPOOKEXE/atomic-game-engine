#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace engine::render::imagegraph {
	struct SourceSdfCloudUniforms {
		std::array<std::byte, 2336> Bytes{};
		static constexpr size_t gradient_blend = 0;
		static constexpr size_t gradient_color = 16;
		static constexpr size_t gradient_time = 1040;
		static constexpr size_t gradient_keys = 2064;
		static constexpr size_t gradient_use_map = 2080;
		static constexpr size_t gradient_map_range = 2096;
		static constexpr size_t dimension = 2112;
		static constexpr size_t position = 2128;
		static constexpr size_t rotation = 2144;
		static constexpr size_t objectScale = 2160;
		static constexpr size_t fov = 2176;
		static constexpr size_t viewRange = 2192;
		static constexpr size_t type = 2208;
		static constexpr size_t density = 2224;
		static constexpr size_t iteration = 2240;
		static constexpr size_t threshold = 2256;
		static constexpr size_t adaptiveIteration = 2272;
		static constexpr size_t fogUse = 2288;
		static constexpr size_t detailScale = 2304;
		static constexpr size_t detailAtten = 2320;
		template <class T> void Set(size_t offset, const T &value, size_t index = 0) {
			static_assert(sizeof(T) <= 16);
			std::memcpy(Bytes.data() + offset + index * 16, &value, sizeof(T));
		}
	};
}
