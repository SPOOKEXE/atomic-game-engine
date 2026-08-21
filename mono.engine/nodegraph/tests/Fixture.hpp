#pragma once

// The suite's own node vocabulary.
//
// **The library ships no node types, so its tests bring their own.** This
// module knows nothing about terrain, pictures or arithmetic: every node type
// arrives through `NodeTypes::Register`, and the set the studio's Demo Nodes
// panel uses is `mono.studio/src/DemoNodes.cpp`, which is a consumer's content.
// Reaching for that from here would tie this suite to a panel that moves for
// reasons that have nothing to do with the model - so these cases register a
// vocabulary they own and can reason about.
//
// It is deliberately dull. The evaluations are the cheapest thing that produces
// a distinguishable answer, because what is under test is the *model*: the
// cycle guard, the content hash, the fold, the save format and the evaluator's
// scheduling. A fixture that computed real noise would make a slow suite that
// tested the noise.
//
// The one place it is not dull is `task.staged`, which sleeps in slices and
// polls `Inputs::Cancelled`. The async cases need something genuinely slow, and
// a task that returned immediately would make every one of them pass whatever
// the evaluator did.

#include <engine/nodegraph/Graph.hpp>

#include <cstdint>
#include <vector>

namespace fixture {

	// A square scalar grid, in 0..1. What a `data.FIELD` wire carries here.
	//
	// The library never learns what this is - it moves as `std::any` and only
	// the type's own `Preview`, `Describe` and `Heights` ever open it, which is
	// the property `Inputs::In` exists to keep.
	struct Field {
		uint32_t Side = 0;
		std::vector<float> Samples;

		float At(uint32_t x, uint32_t y) const {
			return Samples[static_cast<size_t>(y) * Side + x];
		}
	};

	// Registers the data types and node types the cases below use.
	//
	// **Idempotent, and registered on first use rather than before `main`.**
	// `NodeTypes::Register` replaces by id, and every case calls this rather
	// than depending on another case having run - a suite whose cases only pass
	// in order is one that fails when somebody runs a single test.
	void RegisterFixtureNodes();
}
