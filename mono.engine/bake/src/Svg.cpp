#include "Decoders.hpp"

#include <engine/core/Xml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A rasteriser for the part of SVG that is shapes, and a refusal for the rest.
//
// **SVG is a document format with a layout engine in it, and this is not one.**
// Text needs fonts and shaping; gradients, filters and masks need an offscreen
// compositor; `<use>` and `xlink:href` need a reference graph; a `style`
// attribute needs CSS. Every one of those, half-implemented, produces a picture
// that is recognisably the right drawing and wrong — the failure
// `bake/AGENTS.md` refuses interlaced PNG and progressive JPEG over. So the
// subset below is drawn tightly and **everything outside it is refused by
// name**, which is what makes a refusal a message somebody can act on rather
// than a silently different icon.
//
// What is read: `<svg>` with `width`/`height`/`viewBox`, `<g>`, `<rect>`,
// `<circle>`, `<ellipse>`, `<line>`, `<polyline>`, `<polygon>` and `<path>`
// with M, L, H, V, C and Z; `fill`, `stroke`, `fill-opacity`,
// `stroke-opacity`, `stroke-width`, `fill-rule` and a `transform` of
// `translate` and `scale`. `<title>`, `<desc>` and `<metadata>` are skipped
// whole, because they carry no marks.
//
// **The bomb here is XML, not the geometry.** A `<!DOCTYPE>` with an `<!ENTITY>`
// in it is the billion-laughs expansion and the external-entity file read, and
// both are refused outright rather than bounded — there is nothing an SVG icon
// needs a document type declaration for. Everything else an uploaded file states
// is a count, and every count is checked before it is used: the markup's length,
// the element count, the nesting depth, the path command count, the flattened
// point count and the raster target.

namespace engine::bake {

	// The scanner is `core`'s since v0.15 — `D00128`, and `core/Xml.hpp` carries
	// why it is at the bottom rather than beside a format that reads markup.
	namespace xml = core::xml;

	namespace {

		// **Every one of these bounds sits on a number somebody uploaded**, which
		// is `bake/AGENTS.md`'s standing rule. A document that trips one is
		// refused by name rather than clamped, because a clamped drawing is a
		// wrong drawing that looks like a setting.

		// The markup itself. Four megabytes is far past any icon and is the
		// ceiling the element and point counts below have to fit inside.
		constexpr size_t MAXIMUM_DOCUMENT_BYTES = 4u * 1024u * 1024u;

		// Elements opened, over the whole document. A refusal here is what stops
		// a hundred megabytes of `<rect/>` from being a hundred megabytes of
		// work — the byte bound alone would still allow it.
		constexpr uint32_t MAXIMUM_ELEMENTS = 4096;

		// How deep `<g>` may nest. The walk keeps an explicit stack rather than
		// recursing, so this bounds memory rather than the C stack — but a
		// drawing nested thirty-two deep is a generator's output and not a
		// drawing, and the bound is what says so.
		constexpr uint32_t MAXIMUM_DEPTH = 32;

		// Commands in one `d`, and attributes on one element.
		//@{
		constexpr uint32_t MAXIMUM_PATH_COMMANDS = 8192;
		constexpr uint32_t MAXIMUM_ATTRIBUTES = 64;
		//@}

		// Flattened points, over the whole document. **The one bound that is not
		// on a stated count**: a curve's point count is chosen here, from how big
		// it lands on the canvas, so this is the ceiling on what those choices
		// may add up to. Half a million of them is eight megabytes of vertices,
		// and what they *cost to draw* is the separate budget below.
		constexpr size_t MAXIMUM_POINTS = 1u << 19;

		// The largest canvas this will rasterise into.
		//
		// **Tighter than `Texture::MAXIMUM_DIMENSION` squared, and deliberately.**
		// A rasteriser allocates the whole canvas before it draws anything and
		// holds four floats a pixel so that a stack of translucent shapes
		// composites without drift — four megapixels is sixty-four megabytes of
		// working set, and an SVG is an icon or a panel rather than a sixteen-k
		// sheet. Each axis is still checked against `MAXIMUM_DIMENSION`, so the
		// refusal names whichever bound was actually hit.
		constexpr uint32_t MAXIMUM_RASTER_PIXELS = 1u << 22;

		// The work every fill in one document may do, added up.
		//
		// **The bound the other counts do not imply.** Four thousand elements is
		// cheap and half a million points is cheap; four thousand *four-point
		// rectangles that each cover the whole canvas* is neither, and nothing
		// above notices — a rectangle is four points however big it is. A single
		// path of half a million edges crossing every scanline is the same
		// problem from the other end, and a point bound tight enough to stop it
		// would refuse ordinary illustrations. So the work itself is budgeted, in
		// the two units it is actually done in: a fill is charged its bounding
		// box, plus one for every scanline sample each of its edges is crossed
		// at.
		//
		// **Sixty-seven million, measured rather than guessed.** Three files
		// through `assetc` on the `dev` preset, which is `-O0`: a 512-pixel
		// illustration of nine hundred stroked circles spends 34M of it and takes
		// 0.3s; four thousand stacked full-canvas rectangles at 2048 are refused
		// after 0.8s; the single densest fill this admits — 7400 edges crossing
		// all 2048 rows — takes 10s, which is the honest ceiling on what one
		// hostile file costs here. Raising it raises that ceiling in proportion.
		constexpr uint64_t MAXIMUM_FILL_WORK = 1u << 26;

		// The most segments one full ellipse is flattened to.
		constexpr uint32_t MAXIMUM_ARC_SEGMENTS = 256;

		// Vertices a round join is drawn with. Eight is enough at the widths a
		// stroke is actually used at and is the same shape at every scale.
		constexpr uint32_t JOIN_SEGMENTS = 8;

		// Scanline samples a pixel row is split into vertically. Coverage across
		// a row is exact — a span contributes its clipped length — so this is
		// only the vertical resolution of an edge, and four is where the
		// remaining stair-stepping stops being visible.
		constexpr uint32_t SUBSAMPLES = 4;

		// ---------------------------------------------------------------------
		// Text scanning
		// ---------------------------------------------------------------------

		bool IsSpace(char character) {
			return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
				   character == '\f';
		}

		bool IsDigit(char character) {
			return character >= '0' && character <= '9';
		}

		char Lowered(char character) {
			return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a')
														  : character;
		}

		// Whitespace or a comma, which is how every SVG number list separates.
		void SkipSeparators(std::string_view &text) {
			while (!text.empty() && (IsSpace(text.front()) || text.front() == ',')) {
				text.remove_prefix(1);
			}
		}

		std::string_view Trimmed(std::string_view text) {
			while (!text.empty() && IsSpace(text.front())) {
				text.remove_prefix(1);
			}
			while (!text.empty() && IsSpace(text.back())) {
				text.remove_suffix(1);
			}
			return text;
		}

		bool EqualsIgnoringCase(std::string_view text, std::string_view lowercase) {
			if (text.size() != lowercase.size()) {
				return false;
			}
			for (size_t index = 0; index < text.size(); index++) {
				if (Lowered(text[index]) != lowercase[index]) {
					return false;
				}
			}
			return true;
		}

		// One SVG number, advancing past it.
		//
		// **Hand-written rather than `from_chars`**, which is the house style
		// here and also the portable answer: floating-point `from_chars` is a
		// library version question, and `strtod` reads the process locale — a
		// decimal comma would turn `0.5` into `0` on somebody's machine and
		// nowhere else.
		bool TakeNumber(std::string_view &text, double &out) {
			size_t index = 0;
			bool negative = false;
			if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
				negative = text[index] == '-';
				index++;
			}

