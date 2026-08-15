#include <engine/core/Xml.hpp>
#include <engine/game/Xml.hpp>

#include <algorithm>
#include <cstdlib>

namespace engine::game {

	namespace {
		// What the scanner is told a save file may hold.
		//
		// **A namespace prefix is kept rather than dropped**, which is the one
		// setting here a reader could reasonably want the other way round. This
		// format writes no prefix, so `<x:Game>` is a document somebody else wrote
		// and reading it as `<Game>` would load it as though it were ours.
		core::xml::Options ScannerOptions(const XmlLimits &limits) {
			core::xml::Options options;
			options.Format = "game";
			options.MaximumAttributes = limits.MaximumAttributes;
			options.DropNamespacePrefix = false;
			return options;
		}

		// The scanner's refusal in this format's vocabulary.
		//
		// **`Refused` survives the trip and that is the whole reason the scanner
		// reports a kind at all.** A document that tried to declare an entity is a
		// different event from one that was truncated, and a mapping that flattened
		// both to "malformed" would bury the only status here that means somebody
		// tried something.
		XmlStatus StatusOf(core::xml::Fault fault) {
			switch (fault) {
			case core::xml::Fault::Truncated:
				return XmlStatus::Truncated;
			case core::xml::Fault::Refused:
				return XmlStatus::Refused;
			case core::xml::Fault::TooManyAttributes:
				return XmlStatus::TooManyAttributes;
			case core::xml::Fault::None:
			case core::xml::Fault::Malformed:
				break;
			}
			return XmlStatus::Malformed;
		}
	}

	const char *Describe(XmlStatus status) {
		switch (status) {
		case XmlStatus::Ok:
			return "ok";
		case XmlStatus::Truncated:
			return "the document ended in the middle of something";
		case XmlStatus::Malformed:
			return "the document is not well formed";
		case XmlStatus::Mismatched:
			return "a closing tag names something other than what was open";
		case XmlStatus::TooDeep:
			return "the document nests deeper than the limit allows";
		case XmlStatus::TooLarge:
			return "the document is larger than the limit allows";
		case XmlStatus::TooManyElements:
			return "the document holds more elements than the limit allows";
		case XmlStatus::TooManyAttributes:
			return "an element carries more attributes than the limit allows";
		case XmlStatus::Refused:
			return "the document uses a feature this reader refuses - a doctype, "
				   "an entity declaration, or an undeclared entity reference";
		}
		return "unknown";
	}

	std::string_view XmlElement::Attribute(std::string_view name, std::string_view fallback) const {
		for (size_t index = 0; index < AttributeNames.size(); index++) {
			if (AttributeNames[index] == name) {
				return AttributeValues[index];
			}
		}
		return fallback;
	}

	bool XmlElement::HasAttribute(std::string_view name) const {
		return std::find(AttributeNames.begin(), AttributeNames.end(), name) != AttributeNames.end();
	}

	XmlStatus ParseXml(std::string_view text, XmlDocument &out, const XmlLimits &limits, uint32_t *line) {
		out.Elements.clear();

		if (text.size() > limits.MaximumBytes) {
			if (line != nullptr) {
				*line = 0;
			}
			return XmlStatus::TooLarge;
		}

		const core::xml::Options options = ScannerOptions(limits);
		std::string_view rest = text;

		// The elements still open, innermost last, as indices into
		// `out.Elements`. **The stack is a vector and not the C stack**, which is
		// what makes a document nested a million deep a `TooDeep` instead of a
		// crash with no file named - and is why `MaximumDepth` is a number a
		// caller may raise.
		std::vector<uint32_t> open;

		core::xml::Failure failure;
		std::vector<core::xml::Attribute> attributes;
		std::string content;
		XmlStatus status = XmlStatus::Ok;

		for (;;) {
			if (open.empty()) {
				// **Nothing but whitespace may precede the root**, comments and
				// the declaration aside - the scanner steps over those. A
				// document that begins with character data is not one this format
				// wrote, and a CDATA section is character data however much of
				// the punctuation it borrows.
				const size_t markup = rest.find('<');
				const std::string_view before =
					markup == std::string_view::npos ? rest : rest.substr(0, markup);
				if (before.find_first_not_of(" \t\r\n\f") != std::string_view::npos ||
					rest.substr(before.size()).starts_with("<![CDATA[")) {
					status = XmlStatus::Malformed;
					break;
				}
				rest.remove_prefix(before.size());
			} else {
				// Text and CDATA, concatenated across the runs an element holds.
				// **The refusal is here rather than in a sweep over the whole
				// document**, and that is the difference `core/Xml.hpp` warns
				// against collapsing: a save file's scripts are CDATA, and a `&`
				// in Luau is an operator.
				if (!core::xml::ReadContent(rest, options, content, failure)) {
					status = StatusOf(failure.Reason);
					break;
				}
				out.Elements[open.back()].Text += content;
			}

			core::xml::Tag tag;
			const core::xml::Scan scan = core::xml::NextTag(rest, options, tag, failure);
			if (scan == core::xml::Scan::Error) {
				status = StatusOf(failure.Reason);
				break;
			}
			if (scan == core::xml::Scan::End) {
				// A document with no root and one that stops inside an element
				// are the same event: it ended in the middle of something.
				status = XmlStatus::Truncated;
				break;
			}

			if (tag.Closing) {
				if (open.empty() || !tag.Attributes.empty()) {
					status = XmlStatus::Malformed;
					break;
				}
				if (out.Elements[open.back()].Name != tag.Name) {
					status = XmlStatus::Mismatched;
					break;
				}

				open.pop_back();
				if (open.empty()) {
					// The root closed. Whatever follows it is not read, which is
					// what a save file's loader has always done.
					break;
				}
				continue;
			}

			// Both bounds are checked before the element is built rather than
			// after, so a document that trips one costs the check and not the
			// work.
			if (open.size() > limits.MaximumDepth) {
				status = XmlStatus::TooDeep;
				break;
			}
			if (out.Elements.size() >= limits.MaximumElements) {
				status = XmlStatus::TooManyElements;
				break;
			}
			if (!core::xml::ReadAttributes(tag.Attributes, options, attributes, failure)) {
				status = StatusOf(failure.Reason);
				break;
			}

			XmlElement element;
			element.Name = tag.Name;
			for (const core::xml::Attribute &attribute : attributes) {
				std::string value;
				if (!core::xml::Unescape(attribute.Value, options, value, failure)) {
					status = StatusOf(failure.Reason);
					break;
				}
				element.AttributeNames.emplace_back(attribute.Name);
				element.AttributeValues.push_back(std::move(value));
			}
			if (status != XmlStatus::Ok) {
				break;
			}

			// The index is taken before the push so that a child's is greater
			// than its parent's and the document reads in order.
			const uint32_t index = static_cast<uint32_t>(out.Elements.size());
			if (!open.empty()) {
				out.Elements[open.back()].Children.push_back(index);
			}
			out.Elements.push_back(std::move(element));

			if (!tag.SelfClosing) {
				open.push_back(index);
			} else if (open.empty()) {
				break;
			}
		}

		if (line != nullptr) {
			// Counted at the end rather than carried through the parse, because
			// the scanner works in views and a line number is wanted once, on a
			// document that failed.
			const std::string_view read = text.substr(0, text.size() - rest.size());
			*line = 1 + static_cast<uint32_t>(std::count(read.begin(), read.end(), '\n'));
		}

		if (status != XmlStatus::Ok) {
			// Emptied rather than left half built, for `ecs::Store::Load`'s
			// reason: a document that is partly one file looks like it works.
			out.Elements.clear();
			return status;
		}

		return XmlStatus::Ok;
	}

