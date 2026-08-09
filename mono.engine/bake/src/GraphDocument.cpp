#include <engine/bake/GraphDocument.hpp>

#include <array>
#include <charconv>
#include <string>
#include <vector>

namespace engine::bake {

	namespace {
		// The header every document starts with, and the version beside it.
		//
		// **A version from the first release rather than added at the second.**
		// A format that ships without one can never be extended without
		// guessing what an unlabelled file is.
		constexpr std::string_view HEADER = "bakegraph 1";

		// Whether an operation adds a node, which is what positions count.
		bool AddsNode(OperationKind kind) {
			return kind != OperationKind::Connect;
		}

		// Whether a node kind carries no parameter, and so may travel as a bare
		// `AddNode`.
		//
		// **A closed list rather than the complement of the parameterised
		// ones.** A node kind added to `Graph` later arrives here as "not
		// allowed" and has to be thought about, rather than silently becoming
		// legal with its parameter dropped.
		bool IsBare(NodeKind kind) {
			return kind == NodeKind::Import || kind == NodeKind::Smooth || kind == NodeKind::Opaque;
		}

		// The text spelling of a bare node kind.
		std::string_view NodeText(NodeKind kind) {
			switch (kind) {
			case NodeKind::Import:
				return "import";
			case NodeKind::Smooth:
				return "smooth";
			case NodeKind::Opaque:
				return "opaque";
			default:
				return "";
			}
		}

		bool NodeFromText(std::string_view text, NodeKind &kind) {
			if (text == "import") {
				kind = NodeKind::Import;
				return true;
			}
			if (text == "smooth") {
				kind = NodeKind::Smooth;
				return true;
			}
			if (text == "opaque") {
				kind = NodeKind::Opaque;
				return true;
			}
			return false;
		}

		// Quotes and escapes a name.
		//
		// **The escape set is the characters that could forge structure**, not a
		// general string escape: a name holding a newline could otherwise write
		// a second operation into the file, and one holding a quote could end
		// its own field early. Everything else, including any UTF-8, travels as
		// itself.
		void AppendQuoted(std::string &out, std::string_view text) {
			out.push_back('"');
			for (const char character : text) {
				switch (character) {
				case '\\':
					out += "\\\\";
					break;
				case '"':
					out += "\\\"";
					break;
				case '\n':
					out += "\\n";
					break;
				case '\r':
					out += "\\r";
					break;
				case '\t':
					out += "\\t";
					break;
				default:
					out.push_back(character);
					break;
				}
			}
			out.push_back('"');
		}

		// Reads a quoted name from `line`, advancing past it.
		bool TakeQuoted(std::string_view &line, std::string &out) {
			while (!line.empty() && line.front() == ' ') {
				line.remove_prefix(1);
			}
			if (line.empty() || line.front() != '"') {
				return false;
			}
			line.remove_prefix(1);

			out.clear();
			while (!line.empty()) {
				const char character = line.front();
				line.remove_prefix(1);

				if (character == '"') {
					return true;
				}
				if (character != '\\') {
					out.push_back(character);
					continue;
				}
				if (line.empty()) {
					return false;
				}

				const char escaped = line.front();
				line.remove_prefix(1);
				switch (escaped) {
				case '\\':
					out.push_back('\\');
					break;
				case '"':
					out.push_back('"');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				default:
					// An escape nothing writes. Refused rather than passed
					// through, so the format has one spelling per name and
					// `Read(Write(d))` cannot be ambiguous.
					return false;
				}
			}

			// Ran off the end with the quote still open.
			return false;
		}

		std::string_view TakeWord(std::string_view &line) {
			while (!line.empty() && line.front() == ' ') {
				line.remove_prefix(1);
			}
			const size_t end = line.find(' ');
			const std::string_view word = line.substr(0, end);
			line.remove_prefix(end == std::string_view::npos ? line.size() : end);
			return word;
		}

		bool TakeFloat(std::string_view &line, float &out) {
			const std::string_view word = TakeWord(line);
			if (word.empty()) {
				return false;
			}
			const char *first = word.data();
			const char *last = first + word.size();
			const auto result = std::from_chars(first, last, out);
			return result.ec == std::errc{} && result.ptr == last;
		}

		bool TakeUnsigned(std::string_view &line, uint32_t &out) {
			const std::string_view word = TakeWord(line);
			if (word.empty()) {
				return false;
			}
			const char *first = word.data();
			const char *last = first + word.size();
			const auto result = std::from_chars(first, last, out);
			return result.ec == std::errc{} && result.ptr == last;
		}

