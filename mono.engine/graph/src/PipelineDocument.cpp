#include <engine/ecs/Components.hpp>
#include <engine/graph/PipelineDocument.hpp>

#include <charconv>
#include <unordered_map>

namespace engine::graph {

	namespace {
		// The header every document starts with, and the version beside it.
		//
		// **A version from the first release rather than added at the second**,
		// which is `bake::GraphDocument`'s argument and the same one.
		constexpr std::string_view HEADER = "renderpipeline 1";

		// The set's header. **A different word rather than a flag on the same
		// one**, so a reader knows which shape it is holding from the first
		// line instead of from whether a `pipeline` line ever turns up.
		constexpr std::string_view SET_HEADER = "renderpipelines 1";

		std::string_view ResourceText(ResourceKind kind) {
			switch (kind) {
			case ResourceKind::Colour:
				return "colour";
			case ResourceKind::Depth:
				return "depth";
			case ResourceKind::Texture:
				return "texture";
			}
			return "";
		}

		bool ResourceFromText(std::string_view text, ResourceKind &kind) {
			if (text == "colour") {
				kind = ResourceKind::Colour;
				return true;
			}
			if (text == "depth") {
				kind = ResourceKind::Depth;
				return true;
			}
			if (text == "texture") {
				kind = ResourceKind::Texture;
				return true;
			}
			return false;
		}

