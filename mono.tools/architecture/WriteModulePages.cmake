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
# **The graph decides which modules are ordered.** Modules with an `AGENTS.md`
# or nonempty prose under `docs/*.md` are listed, with graph modules ordered by
# layer. Directories outside the graph that carry an `AGENTS.md` go last rather
# than being dropped.
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
# `__8`. `docs/THIRD_PARTY_NOTICES.md` and `mono.unified_tests/AGENTS.md` are the two
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

_graph_keys(modules module_names)
foreach(name IN LISTS module_names)
	_directory_of("${name}" directory)
	if(directory STREQUAL "")
		continue()
	endif()

	file(GLOB prose "${ROOT}/${directory}/docs/*.md")
	foreach(file IN LISTS prose)
		file(READ "${file}" contents)
		string(STRIP "${contents}" contents)
		if(NOT contents STREQUAL "")
			list(APPEND MONO_PRESENT "${directory}")
			break()
		endif()
	endforeach()
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

# Classify the existing ordered walk, rather than maintaining another module
# inventory. Directory owners cover overview/tools even without a graph row;
# unknown directories and future layer bands remain visible under Other guides.
function(_category_of directory out)
	if(directory STREQUAL "mono.engine")
		set(category overview)
	elseif(directory MATCHES "^mono\\.tools(/|$)")
		set(category tools)
	else()
		set(category other)
		foreach(name IN LISTS module_names program_names)
			_directory_of("${name}" candidate)
			if(NOT candidate STREQUAL directory)
				continue()
			endif()
			string(JSON layer ERROR_VARIABLE error GET "${MONO_EXPECTED}" modules ${name} layer)
			if(error OR layer STREQUAL "")
				set(category products)
			elseif(layer GREATER_EQUAL 1 AND layer LESS_EQUAL 4)
				set(category foundations)
			elseif(layer GREATER_EQUAL 5 AND layer LESS_EQUAL 8)
				set(category simulation)
			elseif(layer GREATER_EQUAL 9 AND layer LESS_EQUAL 11)
				set(category execution)
			elseif(layer GREATER_EQUAL 12 AND layer LESS_EQUAL 13)
				set(category runtime)
			endif()
			break()
		endforeach()
	endif()
	set(${out} "${category}" PARENT_SCOPE)
endfunction()

set(MONO_CATEGORIES overview foundations simulation execution runtime products tools other)
set(MONO_TITLE_overview "Engine overview")
set(MONO_TITLE_foundations "Foundations (L1-4)")
set(MONO_TITLE_simulation "Scene and simulation (L5-8)")
set(MONO_TITLE_execution "Content and execution (L9-11)")
set(MONO_TITLE_runtime "Runtime (L12-13)")
set(MONO_TITLE_products "Products")
set(MONO_TITLE_tools "Tools")
set(MONO_TITLE_other "Other guides")
set(MONO_DESCRIPTION_overview "Shared invariants for every engine library.")
set(MONO_DESCRIPTION_foundations "Platform values, parallel work, storage and world ticking.")
set(MONO_DESCRIPTION_simulation "Geometry, authored scenes, assets and simulation systems.")
set(MONO_DESCRIPTION_execution "Content processing, scripting and transport services.")
set(MONO_DESCRIPTION_runtime "High-level runtime and presentation systems.")
set(MONO_DESCRIPTION_products "Programs and their owned modules outside the engine layer stack.")
set(MONO_DESCRIPTION_tools "Developer tools and their module guides.")
set(MONO_DESCRIPTION_other "Guides outside the current graph categories, preserved for discovery.")
set(MONO_DIRECTORY_START "\n\n@htmlonly\n<div class=\"docs-directory\">\n@endhtmlonly\n")
set(MONO_MODULE_DIRECTORY_START "\n\n@htmlonly\n<div class=\"docs-directory docs-module-directory\">\n@endhtmlonly\n")
set(MONO_DIRECTORY_END "\n\n@htmlonly\n</div>\n@endhtmlonly\n")

