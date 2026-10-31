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

# Keep fault injection outside test_ecs: that binary owns the heap profiler's
# global allocator, while this helper wraps only its own linked allocations.
if(MONO_BUILD_TESTS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
	CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CROSSCOMPILING)
	add_executable(ecs_allocation_fault fault/Allocation.cpp)
	target_link_libraries(ecs_allocation_fault PRIVATE Engine::ecs)
	target_include_directories(ecs_allocation_fault PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	target_link_options(ecs_allocation_fault PRIVATE
		"-Wl,--wrap=_Znwm" "-Wl,--wrap=_ZnwmSt11align_val_t"
		"-Wl,--wrap=_ZdlPv" "-Wl,--wrap=_ZdlPvm" "-Wl,--wrap=_ZdlPvSt11align_val_t")
	add_dependencies(test_ecs ecs_allocation_fault)
	set_property(SOURCE tests/SparseSet.cpp APPEND PROPERTY COMPILE_DEFINITIONS
		"MONO_ECS_ALLOCATION_FAULT=\"$<TARGET_FILE:ecs_allocation_fault>\"")
endif()