		// **The escape set is the characters that could forge structure**, not a
		// general string escape: a name holding a newline could otherwise write
		// a second edit into the file. Held identical to `bake`'s so the two
		// formats cannot drift into disagreeing about what a name is.
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
					// An escape nothing writes, so the format keeps one spelling
					// per name and the round trip cannot be ambiguous.
					return false;
				}
			}
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

		bool TakeFlag(std::string_view &line, bool &out) {
			const std::string_view word = TakeWord(line);
			if (word == "yes") {
				out = true;
				return true;
			}
			if (word == "no") {
				out = false;
				return true;
			}
			return false;
		}

		// **Words rather than 0 and 1.** These files are meant to be read, and
		// `perview no` says what it means where `perview 0` needs the header
		// open beside it.
		std::string_view FlagText(bool value) {
			return value ? "yes" : "no";
		}
	}

	const char *Describe(EditKind kind) {
		switch (kind) {
		case EditKind::AddResource:
			return "resource";
		case EditKind::AddNode:
			return "node";
		case EditKind::Reads:
			return "reads";
		case EditKind::Writes:
			return "writes";
		case EditKind::Enable:
			return "enable";
		}
		return "unknown";
	}

	const char *Describe(PipelineDocumentStatus status) {
		switch (status) {
		case PipelineDocumentStatus::Ok:
			return "ok";
		case PipelineDocumentStatus::UnknownName:
			return "an edit names something this document has not declared";
		case PipelineDocumentStatus::Refused:
			return "the graph refused the edit";
		case PipelineDocumentStatus::Malformed:
			return "not a render pipeline document";
		case PipelineDocumentStatus::Invalid:
			return "the document builds a graph that will not compile";
		}
		return "unknown";
	}

	void PipelineDocument::Record(Edit edit) {
		Records.push_back(std::move(edit));
	}

	bool PipelineDocument::Undo() {
		if (Records.empty()) {
			return false;
		}
		Records.pop_back();
		return true;
	}

	void PipelineDocument::Clear() {
		Records.clear();
	}

	PipelineDocumentStatus Build(const PipelineDocument &document, RenderGraph &graph, core::Name &offender) {
		// **Nodes are built up and added at the end of their run of edits**,
		// because `RenderGraph::AddNode` takes a whole node and the document
		// states its reads and writes as separate edits. So a `node` edit opens
		// one, `reads` and `writes` accumulate into it, and the next `node` — or
		// the end of the document — closes it.
		std::unordered_map<uint32_t, ResourceId> resources;
		std::unordered_map<uint32_t, NodeId> nodes;

		Node pending;
		bool building = false;

		const auto closePending = [&]() {
			if (!building) {
				return true;
			}
			building = false;

			const NodeId added = graph.AddNode(pending);
			if (!added.IsValid()) {
				offender = pending.Name;
				return false;
			}
			nodes[pending.Name.Id()] = added;
			return true;
		};

		for (const Edit &edit : document.Edits()) {
			if (edit.Kind == EditKind::AddResource || edit.Kind == EditKind::AddNode ||
				edit.Kind == EditKind::Enable) {
				if (!closePending()) {
					return PipelineDocumentStatus::Refused;
				}
			}

			switch (edit.Kind) {
			case EditKind::AddResource: {
				const ResourceId added =
					graph.AddResource({edit.Name, edit.Resource, edit.Width, edit.Height});
				if (!added.IsValid()) {
					offender = edit.Name;
					return PipelineDocumentStatus::Refused;
				}
				resources[edit.Name.Id()] = added;
				break;
			}
			case EditKind::AddNode:
				pending = Node{};
				pending.Name = edit.Name;
				pending.Kind = edit.NodeKind;
				pending.PerView = edit.PerView;
				pending.Optional = edit.Optional;
				building = true;
				break;
			case EditKind::Reads:
			case EditKind::Writes: {
				if (!building) {
					offender = edit.Target;
					return PipelineDocumentStatus::UnknownName;
				}
				const auto found = resources.find(edit.Target.Id());
				if (found == resources.end()) {
					offender = edit.Target;
					return PipelineDocumentStatus::UnknownName;
				}
				(edit.Kind == EditKind::Reads ? pending.Reads : pending.Writes).push_back(found->second);
				break;
			}
			case EditKind::Enable: {
				const auto found = nodes.find(edit.Name.Id());
				if (found == nodes.end()) {
					offender = edit.Name;
					return PipelineDocumentStatus::UnknownName;
				}
				graph.SetEnabled(found->second, edit.Enabled);
				break;
			}
			}
		}

		if (!closePending()) {
			return PipelineDocumentStatus::Refused;
		}

		// **Compiled here rather than left to the caller.** A document that
		// builds a graph nothing can run is a broken save file, and saying so at
		// load is the difference between a diagnostic naming the node and a
		// frame lit by whatever was in that memory.
		CompiledGraph compiled;
		if (graph.Compile(compiled, offender) != GraphStatus::Ok) {
			return PipelineDocumentStatus::Invalid;
		}

		return PipelineDocumentStatus::Ok;
	}

	std::string Write(const PipelineDocument &document) {
		std::string out;
		out += HEADER;
		out.push_back('\n');

		for (const Edit &edit : document.Edits()) {
			out += Describe(edit.Kind);

			switch (edit.Kind) {
			case EditKind::AddResource:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out += ' ' + std::string(ResourceText(edit.Resource));
				out += ' ' + std::to_string(edit.Width) + ' ' + std::to_string(edit.Height);
				break;
			case EditKind::AddNode:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out.push_back(' ');
				AppendQuoted(out, edit.NodeKind.Text());
				out += ' ' + std::string(FlagText(edit.PerView));
				out += ' ' + std::string(FlagText(edit.Optional));
				break;
			case EditKind::Reads:
			case EditKind::Writes:
				out.push_back(' ');
				AppendQuoted(out, edit.Target.Text());
				break;
			case EditKind::Enable:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out += ' ' + std::string(FlagText(edit.Enabled));
				break;
			}

			out.push_back('\n');
		}

		return out;
	}

	PipelineDocumentStatus Read(std::string_view text, PipelineDocument &document, core::Name &offender) {
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
			offender = core::Name(HEADER);
			return PipelineDocumentStatus::Malformed;
		}

		while (!text.empty()) {
			const std::string_view whole = nextLine();
			if (whole.empty()) {
				continue;
			}

			std::string_view line = whole;
			const std::string_view word = TakeWord(line);

			Edit edit;
			std::string name;
			std::string second;
			bool parsed = true;

			if (word == "resource") {
				edit.Kind = EditKind::AddResource;
				parsed = TakeQuoted(line, name) && ResourceFromText(TakeWord(line), edit.Resource) &&
						 TakeUnsigned(line, edit.Width) && TakeUnsigned(line, edit.Height);
				edit.Name = core::Name(name);
			} else if (word == "node") {
				edit.Kind = EditKind::AddNode;
				parsed = TakeQuoted(line, name) && TakeQuoted(line, second) && TakeFlag(line, edit.PerView) &&
						 TakeFlag(line, edit.Optional);
				edit.Name = core::Name(name);
				edit.NodeKind = core::Name(second);
			} else if (word == "reads" || word == "writes") {
				edit.Kind = word == "reads" ? EditKind::Reads : EditKind::Writes;
				parsed = TakeQuoted(line, name);
				edit.Target = core::Name(name);
			} else if (word == "enable") {
				edit.Kind = EditKind::Enable;
				parsed = TakeQuoted(line, name) && TakeFlag(line, edit.Enabled);
				edit.Name = core::Name(name);
			} else {
				parsed = false;
			}

			// **Trailing text is a refusal, not something to ignore.** A line
			// with a spare word on it is somebody's misunderstanding of the
			// format, and accepting it silently would build something other than
			// what they wrote.
			if (parsed && !TakeWord(line).empty()) {
				parsed = false;
			}

			if (!parsed) {
				offender = core::Name(whole);
				return PipelineDocumentStatus::Malformed;
			}

			document.Record(std::move(edit));
		}

		return PipelineDocumentStatus::Ok;
	}

	bool PipelineSet::Set(core::Name name, PipelineDocument document) {
		if (!name.IsValid()) {
			return false;
		}

		for (size_t index = 0; index < Order.size(); index++) {
			if (Order[index] == name) {
				Documents[index] = std::move(document);
				return true;
			}
		}

		// Inserted in sorted position rather than appended and sorted later, so
		// `Names` is always ordered and a save is byte-identical whatever order
		// an editor added things in.
		size_t at = 0;
		while (at < Order.size() && Order[at].Text() < name.Text()) {
			at++;
		}
		Order.insert(Order.begin() + static_cast<ptrdiff_t>(at), name);
		Documents.insert(Documents.begin() + static_cast<ptrdiff_t>(at), std::move(document));
		return true;
	}

	const PipelineDocument *PipelineSet::Find(core::Name name) const {
		for (size_t index = 0; index < Order.size(); index++) {
			if (Order[index] == name) {
				return &Documents[index];
			}
		}
		return nullptr;
	}

	bool PipelineSet::Remove(core::Name name) {
		for (size_t index = 0; index < Order.size(); index++) {
			if (Order[index] == name) {
				Order.erase(Order.begin() + static_cast<ptrdiff_t>(index));
				Documents.erase(Documents.begin() + static_cast<ptrdiff_t>(index));
				return true;
			}
		}
		return false;
	}

	void PipelineSet::Clear() {
		Order.clear();
		Documents.clear();
	}

	std::string Write(const PipelineSet &set) {
		std::string out;
		out += SET_HEADER;
		out.push_back('\n');

		for (size_t index = 0; index < set.Count(); index++) {
			const core::Name name = set.Names()[index];

			out += "pipeline ";
			AppendQuoted(out, name.Text());
			out.push_back('\n');

			// **The document's own text minus its header.** Written through the
			// same function rather than a second emitter, so the two formats
			// cannot drift into disagreeing about how an edit is spelled.
			std::string body = Write(*set.Find(name));
			body.erase(0, HEADER.size() + 1);
			out += body;
		}

		return out;
	}

	PipelineDocumentStatus Read(std::string_view text, PipelineSet &set, core::Name &offender) {
		set.Clear();

		const auto nextLine = [&text]() -> std::string_view {
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			return line;
		};

		if (text.empty() || nextLine() != SET_HEADER) {
			offender = core::Name(SET_HEADER);
			return PipelineDocumentStatus::Malformed;
		}

		// **Each pipeline's lines are gathered and handed to the document
		// reader**, rather than parsed a second time here. One parser for one
		// grammar; this function only decides where one pipeline stops.
		core::Name pending;
		std::string body;

		const auto flush = [&]() {
			if (!pending.IsValid()) {
				return PipelineDocumentStatus::Ok;
			}

			PipelineDocument document;
			const std::string whole = std::string(HEADER) + "\n" + body;
			const PipelineDocumentStatus status = Read(whole, document, offender);
			if (status != PipelineDocumentStatus::Ok) {
				return status;
			}

			set.Set(pending, std::move(document));
			pending = core::Name{};
			body.clear();
			return PipelineDocumentStatus::Ok;
		};

		while (!text.empty()) {
			const std::string_view whole = nextLine();
			if (whole.empty()) {
				continue;
			}

			std::string_view line = whole;
			if (TakeWord(line) == "pipeline") {
				const PipelineDocumentStatus status = flush();
				if (status != PipelineDocumentStatus::Ok) {
					return status;
				}

				std::string name;
				if (!TakeQuoted(line, name) || !TakeWord(line).empty() || name.empty()) {
					offender = core::Name(whole);
					return PipelineDocumentStatus::Malformed;
				}
				pending = core::Name(name);
				continue;
			}

			// An edit belonging to no named pipeline is a file somebody
			// hand-edited into a shape a set cannot represent.
			if (!pending.IsValid()) {
				offender = core::Name(whole);
				return PipelineDocumentStatus::Malformed;
			}

			body += whole;
			body.push_back('\n');
		}

		return flush();
	}

	void RegisterPipelineComponents() {
		// No writer or reader: a pipeline set is authored content that travels
		// in the world document, not state that travels in a replication
		// snapshot. Registering it with serialisers would claim it belonged on
		// the wire.
		ecs::Components::Register<PipelineSet>("graph.PipelineSet");
	}

	PipelineDocument StandardDocument() {
		PipelineDocument document;

		const auto resource = [&document](std::string_view name, ResourceKind kind) {
			Edit edit;
			edit.Kind = EditKind::AddResource;
			edit.Name = core::Name(name);
			edit.Resource = kind;
			document.Record(std::move(edit));
		};

		const auto node = [&document](std::string_view name, bool perView, bool optional) {
			Edit edit;
			edit.Kind = EditKind::AddNode;
			edit.Name = core::Name(name);
			edit.NodeKind = core::Name(name);
			edit.PerView = perView;
			edit.Optional = optional;
			document.Record(std::move(edit));
		};

		const auto touches = [&document](EditKind kind, std::string_view target) {
			Edit edit;
			edit.Kind = kind;
			edit.Target = core::Name(target);
			document.Record(std::move(edit));
		};

		resource("shadow", ResourceKind::Depth);
		resource("surface", ResourceKind::Colour);
		resource("colour", ResourceKind::Colour);
		resource("depth", ResourceKind::Depth);
		resource("window", ResourceKind::Colour);

		node("shadow", false, true);
		touches(EditKind::Writes, "shadow");

		node("surface", true, true);
		touches(EditKind::Reads, "shadow");
		touches(EditKind::Writes, "surface");

		node("opaque", true, false);
		touches(EditKind::Reads, "shadow");
		touches(EditKind::Reads, "surface");
		touches(EditKind::Writes, "colour");
		touches(EditKind::Writes, "depth");

		node("transparent", true, true);
		touches(EditKind::Reads, "colour");
		touches(EditKind::Reads, "depth");
		touches(EditKind::Writes, "colour");

		node("overlay", false, true);
		touches(EditKind::Writes, "window");

		node("interface", false, true);
		touches(EditKind::Writes, "window");

		return document;
	}
}
