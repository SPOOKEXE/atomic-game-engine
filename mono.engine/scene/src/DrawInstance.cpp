#include <engine/scene/DrawInstance.hpp>

#include <algorithm>

namespace engine::scene {

	namespace {
		// Below this a value is arithmetic noise rather than an author's
		// intent. A tween settling on "opaque" lands a few millionths off, and
		// paying a sort and a pipeline switch for that is paying for nothing.
		constexpr float TRANSPARENCY_EPSILON = 1.0f / 1024.0f;
	}

	bool IsTransparent(const DrawInstance &instance) {
		return instance.Transparency > TRANSPARENCY_EPSILON;
	}

	size_t OrderForDrawing(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	) {
		// **Resized rather than cleared and filled.** A steady scene calls this
		// every frame per view, and `clear` followed by `push_back` would
		// value-initialise every element only to overwrite it a moment later.
		order.resize(instances.size());

		// The opaque head keeps the order the world produced it in, so an opaque
		// scene comes out of this exactly as it went in — which is what makes a
		// recording of one replay, and what makes the cost on a scene with no
		// transparency a single pass and no comparisons.
		size_t opaque = 0;
		size_t transparent = instances.size();

		for (size_t index = 0; index < instances.size(); index++) {
			if (IsTransparent(instances[index])) {
				// Filled from the back, so both halves are written in one pass
				// without knowing either count in advance.
				order[--transparent] = static_cast<uint32_t>(index);
			} else {
				order[opaque++] = static_cast<uint32_t>(index);
			}
		}

		if (opaque == instances.size()) {
			return opaque;
		}

		// **The tail came out reversed, and a stable sort preserves that.**
		// This looked unnecessary until a test put three panes at one distance
		// and got them back `{2, 1, 0}`: stability only promises that *equal*
		// elements keep the order they were given, and the order they were given
		// was backwards. Reversing here is what makes "equal distances keep
		// world order" true, and that is what makes a recording of a scene with
		// coincident transparent faces replay.
		std::reverse(order.begin() + static_cast<ptrdiff_t>(opaque), order.end());

		// Farthest first. Squared distance, because the square root is monotonic
		// and cannot change an ordering — and this runs over every transparent
		// instance every frame per view.
		std::stable_sort(
			order.begin() + static_cast<ptrdiff_t>(opaque), order.end(), [&](uint32_t left, uint32_t right) {
				return (instances[left].Frame.Position - eye).MagnitudeSquared() >
					   (instances[right].Frame.Position - eye).MagnitudeSquared();
			}
		);

		return opaque;
	}

	size_t PartitionCasters(std::span<const DrawInstance> instances, std::span<uint32_t> order) {
		const auto casts = [instances](uint32_t index) {
			return index < instances.size() && instances[index].CastShadow;
		};

		return static_cast<size_t>(
			std::distance(order.begin(), std::stable_partition(order.begin(), order.end(), casts))
		);
	}

	ScenePlan OrderScene(
		std::span<const DrawInstance> instances, const core::Vector3 &eye, std::vector<uint32_t> &order
	) {
		ScenePlan plan;

		const size_t opaque = OrderForDrawing(instances, eye, order);
		plan.Opaque = static_cast<uint32_t>(opaque);
		plan.Transparent = static_cast<uint32_t>(instances.size() - opaque);

		if (opaque == 0) {
			return plan;
		}

		// **Mirrors to the back of the opaque head, so the surface pass can
		// skip them.** A mirror sits between its own reflection camera and the
		// world — the camera is *behind* the plane looking through it — so
		// drawing the pane into its own reflection fills the texture with the
		// pane, and the mirror then shows itself. That reads as a mirror which
		// is not working at all rather than as an ordering mistake.
		//
		// Physically right as well as necessary: nothing sees itself in its own
		// reflection.
		const std::span<uint32_t> head(order.data(), opaque);
		const auto boundary = std::stable_partition(head.begin(), head.end(), [instances](uint32_t index) {
			return index < instances.size() && instances[index].Surface < 0;
		});

		plan.Surfaces = static_cast<uint32_t>(std::distance(boundary, head.end()));
		plan.Reflected = plan.Opaque - plan.Surfaces;

		// Casters within each of those two runs. Nested rather than replacing
		// the partition above, for the reason `ScenePlan::SurfaceCasters`
		// gives.
		plan.ReflectedCasters =
			static_cast<uint32_t>(PartitionCasters(instances, head.subspan(0, plan.Reflected)));
		plan.SurfaceCasters =
			static_cast<uint32_t>(PartitionCasters(instances, head.subspan(plan.Reflected, plan.Surfaces)));

		return plan;
	}
}
