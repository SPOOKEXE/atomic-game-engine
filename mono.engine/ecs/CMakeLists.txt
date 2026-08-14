# L3. The storage every other layer reads through.
#
# This layer is also a shipped API surface - userland gets an ECS library at
# L13 - so what leaves include/ becomes a migration cost the day it changes.
# See AGENTS.md in this directory.

# No vendor appears here at all, and that is the point. The storage is the
# engine's own - Column, ComponentSet, SparseSet and the archetype behind them -
# so nothing about a third-party ECS propagates into the templates in Store.hpp.
# `Store::Native()` was the debt AGENTS.md described; it is paid.
mono_add_library(ecs
	TIER shared
	DEPS
		Engine::core
		Engine::parallel
)
