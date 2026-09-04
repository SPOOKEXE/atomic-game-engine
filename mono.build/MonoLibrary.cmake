# MonoLibrary.cmake - module and program declarations, and the tier check.
#
# Every engine library is declared with mono_add_library and carries exactly one
# tier. Every program is declared with mono_add_program and stages into its own
# runnable directory. The tier rule is checked here, at configure time, so that a
# violation fails the build with the offending edge named rather than becoming a
# convention somebody has to remember.
#
# See docs/CODE_ARCH.md for the shape of a module (§3), the tiers (§5) and what
# each program links (§7).

include_guard(GLOBAL)

set(MONO_LIBRARY_DIR "${CMAKE_CURRENT_LIST_DIR}")
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

# Vendor targets carry no tier. Absence means "not ours", not "shared" - a
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
# A constant a shader shares with C++ is passed in rather than typed twice.
#
# `defines` is a list of `NAME=VALUE`, each becoming a `-D` on the glslc command
# line. It exists because a number spelled in a header *and* in GLSL is a
# constraint the build does not check, which AGENTS.md rule 6 calls
# documentation - and the failure it hides is quiet in both directions: a shader
# capped lower silently ignores the tail of a uniform array, and one capped
# higher reads past the buffer. Both look like "that light does not work".
#
# The value is read out of the header that declares it, so C++ stays the one
# home and the shader carries no literal of its own to disagree with. A shader
# still compiles standalone, because each `#ifndef`s its own fallback.
function(_mono_add_shaders name target defines)
	set(source_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
	if(NOT IS_DIRECTORY "${source_dir}")
		return()
	endif()

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS
		"${source_dir}/*.vert" "${source_dir}/*.frag" "${source_dir}/*.comp")
	if(NOT sources)
		return()
	endif()

	# **`.glsl` is a shared fragment, not a stage, and it is compiled by nobody.**
	# `instance.glsl` holds the decode `opaque.vert` and `shadow.vert` both need;
	# a stage `#include`s it and glslc resolves the quoted path against the
	# including file's own directory.
	#
	# Every stage in the module depends on every one of them, which is coarser
	# than tracking real include edges and is the right trade: a shader fragment
	# is a handful of files, over-building one module's shaders costs a second,
	# and the alternative failure is the one this build already refuses to
	# tolerate - an edit that leaves a stale `.spv` on disk and surfaces as a
	# pipeline that will not create.
	file(GLOB_RECURSE shader_headers CONFIGURE_DEPENDS "${source_dir}/*.glsl")

	if(NOT MONO_GLSLC)
		message(FATAL_ERROR
			"${name} owns shaders but no glslc was resolved.\n"
			"  A client-tier module was configured with MONO_BUILD_CLIENT off, "
			"or MONO_VENDORED_GLSLC is off and none is on PATH.")
	endif()

	set(stage "${MONO_SHADER_STAGE}/${name}")
	file(MAKE_DIRECTORY "${stage}")

	set(define_flags "")
	foreach(define IN LISTS defines)
		list(APPEND define_flags "-D${define}")
	endforeach()

	set(outputs "")
	foreach(shader IN LISTS sources)
		get_filename_component(shader_name "${shader}" NAME)
		set(spv "${stage}/${shader_name}.spv")
		add_custom_command(
			OUTPUT "${spv}"
			COMMAND ${MONO_GLSLC} ${define_flags} -I "${source_dir}" "${shader}" -o "${spv}"
			# The compiler is a dependency, not just a command. Without it a
			# shaderc bump leaves every .spv on disk stale, and the mismatch
			# surfaces as a pipeline that fails to create. The `.glsl` fragments
			# are dependencies for the same reason - see the glob above.
			DEPENDS "${shader}" ${shader_headers} ${MONO_GLSLC_DEPENDS}
			COMMENT "SPIR-V ${name}/${shader_name}"
			VERBATIM
		)
		list(APPEND outputs "${spv}")

		# **MSL beside the SPIR-V, on every platform rather than on Apple.** The
		# translation is a property of the module and not of the machine doing
		# it, so translating only where the result is loaded would mean the one
		# platform that cannot run `just check` is the only one that ever
		# produces the file. Emitting it everywhere is what lets a Linux build
		# fail on a shader Metal could not express - which is the half of
		# `docs/DEFERRED.md` D00001 that does not need a Mac.
		#
		# The cost is one small process per shader; `just shader-check` reads
		# every one of these back, so nothing here is produced and unread.
		set(msl "${stage}/${shader_name}.msl")
		add_custom_command(
			OUTPUT "${msl}"
			COMMAND shadercross "${spv}" -o "${msl}"
			DEPENDS "${spv}" shadercross
			COMMENT "MSL ${name}/${shader_name}"
			VERBATIM
		)
		list(APPEND outputs "${msl}")
	endforeach()

	# A named handle for `cmake --build --target engine_resources_shaders`. A
	# program does not depend on it: it names the compiled files below instead,
	# which is what lets Ninja decide the staging is already done.
	add_custom_target(${target}_shaders DEPENDS ${outputs})
	set_property(TARGET ${target} PROPERTY MONO_SHADER_DIR "${stage}")
	set_property(TARGET ${target} PROPERTY MONO_SHADER_OUTPUTS "${outputs}")

	# **A module's staging directory is reconciled here, not merely written
	# to.** A `.spv` is an `add_custom_command` output and CMake deletes an
	# output only when the command that made it is re-run - so a shader that is
	# renamed, deleted, or moved to another module leaves its compiled form on
	# disk forever. `just shader-check` then reports on files whose source no
	# longer exists, which it did: 35 orphans from a v0.15 move sat beside 14
	# real ones and inflated the count to 49.
	#
	# Configure time is the right time because `CONFIGURE_DEPENDS` on the glob
	# above already re-runs the configure the moment the source set changes.
	set(expected "")
	foreach(output IN LISTS outputs)
		get_filename_component(output_name "${output}" NAME)
		list(APPEND expected "${output_name}")
	endforeach()
	_mono_prune_directory("${stage}" "${expected}")

	set_property(GLOBAL APPEND PROPERTY MONO_SHADER_MODULES "${name}")
