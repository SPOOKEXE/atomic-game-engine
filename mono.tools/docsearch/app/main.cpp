// Documentation search MCP server.
//
// Reads built HTML documentation and serves full-text search via MCP over stdio.
// MCP clients start this as a subprocess and speak newline-delimited JSON-RPC.
//
// Tools provided:
//   doc_search   - Search documentation by query string
//   doc_get      - Get a specific documentation page by path
//   doc_list     - List available documentation pages
//
// Point a client at it:
//
//     "docsearch": { "command": ".cache/build/dev/tools/docsearch",
//                     "args": ["--docs", ".cache/build/dev/docs/html"] }

#include <engine/core/Arguments.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

	// Strip HTML tags and decode common entities for search indexing.
	std::string StripHtml(const std::string &html) {
		std::string result;
		result.reserve(html.size());
		bool in_tag = false;
		bool in_script = false;
		bool in_style = false;

		for (size_t i = 0; i < html.size(); ++i) {
			char c = html[i];

			if (in_tag) {
				if (c == '>') {
					in_tag = false;
					// Check if we just closed a script or style tag
					std::string tag_lower = result.substr(result.rfind('<'));
					std::transform(tag_lower.begin(), tag_lower.end(), tag_lower.begin(), ::tolower);
					if (tag_lower.find("<script") != std::string::npos)
						in_script = true;
					else if (tag_lower.find("</script") != std::string::npos)
						in_script = false;
					else if (tag_lower.find("<style") != std::string::npos)
						in_style = true;
					else if (tag_lower.find("</style") != std::string::npos)
						in_style = false;
				}
				continue;
			}

			if (in_script || in_style) {
				if (c == '<') {
					// Look ahead for closing tag
					std::string peek(html.substr(i, 10));
					std::transform(peek.begin(), peek.end(), peek.begin(), ::tolower);
					if (peek.find("</script") == 0 || peek.find("</style") == 0) {
						in_tag = true;
						result += ' ';
					}
				}
				continue;
			}

			if (c == '<') {
				in_tag = true;
				result += ' ';
				continue;
			}

			// Decode common HTML entities
			if (c == '&') {
				std::string entity(html.substr(i, std::min(size_t(10), html.size() - i)));
				size_t semi = entity.find(';');
				if (semi != std::string::npos) {
					entity = entity.substr(0, semi + 1);
					if (entity == "&amp;") {
						result += '&';
						i += entity.size() - 1;
						continue;
					}
					if (entity == "&lt;") {
						result += '<';
						i += entity.size() - 1;
						continue;
					}
					if (entity == "&gt;") {
						result += '>';
						i += entity.size() - 1;
						continue;
					}
					if (entity == "&quot;") {
						result += '"';
						i += entity.size() - 1;
						continue;
					}
					if (entity == "&#39;") {
						result += '\'';
						i += entity.size() - 1;
						continue;
					}
					if (entity == "&nbsp;") {
						result += ' ';
						i += entity.size() - 1;
						continue;
					}
				}
			}

			result += c;
		}
		return result;
	}

	// Normalize text for search: lowercase and collapse whitespace.
	std::string Normalize(const std::string &text) {
		std::string result;
		result.reserve(text.size());
		bool prev_space = false;
		for (char c : text) {
			if (std::isspace(static_cast<unsigned char>(c))) {
				if (!prev_space) {
					result += ' ';
					prev_space = true;
				}
			} else {
				result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				prev_space = false;
			}
		}
		return result;
	}

	// Split a query into terms.
	std::vector<std::string> Tokenize(const std::string &query) {
		std::vector<std::string> tokens;
		std::istringstream stream(query);
		std::string token;
		while (stream >> token) {
			// Skip very short tokens
			if (token.size() >= 2) {
				tokens.push_back(Normalize(token));
			}
		}
		return tokens;
	}

	struct DocEntry {
		std::string path;		// Relative path from docs root
		std::string title;		// Page title extracted from HTML
		std::string content;	// Raw HTML content
		std::string normalized; // Normalized text for search
	};

	class DocIndex {
	  public:
		bool Load(const std::filesystem::path &docs_dir) {
			if (!std::filesystem::exists(docs_dir)) {
				std::cerr << "docsearch: " << docs_dir << " does not exist\n";
				return false;
			}

			for (const auto &entry : std::filesystem::recursive_directory_iterator(docs_dir)) {
				if (!entry.is_regular_file()) continue;
				if (entry.path().extension() != ".html") continue;

				std::ifstream file(entry.path());
				if (!file) continue;

				std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

				DocEntry doc;
				doc.path = std::filesystem::relative(entry.path(), docs_dir).string();
				doc.content = std::move(html);

				// Extract title from <title> tag
				auto title_start = doc.content.find("<title>");
				auto title_end = doc.content.find("</title>");
				if (title_start != std::string::npos && title_end != std::string::npos) {
					doc.title = doc.content.substr(title_start + 7, title_end - title_start - 7);
				} else {
					doc.title = doc.path;
				}

				doc.normalized = Normalize(StripHtml(doc.content));
				docs_.push_back(std::move(doc));
			}

			std::cerr << "docsearch: indexed " << docs_.size() << " pages from " << docs_dir << "\n";
			return true;
		}

		json Search(const std::string &query, size_t limit = 10) const {
			auto terms = Tokenize(query);
			if (terms.empty()) {
				return json::array();
			}

			// Score each document by term frequency
			std::vector<std::pair<size_t, double>> scored;
			scored.reserve(docs_.size());

			for (size_t i = 0; i < docs_.size(); ++i) {
				double score = 0.0;
				for (const auto &term : terms) {
					// Count occurrences
					size_t pos = 0;
					size_t count = 0;
					while ((pos = docs_[i].normalized.find(term, pos)) != std::string::npos) {
						++count;
						++pos;
					}
					// TF score: log-normalized frequency
					if (count > 0) {
						score += 1.0 + std::log(static_cast<double>(count));
					}
				}
				if (score > 0.0) {
					scored.push_back({i, score});
				}
			}

			// Sort by score descending
			std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) {
				return a.second > b.second;
			});

			// Build results
			json results = json::array();
			size_t count = std::min(limit, scored.size());
			for (size_t i = 0; i < count; ++i) {
				const auto &doc = docs_[scored[i].first];
				// Extract a snippet around the first term match
				std::string snippet = ExtractSnippet(doc.normalized, terms[0]);
				results.push_back(
					{{"path", doc.path},
					 {"title", doc.title},
					 {"score", scored[i].second},
					 {"snippet", snippet}}
				);
			}
			return results;
		}

		json ListPages() const {
			json pages = json::array();
			for (const auto &doc : docs_) {
				pages.push_back({{"path", doc.path}, {"title", doc.title}});
			}
			return pages;
		}

		std::optional<std::string> GetPage(const std::string &path) const {
			for (const auto &doc : docs_) {
				if (doc.path == path) {
					return doc.content;
				}
			}
			return std::nullopt;
		}

	  private:
		std::string ExtractSnippet(const std::string &normalized, const std::string &term) const {
			size_t pos = normalized.find(term);
			if (pos == std::string::npos) {
				// Return beginning of content
				size_t len = std::min(size_t(200), normalized.size());
				return normalized.substr(0, len) + "...";
			}
			// Show context around the match
			size_t start = (pos > 60) ? pos - 60 : 0;
			size_t end = std::min(pos + 140, normalized.size());
			std::string snippet;
			if (start > 0) snippet += "...";
			snippet += normalized.substr(start, end - start);
			if (end < normalized.size()) snippet += "...";
			return snippet;
		}

		std::vector<DocEntry> docs_;
	};

	// MCP JSON-RPC handling
	json HandleRequest(const DocIndex &index, const json &request) {
		json response;
		response["jsonrpc"] = "2.0";

		if (request.contains("id")) {
			response["id"] = request["id"];
		}

		if (!request.contains("method")) {
			response["error"] = {{"code", -32600}, {"message", "Missing method"}};
			return response;
		}

		std::string method = request["method"].get<std::string>();

		if (method == "initialize") {
			response["result"] = {
				{"protocolVersion", "2024-11-05"},
				{"capabilities", {{"tools", json::object()}}},
				{"serverInfo", {{"name", "docsearch"}, {"version", "1.0.0"}}}
			};
		} else if (method == "tools/list") {
			response["result"] = {
				{"tools",
				 json::array(
					 {{{"name", "doc_search"},
					   {"description", "Search engine documentation by query string"},
					   {"inputSchema",
						{{"type", "object"},
						 {"properties",
						  {{"query", {{"type", "string"}, {"description", "Search query"}}},
						   {"limit", {{"type", "integer"}, {"description", "Max results (default 10)"}}}}},
						 {"required", json::array({"query"})}}}},
					  {{"name", "doc_get"},
					   {"description", "Get a specific documentation page by path"},
					   {"inputSchema",
						{{"type", "object"},
						 {"properties",
						  {{"path",
							{{"type", "string"}, {"description", "Page path relative to docs root"}}}}},
						 {"required", json::array({"path"})}}}},
					  {{"name", "doc_list"},
					   {"description", "List all available documentation pages"},
					   {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}}}
				 )}
			};
		} else if (method == "tools/call") {
			if (!request.contains("params") || !request["params"].contains("arguments")) {
				response["error"] = {{"code", -32602}, {"message", "Missing arguments"}};
				return response;
			}

			const auto &args = request["params"]["arguments"];
			std::string tool_name = request["params"].value("name", "");

			if (tool_name == "doc_search") {
				std::string query = args.value("query", "");
				size_t limit = args.value("limit", size_t(10));
				auto results = index.Search(query, limit);
				response["result"] = {
					{"content", json::array({{{"type", "text"}, {"text", results.dump(2)}}})}
				};
			} else if (tool_name == "doc_get") {
				std::string path = args.value("path", "");
				auto page = index.GetPage(path);
				if (page) {
					response["result"] = {{"content", json::array({{{"type", "text"}, {"text", *page}}})}};
				} else {
					response["error"] = {{"code", -32602}, {"message", "Page not found: " + path}};
				}
			} else if (tool_name == "doc_list") {
				auto pages = index.ListPages();
				response["result"] = {
					{"content", json::array({{{"type", "text"}, {"text", pages.dump(2)}}})}
				};
			} else {
				response["error"] = {{"code", -32601}, {"message", "Unknown tool: " + tool_name}};
			}
		} else if (method == "notifications/initialized") {
			// Notification, no response needed
			return json(nullptr);
		} else {
			response["error"] = {{"code", -32601}, {"message", "Unknown method: " + method}};
		}

		return response;
	}

} // namespace

