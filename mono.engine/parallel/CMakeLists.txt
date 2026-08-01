# L2. Below the ECS, because a world's affinity is a concurrency fact and the
# storage at L3 has to be able to record it.

mono_add_library(parallel
	TIER shared
	DEPS Engine::core
)

find_package(Threads REQUIRED)
target_link_libraries(engine_parallel PUBLIC Threads::Threads)
