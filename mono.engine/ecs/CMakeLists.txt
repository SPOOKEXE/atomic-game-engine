# L3. The storage every other layer reads through.
#
# This layer is also a shipped API surface — userland gets an ECS library at
# L13 — so what leaves include/ becomes a migration cost the day it changes.
# See AGENTS.md in this directory.

mono_add_library(ecs
	TIER shared
	DEPS
		Engine::core
		Engine::parallel

	# flecs types appear in the templates in Store.hpp, so it propagates. That
	# is the debt AGENTS.md describes and the thing bindings will have to pay
	# down; it is not a licence to use flecs directly from another module.
	VENDOR_PUBLIC
		flecs::flecs_static
)
