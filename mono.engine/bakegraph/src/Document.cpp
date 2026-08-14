#include <engine/bakegraph/Document.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <utility>
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

		// The text spelling of a bare node kind.
		std::string_view NodeText(NodeKind kind) {
			switch (kind) {
			case NodeKind::Import:
				return "import";
			case NodeKind::Smooth:
				return "smooth";
			case NodeKind::Opaque:
				return "opaque";
			case NodeKind::Mipmap:
				return "mipmap";
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
			if (text == "mipmap") {
				kind = NodeKind::Mipmap;
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
		case OperationKind::AddRasterize:
			return "rasterize";
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

	namespace {
		// One operation per line, and the only place a line is spelled.
		//
		// **Shared by the one-document format and the set**, which is what makes
		// the first a strict substring of the second. Two spellings of an
		// operation is the drift `IsBareNode` was made public to prevent, one
		// level up.
		void AppendOperations(std::string &out, const Document &document) {
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
				case OperationKind::AddRasterize:
					out += ' ' + std::to_string(operation.Width) + ' ' + std::to_string(operation.Height);
					break;
				case OperationKind::Connect:
					out += ' ' + std::to_string(operation.From) + ' ' + std::to_string(operation.To);
					break;
				}

				out.push_back('\n');
			}
		}

		// One operation from one line, and the only place a line is read.
		//
		// **A node kind this build does not have lands here as `false`**, which
		// is the whole of the unknown-kind story: `NodeFromText` knows a closed
		// list, so `node bevel` from a newer editor is refused exactly as a
		// misspelling is. The caller decides what a refusal costs — and for a
		// world document that is the pipeline rather than the world.
		//
		// @return `false` when the line is not an operation.
		bool ParseOperation(std::string_view whole, Operation &operation) {
			std::string_view line = whole;
			const std::string_view word = TakeWord(line);
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
			} else if (word == "resize" || word == "rasterize") {
				operation.Kind = word == "resize" ? OperationKind::AddResize : OperationKind::AddRasterize;
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
			return parsed && TakeWord(line).empty();
		}

		// Hands back one line at a time, consuming `text` as it goes.
		//
		// A `\r` is tolerated so a document written on one platform reads on
		// another; nothing here ever writes one.
		std::string_view NextLine(std::string_view &text) {
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);

			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			return line;
		}

		// The header, or `Malformed` naming it.
		bool TakeHeader(std::string_view &text, std::string &offender) {
			if (text.empty() || NextLine(text) != HEADER) {
				offender = std::string(HEADER);
				return false;
			}
			return true;
		}
	}

	std::string Write(const Document &document) {
		std::string out;
		out += HEADER;
		out.push_back('\n');
		AppendOperations(out, document);
		return out;
	}

	DocumentStatus Read(std::string_view text, Document &document, std::string &offender) {
		document.Clear();

		if (!TakeHeader(text, offender)) {
			return DocumentStatus::Malformed;
		}

		while (!text.empty()) {
			const std::string_view whole = NextLine(text);
			if (whole.empty()) {
				continue;
			}

			Operation operation;
			if (!ParseOperation(whole, operation)) {
				offender = std::string(whole);
				return DocumentStatus::Malformed;
			}

			document.Record(std::move(operation));
		}

		return DocumentStatus::Ok;
	}

	bool PipelineSet::Set(core::Name name, Document document) {
		if (!name.IsValid() || name.Text().empty()) {
			return false;
		}

		for (size_t index = 0; index < Order.size(); ++index) {
			if (Order[index] == name) {
				Documents[index] = std::move(document);
				return true;
			}
		}

		// Sorted on insert rather than on the way out, so `Names` stays a span
		// and the two vectors are never out of step for a reader.
		const auto at =
			std::lower_bound(Order.begin(), Order.end(), name, [](core::Name left, core::Name right) {
				return left.Text() < right.Text();
			});
		const size_t index = static_cast<size_t>(at - Order.begin());
		Order.insert(at, name);
		Documents.insert(Documents.begin() + static_cast<ptrdiff_t>(index), std::move(document));
		return true;
	}

	const Document *PipelineSet::Find(core::Name name) const {
		for (size_t index = 0; index < Order.size(); ++index) {
			if (Order[index] == name) {
				return &Documents[index];
			}
		}
		return nullptr;
	}

	bool PipelineSet::Remove(core::Name name) {
		for (size_t index = 0; index < Order.size(); ++index) {
			if (Order[index] != name) {
				continue;
			}
			Order.erase(Order.begin() + static_cast<ptrdiff_t>(index));
			Documents.erase(Documents.begin() + static_cast<ptrdiff_t>(index));
			return true;
		}
		return false;
	}

	void PipelineSet::Clear() {
		Order.clear();
		Documents.clear();
	}

	std::string Write(const PipelineSet &set) {
		std::string out;
		out += HEADER;
		out.push_back('\n');

		for (const core::Name name : set.Names()) {
			out += "pipeline ";
			AppendQuoted(out, name.Text());
			out.push_back('\n');
			AppendOperations(out, *set.Find(name));
		}

		return out;
	}

	DocumentStatus Read(std::string_view text, PipelineSet &set, std::string &offender) {
		set.Clear();

		if (!TakeHeader(text, offender)) {
			return DocumentStatus::Malformed;
		}

		core::Name open;
		Document document;

		const auto flush = [&set, &open, &document]() {
			if (open.IsValid()) {
				set.Set(open, std::move(document));
			}
			document.Clear();
		};

		while (!text.empty()) {
			const std::string_view whole = NextLine(text);
			if (whole.empty()) {
				continue;
			}

			std::string_view line = whole;
			if (TakeWord(line) == "pipeline") {
				std::string name;
				if (!TakeQuoted(line, name) || !TakeWord(line).empty() || name.empty()) {
					offender = std::string(whole);
					return DocumentStatus::Malformed;
				}

				flush();
				open = core::Name(name);
				continue;
			}

			// An operation belonging to no pipeline. Refused rather than
			// collected into an unnamed one, because a document this cannot
			// name is a document it cannot write back.
			Operation operation;
			if (!open.IsValid() || !ParseOperation(whole, operation)) {
				offender = std::string(whole);
				return DocumentStatus::Malformed;
			}

			document.Record(std::move(operation));
		}

		flush();
		return DocumentStatus::Ok;
	}

	bool IsBareNode(NodeKind kind) {
		return kind == NodeKind::Import || kind == NodeKind::Smooth || kind == NodeKind::Opaque ||
			   kind == NodeKind::Mipmap;
	}
}
