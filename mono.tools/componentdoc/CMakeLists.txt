# The ECS component catalogue generator.
#
# **Links past `core` for the same reason `bindings` does.** The catalogue is
# generated from the component registry, and a registry is populated by calling
# the registrations - so the tool has to link every module that has any. A
# catalogue generated against an empty table would be a valid file describing
# nothing, and its own drift check would pass forever.
#
# Everything here is `shared`, so this builds in a `server` preset. That is
# deliberate rather than incidental: the catalogue describes what a world can
# hold, and a dedicated server holds the same components a client does.
add_executable(componentdoc app/main.cpp)
target_link_libraries(componentdoc PRIVATE
	Engine::core
	Engine::ecs
	Engine::world
	Engine::scene
	Engine::gui
	Engine::physics
	Engine::effects
	Engine::graph
	Engine::script
	Engine::replication

	# **Because `examples` registers two components a saved world can hold.**
	# The example scene is linked into the client, the server and the studio
	# alike, so its rows are as much part of the catalogue as `scene`'s.
	Engine::examples)
target_compile_options(componentdoc PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(componentdoc PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
