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
	// The content-origin services a product hook may borrow for its lease lifetime.
	struct ControlHookContext {
		engine::control::Surface &Surface;
		Origin &ContentOrigin;
		Service &ContentService;
		engine::control::Server &ControlServer;
	};

	// Installs the ordered MCP vocabulary this content origin can answer.
	engine::control::HookLease ConfigureControlHooks(ControlHookContext context, std::string &failure);
}
