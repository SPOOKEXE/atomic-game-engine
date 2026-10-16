#include <testrunner/Process.hpp>

#define WIN32_LEAN_AND_MEAN
#include <array>
#include <cstddef>
#include <vector>
#include <windows.h>

namespace testrunner {

	namespace {
		// CreateProcessW takes one command line, not a list, so the quoting
		// this header exists to avoid has to happen once, here, correctly.
		//
		// These are the rules CommandLineToArgvW parses back - a backslash is
		// only special immediately before a quote, which is why the run is
		// counted rather than every backslash escaped.
		std::wstring Quote(const std::string &argument) {
			std::wstring wide;
			wide.reserve(argument.size());
			for (const char character : argument) {
				wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
			}

			if (!wide.empty() && wide.find_first_of(L" \t\"") == std::wstring::npos) {
				return wide;
			}

			std::wstring quoted = L"\"";
			for (size_t index = 0; index < wide.size(); index++) {
				size_t backslashes = 0;
				while (index < wide.size() && wide[index] == L'\\') {
					backslashes++;
					index++;
				}

				if (index == wide.size()) {
					// Trailing backslashes precede the closing quote, so they
					// have to be doubled or they escape it.
					quoted.append(backslashes * 2, L'\\');
					break;
				}

				if (wide[index] == L'"') {
					quoted.append(backslashes * 2 + 1, L'\\');
				} else {
					quoted.append(backslashes, L'\\');
				}
				quoted.push_back(wide[index]);
			}
			quoted.push_back(L'"');
			return quoted;
		}
	}

	ProcessResult Run(const std::vector<std::string> &arguments) {
		ProcessResult result;
		if (arguments.empty()) {
			return result;
		}

		std::wstring commandLine;
		for (size_t index = 0; index < arguments.size(); index++) {
			if (index > 0) {
				commandLine.push_back(L' ');
			}
			commandLine += Quote(arguments[index]);
		}

		SIZE_T attributeBytes = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
		if (attributeBytes == 0) return result;
		std::vector<std::byte> attributeStorage(attributeBytes);

		SECURITY_ATTRIBUTES security{};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;

		HANDLE readEnd = nullptr;
		HANDLE writeEnd = nullptr;
		if (!CreatePipe(&readEnd, &writeEnd, &security, 0)) {
			return result;
		}
		// The parent alone reads this capture pipe. Only the writer belongs in
		// the child's explicit inheritance list.
		if (!SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0)) {
			CloseHandle(readEnd);
			CloseHandle(writeEnd);
			return result;
		}

		// STARTF_USESTDHANDLES needs a valid inheritable stdin handle. Keep the
		// parent's stdin when there is one, and use NUL in a detached runner.
		HANDLE childInput = nullptr;
		const HANDLE parentInput = GetStdHandle(STD_INPUT_HANDLE);
		if (parentInput && parentInput != INVALID_HANDLE_VALUE) {
			if (!DuplicateHandle(
					GetCurrentProcess(),
					parentInput,
					GetCurrentProcess(),
					&childInput,
					0,
					TRUE,
					DUPLICATE_SAME_ACCESS
				)) {
				CloseHandle(readEnd);
				CloseHandle(writeEnd);
				return result;
			}
		} else {
			childInput = CreateFileW(
				L"NUL",
				GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				&security,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr
			);
		}
		if (childInput == INVALID_HANDLE_VALUE) {
			CloseHandle(readEnd);
			CloseHandle(writeEnd);
			return result;
		}

		// TRUE without a handle list would copy another concurrent Run call's
		// write pipe into this child, keeping that call's reader open past exit.
		HANDLE inherited[] = {writeEnd, childInput};
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdOutput = writeEnd;
		startup.StartupInfo.hStdError = writeEnd;
		startup.StartupInfo.hStdInput = childInput;
		startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
		if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) {
			CloseHandle(childInput);
			CloseHandle(readEnd);
			CloseHandle(writeEnd);
			return result;
		}
		if (!UpdateProcThreadAttribute(
				startup.lpAttributeList,
				0,
				PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				inherited,
				sizeof(inherited),
				nullptr,
				nullptr
			)) {
			DeleteProcThreadAttributeList(startup.lpAttributeList);
			CloseHandle(childInput);
			CloseHandle(readEnd);
			CloseHandle(writeEnd);
			return result;
		}

		PROCESS_INFORMATION process{};
		const BOOL started = CreateProcessW(
			nullptr,
			commandLine.data(),
			nullptr,
			nullptr,
			TRUE,
			EXTENDED_STARTUPINFO_PRESENT,
			nullptr,
			nullptr,
			&startup.StartupInfo,
			&process
		);

		DeleteProcThreadAttributeList(startup.lpAttributeList);
		CloseHandle(childInput);
		CloseHandle(writeEnd);
		if (!started) {
			CloseHandle(readEnd);
			return result;
		}

		// Drain before waiting: the child blocks once it has written a pipe
		// buffer's worth, and waiting first would deadlock against it.
		std::array<char, 4096> buffer{};
		DWORD read = 0;
		while (ReadFile(readEnd, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) &&
			   read > 0) {
			result.Output.append(buffer.data(), read);
		}
		CloseHandle(readEnd);

		WaitForSingleObject(process.hProcess, INFINITE);

		DWORD exitCode = 0;
		GetExitCodeProcess(process.hProcess, &exitCode);
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);

		result.Started = true;
		result.ExitCode = static_cast<int>(exitCode);
		return result;
	}
}
