#include "Utf8.hpp"

#include <engine/core/Chars.hpp>
#include <engine/core/Log.hpp>
#include <engine/gui/RichText.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>

namespace engine::gui {

	namespace {
		// What a stretch of text looks like, as the parse walks it.
		//
		// **A stack of these rather than a flag set**, because tags nest: a
		// `<b>` inside a `<font>` inherits the colour and closing the `<b>`
		// has to put the weight back without touching it. Pushing a copy and
		// popping it is the whole of that rule.
		struct Style {
			core::Color3 Tint;
			float Transparency = 0.0f;
			int32_t Size = 0;
			FontFace Font = FontFace::Regular;
			bool Bold = false;
			bool Italic = false;
			bool Underline = false;
			bool Strike = false;
		};

		// Which face a weight and a slant ask for.
		//
		// **Four faces and not a family**, which is `FontFace`'s own rule: this
		// engine ships one face per role, so bold-italic resolves to bold. The
		// alternative is a fifth face nothing has, and a run that silently drew
		// nothing would be worse than one drawn in the nearer of the two.
		FontFace FaceFor(const Style &style) {
			if (style.Bold) {
				return FontFace::Bold;
			}
			if (style.Italic) {
				return FontFace::Italic;
			}
			return style.Font;
		}

		bool Styled(const Style &style, const Label &base) {
			return style.Bold || style.Italic || style.Underline || style.Strike || style.Size != 0 ||
				   FaceFor(style) != base.Font || style.Transparency != base.Transparency ||
				   style.Tint.R != base.Color.R || style.Tint.G != base.Color.G ||
				   style.Tint.B != base.Color.B;
		}

		// One `name="value"` pair, or nothing.
		//
		// Single and double quotes both, because a colour written with an
		// apostrophe is a string somebody pasted out of a document and not a
		// mistake worth refusing.
		bool NextAttribute(std::string_view &rest, std::string_view &name, std::string_view &value) {
			size_t at = 0;
			while (at < rest.size() && std::isspace(static_cast<unsigned char>(rest[at])) != 0) {
				at++;
			}
			if (at >= rest.size()) {
				return false;
			}

			const size_t nameStart = at;
			while (at < rest.size() && rest[at] != '=' &&
				   std::isspace(static_cast<unsigned char>(rest[at])) == 0) {
				at++;
			}
			name = rest.substr(nameStart, at - nameStart);

			while (at < rest.size() && std::isspace(static_cast<unsigned char>(rest[at])) != 0) {
				at++;
			}
			if (at >= rest.size() || rest[at] != '=') {
				return false;
			}
			at++;

			while (at < rest.size() && std::isspace(static_cast<unsigned char>(rest[at])) != 0) {
				at++;
			}
			if (at >= rest.size() || (rest[at] != '"' && rest[at] != '\'')) {
				return false;
			}

			const char quote = rest[at];
			at++;
			const size_t valueStart = at;
			while (at < rest.size() && rest[at] != quote) {
				at++;
			}
			if (at >= rest.size()) {
				return false;
			}

			value = rest.substr(valueStart, at - valueStart);
			rest = rest.substr(at + 1);
			return true;
		}

		// `#RRGGBB`, `#RGB` or `rgb(r,g,b)`, which is the set Roblox accepts.
		bool ParseColour(std::string_view text, core::Color3 &out) {
			const auto channel = [](unsigned value) { return static_cast<float>(value) / 255.0f; };

			if (!text.empty() && text.front() == '#') {
				const std::string_view digits = text.substr(1);
				if (digits.size() != 6 && digits.size() != 3) {
					return false;
				}

				unsigned parts[3]{};
				for (size_t index = 0; index < 3; index++) {
					const size_t width = digits.size() == 6 ? 2 : 1;
					const std::string_view piece = digits.substr(index * width, width);
					unsigned value = 0;
					const auto *end = piece.data() + piece.size();
					if (std::from_chars(piece.data(), end, value, 16).ptr != end) {
						return false;
					}

					// `#abc` is `#aabbcc`, which is CSS's rule and the one every
					// author who writes three digits means.
					parts[index] = width == 1 ? value * 17 : value;
				}

				out = core::Color3{channel(parts[0]), channel(parts[1]), channel(parts[2])};
				return true;
			}

			if (text.starts_with("rgb(") && text.back() == ')') {
				std::string_view body = text.substr(4, text.size() - 5);
				unsigned parts[3]{};
				for (unsigned &part : parts) {
					while (!body.empty() && (body.front() == ' ' || body.front() == ',')) {
						body = body.substr(1);
					}
					const auto *end = body.data() + body.size();
					const auto result = std::from_chars(body.data(), end, part);
					if (result.ec != std::errc{}) {
						return false;
					}
					body = body.substr(static_cast<size_t>(result.ptr - body.data()));
				}
				out = core::Color3{channel(parts[0]), channel(parts[1]), channel(parts[2])};
				return true;
			}

			return false;
		}

