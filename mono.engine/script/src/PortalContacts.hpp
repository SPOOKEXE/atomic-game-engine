#pragma once

#include <cstdint>

namespace engine::ecs {
	class Store;
}
namespace engine::script {
	bool RegisterPortalContacts();
	bool ConfigurePortalContacts(ecs::Store &store, uint64_t incarnation);
}
