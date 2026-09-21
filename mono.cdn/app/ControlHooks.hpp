#pragma once

#include <engine/control/HookRegistry.hpp>

#include <string>

namespace cdn {
	class Origin;
	class Service;
}

namespace engine::control {
	class Server;
	class Surface;
}

namespace cdn {

	// Installs the ordered MCP vocabulary this content origin can answer.
	engine::control::HookLease ConfigureControlHooks(
		engine::control::Surface &surface,
		Origin &origin,
		Service &service,
		engine::control::Server &server,
		std::string &failure
	);
}
