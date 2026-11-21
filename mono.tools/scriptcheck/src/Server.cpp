#include <cctype>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <scriptcheck/Server.hpp>

namespace scriptcheck {

	namespace {
		using Json = nlohmann::json;

		// LSP's `DiagnosticSeverity.Error` and `CompletionItemKind` values.
		constexpr int SEVERITY_ERROR = 1;

		int CompletionKind(const std::string &kind) {
			if (kind == "keyword") return 14;
			if (kind == "string") return 1;
			if (kind == "type") return 7;
			return 10;
		}

		Json Range(int line, int column, int endLine, int endColumn) {
			return {
				{"start", {{"line", line - 1}, {"character", column - 1}}},
				{"end", {{"line", endLine - 1}, {"character", endColumn - 1}}},
			};
		}

		std::string Response(const Json &id, Json result) {
			return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump();
		}

		std::string Failure(const Json &id, int code, const std::string &message) {
			return Json{
				{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}
			}.dump();
		}
	}

	Server::Server(Checker &checker) : Check(checker) {}

	std::vector<std::string> Server::Handle(std::string_view message) {
		std::vector<std::string> out;
		const Json request = Json::parse(message, nullptr, false);
		if (request.is_discarded() || !request.is_object()) {
			out.push_back(Failure(nullptr, -32700, "not JSON"));
			return out;
		}

		const std::string method = request.value("method", "");
		const Json id = request.contains("id") ? request["id"] : Json();
		const Json params = request.value("params", Json::object());

		// Every open document again, because a change to one module can fix or
		// break the modules that require it.
		const auto publish = [&] {
			for (const std::string &file : Open) {
				Json diagnostics = Json::array();
				for (const Diagnostic &diagnostic : Check.Check(file)) {
					diagnostics.push_back({
						{"range",
						 Range(diagnostic.Line, diagnostic.Column, diagnostic.EndLine, diagnostic.EndColumn)},
						{"severity", SEVERITY_ERROR},
						{"source", "scriptcheck"},
						{"code", diagnostic.Code},
						{"message", diagnostic.Message},
					});
				}
				out.push_back(
					Json{
						{"jsonrpc", "2.0"},
						{"method", "textDocument/publishDiagnostics"},
						{"params", {{"uri", UriOfPath(file)}, {"diagnostics", std::move(diagnostics)}}},
					}
						.dump()
				);
			}
		};

		const auto document = [&]() -> std::string {
			return PathOfUri(params.value("textDocument", Json::object()).value("uri", ""));
		};

		const auto position = [&](int &line, int &column) {
			const Json at = params.value("position", Json::object());
			line = at.value("line", 0) + 1;
			column = at.value("character", 0) + 1;
		};

		if (method == "initialize") {
			std::string root;
			if (params.contains("rootUri") && params["rootUri"].is_string()) {
				root = PathOfUri(params["rootUri"].get<std::string>());
			} else if (params.contains("rootPath") && params["rootPath"].is_string()) {
				root = params["rootPath"].get<std::string>();
			}
			if (!root.empty()) {
				Check.SetRoots(Discover(root).Roots);
			}
			out.push_back(Response(
				id,
				{
					{"capabilities",
					 {
						 {"textDocumentSync", {{"openClose", true}, {"change", 1}, {"save", true}}},
						 {"hoverProvider", true},
						 {"completionProvider", {{"triggerCharacters", {".", ":"}}}},
					 }},
					{"serverInfo", {{"name", "scriptcheck"}}},
				}
			));
		} else if (method == "textDocument/didOpen") {
			const std::string file = document();
			Check.SetText(file, params["textDocument"].value("text", ""));
			Open.insert(file);
			publish();
		} else if (method == "textDocument/didChange") {
			const std::string file = document();
			const Json changes = params.value("contentChanges", Json::array());
			if (!changes.empty()) {
				Check.SetText(file, changes.back().value("text", ""));
			}
			publish();
		} else if (method == "textDocument/didSave") {
			const std::string file = document();
			if (params.contains("text") && params["text"].is_string()) {
				Check.SetText(file, params["text"].get<std::string>());
			}
			publish();
		} else if (method == "textDocument/didClose") {
			const std::string file = document();
			Check.Forget(file);
			Open.erase(file);
			out.push_back(
				Json{
					{"jsonrpc", "2.0"},
					{"method", "textDocument/publishDiagnostics"},
					{"params", {{"uri", UriOfPath(file)}, {"diagnostics", Json::array()}}},
				}
					.dump()
			);
			publish();
		} else if (method == "textDocument/hover") {
			int line = 1;
			int column = 1;
			position(line, column);
			const std::optional<std::string> type = Check.TypeAt(document(), line, column);
			out.push_back(Response(
				id,
				type ? Json{{"contents", {{"kind", "markdown"}, {"value", "```luau\n" + *type + "\n```"}}}}
					 : Json()
			));
		} else if (method == "textDocument/completion") {
			int line = 1;
			int column = 1;
			position(line, column);
			Json items = Json::array();
			for (const Completion &completion : Check.Complete(document(), line, column)) {
				items.push_back({
					{"label", completion.Label},
					{"kind", CompletionKind(completion.Kind)},
					{"detail", completion.Detail},
				});
			}
			out.push_back(Response(id, {{"isIncomplete", false}, {"items", std::move(items)}}));
		} else if (method == "shutdown") {
			ShutDown = true;
			out.push_back(Response(id, nullptr));
		} else if (method == "exit") {
			Exited = true;
		} else if (!id.is_null()) {
			// A request nobody handles still gets an answer, or the client
			// waits forever. Notifications (`initialized`, `$/...`) need none.
			out.push_back(Failure(id, -32601, "scriptcheck does not handle " + method));
		}
		return out;
	}

