# MonoLibrary.cmake — module and program declarations, and the tier check.
#
# Every engine library is declared with mono_add_library and carries exactly one
# tier. Every program is declared with mono_add_program and stages into its own
# runnable directory. The tier rule is checked here, at configure time, so that a
# violation fails the build with the offending edge named rather than becoming a
# convention somebody has to remember.
#
# See repo_layout.md §4 (module shape), §6 (tiers) and §15 (staged trees).

include_guard(GLOBAL)

set(MONO_KNOWN_TIERS shared client server)

# What each tier is allowed to link. One table, so the rule reads in one place.
set(MONO_TIER_ALLOWS_shared shared)
set(MONO_TIER_ALLOWS_client shared client)
set(MONO_TIER_ALLOWS_server shared server)

# Every module and program declared, in declaration order. The architecture test
# compares this against mono.tools/architecture/expected_graph.json.
set_property(GLOBAL PROPERTY MONO_ALL_MODULES "")
set_property(GLOBAL PROPERTY MONO_ALL_PROGRAMS "")

# ---------------------------------------------------------------------------
# Tier checking
# ---------------------------------------------------------------------------

# Vendor targets carry no tier. Absence means "not ours", not "shared" — a
# missing tier on a first-party target is caught by mono_add_library instead.
function(_mono_tier_of target out)
	set(tier "")
	if(TARGET ${target})
		get_target_property(tier ${target} MONO_TIER)
		if(tier STREQUAL "tier-NOTFOUND")
			set(tier "")
		endif()
	endif()
	set(${out} "${tier}" PARENT_SCOPE)
endfunction()

function(_mono_check_tiers target tier deps escapes)
	foreach(dep IN LISTS deps)
		_mono_tier_of("${dep}" dep_tier)
		if(dep_tier STREQUAL "")
			continue()
		endif()

		list(FIND MONO_TIER_ALLOWS_${tier} "${dep_tier}" allowed)
		if(allowed GREATER_EQUAL 0)
			continue()
		endif()

		list(FIND escapes "${dep}" excused)
		if(excused GREATER_EQUAL 0)
			message(STATUS "  tier escape: ${target} [${tier}] -> ${dep} [${dep_tier}]")
			continue()
		endif()

		message(FATAL_ERROR
			"Tier violation: ${target} is [${tier}] and may not depend on "
			"${dep}, which is [${dep_tier}].\n"
			"  A [${tier}] target may only link: ${MONO_TIER_ALLOWS_${tier}}\n"
			"  If this edge is deliberate, name it in ALLOW_TIER_ESCAPE and say "
			"why in a comment beside it."
		)
	endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Shaders
# ---------------------------------------------------------------------------

# A module owning GLSL compiles it into a directory named after the module, so
# two modules cannot collide on fullscreen.vert. Programs copy those directories
# in wholesale, which is why the module name is part of the path.
function(_mono_add_shaders name target)
	set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
	if(NOT IS_DIRECTORY "${source_dir}")
		return()
	endif()

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS
		"${source_dir}/*.vert" "${source_dir}/*.frag" "${source_dir}/*.comp")
	if(NOT sources)
		return()
	endif()

	if(NOT MONO_GLSLC)
		message(FATAL_ERROR
			"${name} owns shaders but no glslc was resolved.\n"
			"  A client-tier module was configured with MONO_BUILD_CLIENT off, "
			"or MONO_VENDORED_GLSLC is off and none is on PATH.")
	endif()

	set(stage "${MONO_SHADER_STAGE}/${name}")
	file(MAKE_DIRECTORY "${stage}")

	set(outputs "")
	foreach(shader IN LISTS sources)
		get_filename_component(shader_name "${shader}" NAME)
		set(spv "${stage}/${shader_name}.spv")
		add_custom_command(
			OUTPUT "${spv}"
			COMMAND ${MONO_GLSLC} "${shader}" -o "${spv}"
			# The compiler is a dependency, not just a command. Without it a
			# shaderc bump leaves every .spv on disk stale, and the mismatch
			# surfaces as a pipeline that fails to create.
			DEPENDS "${shader}" ${MONO_GLSLC_DEPENDS}
			COMMENT "SPIR-V ${name}/${shader_name}"
			VERBATIM
		)
		list(APPEND outputs "${spv}")
	endforeach()

	add_custom_target(${target}_shaders DEPENDS ${outputs})
	set_property(TARGET ${target} PROPERTY MONO_SHADER_DIR "${stage}")
	set_property(TARGET ${target} PROPERTY MONO_SHADER_TARGET ${target}_shaders)
