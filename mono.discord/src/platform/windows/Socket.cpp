#include "Platform.hpp"

#include <engine/core/Log.hpp>

#include <array>
#include <discord/Channel.hpp>
#include <string>

// Windows.h defines min and max as macros, which breaks anything that names
// std::min. Every other platform file in this repository does the same.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace discord {

	namespace {
		// Discord numbers its pipes, and a second client takes the next one.
		constexpr int SOCKETS = 10;

		// One end of a connected named pipe.
		//
		// **Reads are non-blocking and writes are not, and that asymmetry is
		// deliberate rather than an oversight.** `PeekNamedPipe` gives a
		// reliable "is there anything" without changing the handle's mode;
		// there is no equally cheap answer for writes on a synchronous handle,
		// and the alternative is overlapped I/O with a completion object per
		// link. A frame here is a few hundred bytes against a 64 KiB pipe
		// buffer, so a blocking write needs Discord to have stopped reading
		// while still holding the pipe open. If that ever shows up in a
		// profile, this is the comment to come back to.
		class PipeChannel final : public Channel {
		  public:
			explicit PipeChannel(HANDLE handle) : Pipe(handle) {}

			~PipeChannel() override {
				Close();
			}

			ChannelStatus Send(std::span<const std::byte> bytes) override {
				if (Pipe == INVALID_HANDLE_VALUE) {
					return ChannelStatus::Closed;
				}

				size_t written = 0;
				while (written < bytes.size()) {
					DWORD wrote = 0;
					const BOOL sent = WriteFile(
						Pipe,
						bytes.data() + written,
						static_cast<DWORD>(bytes.size() - written),
						&wrote,
						nullptr
					);
					if (sent == 0) {
						// Half a frame may have reached Discord, and nothing
						// recovers a framed stream from that. The pipe goes and
						// the link re-handshakes with the current activity.
						Close();
						return ChannelStatus::Closed;
					}
					written += wrote;
				}
				return ChannelStatus::Ok;
			}

			ChannelStatus Receive(std::vector<std::byte> &into) override {
				if (Pipe == INVALID_HANDLE_VALUE) {
					return ChannelStatus::Closed;
				}

				bool any = false;
				for (;;) {
					DWORD waiting = 0;
					if (PeekNamedPipe(Pipe, nullptr, 0, nullptr, &waiting, nullptr) == 0) {
						Close();
						return any ? ChannelStatus::Ok : ChannelStatus::Closed;
					}
					if (waiting == 0) {
						return any ? ChannelStatus::Ok : ChannelStatus::Empty;
					}

					std::array<std::byte, 4096> landing{};
					const DWORD wanted =
						waiting < landing.size() ? waiting : static_cast<DWORD>(landing.size());
					DWORD read = 0;
					if (ReadFile(Pipe, landing.data(), wanted, &read, nullptr) == 0 || read == 0) {
						Close();
						return any ? ChannelStatus::Ok : ChannelStatus::Closed;
					}

					into.insert(into.end(), landing.begin(), landing.begin() + read);
					any = true;
				}
			}

			bool Open() const override {
				return Pipe != INVALID_HANDLE_VALUE;
			}

			void Close() override {
				if (Pipe != INVALID_HANDLE_VALUE) {
					CloseHandle(Pipe);
					Pipe = INVALID_HANDLE_VALUE;
				}
			}

		  private:
			HANDLE Pipe = INVALID_HANDLE_VALUE;
		};

		std::unique_ptr<Channel> Dial(const std::string &name) {
			const HANDLE pipe = CreateFileA(
				name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr
			);
			if (pipe == INVALID_HANDLE_VALUE) {
				return nullptr;
			}
			return std::make_unique<PipeChannel>(pipe);
		}
	}

	std::vector<std::string> SocketCandidates() {
		// One layout, unlike POSIX: Windows has no Flatpak and no Snap, and
		// every Discord build puts its pipe in the same namespace.
		std::vector<std::string> candidates;
		candidates.reserve(SOCKETS);
		for (int index = 0; index < SOCKETS; index++) {
			candidates.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(index));
		}
		return candidates;
	}

	std::unique_ptr<Channel> ConnectLocal(std::string_view socketOverride) {
		if (!socketOverride.empty()) {
			return Dial(std::string(socketOverride));
		}

		for (const std::string &name : SocketCandidates()) {
			if (std::unique_ptr<Channel> wire = Dial(name)) {
				ENGINE_TRACE("discord: connected on {}", name);
				return wire;
			}
		}
		return nullptr;
	}

	int ProcessId() {
		return static_cast<int>(GetCurrentProcessId());
	}
}