	int Server::Run(std::istream &in, std::ostream &out) {
		while (!Exited) {
			const std::optional<std::string> message = ReadMessage(in);
			if (!message) {
				break;
			}
			for (const std::string &reply : Handle(*message)) {
				WriteMessage(out, reply);
			}
		}
		return ShutDown ? 0 : 1;
	}

	std::optional<std::string> ReadMessage(std::istream &in) {
		size_t length = 0;
		bool sized = false;
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				if (sized) {
					break;
				}
				continue;
			}
			constexpr std::string_view HEADER = "Content-Length:";
			if (line.starts_with(HEADER)) {
				length = std::stoul(line.substr(HEADER.size()));
				sized = true;
			}
		}
		if (!sized) {
			return std::nullopt;
		}
		std::string body(length, '\0');
		if (!in.read(body.data(), static_cast<std::streamsize>(length))) {
			return std::nullopt;
		}
		return body;
	}

	void WriteMessage(std::ostream &out, std::string_view body) {
		out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
		out.flush();
	}

	std::string PathOfUri(std::string_view uri) {
		constexpr std::string_view SCHEME = "file://";
		if (uri.starts_with(SCHEME)) {
			uri.remove_prefix(SCHEME.size());
		}
		std::string path;
		for (size_t index = 0; index < uri.size(); index++) {
			if (uri[index] == '%' && index + 2 < uri.size() && std::isxdigit(uri[index + 1]) &&
				std::isxdigit(uri[index + 2])) {
				path.push_back(
					static_cast<char>(std::stoi(std::string(uri.substr(index + 1, 2)), nullptr, 16))
				);
				index += 2;
			} else {
				path.push_back(uri[index]);
			}
		}
		// `/C:/x` on Windows is `C:/x`.
		if (path.size() > 2 && path[0] == '/' && path[2] == ':') {
			path.erase(0, 1);
		}
		return path;
	}

	std::string UriOfPath(const std::filesystem::path &path) {
		const std::string text = std::filesystem::absolute(path).lexically_normal().generic_string();
		std::string uri = text.starts_with('/') ? "file://" : "file:///";
		for (const unsigned char c : text) {
			if (std::isalnum(c) ||
				std::string_view("/-_.~:").find(static_cast<char>(c)) != std::string_view::npos) {
				uri.push_back(static_cast<char>(c));
			} else {
				constexpr char HEX[] = "0123456789ABCDEF";
				uri += '%';
				uri += HEX[c >> 4];
				uri += HEX[c & 15];
			}
		}
		return uri;
	}
}