endfunction()

# ---------------------------------------------------------------------------
# Platform sources
# ---------------------------------------------------------------------------

# Anything under src/platform/<os>/ compiles only on that <os>. The alternative
# — one file per platform, each wrapped in an #if that empties it elsewhere —
# puts the same rule in every file and lets one of them forget.
#
# No public header names an operating system. This is the only place in the
# build that does.
if(WIN32)
	set(MONO_PLATFORM windows)
elseif(ANDROID)
	set(MONO_PLATFORM android)
elseif(IOS)
	set(MONO_PLATFORM ios)
elseif(APPLE)
	set(MONO_PLATFORM macos)
elseif(UNIX)
	set(MONO_PLATFORM linux)
else()
	message(FATAL_ERROR "Unknown platform. Add it to MONO_PLATFORM in MonoLibrary.cmake.")
endif()

set(MONO_ALL_PLATFORMS windows linux macos android ios posix)

# `posix` is a family rather than a platform: everything except Windows. It
# exists so that an implementation genuinely shared by Linux, macOS, Android and
# iOS — process spawning, for one — is one file rather than four identical ones
# or one file wrapped in an #if that empties it four times.
#
# A specific platform directory still wins where the shared one is wrong; both
# would compile, so do not put two implementations of the same symbol in
# platform/posix/ and platform/linux/.
if(WIN32)
	set(MONO_PLATFORM_FAMILY windows)
else()
	set(MONO_PLATFORM_FAMILY posix)
endif()

