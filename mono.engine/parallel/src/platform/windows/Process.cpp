#include "Winsock.hpp"

#include <engine/core/Log.hpp>
#include <engine/parallel/Process.hpp>

#define WIN32_LEAN_AND_MEAN
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <windows.h>

namespace engine::parallel {

	namespace {
		// A `Process` carries two opaque numbers. Here they are the process id,
		// which is what `Id()` promises and what a console control event is
		// addressed to, and the handle, which is what everything else needs.
		//
		// Both, rather than the id alone: an id is reused once the process is
		// gone, so a supervisor that polled by id could eventually wait on a
		// stranger. The handle keeps the dead process referenced until it is
		// reaped, which is what makes the id safe to keep beside it.
		HANDLE AsHandle(uint64_t native) {
			return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(native));
		}

		uint64_t AsNative(HANDLE handle) {
			return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
		}

		// The exit code `Kill` gives a child.
		//
		// Windows' own code for a process stopped from outside, chosen so that
		// a supervisor's log and a crash dump agree with each other and with
		// what the operating system would have written had it done the killing.
		constexpr DWORD KILLED = 0xC000013Au; // STATUS_CONTROL_C_EXIT

		// Turns an exit code into how the child ended.
		//
		// **Windows has no signals, so the hard-fault case has to be read out
		// of the exit code.** A process that died of a fault exits with the
		// NTSTATUS that killed it, and every one of those carries the two
		// severity bits set - 0xC0000005 for an access violation, 0xC0000409
		// for a stack buffer overrun, 0xC0000017 for out of memory. A program
		// that returned a number from main cannot collide with them: `main`
		// returns an `int` and the CRT truncates it to the low bits.
		//
		// So `Signalled` here means exactly what it means on POSIX - the child
		// did not get to choose how it ended - and `Faulted()` reads the same
		// on both.
		ProcessStatus Classify(DWORD code) {
			ProcessStatus status;

			if ((code & 0xC0000000u) != 0xC0000000u) {
				status.Reason = ExitReason::Exited;
				status.Code = static_cast<int>(code);
				return status;
			}

			status.Reason = ExitReason::Signalled;

			// There is no signal number to report, so the low half of the
			// status goes in its place: it is the part that tells an access
			// violation from a stack overflow, which is what a person reading
			// the log actually wants. Never zero, because a caller checks it.
			const auto facility = static_cast<int>(code & 0xFFFFu);
			status.Signal = facility != 0 ? facility : 1;
			return status;
		}

		// Reaps a child if it has ended, without waiting when `block` is false.
		ProcessStatus Reap(uint64_t native, bool block) {
			ProcessStatus status;
			if (native == 0) {
				status.Reason = ExitReason::Gone;
				return status;
			}

			const HANDLE handle = AsHandle(native);
			const DWORD waited = WaitForSingleObject(handle, block ? INFINITE : 0);

			if (waited == WAIT_TIMEOUT) {
				status.Reason = ExitReason::Running;
				return status;
			}
			if (waited != WAIT_OBJECT_0) {
				// Never ours, or already closed. Either way there is nothing
				// left to observe.
				status.Reason = ExitReason::Gone;
				return status;
			}

			DWORD code = 0;
			if (!GetExitCodeProcess(handle, &code)) {
				status.Reason = ExitReason::Gone;
				return status;
			}
			return Classify(code);
		}

		// Quotes one argument the way `CommandLineToArgvW` parses it back.
		//
		// `CreateProcessW` takes one command line rather than a list, so the
		// quoting a caller passing a vector is entitled to avoid has to happen
		// once, here, correctly. A backslash is only special immediately before
		// a quote, which is why the run is counted rather than every backslash
		// escaped.
		//
		// The same rules are spelled out again in
		// `mono.tools/testrunner/src/platform/windows/Process.cpp`. Two copies
		// rather than a shared one because a test runner is a tool and this is
		// the engine, and a dependency between them to share thirty lines would
		// buy the tighter coupling of the two.
		std::wstring QuoteArgument(const std::wstring &argument) {
			if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring::npos) {
				return argument;
			}

