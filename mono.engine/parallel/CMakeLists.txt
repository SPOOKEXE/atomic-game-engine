# L2. Below the ECS, because a world's affinity is a concurrency fact and the
# storage at L3 has to be able to record it.

mono_add_library(parallel
	TIER shared
	DEPS Engine::core
)

find_package(Threads REQUIRED)
target_link_libraries(engine_parallel PUBLIC Threads::Threads)

# Winsock, for the cross-process channel. Windows has no `socketpair`, so the
# pair is built out of listen/connect/accept on loopback - see
# src/platform/windows/Channel.cpp - and all of that needs the library linked
# and started before the first call.
if(WIN32)
	target_link_libraries(engine_parallel PRIVATE ws2_32)
endif()