			double value = 0.0;
			bool digits = false;
			while (index < text.size() && IsDigit(text[index])) {
				value = value * 10.0 + static_cast<double>(text[index] - '0');
				index++;
				digits = true;
			}
			if (index < text.size() && text[index] == '.') {
				index++;
				double scale = 0.1;
				while (index < text.size() && IsDigit(text[index])) {
					value += static_cast<double>(text[index] - '0') * scale;
					scale *= 0.1;
					index++;
					digits = true;
				}
			}
			if (!digits) {
				return false;
			}

			if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
				size_t exponentIndex = index + 1;
				bool exponentNegative = false;
				if (exponentIndex < text.size() &&
					(text[exponentIndex] == '+' || text[exponentIndex] == '-')) {
					exponentNegative = text[exponentIndex] == '-';
					exponentIndex++;
				}

				int exponent = 0;
				bool exponentDigits = false;
				while (exponentIndex < text.size() && IsDigit(text[exponentIndex])) {
					// Saturated rather than accumulated without limit: an
					// exponent of a thousand digits is still one number, and the
					// finiteness check below is what refuses it.
					exponent = std::min(exponent * 10 + (text[exponentIndex] - '0'), 9999);
					exponentIndex++;
					exponentDigits = true;
				}

				// An `e` with no digits after it is not part of the number — it
				// is the next token, which in a path is a command letter.
				if (exponentDigits) {
					value *= std::pow(10.0, exponentNegative ? -exponent : exponent);
					index = exponentIndex;
				}
			}

			if (!std::isfinite(value)) {
				return false;
			}

