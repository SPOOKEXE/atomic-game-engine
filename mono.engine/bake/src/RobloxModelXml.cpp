#include <engine/bake/RobloxModel.hpp>
#include <engine/core/Chars.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Xml.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Roblox's XML model container, `.rbxmx`, read into the same tree `.rbxm` is.
//
// **The same instance tree in another spelling, so it produces the same types
// and goes through the same mapping.** `RobloxModel.cpp` reads the binary
// container; this reads the XML one; `studio::RojoSync` cannot tell which it was
// given. A second model type would be the copy that drifts, and
// `tests/RobloxModel.cpp` holds the case that keeps the two honest - one model
// written both ways, asserted to come back identical.
//
// **A referent is even less than it is in the binary container.** There the
// numbers carry the shape of the tree; here the tree *is* the nesting of `Item`
// elements and a `referent` attribute exists only so that a `Ref` property can
// point at one. So the attribute is not read at all, and a `Ref` is refused for
// the reason `RobloxModel.hpp` gives: a number naming a row of one file is not a
// name anything outside it can resolve.
//
// **What refuses a property here is not what refuses one there, and the format
// is why.** A `PROP` chunk's bytes are positional, so a value the binary reader
// cannot decode ends that file; an XML element carries its own end tag, so a
// value this cannot decode is skipped and the document carries on. Both are the
// same rule read against the format in front of them - never guess, and never
// lose more than the thing that was not understood.

namespace engine::bake {

	// The scanner is `core`'s since v0.15 - `D00128`, and `core/Xml.hpp` carries
	// why it is at the bottom rather than beside a format that reads markup.
	namespace xml = core::xml;

	namespace {
		// What a failure here is called, what one element may carry, and that a
		// prefix means nothing to a model - Studio writes `xmime` on the root and
		// nothing below it. Studio writes three attributes at most on anything
		// below `<roblox>`; the root itself carries the schema namespaces.
		constexpr xml::Options XML{"rbxmx", 16, true};

		// The scanner with this format's settings already bound, so that a call
		// site says what it is asking for rather than repeating the settings.
		//
		// **The refusal's kind is dropped and its sentence kept.** A model that is
		// refused is refused; nothing here does anything different for a DOCTYPE
		// than for a tag with no name, which is what `game` needs the kind for.
		//@{
		xml::Scan NextTag(std::string_view &text, xml::Tag &tag, std::string &failure) {
			xml::Failure refusal;
			const xml::Scan scan = xml::NextTag(text, XML, tag, refusal);
			if (scan == xml::Scan::Error) {
				failure = std::move(refusal.Message);
			}
			return scan;
		}

		bool ReadAttributes(std::string_view text, std::vector<xml::Attribute> &out, std::string &failure) {
			xml::Failure refusal;
			if (xml::ReadAttributes(text, XML, out, refusal)) {
				return true;
			}
			failure = std::move(refusal.Message);
			return false;
		}

		bool ReadContent(std::string_view &text, std::string &out, std::string &failure) {
			xml::Failure refusal;
			if (xml::ReadContent(text, XML, out, refusal)) {
				return true;
			}
			failure = std::move(refusal.Message);
			return false;
		}

		bool Unescape(std::string_view text, std::string &out, std::string &failure) {
			xml::Failure refusal;
			if (xml::Unescape(text, XML, out, refusal)) {
				return true;
			}
			failure = std::move(refusal.Message);
			return false;
		}
		//@}

		// How deep the *elements* may nest.
		//
		// **The model's depth bound applied to the markup**, which bounds it a
		// little tighter than the binary reader does - an `Item` sixty levels down
		// is sixty-two elements down, because `<roblox>` and `<Properties>` are
		// levels too. The difference is between two depths nothing real reaches,
		// and one number for both readers is worth more than the last two levels.
		constexpr uint32_t MAXIMUM_ELEMENT_DEPTH = MAXIMUM_ROBLOX_DEPTH;

		// How deep one property value may nest, and how many parts it may have.
		//
		// `Rect2D` is the deepest at two - `min` then `X` - and
		// `CoordinateFrame` is the widest at twelve. Anything past these is not a
		// value this format has.
		//@{
		constexpr uint32_t MAXIMUM_VALUE_DEPTH = 4;
		constexpr uint32_t MAXIMUM_VALUE_FIELDS = 32;
		//@}

