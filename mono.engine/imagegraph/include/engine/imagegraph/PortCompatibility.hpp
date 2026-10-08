#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	/// Directional source junction casts for links touching catalogue nodes or authored junctions.
	/// Explicit union sockets and computed noise coordinates must be checked separately.
	bool CatalogueJunctionCompatible(ValueType from, ValueType to);
}
