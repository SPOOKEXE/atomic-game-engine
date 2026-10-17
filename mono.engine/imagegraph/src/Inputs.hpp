#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	// Compiles an already resolved snapshot, without recursively applying inputs.
	bool CompileResolved(const Document &, Plan &, Diagnostic &);
}
