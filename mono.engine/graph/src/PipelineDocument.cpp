#include <engine/ecs/Components.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/Schedule.hpp>

#include <charconv>
#include <cmath>
#include <unordered_map>

namespace engine::graph {

	namespace {
		// The header every document starts with, and the version beside it.
		//
		// **A version from the first release rather than added at the second**,
		// which is `bake::GraphDocument`'s argument and the same one.
		constexpr std::string_view HEADER = "renderpipeline 2";
		constexpr std::string_view LEGACY_HEADER = "renderpipeline 1";

		// The set's header. **A different word rather than a flag on the same
		// one**, so a reader knows which shape it is holding from the first
		// line instead of from whether a `pipeline` line ever turns up.
		constexpr std::string_view SET_HEADER = "renderpipelines 2";
		constexpr std::string_view LEGACY_SET_HEADER = "renderpipelines 1";

		std::string_view ResourceText(ResourceKind kind) {
			switch (kind) {
			case ResourceKind::Colour:
				return "colour";
			case ResourceKind::Depth:
				return "depth";
			case ResourceKind::Texture:
				return "texture";
			case ResourceKind::Storage:
				return "storage";
			case ResourceKind::Buffer:
				return "buffer";
			case ResourceKind::Entities:
				return "entities";
			case ResourceKind::Camera:
				return "camera";
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
			if (text == "storage") {
				kind = ResourceKind::Storage;
				return true;
			}
			if (text == "buffer") {
				kind = ResourceKind::Buffer;
				return true;
			}
			if (text == "entities") {
				kind = ResourceKind::Entities;
				return true;
			}
			if (text == "camera") {
				kind = ResourceKind::Camera;
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

		// A canvas coordinate.
		//
		// **Read as an integer and written as one**, which is the whole reason
		// this is not `std::from_chars` over a float: a position is a pixel, a
		// document has to round-trip byte for byte, and a decimal expansion is
		// where that stops being true. Rounding a drag to the pixel it landed on
		// costs nothing anybody can see.
		bool TakeCoordinate(std::string_view &line, float &out) {
			const std::string_view word = TakeWord(line);
			if (word.empty()) {
				return false;
			}
			const char *first = word.data();
			const char *last = first + word.size();

			int32_t whole = 0;
			const auto result = std::from_chars(first, last, whole);
			if (result.ec != std::errc{} || result.ptr != last) {
				return false;
			}
			out = static_cast<float>(whole);
			return true;
		}

		std::string Rounded(float value) {
			return std::to_string(static_cast<int32_t>(std::lround(value)));
		}

		// A scope, by name.
		//
		// **A word rather than the boolean it replaced.** `perview no` said two
		// different things depending on which "no" was meant; `frame` and
		// `world` say which.
		bool ScopeFromText(std::string_view text, NodeScope &out) {
			for (const NodeScope candidate : {NodeScope::Frame, NodeScope::World, NodeScope::View}) {
				if (text == Describe(candidate)) {
					out = candidate;
					return true;
				}
			}
			return false;
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
		case EditKind::Set:
			return "set";
		case EditKind::Move:
			return "move";
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
		// one, `reads` and `writes` accumulate into it, and the next `node` - or
		// the end of the document - closes it.
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
				const ResourceId added = graph.AddResource({
					.Name = edit.Name,
					.Kind = edit.Resource,
					.Format = edit.Format,
					.Width = edit.Width,
					.Height = edit.Height,
					.External = edit.External,
					.Divisor = edit.Divisor,
				});
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
				pending.Scope = edit.Scope;
				building = true;
				break;
			case EditKind::Set: {
				// **The node above it, for `reads`' reason.** A `set` with no
				// node before it configures nothing, which is a document
				// somebody built wrong rather than a line to skip.
				if (!building) {
					offender = edit.Key;
					return PipelineDocumentStatus::UnknownName;
				}
				if (!edit.Key.IsValid()) {
					offender = edit.Key;
					return PipelineDocumentStatus::UnknownName;
				}

				bool replaced = false;
				for (NodeParameter &parameter : pending.Parameters) {
					if (parameter.Key == edit.Key) {
						parameter.Value = edit.Value;
						replaced = true;
						break;
					}
				}
				if (!replaced) {
					pending.Parameters.push_back(NodeParameter{edit.Key, edit.Value});
				}
				break;
			}
			case EditKind::Reads:
			case EditKind::Writes: {
				if (!building) {
					offender = edit.Target;
					return PipelineDocumentStatus::UnknownName;
				}

				// **An unnamed target is an empty slot, not a mistake.** An
				// editor's node has a fixed row of ports and some of them are
				// unwired; writing one line per row - empty ones included - is
				// what lets a reader put each binding back in the row it came
				// from, because a document has no slot index and position in
				// the list is the index. The runtime has no such notion, so
				// this is where the two part company.
				if (!edit.Target.IsValid()) {
					break;
				}
				const auto found = resources.find(edit.Target.Id());
				if (found == resources.end()) {
					offender = edit.Target;
					return PipelineDocumentStatus::UnknownName;
				}
				if (edit.Kind == EditKind::Reads) {
					pending.Reads.push_back(found->second);
					pending.ReadPorts.push_back(edit.Key);
				} else {
					pending.Writes.push_back(found->second);
					pending.WritePorts.push_back(edit.Key);
				}
				break;
			}
			case EditKind::Move:
				// **Ignored, and that is the whole point of it.** See `EditKind::Move`:
				// where a box sits must not be able to change what a frame computes.
				break;

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
		ExecutionSchedule schedule;
		if (CompileSchedule(graph, schedule, offender) != ScheduleStatus::Ok) {
			return PipelineDocumentStatus::Invalid;
		}

		return PipelineDocumentStatus::Ok;
	}

	std::unordered_map<uint32_t, std::pair<float, float>> PositionsOf(const PipelineDocument &document) {
		std::unordered_map<uint32_t, std::pair<float, float>> placed;

		// **Forwards, so the last `Move` wins.** A document is a record of what
		// somebody did and a drag is a sequence of them; replaying backwards to
		// stop early would be an optimisation that inverted the meaning.
		for (const Edit &edit : document.Edits()) {
			if (edit.Kind == EditKind::Move && edit.Name.IsValid()) {
				placed[edit.Name.Id()] = {edit.X, edit.Y};
			}
		}

		return placed;
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
				out += ' ' + std::string(Describe(edit.Format));
				out += ' ' + std::to_string(edit.Width) + ' ' + std::to_string(edit.Height);
				out += ' ' + std::to_string(edit.Divisor);
				out += ' ' + std::string(FlagText(edit.External));
				break;
			case EditKind::AddNode:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out.push_back(' ');
				AppendQuoted(out, edit.NodeKind.Text());
				out += ' ' + std::string(Describe(edit.Scope));
				// Kept as false in version 2 documents so older readers retain the
				// same token shape. The old optional field never affected a schedule.
				out += " no";
				break;
			case EditKind::Reads:
			case EditKind::Writes:
				out.push_back(' ');
				AppendQuoted(out, edit.Target.Text());
				if (edit.Key.IsValid()) {
					out.push_back(' ');
					AppendQuoted(out, edit.Key.Text());
				}
				break;
			case EditKind::Enable:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out += ' ' + std::string(FlagText(edit.Enabled));
				break;
			case EditKind::Set:
				out.push_back(' ');
				AppendQuoted(out, edit.Key.Text());
				out.push_back(' ');
				AppendQuoted(out, edit.Value);
				break;
			case EditKind::Move:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out += ' ' + Rounded(edit.X) + ' ' + Rounded(edit.Y);
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

		if (text.empty()) {
			offender = core::Name(HEADER);
			return PipelineDocumentStatus::Malformed;
		}
		const std::string_view header = nextLine();
		const bool legacy = header == LEGACY_HEADER;
		if (!legacy && header != HEADER) {
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
						 ParseResourceFormat(TakeWord(line), edit.Format) && TakeUnsigned(line, edit.Width) &&
						 TakeUnsigned(line, edit.Height) && TakeUnsigned(line, edit.Divisor);
				if (parsed && !legacy) {
					parsed = TakeFlag(line, edit.External);
				}
				edit.Name = core::Name(name);
			} else if (word == "node") {
				edit.Kind = EditKind::AddNode;
				bool ignoredOptional = false;
				parsed = TakeQuoted(line, name) && TakeQuoted(line, second) &&
						 ScopeFromText(TakeWord(line), edit.Scope) && TakeFlag(line, ignoredOptional);
				edit.Name = core::Name(name);
				edit.NodeKind = core::Name(second);
			} else if (word == "reads" || word == "writes") {
				edit.Kind = word == "reads" ? EditKind::Reads : EditKind::Writes;
				parsed = TakeQuoted(line, name);
				edit.Target = core::Name(name);
				while (!line.empty() && line.front() == ' ') {
					line.remove_prefix(1);
				}
				if (parsed && !line.empty()) {
					parsed = TakeQuoted(line, second);
					edit.Key = core::Name(second);
				}
			} else if (word == "move") {
				edit.Kind = EditKind::Move;
				parsed =
					TakeQuoted(line, name) && TakeCoordinate(line, edit.X) && TakeCoordinate(line, edit.Y);
				edit.Name = core::Name(name);
			} else if (word == "set") {
				edit.Kind = EditKind::Set;
				parsed = TakeQuoted(line, name) && TakeQuoted(line, second);
				edit.Key = core::Name(name);
				edit.Value = second;
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

		if (text.empty()) {
			offender = core::Name(SET_HEADER);
			return PipelineDocumentStatus::Malformed;
		}
		const std::string_view setHeader = nextLine();
		const bool legacy = setHeader == LEGACY_SET_HEADER;
		if (!legacy && setHeader != SET_HEADER) {
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
			const std::string whole = std::string(legacy ? LEGACY_HEADER : HEADER) + "\n" + body;
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

	PipelineDocument DefaultPbrDocument() {
		PipelineDocument document;

		const auto resource = [&document](
								  std::string_view name,
								  ResourceKind kind,
								  ResourceFormat format,
								  uint32_t divisor = 1,
								  bool external = false
							  ) {
			Edit edit;
			edit.Kind = EditKind::AddResource;
			edit.Name = core::Name(name);
			edit.Resource = kind;
			edit.Format = format;
			edit.Divisor = divisor;
			edit.External = external;
			document.Record(std::move(edit));
		};

		const auto node = [&document](std::string_view name, NodeScope scope) {
			Edit edit;
			edit.Kind = EditKind::AddNode;
			edit.Name = core::Name(name);
			edit.NodeKind = core::Name(name);
			edit.Scope = scope;
			document.Record(std::move(edit));
		};

		const auto touches = [&document](EditKind kind, std::string_view target, std::string_view port) {
			Edit edit;
			edit.Kind = kind;
			edit.Target = core::Name(target);
			edit.Key = core::Name(port);
			document.Record(std::move(edit));
		};

		resource("shadow", ResourceKind::Depth, ResourceFormat::D32F, 1, true);
		resource("last-frame", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("mirror-views", ResourceKind::Colour, ResourceFormat::RGBA8, 1, true);
		resource("portal-image", ResourceKind::Texture, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("portal-display", ResourceKind::Texture, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("portal-light", ResourceKind::Texture, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("world-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("view-camera", ResourceKind::Camera, ResourceFormat::R8);
		resource("view-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("visible-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("ordered-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("view-instances", ResourceKind::Buffer, ResourceFormat::R8);
		resource("albedo", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("normal", ResourceKind::Colour, ResourceFormat::RGB10A2);
		resource("material", ResourceKind::Colour, ResourceFormat::RGBA8);
		resource("emissive", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("depth", ResourceKind::Depth, ResourceFormat::D24S8);
		resource("linear-depth", ResourceKind::Colour, ResourceFormat::R32F);
		resource("occlusion", ResourceKind::Colour, ResourceFormat::R8, 2, true);
		resource("lit", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("sky-lit", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("tonemapped", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("portaled", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("mirrored", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("display", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("scene-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("interface-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("composed-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);

		node("world", NodeScope::World);
		touches(EditKind::Writes, "world-entities", "entities");

		node("shadow", NodeScope::World);
		touches(EditKind::Reads, "world-entities", "entities");
		touches(EditKind::Writes, "shadow", "shadow");

		node("camera", NodeScope::View);
		touches(EditKind::Writes, "view-camera", "camera");

		node("last-frame", NodeScope::View);
		touches(EditKind::Writes, "last-frame", "image");

		node("entities", NodeScope::View);
		touches(EditKind::Writes, "view-entities", "entities");

		node("cull-frustum", NodeScope::View);
		touches(EditKind::Reads, "view-entities", "entities");
		touches(EditKind::Reads, "view-camera", "camera");
		touches(EditKind::Writes, "visible-entities", "entities");

		node("order-draw", NodeScope::View);
		touches(EditKind::Reads, "visible-entities", "entities");
		touches(EditKind::Reads, "view-camera", "camera");
		touches(EditKind::Writes, "ordered-entities", "entities");

		node("upload-instances", NodeScope::View);
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Writes, "view-instances", "instances");

		node("mirror-capture", NodeScope::View);
		touches(EditKind::Reads, "last-frame", "last-frame");
		touches(EditKind::Reads, "world-entities", "world-state");
		touches(EditKind::Reads, "shadow", "shadow");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "mirror-views", "surface");
		{
			Edit feedback;
			feedback.Kind = EditKind::Set;
			feedback.Key = core::Name("feedback");
			feedback.Value = "last-frame";
			document.Record(std::move(feedback));
			Edit recursion;
			recursion.Kind = EditKind::Set;
			recursion.Key = core::Name("max-recursion");
			recursion.Value = "3";
			document.Record(std::move(recursion));
		}

		node("portal-capture", NodeScope::View);
		touches(EditKind::Reads, "shadow", "shadow");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "portal-image", "portal");
		touches(EditKind::Writes, "portal-light", "light");

		node("portal-tonemap", NodeScope::View);
		touches(EditKind::Reads, "portal-image", "portal");
		touches(EditKind::Writes, "portal-display", "portal");

		node("gbuffer", NodeScope::View);
		touches(EditKind::Reads, "shadow", "shadow");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "albedo", "albedo");
		touches(EditKind::Writes, "normal", "normal");
		touches(EditKind::Writes, "material", "material");
		touches(EditKind::Writes, "emissive", "emissive");
		touches(EditKind::Writes, "depth", "depth");

		node("depth-linearise", NodeScope::View);
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Writes, "linear-depth", "linear");

		node("ssao", NodeScope::View);
		touches(EditKind::Reads, "linear-depth", "depth");
		touches(EditKind::Reads, "normal", "normal");
		touches(EditKind::Writes, "occlusion", "occlusion");

		node("deferred-lighting", NodeScope::View);
		touches(EditKind::Reads, "albedo", "albedo");
		touches(EditKind::Reads, "normal", "normal");
		touches(EditKind::Reads, "material", "material");
		touches(EditKind::Reads, "emissive", "emissive");
		touches(EditKind::Reads, "linear-depth", "depth");
		touches(EditKind::Reads, "occlusion", "occlusion");
		touches(EditKind::Reads, "shadow", "shadow");
		// The seam light-field: what orders this pass after portal-capture, so
		// the projection samples this frame's captures rather than last frame's.
		touches(EditKind::Reads, "portal-light", "portal-light");
		touches(EditKind::Writes, "lit", "colour");

		node("sky", NodeScope::View);
		touches(EditKind::Reads, "lit", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Writes, "sky-lit", "colour");

		node("tonemap", NodeScope::View);
		touches(EditKind::Reads, "sky-lit", "colour");
		touches(EditKind::Writes, "tonemapped", "colour");

		node("portal-overlay", NodeScope::View);
		touches(EditKind::Reads, "tonemapped", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "portal-display", "portal");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "portaled", "colour");

		node("mirror-overlay", NodeScope::View);
		touches(EditKind::Reads, "portaled", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "mirror-views", "surface");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "mirrored", "colour");

		node("transparent", NodeScope::View);
		touches(EditKind::Reads, "mirrored", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "display", "colour");

		node("present", NodeScope::Frame);
		touches(EditKind::Reads, "display", "image");
		touches(EditKind::Writes, "scene-image", "image");

		node("interface", NodeScope::Frame);
		touches(EditKind::Writes, "interface-image", "image");

		node("overlay", NodeScope::Frame);
		touches(EditKind::Reads, "scene-image", "scene");
		touches(EditKind::Reads, "interface-image", "interface");
		touches(EditKind::Writes, "composed-image", "image");

		node("output-image", NodeScope::Frame);
		touches(EditKind::Reads, "composed-image", "image");

		return document;
	}

	PipelineDocument DefaultPbrTierBDocument() {
		const PipelineDocument full = DefaultPbrDocument();
		PipelineDocument reduced;
		bool skipNode = false;
		for (const Edit &edit : full.Edits()) {
			if (edit.Kind == EditKind::AddResource) {
				skipNode = false;
				if (edit.Name == core::Name("depth-pyramid") || edit.Name == core::Name("occlusion")) {
					continue;
				}
			}
			if (edit.Kind == EditKind::AddNode) {
				skipNode = edit.NodeKind == core::Name("hzb") || edit.NodeKind == core::Name("ssao");
			}
			if (skipNode || (edit.Kind == EditKind::Reads && edit.Target == core::Name("occlusion"))) {
				continue;
			}
			reduced.Record(edit);
		}
		return reduced;
	}

	PipelineDocument DefaultForwardTierCDocument() {
		PipelineDocument document;
		const auto resource =
			[&document](
				std::string_view name, ResourceKind kind, ResourceFormat format, bool external = false
			) {
				document.Record(
					Edit{
						.Kind = EditKind::AddResource,
						.Name = core::Name(name),
						.Resource = kind,
						.Format = format,
						.External = external,
					}
				);
			};
		const auto node = [&document](std::string_view name, NodeScope scope) {
			document.Record(
				Edit{
					.Kind = EditKind::AddNode,
					.Name = core::Name(name),
					.NodeKind = core::Name(name),
					.Scope = scope,
				}
			);
		};
		const auto touches = [&document](EditKind kind, std::string_view target, std::string_view port) {
			document.Record(
				Edit{
					.Kind = kind,
					.Target = core::Name(target),
					.Key = core::Name(port),
				}
			);
		};

		resource("world-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("view-camera", ResourceKind::Camera, ResourceFormat::R8);
		resource("view-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("visible-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("ordered-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("view-instances", ResourceKind::Buffer, ResourceFormat::R8);
		resource("forward-colour", ResourceKind::Colour, ResourceFormat::RGB10A2);
		resource("depth", ResourceKind::Depth, ResourceFormat::D24S8);
		resource("scene-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("interface-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, true);
		resource("composed-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);

		node("world", NodeScope::World);
		touches(EditKind::Writes, "world-entities", "entities");
		node("camera", NodeScope::View);
		touches(EditKind::Writes, "view-camera", "camera");
		node("entities", NodeScope::View);
		touches(EditKind::Writes, "view-entities", "entities");
		node("cull-frustum", NodeScope::View);
		touches(EditKind::Reads, "view-entities", "entities");
		touches(EditKind::Reads, "view-camera", "camera");
		touches(EditKind::Writes, "visible-entities", "entities");
		node("order-draw", NodeScope::View);
		touches(EditKind::Reads, "visible-entities", "entities");
		touches(EditKind::Reads, "view-camera", "camera");
		touches(EditKind::Writes, "ordered-entities", "entities");
		node("upload-instances", NodeScope::View);
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Writes, "view-instances", "instances");
		node("forward", NodeScope::View);
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Writes, "forward-colour", "colour");
		touches(EditKind::Writes, "depth", "depth");
		node("present", NodeScope::Frame);
		touches(EditKind::Reads, "forward-colour", "image");
		touches(EditKind::Writes, "scene-image", "image");
		node("interface", NodeScope::Frame);
		touches(EditKind::Writes, "interface-image", "image");
		node("overlay", NodeScope::Frame);
		touches(EditKind::Reads, "scene-image", "scene");
		touches(EditKind::Reads, "interface-image", "interface");
		touches(EditKind::Writes, "composed-image", "image");
		node("output-image", NodeScope::Frame);
		touches(EditKind::Reads, "composed-image", "image");
		return document;
	}
}
