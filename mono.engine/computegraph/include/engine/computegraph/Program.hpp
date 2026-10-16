#pragma once

// Executable source identities stay neutral; rendering hosts compile and reflect them.
// @tier L3 · shared

#include <engine/computegraph/Document.hpp>

namespace engine::computegraph {
	// Named executable code supplied independently of its authored binding contract.
	struct KernelCode {
		// Stable identity matching one authored kernel interface.
		std::string Name{};
		// Supply exactly one inline GLSL source or staged engine shader filename.
		std::string Source{};
		// Staged shader filename, exclusive with Source.
		std::string BuiltIn{};
	};
	// A complete reusable typed graph and executable source declarations.
	struct Program {
		// Domain-owned buffer layouts, named interfaces and dependent passes.
		Document Definition{};
		// One executable source for each reusable kernel declaration.
		std::vector<KernelCode> Kernels{};
		// Every nonzero diagnostic word prevents publication of every requested output.
		std::vector<std::string> ErrorBuffers{};
	};
}
