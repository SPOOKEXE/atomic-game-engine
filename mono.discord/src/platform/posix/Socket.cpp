#include "Platform.hpp"

#include <engine/core/Log.hpp>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <discord/Channel.hpp>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace discord {

	namespace {
		// Where a Discord socket has been found to live.
		//
		// **This list is the whole reason the search is a function rather than
		// a path.** The first entry is where the official client on a plain
		// install puts it, and the six after it are Flatpak, Snap and Vesktop -
		// which is how a large share of Linux users actually run Discord. A
		// search that stopped at the first entry would report "Discord is not
		// running" to somebody looking straight at it, and there would be
		// nothing in a log to say why.
		//
		// Taken from `vionya/discord-rich-presence`, which is the maintained
		// implementation of this protocol and the one that keeps this list
		// current.
		constexpr std::array<const char *, 7> SUBPATHS{{
			"",
			"app/com.discordapp.Discord/",
			"app/dev.vencord.Vesktop/",
			".flatpak/com.discordapp.Discord/xdg-run/",
			".flatpak/dev.vencord.Vesktop/xdg-run/",
			"snap.discord-canary/",
			"snap.discord/",
		}};

		// The directories those subpaths hang off, in the order tried.
		constexpr std::array<const char *, 4> ROOTS{{"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}};

		// Discord numbers its sockets, and a second client takes the next one.
		constexpr int SOCKETS = 10;

		// The base directory a root names, with the Snap exception applied.
		//
		// **Inside a Snap confinement `XDG_RUNTIME_DIR` points at the snap's
		// own directory**, which is not where Discord's socket is - the parent
		// is. Every other root is taken as it stands.
		std::string RootDirectory(const char *variable, bool confined) {
			const char *value = std::getenv(variable);
			if (value == nullptr || *value == '\0') {
				return {};
			}

			std::string directory(value);
			if (confined && std::strcmp(variable, "XDG_RUNTIME_DIR") == 0) {
				const size_t slash = directory.rfind('/');
				directory = slash == std::string::npos ? std::string() : directory.substr(0, slash);
			}
			if (!directory.empty() && directory.back() != '/') {
				directory.push_back('/');
			}
			return directory;
		}

		// One end of a connected unix domain socket.
		class SocketChannel final : public Channel {
		  public:
			explicit SocketChannel(int handle) : Descriptor(handle) {}

			~SocketChannel() override {
				Close();
			}

			ChannelStatus Send(std::span<const std::byte> bytes) override {
				if (Descriptor < 0) {
					return ChannelStatus::Closed;
				}

				size_t written = 0;
				while (written < bytes.size()) {
					const ssize_t wrote = ::send(
						Descriptor,
						bytes.data() + written,
						bytes.size() - written,
#ifdef MSG_NOSIGNAL
						MSG_NOSIGNAL
#else
						0
#endif
					);

					if (wrote > 0) {
						written += static_cast<size_t>(wrote);
						continue;
					}
					if (wrote < 0 && (errno == EINTR)) {
						continue;
					}

					// Nothing went, so the caller simply drops what it was
					// saying and the pipe stays usable. See the header for why
					// that is safe for this protocol and not for most.
					if (written == 0 && wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
						return ChannelStatus::Full;
					}

					// Half a frame reached Discord. Nothing can recover a
					// framed stream from that, so the pipe goes and the link
					// re-handshakes with the current activity.
					Close();
					return ChannelStatus::Closed;
				}
				return ChannelStatus::Ok;
			}

			ChannelStatus Receive(std::vector<std::byte> &into) override {
				if (Descriptor < 0) {
					return ChannelStatus::Closed;
				}

				std::array<std::byte, 4096> landing{};
				bool any = false;
				for (;;) {
					const ssize_t read = ::recv(Descriptor, landing.data(), landing.size(), 0);
					if (read > 0) {
						into.insert(into.end(), landing.begin(), landing.begin() + read);
						any = true;
						continue;
					}
					if (read == 0) {
						// An orderly hang-up. Whatever was already appended
						// still counts, and the next call reports the close.
						Close();
						return any ? ChannelStatus::Ok : ChannelStatus::Closed;
					}
					if (errno == EINTR) {
						continue;
					}
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						return any ? ChannelStatus::Ok : ChannelStatus::Empty;
					}
					Close();
					return any ? ChannelStatus::Ok : ChannelStatus::Closed;
				}
			}

			bool Open() const override {
				return Descriptor >= 0;
			}

			void Close() override {
				if (Descriptor >= 0) {
					::close(Descriptor);
					Descriptor = -1;
				}
			}

		  private:
			int Descriptor = -1;
		};

		// Connects to one path, or answers `nullptr`.
		std::unique_ptr<Channel> Dial(const std::string &path) {
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			if (path.size() >= sizeof(address.sun_path)) {
				// Longer than the kernel's field. Not an error worth reporting:
				// it is a directory nobody's Discord is in.
				return nullptr;
			}
			std::memcpy(address.sun_path, path.c_str(), path.size());

			const int handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
			if (handle < 0) {
				return nullptr;
			}

#ifdef SO_NOSIGPIPE
			// macOS has no MSG_NOSIGNAL. Without one of the two, a Discord that
			// exits mid-write takes the whole program down with SIGPIPE.
			const int on = 1;
			::setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

			if (::connect(handle, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
				::close(handle);
				return nullptr;
			}

			// **Non-blocking after connecting, not before.** A unix domain
			// connect completes or fails immediately, so doing it this way
			// costs nothing and avoids the EINPROGRESS dance that a
			// non-blocking connect would need for no benefit here.
			const int flags = ::fcntl(handle, F_GETFL, 0);
			if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0) {
				::close(handle);
				return nullptr;
			}

			return std::make_unique<SocketChannel>(handle);
		}
	}

	std::vector<std::string> SocketCandidates() {
		const bool confined = std::getenv("SNAP") != nullptr;

		std::vector<std::string> candidates;
		candidates.reserve(ROOTS.size() * SOCKETS * SUBPATHS.size());

		for (const char *variable : ROOTS) {
			const std::string root = RootDirectory(variable, confined);
			if (root.empty()) {
				continue;
			}

			// **Socket number outer, install layout inner.** Discord hands the
			// lowest free number to the first client that started, so
			// `discord-ipc-0` under any layout is a better guess than
			// `discord-ipc-1` under the plainest one.
			for (int index = 0; index < SOCKETS; index++) {
				for (const char *subpath : SUBPATHS) {
					candidates.push_back(root + subpath + "discord-ipc-" + std::to_string(index));
				}
			}
		}

		// `/tmp` last and unconditionally, because a machine with none of the
		// four variables set still has one.
		for (int index = 0; index < SOCKETS; index++) {
			candidates.push_back("/tmp/discord-ipc-" + std::to_string(index));
		}
		return candidates;
	}

	std::unique_ptr<Channel> ConnectLocal(std::string_view socketOverride) {
		if (!socketOverride.empty()) {
			return Dial(std::string(socketOverride));
		}

		for (const std::string &path : SocketCandidates()) {
			if (std::unique_ptr<Channel> wire = Dial(path)) {
				ENGINE_TRACE("discord: connected on {}", path);
				return wire;
			}
		}
		return nullptr;
	}

	int ProcessId() {
		return static_cast<int>(::getpid());
	}
}
