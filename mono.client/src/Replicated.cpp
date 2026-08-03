#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <optional>

namespace client {

	using engine::core::CFrame;
	using engine::ecs::Entity;
	using engine::ecs::Phase;
	using engine::ecs::Scheduler;
	using engine::ecs::Store;
	using engine::replication::InterpolationSettings;
	using engine::replication::SnapshotBuffer;
	using engine::scene::Bounds;
	using engine::scene::DrawInstance;
	using engine::scene::Transform;
	using engine::scene::Visual;

	namespace {
		// What the server sent, turned into what a compositor takes.
		//
		// **Interpolated between two received ticks, not between two ticks of a
		// local simulation.** The demo world beside this one interpolates from
		// `PreviousTransform` to `Transform` because it owns both ends of its
		// own tick; a replica owns neither, and the two states worth
		// interpolating between arrived over a link that drops and reorders.
		// `engine::replication::SnapshotBuffer` is what holds them and decides
		// where between them the world is drawn; this only asks.
		//
		// **Nothing here writes a component.** The interpolated pose goes into a
		// `DrawInstance` and nowhere else — a render-rate quantity that reached
		// `Transform` would make the world this process replicates depend on the
		// frame rate of whoever was watching it.
		void CollectReplicated(Store &store) {
			auto *drawList = store.ResourceMutable<DrawList>();
			auto *buffer = store.ResourceMutable<SnapshotBuffer>();
			if (drawList == nullptr || buffer == nullptr) {
				return;
			}

			// Once per frame, with the frame's own seconds. The buffer reads no
			// clock of its own, which is what makes a stall something a suite
			// states rather than waits for.
			buffer->Advance(store.Time().FrameDelta);

			// `Each` rather than the batched walk the demo uses, and the reason
			// is in `Store::EachBatch`'s own header: a batch is handed columns
			// and deliberately no entity, and the pose this needs is joined *by*
			// entity. Cleared and refilled into kept capacity, so a steady world
			// allocates nothing after the first frame.
			drawList->Instances.clear();
			drawList->Instances.reserve(store.CountMatching<Transform, Bounds, Visual>());

			store.Each<const Transform, const Bounds, const Visual>(
				[drawList, buffer](
					Entity entity, const Transform &transform, const Bounds &bounds, const Visual &visual
				) {
					// Nothing for an entity the buffer has never seen, and
					// nothing for the predicted one — the first has only its
					// received pose to draw and the second must not be delayed
					// at all.
					const std::optional<CFrame> interpolated = buffer->Sample(entity);

					drawList->Instances.push_back(
						DrawInstance{
							interpolated.value_or(transform.Frame),
							bounds.HalfExtent,
							visual.Tint,
							visual.Mesh,
							visual.Material,
						}
					);
				}
			);

			// Named apart from the demo's `render.instances` so a run with
			// `--connect` says which of the two worlds produced what. A replica
			// that joined and draws nothing reads exactly like a replica that
			// never joined unless this number exists.
			engine::core::Metrics::Count(
				"replica.instances", static_cast<double>(drawList->Instances.size())
			);

			// The two numbers that say whether the interpolation is working, and
			// they are worth having on the panel rather than in a test: `behind`
			// sits at the configured delay on a healthy link and falls toward
			// zero as a late packet eats the budget, and `stalls` is what it
			// costs when it runs out.
			engine::core::Metrics::Count("replica.behind.ticks", buffer->Behind());
			engine::core::Metrics::Count("replica.stalls", static_cast<double>(buffer->Stats().Stalls));

			// Measured, and worth showing because it is not the rate this client
			// was configured with — the server's default is 30 and this one's is
			// 60, and nothing on the wire says which is right.
			engine::core::Metrics::Count("replica.tickrate", buffer->MeasuredTickRate());
		}
	}

	void
	BuildReplicatedWorld(Store &store, Scheduler &scheduler, const InterpolationSettings &interpolation) {
		// The components a snapshot names have to exist before one arrives, or
		// every name resolves to nothing and the world applies empty. These are
		// the server's types because they are nobody's types — both programs
		// register the same set, which is the whole of what v0.4 bought here.
		engine::scene::RegisterSceneComponents();

		store.SetResource(DrawList{});

		// In the store rather than on `Client`, by the same test everything else
		// here answers: a second replicated world in this process would want its
		// own, and it is read and written by a system. `mono.client/AGENTS.md`.
		store.SetResource(SnapshotBuffer{interpolation});

		// `PreRender` only. Everything in this world arrived; the one thing this
		// process is allowed to derive from it is what to draw.
		scheduler.Add("collect-replicated", Phase::PreRender, CollectReplicated);
	}

	void RecordReplicatedTick(Store &store, uint64_t tick) {
		auto *buffer = store.ResourceMutable<SnapshotBuffer>();
		if (buffer == nullptr || tick == 0 || buffer->Holds(tick)) {
			return;
		}

		store.Each<const Transform>([buffer, tick](Entity entity, const Transform &transform) {
			buffer->Record(tick, entity, transform.Frame);
		});
	}
}