		// The named entities, plus the numeric form.
		//
		// @return How many bytes of `source` the entity took, or zero when it is
		//         not one - in which case the caller writes the `&` literally,
		//         because an ampersand in ordinary prose is not an error.
		size_t Entity(std::string_view source, std::string &out) {
			struct Named {
				std::string_view Text;
				char Replacement;
			};
			constexpr Named NAMED[]{
				{"&lt;", '<'},
				{"&gt;", '>'},
				{"&amp;", '&'},
				{"&quot;", '"'},
				{"&apos;", '\''},
			};

			for (const Named &entity : NAMED) {
				if (source.starts_with(entity.Text)) {
					out.push_back(entity.Replacement);
					return entity.Text.size();
				}
			}

			if (!source.starts_with("&#")) {
				return 0;
			}

			const size_t close = source.find(';');
			if (close == std::string_view::npos || close <= 2) {
				return 0;
			}

			const std::string_view digits = source.substr(2, close - 2);
			unsigned codepoint = 0;
			const auto *end = digits.data() + digits.size();
			if (std::from_chars(digits.data(), end, codepoint).ptr != end) {
				return 0;
			}

			// UTF-8, written out rather than reached for: this module has no
			// encoder and one entity is not a reason to grow `core` a
			// dependency.
			if (codepoint < 0x80) {
				out.push_back(static_cast<char>(codepoint));
			} else if (codepoint < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			} else if (codepoint < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			} else if (codepoint <= 0x10FFFF) {
				out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			} else {
				return 0;
			}

			return close + 1;
		}
	}

