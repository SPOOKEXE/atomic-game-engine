# The architecture test: the real CMake target graph against the expectation.
#
# Run as a script - `cmake -DGRAPH=... -DEXPECTED=... -P CheckTargetGraph.cmake` -
# so it needs nothing built. CMake rather than a compiled tool because the input
# is CMake's own output, and CMake already has a JSON reader.
#
# The tier rule itself is enforced at configure time by mono_check_all_tiers,
# which fails the build with the offending edge named. This exists for the other
# half of the job: making an architectural change show up as a diff to a
# checked-in file rather than only in a build log.
#
# It also carries the layer check. `AGENTS.md` rule 1 says a layer may see every
# layer below it and none above it, and until v0.19 nothing checked that -
# `docs/CODE_QUALITY.md` asked a reviewer to "check the layer heights by hand",
# which rule 6 calls the third category of rule that exists only in somebody's
# memory. Each module in the expectation now carries a `layer`, and an edge that
# does not run downward fails here by name.
#
# The layer check reads the expectation rather than the emitted graph, and that
# is sufficient rather than lax: `links` is already compared against the build
# exactly, so an expectation whose edges all run downward describes a build whose
# edges all run downward. A module with no `layer` is the program band - the
# thing that links modules and that no module links - and it is checked from the
# other side instead: nothing that has a layer may link something that has none.

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED GRAPH OR NOT DEFINED EXPECTED)
	message(FATAL_ERROR "Usage: cmake -DGRAPH=<target-graph.json> -DEXPECTED=<expected_graph.json> -P ...")
endif()

if(NOT EXISTS "${GRAPH}")
	message(FATAL_ERROR "${GRAPH} not found. Configure a preset first: cmake --preset dev")
endif()
if(NOT EXISTS "${EXPECTED}")
	message(FATAL_ERROR "${EXPECTED} not found.")
endif()

file(READ "${GRAPH}" MONO_GRAPH)
file(READ "${EXPECTED}" MONO_EXPECTED)

set(MONO_FAILURES "")

macro(_fail message)
	list(APPEND MONO_FAILURES "${message}")
endmacro()

# JSON has no member iteration, so the member count plus an index walk is how
# a set of keys is read.
function(_json_keys json path out)
	set(keys "")
	string(JSON count ERROR_VARIABLE error LENGTH "${json}" ${path})
	if(error OR count STREQUAL "")
		set(${out} "" PARENT_SCOPE)
		return()
	endif()

	math(EXPR last "${count} - 1")
	foreach(index RANGE 0 ${last})
		string(JSON key MEMBER "${json}" ${path} ${index})
		# `_comment` carries the prose. Anything starting with an underscore is
		# documentation rather than an entry.
		if(NOT key MATCHES "^_")
			list(APPEND keys "${key}")
		endif()
	endforeach()
	set(${out} "${keys}" PARENT_SCOPE)
endfunction()

function(_json_links json section key out)
	set(links "")
	string(JSON count ERROR_VARIABLE error LENGTH "${json}" ${section} ${key} links)
	if(NOT error AND NOT count STREQUAL "" AND count GREATER 0)
		math(EXPR last "${count} - 1")
		foreach(index RANGE 0 ${last})
			string(JSON link GET "${json}" ${section} ${key} links ${index})
			list(APPEND links "${link}")
		endforeach()
	endif()
	list(SORT links)
	set(${out} "${links}" PARENT_SCOPE)
endfunction()

# `lateral` is an optional array, so this is `_json_links` without the assumption
# that the member is there.
function(_json_array json path member out)
	set(values "")
	string(JSON count ERROR_VARIABLE error LENGTH "${json}" ${path} ${member})
	if(NOT error AND NOT count STREQUAL "" AND count GREATER 0)
		math(EXPR last "${count} - 1")
		foreach(index RANGE 0 ${last})
			string(JSON value GET "${json}" ${path} ${member} ${index})
			list(APPEND values "${value}")
		endforeach()
	endif()
	set(${out} "${values}" PARENT_SCOPE)
endfunction()

# An entry with no `requires` is unconditional. One with it is expected exactly
# when its option is on - so both "this was deleted" and "this is in a build
# that was configured without it" stay detectable.
function(_is_built key out)
	string(JSON option ERROR_VARIABLE error GET "${MONO_EXPECTED}" ${ARGV2} ${key} requires)
	if(error OR option STREQUAL "")
		set(${out} TRUE PARENT_SCOPE)
		return()
	endif()

	string(JSON value ERROR_VARIABLE optionError GET "${MONO_GRAPH}" options ${option})
	if(optionError OR NOT value)
		set(${out} FALSE PARENT_SCOPE)
	else()
		set(${out} TRUE PARENT_SCOPE)
	endif()
endfunction()

