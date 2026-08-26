#include <engine/ecs/Classes.hpp>
#include <engine/script/Changes.hpp>

namespace engine::script {

	namespace {
		using ecs::ComponentId;
		using ecs::Entity;
		using ecs::PropertyDescriptor;
		using ecs::Store;
	}

	uint64_t ChangeQueue::KeyOf(Entity instance, core::Name property) {
		return (instance.Id << 32) ^ (instance.Id >> 32) ^ static_cast<uint64_t>(property.Id());
	}

	void ChangeQueue::Watch(Store &store, Entity instance) {
		Watched.insert(instance.Id);

		for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Reads == nullptr) {
				continue;
			}

			for (const ComponentId component : property.Reads->Ids()) {
				// Once per component for the whole world. The sets overlap
				// heavily - `CFrame`, `Position` and `Orientation` all read
				// `Transform` - so a part's eleven properties buy far fewer
				// than eleven subscriptions.
				if (!Subscribed.insert(component.Index).second) {
					continue;
				}

				Subscriptions.push_back(store.OnChangedComponent(
					component, [this, component](Store &world, Entity changed, const void *) {
						// Fires for every entity whose component moved,
						// watched or not. The filter is what makes a
						// per-type signal a per-instance one, and it is a
						// hash lookup rather than a walk of the listeners.
						if (Watched.find(changed.Id) != Watched.end()) {
							Fan(world, changed, component);
						}
					}
				));
			}
		}
	}

	void ChangeQueue::Unwatch(Entity instance) {
		Watched.erase(instance.Id);

		// Anything already queued for it is dropped too. A script that
		// disconnected its last listener and then received the call it had just
		// cancelled would be right to call that a bug.
		std::erase_if(Pending, [&](const Change &change) {
			if (change.Instance != instance) {
				return false;
			}
			Queued.erase(KeyOf(change.Instance, change.Property));
			return true;
		});
	}

	void ChangeQueue::Fan(const Store &store, Entity instance, ComponentId component) {
		for (const PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Reads == nullptr || !property.Reads->Contains(component)) {
				continue;
			}

			// **Any component in the set, not all of them**, and that is the
			// correct direction to be imprecise in. `Position` and `Orientation`
			// both read the whole `Transform`, so a pure rotation reports
			// `Position` as well. A spurious call a script can ignore beats a
			// missed one it can never recover from.
			if (Queued.insert(KeyOf(instance, property.Name)).second) {
				Pending.push_back(Change{instance, property.Name});
			}
		}
	}

	void ChangeQueue::Record(Entity instance, core::Name name) {
		// The same dedup `Fan` uses, so an attribute written three times in one
		// tick signals once with the value it ended at.
		if (Queued.insert(KeyOf(instance, name)).second) {
			Pending.push_back(Change{instance, name});
		}
	}

	void ChangeQueue::Drain(const std::function<void(Entity, core::Name)> &body) {
		if (Pending.empty()) {
			return;
		}

		// Moved out before anything runs. A handler may write a property, and
		// that write belongs to the *next* barrier - appending to the vector
		// being walked would fire it in this one and, for a handler that writes
		// what it listens to, would never stop.
		std::vector<Change> draining;
		draining.swap(Pending);
		Queued.clear();

		for (const Change &change : draining) {
			body(change.Instance, change.Property);
		}
	}

	void ChangeQueue::Detach(Store &store) {
		for (const Store::Connection connection : Subscriptions) {
			store.Disconnect(connection);
		}

		Subscriptions.clear();
		Subscribed.clear();
		Watched.clear();
		Queued.clear();
		Pending.clear();
	}
}