		// One part of a property's value, keyed by where it sat inside it.
		//
		// **A flat list of paths rather than a little tree**, because every value
		// this reads knows exactly which parts it wants: a `Vector3` asks for
		// `X`, a `Rect2D` asks for `min/X`. A tree would be a structure to walk
		// where a lookup does.
		struct Field {
			std::string Path;
			std::string Text;
		};

		const std::string *TextOf(const std::vector<Field> &fields, std::string_view path) {
			for (const Field &field : fields) {
				if (field.Path == path) {
					return &field.Text;
				}
			}
			return nullptr;
		}

		// Trims XML's insignificant whitespace off a number or a keyword.
		std::string_view Trimmed(std::string_view text) {
			const auto space = [](char character) {
				return character == ' ' || character == '\t' || character == '\n' || character == '\r';
			};
			while (!text.empty() && space(text.front())) {
				text.remove_prefix(1);
			}
			while (!text.empty() && space(text.back())) {
				text.remove_suffix(1);
			}
			return text;
		}

		// `from_chars` rather than `strtod`, for `Obj.cpp`'s reason: the latter
		// reads the process locale, so a machine set to a comma decimal separator
		// would read every `1.5` in a model as `1`.
		//
		// **An infinity is a value and not a malformed number**, which is the one
		// place this had to be decided rather than assumed: Roblox writes `INF`
		// for a `BodyForce`'s ceiling, and the binary container stores the bit
		// pattern that the binary reader hands straight back. Rejecting it here
		// would make the two containers disagree about the same model.
		bool ParseDouble(std::string_view text, double &out) {
			const std::string_view trimmed = Trimmed(text);
			const auto result = core::FromChars(trimmed.data(), trimmed.data() + trimmed.size(), out);
			return result.ec == std::errc() && result.ptr == trimmed.data() + trimmed.size();
		}

		bool ParseLong(std::string_view text, int64_t &out) {
			const std::string_view trimmed = Trimmed(text);
			const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out);
			return result.ec == std::errc() && result.ptr == trimmed.data() + trimmed.size();
		}

