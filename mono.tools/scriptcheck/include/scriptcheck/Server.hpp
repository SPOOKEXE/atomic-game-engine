#pragma once

// `scriptcheck --serve`: a Language Server Protocol subset over stdio.
//
// Keeps one `Checker` warm so an editor or a plugin's own tooling pays for the
// declarations once. Handles `initialize` (its root folder becomes the require
// roots, found the way `Discover` finds them), `textDocument/didOpen`,
// `didChange` (full text), `didSave` and `didClose` (each answered with
// `textDocument/publishDiagnostics` per file holding errors from any open
// document or its requires, and an empty list for a file that has just become
// clean, since an edit to one module can change another's errors),
// `textDocument/hover`,
// `textDocument/completion`, `shutdown` and `exit`.

#include <iosfwd>
#include <optional>
#include <scriptcheck/Check.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace scriptcheck {

	class Server {
	  public:
		explicit Server(Checker &checker);

		// One JSON-RPC message in, the messages to send back out (a response
		// and any notifications), in order.
		std::vector<std::string> Handle(std::string_view message);

		// True once `exit` has arrived.
		bool Done() const {
			return Exited;
		}

		// Runs until `exit` or end of input. Returns 0 when `shutdown` came
		// first, 1 otherwise, which is what the protocol asks of a server.
		int Run(std::istream &in, std::ostream &out);

	  private:
		Checker &Check;
		std::set<std::string> Open;
		// Files last sent a non-empty list, so a clean one can be cleared.
		std::set<std::string> Published;
		bool ShutDown = false;
		bool Exited = false;
	};

	// LSP framing: a `Content-Length` header, a blank line, then the body.
	std::optional<std::string> ReadMessage(std::istream &in);
	void WriteMessage(std::ostream &out, std::string_view body);

	// `file:///a%20b/c.luau` -> `/a b/c.luau`, and back.
	std::string PathOfUri(std::string_view uri);
	std::string UriOfPath(const std::filesystem::path &path);
}