			std::wstring quoted = L"\"";
			for (size_t index = 0; index < argument.size(); index++) {
				size_t backslashes = 0;
				while (index < argument.size() && argument[index] == L'\\') {
					backslashes++;
					index++;
				}

				if (index == argument.size()) {
					// Trailing backslashes precede the closing quote, so they
					// have to be doubled or they escape it.
					quoted.append(backslashes * 2, L'\\');
					break;
				}

				if (argument[index] == L'"') {
					quoted.append(backslashes * 2 + 1, L'\\');
				} else {
					quoted.append(backslashes, L'\\');
				}
				quoted.push_back(argument[index]);
			}
			quoted.push_back(L'"');
			return quoted;
		}

		// Widens an argument, which arrives as UTF-8 because everything else in
		// the engine is.
		std::wstring Widen(const std::string &text) {
			if (text.empty()) {
				return {};
			}

			const int needed =
				MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			if (needed <= 0) {
				return {};
			}

			std::wstring wide(static_cast<size_t>(needed), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
			return wide;
		}

		// This process's environment with the handover pipe added to it.
		//
		// Built rather than set with `SetEnvironmentVariableW` around the
		// spawn, because that would be a race: two supervisors starting hosts
		// on two threads would each see the other's value, and the child that
		// read the wrong one would take a channel meant for a stranger.
		//
		// The block is `name=value\0` repeated and terminated by an extra
		// `\0`. Any variable of this name already in the environment is
		// dropped, so a host that starts a host of its own does not pass its
		// own handover down. Null for a child that gets no channel, which
		// leaves the name absent rather than present and meaningless.
		std::vector<wchar_t> EnvironmentWith(HANDLE handover) {
			std::vector<wchar_t> block;

			const std::wstring prefix = std::wstring(platform::INHERITED_VARIABLE) + L"=";

			wchar_t *existing = GetEnvironmentStringsW();
			if (existing != nullptr) {
				for (const wchar_t *entry = existing; *entry != L'\0';) {
					const size_t length = wcslen(entry);
					const bool ours = _wcsnicmp(entry, prefix.c_str(), prefix.size()) == 0;
					if (!ours) {
						block.insert(block.end(), entry, entry + length + 1);
					}
					entry += length + 1;
				}
				FreeEnvironmentStringsW(existing);
			}

			if (handover != nullptr) {
				const std::wstring entry =
					prefix +
					std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handover)));
				block.insert(block.end(), entry.c_str(), entry.c_str() + entry.size() + 1);
			}

			// The terminator. A block that is otherwise empty still needs one,
			// which is why this is unconditional.
			block.push_back(L'\0');
			return block;
		}

		// Whether a handle is a console, which decides how it reaches a child.
		//
		// **Console handles must not go in an inherit list.** A child that
		// shares its parent's console gets the console's handles by attaching
		// to it, and naming one in `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` makes
		// `CreateProcessW` fail outright. A handle that is a file or a pipe -
		// a supervisor whose output was redirected to a log - has to be listed,
		// or the child's output goes nowhere.
		//
		// Only ever asked about a handle that exists; an absent stream is
		// neither case and the caller drops it before getting here.
		bool IsConsole(HANDLE handle) {
			return GetFileType(handle) == FILE_TYPE_CHAR;
		}
	}

	Process::~Process() {
		// A supervisor that goes away must not leave hosts behind: an orphaned
		// host holds its worlds, its memory and its port, and nothing is left
		// that knows to stop it.
		if (Identifier != 0 && Poll().Alive()) {
			Kill();
			Wait();
		}
	}

	Process::Process(Process &&other) noexcept
		: Identifier(other.Identifier), Native(other.Native), Last(other.Last) {
		other.Identifier = 0;
		other.Native = 0;
		other.Last = ProcessStatus{};
	}

	Process &Process::operator=(Process &&other) noexcept {
		if (this == &other) {
			return *this;
		}

		if (Identifier != 0 && Poll().Alive()) {
			Kill();
			Wait();
		}

		Identifier = other.Identifier;
		Native = other.Native;
		Last = other.Last;
		other.Identifier = 0;
		other.Native = 0;
		other.Last = ProcessStatus{};
		return *this;
	}

	bool Process::Start(const std::filesystem::path &program, const std::vector<std::string> &arguments) {
		return Start(program, arguments, ChannelEnd{});
	}

	bool Process::Start(
		const std::filesystem::path &program, const std::vector<std::string> &arguments, ChannelEnd endpoint
	) {
		if (Identifier != 0) {
			return false;
		}

		const std::wstring path = program.wstring();

		std::wstring commandLine = QuoteArgument(path);
		for (const std::string &argument : arguments) {
			commandLine.push_back(L' ');
			commandLine += QuoteArgument(Widen(argument));
		}

		// Which handles the child is allowed to have, and only those. Without a
		// list, inheritance is all-or-nothing: the child would receive every
		// inheritable handle this process holds, including the driver's own end
		// of the channel being handed over - and while it held a copy, neither
		// side would ever see the other go away.
		std::vector<HANDLE> inherited;

		HANDLE streams[3] = {
			GetStdHandle(STD_INPUT_HANDLE),
			GetStdHandle(STD_OUTPUT_HANDLE),
			GetStdHandle(STD_ERROR_HANDLE),
		};

		// **Two ways a child's output reaches the same place its supervisor's
		// does, and they cannot be mixed.** A console child attaches to its
		// parent's console and picks the streams up from there, needing no
		// handles passed at all. A supervisor whose own output was redirected
		// to a log has file or pipe handles instead, and those have to be
		// passed and inherited explicitly.
		//
		// The handle list is what forces the choice: once one exists, only what
		// is named in it is inherited, and a console handle named in one is
		// what `CreateProcessW` refuses outright. So the streams are passed
		// only when all three are redirected, and otherwise none are and the
		// console carries them.
		//
		// The case that loses is a supervisor with *some* streams redirected
		// and some on the console: its children put all three on the console.
		// Written down rather than worked around, because the shape that would
		// handle it is a pipe per stream and a thread to drain each, which is a
		// lot of machinery for a configuration nothing does on purpose.
		// A stream is one of three things, and only the middle one decides:
		// absent, which is nothing to pass on; a console, which cannot be
		// passed; or a file or pipe, which must be. So the streams are passed
		// when at least one is passable and none is a console.
		bool anyConsole = false;
		bool anyPassable = false;
		for (HANDLE &stream : streams) {
			if (stream == INVALID_HANDLE_VALUE) {
				stream = nullptr;
			}
			if (stream == nullptr) {
				continue;
			}
			if (IsConsole(stream)) {
				anyConsole = true;
			} else {
				anyPassable = true;
			}
		}

		const bool redirected = anyPassable && !anyConsole;

		if (redirected) {
			for (HANDLE stream : streams) {
				// An absent stream stays absent: it is passed as null in the
				// startup information, which is legal, and naming it in the
				// inherit list is not.
				if (stream == nullptr) {
					continue;
				}
				SetHandleInformation(stream, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
				inherited.push_back(stream);
			}
		}

		// The channel does not go in the list, because a socket cannot cross a
		// spawn: the handle arrives in the child and Winsock does not recognise
		// it. What crosses is a pipe, and the socket follows over it once the
		// child exists and can be named. See `Winsock.hpp`.
		HANDLE handoverChild = nullptr;
		HANDLE handoverParent = nullptr;
		if (endpoint.Valid()) {
			if (!platform::MakeChannelHandover(
					reinterpret_cast<void *&>(handoverChild), reinterpret_cast<void *&>(handoverParent)
				)) {
				ENGINE_ERROR("could not prepare a channel for '{}': {}", program.string(), GetLastError());
				return false;
			}
			inherited.push_back(handoverChild);
		}

		// Closes both ends of the handover whatever happens next, so no early
		// return has to remember to.
		struct Handover {
			HANDLE Child;
			HANDLE Parent;

			~Handover() {
				if (Child != nullptr) {
					CloseHandle(Child);
				}
				if (Parent != nullptr) {
					CloseHandle(Parent);
				}
			}
		} handover{handoverChild, handoverParent};

		STARTUPINFOEXW startup{};

		if (redirected) {
			startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
			startup.StartupInfo.hStdInput = streams[0];
			startup.StartupInfo.hStdOutput = streams[1];
			startup.StartupInfo.hStdError = streams[2];
		}

		// A new process group, so `RequestStop` has a group to address a
		// console control event to. It also means the child does not take the
		// operator's Ctrl+C, which is correct for a supervised host: the
		// supervisor decides when its hosts stop.
		DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;

		// `cb` names which of the two structures this is, so it tracks the flag
		// rather than the variable's type: the extended size without
		// EXTENDED_STARTUPINFO_PRESENT is a size `CreateProcessW` rejects.
		startup.StartupInfo.cb = sizeof(STARTUPINFOW);

		std::vector<char> attributeStorage;
		if (!inherited.empty()) {
			SIZE_T size = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
			attributeStorage.resize(size);

			auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
				ENGINE_ERROR("could not restrict what '{}' inherits: {}", program.string(), GetLastError());
				return false;
			}

			if (!UpdateProcThreadAttribute(
					attributes,
					0,
					PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
					inherited.data(),
					inherited.size() * sizeof(HANDLE),
					nullptr,
					nullptr
				)) {
				DeleteProcThreadAttributeList(attributes);
				// Refused rather than started without the list. Spawning anyway
				// would mean turning inheritance on with nothing restricting
				// it, which hands the child every inheritable handle this
				// process holds - including the driver's own end of this very
				// channel, whose whole job is to notice when one side goes
				// away.
				ENGINE_ERROR("could not restrict what '{}' inherits: {}", program.string(), GetLastError());
				return false;
			}

			startup.lpAttributeList = attributes;
			startup.StartupInfo.cb = sizeof(startup);
			flags |= EXTENDED_STARTUPINFO_PRESENT;
		}

		std::vector<wchar_t> environment = EnvironmentWith(handoverChild);

		PROCESS_INFORMATION child{};
		const BOOL started = CreateProcessW(
			path.c_str(),
			commandLine.data(),
			nullptr,
			nullptr,
			// Handle inheritance is on only when there is something in the
			// list; with the list present it is the list that decides, and
			// without one there is nothing to give away.
			inherited.empty() ? FALSE : TRUE,
			flags,
			environment.data(),
			nullptr,
			&startup.StartupInfo,
			&child
		);

		if (startup.lpAttributeList != nullptr) {
			DeleteProcThreadAttributeList(startup.lpAttributeList);
		}

		// The second half of the handover, and it can only happen here: the
		// description Winsock mints is made out for one process id, and there
		// was no process id until the line above. The child is already blocked
		// reading the pipe for it.
		if (started && endpoint.Valid()) {
			platform::SendChannelToChild(endpoint.Raw(), child.dwProcessId, handoverParent);
		}

		// Closed here, on every path. While this process holds a copy the child
		// can never see the channel end, because a socket is counted by
		// references rather than by intentions - and the child's copy is one of
		// its own, made by the handover rather than shared with this one.
		endpoint.Close();

		if (!started) {
			ENGINE_ERROR("could not start '{}': {}", program.string(), GetLastError());
			return false;
		}

		// The thread handle is of no use to a supervisor and holding it would
		// keep the primary thread's kernel object alive for the life of the
		// child.
		CloseHandle(child.hThread);

		Identifier = static_cast<uint64_t>(child.dwProcessId);
		Native = AsNative(child.hProcess);
		Last = ProcessStatus{};
		Last.Reason = ExitReason::Running;
		return true;
	}

	ProcessStatus Process::Poll() {
		if (Identifier == 0) {
			return Last;
		}

		const ProcessStatus status = Reap(Native, false);
		if (!status.Alive()) {
			// Reaped, so nothing accumulates for a supervisor polling every
			// barrier. The last status is kept so a caller can still ask how it
			// ended after the fact.
			CloseHandle(AsHandle(Native));
			Last = status;
			Identifier = 0;
			Native = 0;
		}
		return status;
	}

	ProcessStatus Process::Wait() {
		if (Identifier == 0) {
			return Last;
		}

		Last = Reap(Native, true);
		CloseHandle(AsHandle(Native));
		Identifier = 0;
		Native = 0;
		return Last;
	}

	bool Process::RequestStop() {
		if (Identifier == 0) {
			return false;
		}

		// **Windows has no signal a process is obliged to notice.** The closest
		// thing is a console control event, which reaches the child because it
		// was started in a process group of its own - and which a host handles
		// the way it would handle SIGTERM: flush a snapshot, close the channel,
		// exit.
		//
		// It only works where there is a console. A supervisor running as a
		// service has none, and there is then nothing polite left to try, so
		// this stops being polite and says so by falling through to the same
		// thing `Kill` does. That is a real difference from POSIX and it is
		// written down here rather than discovered: on Windows without a
		// console, `RequestStop` gives the child no chance to flush.
		if (GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(Identifier)) != 0) {
			return true;
		}
		return Kill();
	}

	bool Process::Kill() {
		if (Identifier == 0) {
			return false;
		}
		return TerminateProcess(AsHandle(Native), KILLED) != 0;
	}
}