		// **Written through `std::to_chars`, which round trips.** The shortest
		// text that reads back as the same float, so a scale of a third saved
		// and reloaded is bit-identical rather than nearly so — and a bake is
		// supposed to be reproducible.
		void AppendFloat(std::string &out, float value) {
			std::array<char, 32> buffer{};
			const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			out.append(buffer.data(), result.ptr);
		}
	}

	const char *Describe(OperationKind kind) {
		switch (kind) {
		case OperationKind::AddSource:
			return "source";
		case OperationKind::AddBuiltin:
			return "builtin";
		case OperationKind::AddNode:
			return "node";
		case OperationKind::AddFit:
			return "fit";
		case OperationKind::AddScale:
			return "scale";
		case OperationKind::AddResize:
			return "resize";
		case OperationKind::AddRetime:
			return "retime";
		case OperationKind::AddWrite:
			return "write";
		case OperationKind::Connect:
			return "connect";
		}
		return "unknown";
	}

	const char *Describe(DocumentStatus status) {
		switch (status) {
		case DocumentStatus::Ok:
			return "ok";
		case DocumentStatus::UnknownNode:
			return "an operation names a node this document does not hold";
		case DocumentStatus::WrongNodeKind:
			return "that node kind has its own operation";
		case DocumentStatus::Refused:
			return "the graph refused the edit";
		case DocumentStatus::Malformed:
			return "not a bake graph document";
		case DocumentStatus::TooManyOperations:
			return "past the node limit";
		}
		return "unknown";
	}

	void Document::Record(Operation operation) {
		Edits.push_back(std::move(operation));
	}

	bool Document::Undo() {
		if (Edits.empty()) {
			return false;
		}
		Edits.pop_back();
		return true;
	}

	size_t Document::NodeCount() const {
		size_t count = 0;
		for (const Operation &operation : Edits) {
			count += AddsNode(operation.Kind) ? 1u : 0u;
		}
		return count;
	}

	void Document::Clear() {
		Edits.clear();
	}

	DocumentStatus
	Build(const Document &document, Graph &graph, const SourceResolver &sources, std::string &offender) {
		// **Counted before anything is built**, so a generated document cannot
		// walk a graph up to its limit and fail on the last node with several
		// thousand allocations already made.
		if (document.NodeCount() > Graph::MAXIMUM_NODES) {
			return DocumentStatus::TooManyOperations;
		}

		// Node handles by document position, so a `Connect` can name what an
		// earlier operation produced without either side knowing what the
		// runtime issued.
		std::vector<NodeId> nodes;
		nodes.reserve(document.NodeCount());

		size_t index = 0;
		for (const Operation &operation : document.Operations()) {
			index++;

			// Named the same way in every diagnostic below: the operation's
			// position, then what it was trying to do.
			const auto describe = [&] {
				return std::to_string(index) + " (" + Describe(operation.Kind) + ")";
			};

			if (operation.Kind == OperationKind::Connect) {
				if (operation.From == 0 || operation.From > nodes.size() || operation.To == 0 ||
					operation.To > nodes.size()) {
					offender = describe();
					return DocumentStatus::UnknownNode;
				}
				if (!graph.Connect(nodes[operation.From - 1], nodes[operation.To - 1])) {
					offender = describe();
					return DocumentStatus::Refused;
				}
				continue;
			}

			NodeId added;
			switch (operation.Kind) {
			case OperationKind::AddSource: {
				// **An absent resolver is the same event as an unknown name.**
				// Both mean the bytes are not available, and a document holding
				// a source is not buildable without them either way.
				const std::span<const std::byte> bytes =
					sources ? sources(operation.Text) : std::span<const std::byte>{};
				if (bytes.empty()) {
					offender = operation.Text;
					return DocumentStatus::Refused;
				}
				added = graph.AddSource(operation.Text, bytes);
				break;
			}
			case OperationKind::AddBuiltin:
				added = graph.AddBuiltin(operation.Text);
				break;
			case OperationKind::AddNode:
				if (!IsBare(operation.Node)) {
					offender = describe();
					return DocumentStatus::WrongNodeKind;
				}
				added = graph.Add(operation.Node);
				break;
			case OperationKind::AddFit:
				added = graph.AddFit(operation.Number);
				break;
			case OperationKind::AddScale:
				added = graph.AddScale(operation.Amount);
				break;
			case OperationKind::AddResize:
				added = graph.AddResize(operation.Width, operation.Height);
				break;
			case OperationKind::AddRetime:
				added = graph.AddRetime(operation.Number);
				break;
			case OperationKind::AddWrite:
				added = graph.AddWrite(operation.Text);
				break;
			case OperationKind::Connect:
				break;
			}

			if (!added.IsValid()) {
				offender = operation.Text.empty() ? describe() : operation.Text;
				return DocumentStatus::Refused;
			}

			nodes.push_back(added);
		}

		return DocumentStatus::Ok;
	}

