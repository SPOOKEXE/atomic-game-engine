#pragma once

// CPU readback of an ImageGraph output for `ImageGraph:ReadImage`.
//
// Evaluates the instance's resolved document with the deterministic CPU
// evaluator, so the same world gives the same bytes on every run and on every
// host. Sources resolve inside the same world: `editable-image://` pixels,
// `editable-mesh://` attributes and other instances' outputs
// (`imagegraph-instance://key#output`). Cooked `.atex` sources and cooked graph
// assets are refused, because this layer holds no asset bytes.

#include <engine/ecs/Entity.hpp>
#include <engine/imagegraph/Document.hpp>

#include <string_view>

namespace engine::ecs {
	class Store;
}

namespace engine::script {
	// Evaluates one output of an ImageGraph instance; empty output selects the
	// instance's Output. Refusal fills diagnostic and leaves out unspecified.
	bool ReadImageGraph(
		ecs::Store &store,
		ecs::Entity instance,
		std::string_view output,
		imagegraph::Image &out,
		imagegraph::Diagnostic &diagnostic
	);
}
