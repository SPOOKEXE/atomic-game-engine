#pragma once

// The engine's basic logical, scalar, comparison, flow and conversion nodes as
// ang node definitions.
//
// A host registers them into its own `ang::Registry` and runs graphs through
// the ang runner, which owns caching by hash, bypass and Async completion.
// Link legality is `ang::Registry::CheckLink` and `ang::CheckNewLink`.
//
// @tier L9 · shared

#include <engine/ang/Registry.hpp>

namespace engine::graph {

	// Adds the built-in nodes to `registry`, which must already hold the ang
	// scalar types (`ang::Registry::WithScalars`). Refuses on the first id the
	// registry already has. Ids: logic.And, logic.Or, logic.Not, logic.Switch,
	// math.Add, math.Multiply, math.Clamp, math.Remap, compare.Equal,
	// compare.GreaterThan, flow.Branch, flow.Merge, flow.Reroute, value.Number,
	// value.Boolean, value.String, convert.ToString.
	ang::RegistryResult RegisterBuiltinNodes(ang::Registry &registry);
}