	std::string Write(const Document &document) {
		std::string out;
		out += HEADER;
		out.push_back('\n');

		for (const Operation &operation : document.Operations()) {
			out += Describe(operation.Kind);

			switch (operation.Kind) {
			case OperationKind::AddSource:
			case OperationKind::AddBuiltin:
			case OperationKind::AddWrite:
				out.push_back(' ');
				AppendQuoted(out, operation.Text);
				break;
			case OperationKind::AddNode:
				out.push_back(' ');
				out += NodeText(operation.Node);
				break;
			case OperationKind::AddFit:
			case OperationKind::AddRetime:
				out.push_back(' ');
				AppendFloat(out, operation.Number);
				break;
			case OperationKind::AddScale:
				out.push_back(' ');
				AppendFloat(out, operation.Amount.X);
				out.push_back(' ');
				AppendFloat(out, operation.Amount.Y);
				out.push_back(' ');
				AppendFloat(out, operation.Amount.Z);
				break;
			case OperationKind::AddResize:
				out += ' ' + std::to_string(operation.Width) + ' ' + std::to_string(operation.Height);
				break;
			case OperationKind::Connect:
				out += ' ' + std::to_string(operation.From) + ' ' + std::to_string(operation.To);
				break;
			}

			out.push_back('\n');
		}

		return out;
	}

	DocumentStatus Read(std::string_view text, Document &document, std::string &offender) {
		document.Clear();

		const auto nextLine = [&text]() -> std::string_view {
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

			// Tolerated so a document written on one platform reads on another;
			// nothing here ever writes one.
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			return line;
		};

		if (text.empty() || nextLine() != HEADER) {
			offender = std::string(HEADER);
			return DocumentStatus::Malformed;
		}

		while (!text.empty()) {
			const std::string_view whole = nextLine();
			if (whole.empty()) {
				continue;
			}

			std::string_view line = whole;
			const std::string_view word = TakeWord(line);

			Operation operation;
			bool parsed = true;

			if (word == "source" || word == "builtin" || word == "write") {
				operation.Kind = word == "source"	 ? OperationKind::AddSource
								 : word == "builtin" ? OperationKind::AddBuiltin
													 : OperationKind::AddWrite;
				parsed = TakeQuoted(line, operation.Text);
			} else if (word == "node") {
				operation.Kind = OperationKind::AddNode;
				parsed = NodeFromText(TakeWord(line), operation.Node);
			} else if (word == "fit" || word == "retime") {
				operation.Kind = word == "fit" ? OperationKind::AddFit : OperationKind::AddRetime;
				parsed = TakeFloat(line, operation.Number);
			} else if (word == "scale") {
				operation.Kind = OperationKind::AddScale;
				parsed = TakeFloat(line, operation.Amount.X) && TakeFloat(line, operation.Amount.Y) &&
						 TakeFloat(line, operation.Amount.Z);
			} else if (word == "resize") {
				operation.Kind = OperationKind::AddResize;
				parsed = TakeUnsigned(line, operation.Width) && TakeUnsigned(line, operation.Height);
			} else if (word == "connect") {
				operation.Kind = OperationKind::Connect;
				parsed = TakeUnsigned(line, operation.From) && TakeUnsigned(line, operation.To);
			} else {
				parsed = false;
			}

			// **Trailing text is a refusal, not something to ignore.** A line
			// with a fourth number on a `resize` is somebody's misunderstanding
			// of the format, and accepting it silently would bake something
			// other than what they wrote.
			if (parsed && !TakeWord(line).empty()) {
				parsed = false;
			}

			if (!parsed) {
				offender = std::string(whole);
				return DocumentStatus::Malformed;
			}

			document.Record(std::move(operation));
		}

		return DocumentStatus::Ok;
	}
}
