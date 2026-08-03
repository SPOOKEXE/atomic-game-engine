#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <client/Compositor.hpp>
#include <cstring>

namespace client {

	namespace {
		// A view frame is a camera and then an instance array.
		//
		// Copied as object representations, which is legal here and nowhere
		// near a wire: both ends of this channel are this process, and
		// `ViewChannel`'s payload is opaque bytes precisely so that the two
		// ends can agree on a layout the layer between them never learns. The
		// instances are `scene::DrawInstance` now, which is the `shared` type a
		// host of any tier can write — but a view crossing a *process* boundary
		// still needs a real encoding rather than this memcpy, because the
		// `core::Name`s inside a draw instance are process-local ids.
		struct Prefix {
			engine::core::CFrame Frame;
			engine::scene::Camera Camera;
			uint32_t Instances = 0;
			uint32_t Reserved = 0;
		};

		size_t PayloadFor(size_t instances) {
			return sizeof(Prefix) + instances * sizeof(engine::scene::DrawInstance);
		}
	}

	void Compositor::Track(engine::world::WorldId id, engine::core::Name world, size_t maximumInstances) {
		if (Find(id) != nullptr) {
			return;
		}

		Slot slot;
		slot.State.Id = id;
		slot.State.World = world;
		slot.Channel = std::make_unique<engine::world::ViewChannel>(PayloadFor(maximumInstances));
		slot.Payload.reserve(PayloadFor(maximumInstances));

		Slots.push_back(std::move(slot));
		States.emplace_back(Slots.back().State);
	}

	Compositor::Slot *Compositor::Find(engine::world::WorldId id) {
		const auto found = std::find_if(Slots.begin(), Slots.end(), [id](const Slot &slot) {
			return slot.State.Id.Index == id.Index;
		});
		return found == Slots.end() ? nullptr : &*found;
	}

	bool Compositor::Publish(
		engine::world::WorldId id,
		const engine::core::CFrame &frame,
		const engine::scene::Camera &camera,
		std::span<const engine::scene::DrawInstance> list,
		uint64_t tick,
		float alpha
	) {
		Slot *slot = Find(id);
		if (slot == nullptr) {
			return false;
		}

		const size_t bytes = PayloadFor(list.size());
		if (bytes > slot->Channel->MaximumPayload()) {
			// Refused rather than truncated. Half a draw list is a frame with
			// holes in it, which reads as a rendering bug rather than as the
			// budget overrun it is.
			ENGINE_WARN(
				"view '{}': {} instances is past this channel's maximum.",
				slot->State.World.Text(),
				list.size()
			);
			return false;
		}

		// One buffer, reused. A publisher that allocated inside its own render
		// phase would be paying the allocator once a frame per world.
		Scratch.resize(bytes);

		Prefix prefix;
		prefix.Frame = frame;
		prefix.Camera = camera;
		prefix.Instances = static_cast<uint32_t>(list.size());
		std::memcpy(Scratch.data(), &prefix, sizeof(Prefix));

		if (!list.empty()) {
			std::memcpy(Scratch.data() + sizeof(Prefix), list.data(), bytes - sizeof(Prefix));
		}

		engine::world::ViewHeader header;
		header.World = slot->State.World;
		header.SourceTick = tick;
		header.Alpha = alpha;
		header.Camera = frame;
		header.PayloadBytes = static_cast<uint32_t>(bytes);

		return slot->Channel->Publish(header, Scratch);
	}

	void Compositor::Compose(float spacing) {
		ENGINE_PROFILE_CAT("Compositor::Compose", engine::core::ProfileCategory::Render);

		Combined.clear();

		for (size_t index = 0; index < Slots.size(); index++) {
			Slot &slot = Slots[index];

			engine::world::ViewHeader header;
			if (slot.Channel->Acquire(header, slot.Payload)) {
				slot.State.Header = header;
				slot.State.Fresh = true;
				slot.State.Stale = 0;
			} else {
				// The last frame is kept and redrawn. A world flickering out of
				// existence for one frame is worse than a world one frame
				// behind, and a compositor running faster than its producers is
				// the normal case rather than a fault.
				slot.State.Fresh = false;
				slot.State.Stale++;
			}

			if (slot.Payload.size() < sizeof(Prefix)) {
				slot.State.Instances = 0;
				continue;
			}

			Prefix prefix;
			std::memcpy(&prefix, slot.Payload.data(), sizeof(Prefix));

			// Trusted only as far as the buffer goes. The producer is this
			// process, but a count read back out of a buffer is still a count
			// that decides how far a memcpy runs.
			const size_t available =
				(slot.Payload.size() - sizeof(Prefix)) / sizeof(engine::scene::DrawInstance);
			const size_t count = std::min<size_t>(prefix.Instances, available);
			slot.State.Instances = count;

			if (index == 0) {
				ViewFrame = prefix.Frame;
				ViewCamera = prefix.Camera;
			}

			const size_t before = Combined.size();
			Combined.resize(before + count);
			if (count > 0) {
				std::memcpy(
					Combined.data() + before,
					slot.Payload.data() + sizeof(Prefix),
					count * sizeof(engine::scene::DrawInstance)
				);
			}

			// Placed, because two worlds' coordinates do not mean the same
			// thing — nothing says they should, and overlaying them would draw
			// two scenes inside each other and call it one.
			if (index > 0 && spacing != 0.0f) {
				const float offset = spacing * static_cast<float>(index);
				for (size_t at = before; at < Combined.size(); at++) {
					Combined[at].Frame.Position.X += offset;
				}
			}
		}

		// Framed so the whole row is visible rather than only the first world.
		// A compositor with one camera and several worlds has to choose, and
		// showing something beats showing the first world and a black gap.
		if (Slots.size() > 1 && spacing != 0.0f) {
			const float span = spacing * static_cast<float>(Slots.size() - 1);
			ViewFrame.Position.X += span * 0.5f;
			ViewFrame.Position.Z += span * 0.5f;
		}

		for (size_t index = 0; index < Slots.size(); index++) {
			States[index] = Slots[index].State;
		}
	}

	uint64_t Compositor::Dropped() const {
		uint64_t dropped = 0;
		for (const Slot &slot : Slots) {
			dropped += slot.Channel->Dropped();
		}
		return dropped;
	}

	size_t Compositor::StaleViews() const {
		size_t stale = 0;
		for (const Slot &slot : Slots) {
			if (!slot.State.Fresh) {
				stale++;
			}
		}
		return stale;
	}
}
