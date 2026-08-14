# SPIR-V to Metal Shading Language, one module at a time.
#
# **A bare executable and no library, unlike `shadercheck` next door, and the
# split between the two is the point.** Everything this program decides is a
# SPIRV-Cross option; there is no pure function here worth a suite, because the
# judgement is upstream's. What needs checking is the *output*, and the thing
# that checks it must not be the thing that produced it — `shadercheck` reads the
# emitted `.msl` back and holds it against the `.spv` it came from, which is a
# check a translator grading its own homework could not be.
#
# Invisible to `expected_graph.json`, for `contentimport`'s reason: the graph
# tracks `mono_add_library` modules and `mono_add_program` products, and a bare
# `add_executable` is neither.
#
# Client-gated by `mono.tools/CMakeLists.txt`, because `MonoVendor.cmake`
# configures SPIRV-Cross only when `MONO_BUILD_CLIENT` is on — the same shape as
# `contentimport` and shaderc.

add_executable(shadercross app/main.cpp)
target_link_libraries(shadercross PRIVATE Engine::core Vendor::spirv-cross)
target_compile_options(shadercross PRIVATE ${MONO_COMPILE_OPTIONS})

# Tools stage together, apart from the products. A tool is not a program a
# player runs, so it does not get a staged tree of its own.
set_target_properties(shadercross PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
