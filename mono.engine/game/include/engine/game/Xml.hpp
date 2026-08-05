#pragma once

// A deliberately small XML reader and writer.
//
// **The subset is the feature.** A game file is read from disk, and a game file
// is a thing a player can be sent — so the parser is an attack surface, and the
// famous XML attacks are all attacks on features this file does not have. No
// DTD, so no billion laughs and no quadratic blowup. No external entities, so
// no XXE and no file read through a document. No processing instructions past
// the declaration, no namespaces, no schema. What is left is elements,
// attributes, text, CDATA and the five predefined entities, which is everything
// a save file needs and nothing an exploit does.
//
// The refusals are **counted separately from ordinary parse errors** for the
// reason `assets::Grant` counts forged tokens apart from expired ones: a
// document that tried to declare an entity is a different event from one that
// was truncated, and burying the first in the second means nobody sees it.
//
// **This is not a general XML library and must not grow into one.** If
// something needs namespaces, it needs a different format rather than this file
// needing a feature. The whole argument for hand-writing a parser instead of
// vendoring one is that the grammar fits on a page and can be read.
//
// @tier L10 · shared

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace engine::game {

	// Why a document was refused.
	//
	// @since v0.7
	enum class XmlStatus : uint8_t {
		// Parsed.
		Ok,

		// Ran out of input in the middle of something.
		Truncated,

		// A tag, attribute or entity that is not well formed.
		Malformed,

		// A closing tag naming something other than what was open.
		Mismatched,

		// Nesting past `XmlLimits::MaximumDepth`.
		//
		// Its own status rather than `Malformed`, because a deeply nested
		// document is well formed and is exactly what a stack-exhaustion attack
		// looks like. A refusal that said "malformed" would send whoever read
		// the log looking for a typo.
		TooDeep,

		// More bytes than `XmlLimits::MaximumBytes`.
		TooLarge,

		// More elements than `XmlLimits::MaximumElements`.
		TooManyElements,

		// A DOCTYPE, an entity declaration, or anything else that would let a
		// document reach outside itself.
		//
		// **Counted apart from `Malformed` on purpose.** This is the only status
		// here that means somebody tried something, rather than that something
		// went wrong.
		Refused,
	};

	// A stable, human-readable name for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(XmlStatus status);

	// What a document may not exceed.
	//
	// **Passed in rather than compiled in**, so a test can drive a refusal with
	// a three-element document instead of building a hundred megabytes to prove
	// the limit exists. A limit that can only be reached by a real attack is a
	// limit nobody has ever seen work.
	//
	// @since v0.7
	struct XmlLimits {
		// The largest document, in bytes. 64 MiB.
		size_t MaximumBytes = 64u * 1024u * 1024u;

		// The deepest nesting. A game file's tree is instances inside
		// instances, so this is a real limit on content rather than a
		// formality — but it is far past any hand-built hierarchy.
		uint32_t MaximumDepth = 256;

		// The largest element count.
		uint32_t MaximumElements = 4u * 1000u * 1000u;
	};

	// One parsed element.
	//
	// Children and attributes are indices into the document's flat arrays
	// rather than pointers, so the tree survives the vectors growing while it
	// is being built — the same reason `world::Universe` holds its worlds by
	// pointer, arrived at from the other direction.
	//
	// @since v0.7
	struct XmlElement {
		// The tag name.
		std::string Name;

		// The element's own text, with entities resolved and CDATA inlined.
		//
		// **Concatenated across the element's text runs and CDATA sections**,
		// which is what makes the `]]>` split in the writer invisible to a
		// reader. Only meaningful for an element with no child elements; a
		// document that mixes text and elements is legal XML and is not
		// something this format produces.
		std::string Text;

		// Attribute names, parallel to `AttributeValues`.
		std::vector<std::string> AttributeNames;

		// Attribute values, with entities resolved.
		std::vector<std::string> AttributeValues;

		// Indices into `XmlDocument::Elements`, in document order.
		std::vector<uint32_t> Children;

		// The value of one attribute.
		//
		// @param name     The attribute to read.
		// @param fallback What to return when it is absent.
		// @return The value, or `fallback`.
		std::string_view Attribute(std::string_view name, std::string_view fallback = {}) const;

		// Whether an attribute is present.
		//
		// @param name The attribute to test.
		// @return `true` when the document carried it.
		bool HasAttribute(std::string_view name) const;
	};

	// A parsed document.
	//
	// @since v0.7
	struct XmlDocument {
		// Every element, in the order they were opened. Index 0 is the root
		// when `Elements` is not empty.
		std::vector<XmlElement> Elements;

		// The root, or `nullptr` for an empty document.
		//
		// @return The root element.
		const XmlElement *Root() const {
			return Elements.empty() ? nullptr : &Elements.front();
		}

		// One element by index.
		//
		// @param index The index, as a parent's `Children` holds it.
		// @return The element, or `nullptr` when the index is out of range.
		const XmlElement *At(uint32_t index) const {
			return index < Elements.size() ? &Elements[index] : nullptr;
		}
	};

	// Parses a document.
	//
	// @param text   The whole document.
	// @param out    Filled in on success, and left empty on any failure — for
	//               the reason `ecs::Store::Load` empties a store rather than
	//               half restoring it.
	// @param limits What the document may not exceed.
	// @param line   Set to the 1-based line a failure was found on, when one is.
	// @return `Ok`, or why not.
	XmlStatus
	ParseXml(std::string_view text, XmlDocument &out, const XmlLimits &limits = {}, uint32_t *line = nullptr);

	// Builds a document, one element at a time.
	//
	// Deliberately a writer rather than a tree: a save file is written once,
	// top to bottom, and building a document object model first would be a
	// second copy of what the store already holds.
	//
	// @since v0.7
	class XmlWriter {
	  public:
		// Creates a writer holding the XML declaration and nothing else.
		XmlWriter();

		// Opens an element. Every one needs a matching `Close`.
		//
		// @param name The tag name.
		void Open(std::string_view name);

		// Writes an attribute on the element most recently opened.
		//
		// Must come before any child or text, which is XML's rule rather than
		// this class's. An attribute written after a child is dropped and
		// counted, rather than producing a document that will not re-parse.
		//
		// @param name  The attribute name.
		// @param value The value, escaped here.
		void Attribute(std::string_view name, std::string_view value);

		// Writes text into the element most recently opened, escaped.
		//
		// @param text The text.
		void Text(std::string_view text);

		// Writes text as a CDATA section, for a program rather than a value.
		//
		// **Splits on `]]>` rather than refusing it.** A script containing that
		// sequence is legal code — it is two array closes and a comparison —
		// and a save format that mangled it would corrupt a file silently. The
		// standard trick is to end the section and open another mid-sequence,
		// and a reader concatenating its runs cannot tell.
		//
		// @param text The program.
		void Verbatim(std::string_view text);

		// Closes the element most recently opened.
		void Close();

		// The document, valid once every `Open` has been closed.
		//
		// @return The whole document, including the declaration.
		const std::string &Finish();

		// Whether anything was dropped — an attribute written after a child, or
		// a `Close` with nothing open.
		//
		// @return `true` when the document is not what the caller asked for.
		bool Failed() const {
			return Broken;
		}

	  private:
		void Indent();
		void EndOpenTag();

		std::string Out;
		std::vector<std::string> Stack;

		// Whether the innermost element's opening tag is still open, so an
		// attribute is still legal.
		bool Pending = false;

		// Whether the innermost element has content, so `Close` writes a
		// closing tag rather than collapsing to `<x />`.
		bool HasContent = false;

		// Whether the innermost element's content is text, so `Close` does not
		// indent the closing tag onto its own line and change the value.
		bool InlineContent = false;

		bool Broken = false;
	};

	// Escapes the five characters XML reserves.
	//
	// @param text The raw text.
	// @return The escaped text.
	std::string EscapeXml(std::string_view text);
}
