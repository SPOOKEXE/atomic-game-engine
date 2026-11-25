#pragma once

// Ordered material layers over a part's surface.
//
// A `MaterialLayer` inside a part names a `ShaderModule` that defines
//
//   void Layer(SurfaceInput i, inout SurfaceOutput o)
//
// where `o` arrives as the surface so far. The layers of a part, sorted by
// `Order`, are composed with its `Material`'s shader (or the part's own maps)
// into one generated surface, so the whole stack is one draw. Parts with the
// same stack share a shader. A layer's opacity is a material parameter, so
// fading one compiles nothing.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/ClassId.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Most layers one part composes; later ones are left out.
	inline constexpr size_t MAX_MATERIAL_LAYERS = 8;

	// How a layer meets the surface under it.
	enum class LayerBlend : uint8_t {
		Over = 0,
		Multiply = 1,
		Add = 2,
		Screen = 3,
	};

	// What a layer may change, one bit each.
	enum LayerChannel : uint8_t {
		LAYER_ALBEDO = 1,
		LAYER_NORMAL = 2,
		LAYER_ROUGHNESS = 4,
		LAYER_METALNESS = 8,
		LAYER_OCCLUSION = 16,
		LAYER_EMISSIVE = 32,
		// The four free values a Light reads.
		LAYER_CUSTOM = 64,
	};
	inline constexpr uint8_t LAYER_ALL = 127;

	// A `MaterialLayer` instance.
	struct MaterialLayer {
		// The `ShaderModule` holding the Layer function.
		core::Name Shader = {};
		// A declared texture and channel such as `masks.g`, or nothing for all of it.
		core::Name Mask = {};
		float Opacity = 1.0f;
		int32_t Order = 0;
		LayerBlend Blend = LayerBlend::Over;
		uint8_t Channels = LAYER_ALL;
		bool Enabled = true;
		uint8_t Reserved = 0;
	};

	// One layer, as composition sees it.
	struct LayerSource {
		std::string Module;
		// The module's text, for the parameters and textures it declares.
		std::string Code;
		std::string Mask;
		float Opacity = 1.0f;
		LayerBlend Blend = LayerBlend::Over;
		uint8_t Channels = LAYER_ALL;
	};

	struct LayeredSurface {
		std::string Source;
		// Each layer's opacity parameter; empty where it was written in as a constant.
		std::vector<std::string> Opacities;
		std::string Error;
	};

	// Composes layers, already in order, over a surface's source. An empty base
	// is the part's own maps.
	LayeredSurface ComposeLayers(std::string_view base, std::span<const LayerSource> layers);

	// Generated surfaces by name. Derived by `ResolveMaterials` and never saved.
	struct LayeredShaders {
		struct Entry {
			std::string Source;
			// The script a compile error is reported on.
			ecs::Entity Reporter = ecs::NULL_ENTITY;
			uint32_t Uses = 0;
		};
		std::unordered_map<uint32_t, Entry> Surfaces;
		// What each part's stack was built from, and the shader it got.
		struct Held {
			uint64_t Signature = 0;
			core::Name Shader;
			std::vector<std::string> Opacities;
			// The opacities last written, so a float a script has since set is not
			// mistaken for one.
			std::vector<float> Written;
		};
		std::unordered_map<uint64_t, Held> Parts;
	};

	// The `MaterialLayer` class id.
	ecs::ClassId MaterialLayerClass();

	// Composes every part that has layers and writes its shader and opacities.
	// Part of `ResolveMaterials`, after each part's base shader is known.
	void ResolveMaterialLayers(ecs::Store &store);
}
