# mono.engine holds libraries and nothing else, and is never a product.
#
# No main.cpp lives under here and nothing ships from it on its own. Every
# program selects the subset of modules it needs; repo_layout.md §8 records
# which, and mono.tools/architecture/ checks the real graph against that table.
#
# Each module carries its own tests/ and its own AGENTS.md. The order below is
# the layer order, for reading — CMake resolves the real graph from each
# module's DEPS.

add_subdirectory(core)      # L0 platform · L1 values      [shared]
add_subdirectory(parallel)  # L2 jobs and threads          [shared]
add_subdirectory(ecs)       # L3 storage                   [shared]
add_subdirectory(world)     # L4 universe, worlds, ticking  [shared]
add_subdirectory(spatial)   # L6 where things are            [shared]
add_subdirectory(scene)     # L7 what a thing in a world is [shared]
add_subdirectory(gui)       # L7 what a 2D thing is         [shared]
add_subdirectory(assets)    # L8 content addressing         [shared]
add_subdirectory(physics)   # L8 shapes, integration, pairs [shared]
add_subdirectory(script)    # L9 running a script           [shared]
add_subdirectory(bake)      # L9 importing and baking content [shared]
add_subdirectory(graph)     # L9 what is drawn, before binding [shared]
add_subdirectory(game)      # L10 what a game file is      [shared]
add_subdirectory(examples)  # L10 the example scene        [shared]
add_subdirectory(net)       # L11 transport and authority   [shared]
add_subdirectory(delivery)  # L11 fetching content          [shared]
add_subdirectory(replication) # L12 what the wire means     [shared]
add_subdirectory(control)   # L13 the control surface      [shared]

# The presentation half of L12, configured only when something links it.
#
# Without this guard the `server` preset would still compile SDL and still need
# a shader compiler, and "the server contains no graphics stack" would be a
# claim about the link line rather than about the build. A server machine may
# have neither installed, and the only way to be sure that works is to not
# configure them.
if(MONO_BUILD_CLIENT)
	add_subdirectory(audio)   # L12 mixer graph, spatialisation [client]
	add_subdirectory(input)   # L12 devices, action mapping  [client]
	add_subdirectory(render)  # L12 RHI, render nodes        [client]
	add_subdirectory(ui)      # L12 the editor's toolkit     [client]
endif()

# TODO(D00017): the hosting half of L12 — orchestration — is a [server] module
# and gets the mirror-image guard on MONO_BUILD_SERVER when it arrives. Filed
# rather than versioned, because no version schedules it and `--worlds N` is a
# harness rather than a caller.