			out = negative ? -value : value;
			text.remove_prefix(index);
			return true;
		}

		// A length as an attribute spells one.
		//
		// **Only unitless and `px`**, because everything else needs a context
		// this has no way to know: `em` needs a font, `%` needs a viewport that
		// is not the one being rasterised into, and `mm` needs a physical DPI.
		bool
		ParseLength(std::string_view text, std::string_view property, double &out, std::string &failure) {
			std::string_view rest = Trimmed(text);
			if (!TakeNumber(rest, out)) {
				failure =
					"svg: " + std::string(property) + " '" + std::string(Trimmed(text)) + "' is not a number";
				return false;
			}

			rest = Trimmed(rest);
			if (rest.empty() || rest == "px") {
				return true;
			}

			failure = "svg: " + std::string(property) + " '" + std::string(Trimmed(text)) +
					  "' — only unitless and px lengths are read, and %, em, ex, pt, pc, cm, mm and in "
					  "are refused";
			return false;
		}

		// ---------------------------------------------------------------------
		// Geometry
		// ---------------------------------------------------------------------

		struct Point {
			double X = 0.0;
			double Y = 0.0;
		};

		// A translate and a scale, and nothing else.
		//
		// **Not a 2x3 matrix, because `rotate`, `matrix` and the skews are
		// refused** — carrying the general form would be an invitation to accept
		// them without deciding to, and the pair below is exactly what the
		// supported `transform` functions and the `viewBox` fit produce.
		struct Transform {
			double ScaleX = 1.0;
			double ScaleY = 1.0;
			double OffsetX = 0.0;
			double OffsetY = 0.0;

			Point Apply(Point point) const {
				return {point.X * ScaleX + OffsetX, point.Y * ScaleY + OffsetY};
			}

			// The larger axis, which is what a curve's flattening is measured
			// against — one segment per device pixel of the bigger direction.
			double DeviceScale() const {
				return std::max(std::abs(ScaleX), std::abs(ScaleY));
			}
		};

		// `outer` applied after `inner`, which is the order a child's own
		// transform composes with its parent's.
		Transform Compose(const Transform &outer, const Transform &inner) {
			return {
				outer.ScaleX * inner.ScaleX,
				outer.ScaleY * inner.ScaleY,
				outer.OffsetX + outer.ScaleX * inner.OffsetX,
				outer.OffsetY + outer.ScaleY * inner.OffsetY,
			};
		}

		// One run of points. Filling closes it whatever `Closed` says; stroking
		// is what the flag is for.
		struct SubPath {
			std::vector<Point> Points;
			bool Closed = false;
		};

		struct Colour {
			double Red = 0.0;
			double Green = 0.0;
			double Blue = 0.0;
		};

		// What a shape is painted with, inherited down the element stack.
		struct Paint {
			bool FillPainted = true;
			Colour Fill;
			double FillOpacity = 1.0;
			bool FillEvenOdd = false;

			bool StrokePainted = false;
			Colour Stroke;
			double StrokeOpacity = 1.0;
			double StrokeWidth = 1.0;
		};

		// ---------------------------------------------------------------------
		// Colour
		// ---------------------------------------------------------------------

		// The names a hand-authored icon actually uses.
		//
		// **A short table and a refusal, rather than all hundred and forty-seven
		// CSS names.** A name this does not know is refused by name, so the file
		// that needs `rebeccapurple` says so in one line instead of arriving grey.
		constexpr std::array<std::pair<std::string_view, uint32_t>, 22> NAMED_COLOURS{{
			{"black", 0x000000},   {"silver", 0xC0C0C0},  {"gray", 0x808080},  {"grey", 0x808080},
			{"white", 0xFFFFFF},   {"maroon", 0x800000},  {"red", 0xFF0000},   {"purple", 0x800080},
			{"fuchsia", 0xFF00FF}, {"magenta", 0xFF00FF}, {"green", 0x008000}, {"lime", 0x00FF00},
			{"olive", 0x808000},   {"yellow", 0xFFFF00},  {"navy", 0x000080},  {"blue", 0x0000FF},
			{"teal", 0x008080},	   {"aqua", 0x00FFFF},	  {"cyan", 0x00FFFF},  {"orange", 0xFFA500},
			{"pink", 0xFFC0CB},	   {"brown", 0xA52A2A},
		}};

		Colour FromPacked(uint32_t packed) {
			return {
				static_cast<double>((packed >> 16) & 0xFFu) / 255.0,
				static_cast<double>((packed >> 8) & 0xFFu) / 255.0,
				static_cast<double>(packed & 0xFFu) / 255.0,
			};
		}

		bool HexDigit(char character, uint32_t &out) {
			if (IsDigit(character)) {
				out = static_cast<uint32_t>(character - '0');
				return true;
			}
			const char lowered = Lowered(character);
			if (lowered >= 'a' && lowered <= 'f') {
				out = static_cast<uint32_t>(lowered - 'a') + 10u;
				return true;
			}
			return false;
		}

		// A paint value: a colour, or `none`.
		//
		// @param property Which attribute this is, so the refusal names it.
		// @param painted  Set to `false` for `none` and `transparent`.
		bool ParseColour(
			std::string_view text, std::string_view property, bool &painted, Colour &out, std::string &failure
		) {
			const std::string_view value = Trimmed(text);
			const auto refuse = [&](std::string_view because) {
				failure = "svg: " + std::string(property) + " '" + std::string(value) + "' — " +
						  std::string(because);
				return false;
			};

			if (value.empty()) {
				return refuse("a paint has to say something");
			}
			if (EqualsIgnoringCase(value, "none") || EqualsIgnoringCase(value, "transparent")) {
				painted = false;
				return true;
			}
			if (EqualsIgnoringCase(value, "currentcolor")) {
				// `currentColor` reads the inherited `color` property, which is a
				// CSS cascade this does not have.
				return refuse("currentColor is refused, because it names a CSS property this does not read");
			}
			if (value.size() >= 4 && EqualsIgnoringCase(value.substr(0, 4), "url(")) {
				return refuse("gradients and patterns are refused: a paint server is a second renderer");
			}

			painted = true;

			if (value.front() == '#') {
				const std::string_view digits = value.substr(1);
				if (digits.size() != 3 && digits.size() != 6) {
					return refuse("only #rgb and #rrggbb are read");
				}

				uint32_t packed = 0;
				for (const char character : digits) {
					uint32_t nibble = 0;
					if (!HexDigit(character, nibble)) {
						return refuse("that is not a hexadecimal colour");
					}
					packed = (packed << 4) | nibble;
				}
				if (digits.size() == 3) {
					// `#abc` is `#aabbcc`, not `#0abc`.
					packed = ((packed & 0xF00u) * 0x1100u) | ((packed & 0x0F0u) * 0x110u) |
							 ((packed & 0x00Fu) * 0x11u);
				}
				out = FromPacked(packed);
				return true;
			}

			if (value.size() > 4 && EqualsIgnoringCase(value.substr(0, 4), "rgb(") && value.back() == ')') {
				std::string_view arguments = value.substr(4, value.size() - 5);
				double channels[3] = {0.0, 0.0, 0.0};

				for (double &channel : channels) {
					SkipSeparators(arguments);
					if (!TakeNumber(arguments, channel)) {
						return refuse("rgb() wants three numbers");
					}
					if (!arguments.empty() && arguments.front() == '%') {
						arguments.remove_prefix(1);
						channel = channel * 255.0 / 100.0;
					}
					channel = std::clamp(channel, 0.0, 255.0) / 255.0;
				}

				SkipSeparators(arguments);
				if (!arguments.empty()) {
					return refuse("rgb() wants three numbers");
				}

				out = {channels[0], channels[1], channels[2]};
				return true;
			}

			for (const auto &[name, packed] : NAMED_COLOURS) {
				if (EqualsIgnoringCase(value, name)) {
					out = FromPacked(packed);
					return true;
				}
			}

			return refuse("not a colour name this knows — #rgb, #rrggbb and rgb() always work");
		}

		// ---------------------------------------------------------------------
		// The XML subset
		// ---------------------------------------------------------------------
		//
		// **`core::xml` is the scanner and this is only its settings.** It was a
		// copy living here until v0.13 and this module's own `Xml.hpp` until
		// v0.15; each move happened when another format wanted markup and two
		// places to keep a DOCTYPE refused became one too many. `core/Xml.hpp`
		// carries the argument for writing one at all rather than vendoring it.

		using xml::Attribute;
		using xml::Find;
		using xml::Scan;
		using xml::Tag;

		// What a failure here is called, what one element may carry, and that a
		// prefix means nothing to a drawing — `<svg:rect>` is a `<rect>`.
		constexpr xml::Options XML{"svg", MAXIMUM_ATTRIBUTES, true};

		// The scanner with this format's settings already bound, so that a call
		// site says what it is asking for rather than repeating the settings.
		//
		// **The refusal's kind is dropped and its sentence kept.** A drawing that
		// is refused is refused; nothing here does anything different for a
		// DOCTYPE than for a tag with no name, which is what `game` needs the
		// kind for.
		//@{
		Scan NextTag(std::string_view &text, Tag &tag, std::string &failure) {
			xml::Failure refusal;
			const Scan scan = xml::NextTag(text, XML, tag, refusal);
			if (scan == Scan::Error) {
				failure = std::move(refusal.Message);
			}
			return scan;
		}

		bool ReadAttributes(std::string_view text, std::vector<Attribute> &out, std::string &failure) {
			xml::Failure refusal;
			if (xml::ReadAttributes(text, XML, out, refusal)) {
				return true;
			}
			failure = std::move(refusal.Message);
			return false;
		}

		bool CheckEntityReferences(std::string_view text, std::string &failure) {
			xml::Failure refusal;
			if (xml::CheckEntityReferences(text, XML, refusal)) {
				return true;
			}
			failure = std::move(refusal.Message);
			return false;
		}
		//@}

		// Attributes that would change the drawing, and that this does not do.
		//
		// **Refused by name rather than ignored**, which is the whole difference
		// between a message and a wrong icon: a shape carrying `opacity="0.2"`
		// drawn opaque is not "close enough", and a `style` attribute holding
		// `fill:red` silently drawn black is a bug report about the decoder.
		constexpr std::array<std::pair<std::string_view, std::string_view>, 16> REFUSED_ATTRIBUTES{{
			{"style", "presentation is read from attributes, and a style attribute is CSS"},
			{"class", "presentation is read from attributes, and a class needs a stylesheet"},
			{"opacity", "group opacity needs an offscreen layer; fill-opacity and stroke-opacity work"},
			{"filter", "filters are a second renderer"},
			{"mask", "masks need an offscreen layer"},
			{"clip-path", "clipping needs an offscreen layer"},
			{"display", "display and visibility are a cascade this does not have"},
			{"visibility", "display and visibility are a cascade this does not have"},
			{"stroke-dasharray", "dashes are refused"},
			{"stroke-dashoffset", "dashes are refused"},
			{"stroke-linecap", "caps are butt and joins are round, and neither is configurable here"},
			{"stroke-linejoin", "caps are butt and joins are round, and neither is configurable here"},
			{"stroke-miterlimit", "caps are butt and joins are round, and neither is configurable here"},
			{"preserveAspectRatio", "the viewBox is always fitted uniformly and centred"},
			{"vector-effect", "a stroke scales with its shape"},
			{"paint-order", "fill is drawn under stroke"},
		}};

		bool RefuseUnsupportedAttributes(
			const std::vector<Attribute> &attributes, std::string_view element, std::string &failure
		) {
			for (const Attribute &attribute : attributes) {
				for (const auto &[name, because] : REFUSED_ATTRIBUTES) {
					if (attribute.Name == name) {
						failure = "svg: <" + std::string(element) + "> carries '" + std::string(name) +
								  "' — " + std::string(because);
						return false;
					}
				}
			}
			return true;
		}

		// ---------------------------------------------------------------------
		// Presentation and transform
		// ---------------------------------------------------------------------

		bool ReadPaint(const std::vector<Attribute> &attributes, Paint &ink, std::string &failure) {
			if (const Attribute *fill = Find(attributes, "fill")) {
				if (!ParseColour(fill->Value, "fill", ink.FillPainted, ink.Fill, failure)) {
					return false;
				}
			}
			if (const Attribute *stroke = Find(attributes, "stroke")) {
				if (!ParseColour(stroke->Value, "stroke", ink.StrokePainted, ink.Stroke, failure)) {
					return false;
				}
			}
			if (const Attribute *width = Find(attributes, "stroke-width")) {
				if (!ParseLength(width->Value, "stroke-width", ink.StrokeWidth, failure)) {
					return false;
				}
				ink.StrokeWidth = std::max(0.0, ink.StrokeWidth);
			}
			if (const Attribute *opacity = Find(attributes, "fill-opacity")) {
				if (!ParseLength(opacity->Value, "fill-opacity", ink.FillOpacity, failure)) {
					return false;
				}
				ink.FillOpacity = std::clamp(ink.FillOpacity, 0.0, 1.0);
			}
			if (const Attribute *opacity = Find(attributes, "stroke-opacity")) {
				if (!ParseLength(opacity->Value, "stroke-opacity", ink.StrokeOpacity, failure)) {
					return false;
				}
				ink.StrokeOpacity = std::clamp(ink.StrokeOpacity, 0.0, 1.0);
			}
			if (const Attribute *rule = Find(attributes, "fill-rule")) {
				const std::string_view value = Trimmed(rule->Value);
				if (value == "nonzero") {
					ink.FillEvenOdd = false;
				} else if (value == "evenodd") {
					ink.FillEvenOdd = true;
				} else {
					failure = "svg: fill-rule '" + std::string(value) + "' is neither nonzero nor evenodd";
					return false;
				}
			}
			return true;
		}

		// `translate` and `scale`, composed left to right.
		//
		// **`rotate`, `matrix`, `skewX` and `skewY` are refused by name.** They
		// are not hard — they are a 2x3 matrix — but a rotated stroke is an
		// elliptical pen and a rotated ellipse is no longer axis-aligned, so
		// accepting them would quietly widen what every shape below has to
		// handle. The refusal is the honest bound.
		bool ParseTransform(std::string_view text, Transform &out, std::string &failure) {
			Transform combined;

			while (true) {
				SkipSeparators(text);
				if (text.empty()) {
					break;
				}

				const size_t open = text.find('(');
				if (open == std::string_view::npos) {
					failure = "svg: transform '" + std::string(Trimmed(text)) + "' is not a transform list";
					return false;
				}

				const std::string_view name = Trimmed(text.substr(0, open));
				text.remove_prefix(open + 1);

				const size_t close = text.find(')');
				if (close == std::string_view::npos) {
					failure = "svg: transform '" + std::string(name) + "(' is never closed";
					return false;
				}

				std::string_view arguments = text.substr(0, close);
				text.remove_prefix(close + 1);

				double first = 0.0;
				double second = 0.0;
				SkipSeparators(arguments);
				const bool hasFirst = TakeNumber(arguments, first);
				SkipSeparators(arguments);
				const bool hasSecond = TakeNumber(arguments, second);
				SkipSeparators(arguments);

				if (!hasFirst || !arguments.empty()) {
					failure = "svg: transform '" + std::string(name) + "' has arguments this cannot read";
					return false;
				}

				Transform local;
				if (name == "translate") {
					local.OffsetX = first;
					local.OffsetY = hasSecond ? second : 0.0;
				} else if (name == "scale") {
					local.ScaleX = first;
					local.ScaleY = hasSecond ? second : first;
				} else {
					failure = "svg: transform '" + std::string(name) +
							  "' is refused — only translate and scale are read, because a rotation or a "
							  "skew turns a stroke into an elliptical pen";
					return false;
				}

				combined = Compose(combined, local);
			}

			out = combined;
			return true;
		}

		// ---------------------------------------------------------------------
		// Shapes, all built in user space
		// ---------------------------------------------------------------------

		// What one document has left to spend, so it cannot flatten or fill its
		// way past what a bake is willing to do however it is spelled.
		struct Budget {
			size_t Points = MAXIMUM_POINTS;
			uint64_t FillWork = MAXIMUM_FILL_WORK;

			bool Take(size_t count, std::string &failure) {
				if (count > Points) {
					failure = "svg: past " + std::to_string(MAXIMUM_POINTS) +
							  " flattened points — the drawing is too dense to rasterise";
					Points = 0;
					return false;
				}
				Points -= count;
				return true;
			}

			bool TakeFillWork(uint64_t count, std::string &failure) {
				if (count > FillWork) {
					failure = "svg: past " + std::to_string(MAXIMUM_FILL_WORK) +
							  " units of fill work — the drawing is too many layers deep, or too "
							  "dense, to rasterise";
					FillWork = 0;
					return false;
				}
				FillWork -= count;
				return true;
			}
		};

		// How many segments an arc of this device radius is flattened to. One
		// per device pixel of circumference is well past what a scanline sampler
		// can tell apart, and the floor keeps a tiny circle from becoming a
		// triangle.
		uint32_t ArcSegments(double deviceRadius) {
			const double wanted = std::abs(deviceRadius);
			if (!std::isfinite(wanted)) {
				return MAXIMUM_ARC_SEGMENTS;
			}
			return static_cast<uint32_t>(std::clamp(wanted, 12.0, static_cast<double>(MAXIMUM_ARC_SEGMENTS)));
		}

		bool AppendEllipse(
			std::vector<SubPath> &out,
			Point centre,
			double radiusX,
			double radiusY,
			double deviceScale,
			Budget &budget,
			std::string &failure
		) {
			const uint32_t segments = ArcSegments(std::max(radiusX, radiusY) * deviceScale);
			if (!budget.Take(segments, failure)) {
				return false;
			}

			SubPath subPath;
			subPath.Closed = true;
			subPath.Points.reserve(segments);

			// **Clockwise in image space, which is the direction the stroke
			// outline's quads wind in.** Two overlapping pieces of one outline
			// have to reinforce under the nonzero rule rather than cancel, and
			// they cancel the moment one of them winds the other way.
			for (uint32_t index = 0; index < segments; index++) {
				const double angle =
					-2.0 * std::numbers::pi_v<double> * static_cast<double>(index) / segments;
				subPath.Points.push_back(
					{centre.X + std::cos(angle) * radiusX, centre.Y + std::sin(angle) * radiusY}
				);
			}

			out.push_back(std::move(subPath));
			return true;
		}

		// A cubic flattened into the subpath it continues, `first` excluded
		// because the subpath already ends there.
		bool AppendCubic(
			SubPath &subPath,
			Point first,
			Point control1,
			Point control2,
			Point last,
			double deviceScale,
			Budget &budget,
			std::string &failure
		) {
			// The control polygon's length is an upper bound on the curve's, so
			// segmenting against it never under-samples.
			const auto distance = [](Point from, Point to) {
				return std::hypot(to.X - from.X, to.Y - from.Y);
			};
			const double length =
				distance(first, control1) + distance(control1, control2) + distance(control2, last);

			const uint32_t segments = static_cast<uint32_t>(
				std::clamp(length * deviceScale * 0.5, 4.0, static_cast<double>(MAXIMUM_ARC_SEGMENTS))
			);
			if (!budget.Take(segments, failure)) {
				return false;
			}

			for (uint32_t index = 1; index <= segments; index++) {
				const double t = static_cast<double>(index) / segments;
				const double inverse = 1.0 - t;
				const double weights[4] = {
					inverse * inverse * inverse,
					3.0 * inverse * inverse * t,
					3.0 * inverse * t * t,
					t * t * t,
				};
				subPath.Points.push_back({
					first.X * weights[0] + control1.X * weights[1] + control2.X * weights[2] +
						last.X * weights[3],
					first.Y * weights[0] + control1.Y * weights[1] + control2.Y * weights[2] +
						last.Y * weights[3],
				});
			}
			return true;
		}

		bool ReadPointList(
			std::string_view text,
			std::string_view element,
			bool closed,
			std::vector<SubPath> &out,
			Budget &budget,
			std::string &failure
		) {
			SubPath subPath;
			subPath.Closed = closed;

			while (true) {
				SkipSeparators(text);
				if (text.empty()) {
					break;
				}

				double x = 0.0;
				double y = 0.0;
				SkipSeparators(text);
				if (!TakeNumber(text, x)) {
					failure = "svg: <" + std::string(element) + "> has a points list this cannot read";
					return false;
				}
				SkipSeparators(text);
				if (!TakeNumber(text, y)) {
					failure = "svg: <" + std::string(element) + "> has an odd number of point coordinates";
					return false;
				}
				if (!budget.Take(1, failure)) {
					return false;
				}
				subPath.Points.push_back({x, y});
			}

			if (subPath.Points.size() >= 2) {
				out.push_back(std::move(subPath));
			}
			return true;
		}

		// `d`, restricted to the commands that are lines and cubics.
		//
		// **A, Q, T and S are refused by name.** An arc is a different
		// parameterisation with three flag-driven branches in it, and a smooth
		// or quadratic curve read as anything else is a curve of the wrong shape
		// through the right endpoints — which is the "recognisably right and
		// wrong everywhere" failure this file exists to avoid.
		bool ReadPath(
			std::string_view text,
			double deviceScale,
			std::vector<SubPath> &out,
			Budget &budget,
			std::string &failure
		) {
			SubPath subPath;
			Point current;
			Point start;
			char command = 0;
			uint32_t commands = 0;

			const auto flush = [&out, &subPath]() {
				if (subPath.Points.size() >= 2) {
					out.push_back(subPath);
				}
				subPath.Points.clear();
				subPath.Closed = false;
			};

			while (true) {
				SkipSeparators(text);
				if (text.empty()) {
					break;
				}

				const char front = text.front();
				const bool isCommand = (Lowered(front) >= 'a' && Lowered(front) <= 'z');
				if (isCommand) {
					command = front;
					text.remove_prefix(1);
					if (++commands > MAXIMUM_PATH_COMMANDS) {
						failure = "svg: a path holds more than " + std::to_string(MAXIMUM_PATH_COMMANDS) +
								  " commands";
						return false;
					}
				} else if (command == 0) {
					failure = "svg: a path starts with a coordinate rather than a command";
					return false;
				} else if (command == 'M') {
					// A repeated moveto is a lineto, which is the specification's
					// rule and not a convenience: a polygon written as one `M`
					// followed by pairs is a common exporter output.
					command = 'L';
				} else if (command == 'm') {
					command = 'l';
				}

				const char upper = static_cast<char>(command - (command >= 'a' ? 'a' - 'A' : 0));
				const bool relative = command >= 'a';

				const auto takeCoordinate = [&text](double &first, double &second) {
					SkipSeparators(text);
					if (!TakeNumber(text, first)) {
						return false;
					}
					SkipSeparators(text);
					return TakeNumber(text, second);
				};

				if (upper == 'Z') {
					if (subPath.Points.size() >= 2) {
						subPath.Closed = true;
						out.push_back(subPath);
					}
					subPath.Points.clear();
					subPath.Closed = false;
					current = start;
					// A subpath after a close starts where the closed one did.
					subPath.Points.push_back(current);
					continue;
				}

				if (upper == 'M' || upper == 'L') {
					double x = 0.0;
					double y = 0.0;
					if (!takeCoordinate(x, y)) {
						failure = "svg: path command '" + std::string(1, command) + "' wants two numbers";
						return false;
					}
					const Point point{
						relative ? current.X + x : x,
						relative ? current.Y + y : y,
					};

					if (upper == 'M') {
						flush();
						start = point;
					}
					if (!budget.Take(1, failure)) {
						return false;
					}
					subPath.Points.push_back(point);
					current = point;
					continue;
				}

				if (upper == 'H' || upper == 'V') {
					double value = 0.0;
					SkipSeparators(text);
					if (!TakeNumber(text, value)) {
						failure = "svg: path command '" + std::string(1, command) + "' wants a number";
						return false;
					}
					Point point = current;
					if (upper == 'H') {
						point.X = relative ? current.X + value : value;
					} else {
						point.Y = relative ? current.Y + value : value;
					}
					if (!budget.Take(1, failure)) {
						return false;
					}
					subPath.Points.push_back(point);
					current = point;
					continue;
				}

				if (upper == 'C') {
					double numbers[6] = {};
					for (double &number : numbers) {
						SkipSeparators(text);
						if (!TakeNumber(text, number)) {
							failure = "svg: path command 'C' wants six numbers";
							return false;
						}
					}

					const double baseX = relative ? current.X : 0.0;
					const double baseY = relative ? current.Y : 0.0;
					const Point control1{baseX + numbers[0], baseY + numbers[1]};
					const Point control2{baseX + numbers[2], baseY + numbers[3]};
					const Point last{baseX + numbers[4], baseY + numbers[5]};

					if (subPath.Points.empty()) {
						if (!budget.Take(1, failure)) {
							return false;
						}
						subPath.Points.push_back(current);
					}
					if (!AppendCubic(
							subPath, current, control1, control2, last, deviceScale, budget, failure
						)) {
						return false;
					}
					current = last;
					continue;
				}

				failure = "svg: path command '" + std::string(1, command) +
						  "' is refused — only M, L, H, V, C and Z are read, and arcs, quadratics and "
						  "smooth curves are not";
				return false;
			}

			flush();
			return true;
		}

		// ---------------------------------------------------------------------
		// Stroking
		// ---------------------------------------------------------------------

		// A stroke as an outline to be filled, built in user space so that the
		// element's own transform stretches the pen exactly as it stretches the
		// shape.
		//
		// **One outline filled once with the nonzero rule, not a quad composited
		// per segment.** Compositing per segment double-blends every join, so a
		// translucent stroke shows a darker dot at each corner; unioning under
		// nonzero blends the whole stroke once. Every piece therefore has to wind
		// the same way — see `AppendEllipse`.
		bool StrokeOutline(
			const std::vector<SubPath> &shape,
			double halfWidth,
			std::vector<SubPath> &out,
			Budget &budget,
			std::string &failure
		) {
			for (const SubPath &subPath : shape) {
				std::vector<Point> points = subPath.Points;
				if (subPath.Closed && points.size() >= 2) {
					points.push_back(points.front());
				}
				if (points.size() < 2) {
					continue;
				}

				for (size_t index = 0; index + 1 < points.size(); index++) {
					const Point from = points[index];
					const Point to = points[index + 1];
					const double deltaX = to.X - from.X;
					const double deltaY = to.Y - from.Y;
					const double length = std::hypot(deltaX, deltaY);
					if (length <= 0.0) {
						continue;
					}

					const double normalX = -deltaY / length * halfWidth;
					const double normalY = deltaX / length * halfWidth;

					if (!budget.Take(4, failure)) {
						return false;
					}

					SubPath quad;
					quad.Closed = true;
					quad.Points = {
						{from.X + normalX, from.Y + normalY},
						{to.X + normalX, to.Y + normalY},
						{to.X - normalX, to.Y - normalY},
						{from.X - normalX, from.Y - normalY},
					};
					out.push_back(std::move(quad));
				}

				// Round joins at every corner, and butt caps — which is SVG's
				// default — at the two ends of an open run. A closed run has no
				// ends, so its start point is a corner like any other.
				const size_t firstJoin = subPath.Closed ? 0u : 1u;
				const size_t lastJoin = points.size() - 1;
				for (size_t index = firstJoin; index < lastJoin; index++) {
					if (!budget.Take(JOIN_SEGMENTS, failure)) {
						return false;
					}

					SubPath join;
					join.Closed = true;
					join.Points.reserve(JOIN_SEGMENTS);
					for (uint32_t step = 0; step < JOIN_SEGMENTS; step++) {
						const double angle =
							-2.0 * std::numbers::pi_v<double> * static_cast<double>(step) / JOIN_SEGMENTS;
						join.Points.push_back({
							points[index].X + std::cos(angle) * halfWidth,
							points[index].Y + std::sin(angle) * halfWidth,
						});
					}
					out.push_back(std::move(join));
				}
			}
			return true;
		}

		// ---------------------------------------------------------------------
		// The rasteriser
		// ---------------------------------------------------------------------

		// Premultiplied float RGBA, unpremultiplied once at the end.
		//
		// **Float rather than bytes, because an icon is layers.** Compositing a
		// dozen translucent shapes through eight-bit premultiplied storage loses
		// a level or two a layer and the error shows as banding in exactly the
		// soft edges antialiasing was for.
		struct Canvas {
			uint32_t Width = 0;
			uint32_t Height = 0;
			std::vector<float> Pixels;
		};

		struct Edge {
			double TopX = 0.0;
			double TopY = 0.0;
			double BottomX = 0.0;
			double BottomY = 0.0;
			int Winding = 1;
		};

		struct Crossing {
			double X = 0.0;
			int Winding = 1;
		};

		// Coverage for one horizontal span, exact in x and clipped to the row.
		void AddSpan(std::vector<float> &coverage, double from, double to, double weight) {
			const double left = std::max(from, 0.0);
			const double right = std::min(to, static_cast<double>(coverage.size()));
			if (right <= left) {
				return;
			}

			const size_t first = static_cast<size_t>(left);
			const size_t last = std::min(static_cast<size_t>(right), coverage.size() - 1);

			if (first == last) {
				coverage[first] += static_cast<float>((right - left) * weight);
				return;
			}

			coverage[first] += static_cast<float>((static_cast<double>(first + 1) - left) * weight);
			for (size_t index = first + 1; index < last; index++) {
				coverage[index] += static_cast<float>(weight);
			}
			coverage[last] += static_cast<float>((right - static_cast<double>(last)) * weight);
		}

		// Fills a set of device-space subpaths, each implicitly closed.
		//
		// @return `false` only when the document has spent its fill budget.
		bool Fill(
			Canvas &canvas,
			const std::vector<SubPath> &subPaths,
			bool evenOdd,
			const Colour &colour,
			double alpha,
			Budget &budget,
			std::string &failure
		) {
			if (alpha <= 0.0 || canvas.Width == 0 || canvas.Height == 0) {
				return true;
			}

			std::vector<Edge> edges;
			double minimumX = static_cast<double>(canvas.Width);
			double maximumX = 0.0;
			double minimumY = static_cast<double>(canvas.Height);
			double maximumY = 0.0;

			for (const SubPath &subPath : subPaths) {
				const size_t count = subPath.Points.size();
				if (count < 3) {
					continue;
				}
				for (size_t index = 0; index < count; index++) {
					const Point from = subPath.Points[index];
					const Point to = subPath.Points[(index + 1) % count];
					if (from.Y == to.Y || !std::isfinite(from.Y) || !std::isfinite(to.Y) ||
						!std::isfinite(from.X) || !std::isfinite(to.X)) {
						// A horizontal edge crosses no scanline, and a
						// non-finite one would poison every comparison below.
						continue;
					}

					Edge edge;
					edge.Winding = from.Y < to.Y ? 1 : -1;
					edge.TopX = from.Y < to.Y ? from.X : to.X;
					edge.TopY = std::min(from.Y, to.Y);
					edge.BottomX = from.Y < to.Y ? to.X : from.X;
					edge.BottomY = std::max(from.Y, to.Y);

					minimumX = std::min({minimumX, from.X, to.X});
					maximumX = std::max({maximumX, from.X, to.X});
					minimumY = std::min(minimumY, edge.TopY);
					maximumY = std::max(maximumY, edge.BottomY);
					edges.push_back(edge);
				}
			}

			if (edges.empty()) {
				return true;
			}

			const int firstRow = std::max(0, static_cast<int>(std::floor(minimumY)));
			const int lastRow =
				std::min(static_cast<int>(canvas.Height) - 1, static_cast<int>(std::ceil(maximumY)));
			if (lastRow < firstRow) {
				return true;
			}

			// **The shape's own columns, not the canvas's.** Clearing and
			// compositing a full row for a shape twelve pixels wide is what turns
			// a drawing of many small shapes into canvas-area work per shape —
			// and it is what would make the budget below fire on files that are
			// perfectly reasonable.
			const int firstColumn = std::max(0, static_cast<int>(std::floor(minimumX)));
			const int lastColumn =
				std::min(static_cast<int>(canvas.Width) - 1, static_cast<int>(std::ceil(maximumX)));
			if (lastColumn < firstColumn) {
				return true;
			}

			// **The bounding box and the edge-crossings both**, because either
			// alone leaves a shape that is cheap by that measure and expensive by
			// the other: a full-canvas rectangle has four edges, and a
			// half-million-edge path can sit inside a hundred pixels. Crossings
			// are counted per sample rather than per row, which is the rate they
			// are really computed and sorted at.
			//
			// **Charged before the fill and not as it goes**, so a document past
			// the budget is refused without first doing the work that put it
			// there.
			const uint64_t rows = static_cast<uint64_t>(lastRow - firstRow + 1);
			if (!budget.TakeFillWork(
					rows * static_cast<uint64_t>(lastColumn - firstColumn + 1) +
						rows * edges.size() * SUBSAMPLES,
					failure
				)) {
				return false;
			}

			// Sorted by their top edge so a row only looks at the edges that
			// reach it — over a drawing of many small shapes this is the
			// difference between linear and quadratic.
			std::sort(edges.begin(), edges.end(), [](const Edge &left, const Edge &right) {
				return left.TopY < right.TopY;
			});

			std::vector<const Edge *> active;
			std::vector<Crossing> crossings;
			std::vector<float> coverage(canvas.Width, 0.0f);
			size_t next = 0;

			for (int row = firstRow; row <= lastRow; row++) {
				const double rowTop = static_cast<double>(row);
				const double rowBottom = rowTop + 1.0;

				while (next < edges.size() && edges[next].TopY < rowBottom) {
					active.push_back(&edges[next]);
					next++;
				}
				std::erase_if(active, [rowTop](const Edge *edge) { return edge->BottomY <= rowTop; });
				if (active.empty()) {
					continue;
				}

				std::fill(coverage.begin() + firstColumn, coverage.begin() + lastColumn + 1, 0.0f);

				for (uint32_t sample = 0; sample < SUBSAMPLES; sample++) {
					const double y = rowTop + (static_cast<double>(sample) + 0.5) / SUBSAMPLES;

					crossings.clear();
					for (const Edge *edge : active) {
						if (y < edge->TopY || y >= edge->BottomY) {
							continue;
						}
						const double t = (y - edge->TopY) / (edge->BottomY - edge->TopY);
						crossings.push_back({edge->TopX + (edge->BottomX - edge->TopX) * t, edge->Winding});
					}
					if (crossings.size() < 2) {
						continue;
					}

					std::sort(
						crossings.begin(), crossings.end(), [](const Crossing &left, const Crossing &right) {
							return left.X < right.X;
						}
					);

					int winding = 0;
					double spanStart = 0.0;
					for (const Crossing &crossing : crossings) {
						const int before = winding;
						winding += evenOdd ? 1 : crossing.Winding;

						const bool wasInside = evenOdd ? (before % 2) != 0 : before != 0;
						const bool isInside = evenOdd ? (winding % 2) != 0 : winding != 0;

						if (!wasInside && isInside) {
							spanStart = crossing.X;
						} else if (wasInside && !isInside) {
							AddSpan(coverage, spanStart, crossing.X, 1.0 / SUBSAMPLES);
						}
					}
				}

				float *pixels = canvas.Pixels.data() + static_cast<size_t>(row) * canvas.Width * 4;
				for (int column = firstColumn; column <= lastColumn; column++) {
					const double covered = std::min(static_cast<double>(coverage[column]), 1.0);
					if (covered <= 0.0) {
						continue;
					}

					const double source = covered * alpha;
					const double keep = 1.0 - source;
					float *pixel = pixels + static_cast<size_t>(column) * 4;

					pixel[0] = static_cast<float>(colour.Red * source + pixel[0] * keep);
					pixel[1] = static_cast<float>(colour.Green * source + pixel[1] * keep);
					pixel[2] = static_cast<float>(colour.Blue * source + pixel[2] * keep);
					pixel[3] = static_cast<float>(source + pixel[3] * keep);
				}
			}
			return true;
		}

		std::vector<SubPath> Transformed(const std::vector<SubPath> &subPaths, const Transform &space) {
			std::vector<SubPath> out;
			out.reserve(subPaths.size());
			for (const SubPath &subPath : subPaths) {
				SubPath moved;
				moved.Closed = subPath.Closed;
				moved.Points.reserve(subPath.Points.size());
				for (const Point &point : subPath.Points) {
					moved.Points.push_back(space.Apply(point));
				}
				out.push_back(std::move(moved));
			}
			return out;
		}

		bool PaintShape(
			Canvas &canvas,
			const std::vector<SubPath> &shape,
			const Transform &space,
			const Paint &ink,
			Budget &budget,
			std::string &failure
		) {
			if (shape.empty()) {
				return true;
			}

			if (ink.FillPainted && ink.FillOpacity > 0.0 &&
				!Fill(
					canvas,
					Transformed(shape, space),
					ink.FillEvenOdd,
					ink.Fill,
					ink.FillOpacity,
					budget,
					failure
				)) {
				return false;
			}

			if (ink.StrokePainted && ink.StrokeOpacity > 0.0 && ink.StrokeWidth > 0.0) {
				std::vector<SubPath> outline;
				if (!StrokeOutline(shape, ink.StrokeWidth * 0.5, outline, budget, failure)) {
					return false;
				}
				// Nonzero always: the outline's pieces overlap by construction
				// and even-odd would punch a hole through every join.
				if (!Fill(
						canvas,
						Transformed(outline, space),
						false,
						ink.Stroke,
						ink.StrokeOpacity,
						budget,
						failure
					)) {
					return false;
				}
			}
			return true;
		}

		// ---------------------------------------------------------------------
		// The document walk
		// ---------------------------------------------------------------------

		// One entry per element that is still open.
		struct Frame {
			Transform Space;
			Paint Ink;
		};

		bool IsIgnoredElement(std::string_view name) {
			// **Skipped whole rather than refused**, because none of them carries
			// a mark and every hand-authored file has a `<title>`.
			return name == "title" || name == "desc" || name == "metadata";
		}

		// Walks past an element's whole subtree, tags included.
		bool SkipSubtree(std::string_view &text, std::string_view name, std::string &failure) {
			int depth = 1;
			uint32_t steps = 0;

			while (depth > 0) {
				Tag tag;
				const Scan scan = NextTag(text, tag, failure);
				if (scan == Scan::Error) {
					return false;
				}
				if (scan == Scan::End) {
					failure = "svg: <" + std::string(name) + "> is never closed";
					return false;
				}
				if (++steps > MAXIMUM_ELEMENTS) {
					failure = "svg: more than " + std::to_string(MAXIMUM_ELEMENTS) + " elements";
					return false;
				}
				if (tag.SelfClosing) {
					continue;
				}
				depth += tag.Closing ? -1 : 1;
			}
			return true;
		}

		// A canvas out of premultiplied floats, into non-premultiplied RGBA8.
		void Resolve(const Canvas &canvas, assets::TextureData &out) {
			out = {};
			out.Width = canvas.Width;
			out.Height = canvas.Height;
			out.Format = assets::TextureFormat::RGBA8;
			out.Pixels.resize(static_cast<size_t>(canvas.Width) * canvas.Height * 4);

			for (size_t pixel = 0; pixel < static_cast<size_t>(canvas.Width) * canvas.Height; pixel++) {
				const float *source = canvas.Pixels.data() + pixel * 4;
				const double alpha = std::clamp(static_cast<double>(source[3]), 0.0, 1.0);

				std::byte *destination = out.Pixels.data() + pixel * 4;
				if (alpha <= 0.0) {
					destination[0] = destination[1] = destination[2] = destination[3] = std::byte{0};
					continue;
				}

				for (int channel = 0; channel < 3; channel++) {
					const double value = std::clamp(static_cast<double>(source[channel]) / alpha, 0.0, 1.0);
					destination[channel] = static_cast<std::byte>(std::lround(value * 255.0));
				}
				destination[3] = static_cast<std::byte>(std::lround(alpha * 255.0));
			}
		}
	}

	bool ReadSvg(
		std::span<const std::byte> bytes,
		uint32_t width,
		uint32_t height,
		assets::TextureData &out,
		std::string &failure
	) {
		if (bytes.empty()) {
			failure = "svg: the document is empty";
			return false;
		}
		if (bytes.size() > MAXIMUM_DOCUMENT_BYTES) {
			failure = "svg: past " + std::to_string(MAXIMUM_DOCUMENT_BYTES) + " bytes of markup";
			return false;
		}

		const std::string_view whole(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		std::string_view text = whole;

		Tag root;
		if (NextTag(text, root, failure) != Scan::Tag) {
			if (failure.empty()) {
				failure = "svg: no elements at all";
			}
			return false;
		}
		if (root.Name != "svg" || root.Closing) {
			failure = "svg: the document's first element is <" + std::string(root.Name) + "> and not <svg>";
			return false;
		}

		// **After the root tag, and that ordering is the whole point.** A
		// billion-laughs document is both a declaration and a swarm of
		// references to it; scanning for the references first would refuse it
		// while naming `&lol;` — which reads like a typo — instead of naming the
		// `<!ENTITY>` that is the actual thing to remove. XML puts any
		// declaration before the root, so by here the scanner has already met it.
		//
		// **A sweep rather than an unescape, because this format never
		// unescapes**: an attribute value is used exactly as written, so there is
		// no point at which a reference would otherwise have been met. `.rbxmx`
		// takes the other route for the reason `xml::ReadContent` gives, and the
		// two must not be collapsed into one policy — `core/Xml.hpp` says what
		// breaks either way round.
		if (!CheckEntityReferences(whole, failure)) {
			return false;
		}

		std::vector<Attribute> attributes;
		if (!ReadAttributes(root.Attributes, attributes, failure)) {
			return false;
		}
		if (!RefuseUnsupportedAttributes(attributes, "svg", failure)) {
			return false;
		}

		// The user-space box the canvas shows. A `viewBox` states it; without
		// one the document's own width and height are it, which is what makes
		// rasterising to a different size a scale rather than a crop.
		double boxX = 0.0;
		double boxY = 0.0;
		double boxWidth = 0.0;
		double boxHeight = 0.0;

		double declaredWidth = 0.0;
		double declaredHeight = 0.0;
		const Attribute *widthAttribute = Find(attributes, "width");
		const Attribute *heightAttribute = Find(attributes, "height");
		const bool declared = widthAttribute != nullptr && heightAttribute != nullptr;

		if (declared) {
			if (!ParseLength(widthAttribute->Value, "width", declaredWidth, failure) ||
				!ParseLength(heightAttribute->Value, "height", declaredHeight, failure)) {
				return false;
			}
		}

		const Attribute *viewBox = Find(attributes, "viewBox");
		if (viewBox != nullptr) {
			std::string_view numbers = viewBox->Value;
			double values[4] = {};
			for (double &value : values) {
				SkipSeparators(numbers);
				if (!TakeNumber(numbers, value)) {
					failure =
						"svg: viewBox '" + std::string(Trimmed(viewBox->Value)) + "' is not four numbers";
					return false;
				}
			}
			SkipSeparators(numbers);
			if (!numbers.empty()) {
				failure = "svg: viewBox '" + std::string(Trimmed(viewBox->Value)) + "' is not four numbers";
				return false;
			}
			boxX = values[0];
			boxY = values[1];
			boxWidth = values[2];
			boxHeight = values[3];
		} else if (declared) {
			boxWidth = declaredWidth;
			boxHeight = declaredHeight;
		} else {
			failure = "svg: the document states neither a width and height nor a viewBox, so there is "
					  "nothing to say what its coordinates mean";
			return false;
		}

		if (!(boxWidth > 0.0) || !(boxHeight > 0.0)) {
			failure = "svg: the drawing's own extent is zero or negative";
			return false;
		}

		// **The raster target is the pipeline's, and only its fallback is the
		// file's.** An SVG has no pixels — that is the whole of what makes it
		// different from every other format here — so a caller that asked for a
		// size gets exactly it, and a caller that asked for nothing gets what the
		// document declared.
		uint32_t canvasWidth = width;
		uint32_t canvasHeight = height;
		if (width == 0 && height == 0) {
			const double intrinsicWidth = declared ? declaredWidth : boxWidth;
			const double intrinsicHeight = declared ? declaredHeight : boxHeight;
			if (!(intrinsicWidth >= 1.0) || !(intrinsicHeight >= 1.0)) {
				failure = "svg: the document's own size is under a pixel, so a raster size has to be given";
				return false;
			}
			canvasWidth = static_cast<uint32_t>(std::lround(intrinsicWidth));
			canvasHeight = static_cast<uint32_t>(std::lround(intrinsicHeight));
		}

		if (canvasWidth == 0 || canvasHeight == 0) {
			failure = "svg: a raster target of zero";
			return false;
		}
		if (canvasWidth > assets::Texture::MAXIMUM_DIMENSION ||
			canvasHeight > assets::Texture::MAXIMUM_DIMENSION) {
			failure = "svg: a raster target past " + std::to_string(assets::Texture::MAXIMUM_DIMENSION) +
					  " pixels on an axis";
			return false;
		}
		if (static_cast<uint64_t>(canvasWidth) * canvasHeight > MAXIMUM_RASTER_PIXELS) {
			failure = "svg: a raster target past " + std::to_string(MAXIMUM_RASTER_PIXELS) + " pixels";
			return false;
		}

		// **Fitted uniformly and centred, which is `preserveAspectRatio`'s
		// default.** Stretching to fill a mismatched target instead would be the
		// one thing every other renderer does not do, and the difference only
		// shows up on the drawings whose aspect somebody got wrong.
		Transform space;
		const double fit = std::min(canvasWidth / boxWidth, canvasHeight / boxHeight);
		space.ScaleX = fit;
		space.ScaleY = fit;
		space.OffsetX = (canvasWidth - boxWidth * fit) * 0.5 - boxX * fit;
		space.OffsetY = (canvasHeight - boxHeight * fit) * 0.5 - boxY * fit;

		Canvas canvas;
		canvas.Width = canvasWidth;
		canvas.Height = canvasHeight;
		canvas.Pixels.assign(static_cast<size_t>(canvasWidth) * canvasHeight * 4, 0.0f);

		Paint ink;
		if (!ReadPaint(attributes, ink, failure)) {
			return false;
		}

		std::vector<Frame> stack;
		stack.push_back({space, ink});

		Budget budget;
		uint32_t elements = 1;

		while (!root.SelfClosing) {
			Tag tag;
			const Scan scan = NextTag(text, tag, failure);
			if (scan == Scan::Error) {
				return false;
			}
			if (scan == Scan::End) {
				// A document that stops without closing its root still drew
				// everything it named, and refusing it would refuse a file that
				// is merely impolite — the rule `ReadImage`'s truncation case
				// already follows.
				break;
			}

			if (tag.Closing) {
				if (stack.size() <= 1) {
					break;
				}
				stack.pop_back();
				continue;
			}

			if (++elements > MAXIMUM_ELEMENTS) {
				failure = "svg: more than " + std::to_string(MAXIMUM_ELEMENTS) + " elements";
				return false;
			}

			if (IsIgnoredElement(tag.Name)) {
				if (!tag.SelfClosing && !SkipSubtree(text, tag.Name, failure)) {
					return false;
				}
				continue;
			}

			const bool shape = tag.Name == "rect" || tag.Name == "circle" || tag.Name == "ellipse" ||
							   tag.Name == "line" || tag.Name == "polyline" || tag.Name == "polygon" ||
							   tag.Name == "path";
			if (!shape && tag.Name != "g") {
				failure = "svg: <" + std::string(tag.Name) +
						  "> is not an element this rasterises — the whole list is svg, g, rect, circle, "
						  "ellipse, line, polyline, polygon and path";
				return false;
			}

			if (!ReadAttributes(tag.Attributes, attributes, failure)) {
				return false;
			}
			if (!RefuseUnsupportedAttributes(attributes, tag.Name, failure)) {
				return false;
			}

			Frame frame = stack.back();
			if (const Attribute *transform = Find(attributes, "transform")) {
				Transform local;
				if (!ParseTransform(transform->Value, local, failure)) {
					return false;
				}
				frame.Space = Compose(frame.Space, local);
			}
			if (!ReadPaint(attributes, frame.Ink, failure)) {
				return false;
			}

			if (shape) {
				const double deviceScale = frame.Space.DeviceScale();
				std::vector<SubPath> subPaths;

				// Every coordinate an element states, read the same way: absent
				// is zero, and a length with a unit on it is refused rather than
				// read as its number.
				const auto length = [&attributes, &failure](std::string_view name, double &value) {
					const Attribute *attribute = Find(attributes, name);
					if (attribute == nullptr) {
						return true;
					}
					return ParseLength(attribute->Value, name, value, failure);
				};

				if (tag.Name == "rect") {
					double x = 0.0;
					double y = 0.0;
					double rectangleWidth = 0.0;
					double rectangleHeight = 0.0;
					if (!length("x", x) || !length("y", y) || !length("width", rectangleWidth) ||
						!length("height", rectangleHeight)) {
						return false;
					}
					if (Find(attributes, "rx") != nullptr || Find(attributes, "ry") != nullptr) {
						failure = "svg: <rect> carries 'rx' or 'ry' — rounded corners are elliptical arcs, "
								  "which are refused";
						return false;
					}
					if (rectangleWidth > 0.0 && rectangleHeight > 0.0) {
						if (!budget.Take(4, failure)) {
							return false;
						}
						SubPath box;
						box.Closed = true;
						// Clockwise in image space, matching every other piece —
						// see `AppendEllipse`.
						box.Points = {
							{x, y},
							{x, y + rectangleHeight},
							{x + rectangleWidth, y + rectangleHeight},
							{x + rectangleWidth, y},
						};
						subPaths.push_back(std::move(box));
					}
				} else if (tag.Name == "circle" || tag.Name == "ellipse") {
					double centreX = 0.0;
					double centreY = 0.0;
					double radiusX = 0.0;
					double radiusY = 0.0;
					if (!length("cx", centreX) || !length("cy", centreY)) {
						return false;
					}
					if (tag.Name == "circle") {
						if (!length("r", radiusX)) {
							return false;
						}
						radiusY = radiusX;
					} else if (!length("rx", radiusX) || !length("ry", radiusY)) {
						return false;
					}
					if (radiusX > 0.0 && radiusY > 0.0 &&
						!AppendEllipse(
							subPaths, {centreX, centreY}, radiusX, radiusY, deviceScale, budget, failure
						)) {
						return false;
					}
				} else if (tag.Name == "line") {
					double x1 = 0.0;
					double y1 = 0.0;
					double x2 = 0.0;
					double y2 = 0.0;
					if (!length("x1", x1) || !length("y1", y1) || !length("x2", x2) || !length("y2", y2)) {
						return false;
					}
					if (!budget.Take(2, failure)) {
						return false;
					}
					SubPath run;
					run.Points = {{x1, y1}, {x2, y2}};
					subPaths.push_back(std::move(run));
				} else if (tag.Name == "polyline" || tag.Name == "polygon") {
					const Attribute *points = Find(attributes, "points");
					if (points != nullptr &&
						!ReadPointList(
							points->Value, tag.Name, tag.Name == "polygon", subPaths, budget, failure
						)) {
						return false;
					}
				} else {
					const Attribute *data = Find(attributes, "d");
					if (data != nullptr && !ReadPath(data->Value, deviceScale, subPaths, budget, failure)) {
						return false;
					}
				}

				if (!PaintShape(canvas, subPaths, frame.Space, frame.Ink, budget, failure)) {
					return false;
				}
			}

			if (!tag.SelfClosing) {
				if (stack.size() >= MAXIMUM_DEPTH) {
					failure = "svg: elements nested more than " + std::to_string(MAXIMUM_DEPTH) + " deep";
					return false;
				}
				stack.push_back(frame);
			}
		}

		Resolve(canvas, out);
		return true;
	}
}
