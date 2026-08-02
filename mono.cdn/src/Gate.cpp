#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>

#include <cdn/Gate.hpp>
#include <utility>

namespace cdn {

	Gate::Gate(engine::assets::GrantKey key) : Shared(std::move(key)) {}

	bool Gate::Admits(
		std::span<const std::byte> token, const engine::assets::ContentHash &bundleRoot, uint64_t nowSeconds
	) const {
		ENGINE_PROFILE("Gate::Admits");

		// Open does the MAC and the expiry, in that order, and counts which of
		// them refused. Doing either of those here as well would be a second
		// implementation of a security check, which is the one kind of
		// duplication that gets one copy updated and not the other.
		const auto grant = engine::assets::Grant::Open(token, Shared, nowSeconds);
		if (!grant) {
			engine::core::Metrics::Count("cdn.gate.refused", 1.0);
			return false;
		}

		if (!grant->Permits(bundleRoot)) {
			// A valid grant asking for content outside its scope. Counted apart
			// from a bad token: this is a client bug or a probe, and a forged
			// token is neither.
			engine::core::Metrics::Count("cdn.gate.outofscope", 1.0);
			return false;
		}

		engine::core::Metrics::Count("cdn.gate.admitted", 1.0);
		return true;
	}
}