	bool ParseRichText(
		std::string_view source, const Label &base, std::string &plain, std::vector<DrawSpan> &spans
	) {
		plain.clear();
		spans.clear();

		Style root;
		root.Tint = base.Color;
		root.Transparency = base.Transparency;
		root.Font = base.Font;

		std::vector<Style> stack{root};
		size_t spanStart = 0;

		// Closes the run that ended here, if it was styled at all.
		const auto flush = [&] {
			const Style &style = stack.back();
			if (plain.size() > spanStart && Styled(style, base)) {
				DrawSpan span;
				span.Begin = static_cast<uint32_t>(spanStart);
				span.End = static_cast<uint32_t>(plain.size());
				span.Tint = style.Tint;
				span.Transparency = style.Transparency;
				span.Size = style.Size;
				span.Font = FaceFor(style);
				span.Underline = style.Underline;
				span.Strike = style.Strike;
				spans.push_back(span);
			}
			spanStart = plain.size();
		};

		// **Every failure below restores the input**, because half a parse is
		// worse than none: a caller shown a partly-stripped string cannot tell
		// whether the markup ran out or the text did.
		// Declared before `refuse` so the diagnostic can say where the parse
		// gave up; the walk below is what advances it.
		size_t at = 0;

		const auto refuse = [&] {
			// **The whole string is shown with its tags literal.** That is the
			// documented behaviour and it is indistinguishable from text that
			// was meant to contain angle brackets, so a mistyped tag renders
			// wrong and nothing says why.
			ENGINE_DEBUG_EVERY(5.0, "rich text refused at byte {} of {}; shown literally", at, source.size());
			plain.assign(source);
			spans.clear();
			return false;
		};

		while (at < source.size()) {
			const char here = source[at];

			if (here == '&') {
				if (const size_t taken = Entity(source.substr(at), plain); taken > 0) {
					at += taken;
					continue;
				}
				plain.push_back(here);
				at++;
				continue;
			}

			if (here != '<') {
				plain.push_back(here);
				at++;
				continue;
			}

			const size_t close = source.find('>', at);
			if (close == std::string_view::npos) {
				return refuse();
			}

			std::string_view tag = source.substr(at + 1, close - at - 1);
			at = close + 1;

			const bool closing = !tag.empty() && tag.front() == '/';
			if (closing) {
				tag = tag.substr(1);
			}

			// A self-closing marker is stripped before the name is read, so
			// `<br/>` and `<br />` are one case rather than three.
			bool selfClosing = false;
			if (!tag.empty() && tag.back() == '/') {
				selfClosing = true;
				tag = tag.substr(0, tag.size() - 1);
			}
			while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.back())) != 0) {
				tag = tag.substr(0, tag.size() - 1);
			}

			size_t nameEnd = 0;
			while (nameEnd < tag.size() && std::isspace(static_cast<unsigned char>(tag[nameEnd])) == 0) {
				nameEnd++;
			}
			const std::string_view name = tag.substr(0, nameEnd);
			std::string_view attributes = tag.substr(nameEnd);

			if (name == "br") {
				flush();
				plain.push_back('\n');
				spanStart = plain.size();
				continue;
			}

			if (closing) {
				if (stack.size() <= 1) {
					return refuse();
				}
				flush();
				stack.pop_back();
				continue;
			}

			flush();
			Style style = stack.back();

			if (name == "b") {
				style.Bold = true;
			} else if (name == "i") {
				style.Italic = true;
			} else if (name == "u") {
				style.Underline = true;
			} else if (name == "s") {
				style.Strike = true;
			} else if (name == "font") {
				std::string_view key;
				std::string_view value;
				while (NextAttribute(attributes, key, value)) {
					if (key == "color") {
						core::Color3 tint;
						if (!ParseColour(value, tint)) {
							return refuse();
						}
						style.Tint = tint;
					} else if (key == "size") {
						int32_t size = 0;
						const auto *end = value.data() + value.size();
						if (std::from_chars(value.data(), end, size).ptr != end || size <= 0) {
							return refuse();
						}
						style.Size = size;
					} else if (key == "transparency") {
						float fade = 0.0f;
						const auto *end = value.data() + value.size();
						if (core::FromChars(value.data(), end, fade).ptr != end) {
							return refuse();
						}
						style.Transparency = std::clamp(fade, 0.0f, 1.0f);
					} else if (key == "face" || key == "family") {
						// By role, which is the only vocabulary this engine's
						// atlas has. An unknown face is left alone rather than
						// refused: a place file naming a Roblox font should
						// still show its words.
						if (value == "Bold") {
							style.Bold = true;
						} else if (value == "Italic") {
							style.Italic = true;
						} else if (value == "Code" || value == "RobotoMono" || value == "Monospace") {
							style.Font = FontFace::Code;
						}
					}
				}
			} else {
				// **An unknown tag is a refusal and not a silent strip.** A
				// misspelled `<bold>` that vanished would look like the text
				// vanished; shown literally, it says exactly what is wrong.
				return refuse();
			}

			if (selfClosing) {
				// It opened and closed in one go, so it styles nothing. Keeping
				// it on the stack would leave every following tag one level
				// deeper than the author wrote.
				continue;
			}

			stack.push_back(style);
		}

		if (stack.size() != 1) {
			return refuse();
		}

		flush();
		return true;
	}

	std::string_view FirstCharacters(std::string_view text, int32_t count) {
		if (count < 0) {
			return text;
		}
		if (count == 0) {
			return {};
		}

		// `ByteOffset` counts from one, which is `Entry::CursorPosition`'s
		// numbering - so the offset after `count` characters is the position
		// `count + 1` starts at.
		return text.substr(0, ByteOffset(text, count + 1));
	}
}
