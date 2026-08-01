#include <engine/ecs/Store.hpp>

namespace engine::ecs {

	Store::Store(std::string_view name)
		: StoreName(name)
		, Owner(std::this_thread::get_id()) {
		// Disabled before anything is put on it. A disabled entity is not in
		// any query's cache, so resources are unreachable from Each and from
		// CountMatching — which is what lets a type be a component on entities
		// and a resource for the world without the two seeing each other.
		auto holder = World.entity("$resources");
		holder.disable();
		ResourceHolder = holder.id();

		// The clock is the one resource that is always present, so that no
		// system has to check whether the world has a time.
		SetResource(WorldTime {});

		ENGINE_TRACE("store '{}' created", StoreName);
	}

	Store::~Store() = default;

	void Store::BindToCallingThread() {
		Owner = std::this_thread::get_id();
	}

	void Store::RequireOwningThread(const char *what) const {
		if (IsOnOwningThread()) {
			return;
		}

		// Abort rather than throw. A store touched from the wrong thread has
		// already raced by the time this runs; unwinding would leave the
		// corruption in place and hand it to whoever catches. The stack at the
		// moment of the violation is the only useful artifact.
		ENGINE_ERROR(
			"store '{}': {} called from a thread that does not own it. "
			"A world's storage is touched only by the thread that ticks it.",
			StoreName,
			what
		);
		std::abort();
	}

	Entity Store::Create() {
		RequireOwningThread("Create");
		return Entity { World.entity().id() };
	}

	Entity Store::Create(std::string_view name) {
		RequireOwningThread("Create");
		return Entity { World.entity(std::string(name).c_str()).id() };
	}

	void Store::Destroy(Entity entity) {
		RequireOwningThread("Destroy");
		World.entity(entity.Id).destruct();
	}

	bool Store::Alive(Entity entity) const {
		if (entity.Id == 0) {
			return false;
		}
		return World.is_alive(entity.Id);
	}

	WorldTime Store::Time() const {
		// Always present: the constructor sets it and nothing removes it.
		return *Resource<WorldTime>();
	}

	void Store::AdvanceTick(float delta) {
		RequireOwningThread("AdvanceTick");

		auto *time = ResourceMutable<WorldTime>();
		time->Delta = delta;
		time->Elapsed += static_cast<double>(delta);
		time->Tick++;
	}

	void Store::SetFrame(float frameDelta, float alpha) {
		RequireOwningThread("SetFrame");

		auto *time = ResourceMutable<WorldTime>();
		time->FrameDelta = frameDelta;
		time->Alpha = alpha;
	}
}