function(_compare_section label section)
	_json_keys("${MONO_GRAPH}" ${section} actual_keys)
	_json_keys("${MONO_EXPECTED}" ${section} expected_keys)

	foreach(key IN LISTS expected_keys)
		_is_built("${key}" built ${section})
		list(FIND actual_keys "${key}" present)

		if(NOT built)
			if(present GREATER_EQUAL 0)
				string(JSON option GET "${MONO_EXPECTED}" ${section} ${key} requires)
				_fail("${label} '${key}' is in the build, but ${option} is off. Something links it unconditionally.")
			endif()
			continue()
		endif()

		if(present LESS 0)
			_fail("${label} '${key}' is expected but the build does not declare it. Either it was removed, or its add_subdirectory is behind an option the expectation does not name in `requires`.")
			continue()
		endif()

		string(JSON actual_tier GET "${MONO_GRAPH}" ${section} ${key} tier)
		string(JSON expected_tier GET "${MONO_EXPECTED}" ${section} ${key} tier)
		if(NOT actual_tier STREQUAL expected_tier)
			_fail("${label} '${key}' is [${actual_tier}] and should be [${expected_tier}].")
		endif()

		_json_links("${MONO_GRAPH}" ${section} "${key}" actual_links)
		_json_links("${MONO_EXPECTED}" ${section} "${key}" expected_links)
		if(NOT "${actual_links}" STREQUAL "${expected_links}")
			_fail("${label} '${key}' links [${actual_links}] and should link [${expected_links}].")
		endif()
	endforeach()

	foreach(key IN LISTS actual_keys)
		list(FIND expected_keys "${key}" known)
		if(known LESS 0)
			_fail("${label} '${key}' exists in the build and not in the expectation. Add it - a new ${label} is an architectural change.")
		endif()
	endforeach()

	set(MONO_FAILURES "${MONO_FAILURES}" PARENT_SCOPE)
endfunction()

# The layer check.
#
# Three edges in this repository run sideways rather than downward, and each is
# deliberate and named in a comment beside itself: `bake` reads the pipeline
# description in `bakegraph`, `delivery` frames its fetches with `net`, and `ui`
# implements `render::FrameOverlayHook`. A sideways edge is allowed exactly when
# the module's `lateral` array names it, so adding one is a diff a reviewer sees
# rather than a silent widening of the rule.
function(_check_layers)
	_json_keys("${MONO_EXPECTED}" modules keys)

	foreach(key IN LISTS keys)
		string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${key} layer)
		if(error OR layer STREQUAL "")
			# The program band. Nothing with a layer may link it, which the loop
			# below reports from the other end.
			continue()
		endif()

		_json_array("${MONO_EXPECTED}" "modules;${key}" lateral allowed)
		_json_links("${MONO_EXPECTED}" modules "${key}" links)

		foreach(link IN LISTS links)
			string(JSON linkLayer ERROR_VARIABLE linkError GET "${MONO_EXPECTED}" modules ${link} layer)

			if(linkError OR linkLayer STREQUAL "")
				_fail("module '${key}' is [L${layer}] and links '${link}', which has no layer. A module may not link the program band.")
				continue()
			endif()

			if(linkLayer GREATER layer)
				_fail("module '${key}' is [L${layer}] and links '${link}' at [L${linkLayer}]. A layer may see every layer below it and none above it.")
			elseif(linkLayer EQUAL layer)
				list(FIND allowed "${link}" named)
				if(named LESS 0)
					_fail("module '${key}' is [L${layer}] and links '${link}' at the same layer. Siblings do not include each other. If the edge is deliberate, name it in this module's `lateral` array and say why beside the edge.")
				endif()
			endif()
		endforeach()
	endforeach()

	set(MONO_FAILURES "${MONO_FAILURES}" PARENT_SCOPE)
endfunction()

_compare_section("module" modules)
_compare_section("program" programs)
_check_layers()

if(MONO_FAILURES)
	message("architecture check failed:\n")
	foreach(failure IN LISTS MONO_FAILURES)
		message("  - ${failure}")
	endforeach()
	message(FATAL_ERROR
		"\nIf the change is intended, update the expectation in the same commit:\n"
		"  ${EXPECTED}")
endif()

_json_keys("${MONO_GRAPH}" modules module_keys)
_json_keys("${MONO_GRAPH}" programs program_keys)
list(LENGTH module_keys module_count)
list(LENGTH program_keys program_count)
set(layered 0)
foreach(key IN LISTS module_keys)
	string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${key} layer)
	if(NOT error AND NOT layer STREQUAL "")
		math(EXPR layered "${layered} + 1")
	endif()
endforeach()
message(STATUS "architecture ok - ${module_count} module(s), ${program_count} program(s), ${layered} layered")
