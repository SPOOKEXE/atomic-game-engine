#pragma once

// One script-owned capture callback retained by the host for one world.
//
// The callback remains opaque to this VM-neutral record. The client invokes it
// only from its completed capture barrier while the lifecycle session proves
// all systems are paused.
// @tier L9 · shared

#include <engine/script/Host.hpp>

namespace engine::script {
	struct DataCaptureDriver {
		HostCallback Callback;
	};
}
