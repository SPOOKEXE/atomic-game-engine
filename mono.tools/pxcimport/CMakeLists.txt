# Bounded filesystem wrapper around the source-backed PXCX imagegraph adapter.
mono_add_library(pxcimport
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::bake
		Engine::imagegraph
		Engine::imagegraphio
)

add_executable(pxcimport app/main.cpp)
target_link_libraries(pxcimport PRIVATE Tool::pxcimport)
target_compile_options(pxcimport PRIVATE ${MONO_COMPILE_OPTIONS})
set_target_properties(pxcimport PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