	std::string EscapeXml(std::string_view text) {
		std::string out;
		out.reserve(text.size());

		for (const char c : text) {
			switch (c) {
			case '<':
				out += "&lt;";
				break;
			case '>':
				out += "&gt;";
				break;
			case '&':
				out += "&amp;";
				break;
			case '"':
				out += "&quot;";
				break;
			case '\'':
				out += "&apos;";
				break;
			default:
				out.push_back(c);
				break;
			}
		}
		return out;
	}

	// -----------------------------------------------------------------------

	XmlWriter::XmlWriter() {
		Out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
	}

	void XmlWriter::Indent() {
		Out.append(Stack.size(), '\t');
	}

	void XmlWriter::EndOpenTag() {
		if (Pending) {
			Out += ">";
			Pending = false;
		}
	}

	void XmlWriter::Open(std::string_view name) {
		if (Pending) {
			EndOpenTag();
			Out += "\n";
		}

		Indent();
		Out += "<";
		Out.append(name);

		Stack.emplace_back(name);
		Pending = true;
		HasContent = false;
		InlineContent = false;
	}

	void XmlWriter::Attribute(std::string_view name, std::string_view value) {
		if (!Pending) {
			// Dropped rather than written, because an attribute after a child
			// produces a document that will not re-parse - and a save file that
			// cannot be loaded is worse than one that is missing a field.
			Broken = true;
			return;
		}

		Out += " ";
		Out.append(name);
		Out += "=\"";
		Out += EscapeXml(value);
		Out += "\"";
	}

	void XmlWriter::Text(std::string_view text) {
		if (Stack.empty()) {
			Broken = true;
			return;
		}

		EndOpenTag();
		Out += EscapeXml(text);
		HasContent = true;
		InlineContent = true;
	}

	void XmlWriter::Verbatim(std::string_view text) {
		if (Stack.empty()) {
			Broken = true;
			return;
		}

		EndOpenTag();
		Out += "<![CDATA[";

		// **The `]]>` split.** A program containing that sequence - two array
		// closes and a comparison, which is ordinary code - would otherwise end
		// the section early and put the rest of the file into the document as
		// markup. Ending and reopening mid-sequence is the standard answer, and
		// a reader that concatenates its text runs cannot tell it happened.
		size_t from = 0;
		for (;;) {
			const size_t found = text.find("]]>", from);
			if (found == std::string_view::npos) {
				Out.append(text.substr(from));
				break;
			}

			Out.append(text.substr(from, found + 2 - from));
			Out += "]]><![CDATA[";
			from = found + 2;
		}

		Out += "]]>";
		HasContent = true;
		InlineContent = true;
	}

	void XmlWriter::Close() {
		if (Stack.empty()) {
			Broken = true;
			return;
		}

		const std::string name = Stack.back();
		Stack.pop_back();

		if (Pending && !HasContent) {
			// Collapsed. A game file is mostly empty elements - a part with
			// default properties, a folder with no children - and `<Part />` is
			// half the bytes of the pair.
			Out += " />\n";
			Pending = false;
		} else {
			EndOpenTag();
			if (!InlineContent) {
				Indent();
			}
			Out += "</";
			Out += name;
			Out += ">\n";
		}

		// The parent now has content, and it is not text.
		HasContent = true;
		InlineContent = false;
	}

	const std::string &XmlWriter::Finish() {
		// An unbalanced document is a caller's bug, and closing the stack here
		// would hide it behind a file that loads and is missing whatever came
		// after the mistake.
		if (!Stack.empty()) {
			Broken = true;
		}
		return Out;
	}
}