function(_mono_select_platform_sources variable)
	set(kept "")
	foreach(source IN LISTS ${variable})
		set(excluded FALSE)
		foreach(platform IN LISTS MONO_ALL_PLATFORMS)
			if(platform STREQUAL MONO_PLATFORM OR platform STREQUAL MONO_PLATFORM_FAMILY)
				continue()
			endif()
			if(source MATCHES "/src/platform/${platform}/")
				set(excluded TRUE)
				break()
			endif()
		endforeach()
		if(NOT excluded)
			list(APPEND kept "${source}")
		endif()
	endforeach()
	set(${variable} "${kept}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# mono_add_library
# ---------------------------------------------------------------------------
#
#   mono_add_library(ecs
#     TIER shared
#     DEPS Engine::core
#   )
#
# Sources are every .cpp under src/. Public headers are include/, private
# headers are src/, and CMake enforces the split rather than trusting it.
function(mono_add_library name)
	cmake_parse_arguments(ARG
		""
		"TIER;NAMESPACE"
		"DEPS;VENDOR;VENDOR_PUBLIC;VENDOR_INCLUDES;DEFINES;ALLOW_TIER_ESCAPE"
		${ARGN})

	if(NOT ARG_TIER)
		message(FATAL_ERROR "mono_add_library(${name}) needs a TIER.")
	endif()
	list(FIND MONO_KNOWN_TIERS "${ARG_TIER}" known)
	if(known LESS 0)
		message(FATAL_ERROR "mono_add_library(${name}): unknown TIER '${ARG_TIER}'.")
	endif()

	# Engine:: for mono.engine modules. A program's own library — mono.client,
	# mono.server — passes NAMESPACE Mono, because it is a product's library
	# rather than a layer of the engine, and the alias should say so.
	if(NOT ARG_NAMESPACE)
		set(ARG_NAMESPACE Engine)
	endif()

	if(ARG_NAMESPACE STREQUAL "Engine")
		set(target engine_${name})
	else()
		set(target ${name}_lib)
	endif()

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
	file(GLOB_RECURSE headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
	_mono_select_platform_sources(sources)

	if(sources)
		add_library(${target} STATIC ${sources} ${headers})
	else()
		# Header-only modules still need a target so that tiers and the
		# dependency graph stay complete.
		add_library(${target} INTERFACE)
	endif()
	add_library(${ARG_NAMESPACE}::${name} ALIAS ${target})

	set_property(TARGET ${target} PROPERTY MONO_TIER "${ARG_TIER}")
	set_property(TARGET ${target} PROPERTY MONO_MODULE_NAME "${name}")
	set_property(TARGET ${target} PROPERTY MONO_DEPS "${ARG_DEPS};${ARG_VENDOR};${ARG_VENDOR_PUBLIC}")
	set_property(TARGET ${target} PROPERTY MONO_DECLARED_DEPS "${ARG_DEPS}")
	set_property(TARGET ${target} PROPERTY MONO_ESCAPES "${ARG_ALLOW_TIER_ESCAPE}")
	set_property(GLOBAL APPEND PROPERTY MONO_ALL_MODULES ${name})
	set_property(GLOBAL PROPERTY MONO_TARGET_${name} ${target})
	set_property(GLOBAL APPEND PROPERTY MONO_TIERED_TARGETS ${target})

	# Checked here for the immediate feedback, and again at the end of the
	# configure by mono_check_all_tiers — see the comment on that function.
	_mono_check_tiers(${target} "${ARG_TIER}" "${ARG_DEPS}" "${ARG_ALLOW_TIER_ESCAPE}")

	if(sources)
		target_compile_features(${target} PUBLIC cxx_std_20)
		target_include_directories(${target}
			PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
			PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
		# VENDOR is private and VENDOR_PUBLIC is not. A vendor type appearing in
		# a public header is the only reason to reach for the second one, and
		# every use of it widens what the rest of the engine can see.
		target_link_libraries(${target}
			PUBLIC ${ARG_DEPS} ${ARG_VENDOR_PUBLIC}
			PRIVATE ${ARG_VENDOR})
		if(ARG_VENDOR_INCLUDES)
			target_include_directories(${target} SYSTEM PRIVATE ${ARG_VENDOR_INCLUDES})
		endif()
		if(ARG_DEFINES)
			target_compile_definitions(${target} PUBLIC ${ARG_DEFINES})
		endif()
		target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
	else()
		target_compile_features(${target} INTERFACE cxx_std_20)
		target_include_directories(${target} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
		target_link_libraries(${target} INTERFACE ${ARG_DEPS} ${ARG_VENDOR})
		if(ARG_DEFINES)
			target_compile_definitions(${target} INTERFACE ${ARG_DEFINES})
		endif()
	endif()

	_mono_add_shaders(${name} ${target})

	if(MONO_BUILD_TESTS AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
		mono_add_tests(${name} DEPS ${ARG_NAMESPACE}::${name})
	endif()

	if(MONO_BUILD_BENCH AND IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks")
		mono_add_benchmarks(${name} DEPS ${ARG_NAMESPACE}::${name})
	endif()
endfunction()

# ---------------------------------------------------------------------------
# mono_add_tests
# ---------------------------------------------------------------------------
#
# One Catch2 binary per module, over every .cpp in that module's tests/. The
# binary links mono_test_main, which handles --mono-suites before handing the
# command line to Catch2 — that listing is what mono.tools/testrunner reads.
function(mono_add_tests name)
	cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
	if(NOT sources)
		return()
	endif()

	set(target test_${name})
	add_executable(${target} ${sources})
	target_link_libraries(${target} PRIVATE ${ARG_DEPS} Engine::testmain Catch2::Catch2)
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})

	# A module's own tests may reach its src/ directory, and only its own tests
	# may. AGENTS.md states the rule this implements: do not widen a public
	# header to make a test easier — link the private one instead. Without this
	# line the only way to test a private type is to publish it, which is the
	# outcome the rule exists to prevent.
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	endif()
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tests")

	add_test(NAME ${name} COMMAND ${target})
	set_property(GLOBAL APPEND PROPERTY MONO_ALL_TEST_TARGETS ${target})
endfunction()

# ---------------------------------------------------------------------------
# mono_add_benchmarks
# ---------------------------------------------------------------------------
#
# One binary per module, over every .cpp in that module's benchmarks/. The same
# shape as mono_add_tests and deliberately so: a benchmark declares
# `TEST_SUITE_ID`, answers `--mono-suites`, and is signed and selected by the
# machinery that already selects tests. A second discovery mechanism would be a
# second thing to keep correct, and the neglected one would be the one that
# silently stopped re-running.
#
# Staged into bench/ rather than tests/, so `just test` never runs a benchmark
# and `just bench` never runs a test — the two answer different questions and a
# suite that did both would be slow at one and imprecise at the other.
function(mono_add_benchmarks name)
	cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp")
	if(NOT sources)
		return()
	endif()

	set(target bench_${name})
	add_executable(${target} ${sources})
	target_link_libraries(${target} PRIVATE ${ARG_DEPS} Engine::benchmain)

	# Optimised whatever the preset says, because a debug build measures the
	# debug build. A benchmark run against unoptimised code reports a number
	# that has no relationship to the one anybody ships, and the danger is not
	# that it is slower — it is that the *ratios* between two implementations
	# invert.
	#
	# **-O3, matching `release`, and the two must not drift.** This line said
	# -O2 while `release` inherited -O2 from RelWithDebInfo, so they agreed by
	# coincidence rather than by construction — and when `release` moved to -O3
	# this became the one place that would have gone on reporting the old
	# number. A benchmark compiled differently from the thing it is a benchmark
	# of measures a binary nobody ships, which is the same failure as measuring
	# a debug build and is harder to notice. If `release` changes level again,
	# change it here in the same commit.
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
	if(NOT MSVC)
		target_compile_options(${target} PRIVATE -O3 -g)
	endif()

	# A module's own benchmarks may reach its src/ directory, for the same
	# reason its tests may: measuring a private type must not require
	# publishing it.
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	endif()

	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/bench")
	set_property(GLOBAL APPEND PROPERTY MONO_ALL_BENCH_TARGETS ${target})
endfunction()

# ---------------------------------------------------------------------------
# mono_add_program
# ---------------------------------------------------------------------------
#
# A program is a thin main over libraries. It stages into its own directory —
# binary, shared libraries and the shaders of every module it links, and nothing
# else. A server/ directory that has grown a shaders/ folder is a link-line
# mistake anyone can see. repo_layout.md §15.
function(mono_add_program name)
	cmake_parse_arguments(ARG
		""
		"TIER;SOURCE"
		"DEPS;VENDOR;ALLOW_TIER_ESCAPE"
		${ARGN})

	if(NOT ARG_TIER)
		message(FATAL_ERROR "mono_add_program(${name}) needs a TIER.")
	endif()
	if(NOT ARG_SOURCE)
		set(ARG_SOURCE "app/main.cpp")
	endif()

	set(target ${name})
	add_executable(${target} "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
	target_compile_features(${target} PRIVATE cxx_std_20)
	target_link_libraries(${target} PRIVATE ${ARG_DEPS} ${ARG_VENDOR})
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})

	set_property(TARGET ${target} PROPERTY MONO_TIER "${ARG_TIER}")
	set_property(TARGET ${target} PROPERTY MONO_DEPS "${ARG_DEPS};${ARG_VENDOR}")
	set_property(TARGET ${target} PROPERTY MONO_DECLARED_DEPS "${ARG_DEPS}")
	set_property(TARGET ${target} PROPERTY MONO_ESCAPES "${ARG_ALLOW_TIER_ESCAPE}")
	set_property(GLOBAL APPEND PROPERTY MONO_ALL_PROGRAMS ${name})
	set_property(GLOBAL APPEND PROPERTY MONO_TIERED_TARGETS ${target})

	_mono_check_tiers(${target} "${ARG_TIER}" "${ARG_DEPS}" "${ARG_ALLOW_TIER_ESCAPE}")

	set(stage "${MONO_STAGE_ROOT}/${name}")
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${stage}"
		LIBRARY_OUTPUT_DIRECTORY "${stage}")

	# The staged directory has to be runnable as it stands, which means the
	# loader must find the shared libraries *there* and not in the build tree.
	#
	# BUILD_WITH_INSTALL_RPATH is the load-bearing part: without it CMake adds
	# every link directory to the build rpath, the binary silently resolves
	# SDL3 out of mono.vendor/sdl/, and the staged tree looks self-contained
	# right up until somebody copies it somewhere else.
	if(APPLE)
		set_target_properties(${target} PROPERTIES
			BUILD_WITH_INSTALL_RPATH TRUE
			INSTALL_RPATH "@executable_path")
	elseif(UNIX)
		set_target_properties(${target} PROPERTIES
			BUILD_WITH_INSTALL_RPATH TRUE
			INSTALL_RPATH "$ORIGIN")
	endif()

	# Shaders reach a program the same way code does: only from modules it links.
	_mono_transitive_deps("${ARG_DEPS}" all_deps)
	set(shader_targets "")
	set(shader_commands "")
	foreach(dep IN LISTS all_deps)
		get_target_property(dir ${dep} MONO_SHADER_DIR)
		if(NOT dir OR dir STREQUAL "dir-NOTFOUND")
			continue()
		endif()
		get_target_property(module ${dep} MONO_MODULE_NAME)
		get_target_property(shader_target ${dep} MONO_SHADER_TARGET)
		list(APPEND shader_targets ${shader_target})
		list(APPEND shader_commands
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${dir}" "${stage}/shaders/${module}")
	endforeach()

	if(shader_commands)
		# A target of its own rather than a POST_BUILD command, so that a
		# shader-only edit still stages without the program relinking.
		add_custom_target(${name}_stage_shaders ALL ${shader_commands}
			COMMENT "Staging shaders into ${stage}/shaders"
			VERBATIM)
		add_dependencies(${name}_stage_shaders ${shader_targets})

		# The program depends on the staging, not the other way round.
		#
		# Reversed, `ALL` still stages it on a full build — and `cmake --build
		# --target client` does not, because nothing it was asked for depends on
		# it. That is exactly what `just run` asks for, so a fresh preset built
		# the client, skipped its shaders, and failed at startup with four
		# "shader not found" lines and no clue that a whole target had been
		# missed. A full build had always happened to run first.
		add_dependencies(${target} ${name}_stage_shaders)
	endif()

	# SDL3 is a shared library; it has to sit beside the binary that loads it.
	if(TARGET SDL3-shared)
		list(FIND all_deps SDL3-shared links_sdl)
		if(links_sdl GREATER_EQUAL 0)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:SDL3-shared>" "${stage}"
				VERBATIM)

			# The binary asks the loader for the soname — libSDL3.so.0 — and
			# not for the versioned file. Copying only the latter produces a
			# staged tree that is missing the name it actually looks up.
			if(UNIX AND NOT APPLE)
				add_custom_command(TARGET ${target} POST_BUILD
					COMMAND ${CMAKE_COMMAND} -E copy_if_different
						"$<TARGET_SONAME_FILE:SDL3-shared>" "${stage}"
					VERBATIM)
			endif()
		endif()
	endif()
endfunction()

# Walks MONO_DEPS breadth-first. Vendor targets have no such property, so the
# walk stops at them, which is what we want — we only care about our own graph
# plus the vendor leaves it reaches.
function(_mono_transitive_deps roots out)
	set(seen "")
	set(queue ${roots})
	while(queue)
		list(POP_FRONT queue current)
		if(NOT TARGET ${current})
			continue()
		endif()
		# Resolve the alias so Engine::core and engine_core are one entry.
		get_target_property(aliased ${current} ALIASED_TARGET)
		if(aliased)
			set(current ${aliased})
		endif()
		list(FIND seen ${current} already)
		if(already GREATER_EQUAL 0)
			continue()
		endif()
		list(APPEND seen ${current})

		get_target_property(deps ${current} MONO_DEPS)
		if(deps AND NOT deps STREQUAL "deps-NOTFOUND")
			list(APPEND queue ${deps})
		endif()
	endwhile()
	set(${out} "${seen}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The deferred tier check
# ---------------------------------------------------------------------------

# The check inside mono_add_library runs while the directory is being
# processed, so it can only see targets declared before it. CMake resolves link
# dependencies lazily, which means a forward reference — `ecs` listing
# `Mono::server`, declared two add_subdirectory calls later — passes it in
# silence. That is precisely the violation worth catching, so the whole set is
# re-checked here once every target exists.
#
# Called from the root CMakeLists after the last add_subdirectory.
function(mono_check_all_tiers)
	get_property(targets GLOBAL PROPERTY MONO_TIERED_TARGETS)

	foreach(target IN LISTS targets)
		get_target_property(tier ${target} MONO_TIER)
		get_target_property(deps ${target} MONO_DECLARED_DEPS)
		get_target_property(escapes ${target} MONO_ESCAPES)

		if(NOT deps OR deps STREQUAL "deps-NOTFOUND")
			continue()
		endif()
		if(escapes STREQUAL "escapes-NOTFOUND")
			set(escapes "")
		endif()

		foreach(dep IN LISTS deps)
			if(NOT TARGET ${dep})
				message(FATAL_ERROR
					"${target} lists '${dep}' in DEPS, and no such target exists.\n"
					"  A misspelled dependency links fine — CMake would resolve it as a "
					"bare library name at link time — so it is refused here instead."
				)
			endif()
		endforeach()

		_mono_check_tiers(${target} "${tier}" "${deps}" "${escapes}")
	endforeach()
endfunction()

# ---------------------------------------------------------------------------
# The target graph, for mono.tools/architecture/
# ---------------------------------------------------------------------------

function(mono_write_target_graph path)
	get_property(modules GLOBAL PROPERTY MONO_ALL_MODULES)
	get_property(programs GLOBAL PROPERTY MONO_ALL_PROGRAMS)

	# The options that decide which targets exist at all. Without these, the
	# expectation cannot tell "this module was removed" from "this preset does
	# not build it", and would have to accept both.
	set(json "{\n  \"options\": {")
	set(first TRUE)
	foreach(option IN ITEMS
			MONO_BUILD_CLIENT MONO_BUILD_SERVER MONO_BUILD_CDN
			MONO_BUILD_TESTS MONO_TRACY MONO_OPTIMISE)
		if(${option})
			set(value "true")
		else()
			set(value "false")
		endif()
		if(first)
			set(first FALSE)
		else()
			string(APPEND json ",")
		endif()
		string(APPEND json "\n    \"${option}\": ${value}")
	endforeach()
	string(APPEND json "\n  },\n  \"modules\": {")
	set(first TRUE)
	foreach(module IN LISTS modules)
		get_property(module_target GLOBAL PROPERTY MONO_TARGET_${module})
		_mono_graph_entry(${module_target} "${module}" entry)
		if(first)
			set(first FALSE)
		else()
			string(APPEND json ",")
		endif()
		string(APPEND json "\n    ${entry}")
	endforeach()
	string(APPEND json "\n  },\n  \"programs\": {")

	set(first TRUE)
	foreach(program IN LISTS programs)
		_mono_graph_entry(${program} "${program}" entry)
		if(first)
			set(first FALSE)
		else()
			string(APPEND json ",")
		endif()
		string(APPEND json "\n    ${entry}")
	endforeach()
	string(APPEND json "\n  }\n}\n")

	file(WRITE "${path}" "${json}")
endfunction()

function(_mono_graph_entry target label out)
	get_target_property(tier ${target} MONO_TIER)
	_mono_transitive_deps("${target}" all_deps)

	set(names "")
	foreach(dep IN LISTS all_deps)
		get_target_property(module ${dep} MONO_MODULE_NAME)
		if(module AND NOT module STREQUAL "module-NOTFOUND" AND NOT module STREQUAL "${label}")
			list(APPEND names "\"${module}\"")
		endif()
	endforeach()
	list(SORT names)
	list(JOIN names ", " joined)

	set(${out} "\"${label}\": { \"tier\": \"${tier}\", \"links\": [${joined}] }" PARENT_SCOPE)
endfunction()
