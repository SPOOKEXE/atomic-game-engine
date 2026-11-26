#pragma once

// The names a shader gives a part's material values.
//
//   #pragma parameter vec3 tint = 1, 0.4, 0.2
//   #pragma parameter float wetness = 0
//   #pragma texture thickness
//
// Values live on the part as sixteen floats and two textures
// (`SurfaceAppearance`), so they save, replicate and batch without knowing what
// they mean; a layout is how a script and a shader agree on which is which.
// Parameters fill the floats in declaration order and never cross a group of
// four.
//
// @tier L7 · shared

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scene {
	struct SurfaceAppearance;

	inline constexpr size_t MAX_MATERIAL_FLOATS = 16;
	inline constexpr size_t MAX_MATERIAL_TEXTURES = 2;
	// Most named images one pass reads.
	inline constexpr size_t MAX_PASS_IMAGES = 4;

	// One declared parameter.
	struct MaterialParameter {
		std::string Name;
		// How many floats, 1 to 4.
		uint32_t Components = 1;
		// Its first float.
		uint32_t Offset = 0;
		std::array<float, 4> Default{};
	};

	// What one shader's source declares.
	struct MaterialLayout {
		std::vector<MaterialParameter> Parameters;
		std::vector<std::string> Textures;
		// For each texture, how many layers its array holds, or 0 for a plain one.
		std::vector<uint32_t> TextureLayers;
		// Named images a pass reads, from `#pragma image name`, in binding order.
		std::vector<std::string> Images;
		// The first declaration that is wrong, as `line: reason`; empty when none.
		std::string Error;

		const MaterialParameter *Find(std::string_view name) const;
		// The texture's slot, or -1.
		int FindTexture(std::string_view name) const;
	};

	// Reads the declarations out of shader source.
	MaterialLayout ParseMaterialLayout(std::string_view source);

	// The set values as text, and back. `Parse` replaces every value, and changes
	// nothing when the text is malformed.
	std::string FormatMaterialParameters(const SurfaceAppearance &appearance);
	bool ParseMaterialParameters(std::string_view text, SurfaceAppearance &appearance);

	// Sets or, with no values, clears one parameter. False for a wrong count.
	bool SetMaterialParameter(
		SurfaceAppearance &appearance, const MaterialParameter &parameter, const float *values, size_t count
	);
}
