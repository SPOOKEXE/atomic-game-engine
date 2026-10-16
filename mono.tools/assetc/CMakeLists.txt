# Filesystem wrapper around the testable bake library; shared-tier tooling only.
mono_add_library(assetc
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::assets
		Engine::bake
		Engine::procedural
		Engine::imagegraph
)

# The end-to-end benchmark joins the ECS host to the ordinary asset cooker.
if(TARGET bench_assetc)
	target_link_libraries(bench_assetc PRIVATE Engine::script)
endif()

add_executable(assetc app/main.cpp)
target_link_libraries(assetc PRIVATE Tool::assetc Engine::core Engine::assets Engine::bake)
target_compile_options(assetc PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(assetc PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