endfunction()

# Everything in `directory` that is not named in `keep`, deleted.
#
# Not recursive and deliberately so: a shader stage is flat, and a sweep that
# descended would be one that could reach a directory somebody put there on
# purpose.
function(_mono_prune_directory directory keep)
	if(NOT IS_DIRECTORY "${directory}")
		return()
	endif()
	file(GLOB present RELATIVE "${directory}" "${directory}/*")
	foreach(entry IN LISTS present)
		if(NOT entry IN_LIST keep)
			file(REMOVE_RECURSE "${directory}/${entry}")
			message(STATUS "shaders: removed stale ${directory}/${entry}")
		endif()
	endforeach()
endfunction()

# The shader stage against the modules that still own shaders.
#
# **The other half of the reconciliation, and it is the half `_mono_add_shaders`
# structurally cannot do.** That function returns early for a module with no
# `shaders/` directory, so a module that *stops* owning shaders is a module
# nothing looks at again - which is exactly what happened when the built-in GLSL
# moved from `render` to `resources` and `shaderstage/render/` was left behind
# whole. Call this after every module has been declared.
function(mono_prune_shader_stage)
	if(NOT MONO_SHADER_STAGE)
		return()
	endif()
	get_property(owners GLOBAL PROPERTY MONO_SHADER_MODULES)
	_mono_prune_directory("${MONO_SHADER_STAGE}" "${owners}")
endfunction()

# ---------------------------------------------------------------------------
# Platform sources
# ---------------------------------------------------------------------------

# Anything under src/platform/<os>/ compiles only on that <os>. The alternative
# - one file per platform, each wrapped in an #if that empties it elsewhere -
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
# iOS - process spawning, for one - is one file rather than four identical ones
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
# Paying for a header once instead of once per file
# ---------------------------------------------------------------------------

# The standard headers a precompiled header is worth building out of.
#
# The sixteen most-included ones across the first-party tree, counted rather
# than guessed - `<vector>` appears in 608 files, `<string>` in 441. Standard
# headers only, and that is the rule rather than an accident: a PCH holding an
# engine header would have to be rebuilt whenever that header changed, which is
# the case a developer waits through, and it could only be given to targets that
# link the module it came from. Adding `core/Log.hpp`, `core/Name.hpp` and the
# two hot value types to this list was measured at 14.7% against 14.0% for the
# list as it stands - a fifth of a percent for a coupling.
set(MONO_PCH_HEADERS
	"<algorithm>" "<array>" "<cmath>" "<cstddef>" "<cstdint>" "<cstring>"
	"<filesystem>" "<functional>" "<memory>" "<optional>" "<span>" "<string>"
	"<string_view>" "<unordered_map>" "<utility>" "<vector>")

