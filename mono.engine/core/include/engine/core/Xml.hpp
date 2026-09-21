#pragma once

// A bounded XML scanner for formats that need tags, attributes and text.
//
// This is not a document model. Callers drive `NextTag` and own any tree or
// nesting limit their format needs. The scanner opens no files, uses no vendor
// library and returns views into the supplied input.
//
// Declarations, external entities and entity definitions are refused by
// construction. `CheckEntityReferences` validates a whole document, while
// `ReadContent` validates references in text and leaves CDATA unchanged.
//
// @tier L1 · shared
// @since v0.15

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core::xml {

	// The kind of a refusal, apart from the sentence describing it.
	//
	// **Separate from the message so that a caller can act on one**, which is
	// what lets `game::ParseXml` keep the save format's own statuses over a
	// scanner that knows nothing about save files. A caller that only reports
	// the failure reads `Failure::Message` and ignores this.
	//
	// @since v0.15
	enum class Fault : uint8_t {
		// No failure.
		None,

		// A tag, an attribute or a reference that is not well formed.
		Malformed,

		// The document ended in the middle of something.
		Truncated,

		// A feature this reader refuses: a declaration, or a reference to an
		// entity that would have needed one.
		//
		// **Its own kind and not `Malformed`**, because this is the only one
		// that means somebody tried something rather than that something went
		// wrong. Burying it in the general failure means nobody sees it.
		Refused,

		// More attributes on one element than `Options::MaximumAttributes`.
		TooManyAttributes,
	};

	// Why a document was refused, and what to tell whoever wrote it.
	//
	// @since v0.15
	struct Failure {
		// The kind, for a caller that switches on it.
		Fault Reason = Fault::None;

		// The sentence a person reads, prefixed with `Options::Format`.
		std::string Message;
	};

	// What a failure message calls the format, and what one element may carry.
	//
	// **The format's name is a parameter so that a message reads as the
	// decoder's own.** "svg: a tag is never closed" is what an author of a
	// drawing needs; "xml: a tag is never closed" would leave them wondering
	// which of their files it was about.
	//
	// @since v0.15
	struct Options {
		// The prefix every failure this scanner writes begins with.
		std::string_view Format = "xml";

		// Attributes one element may carry, checked before the vector grows.
		uint32_t MaximumAttributes = 64;

		// Whether `<svg:rect>` is read as `<rect>`.
		//
		// **Off by default, because dropping a prefix is a claim about the
		// document.** A format with one vocabulary - a drawing, a model - means
		// nothing by a prefix and is easier to read without one. A format that
		// might mean something by it has to see it, so the default is the name
		// as written.
		bool DropNamespacePrefix = false;
	};

	// One `name="value"` pair, pointing into the document.
	//
	// @since v0.15
	struct Attribute {
		// The attribute's name, as written. No prefix is dropped: a prefix on an
		// attribute is meaningful where a prefix on an element is not.
		std::string_view Name;

		// Its value, still escaped. `Unescape` is the caller's to run on the
		// ones it actually uses.
		std::string_view Value;
	};

	// One tag, pointing into the document.
	//
	// @since v0.15
	struct Tag {
		// The element's name, with any namespace prefix dropped when
		// `Options::DropNamespacePrefix` asked for that.
		std::string_view Name;

		// The raw attribute run, for `ReadAttributes`. Left unparsed because a
		// caller that only wants the name should not pay for a vector.
		std::string_view Attributes;

		// Whether this is `</name>`.
		bool Closing = false;

		// Whether this is `<name/>`, which opens and closes in one tag.
		bool SelfClosing = false;
	};

	// What `NextTag` found.
	//
	// @since v0.15
	enum class Scan : uint8_t {
		// A tag, in `tag`.
		Tag,

		// The end of the document, with no tag left.
		End,

		// Markup this refuses, with `failure` saying which.
		Error,
	};

	// The name with any namespace prefix dropped.
	//
	// @param name The name as written.
	// @return Everything after the first colon, or the whole name.
	std::string_view WithoutPrefix(std::string_view name);

	// Advances to the next tag, stepping over character data, comments,
	// processing instructions and CDATA sections.
	//
	// **A `<!DOCTYPE` or `<!ENTITY` is an error and not a skip**, which is this
	// file's whole security position - see the header comment. `<![CDATA[` is
	// the one `<!` that is not a declaration and is stepped over as the
	// character data it is.
	//
	// @param[in,out] text    Consumed up to and including the tag returned.
	// @param         options What the format is called and what it allows.
	// @param[out]    tag     Filled when the result is `Scan::Tag`.
	// @param[out]    failure Set when the result is `Scan::Error`.
	// @return Whether a tag was found, the document ended, or neither.
	Scan NextTag(std::string_view &text, const Options &options, Tag &tag, Failure &failure);

	// Splits a `Tag::Attributes` run into pairs.
	//
	// @param      text    The run, as `Tag::Attributes` gives it.
	// @param      options What the format is called and what it allows.
	// @param[out] out     Cleared, then filled.
	// @param[out] failure Set when this returns `false`.
	// @return `false` on a name that is not a name, an unquoted value, an
	//         unterminated one, a `<` inside one, or more than
	//         `Options::MaximumAttributes` of them.
	bool ReadAttributes(
		std::string_view text, const Options &options, std::vector<Attribute> &out, Failure &failure
	);

	// The attribute of that name, or `nullptr`.
	//
	// @param attributes The run `ReadAttributes` filled.
	// @param name       The attribute to look for.
	// @return The attribute, or `nullptr` when the element did not carry it.
	const Attribute *Find(const std::vector<Attribute> &attributes, std::string_view name);

	// Refuses every entity reference in `text` that is not one of the five XML
	// predefines or a numeric character reference.
	//
	// **For a caller that never unescapes**, which is the SVG rasteriser: it
	// uses attribute values as written, so a reference has to be refused by a
	// sweep rather than at the point it would have been expanded. A caller that
	// runs `Unescape` over everything it reads has the same protection from
	// that, and does not want this - see `ReadContent` on why a sweep is wrong
	// once a document may hold CDATA.
	//
	// @param      text    The whole document, or as much of it as is text.
	// @param      options What the format is called and what it allows.
	// @param[out] failure Set when this returns `false`.
	// @return `false` on the first reference that is not read here.
	bool CheckEntityReferences(std::string_view text, const Options &options, Failure &failure);

	// Expands the five predefined entities and numeric character references, and
	// refuses anything else.
	//
	// @param      text    The escaped text.
	// @param      options What the format is called and what it allows.
	// @param[out] out     Appended to, not cleared.
	// @param[out] failure Set when this returns `false`.
	// @return `false` on a reference this does not read.
	bool Unescape(std::string_view text, const Options &options, std::string &out, Failure &failure);

	// Reads character data up to the next tag, CDATA sections included.
	//
	// **Character data is unescaped and a CDATA section is taken verbatim**,
	// which is what makes a sweep over the whole document the wrong tool for a
	// format that has one: a real `.rbxmx` in this engine's own test corpus
	// carries the Luau pattern `"[&;]"` inside a script's CDATA, and a document
	// -wide entity sweep refuses that file while naming an entity nobody wrote.
	// A save file's scripts are the same shape and take the same route.
	//
	// @param[in,out] text    Left positioned at the `<` that ended the run, so
	//                        the next `NextTag` sees it.
	// @param         options What the format is called and what it allows.
	// @param[out]    out     Cleared, then filled.
	// @param[out]    failure Set when this returns `false`.
	// @return `false` on a reference this does not read, or an unclosed CDATA
	//         section.
	bool ReadContent(std::string_view &text, const Options &options, std::string &out, Failure &failure);
}
