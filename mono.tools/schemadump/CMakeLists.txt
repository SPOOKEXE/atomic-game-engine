# The schema dump tool for classes and components.
#
# Outputs the complete TOML schema describing registered ECS components and
# classes. The registry owns the facts, so this target links each module that
# registers one rather than maintaining a second list in a document.
#
# Everything here is `shared` so this builds in a `server` preset.
add_executable(schemadump app/main.cpp)
target_link_libraries(schemadump PRIVATE
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
	Engine::examples
)
target_compile_options(schemadump PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(schemadump PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools"
)
