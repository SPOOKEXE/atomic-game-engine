# Filesystem wrapper around the testable bake library; shared-tier tooling only.
mono_add_library(assetc
	TIER shared
	NAMESPACE Tool
	DEPS
		Engine::core
		Engine::assets
		Engine::bake
)

add_executable(assetc app/main.cpp)
target_link_libraries(assetc PRIVATE Tool::assetc Engine::core Engine::assets Engine::bake)
target_compile_options(assetc PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(assetc PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
