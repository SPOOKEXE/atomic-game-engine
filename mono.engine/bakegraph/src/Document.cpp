#include <engine/bakegraph/Document.hpp>
#include <engine/graphcore/Document.hpp>

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

		using namespace graphcore::text;
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
		case OperationKind::AddDecimate:
			return "decimate";
		case OperationKind::AddFlipbook:
			return "flipbook";
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
					out += IsBareNode(operation.Node) ? NodeKindId(operation.Node) : std::string_view{};
					break;
				case OperationKind::AddFit:
				case OperationKind::AddRetime:
				case OperationKind::AddDecimate:
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
				case OperationKind::AddFlipbook:
					out += ' ' + std::to_string(operation.Side) + ' ' + std::to_string(operation.Frames);
					out.push_back(' ');
					AppendFloat(out, operation.Number);
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
		// misspelling is. The caller decides what a refusal costs - and for a
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
				parsed = NodeKindFromId(TakeWord(line), operation.Node) && IsBareNode(operation.Node);
			} else if (word == "fit" || word == "retime" || word == "decimate") {
				operation.Kind = word == "fit"		? OperationKind::AddFit
								 : word == "retime" ? OperationKind::AddRetime
													: OperationKind::AddDecimate;
				parsed = TakeFloat(line, operation.Number);
			} else if (word == "scale") {
				operation.Kind = OperationKind::AddScale;
				parsed = TakeFloat(line, operation.Amount.X) && TakeFloat(line, operation.Amount.Y) &&
						 TakeFloat(line, operation.Amount.Z);
			} else if (word == "resize" || word == "rasterize") {
				operation.Kind = word == "resize" ? OperationKind::AddResize : OperationKind::AddRasterize;
				parsed = TakeUnsigned(line, operation.Width) && TakeUnsigned(line, operation.Height);
			} else if (word == "flipbook") {
				operation.Kind = OperationKind::AddFlipbook;
				parsed = TakeUnsigned(line, operation.Side) && TakeUnsigned(line, operation.Frames) &&
						 TakeFloat(line, operation.Number);
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
			return parsed && AtEnd(line);
		}

		// The header, or `Malformed` naming it.
		bool TakeDocumentHeader(std::string_view &text, std::string &offender) {
			if (TakeHeader(text, std::span<const std::string_view>(&HEADER, 1)) != 0) {
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

		if (!TakeDocumentHeader(text, offender)) {
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

	std::string Write(const PipelineSet &set) {
		std::string out;
		out += HEADER;
		out.push_back('\n');
		for (const core::Name name : set.Names()) {
			AppendSection(out, name.Text());
			AppendOperations(out, *set.Find(name));
		}
		return out;
	}

	DocumentStatus Read(std::string_view text, PipelineSet &set, std::string &offender) {
		set.Clear();
		if (!TakeDocumentHeader(text, offender)) {
			return DocumentStatus::Malformed;
		}
		core::Name open;
		Document document;
		const auto flush = [&] {
			if (open.IsValid()) {
				set.Set(open, std::move(document));
			}
			document.Clear();
		};
		const bool read = ReadSections(
			text,
			[&](std::string_view name) {
				flush();
				open = core::Name(name);
			},
			[&](std::string_view line) {
				Operation operation;
				if (!ParseOperation(line, operation)) {
					return false;
				}
				document.Record(std::move(operation));
				return true;
			},
			offender
		);
		if (!read) {
			return DocumentStatus::Malformed;
		}
		flush();
		return DocumentStatus::Ok;
	}

	bool IsBareNode(NodeKind kind) {
		return kind == NodeKind::Import || kind == NodeKind::Smooth || kind == NodeKind::Opaque ||
			   kind == NodeKind::Mipmap;
	}
}

namespace engine::bake {

	namespace {
		// Indexed by ordinal; append only, as the ordinals are.
		constexpr std::string_view NODE_KIND_IDS[] = {
			"source",
			"builtin",
			"import",
			"fit",
			"scale",
			"smooth",
			"resize",
			"opaque",
			"mipmap",
			"retime",
			"write",
			"rasterize",
			"decimate",
			"flipbook",
		};
	}

	std::string_view NodeKindId(NodeKind kind) {
		const auto index = static_cast<size_t>(kind);
		return index < std::size(NODE_KIND_IDS) ? NODE_KIND_IDS[index] : std::string_view{};
	}

	bool NodeKindFromId(std::string_view id, NodeKind &kind) {
		for (size_t index = 0; index < std::size(NODE_KIND_IDS); ++index) {
			if (!id.empty() && NODE_KIND_IDS[index] == id) {
				kind = static_cast<NodeKind>(index);
				return true;
			}
		}
		return false;
	}
}