# Whichever of the two header-amortising strategies this preset asked for.
#
# **They are two answers to one question and this repository picks one.** Both
# exist to stop the same header being parsed once per source file: a unity build
# concatenates eight sources so the headers are read once for the batch, and a
# precompiled header serialises the parse of the standard library so every
# source reads it back instead of re-parsing it. Applying both means paying to
# build a PCH per target that a unity build has already made almost redundant,
# so unity wins where a preset asks for it.
#
# Measured on this machine with the real compile flags, ccache bypassed, best of
# two runs each way, three modules of different shapes:
#
#   | module         | TUs | plain     | with PCH  |      |
#   |----------------|-----|-----------|-----------|------|
#   | `engine/net`   |  19 |  27.1 CPU |  22.7 CPU | -16% |
#   | `engine/scene` |  31 |  28.7 CPU |  24.7 CPU | -14% |
#   | `studio`       |  59 | 180.7 CPU | 164.6 CPU |  -9% |
#
# **10.4% weighted, which is why this ranks below the unity build rather than
# above it.** That is the opposite of the usual intuition about precompiled
# headers and it is the reason the number was taken rather than assumed;
# `docs/ARCH_REVIEW.md` §E2 measured 11% independently, and the unity build
# measured 59% over the whole library set.
#
# **`unity_allowed` is FALSE for every test and benchmark binary, and that is a
# property of how a suite is declared rather than a shortcut.** `TEST_SUITE_ID`
# puts two fixed-name objects at file scope - `MonoTestSuiteId` and
# `MonoTestSuiteDeclaration` - and `TEST_DEPENDS` reads the first of them by
# that name. One file, one suite, one pair of objects. Concatenating eight test
# files puts eight of each in one translation unit, which is 509 redefinitions
# across the tree and not a thing renaming can fix: the contract the runner
# reads is that a suite *is* a file. Those binaries take the precompiled header
# instead. `release` builds no tests at all, so the preset this matters to is
# `ci`.
#
# One place for all three kinds, so that libraries, test binaries and benchmark
# binaries cannot end up on settings that disagree.
function(_mono_batch_headers target sources unity_allowed)
	if(MONO_UNITY_BUILD AND unity_allowed)
		set_target_properties(${target} PROPERTIES
			UNITY_BUILD ON
			UNITY_BUILD_MODE BATCH
			UNITY_BUILD_BATCH_SIZE ${MONO_UNITY_BATCH})
		_mono_keep_spdlog_sources_apart("${sources}")
	elseif(MONO_PCH)
		target_precompile_headers(${target} PRIVATE ${MONO_PCH_HEADERS})
	endif()
endfunction()

