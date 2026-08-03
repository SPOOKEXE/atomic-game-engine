#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>

#include <algorithm>
#include <client/Demo.hpp>
#include <client/Replicated.hpp>

namespace client {

	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		// What the server sent, turned into what a compositor takes.
		//
		// **No interpolation, and that is a stated gap rather than an
		// oversight.** The demo interpolates between `PreviousTransform` and
		// `Transform` because it owns both ends of its own tick. A replica owns
		// neither: what it has is the last snapshot the authority sent, and the
		// two states worth interpolating between are two *received* ticks, which
		// is snapshot buffering and belongs in `replication` rather than here.
		// Drawing the newest received transform is honest — it judders at the
		// server's tick rate, and inventing a smoother lie in this file would
		// put a second interpolator in the engine.
		void CollectReplicated(Store &store) {
			auto *drawList = store.ResourceMutable<DrawList>();
			if (drawList == nullptr) {
				return;
			}

			// Sized once, then written by index, exactly as the demo's does: on
			// a steady world this is a no-op and no element is value-initialised
			// only to be overwritten. The count is a floor rather than a
			// contract — it comes from a different query than the one the batch
			// walks — so the shrink below settles the real size.
			const size_t matching = store.CountMatching<Transform, Bounds, Visual>();
			drawList->Instances.resize(matching);

			DrawInstance *const out = drawList->Instances.data();
			const size_t capacity = drawList->Instances.size();

			const size_t written = store.EachBatchParallel<const Transform, const Bounds, const Visual>(
				[out, capacity](
					size_t first,
					size_t rows,
					const Transform *transforms,
					const Bounds *bounds,
					const Visual *visuals
				) {
					// The two queries agree, and this is what happens if
					// they ever stop: instances go missing rather than a
					// worker writing past the end of the buffer.
					if (first >= capacity) {
						return;
					}
					rows = std::min(rows, capacity - first);

					for (size_t row = 0; row < rows; row++) {
						out[first + row] = DrawInstance{
							transforms[row].Frame,
							bounds[row].HalfExtent,
							visuals[row].Tint,
							visuals[row].Mesh,
							visuals[row].Material,
						};
					}
				}
			);

			drawList->Instances.resize(std::min(written, drawList->Instances.size()));

			// Named apart from the demo's `render.instances` so a run with
			// `--connect` says which of the two worlds produced what. A replica
			// that joined and draws nothing reads exactly like a replica that
			// never joined unless this number exists.
			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);
		}
	}

	void BuildReplicatedWorld(Store &store, Scheduler &scheduler) {
		// The components a snapshot names have to exist before one arrives, or
		// every name resolves to nothing and the world applies empty. These are
		// the server's types because they are nobody's types — both programs
		// register the same set, which is the whole of what v0.4 bought here.
		engine::scene::RegisterSceneComponents();

		store.SetResource(DrawList{});

		// `PreRender` only. Everything in this world arrived; the one thing this
		// process is allowed to derive from it is what to draw.
		scheduler.Add("collect-replicated", Phase::PreRender, CollectReplicated);
	}
}
