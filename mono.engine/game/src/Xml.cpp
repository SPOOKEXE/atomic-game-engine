#include <engine/game/Xml.hpp>

#include <algorithm>
#include <cstdlib>

namespace engine::game {

	namespace {
		bool IsSpace(char c) {
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		// XML's name rule, narrowed. Letters, digits, underscore, hyphen, dot
		// and colon — and colon only because it is legal, not because anything
		// here means anything by it. Namespaces are not supported and a
		// document using one gets a tag whose name happens to contain a colon.
		bool IsNameStart(char c) {
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
		}

		bool IsNameChar(char c) {
			return IsNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
		}

		// Appends one code point as UTF-8.
		//
		// Numeric character references are the one place a document names a
		// character rather than writing it, and a game file that round-tripped
		// a player's name through `&#233;` and got a byte back would be a save
		// file that corrupts non-ASCII text.
		void AppendUtf8(std::string &out, uint32_t code) {
			if (code < 0x80) {
				out.push_back(static_cast<char>(code));
			} else if (code < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (code >> 6)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
			} else if (code < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (code >> 12)));
				out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (code >> 18)));
				out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
			}
		}

		// The whole parser's state, so that the recursive descent below carries
		// one thing rather than seven.
		struct Parser {
			std::string_view Text;
			size_t At = 0;
			uint32_t Line = 1;
			XmlLimits Limits;
			XmlDocument *Out = nullptr;
			XmlStatus Status = XmlStatus::Ok;

			bool Done() const {
				return At >= Text.size();
			}

			char Peek(size_t ahead = 0) const {
				return At + ahead < Text.size() ? Text[At + ahead] : '\0';
			}

			bool Starts(std::string_view prefix) const {
				return Text.compare(At, prefix.size(), prefix) == 0;
			}

			void Advance(size_t count = 1) {
				for (size_t index = 0; index < count && At < Text.size(); index++) {
					if (Text[At] == '\n') {
						Line++;
					}
					At++;
				}
			}

			void SkipSpace() {
				while (!Done() && IsSpace(Peek())) {
					Advance();
				}
			}

			bool Fail(XmlStatus why) {
				// First failure wins. A later one is a consequence of this one,
				// and reporting it would send a reader to the wrong line.
				if (Status == XmlStatus::Ok) {
					Status = why;
				}
				return false;
			}

			std::string ReadName() {
				std::string name;
				if (Done() || !IsNameStart(Peek())) {
					return name;
				}
				while (!Done() && IsNameChar(Peek())) {
					name.push_back(Peek());
					Advance();
				}
				return name;
			}

			// One entity reference, already past the ampersand.
			bool ReadEntity(std::string &out) {
				if (Starts("&lt;")) {
					out.push_back('<');
					Advance(4);
					return true;
				}
				if (Starts("&gt;")) {
					out.push_back('>');
					Advance(4);
					return true;
				}
				if (Starts("&amp;")) {
					out.push_back('&');
					Advance(5);
					return true;
				}
				if (Starts("&quot;")) {
					out.push_back('"');
					Advance(6);
					return true;
				}
				if (Starts("&apos;")) {
					out.push_back('\'');
					Advance(6);
					return true;
				}

				if (Starts("&#")) {
					Advance(2);
					const int base = Peek() == 'x' || Peek() == 'X' ? 16 : 10;
					if (base == 16) {
						Advance();
					}

					uint32_t code = 0;
					bool any = false;
					while (!Done() && Peek() != ';') {
						const char c = Peek();
						uint32_t digit = 0;
						if (c >= '0' && c <= '9') {
							digit = static_cast<uint32_t>(c - '0');
						} else if (base == 16 && c >= 'a' && c <= 'f') {
							digit = static_cast<uint32_t>(c - 'a') + 10;
						} else if (base == 16 && c >= 'A' && c <= 'F') {
							digit = static_cast<uint32_t>(c - 'A') + 10;
						} else {
							return Fail(XmlStatus::Malformed);
						}

						// Bounded before it overflows rather than after. Unicode
						// stops at 0x10FFFF, so anything past it is a malformed
						// reference and not a character nobody can render.
						code = code * static_cast<uint32_t>(base) + digit;
						if (code > 0x10FFFF) {
							return Fail(XmlStatus::Malformed);
						}
						any = true;
						Advance();
					}

					if (!any || Done()) {
						return Fail(Done() ? XmlStatus::Truncated : XmlStatus::Malformed);
					}
					Advance();
					AppendUtf8(out, code);
					return true;
				}

				// **Anything else is a refusal, not a parse error.** A document
				// referencing `&payload;` is a document expecting an entity
				// declaration it was not allowed to make, which is the XXE
				// attempt this parser exists to have no answer to.
				return Fail(XmlStatus::Refused);
			}

			// Text up to the next `<`, entities resolved.
			bool ReadText(std::string &out) {
				while (!Done() && Peek() != '<') {
					if (Peek() == '&') {
						if (!ReadEntity(out)) {
							return false;
						}
						continue;
					}
					out.push_back(Peek());
					Advance();
				}
				return true;
			}

			// An attribute value, already past the name and the equals sign.
			bool ReadAttributeValue(std::string &out) {
				const char quote = Peek();
				if (quote != '"' && quote != '\'') {
					return Fail(XmlStatus::Malformed);
				}
				Advance();

				while (!Done() && Peek() != quote) {
					if (Peek() == '&') {
						if (!ReadEntity(out)) {
							return false;
						}
						continue;
					}
					if (Peek() == '<') {
						return Fail(XmlStatus::Malformed);
					}
					out.push_back(Peek());
					Advance();
				}

				if (Done()) {
					return Fail(XmlStatus::Truncated);
				}
				Advance();
				return true;
			}

			// Comments, the declaration, and the things this parser refuses.
			//
			// Returns false on a refusal or a malformed prologue; sets `skipped`
			// when something was consumed, so the caller can tell "nothing here"
			// from "something was here and is now gone".
			bool SkipProlog(bool &skipped) {
				skipped = false;

				if (Starts("<!--")) {
					Advance(4);
					while (!Done() && !Starts("-->")) {
						Advance();
					}
					if (Done()) {
						return Fail(XmlStatus::Truncated);
					}
					Advance(3);
					skipped = true;
					return true;
				}

				if (Starts("<?")) {
					// The XML declaration, and nothing else. A processing
					// instruction is a directive to whatever is reading the
					// document, and this parser has no business obeying one.
					Advance(2);
					while (!Done() && !Starts("?>")) {
						Advance();
					}
					if (Done()) {
						return Fail(XmlStatus::Truncated);
					}
					Advance(2);
					skipped = true;
					return true;
				}

				if (Starts("<!DOCTYPE") || Starts("<!ENTITY") || Starts("<!ELEMENT") || Starts("<!ATTLIST") ||
					Starts("<!NOTATION")) {
					// **The refusal this file exists for.** A DOCTYPE is where
					// every entity-expansion attack is declared, and there is no
					// version of "support it safely" that is simpler than not
					// supporting it.
					return Fail(XmlStatus::Refused);
				}

				return true;
			}

			// One element, already positioned at its `<`.
			bool ReadElement(uint32_t depth, uint32_t &index) {
				if (depth > Limits.MaximumDepth) {
					return Fail(XmlStatus::TooDeep);
				}
				if (Out->Elements.size() >= Limits.MaximumElements) {
					return Fail(XmlStatus::TooManyElements);
				}

				Advance();

				XmlElement element;
				element.Name = ReadName();
				if (element.Name.empty()) {
					return Fail(XmlStatus::Malformed);
				}

				// Reserved before the children are read, so a child's index is
				// greater than its parent's and the document reads in order.
				index = static_cast<uint32_t>(Out->Elements.size());
				Out->Elements.push_back(std::move(element));

				bool selfClosing = false;
				for (;;) {
					SkipSpace();
					if (Done()) {
						return Fail(XmlStatus::Truncated);
					}

					if (Starts("/>")) {
						Advance(2);
						selfClosing = true;
						break;
					}
					if (Peek() == '>') {
						Advance();
						break;
					}

					const std::string name = ReadName();
					if (name.empty()) {
						return Fail(XmlStatus::Malformed);
					}

					SkipSpace();
					if (Peek() != '=') {
						return Fail(XmlStatus::Malformed);
					}
					Advance();
					SkipSpace();

					std::string value;
					if (!ReadAttributeValue(value)) {
						return false;
					}

					Out->Elements[index].AttributeNames.push_back(name);
					Out->Elements[index].AttributeValues.push_back(std::move(value));
				}

				if (selfClosing) {
					return true;
				}

				for (;;) {
					if (Done()) {
						return Fail(XmlStatus::Truncated);
					}

					if (Peek() != '<') {
						std::string text;
						if (!ReadText(text)) {
							return false;
						}
						Out->Elements[index].Text += text;
						continue;
					}

					if (Starts("</")) {
						Advance(2);
						const std::string closing = ReadName();
						SkipSpace();
						if (Peek() != '>') {
							return Fail(Done() ? XmlStatus::Truncated : XmlStatus::Malformed);
						}
						Advance();
						if (closing != Out->Elements[index].Name) {
							return Fail(XmlStatus::Mismatched);
						}
						return true;
					}

					if (Starts("<![CDATA[")) {
						Advance(9);
						const size_t start = At;
						while (!Done() && !Starts("]]>")) {
							Advance();
						}
						if (Done()) {
							return Fail(XmlStatus::Truncated);
						}

						// Appended raw. **This is what makes the writer's `]]>`
						// split invisible**: two adjacent sections concatenate
						// into the text that was written, and nothing here has
						// to know the split happened.
						Out->Elements[index].Text.append(Text.substr(start, At - start));
						Advance(3);
						continue;
					}

					bool skipped = false;
					if (!SkipProlog(skipped)) {
						return false;
					}
					if (skipped) {
						continue;
					}

					uint32_t child = 0;
					if (!ReadElement(depth + 1, child)) {
						return false;
					}
					Out->Elements[index].Children.push_back(child);
				}
			}
		};
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
		case XmlStatus::Refused:
			return "the document uses a feature this reader refuses — a doctype, "
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

		Parser parser;
		parser.Text = text;
		parser.Limits = limits;
		parser.Out = &out;

		// The prologue: whitespace, comments and the declaration, before the
		// root. A DOCTYPE would be here too, and is where it is refused.
		for (;;) {
			parser.SkipSpace();
			if (parser.Done()) {
				parser.Fail(XmlStatus::Truncated);
				break;
			}

			bool skipped = false;
			if (!parser.SkipProlog(skipped)) {
				break;
			}
			if (skipped) {
				continue;
			}

			if (parser.Peek() != '<') {
				parser.Fail(XmlStatus::Malformed);
				break;
			}

			uint32_t root = 0;
			parser.ReadElement(0, root);
			break;
		}

		if (line != nullptr) {
			*line = parser.Line;
		}

		if (parser.Status != XmlStatus::Ok) {
			// Emptied rather than left half built, for `ecs::Store::Load`'s
			// reason: a document that is partly one file looks like it works.
			out.Elements.clear();
			return parser.Status;
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
			// produces a document that will not re-parse — and a save file that
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

		// **The `]]>` split.** A program containing that sequence — two array
		// closes and a comparison, which is ordinary code — would otherwise end
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
			// Collapsed. A game file is mostly empty elements — a part with
			// default properties, a folder with no children — and `<Part />` is
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