set(MONO_PAGE
"# Module guides

Find module invariants and guides by engine layer or owning product. Read the
owning module's invariants before changing its code;
[AGENTS.md](@ref md_AGENTS) explains the shared rules.
")

set(MONO_MODULE_PAGES "")
foreach(directory IN LISTS MONO_ORDER)
	_category_of("${directory}" category)
	set(module_pages "")
	if(EXISTS "${ROOT}/${directory}/AGENTS.md")
		_page_id("${directory}/AGENTS.md" id)
		list(APPEND module_pages "${id}")
	endif()

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
		list(APPEND module_pages "${id}")
	endforeach()

	list(LENGTH module_pages page_count)
	get_filename_component(label "${directory}" NAME)
	string(REGEX REPLACE "^mono\\." "" label "${label}")
	string(REGEX REPLACE "^mono\\." "" owner_path "${directory}")
	if(NOT directory MATCHES "^mono\\.(engine|tools)/" AND owner_path MATCHES "/")
		set(label "${owner_path}")
	endif()
	if(page_count GREATER 1)
		# Derive the wrapper identity from its full directory, independently of its label.
		_page_id("${directory}" encoded_directory)
		string(REGEX REPLACE "^md_" "docs_module_" module_id "${encoded_directory}")
		string(APPEND MONO_LINKS_${category} "\n- @subpage ${module_id} \"${label}\"\n\n  `${directory}/`\n")
		string(APPEND MONO_MODULE_PAGES "\n\n@page ${module_id} ${label}\n\nGuides and invariants for `${directory}/`.${MONO_DIRECTORY_START}")
		foreach(id IN LISTS module_pages)
			if(id MATCHES "_2AGENTS$")
				string(APPEND MONO_MODULE_PAGES "\n- @subpage ${id} \"Invariants\"")
			else()
				string(APPEND MONO_MODULE_PAGES "\n- @subpage ${id}")
			endif()
		endforeach()
		string(APPEND MONO_MODULE_PAGES "${MONO_DIRECTORY_END}")
	else()
		foreach(id IN LISTS module_pages)
			string(APPEND MONO_LINKS_${category} "\n- @subpage ${id} \"${label}\"\n\n  `${directory}/`\n")
		endforeach()
	endif()
endforeach()

string(APPEND MONO_PAGE "${MONO_DIRECTORY_START}")
foreach(category IN LISTS MONO_CATEGORIES)
	if(NOT "${MONO_LINKS_${category}}" STREQUAL "")
		string(APPEND MONO_PAGE "\n- @subpage docs_modules_${category}\n\n  ${MONO_DESCRIPTION_${category}}\n")
	endif()
endforeach()
string(APPEND MONO_PAGE "${MONO_DIRECTORY_END}")
string(APPEND MONO_PAGE "\n\nGenerated from the module graph and source guides. After adding module documentation,\nrun `just docs-pages`; `just docs-pages-check` checks this index.")
foreach(category IN LISTS MONO_CATEGORIES)
	if(NOT "${MONO_LINKS_${category}}" STREQUAL "")
		string(APPEND MONO_PAGE "\n\n@page docs_modules_${category} ${MONO_TITLE_${category}}\n\n${MONO_DESCRIPTION_${category}}${MONO_MODULE_DIRECTORY_START}${MONO_LINKS_${category}}${MONO_DIRECTORY_END}")
	endif()
endforeach()
string(APPEND MONO_PAGE "${MONO_MODULE_PAGES}")

if(CHECK)
	if(NOT EXISTS "${OUT}")
		message(FATAL_ERROR "${OUT} does not exist. Run `just docs-pages`.")
	endif()

	file(READ "${OUT}" current)
	if(NOT current STREQUAL MONO_PAGE)
		message(FATAL_ERROR
			"${OUT} is stale.\n"
			"  It is generated from mono.tools/architecture/expected_graph.json and the "
			"AGENTS.md and docs/*.md files in the tree. Run `just docs-pages` and commit the result.")
	endif()

	list(LENGTH MONO_ORDER entries)
	message(STATUS "module pages ok - ${entries} module page(s)")
	return()
endif()

file(WRITE "${OUT}" "${MONO_PAGE}")
list(LENGTH MONO_ORDER entries)
message(STATUS "wrote ${OUT} - ${entries} module page(s)")