# A source that includes spdlog directly is left out of the concatenation.
#
# **Two files in this repository complete `spdlog::logger` themselves**, because
# `core/Log.hpp` only forward-declares it - `mono.studio/src/Editor.cpp`, which
# installs a sink, and `core/tests/Log.cpp`. Doing that pulls in fmt's full
# `format.h`, which *partially specialises* `formatter<std::basic_string<...>>`.
# Any neighbour compiled before it in the same batch that formatted a
# `std::string` has already instantiated that template, and the specialisation
# then arrives too late:
#
#   spdlog/fmt/bundled/format.h:3943: error: partial specialization of
#   'formatter<basic_string<_CharT, _Traits, _Allocator>, Char>' after
#   instantiation of 'formatter<basic_string<char>, char, void>'
#
# **Detected rather than listed, because of what that message is like.** It
# names a line in a vendored header and no file of ours, and it appears only in
# the two presets that concatenate - so a developer who adds a third sink would
# meet it for the first time in CI, with nothing in the text pointing at what
# they wrote. A grep for the include costs a configure about a fifth of a second
# and cannot go stale.
function(_mono_keep_spdlog_sources_apart sources)
	foreach(source IN LISTS sources)
		file(STRINGS "${source}" hit REGEX "^#include <spdlog/" LIMIT_COUNT 1)
		if(hit)
			set_source_files_properties("${source}" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
		endif()
	endforeach()
endfunction()

# Bind profiler scopes to the product that compiled the call site. Engine
# modules need no definition because Profiling.hpp defaults them to Engine.
function(_mono_bind_profile_owner target name)
	if(name STREQUAL "client")
		target_compile_definitions(${target} PRIVATE
			ENGINE_PROFILE_OWNER=::engine::core::ProfileOwner::Client)
	elseif(name STREQUAL "server")
		target_compile_definitions(${target} PRIVATE
			ENGINE_PROFILE_OWNER=::engine::core::ProfileOwner::Server)
	elseif(name STREQUAL "studio")
		target_compile_definitions(${target} PRIVATE
			ENGINE_PROFILE_OWNER=::engine::core::ProfileOwner::Studio)
	endif()
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
		"DEPS;VENDOR;VENDOR_PUBLIC;VENDOR_INCLUDES;DEFINES;SHADER_DEFINES;ALLOW_TIER_ESCAPE;NO_UNITY"
		${ARGN})

	if(NOT ARG_TIER)
		message(FATAL_ERROR "mono_add_library(${name}) needs a TIER.")
	endif()
	list(FIND MONO_KNOWN_TIERS "${ARG_TIER}" known)
	if(known LESS 0)
		message(FATAL_ERROR "mono_add_library(${name}): unknown TIER '${ARG_TIER}'.")
	endif()

	# Engine:: for mono.engine modules. A program's own library - mono.client,
	# mono.server - passes NAMESPACE Mono, because it is a product's library
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
	# configure by mono_check_all_tiers - see the comment on that function.
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

		# **The module's own name, so that 711 log call sites gained a category
		# without one of them being edited to say so.** `ENGINE_LOG_CATEGORY` is
		# what `ENGINE_INFO` and its siblings pass to the macro, and one
		# definition here is the whole of "one category per module by
		# convention" - a convention the build applies rather than one somebody
		# remembers. A file logging on behalf of another area passes a category
		# explicitly to `ENGINE_LOG`.
		#
		# PRIVATE, because a category is a property of the translation unit that
		# writes the line and not of anything it links: a header of this
		# module's, logging from inside another module's source, is that other
		# module's line to explain.
		target_compile_definitions(${target} PRIVATE ENGINE_LOG_CATEGORY="${name}")
		_mono_bind_profile_owner(${target} ${name})

		target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
		if(MONO_COMPILE_DEFINITIONS)
			target_compile_definitions(${target} PRIVATE ${MONO_COMPILE_DEFINITIONS})
		endif()

		# **`NO_UNITY` names a source that has to stay a translation unit of its
		# own, and every use of it is a defect somewhere else.** A static archive
		# is searched a member at a time, so an object nothing wants is an object
		# whose unresolved references are never read - and concatenating eight
		# sources into one member makes the whole batch arrive as soon as any one
		# of them is wanted. A module that has been getting away with an
		# undeclared dependency stops getting away with it, in `release` and `ci`
		# only, which is the worst place to find out.
		#
		# So each entry here is a note that says which edge is wrong. The
		# alternative - putting the missing library on every consumer's link line
		# - makes the accident permanent and spreads it.
		foreach(source IN LISTS ARG_NO_UNITY)
			set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/${source}"
				PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
		endforeach()

		_mono_batch_headers(${target} "${sources}" TRUE)
	else()
		target_compile_features(${target} INTERFACE cxx_std_20)
		target_include_directories(${target} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
		target_link_libraries(${target} INTERFACE ${ARG_DEPS} ${ARG_VENDOR})
		if(ARG_DEFINES)
			target_compile_definitions(${target} INTERFACE ${ARG_DEFINES})
		endif()
	endif()

	_mono_add_shaders(${name} ${target} "${ARG_SHADER_DEFINES}")

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
# command line to Catch2 - that listing is what mono.tools/testrunner reads.
function(mono_add_tests name)
	cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})

	file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
	if(NOT sources)
		return()
	endif()

	set(target test_${name})
	add_executable(${target} ${sources})
	target_link_libraries(${target} PRIVATE ${ARG_DEPS} Engine::testmain Catch2::Catch2)
	target_compile_definitions(${target} PRIVATE ENGINE_LOG_CATEGORY="${name}")
	_mono_bind_profile_owner(${target} ${name})
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
	if(MONO_COMPILE_DEFINITIONS)
		target_compile_definitions(${target} PRIVATE ${MONO_COMPILE_DEFINITIONS})
	endif()
	_mono_batch_headers(${target} "${sources}" FALSE)

	# A module's own tests may reach its src/ directory, and only its own tests
	# may. AGENTS.md states the rule this implements: do not widen a public
	# header to make a test easier - link the private one instead. Without this
	# line the only way to test a private type is to publish it, which is the
	# outcome the rule exists to prevent.
	if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src")
		target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
	endif()
	set_target_properties(${target} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tests")

	# The vendored typefaces, for a test of something that reads them.
	#
	# **Same rule as a program's, applied to the one place it was not.** A
	# program links `ui` or `render` and gets fonts staged beside it; a *test*
	# of `render::GlyphAtlas` linked the same module and got nothing, so every
	# case that needed a real .ttf took its "no staged fonts" branch and passed
	# without asserting anything. A test that skips silently is worse than one
	# that fails: it reports green for a rasteriser nobody ran.
	get_target_property(_mono_test_deps ${target} LINK_LIBRARIES)
	if("Engine::render" IN_LIST _mono_test_deps OR "Engine::ui" IN_LIST _mono_test_deps)
		file(GLOB _mono_test_fonts "${CMAKE_SOURCE_DIR}/mono.vendor/fonts/*.ttf")

		# The directory first, for the reason the program-side copy gives:
		# `copy_if_different` given a destination that does not exist writes a
		# *file* under that name.
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${MONO_STAGE_ROOT}/tests/fonts"
			VERBATIM)

		foreach(_font IN LISTS _mono_test_fonts)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"${_font}" "${MONO_STAGE_ROOT}/tests/fonts/"
				VERBATIM)
		endforeach()
	endif()

	# A renderer test that opts into a real device reads the same compiled
	# resources as a client program. Keep SDL and the shaders out of unrelated
	# suites, but make `test_render` runnable from its staged directory without a
	# full client build happening to have populated another tree first.
	if("Engine::render" IN_LIST _mono_test_deps)
		get_target_property(_mono_test_shader_dir engine_resources MONO_SHADER_DIR)
		get_target_property(_mono_test_shader_inputs engine_resources MONO_SHADER_OUTPUTS)
		set(_mono_test_shader_outputs "")
		foreach(_mono_test_shader IN LISTS _mono_test_shader_inputs)
			get_filename_component(_mono_test_shader_name "${_mono_test_shader}" NAME)
			list(APPEND _mono_test_shader_outputs
				"${MONO_STAGE_ROOT}/tests/shaders/resources/${_mono_test_shader_name}")
		endforeach()

		add_custom_command(
			OUTPUT ${_mono_test_shader_outputs}
			COMMAND ${CMAKE_COMMAND} -E rm -rf "${MONO_STAGE_ROOT}/tests/shaders/resources"
			COMMAND ${CMAKE_COMMAND} -E copy_directory
				"${_mono_test_shader_dir}" "${MONO_STAGE_ROOT}/tests/shaders/resources"
			DEPENDS ${_mono_test_shader_inputs}
			COMMENT "Staging renderer shaders into tests/shaders"
			VERBATIM)
		add_custom_target(${target}_stage_shaders DEPENDS ${_mono_test_shader_outputs})
		add_dependencies(${target} ${target}_stage_shaders)
	endif()

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
# and `just bench` never runs a test - the two answer different questions and a
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
	target_compile_definitions(${target} PRIVATE ENGINE_LOG_CATEGORY="${name}")
	_mono_bind_profile_owner(${target} ${name})
	_mono_batch_headers(${target} "${sources}" FALSE)

	# Optimised whatever the preset says, because a debug build measures the
	# debug build. A benchmark run against unoptimised code reports a number
	# that has no relationship to the one anybody ships, and the danger is not
	# that it is slower - it is that the *ratios* between two implementations
	# invert.
	#
	# **-O3, matching `release`, and the two must not drift.** This line said
	# -O2 while `release` inherited -O2 from RelWithDebInfo, so they agreed by
	# coincidence rather than by construction - and when `release` moved to -O3
	# this became the one place that would have gone on reporting the old
	# number. A benchmark compiled differently from the thing it is a benchmark
	# of measures a binary nobody ships, which is the same failure as measuring
	# a debug build and is harder to notice. If `release` changes level again,
	# change it here in the same commit.
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
	if(MONO_COMPILE_DEFINITIONS)
		target_compile_definitions(${target} PRIVATE ${MONO_COMPILE_DEFINITIONS})
	endif()
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
# A program is a thin main over libraries. It stages into its own directory -
# binary, shared libraries and the shaders of every module it links, and nothing
# else. A server/ directory that has grown a shaders/ folder is a link-line
# mistake anyone can see.
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
	# Windows reads an executable's icon from its resource table. Keep the
	# resource beside each program's generated build files so one source icon
	# remains the asset for every shipped application.
	if(WIN32)
		set(MONO_PROGRAM_ICON "${CMAKE_SOURCE_DIR}/assets/small-icon.ico")
		set(MONO_PROGRAM_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/${target}.rc")
		configure_file(
			"${MONO_LIBRARY_DIR}/ProgramIcon.rc.in"
			"${MONO_PROGRAM_RESOURCE}"
			@ONLY)
		target_sources(${target} PRIVATE "${MONO_PROGRAM_RESOURCE}")
	endif()
	target_compile_features(${target} PRIVATE cxx_std_20)
	target_link_libraries(${target} PRIVATE ${ARG_DEPS} ${ARG_VENDOR})
	target_compile_definitions(${target} PRIVATE ENGINE_LOG_CATEGORY="${name}")
	_mono_bind_profile_owner(${target} ${name})
	target_compile_options(${target} PRIVATE ${MONO_COMPILE_OPTIONS})
	if(MONO_COMPILE_DEFINITIONS)
		target_compile_definitions(${target} PRIVATE ${MONO_COMPILE_DEFINITIONS})
	endif()

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
	set(shader_commands "")
	set(shader_modules "")
	set(shader_inputs "")
	set(shader_staged "")
	foreach(dep IN LISTS all_deps)
		get_target_property(dir ${dep} MONO_SHADER_DIR)
		if(NOT dir OR dir STREQUAL "dir-NOTFOUND")
			continue()
		endif()
		get_target_property(module ${dep} MONO_MODULE_NAME)
		get_target_property(compiled ${dep} MONO_SHADER_OUTPUTS)
		list(APPEND shader_modules "${module}")
		list(APPEND shader_inputs ${compiled})
		# Every file this copy will produce, named. See the custom command
		# below for why they are named rather than left implicit.
		foreach(file IN LISTS compiled)
			get_filename_component(file_name "${file}" NAME)
			list(APPEND shader_staged "${stage}/shaders/${module}/${file_name}")
		endforeach()
		# `rm -rf` before the copy, so the staged directory is the module's
		# directory rather than the union of every one it has ever been. A copy
		# alone leaves a renamed shader staged beside its replacement, and the
		# renderer opens files by name - so the stale one loads and nothing says
		# which of the two it got. The whole directory is a handful of kilobytes,
		# so it costs nothing to be exact on the builds that do run it.
		list(APPEND shader_commands
			COMMAND ${CMAKE_COMMAND} -E rm -rf "${stage}/shaders/${module}"
			COMMAND ${CMAKE_COMMAND} -E copy_directory "${dir}" "${stage}/shaders/${module}")
	endforeach()

	# A module this program no longer links leaves a whole directory behind, and
	# the per-module sweep above cannot see it - there is no command for a module
	# that is not on the link line. Configure time, for `_mono_add_shaders`'
	# reason: the link row is known here and changes to it re-run the configure.
	_mono_prune_directory("${stage}/shaders" "${shader_modules}")

	if(shader_commands)
		# **Every staged file is a declared output, and that is what makes a
		# null build null.** This was an `add_custom_target` carrying the
		# commands directly, which Ninja has no choice but to run every time: a
		# target with no output is a target that can never be up to date. It was
		# most of the 0.09 s null build, so it cost nothing measurable - but a
		# build that always has something to do hides the one that does not, and
		# the next thing added beside it inherits the same blindness.
		#
		# Naming the copies rather than a stamp file is the part that matters
		# for correctness. `just shader-check` and `just check-server-is-headless`
		# both read the staged tree, and a stamp says only "the copy ran once":
		# delete one `.spv` from the stage and a stamp-guarded build hands the
		# missing file straight back. With the files themselves as outputs,
		# Ninja restores anything removed and re-copies whenever a compiled
		# shader is newer, which is the same guarantee the old always-run target
		# gave for a fraction of the work.
		#
		# A target of its own rather than a POST_BUILD command, so that a
		# shader-only edit still stages without the program relinking.
		add_custom_command(
			OUTPUT ${shader_staged}
			${shader_commands}
			DEPENDS ${shader_inputs}
			COMMENT "Staging shaders into ${stage}/shaders"
			VERBATIM)
		add_custom_target(${name}_stage_shaders ALL DEPENDS ${shader_staged})

		# The program depends on the staging, not the other way round.
		#
		# Reversed, `ALL` still stages it on a full build - and `cmake --build
		# --target client` does not, because nothing it was asked for depends on
		# it. That is exactly what `just run` asks for, so a fresh preset built
		# the client, skipped its shaders, and failed at startup with four
		# "shader not found" lines and no clue that a whole target had been
		# missed. A full build had always happened to run first.
		add_dependencies(${target} ${name}_stage_shaders)
	endif()

	# The vendored typefaces, for a program that draws an interface.
	#
	# Same rule as SDL below: staged because the program links the thing that
	# reads them, not because somebody remembered. A program with no `ui` on its
	# link line has nothing that opens a .ttf, and staging three megabytes of
	# fonts into a dedicated server would be three megabytes of a claim that it
	# draws.
	# **Either module reads them now, and the gate says so rather than naming
	# one.** `engine_ui` opens the .ttf files through imgui's atlas; since v0.8
	# `render::GlyphAtlas` opens the same four directly, so that a shipped
	# client can draw a `ScreenGui` without linking the editor's toolkit. A gate
	# that still named only `ui` staged nothing for a program that draws text
	# through the other one - and the symptom is a client whose interface has no
	# glyphs in it.
	list(FIND all_deps engine_ui links_ui)
	list(FIND all_deps engine_render links_render)
	if(links_ui GREATER_EQUAL 0 OR links_render GREATER_EQUAL 0)
		file(GLOB _mono_fonts "${CMAKE_SOURCE_DIR}/mono.vendor/fonts/*.ttf")

		# The directory first. `copy_if_different` given a destination that does
		# not exist writes a *file* under that name, so without this the staged
		# tree grows a file called `fonts` holding one typeface and the next copy
		# overwrites it - which fails as "not a directory" a long way from here.
		add_custom_command(TARGET ${target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E make_directory "${stage}/fonts"
			VERBATIM)

		foreach(_font IN LISTS _mono_fonts)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_font}" "${stage}/fonts/"
				VERBATIM)
		endforeach()
	endif()

	# SDL3 is a shared library; it has to sit beside the binary that loads it.
	if(TARGET SDL3-shared)
		list(FIND all_deps SDL3-shared links_sdl)
		if(links_sdl GREATER_EQUAL 0)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:SDL3-shared>" "${stage}"
				VERBATIM)

			# The binary asks the loader for the soname - libSDL3.so.0 - and
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

	# A Vulkan presentation program on Apple needs the portability driver beside
	# its executable. The renderer links the imported runtime only to declare
	# that packaging edge; SDL opens it dynamically through its Vulkan loader.
	if(APPLE AND TARGET MoltenVK::Runtime)
		list(FIND all_deps MoltenVK::Runtime links_moltenvk)
		if(links_moltenvk GREATER_EQUAL 0)
			add_custom_command(TARGET ${target} POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different
					"$<TARGET_FILE:MoltenVK::Runtime>" "${stage}"
				VERBATIM)
		endif()
	endif()
endfunction()

# Walks MONO_DEPS breadth-first. Vendor targets have no such property, so the
# walk stops at them, which is what we want - we only care about our own graph
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
# dependencies lazily, which means a forward reference - `ecs` listing
# `Mono::server`, declared two add_subdirectory calls later - passes it in
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
					"  A misspelled dependency links fine - CMake would resolve it as a "
					"bare library name at link time - so it is refused here instead."
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
