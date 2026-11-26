#pragma once

// Material graphs (R03): typed nodes that compile to a `Surface` function, so a
// material is authored without writing GLSL and still goes through the one
// shader compiler and library.
//
// A `MaterialGraph` instance holds its document as `Source`, one statement per
// line; `Material.Shader` names the graph like any `ShaderScript`:
//
//   # A dissolve: noise eats the surface from `amount` upwards.
//   parameter float amount = 0.3
//   parameter vec3 edge = 1, 0.45, 0.1
//   texture pattern
//   keyword GLOW
//   base default                       # start from the part's own maps
//   node n = noise uv 12
//   node cut = step amount n
//   node rim = sub 1 (smoothstep amount (add amount 0.06) n)
//   output Albedo = mix base.Albedo edge rim
//   output@GLOW Emissive = mul edge (mul rim 4)
//   discard cut
//
// Nodes take node ids, parameters, inputs (`uv`, `meshuv`, `normal`,
// `position`, `view`, `time`, `tint`, `attribute0` to `attribute3`), the base
// surface's fields (`base.Albedo`), numbers and parenthesised expressions, each
// optionally swizzled (`n.xy`). Types are float and vec2 to vec4; a float
// widens to any vector. `call Module.Function a b` calls a function a
// `ShaderModule` defines. Outputs under `@KEYWORD` exist only in that
// permutation; a graph declares at most four keywords.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/ClassId.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Most keywords one graph declares, so at most sixteen permutations.
	inline constexpr size_t MAX_GRAPH_KEYWORDS = 4;
	// Most nodes one graph holds.
	inline constexpr size_t MAX_GRAPH_NODES = 512;

	struct CompiledGraph {
		// GLSL for a `ShaderScript`: declarations then a `Surface` function.
		std::string Source;
		// `line: reason` for the first statement that is wrong; empty on success.
		std::string Error;
	};

	// Compiles a graph document. `name` labels its lines for compile errors, so
	// a mistake the GLSL compiler finds still names the graph's own line.
	CompiledGraph CompileMaterialGraph(std::string_view document, std::string_view name);

	// The `MaterialGraph` class id. Its `Source` is the document.
	ecs::ClassId MaterialGraphClass();

	// Compiled graphs by entity, rebuilt when a graph's revision moves. Derived
	// and never saved.
	struct CompiledGraphs {
		struct Entry {
			uint32_t Revision = 0;
			std::string Code;
		};
		std::unordered_map<uint64_t, Entry> Entries;
	};
}
