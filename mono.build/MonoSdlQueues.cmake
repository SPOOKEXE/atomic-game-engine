include_guard(GLOBAL)

# Materialize pinned SDL sources with the required Vulkan correctness patches and,
# when enabled, the private queue patch. Git archives tracked sources only; a
# content-keyed directory keeps concurrent presets from replacing active builds.
function(mono_prepare_sdl_queues root output revision_output)
	find_package(Git REQUIRED)
	set(source "${root}/mono.vendor/sdl")
	set(patches
		"${root}/mono.vendor/patches/sdl/vulkan-compute-samplers.patch"
		"${root}/mono.vendor/patches/sdl/vulkan-texture-cancellation.patch"
		"${root}/mono.vendor/patches/sdl/vulkan-defrag-source-layout.patch")
	if(MONO_SDL_VULKAN_QUEUES)
		list(APPEND patches "${root}/mono.vendor/patches/sdl/vulkan-queues.patch")
	endif()
	if(NOT EXISTS "${source}/CMakeLists.txt")
		message(FATAL_ERROR "SDL sources are missing. Run just setup.")
	endif()
	foreach(patch IN LISTS patches)
		if(NOT EXISTS "${patch}")
			message(FATAL_ERROR "Required SDL patch is missing: ${patch}")
		endif()
	endforeach()
	unset(patch)

	# Repository-local Git environment from hooks must not redirect these reads.
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" rev-parse --local-env-vars
		OUTPUT_VARIABLE local_environment OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE result
	)
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "Cannot inspect the Git environment for SDL preparation.")
	endif()
	string(REPLACE "\n" ";" local_environment "${local_environment}")
	set(git "${CMAKE_COMMAND}" -E env)
	foreach(variable IN LISTS local_environment)
		string(STRIP "${variable}" variable)
		if(NOT variable STREQUAL "")
			list(APPEND git "--unset=${variable}")
		endif()
	endforeach()
	list(APPEND git "${GIT_EXECUTABLE}")

	execute_process(
		COMMAND ${git} -C "${root}" ls-files --stage -- mono.vendor/sdl
		OUTPUT_VARIABLE gitlink OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_VARIABLE error RESULT_VARIABLE result
	)
	if(NOT result EQUAL 0 OR NOT gitlink MATCHES "^160000 ([0-9a-f]+) 0[\t]mono[.]vendor/sdl$")
		message(FATAL_ERROR "Cannot read the pinned SDL gitlink: ${error}")
	endif()
	set(pinned_commit "${CMAKE_MATCH_1}")
	execute_process(
		COMMAND ${git} -C "${source}" rev-parse HEAD
		OUTPUT_VARIABLE checkout OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_VARIABLE error RESULT_VARIABLE result
	)
	if(NOT result EQUAL 0 OR NOT checkout STREQUAL pinned_commit)
		message(FATAL_ERROR "SDL checkout does not match its pinned gitlink. Run just setup. ${error}")
	endif()
	execute_process(
		COMMAND ${git} -C "${source}" status --porcelain --untracked-files=no
		OUTPUT_VARIABLE edits OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_VARIABLE error RESULT_VARIABLE result
	)
	if(NOT result EQUAL 0 OR NOT edits STREQUAL "")
		message(FATAL_ERROR "SDL must stay pristine. Move local changes into its project patch. ${edits} ${error}")
	endif()

	set(patch_fingerprint "")
	foreach(patch IN LISTS patches)
		file(SHA256 "${patch}" patch_hash)
		get_filename_component(patch_name "${patch}" NAME)
		string(APPEND patch_fingerprint "${patch_name}:${patch_hash}\n")
	endforeach()
	string(SHA256 fingerprint "atomic-sdl-patches-v2\n${pinned_commit}\n${patch_fingerprint}")
	string(SUBSTRING "${fingerprint}" 0 16 key)
	set(cache "${root}/.cache/vendor/sdl")
	set(destination "${cache}/${key}")
	set(${revision_output} "SDL-g${pinned_commit}-atomic-${key}" PARENT_SCOPE)
	file(MAKE_DIRECTORY "${cache}")
	file(LOCK "${cache}/prepare.lock" GUARD FUNCTION TIMEOUT 60 RESULT_VARIABLE result)
	if(NOT result STREQUAL "0")
		message(FATAL_ERROR "Cannot lock SDL source preparation: ${result}")
	endif()
	if(EXISTS "${destination}/.atomic-source.sha256" AND EXISTS "${destination}/CMakeLists.txt")
		file(READ "${destination}/.atomic-source.sha256" previous)
		if(previous STREQUAL fingerprint)
			set(${output} "${destination}" PARENT_SCOPE)
			return()
		endif()
		message(FATAL_ERROR "SDL source-cache key collision or invalid stamp: ${destination}")
	endif()

	set(staging "${cache}/.prepare-${key}")
	set(archive "${cache}/.archive-${key}.tar")
	file(REMOVE_RECURSE "${staging}")
	file(MAKE_DIRECTORY "${staging}")
	execute_process(
		COMMAND ${git} -C "${source}" archive --format=tar "--output=${archive}" "${pinned_commit}"
		ERROR_VARIABLE error RESULT_VARIABLE result
	)
	if(NOT result EQUAL 0)
		file(REMOVE_RECURSE "${staging}")
		file(REMOVE "${archive}")
		message(FATAL_ERROR "Cannot archive the pinned SDL source: ${error}")
	endif()
	# Archive mtimes precede existing objects; stamp fresh extraction so Ninja and
	# Make rebuild after a source revision, including when a previous pin returns.
	file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${staging}" TOUCH)
	file(REMOVE "${archive}")
	file(RELATIVE_PATH staging_relative "${root}" "${staging}")
	foreach(patch IN LISTS patches)
		get_filename_component(patch_name "${patch}" NAME)
		execute_process(
			COMMAND ${git} -C "${root}" apply --whitespace=error "--directory=${staging_relative}" "${patch}"
			ERROR_VARIABLE error RESULT_VARIABLE result
		)
		if(NOT result EQUAL 0)
			file(REMOVE_RECURSE "${staging}")
			message(FATAL_ERROR "SDL patch ${patch_name} no longer applies to ${pinned_commit}: ${error}")
		endif()
	endforeach()
	file(WRITE "${staging}/.atomic-source.sha256" "${fingerprint}")
	file(REMOVE_RECURSE "${destination}")
	file(RENAME "${staging}" "${destination}")
	message(STATUS "Prepared patched SDL sources at ${destination}")
	set(${output} "${destination}" PARENT_SCOPE)
endfunction()
