#pragma once

// Ordered material layers over a part's surface.
//
// A `MaterialLayer` inside a part names a `ShaderModule` that defines
//
//   void Layer(SurfaceInput i, inout SurfaceOutput o)
//
// where `o` arrives as the surface so far. The layers of a part, sorted by
// `Order`, are composed with its `Material`'s shader (or the part's own maps)
// into one generated surface, so the whole stack is one draw. A `Material` with
// `Keywords` goes the same way, with `#define`s for the switches its shader
// declares with `#pragma keyword NAME`. Parts with the
// same stack share a shader. A layer's opacity is a material parameter, so
// fading one compiles nothing. Past `MAX_MATERIAL_LAYERS`, the layers below the
// top seven are baked: drawn once into the part's UVs as a `bake/` surface, and
// read back by the live stack as one texture array.
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
	// Most layers one part composes live; below these the stack is baked.
	inline constexpr size_t MAX_MATERIAL_LAYERS = 8;
	// The side of a baked prefix's textures, in texels.
	inline constexpr uint32_t LAYER_BAKE_SIZE = 1024;

	// How a layer meets the surface under it.
	enum class LayerBlend : uint8_t {
		Over = 0,
		Multiply = 1,
		Add = 2,
		Screen = 3,
		// The higher of the two shows, by `Height` and opacity.
		Height = 4,
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
		// A declared texture and channel such as `masks.g`, or nothing for all of
		// it. `@mesh` reads it at the authored UVs and `@attribute0` to
		// `@attribute3` at that display attribute's first two values.
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
	// is the part's own maps. `keywords` are the base's declared switches to turn
	// on, joined by commas.
	// With `constantOpacity`, every opacity is written in, as a bake needs.
	LayeredSurface ComposeLayers(
		std::string_view base,
		std::span<const LayerSource> layers,
		std::string_view keywords = {},
		bool constantOpacity = false
	);

	// A surface that reads a baked prefix from `engineBaked`, a five-layer
	// array declared after `base`'s own textures, keeping `base`'s Light and
	// helpers. Until the bake lands it draws `base` as it was.
	std::string BakedBase(std::string_view base);

	// One part's baked prefix, for the renderer to draw into its UVs.
	struct LayerBake {
		ecs::Entity Part = ecs::NULL_ENTITY;
		core::Name Shader;
		// Its textures are `Output` followed by `/0` to `/4`.
		core::Name Output;
		uint64_t Revision = 0;
		// The part's floats the bake reads: the base's own. A baked layer's
		// parameters keep their defaults.
		uint16_t ValueMask = 0;
	};

	// Every baked prefix the world holds.
	void LayerBakesOf(const ecs::Store &store, std::vector<LayerBake> &out);

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
			// The baked prefix, when the stack is longer than the live limit.
			core::Name BakeShader;
			core::Name BakeOutput;
			uint64_t BakeRevision = 0;
			uint16_t BakeValueMask = 0;
		};
		std::unordered_map<uint64_t, Held> Parts;
	};

	// The `MaterialLayer` class id.
	ecs::ClassId MaterialLayerClass();

	// Composes every part that has layers and writes its shader and opacities.
	// Part of `ResolveMaterials`, after each part's base shader is known.
	void ResolveMaterialLayers(ecs::Store &store);
}
