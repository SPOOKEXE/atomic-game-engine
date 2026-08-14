# L4. Universe and worlds: who owns which store, and when each one ticks.
#
# No vendor and no product. This module holds simulation *structure* - a world
# is a store plus a scheduler plus a clock, and a universe is the set of them -
# so both programs run worlds out of the same code and neither owns any of it.
#
# Deliberately does not depend on: render, input, or anything client-tier. A
# world that could reach a renderer would be a world that cannot run headless,
# and running headless is what the server is.
mono_add_library(world
	TIER shared
	DEPS
		Engine::core
		Engine::parallel
		Engine::ecs
)
