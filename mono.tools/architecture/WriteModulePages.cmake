# The module index for the API reference, generated from the graph and the tree.
#
# Run as a script - `cmake -DEXPECTED=... -DROOT=... -DOUT=... -P` - so it needs
# nothing built, exactly as `CheckTargetGraph.cmake` does and for the same
# reason: its input is a checked-in JSON document and CMake already has a reader.
#
# **The page it writes was hand-maintained and listed ten of the engine's
# twenty-nine modules.** That is the failure a hand-maintained list of a
# generated fact always has: adding a module means remembering a file nobody
# opens, and nothing objects when you do not. `docs/ARCH_REVIEW.md` §B found it,
# and it is the same data `just test-architecture` already checks - so the page
# is now a walk of the tree ordered by the graph.
#
# **The tree decides what is listed and the graph decides the order.** A glob
# finds every `AGENTS.md`, so a module that exists is listed whether or not it is
# in the expectation; the layers put them bottom upward, which is the order the
# dependency rule reads in. A directory the graph does not know goes last rather
# than being dropped, because a page missing an entry is the bug being fixed.
#
# `-DCHECK=YES` compares instead of writing, which is what `just docs-check-pages`
# runs. Rule 6: a rule the build does not check is documentation, and "this page
# lists every module" is a rule.

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED EXPECTED OR NOT DEFINED ROOT OR NOT DEFINED OUT)
	message(FATAL_ERROR "Usage: cmake -DEXPECTED=<expected_graph.json> -DROOT=<repo> -DOUT=<page.md> [-DCHECK=YES] -P ...")
endif()

if(NOT EXISTS "${EXPECTED}")
	message(FATAL_ERROR "${EXPECTED} not found.")
endif()

file(READ "${EXPECTED}" MONO_EXPECTED)

# The id Doxygen derives from a markdown file's path.
#
# **Order matters and is not obvious.** The underscore doubling has to happen
# before the dot becomes `_8`, or the `_8` it just wrote gets doubled into
# `__8`. `THIRD_PARTY_NOTICES.md` and `mono.unified_tests/AGENTS.md` are the two
# entries that prove both halves.
#
# These ids are stable for a Doxygen version and are not promised across one.
# An upgrade that renamed them shows up as an unresolved `@subpage`, which is a
# warning, and `just docs-check` fails on warnings.
function(_page_id relative out)
	string(REGEX REPLACE "\\.md$" "" id "${relative}")
	string(REPLACE "_" "__" id "${id}")
	string(REPLACE "." "_8" id "${id}")
	string(REPLACE "/" "_2" id "${id}")
	set(${out} "md_${id}" PARENT_SCOPE)
endfunction()

# JSON has no member iteration, so a member count plus an index walk reads a set
# of keys. The same function `CheckTargetGraph.cmake` needs, for the same reason.
function(_graph_keys section out)
	set(keys "")
	string(JSON count ERROR_VARIABLE error LENGTH "${MONO_EXPECTED}" ${section})
	if(error OR count STREQUAL "")
		set(${out} "" PARENT_SCOPE)
		return()
	endif()

	math(EXPR last "${count} - 1")
	foreach(index RANGE 0 ${last})
		string(JSON key MEMBER "${MONO_EXPECTED}" ${section} ${index})
		if(NOT key MATCHES "^_")
			list(APPEND keys "${key}")
		endif()
	endforeach()
	set(${out} "${keys}" PARENT_SCOPE)
endfunction()

# Where a graph row's sources are. A module is under `mono.engine`, a program or
# a member is `mono.<name>`, and a tool is under `mono.tools`.
#
# **The glob at the end is not a fourth guess, it is the general case.** A module
# can live under the one program that owns it - `mono.studio/nodegraph` is a node
# editor nothing else links - and hard-coding a prefix per owner is a list that
# goes stale the next time one moves.
function(_directory_of name out)
	foreach(candidate "mono.engine/${name}" "mono.${name}" "mono.tools/${name}")
		if(IS_DIRECTORY "${ROOT}/${candidate}")
			set(${out} "${candidate}" PARENT_SCOPE)
			return()
		endif()
	endforeach()

	file(GLOB found LIST_DIRECTORIES true "${ROOT}/mono.*/${name}")
	foreach(candidate IN LISTS found)
		file(RELATIVE_PATH relative "${ROOT}" "${candidate}")
		if(IS_DIRECTORY "${candidate}" AND NOT relative MATCHES "^mono\\.vendor/")
			set(${out} "${relative}" PARENT_SCOPE)
			return()
		endif()
	endforeach()

	set(${out} "" PARENT_SCOPE)
endfunction()

# Every directory holding an `AGENTS.md`, relative to the repository root.
set(MONO_PRESENT "")
file(GLOB_RECURSE agents_files "${ROOT}/mono.*/AGENTS.md")
foreach(file IN LISTS agents_files)
	file(RELATIVE_PATH relative "${ROOT}" "${file}")
	# Their code, their conventions. `mono.vendor` carries submodules whose
	# markdown is not this repository's documentation.
	if(relative MATCHES "^mono\\.vendor/" OR relative MATCHES "/node_modules/")
		continue()
	endif()
	get_filename_component(directory "${relative}" DIRECTORY)
	list(APPEND MONO_PRESENT "${directory}")
endforeach()
list(SORT MONO_PRESENT)
list(REMOVE_DUPLICATES MONO_PRESENT)

