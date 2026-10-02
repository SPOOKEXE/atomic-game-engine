#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace engine::render::imagegraph {
	struct SourceSdfScatterUniforms {
		std::array<std::byte, 11104> Bytes{};
		static constexpr size_t operations = 0;
		static constexpr size_t opArgument = 512;
		static constexpr size_t opLength = 1024;
		static constexpr size_t shapeAmount = 1040;
		static constexpr size_t shape = 1056;
		static constexpr size_t size = 1312;
		static constexpr size_t radius = 1568;
		static constexpr size_t thickness = 1824;
		static constexpr size_t crop = 2080;
		static constexpr size_t angle = 2336;
		static constexpr size_t height = 2592;
		static constexpr size_t radRange = 2848;
		static constexpr size_t sizeUni = 3104;
		static constexpr size_t elongate = 3360;
		static constexpr size_t rounded = 3616;
		static constexpr size_t corner = 3872;
		static constexpr size_t size2D = 4128;
		static constexpr size_t sides = 4384;
		static constexpr size_t waveAmp = 4640;
		static constexpr size_t waveInt = 4896;
		static constexpr size_t waveShift = 5152;
		static constexpr size_t twistAxis = 5408;
		static constexpr size_t twistAmount = 5664;
		static constexpr size_t position = 5920;
		static constexpr size_t rotation = 6176;
		static constexpr size_t objectScale = 6432;
		static constexpr size_t tileActive = 6688;
		static constexpr size_t tileSize = 6944;
		static constexpr size_t tileAmount = 7200;
		static constexpr size_t tileShiftPos = 7456;
		static constexpr size_t tileShiftRot = 7712;
		static constexpr size_t tileShiftSca = 7968;
		static constexpr size_t diffuseColor = 8224;
		static constexpr size_t reflective = 8480;
		static constexpr size_t specular = 8736;
		static constexpr size_t useTexture = 8992;
		static constexpr size_t textureFilter = 9248;
		static constexpr size_t textureScale = 9504;
		static constexpr size_t triplanar = 9760;
		static constexpr size_t volumetric = 10016;
		static constexpr size_t volumeDensity = 10272;
		static constexpr size_t MAX_MARCHING_STEPS = 10528;
		static constexpr size_t ortho = 10544;
		static constexpr size_t fov = 10560;
		static constexpr size_t orthoScale = 10576;
		static constexpr size_t viewRange = 10592;
		static constexpr size_t depthRange = 10608;
		static constexpr size_t depthInt = 10624;
		static constexpr size_t background = 10640;
		static constexpr size_t ambientIntns = 10656;
		static constexpr size_t useLight = 10672;
		static constexpr size_t lightPosition = 10688;
		static constexpr size_t lightInten = 10704;
		static constexpr size_t lightColor = 10720;
		static constexpr size_t useEnv = 10736;
		static constexpr size_t envFilter = 10752;
		static constexpr size_t drawGrid = 10768;
		static constexpr size_t gridStep = 10784;
		static constexpr size_t gridScale = 10800;
		static constexpr size_t gridDrawScale = 10816;
		static constexpr size_t gridOpacity = 10832;
		static constexpr size_t gridColor = 10848;
		static constexpr size_t axisBlend = 10864;
		static constexpr size_t seed = 10880;
		static constexpr size_t dimension = 10896;
		static constexpr size_t grid = 10912;
		static constexpr size_t positionOrigin = 10928;
		static constexpr size_t positionOffset = 10944;
		static constexpr size_t rotationOrigin = 10960;
		static constexpr size_t rotationMin = 10976;
		static constexpr size_t rotationMax = 10992;
		static constexpr size_t objScale = 11008;
		static constexpr size_t camRotation = 11024;
		static constexpr size_t camScale = 11040;
		static constexpr size_t camRatio = 11056;
		static constexpr size_t drawBg = 11072;
		static constexpr size_t shapeAtlasUvScale = 11088;
		template <class T> void Set(size_t offset, const T &value, size_t index = 0) {
			static_assert(sizeof(T) <= 16);
			std::memcpy(Bytes.data() + offset + index * 16, &value, sizeof(T));
		}
	};
}
