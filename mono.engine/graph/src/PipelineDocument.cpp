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
		constexpr std::string_view HEADER = "renderpipeline 3";
		constexpr std::string_view VERSION_TWO_HEADER = "renderpipeline 2";
		constexpr std::string_view LEGACY_HEADER = "renderpipeline 1";

		// The set's header. **A different word rather than a flag on the same
		// one**, so a reader knows which shape it is holding from the first
		// line instead of from whether a `pipeline` line ever turns up.
		constexpr std::string_view SET_HEADER = "renderpipelines 3";
		constexpr std::string_view VERSION_TWO_SET_HEADER = "renderpipelines 2";
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
		case EditKind::Group:
			return "group";
		case EditKind::Comment:
			return "comment";
		case EditKind::Mute:
			return "mute";
		case EditKind::Preview:
			return "preview";
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
					.External = edit.External || edit.Lifetime != ResourceLifetime::Transient,
					.Divisor = edit.Divisor,
					.Access = edit.Access,
					.Samples = edit.Samples,
					.Depth = edit.Depth,
					.Layers = edit.Layers,
					.FirstMip = edit.FirstMip,
					.MipCount = edit.MipCount,
					.ColourSpace = edit.ColourSpace,
					.AlphaSpace = edit.AlphaSpace,
					.BufferStride = edit.BufferStride,
					.Lifetime = edit.Lifetime,
					.Owner = edit.Owner,
					.HistoryGeneration = edit.HistoryGeneration,
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
			case EditKind::Group:
			case EditKind::Comment:
			case EditKind::Mute:
			case EditKind::Preview:
				// Authoring records survive text replay but cannot alter runtime work.
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
				out += ' ' + std::string(Describe(edit.Access));
				out += ' ' + std::to_string(edit.Samples);
				out += ' ' + std::to_string(edit.Depth);
				out += ' ' + std::to_string(edit.Layers);
				out += ' ' + std::to_string(edit.FirstMip);
				out += ' ' + std::to_string(edit.MipCount);
				out += ' ' + std::string(Describe(edit.ColourSpace));
				out += ' ' + std::string(Describe(edit.AlphaSpace));
				out += ' ' + std::to_string(edit.BufferStride);
				out += ' ' + std::string(Describe(edit.Lifetime));
				out.push_back(' ');
				AppendQuoted(out, edit.Owner.Text());
				out += ' ' + std::to_string(edit.HistoryGeneration);
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
			case EditKind::Group:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out.push_back(' ');
				AppendQuoted(out, edit.Target.Text());
				break;
			case EditKind::Comment:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out.push_back(' ');
				AppendQuoted(out, edit.Value);
				break;
			case EditKind::Mute:
				out.push_back(' ');
				AppendQuoted(out, edit.Name.Text());
				out += ' ' + std::string(FlagText(edit.Enabled));
				break;
			case EditKind::Preview:
				out.push_back(' ');
				AppendQuoted(out, edit.Target.Text());
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
		const bool versionOne = header == LEGACY_HEADER;
		const bool versionTwo = header == VERSION_TWO_HEADER;
		if (!versionOne && !versionTwo && header != HEADER) {
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
				if (parsed && !versionOne) {
					parsed = TakeFlag(line, edit.External);
				}
				if (parsed && !versionOne && !versionTwo) {
					parsed = ParseResourceAccess(TakeWord(line), edit.Access) &&
							 TakeUnsigned(line, edit.Samples) && TakeUnsigned(line, edit.Depth) &&
							 TakeUnsigned(line, edit.Layers) && TakeUnsigned(line, edit.FirstMip) &&
							 TakeUnsigned(line, edit.MipCount) &&
							 ParseResourceColourSpace(TakeWord(line), edit.ColourSpace) &&
							 ParseResourceAlphaSpace(TakeWord(line), edit.AlphaSpace) &&
							 TakeUnsigned(line, edit.BufferStride) &&
							 ParseResourceLifetime(TakeWord(line), edit.Lifetime) &&
							 TakeQuoted(line, second) && TakeUnsigned(line, edit.HistoryGeneration);
					edit.Owner = core::Name(second);
				} else if (parsed && edit.External) {
					edit.Lifetime = ResourceLifetime::External;
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
			} else if (word == "group") {
				edit.Kind = EditKind::Group;
				parsed = TakeQuoted(line, name) && TakeQuoted(line, second);
				edit.Name = core::Name(name);
				edit.Target = core::Name(second);
			} else if (word == "comment") {
				edit.Kind = EditKind::Comment;
				parsed = TakeQuoted(line, name) && TakeQuoted(line, second);
				edit.Name = core::Name(name);
				edit.Value = second;
			} else if (word == "mute") {
				edit.Kind = EditKind::Mute;
				parsed = TakeQuoted(line, name) && TakeFlag(line, edit.Enabled);
				edit.Name = core::Name(name);
			} else if (word == "preview") {
				edit.Kind = EditKind::Preview;
				parsed = TakeQuoted(line, name);
				edit.Target = core::Name(name);
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
		const bool versionOne = setHeader == LEGACY_SET_HEADER;
		const bool versionTwo = setHeader == VERSION_TWO_SET_HEADER;
		if (!versionOne && !versionTwo && setHeader != SET_HEADER) {
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
			const std::string whole = std::string(
										  versionOne   ? LEGACY_HEADER
										  : versionTwo ? VERSION_TWO_HEADER
													   : HEADER
									  ) +
									  "\n" + body;
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
		const auto historyResource = [&document](std::string_view name) {
			Edit edit;
			edit.Kind = EditKind::AddResource;
			edit.Name = core::Name(name);
			edit.Resource = ResourceKind::Storage;
			edit.Format = ResourceFormat::RGBA16F;
			edit.Width = 1024;
			edit.Height = 512;
			edit.Lifetime = ResourceLifetime::History;
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
		resource("mirror-views", ResourceKind::Colour, ResourceFormat::RGBA16F, 1, true);
		resource("portal-image", ResourceKind::Texture, ResourceFormat::RGBA16F, 1, true);
		resource("portal-light", ResourceKind::Texture, ResourceFormat::RGBA16F, 1, true);
		resource("world-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("view-camera", ResourceKind::Camera, ResourceFormat::R8);
		resource("view-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("visible-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("ordered-entities", ResourceKind::Entities, ResourceFormat::R8);
		resource("resident-meshes", ResourceKind::Buffer, ResourceFormat::R8);
		historyResource("environment-sky");
		historyResource("environment-clouds");
		resource("view-instances", ResourceKind::Buffer, ResourceFormat::R8);
		resource("lod-instances", ResourceKind::Buffer, ResourceFormat::R8);
		resource("albedo", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("normal", ResourceKind::Colour, ResourceFormat::RGB10A2);
		resource("material", ResourceKind::Colour, ResourceFormat::RGBA8);
		resource("emissive", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("depth", ResourceKind::Depth, ResourceFormat::D24S8);
		resource("linear-depth", ResourceKind::Colour, ResourceFormat::R32F);
		resource("occlusion", ResourceKind::Colour, ResourceFormat::R8, 2, true);
		resource("lit", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("sky-lit", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("volume-lit", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("lens-b", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("lens-scratch", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("tonemapped", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("portaled", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("mirrored", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("display", ResourceKind::Colour, ResourceFormat::RGBA16F);
		resource("scene-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("interface-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, 1, true);
		resource("composed-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);

		node("world", NodeScope::World);
		touches(EditKind::Writes, "world-entities", "entities");

		node("mesh-residency", NodeScope::World);
		touches(EditKind::Writes, "resident-meshes", "meshes");

		node("shadow", NodeScope::World);
		touches(EditKind::Reads, "world-entities", "entities");
		touches(EditKind::Reads, "resident-meshes", "meshes");
		touches(EditKind::Writes, "shadow", "shadow");

		node("skybox-compute", NodeScope::World);
		touches(EditKind::Writes, "environment-sky", "sky");

		node("clouds-compute", NodeScope::World);
		touches(EditKind::Reads, "environment-sky", "sky");
		touches(EditKind::Writes, "environment-clouds", "clouds");

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

		node("delta-upload", NodeScope::View);
		touches(EditKind::Reads, "resident-meshes", "meshes");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Writes, "view-instances", "instances");

		node("select-lod", NodeScope::View);
		touches(EditKind::Reads, "view-instances", "instances");
		touches(EditKind::Reads, "view-camera", "camera");
		touches(EditKind::Writes, "lod-instances", "instances");

		node("surface-capture", NodeScope::View);
		touches(EditKind::Reads, "world-entities", "world-state");
		touches(EditKind::Reads, "shadow", "shadow");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "lod-instances", "instances");
		touches(EditKind::Writes, "mirror-views", "surface");
		touches(EditKind::Writes, "portal-image", "portal");
		touches(EditKind::Writes, "portal-light", "light");

		node("gbuffer", NodeScope::View);
		touches(EditKind::Reads, "shadow", "shadow");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "lod-instances", "instances");
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
		// The seam light-field orders this pass after surface-capture, so
		// the projection samples this frame's captures rather than last frame's.
		touches(EditKind::Reads, "portal-light", "portal-light");
		touches(EditKind::Writes, "lit", "colour");

		node("sky", NodeScope::View);
		touches(EditKind::Reads, "lit", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "environment-clouds", "environment");
		touches(EditKind::Writes, "sky-lit", "colour");

		node("fog", NodeScope::View);
		touches(EditKind::Reads, "sky-lit", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Writes, "volume-lit", "colour");

		node("portal-overlay", NodeScope::View);
		touches(EditKind::Reads, "volume-lit", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "portal-image", "portal");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "lod-instances", "instances");
		touches(EditKind::Writes, "portaled", "colour");

		node("mirror-overlay", NodeScope::View);
		touches(EditKind::Reads, "portaled", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "mirror-views", "surface");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "lod-instances", "instances");
		touches(EditKind::Writes, "mirrored", "colour");

		node("transparent", NodeScope::View);
		touches(EditKind::Reads, "mirrored", "colour");
		touches(EditKind::Reads, "depth", "depth");
		touches(EditKind::Reads, "ordered-entities", "entities");
		touches(EditKind::Reads, "lod-instances", "instances");
		touches(EditKind::Writes, "display", "colour");

		node("shader-lenses", NodeScope::View);
		touches(EditKind::Reads, "display", "colour");
		touches(EditKind::Reads, "linear-depth", "depth");
		touches(EditKind::Writes, "lens-b", "colour");
		touches(EditKind::Writes, "lens-scratch", "scratch");

		node("tonemap", NodeScope::View);
		touches(EditKind::Reads, "lens-b", "colour");
		touches(EditKind::Writes, "tonemapped", "colour");

		node("present", NodeScope::Frame);
		touches(EditKind::Reads, "tonemapped", "image");
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

	PipelineDocument DefaultPbrDataCaptureDocument() {
		PipelineDocument document = DefaultPbrDocument();
		document.Record(
			{.Kind = EditKind::AddNode,
			 .Name = core::Name("data-capture"),
			 .NodeKind = core::Name("capture"),
			 .Scope = NodeScope::Frame}
		);
		for (const auto &[resource, port] : std::array<std::pair<const char *, const char *>, 3>{
				 {{"lit", "source"}, {"linear-depth", "depth"}, {"normal", "normal"}}
			 }) {
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name(port)}
			);
		}
		for (const auto &[node, resource] : std::array<std::pair<const char *, const char *>, 3>{
				 {{"data-capture-albedo", "albedo"},
				  {"data-capture-material", "material"},
				  {"data-capture-emissive", "emissive"}}
			 }) {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name(node),
				 .NodeKind = core::Name("capture"),
				 .Scope = NodeScope::Frame}
			);
			document.Record(
				{.Kind = EditKind::Reads, .Target = core::Name(resource), .Key = core::Name("source")}
			);
		}

		return document;
	}

	PipelineDocument DefaultPortalBodyDocument(
		bool seamProjection,
		bool orderedLayers,
		bool spatialOverlay,
		bool shaderLenses,
		bool nestedApertures,
		size_t transparentLayerCount,
		bool retainedAmbient,
		bool retainedDirectional
	) {
		PipelineDocument document;
		retainedAmbient = retainedAmbient || retainedDirectional;
		if (retainedAmbient) {
			for (const auto &[name, format] :
				 {std::pair{"ambient-depth", ResourceFormat::R32F},
				  std::pair{"ambient-normal", ResourceFormat::RGB10A2}}) {
				document.Record(
					{.Kind = EditKind::AddResource,
					 .Name = core::Name(name),
					 .Resource = ResourceKind::Colour,
					 .Format = format}
				);
			}
		}
		const auto base = DefaultPbrDocument();
		core::Name currentNode;
		std::vector<Edit> ambientShading;
		for (auto edit : base.Edits()) {
			if (edit.Kind == EditKind::AddNode) currentNode = edit.Name;
			if (edit.Kind == EditKind::AddNode && edit.Name == core::Name("sky")) break;
			if (retainedAmbient && currentNode == core::Name("ssao") && edit.Kind == EditKind::Reads) {
				if (edit.Key == core::Name("depth")) edit.Target = core::Name("ambient-depth");
				if (edit.Key == core::Name("normal")) edit.Target = core::Name("ambient-normal");
			}
			if (retainedAmbient &&
				(currentNode == core::Name("ssao") || currentNode == core::Name("deferred-lighting")))
				ambientShading.push_back(edit);
			else
				document.Record(edit);
		}
		const auto resource = [&](const char *name, ResourceFormat format) {
			document.Record(
				{.Kind = EditKind::AddResource,
				 .Name = core::Name(name),
				 .Resource = ResourceKind::Colour,
				 .Format = format}
			);
		};
		const auto node = [&](const char *name, const char *kind, NodeScope scope = NodeScope::View) {
			document.Record(
				{.Kind = EditKind::AddNode,
				 .Name = core::Name(name),
				 .NodeKind = core::Name(kind),
				 .Scope = scope}
			);
		};
		const auto edge = [&](EditKind kind, const char *name, const char *port) {
			document.Record({.Kind = kind, .Target = core::Name(name), .Key = core::Name(port)});
		};
		const auto appendOpaque = [&] {
			if (nestedApertures) {
				resource("aperture-colour", ResourceFormat::RGBA16F);
				node("portal-overlay", "portal-overlay");
				edge(EditKind::Reads, "lit", "colour");
				edge(EditKind::Reads, "depth", "depth");
				edge(EditKind::Reads, "portal-image", "portal");
				edge(EditKind::Reads, "ordered-entities", "entities");
				edge(EditKind::Reads, "view-instances", "instances");
				edge(EditKind::Writes, "aperture-colour", "colour");
			}
			resource("opaque-depth", ResourceFormat::R32F);
			node("opaque-depth-export", "depth-linearise");
			edge(EditKind::Reads, "depth", "depth");
			// Portal overlay writes shared hardware depth as well as its colour target.
			if (nestedApertures) edge(EditKind::Reads, "aperture-colour", "after-colour");
			edge(EditKind::Writes, "opaque-depth", "linear");
			document.Record({.Kind = EditKind::Set, .Key = core::Name("background"), .Value = "zero"});
		};
		if (!retainedAmbient) appendOpaque();
		resource("room-colour", ResourceFormat::RGBA16F);
		resource("room-depth", ResourceFormat::R32F);
		resource("composed-colour", ResourceFormat::RGBA16F);
		resource("composed-depth", ResourceFormat::R32F);
		if (retainedAmbient) {
			resource("room-normal", ResourceFormat::RGB10A2);
			resource("room-ambient-response", ResourceFormat::RGBA32F);
			resource("room-lighting-baseline", ResourceFormat::RGBA32F);
			if (retainedDirectional) resource("room-directional-response", ResourceFormat::RGBA32F);
		}
		node("room-image", "eye-image");
		document.Record({.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"});
		document.Record(
			{.Kind = EditKind::Set, .Key = core::Name("projection"), .Value = seamProjection ? "seam" : "eye"}
		);
		edge(EditKind::Writes, "room-depth", "depth");
		edge(EditKind::Writes, "room-colour", "colour");
		if (retainedAmbient) {
			edge(EditKind::Writes, "room-normal", "normal");
			edge(EditKind::Writes, "room-ambient-response", "ambient-response");
			edge(EditKind::Writes, "room-lighting-baseline", "lighting-baseline");
			if (retainedDirectional)
				edge(EditKind::Writes, "room-directional-response", "directional-response");
			node("ambient-merge", "ambient-merge");
			edge(EditKind::Reads, "linear-depth", "body-depth");
			edge(EditKind::Reads, "normal", "body-normal");
			edge(EditKind::Reads, "room-depth", "room-depth");
			edge(EditKind::Reads, "room-normal", "room-normal");
			edge(EditKind::Writes, "ambient-depth", "depth");
			edge(EditKind::Writes, "ambient-normal", "normal");
			for (const auto &edit : ambientShading)
				document.Record(edit);
			resource("room-ambient-colour", ResourceFormat::RGBA16F);
			node("ambient-correct", "ambient-correct");
			edge(EditKind::Reads, "room-lighting-baseline", "lighting-baseline");
			edge(EditKind::Reads, "room-ambient-response", "response");
			edge(EditKind::Reads, "occlusion", "occlusion");
			if (retainedDirectional) {
				edge(EditKind::Reads, "room-directional-response", "directional-response");
				edge(EditKind::Reads, "room-depth", "room-depth");
				edge(EditKind::Reads, "room-normal", "room-normal");
				edge(EditKind::Reads, "shadow", "shadow");
			}
			edge(EditKind::Writes, "room-ambient-colour", "colour");
		}
		if (retainedAmbient) appendOpaque();
		node("body-compose", "depth-compose");
		edge(EditKind::Reads, "room-depth", "background-depth");
		edge(EditKind::Reads, nestedApertures ? "aperture-colour" : "lit", "foreground");
		edge(EditKind::Reads, retainedAmbient ? "room-ambient-colour" : "room-colour", "background");
		edge(EditKind::Reads, "opaque-depth", "foreground-depth");
		edge(EditKind::Writes, "composed-depth", "depth");
		edge(EditKind::Writes, "composed-colour", "colour");
		std::string composedColour = "composed-colour", composedDepth = "composed-depth";
		if (orderedLayers) {
			for (int layer = static_cast<int>(std::min(transparentLayerCount, size_t{2})) - 1; layer >= 0;
				 --layer) {
				const auto name = "transparent-" + std::to_string(layer);
				const auto colour = name + "-colour", depth = name + "-depth";
				const auto outputColour = name + "-composed-colour", outputDepth = name + "-composed-depth";
				resource(colour.c_str(), ResourceFormat::RGBA16F);
				resource(depth.c_str(), ResourceFormat::R32F);
				resource(outputColour.c_str(), ResourceFormat::RGBA16F);
				resource(outputDepth.c_str(), ResourceFormat::R32F);
				node((name + "-image").c_str(), "eye-image");
				document.Record(
					{.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"}
				);
				document.Record(
					{.Kind = EditKind::Set,
					 .Key = core::Name("projection"),
					 .Value = seamProjection ? "seam" : "eye"}
				);
				document.Record({.Kind = EditKind::Set, .Key = core::Name("layer"), .Value = name});
				edge(EditKind::Writes, colour.c_str(), "colour");
				edge(EditKind::Writes, depth.c_str(), "depth");
				node((name + "-compose").c_str(), "depth-compose");
				document.Record({.Kind = EditKind::Set, .Key = core::Name("mode"), .Value = "premultiplied"});
				edge(EditKind::Reads, colour.c_str(), "foreground");
				edge(EditKind::Reads, depth.c_str(), "foreground-depth");
				edge(EditKind::Reads, composedColour.c_str(), "background");
				edge(EditKind::Reads, composedDepth.c_str(), "background-depth");
				edge(EditKind::Writes, outputColour.c_str(), "colour");
				edge(EditKind::Writes, outputDepth.c_str(), "depth");
				composedColour = outputColour;
				composedDepth = outputDepth;
			}
		}
		if (spatialOverlay) {
			resource("spatial-overlay-colour", ResourceFormat::RGBA16F);
			resource("overlay-composed-colour", ResourceFormat::RGBA16F);
			node("spatial-overlay-image", "eye-image");
			document.Record({.Kind = EditKind::Set, .Key = core::Name("scope"), .Value = "opaque-lighting"});
			document.Record(
				{.Kind = EditKind::Set,
				 .Key = core::Name("projection"),
				 .Value = seamProjection ? "seam" : "eye"}
			);
			document.Record({.Kind = EditKind::Set, .Key = core::Name("layer"), .Value = "spatial-overlay"});
			edge(EditKind::Writes, "spatial-overlay-colour", "colour");
			node("spatial-overlay-compose", "colour-compose");
			edge(EditKind::Reads, "spatial-overlay-colour", "foreground");
			edge(EditKind::Reads, composedColour.c_str(), "background");
			edge(EditKind::Writes, "overlay-composed-colour", "colour");
			composedColour = "overlay-composed-colour";
		}
		if (shaderLenses) {
			resource("body-lens-colour", ResourceFormat::RGBA16F);
			resource("body-lens-scratch", ResourceFormat::RGBA16F);
			node("shader-lenses", "shader-lenses");
			edge(EditKind::Reads, composedColour.c_str(), "colour");
			edge(EditKind::Reads, composedDepth.c_str(), "depth");
			edge(EditKind::Writes, "body-lens-colour", "colour");
			edge(EditKind::Writes, "body-lens-scratch", "scratch");
			composedColour = "body-lens-colour";
		}

		node("export", "capture", NodeScope::Frame);
		edge(EditKind::Reads, composedColour.c_str(), "source");
		edge(EditKind::Reads, composedDepth.c_str(), "depth");
		return document;
	}

	PipelineDocument DefaultEyeDocument() {
		PipelineDocument result;
		result.Record(
			{.Kind = EditKind::AddResource,
			 .Name = core::Name("eye-hdr"),
			 .Resource = ResourceKind::Colour,
			 .Format = ResourceFormat::RGBA16F}
		);
		const auto basis = DefaultPbrDocument();
		bool keep = false;
		for (auto edit : basis.Edits()) {
			if (edit.Kind == EditKind::AddResource) {
				if (edit.Name == core::Name("tonemapped") || edit.Name == core::Name("scene-image") ||
					edit.Name == core::Name("interface-image") || edit.Name == core::Name("composed-image"))
					result.Record(std::move(edit));
				continue;
			}
			if (edit.Kind == EditKind::AddNode) {
				keep = edit.Name == core::Name("tonemap") || edit.Name == core::Name("present") ||
					   edit.Name == core::Name("interface") || edit.Name == core::Name("overlay") ||
					   edit.Name == core::Name("output-image");
				if (edit.Name == core::Name("tonemap")) {
					result.Record(
						{.Kind = EditKind::AddNode,
						 .Name = core::Name("eye-image"),
						 .NodeKind = core::Name("eye-image"),
						 .Scope = NodeScope::View}
					);
					result.Record(
						{.Kind = EditKind::Writes,
						 .Target = core::Name("eye-hdr"),
						 .Key = core::Name("colour")}
					);
				}
			}
			if (!keep) continue;
			if (edit.Kind == EditKind::Reads && edit.Target == core::Name("lens-b"))
				edit.Target = core::Name("eye-hdr");
			if (edit.Kind == EditKind::Reads && edit.Target == core::Name("display"))
				edit.Target = core::Name("tonemapped");
			result.Record(std::move(edit));
		}
		return result;
	}

	PipelineDocument DefaultWorldHdrDocument() {
		// Native and captured worlds share the spatial chain before display encoding.
		return DefaultPbrDocument();
	}

	PipelineDocument DefaultPbrTierBDocument() {
		const PipelineDocument full = DefaultPbrDocument();
		PipelineDocument reduced;
		bool skipNode = false;
		for (const Edit &edit : full.Edits()) {
			if (edit.Kind == EditKind::AddResource) {
				skipNode = false;
				if (edit.Name == core::Name("depth-pyramid") || edit.Name == core::Name("occlusion") ||
					edit.Name == core::Name("lod-instances") || edit.Name == core::Name("environment-sky") ||
					edit.Name == core::Name("environment-clouds")) {
					continue;
				}
			}
			if (edit.Kind == EditKind::AddNode) {
				skipNode = edit.NodeKind == core::Name("hzb") || edit.NodeKind == core::Name("ssao") ||
						   edit.NodeKind == core::Name("select-lod") ||
						   edit.NodeKind == core::Name("skybox-compute") ||
						   edit.NodeKind == core::Name("clouds-compute");
			}
			if (skipNode ||
				(edit.Kind == EditKind::Reads && (edit.Target == core::Name("occlusion") ||
												  edit.Target == core::Name("environment-clouds")))) {
				continue;
			}
			Edit reducedEdit = edit;
			if (reducedEdit.Kind == EditKind::Reads && reducedEdit.Target == core::Name("lod-instances")) {
				reducedEdit.Target = core::Name("view-instances");
			}
			reduced.Record(std::move(reducedEdit));
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
		resource("resident-meshes", ResourceKind::Buffer, ResourceFormat::R8);
		resource("view-instances", ResourceKind::Buffer, ResourceFormat::R8);
		resource("forward-colour", ResourceKind::Colour, ResourceFormat::RGB10A2);
		resource("depth", ResourceKind::Depth, ResourceFormat::D24S8);
		resource("scene-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);
		resource("interface-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB, true);
		resource("composed-image", ResourceKind::Colour, ResourceFormat::RGBA8_SRGB);

		node("world", NodeScope::World);
		touches(EditKind::Writes, "world-entities", "entities");
		node("mesh-residency", NodeScope::World);
		touches(EditKind::Writes, "resident-meshes", "meshes");
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
		node("delta-upload", NodeScope::View);
		touches(EditKind::Reads, "resident-meshes", "meshes");
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