		bool ParseWord(std::string_view text, uint32_t &out) {
			const std::string_view trimmed = Trimmed(text);
			const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), out);
			return result.ec == std::errc() && result.ptr == trimmed.data() + trimmed.size();
		}

		// One component of a value, by the name the format gives it.
		bool XmlComponent(const std::vector<Field> &fields, std::string_view path, float &out) {
			const std::string *text = TextOf(fields, path);
			if (text == nullptr) {
				return false;
			}

			double value = 0.0;
			if (!ParseDouble(*text, value)) {
				return false;
			}
			out = static_cast<float>(value);
			return true;
		}

		// The base64 a `BinaryString` and a shared string are written in.
		//
		// **Whitespace is skipped rather than refused**, because Studio wraps the
		// shared string table at seventy-two columns and every line break inside
		// one is padding it put there.
		bool DecodeBase64(std::string_view text, std::string &out) {
			static constexpr std::string_view ALPHABET =
				"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

			out.clear();
			out.reserve(text.size() / 4 * 3);

			uint32_t accumulated = 0;
			uint32_t bits = 0;
			size_t padding = 0;

			for (const char character : text) {
				if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
					continue;
				}
				if (character == '=') {
					padding++;
					continue;
				}

				const size_t digit = ALPHABET.find(character);
				if (digit == std::string_view::npos || padding > 0) {
					return false;
				}

				accumulated = (accumulated << 6) | static_cast<uint32_t>(digit);
				bits += 6;
				if (bits >= 8) {
					bits -= 8;
					out.push_back(static_cast<char>((accumulated >> bits) & 0xFFu));
				}
			}

			// The leftover bits of a group must be zero, and there is never a
			// whole byte of them - a group of one base64 digit encodes nothing.
			return padding <= 2 && (accumulated & ((1u << bits) - 1u)) == 0;
		}

		// A type this reader knows about and will not decode, named for the note.
		//
		// **The two that matter are worded as `RobloxModel.cpp` words them**, so
		// that the same refusal reads the same whichever container an author
		// exported. Everything else falls through to its own element name, which
		// is the honest answer for a type this file has never heard of.
		const char *NameOfRefusedElement(std::string_view element) {
			if (element == "token") {
				return "an Enum, which is a number naming a member of Roblox's table";
			}
			if (element == "Ref") {
				return "a reference, which is a number naming a row of this file";
			}
			if (element == "NumberSequence") {
				return "a NumberSequence";
			}
			if (element == "ColorSequence") {
				return "a ColorSequence";
			}
			if (element == "PhysicalProperties") {
				return "a PhysicalProperties";
			}
			if (element == "OptionalCoordinateFrame") {
				return "an optional CFrame";
			}
			if (element == "SecurityCapabilities") {
				return "a set of security capabilities";
			}
			if (element == "UniqueId") {
				return "a UniqueId";
			}
			if (element == "Font") {
				return "a Font";
			}
			if (element == "BrickColor") {
				return "a BrickColor";
			}
			if (element == "Ray") {
				return "a Ray";
			}
			if (element == "Faces") {
				return "a Faces";
			}
			if (element == "Axes") {
				return "an Axes";
			}
			return nullptr;
		}

		// What decoding one property element produced.
		enum class ValueResult : uint8_t {
			// The value is in `out`.
			Decoded,

			// A type this reader does not turn into a value.
			Unsupported,

			// A type it does, holding something that is not one.
			Malformed,
		};

		// One property element's value, from the parts its subtree held.
		//
		// **Keyed on the element name, which is what the format calls the type.**
		// The names differ from the binary container's numbers in three places
		// worth knowing: a `CFrame` is a `CoordinateFrame`, a `Rect` is a
		// `Rect2D`, and `Content` and `BinaryString` are separate elements here
		// for what the binary container stores as one `String` - which is why
		// they are read rather than refused, since refusing them would make a
		// `Decal` imported from XML lose the texture the same model keeps when it
		// is imported from `.rbxm`.
		ValueResult DecodeValue(
			std::string_view element,
			const std::vector<Field> &fields,
			const std::unordered_map<std::string, std::string> &shared,
			RobloxValue &out
		) {
			out = RobloxValue{};

			const std::string *own = TextOf(fields, "");
			const std::string_view text = own != nullptr ? std::string_view(*own) : std::string_view();

			if (element == "string" || element == "ProtectedString") {
				out.Kind = RobloxValueKind::Text;
				out.Text = text;
				return ValueResult::Decoded;
			}

			if (element == "Content") {
				// `<url>` when it names something and `<null/>` when it does not.
				// Both are the plain string the binary container writes.
				const std::string *url = TextOf(fields, "url");
				out.Kind = RobloxValueKind::Text;
				out.Text = url != nullptr ? *url : std::string();
				return ValueResult::Decoded;
			}

			if (element == "BinaryString") {
				out.Kind = RobloxValueKind::Text;
				return DecodeBase64(text, out.Text) ? ValueResult::Decoded : ValueResult::Malformed;
			}

			if (element == "SharedString") {
				// The table's key, resolved here so that nothing outside this
				// parse ever sees it - the same rule the binary reader's `SSTR`
				// index follows.
				const auto found = shared.find(std::string(Trimmed(text)));
				out.Kind = RobloxValueKind::Text;
				if (found != shared.end()) {
					out.Text = found->second;
				}
				return ValueResult::Decoded;
			}

			if (element == "bool") {
				const std::string_view value = Trimmed(text);
				if (value != "true" && value != "false") {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Bool;
				out.Bool = value == "true";
				return ValueResult::Decoded;
			}

			if (element == "int" || element == "int64") {
				int64_t value = 0;
				if (!ParseLong(text, value)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Integer;
				out.Integer = value;
				return ValueResult::Decoded;
			}

			if (element == "float" || element == "double") {
				double value = 0.0;
				if (!ParseDouble(text, value)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Number;
				out.Number = value;
				return ValueResult::Decoded;
			}

			if (element == "UDim") {
				float scale = 0.0f;
				float offset = 0.0f;
				if (!XmlComponent(fields, "S", scale) || !XmlComponent(fields, "O", offset)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::UDim;
				out.UDim = core::UDim{scale, offset};
				return ValueResult::Decoded;
			}

			if (element == "UDim2") {
				float xScale = 0.0f;
				float xOffset = 0.0f;
				float yScale = 0.0f;
				float yOffset = 0.0f;
				if (!XmlComponent(fields, "XS", xScale) || !XmlComponent(fields, "XO", xOffset) ||
					!XmlComponent(fields, "YS", yScale) || !XmlComponent(fields, "YO", yOffset)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::UDim2;
				out.UDim2 = core::UDim2{core::UDim{xScale, xOffset}, core::UDim{yScale, yOffset}};
				return ValueResult::Decoded;
			}

			if (element == "Vector2") {
				float x = 0.0f;
				float y = 0.0f;
				if (!XmlComponent(fields, "X", x) || !XmlComponent(fields, "Y", y)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Vector2;
				out.Vector2 = core::Vector2{x, y};
				return ValueResult::Decoded;
			}

			if (element == "Vector3") {
				float x = 0.0f;
				float y = 0.0f;
				float z = 0.0f;
				if (!XmlComponent(fields, "X", x) || !XmlComponent(fields, "Y", y) ||
					!XmlComponent(fields, "Z", z)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Vector3;
				out.Vector3 = core::Vector3{x, y, z};
				return ValueResult::Decoded;
			}

			if (element == "Color3") {
				float red = 0.0f;
				float green = 0.0f;
				float blue = 0.0f;
				if (!XmlComponent(fields, "R", red) || !XmlComponent(fields, "G", green) ||
					!XmlComponent(fields, "B", blue)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Color3;
				out.Color3 = core::Color3{red, green, blue};
				return ValueResult::Decoded;
			}

			if (element == "Color3uint8") {
				// **One packed number rather than three byte planes**, which is
				// this format's spelling of the binary container's `Color3uint8`
				// and the same colour: alpha in the top byte, which is always
				// opaque and is discarded.
				uint32_t packed = 0;
				if (!ParseWord(text, packed)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Color3;
				out.Color3 = core::Color3{
					static_cast<float>((packed >> 16) & 0xFFu) / 255.0f,
					static_cast<float>((packed >> 8) & 0xFFu) / 255.0f,
					static_cast<float>(packed & 0xFFu) / 255.0f,
				};
				return ValueResult::Decoded;
			}

			if (element == "Rect2D") {
				float minimumX = 0.0f;
				float minimumY = 0.0f;
				float maximumX = 0.0f;
				float maximumY = 0.0f;
				if (!XmlComponent(fields, "min/X", minimumX) || !XmlComponent(fields, "min/Y", minimumY) ||
					!XmlComponent(fields, "max/X", maximumX) || !XmlComponent(fields, "max/Y", maximumY)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::Rect;
				out.Rect = core::Rect{
					core::Vector2{minimumX, minimumY},
					core::Vector2{maximumX, maximumY},
				};
				return ValueResult::Decoded;
			}

			if (element == "NumberRange") {
				// Two numbers in the element's own text, with a trailing space
				// Studio always writes.
				std::string_view rest = Trimmed(text);
				const size_t split = rest.find(' ');
				double minimum = 0.0;
				double maximum = 0.0;
				if (split == std::string_view::npos || !ParseDouble(rest.substr(0, split), minimum) ||
					!ParseDouble(rest.substr(split + 1), maximum)) {
					return ValueResult::Malformed;
				}
				out.Kind = RobloxValueKind::NumberRange;
				out.NumberRange = core::NumberRange{static_cast<float>(minimum), static_cast<float>(maximum)};
				return ValueResult::Decoded;
			}

			if (element == "CoordinateFrame") {
				float position[3] = {};
				if (!XmlComponent(fields, "X", position[0]) || !XmlComponent(fields, "Y", position[1]) ||
					!XmlComponent(fields, "Z", position[2])) {
					return ValueResult::Malformed;
				}

				// **The whole rotation matrix, always.** The binary container
				// spends one byte on the twenty-four axis-aligned rotations and
				// nine floats on everything else; the XML one writes the nine
				// every time, so there is no special case here and both arrive as
				// the same quaternion.
				float rotation[9] = {};
				for (int row = 0; row < 3; row++) {
					for (int column = 0; column < 3; column++) {
						const std::string name = "R" + std::to_string(row) + std::to_string(column);
						if (!XmlComponent(fields, name, rotation[row * 3 + column])) {
							return ValueResult::Malformed;
						}
					}
				}

				// Roblox's space is right-handed and Y-up with -Z forward, which
				// is this engine's, so the basis vectors transfer with no
				// conversion. The matrix is written in rows and glm holds
				// columns.
				glm::mat3 basis;
				basis[0] = glm::vec3(rotation[0], rotation[3], rotation[6]);
				basis[1] = glm::vec3(rotation[1], rotation[4], rotation[7]);
				basis[2] = glm::vec3(rotation[2], rotation[5], rotation[8]);

				out.Kind = RobloxValueKind::CFrame;
				out.CFrame =
					core::CFrame(core::Vector3{position[0], position[1], position[2]}, glm::quat_cast(basis));
				return ValueResult::Decoded;
			}

			return ValueResult::Unsupported;
		}

		// Reads one property element's subtree into its parts.
		//
		// Called with `text` just past the element's opening tag and leaves it
		// just past its closing one, so a value this does not understand costs
		// exactly its own element.
		bool ReadValueFields(
			std::string_view &text, std::string_view element, std::vector<Field> &out, std::string &failure
		) {
			out.clear();

			std::vector<std::string_view> path;
			std::string joined;

			const auto rejoin = [&]() {
				joined.clear();
				for (const std::string_view part : path) {
					if (!joined.empty()) {
						joined.push_back('/');
					}
					joined.append(part);
				}
			};

			const auto fieldFor = [&](const std::string &name) -> Field * {
				for (Field &field : out) {
					if (field.Path == name) {
						return &field;
					}
				}
				if (out.size() >= MAXIMUM_VALUE_FIELDS) {
					return nullptr;
				}
				out.push_back(Field{name, {}});
				return &out.back();
			};

			while (true) {
				std::string content;
				if (!ReadContent(text, content, failure)) {
					return false;
				}
				if (!content.empty()) {
					Field *field = fieldFor(joined);
					if (field == nullptr) {
						failure = "rbxmx: a <" + std::string(element) + "> holds more than " +
								  std::to_string(MAXIMUM_VALUE_FIELDS) + " parts";
						return false;
					}
					field->Text.append(content);
				}

				xml::Tag inner;
				const xml::Scan scan = NextTag(text, inner, failure);
				if (scan == xml::Scan::Error) {
					return false;
				}
				if (scan == xml::Scan::End) {
					failure = "rbxmx: a <" + std::string(element) + "> is never closed";
					return false;
				}

				if (inner.Closing) {
					const std::string_view expected = path.empty() ? element : path.back();
					if (inner.Name != expected) {
						failure = "rbxmx: </" + std::string(inner.Name) + "> closes <" +
								  std::string(expected) + ">";
						return false;
					}
					if (path.empty()) {
						return true;
					}
					path.pop_back();
					rejoin();
					continue;
				}

				if (inner.SelfClosing) {
					continue;
				}

				if (path.size() >= MAXIMUM_VALUE_DEPTH) {
					failure = "rbxmx: a <" + std::string(element) + "> nests deeper than a value does";
					return false;
				}
				path.push_back(inner.Name);
				rejoin();
			}
		}

		// The `<SharedStrings>` table, which Studio writes *after* every `Item`
		// that refers to it.
		//
		// **So it is read in a pass of its own**, which is the whole reason there
		// are two: a value cannot be resolved as it is met, and the alternative -
		// leaving keys in the tree and walking it again afterwards - would let a
		// key escape the reader if anything ever returned early.
		bool ReadSharedStrings(
			std::string_view whole, std::unordered_map<std::string, std::string> &out, std::string &failure
		) {
			// **A substring test before a scan**, because most files carry no
			// table at all and would otherwise pay a whole extra pass to find
			// that out. A false positive costs the scan and nothing else - the
			// text can only be inside a CDATA section, which the scanner steps
			// over.
			if (whole.find("<SharedStrings") == std::string_view::npos) {
				return true;
			}

			std::string_view text = whole;
			std::vector<xml::Attribute> attributes;

			while (true) {
				xml::Tag tag;
				const xml::Scan scan = NextTag(text, tag, failure);
				if (scan == xml::Scan::Error) {
					return false;
				}
				if (scan == xml::Scan::End) {
					return true;
				}
				if (tag.Name != "SharedStrings" || tag.Closing || tag.SelfClosing) {
					continue;
				}

				while (true) {
					xml::Tag entry;
					const xml::Scan found = NextTag(text, entry, failure);
					if (found == xml::Scan::Error) {
						return false;
					}
					if (found == xml::Scan::End) {
						failure = "rbxmx: the shared string table is never closed";
						return false;
					}
					if (entry.Closing) {
						// The table's own end. Every other closing tag here is
						// one entry's, and `ReadContent` has already left the
						// scanner on it.
						if (entry.Name == "SharedStrings") {
							return true;
						}
						continue;
					}
					if (entry.Name != "SharedString" || entry.SelfClosing) {
						continue;
					}

					if (!ReadAttributes(entry.Attributes, attributes, failure)) {
						return false;
					}

					const xml::Attribute *key = xml::Find(attributes, "md5");
					std::string encoded;
					if (!ReadContent(text, encoded, failure)) {
						return false;
					}

					std::string decoded;
					if (key != nullptr && DecodeBase64(encoded, decoded)) {
						out.emplace(std::string(key->Value), std::move(decoded));
					}
				}
			}
		}

		// One open element, and what the walk below has to do when it closes.
		struct Open {
			std::string_view Name;

			// Whether it is an `<Item>`, so that closing it moves an instance
			// into whatever it was inside.
			bool Item = false;

			// Whether it is the `<Properties>` of an `<Item>`, so that an element
			// opening inside it is a property rather than a container.
			bool Properties = false;
		};
	}

	bool ReadRobloxModelXml(std::span<const std::byte> bytes, RobloxModel &out, std::string &failure) {
		std::string_view whole(reinterpret_cast<const char *>(bytes.data()), bytes.size());

		// A byte order mark an editor added rather than Studio. Stepped over so a
		// re-saved file is not refused for a reason nobody can see in it.
		if (whole.starts_with("\xEF\xBB\xBF")) {
			whole.remove_prefix(3);
		}

		// **Named, because a renamed file is how somebody actually arrives
		// here.** The binary reader says the same thing from the other side, and
		// between them an author who swapped an extension is told which they have
		// rather than that their model is corrupt.
		if (whole.starts_with("<roblox!")) {
			failure = "rbxmx: this is the binary .rbxm container and not the XML one";
			return false;
		}

		std::unordered_map<std::string, std::string> shared;
		if (!ReadSharedStrings(whole, shared, failure)) {
			return false;
		}

		std::string_view text = whole;
		std::vector<xml::Attribute> attributes;

		xml::Tag root;
		if (NextTag(text, root, failure) != xml::Scan::Tag) {
			if (failure.empty()) {
				failure = "rbxmx: no elements at all";
			}
			return false;
		}
		if (root.Name != "roblox" || root.Closing) {
			failure =
				"rbxmx: the document's first element is <" + std::string(root.Name) + "> and not <roblox>";
			return false;
		}

		if (!ReadAttributes(root.Attributes, attributes, failure)) {
			return false;
		}

		// The container's own version, which is 4 everywhere Studio has written
		// one. Refused rather than guessed, exactly as the binary reader refuses
		// anything but version 0.
		const xml::Attribute *version = xml::Find(attributes, "version");
		if (version == nullptr || Trimmed(version->Value) != "4") {
			failure = "rbxmx: version '" +
					  std::string(version != nullptr ? Trimmed(version->Value) : std::string_view()) +
					  "', and this reads version 4";
			return false;
		}

		RobloxModel model;
		if (root.SelfClosing) {
			out = std::move(model);
			return true;
		}

		std::vector<Open> stack;
		std::vector<RobloxInstance> items;
		std::vector<Field> fields;
		uint64_t instances = 0;

		stack.push_back(Open{root.Name, false, false});

		while (!stack.empty()) {
			xml::Tag tag;
			const xml::Scan scan = NextTag(text, tag, failure);
			if (scan == xml::Scan::Error) {
				return false;
			}
			if (scan == xml::Scan::End) {
				failure = "rbxmx: <" + std::string(stack.back().Name) + "> is never closed";
				return false;
			}

			if (tag.Closing) {
				if (tag.Name != stack.back().Name) {
					failure = "rbxmx: </" + std::string(tag.Name) + "> closes <" +
							  std::string(stack.back().Name) + ">";
					return false;
				}

				const bool item = stack.back().Item;
				stack.pop_back();

				if (item) {
					// **Moved into whatever is still open**, which is what makes
					// the nesting of the markup the shape of the tree - the one
					// thing a referent is allowed to become, and here it is not
					// even that.
					RobloxInstance node = std::move(items.back());
					items.pop_back();
					if (items.empty()) {
						model.Roots.push_back(std::move(node));
					} else {
						items.back().Children.push_back(std::move(node));
					}
				}
				continue;
			}

			// A property, which is every element inside an `<Item>`'s
			// `<Properties>`. Consumed whole here rather than pushed, because its
			// subtree is a value and not a level of the tree.
			if (!stack.empty() && stack.back().Properties) {
				const std::string_view element = tag.Name;
				const std::string_view run = tag.Attributes;
				const bool empty = tag.SelfClosing;

				if (!ReadAttributes(run, attributes, failure)) {
					return false;
				}

				std::string name;
				const xml::Attribute *named = xml::Find(attributes, "name");
				if (named != nullptr && !Unescape(named->Value, name, failure)) {
					return false;
				}

				fields.clear();
				if (!empty && !ReadValueFields(text, element, fields, failure)) {
					return false;
				}
				if (name.empty()) {
					model.Notes.push_back("a <" + std::string(element) + "> carries no name - skipped");
					continue;
				}

				RobloxValue value;
				const ValueResult result = DecodeValue(element, fields, shared, value);
				if (result == ValueResult::Unsupported) {
					const char *refused = NameOfRefusedElement(element);
					model.Notes.push_back(
						items.back().ClassName + "." + name + " is " +
						(refused != nullptr
							 ? std::string(refused)
							 : "a type this reader does not know (" + std::string(element) + ")") +
						" - skipped"
					);
					continue;
				}
				if (result == ValueResult::Malformed) {
					model.Notes.push_back(
						items.back().ClassName + "." + name + " does not hold a " + std::string(element) +
						" - skipped"
					);
					continue;
				}

				// **`Name` becomes the instance's name and is not also a
				// property**, which is `RobloxModel.cpp`'s rule and has to be the
				// same one: two readers disagreeing about where a name lives
				// would be two trees the mapping treats differently.
				if (name == "Name" && value.Kind == RobloxValueKind::Text) {
					items.back().Name = value.Text;
					continue;
				}
				items.back().Properties.push_back(RobloxProperty{std::move(name), std::move(value)});
				continue;
			}

			if (stack.size() >= MAXIMUM_ELEMENT_DEPTH) {
				failure = "rbxmx: elements nest deeper than this reads";
				return false;
			}

			Open open{tag.Name, tag.Name == "Item", false};

			if (open.Item) {
				if (!ReadAttributes(tag.Attributes, attributes, failure)) {
					return false;
				}

				const xml::Attribute *klass = xml::Find(attributes, "class");
				if (klass == nullptr) {
					failure = "rbxmx: an Item states no class";
					return false;
				}

				// **A count of what has been read rather than one the file
				// states**, which is the difference between the two containers
				// worth remembering: an `.rbxm` chunk declares how many instances
				// follow and can lie, while an `.rbxmx` cannot claim an instance
				// it did not spend bytes on. This bounds the walk anyway, because
				// bytes are cheaper than instances.
				instances++;
				if (instances > MAXIMUM_ROBLOX_INSTANCES) {
					failure = "rbxmx: more instances than this will read";
					return false;
				}

				std::string className;
				if (!Unescape(klass->Value, className, failure)) {
					return false;
				}

				RobloxInstance &instance = items.emplace_back();
				instance.ClassName = className;

				// Roblox's own default for an instance nobody renamed, replaced
				// if the file carries a `Name`.
				instance.Name = className;
			}

			// `<Properties>` only counts as one directly inside an `<Item>`.
			// Anywhere else it is an element with a familiar name.
			open.Properties = tag.Name == "Properties" && !stack.empty() && stack.back().Item;

			if (!tag.SelfClosing) {
				stack.push_back(open);
				continue;
			}

			// A self-closing `<Item/>` is an instance with nothing in it, and
			// closes where it opened.
			if (open.Item) {
				RobloxInstance node = std::move(items.back());
				items.pop_back();
				if (items.empty()) {
					model.Roots.push_back(std::move(node));
				} else {
					items.back().Children.push_back(std::move(node));
				}
			}
		}

		// **The same taxonomy as the binary reader's, deliberately.** Both
		// containers produce one `RobloxModel`, and an import that lost
		// properties should read the same whichever file it came out of.
		for (const std::string &note : model.Notes) {
			ENGINE_DEBUG("rbxmx: {}", note);
		}
		if (!model.Notes.empty()) {
			core::Metrics::Count("bake.rbxmx.notes", static_cast<double>(model.Notes.size()));
			ENGINE_WARN(
				"rbxmx: {} root(s) read with {} thing(s) this reader could not keep",
				model.Roots.size(),
				model.Notes.size()
			);
		} else {
			ENGINE_DEBUG("rbxmx: {} root(s) read whole", model.Roots.size());
		}

		out = std::move(model);
		return true;
	}
}