int main(int argc, char **argv) {
	// No Log::Initialise - stdout is the protocol, stderr is logs.
	engine::core::Arguments arguments(
		"docsearch", "atomic - MCP server for documentation search over built HTML."
	);
	arguments.Value("docs", "DIR", "Path to built HTML documentation directory");
	arguments.Flag("verbose", "Log requests to stderr");

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

	std::string docs_path{arguments.Get("docs").value_or(".cache/build/dev/docs/html")};
	bool verbose = arguments.Has("verbose");

	DocIndex index;
	if (!index.Load(docs_path)) {
		return 1;
	}

	// Read newline-delimited JSON-RPC from stdin, write responses to stdout
	std::string line;
	while (std::getline(std::cin, line)) {
		if (line.empty()) continue;

		json request;
		try {
			request = json::parse(line);
		} catch (const json::parse_error &e) {
			std::cerr << "docsearch: parse error: " << e.what() << "\n";
			continue;
		}

		if (verbose) {
			std::cerr << "docsearch: " << request.value("method", "?") << "\n";
		}

		json response = HandleRequest(index, request);

		// Notifications don't get a response
		if (response.is_null()) continue;

		std::string response_str = response.dump();
		std::cout << response_str << "\n";
		std::cout.flush();
	}

	return 0;
}
