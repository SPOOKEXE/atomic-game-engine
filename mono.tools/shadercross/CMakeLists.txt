# SPIR-V to Metal Shading Language, one module at a time.
#
# **A bare executable and no library**, because the translation is
# `Engine::msl` — `render` needs the same one while the engine runs, and one
# implementation is what stops a `ShaderScript` and a built-in shader from
# landing on different Metal indices. What is left here is a file to read and a
# path to write, which is a main.cpp's worth of program.
#
# **What checks the output is not this.** `shadercheck` reads the emitted `.msl`
# back and holds it against the `.spv` it came from, deriving the same indices
# from SDL's documentation independently — a translator grading its own homework
# would not be a check.
#
# Invisible to `expected_graph.json`, for `contentimport`'s reason: the graph
# tracks `mono_add_library` modules and `mono_add_program` products, and a bare
# `add_executable` is neither. `Engine::msl` is in it.
#
# Client-gated by `mono.tools/CMakeLists.txt`, because `MonoVendor.cmake`
# configures SPIRV-Cross only when `MONO_BUILD_CLIENT` is on — the same shape as
# `contentimport` and shaderc.

add_executable(shadercross app/main.cpp)
target_link_libraries(shadercross PRIVATE Engine::core Engine::msl)
target_compile_options(shadercross PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(shadercross PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
