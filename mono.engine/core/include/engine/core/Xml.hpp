#pragma once

// The engine's XML reader: a tag scanner over a buffer, and a refusal for
// everything a document type declaration can do.
//
// **This is markup, not a document model.** There is no tree here, no
// namespaces, no schema and no entity table - a caller drives `NextTag` and
// keeps whatever stack its own format needs. That is enough for the three
// formats in this engine that are XML and is deliberately not enough for
// anything else.
//
// ## Why this is at L1, and why that is the whole point
//
// **The scanner sits at the bottom because it needs nothing.** It opens no
// file, links no vendor, allocates no global and names no other module's type -
// a `std::string_view` goes in and a `std::string_view` comes out. A module's
// height is the height of what it needs, and this needs the standard library.
//
// It was written three times before it was written here, and the tier is why:
// `game::ParseXml` reads the save format at L10, `Svg.cpp` scanned drawings at
// L9 from v0.13, and `bake/src/Xml.hpp` was extracted from that copy at v0.15
// when `.rbxmx` wanted markup too. `bake` cannot call `game` - `game` is L10 -
// so each of the three was a place the same refusals had to be kept true, and
// the second one to be edited is the one that gets forgotten. `D00128` closed
// by moving the reader *down* rather than by vendoring one, which is the move
// `assets::ResizeImage` made earlier in the same version.
//
// **`assets` was the other candidate and it is one tier too high.** `game` does
// not link `assets` and has no reason to: putting a string scanner there would
// have dragged content addressing, BLAKE3 and Crypto++ underneath the save
// format to gain a parser that uses none of them. `ResizeImage` went to
// `assets` because it is arithmetic over an `assets::TextureData` and followed
// its own dependency; this one has no dependency, so it followed it here.
//
// ## Why this is written rather than vendored
//
// `mono.vendor/AGENTS.md` prefers a submodule and puts the burden of proof on
// the alternative, so this is the argument. Three things carry it:
//
// - **A document is a thing a player can be sent.** The famous XML attacks -
//   entity expansion, external entities, quadratic blowup - are all attacks on
//   features a save file, a drawing and a model do not need. A parser that has
//   them and turns them off is safe by configuration, and safety that is a
//   default is safety somebody has to keep right for ever.
// - **What is refused cannot be misconfigured.** A `<!DOCTYPE` or `<!ENTITY` is
//   not parsed at all here: there is no code that could expand an entity, so
//   there is no option that could switch it on.
// - **Vendoring would not have removed a hand-written parser**, it would have
//   added a library beside one - this engine had three of its own before it had
//   one. Consolidating deletes code; vendoring would have added a dependency
//   and left the code.
//
// The trade this accepts is real and is worth writing down: a hand-written
// parser over hostile input is a liability, and it is paid for by keeping the
// grammar tiny, by bounding every count before it is used, and by
// `tests/Xml.cpp` here plus the suites at all three callers driving the three
// attacks rather than a note saying they were considered.
//
// ## The three attacks, and where each is stopped
//
// - **Entity expansion**, the billion laughs: a kilobyte of declarations that
//   unfolds into gigabytes while it is parsed. Stopped at the declaration -
//   `<!` is refused, so no entity can be defined - and again at the reference,
//   where anything but the five predefined names and a numeric character
//   reference is refused. Two locks on one door, because a reference that was
//   silently dropped instead would make a bomb look like a file with a typo in
//   it.
// - **External entities**, which are a file read performed by a reader that
//   never opens one. The same refusal: a `SYSTEM` identifier can only appear
//   inside a declaration, and there are none.
// - **Unbounded nesting.** Nothing here recurses - `NextTag` is a scan over a
//   `std::string_view` and the caller keeps the stack - so depth is the
//   caller's count to bound, and every caller bounds it. A parser that recursed
//   per element would put that bound on the C stack, where exceeding it is a
//   crash with no file named.
//
// ## Two refusal policies, and they are not the same policy
//
// `CheckEntityReferences` sweeps a whole document and `ReadContent` refuses at
// each point a reference is actually read, with CDATA exempt. **Collapsing them
// into one policy reintroduces a bug a real file found**: see the comment on
// each, which says which caller it is for and what breaks if it is given the
// other one.
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
