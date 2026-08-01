#include <testrunner/Process.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>

namespace testrunner {

	namespace {
		// CreateProcessW takes one command line, not a list, so the quoting
		// this header exists to avoid has to happen once, here, correctly.
		//
		// These are the rules CommandLineToArgvW parses back — a backslash is
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

		SECURITY_ATTRIBUTES security {};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;

		HANDLE readEnd = nullptr;
		HANDLE writeEnd = nullptr;
		if (!CreatePipe(&readEnd, &writeEnd, &security, 0)) {
			return result;
		}
		// The child must not inherit the read end, or the pipe never reports
		// EOF and the read below blocks forever.
		SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

		std::wstring commandLine;
		for (size_t index = 0; index < arguments.size(); index++) {
			if (index > 0) {
				commandLine.push_back(L' ');
			}
			commandLine += Quote(arguments[index]);
		}

		STARTUPINFOW startup {};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdOutput = writeEnd;
		startup.hStdError = writeEnd;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

		PROCESS_INFORMATION process {};
		const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
			0, nullptr, nullptr, &startup, &process);

		CloseHandle(writeEnd);
		if (!started) {
			CloseHandle(readEnd);
			return result;
		}

		// Drain before waiting: the child blocks once it has written a pipe
		// buffer's worth, and waiting first would deadlock against it.
		std::array<char, 4096> buffer {};
		DWORD read = 0;
		while (ReadFile(readEnd, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)
			&& read > 0) {
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
