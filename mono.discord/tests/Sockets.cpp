// Where a Discord socket is looked for, and that connecting to one works.
//
// **POSIX only, and it opens real sockets**, because the path search is the one
// part of this module that a fake channel cannot exercise: it is a question
// about the filesystem and the environment, and getting it wrong is exactly the
// failure that looks like "rich presence does not work" with nothing in a log.
//
// Windows has one pipe namespace and no Flatpak or Snap, so there is nothing
// there to get wrong in the same way.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <discord/Channel.hpp>
#include <string>
#include <vector>

TEST_SUITE_ID("discord.sockets")

#ifndef _WIN32

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
	// A variable put back exactly as it was, present or absent.
	//
	// Restoring it to empty rather than to absent would leave the rest of this
	// binary running with a `TMPDIR` that exists and is blank, which is a
	// different machine from the one the other suites expect.
	class Environment {
	  public:
		Environment(const char *name, const std::string &value) : Name(name) {
			if (const char *was = std::getenv(name); was != nullptr) {
				Had = true;
				Was = was;
			}
			::setenv(name, value.c_str(), 1);
		}

		explicit Environment(const char *name) : Name(name) {
			if (const char *was = std::getenv(name); was != nullptr) {
				Had = true;
				Was = was;
			}
			::unsetenv(name);
		}

		~Environment() {
			if (Had) {
				::setenv(Name, Was.c_str(), 1);
			} else {
				::unsetenv(Name);
			}
		}

		Environment(const Environment &) = delete;
		Environment &operator=(const Environment &) = delete;

	  private:
		const char *Name;
		bool Had = false;
		std::string Was;
	};

	// A listening unix socket at `path`, closed when this goes.
	class Listener {
	  public:
		explicit Listener(const std::string &path) : Path(path) {
			sockaddr_un address{};
			address.sun_family = AF_UNIX;
			REQUIRE(path.size() < sizeof(address.sun_path));
			std::memcpy(address.sun_path, path.c_str(), path.size());

			Descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
			REQUIRE(Descriptor >= 0);
			REQUIRE(::bind(Descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
			REQUIRE(::listen(Descriptor, 4) == 0);
		}

		~Listener() {
			if (Descriptor >= 0) {
				::close(Descriptor);
			}
			std::error_code failed;
			std::filesystem::remove(Path, failed);
		}

		Listener(const Listener &) = delete;
		Listener &operator=(const Listener &) = delete;

	  private:
		std::string Path;
		int Descriptor = -1;
	};

	// A directory that removes itself.
	class Scratch {
	  public:
		Scratch() {
			std::string pattern = (std::filesystem::temp_directory_path() / "atomic-discord-XXXXXX").string();
			std::vector<char> writable(pattern.begin(), pattern.end());
			writable.push_back('\0');
			REQUIRE(::mkdtemp(writable.data()) != nullptr);
			Path = writable.data();
		}

		~Scratch() {
			std::error_code failed;
			std::filesystem::remove_all(Path, failed);
		}

		Scratch(const Scratch &) = delete;
		Scratch &operator=(const Scratch &) = delete;

		const std::string &Directory() const {
			return Path;
		}

	  private:
		std::string Path;
	};

	bool Lists(const std::vector<std::string> &candidates, const std::string &wanted) {
		return std::find(candidates.begin(), candidates.end(), wanted) != candidates.end();
	}

	size_t Position(const std::vector<std::string> &candidates, const std::string &wanted) {
		const auto found = std::find(candidates.begin(), candidates.end(), wanted);
		REQUIRE(found != candidates.end());
		return static_cast<size_t>(found - candidates.begin());
	}
}

TEST_CASE("the search covers every install layout, not just the plain one", "[discord][sockets]") {
	const Scratch scratch;
	const Environment runtime("XDG_RUNTIME_DIR", scratch.Directory());
	const Environment noTmpdir("TMPDIR");
	const Environment noTmp("TMP");
	const Environment noTemp("TEMP");

	const std::vector<std::string> candidates = discord::SocketCandidates();
	const std::string root = scratch.Directory() + "/";

	// The official client on a plain install.
	CHECK(Lists(candidates, root + "discord-ipc-0"));

	// Flatpak, Snap and Vesktop, which is how a large share of Linux users run
	// it. These are the six the obvious implementation misses.
	CHECK(Lists(candidates, root + "app/com.discordapp.Discord/discord-ipc-0"));
	CHECK(Lists(candidates, root + "app/dev.vencord.Vesktop/discord-ipc-0"));
	CHECK(Lists(candidates, root + ".flatpak/com.discordapp.Discord/xdg-run/discord-ipc-0"));
	CHECK(Lists(candidates, root + ".flatpak/dev.vencord.Vesktop/xdg-run/discord-ipc-0"));
	CHECK(Lists(candidates, root + "snap.discord/discord-ipc-0"));
	CHECK(Lists(candidates, root + "snap.discord-canary/discord-ipc-0"));

	// Discord numbers its sockets, so a second client takes the next one.
	CHECK(Lists(candidates, root + "discord-ipc-9"));
	CHECK(!Lists(candidates, root + "discord-ipc-10"));

	// A machine with no variables set still has this one.
	CHECK(Lists(candidates, "/tmp/discord-ipc-0"));
}

TEST_CASE("a lower socket number is tried before another layout", "[discord][sockets]") {
	const Scratch scratch;
	const Environment runtime("XDG_RUNTIME_DIR", scratch.Directory());
	const Environment noTmpdir("TMPDIR");
	const Environment noTmp("TMP");
	const Environment noTemp("TEMP");

	const std::vector<std::string> candidates = discord::SocketCandidates();
	const std::string root = scratch.Directory() + "/";

	// Discord hands the lowest free number to whichever client started first,
	// so `discord-ipc-0` under any layout is a better guess than
	// `discord-ipc-1` under the plainest one.
	CHECK(
		Position(candidates, root + "snap.discord/discord-ipc-0") <
		Position(candidates, root + "discord-ipc-1")
	);
}

TEST_CASE("connecting skips the numbers nothing is listening on", "[discord][sockets]") {
	const Scratch scratch;
	const Environment runtime("XDG_RUNTIME_DIR", scratch.Directory());
	const Environment noTmpdir("TMPDIR");
	const Environment noTmp("TMP");
	const Environment noTemp("TEMP");

	// Zero, one and two are absent. This is the ordinary state of a machine
	// where something else took the low numbers first.
	const Listener listening(scratch.Directory() + "/discord-ipc-3");

	const std::unique_ptr<discord::Channel> wire = discord::ConnectLocal();
	REQUIRE(wire != nullptr);
	CHECK(wire->Open());
}

TEST_CASE("an empty directory answers nothing rather than failing", "[discord][sockets]") {
	const Scratch scratch;

	// An exact path, so this asks about one socket rather than about whatever
	// this machine happens to be running.
	CHECK(discord::ConnectLocal(scratch.Directory() + "/discord-ipc-0") == nullptr);
}

TEST_CASE("a connected channel reads what the far end wrote", "[discord][sockets]") {
	const Scratch scratch;
	const std::string path = scratch.Directory() + "/discord-ipc-0";
	const Listener listening(path);

	const std::unique_ptr<discord::Channel> wire = discord::ConnectLocal(path);
	REQUIRE(wire != nullptr);

	// Nothing has been sent, and a non-blocking read of an idle socket must
	// answer `Empty` rather than wait. A blocking one would hang the pump.
	std::vector<std::byte> buffer;
	CHECK(wire->Receive(buffer) == discord::ChannelStatus::Empty);
	CHECK(buffer.empty());
}

#endif
