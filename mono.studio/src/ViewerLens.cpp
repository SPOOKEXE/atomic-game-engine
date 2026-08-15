#include <studio/ViewerLens.hpp>

namespace studio {

	namespace {
		// The three fields that are the lens, compared one by one rather than
		// through a `memcmp`: the struct is three floats today and a fourth added
		// tomorrow should be a compile error here rather than a silent widening
		// of what counts as "the author touched it".
		bool Same(const engine::scene::Camera &left, const engine::scene::Camera &right) {
			return left.FieldOfViewRadians == right.FieldOfViewRadians &&
				   left.NearPlane == right.NearPlane && left.FarPlane == right.FarPlane;
		}
	}

	std::optional<engine::scene::Camera> ViewerLensToWrite(
		const engine::scene::Camera &onInstance,
		const std::optional<engine::scene::Camera> &written,
		const engine::scene::Camera &derived
	) {
		// Nothing written yet, so the camera has just been minted and the lens on
		// it is whatever `CreateInstance` left there. The editor's is better: it
		// carries the far plane the fly speed asks for.
		if (!written.has_value()) {
			return derived;
		}

		// **Somebody else has written it since, and that somebody is the author
		// or a script.** Either way it is not this panel's any more. Refusing
		// here is what makes the properties panel work on the three fields at
		// all, and it is sticky without a flag: `written` is only recorded when
		// a write happens, so it goes on disagreeing for as long as their value
		// stands.
		if (!Same(onInstance, *written)) {
			return std::nullopt;
		}

		// Ours, and unchanged. Writing an identical value would be a dirty mark
		// per panel per frame on a row nothing has moved - which the explorer
		// and the edit stream both pay for.
		if (Same(derived, onInstance)) {
			return std::nullopt;
		}

		return derived;
	}
}