# The order: the engine's own table, then the layers bottom upward, then the
# programs, then the tooling, then whatever the graph did not name.
set(MONO_ORDER "")

macro(_take directory)
	list(FIND MONO_PRESENT "${directory}" _found)
	if(_found GREATER_EQUAL 0)
		list(APPEND MONO_ORDER "${directory}")
		list(REMOVE_AT MONO_PRESENT ${_found})
	endif()
endmacro()

_take("mono.engine")

_graph_keys(modules module_names)
_graph_keys(programs program_names)

# Layer by layer rather than sorting pairs, because CMake has no sort key and a
# stack this shallow makes the loop the simpler of the two.
set(MONO_LAYERED "")
foreach(name IN LISTS module_names)
	string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${name} layer)
	if(NOT error AND NOT layer STREQUAL "")
		list(APPEND MONO_LAYERED "${layer}")
	endif()
endforeach()
if(MONO_LAYERED)
	list(SORT MONO_LAYERED COMPARE NATURAL)
	list(REMOVE_DUPLICATES MONO_LAYERED)
endif()

foreach(height IN LISTS MONO_LAYERED)
	set(at_height "")
	foreach(name IN LISTS module_names)
		string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${name} layer)
		if(NOT error AND layer STREQUAL "${height}")
			list(APPEND at_height "${name}")
		endif()
	endforeach()
	list(SORT at_height)

	foreach(name IN LISTS at_height)
		_directory_of("${name}" directory)
		if(NOT directory STREQUAL "")
			_take("${directory}")
		endif()
	endforeach()
endforeach()

# The program band, executables first and then the tooling, each alphabetically.
set(MONO_BAND "")
foreach(name IN LISTS module_names program_names)
	list(FIND MONO_BAND "${name}" seen)
	if(seen GREATER_EQUAL 0)
		continue()
	endif()
	string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${name} layer)
	if(NOT error AND NOT layer STREQUAL "")
		continue()
	endif()
	list(APPEND MONO_BAND "${name}")
endforeach()
list(SORT MONO_BAND)

foreach(name IN LISTS MONO_BAND)
	_directory_of("${name}" directory)
	if(directory MATCHES "^mono\\.tools/")
		continue()
	endif()
	if(NOT directory STREQUAL "")
		_take("${directory}")
	endif()
endforeach()

_take("mono.tools")
foreach(name IN LISTS MONO_BAND)
	_directory_of("${name}" directory)
	if(directory MATCHES "^mono\\.tools/")
		_take("${directory}")
	endif()
endforeach()

# Anything with an `AGENTS.md` the graph never mentioned. Last rather than
# dropped: a page missing an entry is the bug this file exists to fix.
foreach(directory IN LISTS MONO_PRESENT)
	list(APPEND MONO_ORDER "${directory}")
endforeach()

set(MONO_PAGE
"# Module invariants

One page per module, each holding the rules that catch real mistakes in that
module rather than the ones that apply everywhere. **Read the one for a module
before changing anything in it** - [AGENTS.md](@ref md_AGENTS) says why, and the
layer stack it describes is what decides which of these a module may read.

The engine bottom to top, then the programs, then the tooling. A module carrying
prose of its own beyond its invariants has it listed underneath.

**Generated.** `mono.tools/architecture/WriteModulePages.cmake` walks the tree
for every `AGENTS.md` and orders them by the layers in `expected_graph.json`, so
a module that exists is listed. Run `just docs-pages` after adding one;
`just docs-pages-check` is what fails when it has not been run.
")

foreach(directory IN LISTS MONO_ORDER)
	_page_id("${directory}/AGENTS.md" id)
	string(APPEND MONO_PAGE "\n- @subpage ${id}")

	# The prose a module keeps beside its invariants. `docs/` only: a markdown
	# file anywhere else under a module is data for a tool or a fixture's
	# README, and neither is a page about the engine.
	file(GLOB prose "${ROOT}/${directory}/docs/*.md")
	list(SORT prose)
	foreach(file IN LISTS prose)
		# **An empty file makes no page, so linking to one is a dangling
		# reference.** Four modules keep a one-byte `docs/index.md` as a
		# placeholder for prose nobody has written; Doxygen skips them, and
		# `docs-check` fails on the unresolved `@subpage` that results. Writing
		# something into one of those files is what puts it on the page.
		file(READ "${file}" contents)
		string(STRIP "${contents}" contents)
		if(contents STREQUAL "")
			continue()
		endif()

		file(RELATIVE_PATH relative "${ROOT}" "${file}")
		_page_id("${relative}" id)
		string(APPEND MONO_PAGE "\n- @subpage ${id}")
	endforeach()
endforeach()

string(APPEND MONO_PAGE "\n")

if(CHECK)
	if(NOT EXISTS "${OUT}")
		message(FATAL_ERROR "${OUT} does not exist. Run `just docs-pages`.")
	endif()

	file(READ "${OUT}" current)
	if(NOT current STREQUAL MONO_PAGE)
		message(FATAL_ERROR
			"${OUT} is stale.\n"
			"  It is generated from mono.tools/architecture/expected_graph.json and the "
			"AGENTS.md files in the tree. Run `just docs-pages` and commit the result.")
	endif()

	list(LENGTH MONO_ORDER entries)
	message(STATUS "module pages ok - ${entries} module page(s)")
	return()
endif()

file(WRITE "${OUT}" "${MONO_PAGE}")
list(LENGTH MONO_ORDER entries)
message(STATUS "wrote ${OUT} - ${entries} module page(s)")
