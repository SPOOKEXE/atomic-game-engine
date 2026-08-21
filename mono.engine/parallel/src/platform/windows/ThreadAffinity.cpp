#include "ThreadAffinity.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

// Windows.h defines min and max as macros, which breaks anything that names
// std::min - or, as it did here, `std::numeric_limits<WORD>::max()`. The
// expansion leaves `(...)()` behind and MSVC reports it as `error C2059: syntax
// error: ')'` on a line that has nothing wrong with it.
//
// **mingw-w64 compiles this file without the guard, which is why it survived.**
// `just preset=windows-cross build` builds every Windows platform file on
// Linux and this one was clean, so the only thing that ever saw the macro was
// the MSVC runner - and that runner is `continue-on-error`, so it said so into
// a log nobody had a reason to open. `mono.discord/src/platform/windows/Socket.cpp`
// carries the same guard and a comment claiming every platform file does; this
// is the file that made that untrue.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace engine::parallel::platform {

	std::vector<Processor> AvailableProcessors() {
		USHORT groupCount = 0;
		if (!::GetProcessGroupAffinity(::GetCurrentProcess(), &groupCount, nullptr) &&
			::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			return {};
		}

		std::vector<USHORT> groups(groupCount);
		if (groupCount == 0 ||
			!::GetProcessGroupAffinity(::GetCurrentProcess(), &groupCount, groups.data())) {
			return {};
		}

		std::vector<Processor> processors;
		if (groups.size() == 1) {
			DWORD_PTR processMask = 0;
			DWORD_PTR systemMask = 0;
			if (::GetProcessAffinityMask(::GetCurrentProcess(), &processMask, &systemMask) != 0) {
				for (DWORD number = 0; number < sizeof(DWORD_PTR) * 8u; number++) {
					if ((processMask & (static_cast<DWORD_PTR>(1) << number)) != 0) {
						processors.push_back(Processor{groups.front(), number});
					}
				}
				return processors;
			}
		}

		for (const USHORT group : groups) {
			const DWORD count = ::GetActiveProcessorCount(group);
			if (count == 0 || count == 0xFFFFFFFFu) {
				continue;
			}
			for (DWORD number = 0; number < count; number++) {
				processors.push_back(Processor{group, number});
			}
		}
		return processors;
	}

	std::vector<Processor> DistinctCoreProcessors() {
		const std::vector<Processor> allowed = AvailableProcessors();
		if (allowed.empty()) {
			return {};
		}

		DWORD bytes = 0;
		if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes) &&
			::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
			return {};
		}

		std::vector<std::byte> storage(bytes);
		auto *information = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
		if (bytes == 0 || !::GetLogicalProcessorInformationEx(RelationProcessorCore, information, &bytes)) {
			return {};
		}

		std::vector<Processor> processors;
		for (DWORD offset = 0; offset < bytes;) {
			auto *entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data() + offset);
			if (entry->Size == 0 || entry->Relationship != RelationProcessorCore) {
				return {};
			}

			bool selected = false;
			for (WORD groupIndex = 0; groupIndex < entry->Processor.GroupCount && !selected; groupIndex++) {
				const GROUP_AFFINITY &group = entry->Processor.GroupMask[groupIndex];
				for (DWORD number = 0; number < sizeof(KAFFINITY) * 8u; number++) {
					if ((group.Mask & (static_cast<KAFFINITY>(1) << number)) == 0) {
						continue;
					}
					const Processor candidate{group.Group, number};
					if (std::find(allowed.begin(), allowed.end(), candidate) != allowed.end()) {
						processors.push_back(candidate);
						selected = true;
						break;
					}
				}
			}

			offset += entry->Size;
		}
		return processors;
	}

	bool PinCurrentThread(Processor processor) {
		if (!processor.Valid() || processor.Group > std::numeric_limits<WORD>::max() ||
			processor.Number >= sizeof(KAFFINITY) * 8u) {
			return false;
		}

		GROUP_AFFINITY selected{};
		selected.Group = static_cast<WORD>(processor.Group);
		selected.Mask = static_cast<KAFFINITY>(1) << processor.Number;
		return ::SetThreadGroupAffinity(::GetCurrentThread(), &selected, nullptr) != 0;
	}

	Processor CurrentProcessor() {
		PROCESSOR_NUMBER current{};
		::GetCurrentProcessorNumberEx(&current);
		return Processor{current.Group, current.Number};
	}
}
