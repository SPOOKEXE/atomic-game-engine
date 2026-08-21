// Model Context Protocol on stdio, forwarded to a running editor.
//
// **A byte pump, and it understands nothing.** MCP clients start a server as a
// subprocess and speak newline-delimited JSON-RPC over its stdin and stdout.
// The editor cannot be that subprocess: it is a windowed program with a
// renderer and a universe, started by a person or by `just edit`, and outliving
// any one client. So the editor listens on loopback and this stands in front of
// it - everything a client writes goes to the socket, everything the socket
// says goes back out.
//
// Because it parses nothing, there is no second copy of the protocol to keep in
// step. A tool added to `engine/control/src/Tools.cpp` or to the editor's own
// `mono.studio/src/Control.cpp` is reachable through here the moment it exists,
// and this file never changes.
//
// **Two threads, because both directions block.** A single-threaded pump would
// have to poll one side while the other was mid-read, and the whole point of
// this process is that it adds no latency to a request/response protocol.
//
// Point a client at it:
//
//     "atomic": { "command": "…/tools/mcpbridge", "args": ["--port", "8730"] }
//
// It exits when either side closes, which is what makes a client's own
// lifecycle management work: killing the bridge does not touch the editor, and
// closing the editor ends the bridge.

#include <engine/core/Arguments.hpp>

#include <array>
#include <asio.hpp>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace {

	// Everything read from `from` is written to `to`, until either stops.
	//
	// **Unbuffered and unparsed.** A line-oriented copy would need a buffer big
	// enough for the largest message anything ever sends, and the largest thing
	// this carries is a world tree - a bound nobody can pick correctly. Bytes
	// have no such limit.
	void Pump(
		std::atomic<bool> &running,
		const std::function<size_t(char *, size_t)> &read,
		const std::function<bool(const char *, size_t)> &write
	) {
		std::array<char, 65536> buffer{};

		while (running.load()) {
			const size_t got = read(buffer.data(), buffer.size());
			if (got == 0) {
				break;
			}
			if (!write(buffer.data(), got)) {
				break;
			}
		}

		running.store(false);
	}
}

int main(int argc, char **argv) {
	// **No `Log::Initialise`, and that is not an omission.** stdout *is* the
	// protocol here: a log line written to it would arrive at the client as a
	// malformed JSON-RPC message and desynchronise the stream. Everything this
	// says goes to stderr, which MCP clients capture and show as server logs.
	engine::core::Arguments arguments(
		"mcpbridge", "atomic - carries Model Context Protocol between stdio and a running editor."
	);
	arguments.Value("port", "PORT", "The editor's --mcp-port (default 8730)");
	arguments.Value("host", "ADDRESS", "Where the editor is listening (default 127.0.0.1)");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::fprintf(stderr, "%s\n\n%s", parsed.Error.c_str(), arguments.Help().c_str());
		return 2;
	}
	if (parsed.VersionRequested) {
		std::fputs(arguments.VersionLine().c_str(), stdout);
		return 0;
	}
	if (parsed.HelpRequested) {
		std::fputs(arguments.Help().c_str(), stdout);
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::fputs(arguments.Describe().c_str(), stdout);
		return 0;
	}

	const auto port = static_cast<uint16_t>(arguments.GetInteger("port", 8730));
	const std::string host(arguments.Get("host").value_or("127.0.0.1"));

	asio::io_context context;
	asio::ip::tcp::socket socket(context);

	try {
		asio::ip::tcp::resolver resolver(context);
		asio::connect(socket, resolver.resolve(host, std::to_string(port)));
		socket.set_option(asio::ip::tcp::no_delay(true));
	} catch (const std::exception &failure) {
		// The common case by far, and worth saying precisely: the editor is not
		// running, or was started without the flag that opens the port.
		std::fprintf(
			stderr,
			"mcpbridge: could not reach an editor at %s:%u - %s\n"
			"Start one with: just edit --mcp-port %u\n",
			host.c_str(),
			port,
			failure.what(),
			port
		);
		return 1;
	}

	std::fprintf(stderr, "mcpbridge: connected to %s:%u\n", host.c_str(), port);

	std::atomic<bool> running{true};

	// stdin → socket
	std::thread inbound([&] {
		Pump(
			running,
			// One byte at a time, so the buffer's size is deliberately unused: a
			// blocking `fread` of more would sit on a partial line until the
			// peer sent enough to fill it, and this is a request/response pipe
			// where the sender is waiting for the reply.
			[](char *into, size_t) { return std::fread(into, 1, 1, stdin) == 1 ? 1u : 0u; },
			[&](const char *from, size_t size) {
				asio::error_code failed;
				asio::write(socket, asio::buffer(from, size), failed);
				return !failed;
			}
		);

		// **Half-closed, not closed, and the difference is every reply still in
		// flight.** Shutting both directions here threw away the answers to
		// whatever had just been sent - a client that writes its last request
		// and closes stdin got nothing back. Closing the sending half instead
		// tells the editor there are no more requests; it finishes the ones it
		// has, closes its end, and *that* is what ends the read below.
		asio::error_code ignored;
		socket.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
	});

	// socket → stdout, and this is the direction that decides when to stop.
	//
	// Its own flag, because the inbound pump clears `running` the moment stdin
	// ends - which is a normal part of shutting down and must not cut this one
	// short while the editor is still answering.
	std::atomic<bool> draining{true};
	Pump(
		draining,
		[&](char *into, size_t size) {
			asio::error_code failed;
			const size_t got = socket.read_some(asio::buffer(into, size), failed);
			return failed ? 0u : got;
		},
		[](const char *from, size_t size) {
			// **Flushed every time.** stdout to a pipe is block-buffered, and a
			// response sitting in a 4 KB buffer is a client waiting forever for
			// a reply that was already written.
			const bool wrote = std::fwrite(from, 1, size, stdout) == size;
			std::fflush(stdout);
			return wrote;
		}
	);

	running.store(false);

	// stdin is still blocked in fread. Detached rather than joined: there is no
	// portable way to cancel a blocking read on stdin, and the process is on its
	// way out - the alternative is hanging on exit for a keystroke that will
	// never come.
	inbound.detach();

	std::fprintf(stderr, "mcpbridge: closed\n");
	return 0;
}
