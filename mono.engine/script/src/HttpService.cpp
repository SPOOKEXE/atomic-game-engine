// The pure half of `HttpService`: JSON, a GUID and a URL escape.
//
// **`RequestAsync`, `GetAsync` and `PostAsync` are absent, and their absence is
// the decision rather than a gap somebody has not got to yet.**
//
// This engine already has a path out to the network, and every part of it is
// deliberate: `mono.cdn` serves a manifest, the manifest is signed, a client
// verifies that signature against a publisher key before a byte of content is
// trusted, and `client/ContentDemand.hpp` decides what is asked for at all.
// Arbitrary outbound HTTP from a game script is not a smaller version of that —
// it is a different thing, and it is a *security* decision: what a game script
// may talk to, whether a dedicated server will make requests on a player's
// behalf, what a request discloses about the machine it left, and who is
// accountable for a place that exfiltrates. Nobody has taken that decision, so
// the methods do not exist.
//
// **Not stubbed, and not present-but-erroring.** A method that exists and
// refuses is a surface an author writes against and then finds does nothing —
// and worse, it is a surface that *looks* decided, so the next reader assumes
// somebody thought about it. An absent method fails the way every other
// unprovided service member fails: "attempt to call a nil value", at the call
// site, in the script that asked. That is the same refusal
// `game:GetService("BreakpointService")` gives outside a studio, for the same
// reason.
//
// If you are here to add one of the three, the thing to add first is the
// decision — written down — about what a script may reach and who says so.
//
// ## What the four that are here have in common
//
// None of them observes anything. Given the same arguments they give the same
// answer on every machine and in every replay, which is the test
// `script/AGENTS.md` sets for anything added to this VM: *what can it observe
// that a recording cannot reproduce?* It is why `os` is not opened, and it is
// why `GenerateGUID` draws from `core::Random` and a counter rather than from a
// clock or the operating system's entropy. See its own comment.
//
// ## JSON is the codec's tree, spelled differently
//
// `Codec.hpp` already decides what a Lua table *is* — which tables are arrays,
// which are maps, what a cycle is, what a key becomes — and a second traversal
// here answering any of those differently would mean one value crossing a bus
// in one shape and landing in a JSON document in another. So `JSONEncode` reads
// the script value with `ScriptCall::ReadValue`, the same walker
// `MessagingService` publishes with, and this file only turns the resulting
// `ScriptValue` into text; `JSONDecode` is the mirror, and answers through the
// same `ReturnValue`. Every table rule below is inherited rather than invented,
// and the ones this file adds are the ones JSON forces: what a number looks
// like, what an escape is, and what has no JSON form at all.
//
// ## Neutral since v0.16
//
// Nothing in this file names a VM. It was four `lua_CFunction`s, which is why
// JavaScript did not have `HttpService` at all — and the irony is that the whole
// file was already written against a shared tree, with only the four wrappers
// around it per language. `GenerateGUID`'s draw counter was the one piece of
// real state, and it crosses as `ScriptCall::NextGuid`.
//
// @tier L9 · shared

#include "Codec.hpp"
#include "ScriptCall.hpp"
#include "ServiceSurface.hpp"

#include <engine/core/Random.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace engine::script {
	namespace {

		// --- a `ScriptValue` as JSON text ------------------------------------

		// Appends one number as the shortest text that reads back as itself.
		//
		// **`std::to_chars` with no format argument, which is the shortest
		// decimal that round-trips.** A fixed `%.17g` would round-trip too and
		// would spell `0.1` as `0.10000000000000001`, so a value written by a
		// script and read back by the same script would not compare equal to the
		// literal it was written from. Losing the value is the failure to avoid;
		// spelling it unrecognisably is the one that gets reported as a bug.
		//
		// @param value The number.
		// @param out   Appended to.
		// @return Null, or why the number has no JSON form.
		const char *WriteJsonNumber(double value, std::string &out) {
			// **A NaN or an infinity is refused, not coerced.** JSON has no
			// spelling for either. JavaScript's `JSON.stringify` writes `null`,
			// which turns a broken number into a *missing field* somewhere far
			// from the arithmetic that produced it; writing a bare `NaN` instead
			// produces text no conformant parser will read back. Refusing is the
			// only answer that reports the problem where it happened.
			if (!std::isfinite(value)) {
				return std::isnan(value) ? "a NaN has no JSON form" : "an infinity has no JSON form";
			}

			// Seventeen significant digits plus a sign, a point and a
			// three-digit exponent fits inside this with room over.
			std::array<char, 32> buffer{};
			const std::to_chars_result written =
				std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			if (written.ec != std::errc{}) {
				return "the number could not be written";
			}

			out.append(buffer.data(), static_cast<size_t>(written.ptr - buffer.data()));
			return nullptr;
		}

		// Appends one string as a quoted JSON string.
		//
		// **Bytes through, above 0x7F.** A Lua string is bytes and nothing in
		// this module decodes an encoding — `Codec.hpp` says so about the wire
		// format and it is equally true here. So a UTF-8 input produces UTF-8
		// output and a string of arbitrary bytes comes back exactly as it went
		// in; re-spelling the high bytes as `\u` escapes would mean guessing what
		// character set they were, and guessing wrong is silent.
		//
		// @param text The bytes.
		// @param out  Appended to.
		void WriteJsonString(std::string_view text, std::string &out) {
			static constexpr char HEX[] = "0123456789ABCDEF";

			out.push_back('"');
			for (const char character : text) {
				const auto byte = static_cast<unsigned char>(character);
				switch (byte) {
				// The two JSON requires, always.
				case '"':
					out.append("\\\"");
					continue;
				case '\\':
					out.append("\\\\");
					continue;

				// The five shorthands, because they are what a reader expects to
				// see and they are four bytes shorter than the `\u` form.
				case '\b':
					out.append("\\b");
					continue;
				case '\f':
					out.append("\\f");
					continue;
				case '\n':
					out.append("\\n");
					continue;
				case '\r':
					out.append("\\r");
					continue;
				case '\t':
					out.append("\\t");
					continue;
				default:
					break;
				}

				// Every other control byte. **`/` is deliberately not escaped**:
				// JSON does not require it, and escaping it only makes the text
				// longer and less readable.
				if (byte < 0x20) {
					out.append("\\u00");
					out.push_back(HEX[byte >> 4]);
					out.push_back(HEX[byte & 0x0F]);
					continue;
				}

				out.push_back(character);
			}
			out.push_back('"');
		}

		// Appends one value's JSON text, sorting map entries on the way.
		//
		// **Sorted by key bytes, exactly as `Encode` sorts.** A Lua table is a
		// hash map, so writing entries in iteration order would make the *string*
		// a script gets back depend on insertion history and allocator state —
		// and that string may then be stored in a property, replicated, or
		// written into a save. `Codec.hpp` §1 is the whole argument, and the only
		// thing that changes here is that the output is text: the same table must
		// produce the same document on every run and in both VMs. By bytes rather
		// than by either language's own comparison, for the same reason that
		// section gives about `"é"`.
		//
		// Non-const, like `Encode`, because the sort happens in place and the
		// tree handed in comes back in the order it was written.
		//
		// @param value The tree.
		// @param out   Appended to.
		// @return Null, or why the value has no JSON form.
		const char *WriteJson(ScriptValue &value, std::string &out) {
			switch (value.Tag) {
			case ValueTag::Nil:
				out.append("null");
				return nullptr;
			case ValueTag::False:
				out.append("false");
				return nullptr;
			case ValueTag::True:
				out.append("true");
				return nullptr;
			case ValueTag::Number:
				return WriteJsonNumber(value.Number, out);
			case ValueTag::String:
				WriteJsonString(value.Text, out);
				return nullptr;

			case ValueTag::Array:
				out.push_back('[');
				for (size_t item = 0; item < value.Items.size(); item++) {
					if (item > 0) {
						out.push_back(',');
					}
					if (const char *failure = WriteJson(value.Items[item], out); failure != nullptr) {
						return failure;
					}
				}
				out.push_back(']');
				return nullptr;

			case ValueTag::Map:
				std::sort(
					value.Entries.begin(), value.Entries.end(), [](const auto &left, const auto &right) {
						return left.first < right.first;
					}
				);

				out.push_back('{');
				for (size_t entry = 0; entry < value.Entries.size(); entry++) {
					if (entry > 0) {
						out.push_back(',');
					}
					WriteJsonString(value.Entries[entry].first, out);
					out.push_back(':');
					if (const char *failure = WriteJson(value.Entries[entry].second, out);
						failure != nullptr) {
						return failure;
					}
				}
				out.push_back('}');
				return nullptr;

			// **The three value types are refused, and the refusal names one.**
			// They cross a *bus* because both ends of a bus are this engine;
			// JSON's readers are not, and there is no notation for a `CFrame`
			// that any of them would understand. A shape invented here — say
			// `{"x":..,"y":..}` — would decode back as a plain table rather than
			// as the type it went in as, so the round trip this service's whole
			// test rests on would quietly stop holding.
			case ValueTag::Vector3:
				return "a Vector3 has no JSON form";
			case ValueTag::Color3:
				return "a Color3 has no JSON form";
			case ValueTag::CFrame:
				return "a CFrame has no JSON form";
			}
			return "the value has no JSON form";
		}

		// --- JSON text as a `ScriptValue` ------------------------------------

		// Appends one code point as UTF-8.
		void AppendUtf8(uint32_t point, std::string &out) {
			if (point < 0x80) {
				out.push_back(static_cast<char>(point));
			} else if (point < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (point >> 6)));
				out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
			} else if (point < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (point >> 12)));
				out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (point >> 18)));
				out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (point & 0x3F)));
			}
		}

		// One document, read once, left to right.
		//
		// **It never trusts what it reads**, which is `Codec::Decode`'s rule and
		// applies here for a stronger reason: a payload reaching `Decode` was
		// written by this engine, and a document reaching this may have been
		// typed by hand. The invariant the refusals are chosen to keep is that
		// **this reader never produces a value `JSONEncode` would refuse** — so
		// anything it accepts can be written straight back out.
		struct JsonReader {
			std::string_view Text;
			size_t At = 0;

			// Why the read stopped, or null.
			const char *Failure = nullptr;

			bool Done() const {
				return At >= Text.size();
			}

			char Peek() const {
				return Text[At];
			}

			// JSON's four, and nothing else. A vertical tab or a form feed
			// between two values is not whitespace to this grammar.
			void SkipSpace() {
				while (!Done()) {
					const char character = Peek();
					if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
						return;
					}
					At++;
				}
			}

			bool Fail(const char *why) {
				if (Failure == nullptr) {
					Failure = why;
				}
				return false;
			}

			// Reads four hex digits as one UTF-16 code unit.
			bool ReadHex4(uint32_t &out) {
				if (At + 4 > Text.size()) {
					return Fail("a \\u escape ran off the end");
				}

				out = 0;
				for (int digit = 0; digit < 4; digit++) {
					const auto byte = static_cast<unsigned char>(Text[At++]);
					uint32_t nibble = 0;
					if (byte >= '0' && byte <= '9') {
						nibble = static_cast<uint32_t>(byte - '0');
					} else if (byte >= 'a' && byte <= 'f') {
						nibble = static_cast<uint32_t>(byte - 'a') + 10;
					} else if (byte >= 'A' && byte <= 'F') {
						nibble = static_cast<uint32_t>(byte - 'A') + 10;
					} else {
						return Fail("a \\u escape is not four hex digits");
					}
					out = (out << 4) | nibble;
				}
				return true;
			}

			// Reads a `"..."` string, cursor on the opening quote.
			bool ReadString(std::string &out) {
				At++;

				while (true) {
					if (Done()) {
						return Fail("a string is not closed");
					}

					const auto byte = static_cast<unsigned char>(Text[At]);
					if (byte == '"') {
						At++;
						return true;
					}

					// **A raw control byte inside a string is refused.** JSON
					// requires it escaped, and accepting it would let a literal
					// newline through — which `JSONEncode` then writes back as
					// `\n`, so the document would not survive a round trip
					// unchanged even though the value did.
					if (byte < 0x20) {
						return Fail("a string holds an unescaped control character");
					}

					// Everything from 0x20 up that is not a quote or a
					// backslash, including every byte above 0x7F, is itself.
					if (byte != '\\') {
						out.push_back(Text[At++]);
						continue;
					}

					At++;
					if (Done()) {
						return Fail("a string ends inside an escape");
					}

					const char escape = Text[At++];
					switch (escape) {
					case '"':
						out.push_back('"');
						continue;
					case '\\':
						out.push_back('\\');
						continue;
					case '/':
						out.push_back('/');
						continue;
					case 'b':
						out.push_back('\b');
						continue;
					case 'f':
						out.push_back('\f');
						continue;
					case 'n':
						out.push_back('\n');
						continue;
					case 'r':
						out.push_back('\r');
						continue;
					case 't':
						out.push_back('\t');
						continue;
					case 'u':
						break;
					default:
						return Fail("a string holds an unknown escape");
					}

					uint32_t unit = 0;
					if (!ReadHex4(unit)) {
						return false;
					}

					// **A surrogate pair is joined; a lone surrogate is
					// refused.** A `\uD83D` on its own names half a character
					// and has no UTF-8 encoding at all — the WTF-8 form exists
					// but nothing downstream reads it, so admitting one would
					// put bytes in a Lua string that no consumer can decode and
					// that `JSONEncode` would then hand back unchanged as
					// invalid UTF-8.
					if (unit >= 0xDC00 && unit <= 0xDFFF) {
						return Fail("a \\u escape is an unpaired low surrogate");
					}

					if (unit >= 0xD800 && unit <= 0xDBFF) {
						if (At + 2 > Text.size() || Text[At] != '\\' || Text[At + 1] != 'u') {
							return Fail("a \\u escape is an unpaired high surrogate");
						}
						At += 2;

						uint32_t low = 0;
						if (!ReadHex4(low)) {
							return false;
						}
						if (low < 0xDC00 || low > 0xDFFF) {
							return Fail("a \\u escape is an unpaired high surrogate");
						}
						unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
					}

					AppendUtf8(unit, out);
				}
			}

			// Reads a number, cursor on its first byte.
			//
			// **The grammar is `std::from_chars`'s, applied to the maximal run of
			// bytes a number can be made of.** Scanning the run first is what
			// keeps `inf` and `nan` out — `from_chars` accepts both words and
			// JSON has neither — and requiring the whole run to be consumed is
			// what makes `1..2` and `1e` errors rather than a `1` followed by
			// rubbish nothing looked at.
			bool ReadNumber(ScriptValue &out) {
				const size_t start = At;
				while (!Done()) {
					const char character = Peek();
					const bool numeric = (character >= '0' && character <= '9') || character == '-' ||
										 character == '+' || character == '.' || character == 'e' ||
										 character == 'E';
					if (!numeric) {
						break;
					}
					At++;
				}

				const std::string_view run = Text.substr(start, At - start);
				double value = 0.0;
				const std::from_chars_result read =
					std::from_chars(run.data(), run.data() + run.size(), value);

				if (read.ec == std::errc::result_out_of_range) {
					// **Out of a double's range is malformed, not infinity.**
					// `1e400` would otherwise decode to an infinity that
					// `JSONEncode` refuses, and this reader's one invariant is
					// that it never produces a value the writer will not take.
					return Fail("a number is outside what a double can hold");
				}
				if (read.ec != std::errc{} || read.ptr != run.data() + run.size()) {
					return Fail("a number is malformed");
				}

				out = ScriptValue{ValueTag::Number};
				out.Number = value;
				return true;
			}

			// Reads `true`, `false` or `null` at the cursor.
			bool ReadWord(std::string_view word, ValueTag tag, ScriptValue &out) {
				if (Text.substr(At, word.size()) != word) {
					return Fail("a value is not recognised");
				}
				At += word.size();

				out = ScriptValue{tag};
				out.Boolean = tag == ValueTag::True;
				return true;
			}

			// Reads one value at the cursor.
			//
			// @param out   Filled in.
			// @param depth How deep this value sits. Zero at the top.
			// @return Whether a value was read.
			bool Read(ScriptValue &out, uint32_t depth) {
				// **The same bound the encoder has, and that is why it is this
				// number rather than a larger one that JSON alone would allow.**
				// A document nested deeper than `CODEC_MAX_DEPTH` would decode
				// into a table `JSONEncode` then refuses — and a recursive
				// descent with no bound at all is a C stack overflow from a
				// string a script was handed.
				if (depth > CODEC_MAX_DEPTH) {
					return Fail(Describe(CodecStatus::TooDeep));
				}

				SkipSpace();
				if (Done()) {
					return Fail("the document ends where a value was expected");
				}

				switch (Peek()) {
				case '{':
					return ReadObject(out, depth);
				case '[':
					return ReadArray(out, depth);
				case '"': {
					out = ScriptValue{ValueTag::String};
					return ReadString(out.Text);
				}
				case 't':
					return ReadWord("true", ValueTag::True, out);
				case 'f':
					return ReadWord("false", ValueTag::False, out);

				// **`null` decodes to `nil`, and Lua has no second answer.**
				// Inside an object the key is simply absent; inside an array it
				// is a hole, so `#` stops meaning what it did. That is the same
				// thing Roblox does, and the alternative — a sentinel value
				// standing for null — is a value every script that touches JSON
				// would then have to know about.
				case 'n':
					return ReadWord("null", ValueTag::Nil, out);
				default:
					return ReadNumber(out);
				}
			}

			bool ReadArray(ScriptValue &out, uint32_t depth) {
				At++;
				out = ScriptValue{ValueTag::Array};

				SkipSpace();
				if (!Done() && Peek() == ']') {
					At++;
					return true;
				}

				while (true) {
					ScriptValue item;
					if (!Read(item, depth + 1)) {
						return false;
					}
					out.Items.push_back(std::move(item));

					SkipSpace();
					if (Done()) {
						return Fail("an array is not closed");
					}
					if (Peek() == ']') {
						At++;
						return true;
					}
					if (Peek() != ',') {
						return Fail("an array wants a ',' or a ']'");
					}
					At++;
				}
			}

			// **A duplicate key is last-one-wins**, which is what every
			// mainstream parser does and what `PushScriptValue` produces anyway
			// by setting the entries in order. Refusing the document instead
			// would reject text that every other tool in the world accepts.
			bool ReadObject(ScriptValue &out, uint32_t depth) {
				At++;
				out = ScriptValue{ValueTag::Map};

				SkipSpace();
				if (!Done() && Peek() == '}') {
					At++;
					return true;
				}

				while (true) {
					SkipSpace();
					if (Done() || Peek() != '"') {
						return Fail("an object wants a string key");
					}

					std::string key;
					if (!ReadString(key)) {
						return false;
					}

					SkipSpace();
					if (Done() || Peek() != ':') {
						return Fail("an object key wants a ':'");
					}
					At++;

					ScriptValue value;
					if (!Read(value, depth + 1)) {
						return false;
					}
					out.Entries.emplace_back(std::move(key), std::move(value));

					SkipSpace();
					if (Done()) {
						return Fail("an object is not closed");
					}
					if (Peek() == '}') {
						At++;
						return true;
					}
					if (Peek() != ',') {
						return Fail("an object wants a ',' or a '}'");
					}
					At++;
				}
			}
		};

		// --- the methods -----------------------------------------------------

		// A walker refusal, said the way a JSON caller will read it.
		//
		// **Not `Describe` itself**, whose wording is a bus's: "cannot cross a
		// world boundary" is the right sentence for `PublishAsync` and a
		// confusing one for a script that only asked for a string. The statuses
		// are shared because the *rules* are shared; the sentences are not,
		// because the two callers are asking different questions.
		const char *DescribeForJson(CodecStatus status) {
			switch (status) {
			case CodecStatus::Cyclic:
				return "the table contains itself";
			case CodecStatus::Unsupported:
				return "the value holds something with no JSON form";
			default:
				return Describe(status);
			}
		}

		// HttpService:JSONEncode(value) -> string
		//
		// **Every table rule here is `Codec.hpp`'s, reached through
		// `ReadScriptValue`, and they are listed because each one is a decision
		// somebody will otherwise assume was an accident:**
		//
		// - **An array is a table whose keys are exactly 1 to `#t`** and nothing
		//   else. It writes as `[...]`.
		// - **Anything else is an object**, including a table with a hole. So
		//   `{1, 2, nil, 4}` is `{"1":1,"2":2,"4":4}` rather than an array with
		//   the tail silently lost, which is the failure mode of every encoder
		//   that trusts `#`.
		// - **A mixed table is an object**, and its numeric keys become their
		//   decimal text: `{1, 2, x = 3}` is `{"1":1,"2":2,"x":3}`. JSON objects
		//   have string keys and JavaScript's do too, so a key type that depended
		//   on which VM wrote it would not be one format.
		// - **An empty table is `{}` and not `[]`.** Lua cannot tell an empty
		//   list from an empty map, so one of the two has to be picked; picking
		//   the same one the codec picks is what stops a value from having one
		//   shape on a bus and another in a document. This differs from Roblox,
		//   which writes `[]`, and from `ReadHostValue`, which reads `{}` as an
		//   array for a reason of its own that `script/AGENTS.md` gives.
		// - **A key that is not a string, a number or a boolean is refused**, and
		//   `Services.cpp` has the reason: stringifying a table gives its
		//   address.
		// - **A cycle is an error**, named as one, rather than a hang.
		// - **Nesting past `CODEC_MAX_DEPTH` is refused.**
		// - **An `Instance` or a function is refused**, because neither means
		//   anything outside this world.
		//
		// And the rules JSON itself forces, decided above: a NaN or an infinity
		// is refused; a `Vector3`, `Color3` or `CFrame` is refused; map keys are
		// written in sorted order; a number is written as the shortest decimal
		// that reads back as the same double.
		void JsonEncode(ScriptCall &call) {
			ScriptValue value;
			CodecStatus why = CodecStatus::Ok;

			if (!call.ReadValue(0, value, why)) {
				call.Raise((std::string("JSONEncode: ") + DescribeForJson(why)).c_str());
			}

			std::string text;
			if (const char *failure = WriteJson(value, text); failure != nullptr) {
				call.Raise((std::string("JSONEncode: ") + failure).c_str());
			}

			call.ReturnString(text);
		}

		// HttpService:JSONDecode(text) -> any
		//
		// **Arrays come back one-based.** `#`, `ipairs` and `table.remove` all
		// mean one-based in Luau, so a zero-based table handed to a script is a
		// list whose first element is invisible to every idiom an author has —
		// and the bug that produces reads as "the server sent me nine of the ten
		// rows". `ScriptCall::ReturnValue` is what makes it so, which is also
		// what makes a decoded document and a delivered message the same shape.
		void JsonDecode(ScriptCall &call) {
			const std::string text = call.AsString(0);
			JsonReader reader{.Text = std::string_view(text)};

			ScriptValue value;
			if (!reader.Read(value, 0)) {
				call.Raise((std::string("JSONDecode: ") + reader.Failure).c_str());
			}

			// **Trailing text is an error rather than something to ignore.**
			// `{"a":1} oops` is far more often a truncated concatenation than a
			// document with a comment on the end, and a parser that stops at the
			// first complete value hides which.
			reader.SkipSpace();
			if (!reader.Done()) {
				call.Raise("JSONDecode: there is text after the value");
			}

			call.ReturnValue(value);
		}

		// A stable 32-bit salt from a world's name.
		//
		// FNV-1a, spelled out here rather than reached for from `core`, because
		// what is wanted is a *specified* mixing of bytes that is the same on
		// every platform — `std::hash` explicitly is not, and `AGENTS.md` rule 4
		// is the same concern one level up.
		uint32_t SaltOf(std::string_view name) {
			uint32_t hash = 2166136261u;
			for (const char character : name) {
				hash ^= static_cast<unsigned char>(character);
				hash *= 16777619u;
			}
			return hash;
		}

		// HttpService:GenerateGUID(wrapInCurlyBraces) -> string
		//
		// **It is deterministic, and that is a decision this engine forces.**
		// `script/AGENTS.md` sets one test for anything added to this VM — what
		// can it observe that a recording cannot reproduce? A GUID drawn from the
		// operating system's entropy or from a clock fails that test outright: a
		// script that names a part after one, or stores one in a property,
		// produces a world that replays differently every time, and the failure
		// surfaces as a byte diff in a recording a long way from the call. It is
		// the same reason `os.time` is not opened.
		//
		// **`just determinism` and `just replay-check` would not have caught
		// it.** Both drive `mono.server` with `--entities` and `--ticks` and no
		// `--game`, so neither run has a script runtime in it at all. The rule is
		// the reason here, not the recipe — which is exactly the third category
		// `AGENTS.md` rule 6 warns about, so it is written down rather than left
		// to be rediscovered.
		//
		// So: `core::Random`, which is a specified hash of an index and a salt
		// and is already what `Random.new()` draws from for the same reason. The
		// index is this runtime's draw counter and the salt comes from the
		// world's name, so two worlds in one universe get different streams and
		// two runs of one world get the same one.
		//
		// **What that costs, said plainly: these are not unpredictable and not
		// unique across processes.** Two servers hosting the same world hand out
		// the same sequence. `core::Random`'s own header says it is not for
		// anything security-sensitive, and a GUID from here must not be used as a
		// session token, an unguessable name, or anything a second machine has to
		// agree is distinct. It is an identifier for things inside one simulated
		// world, which is what a script actually uses one for.
		//
		// The version and variant bits are stamped anyway, so the string parses
		// as a UUID for anything that reads it as one. That is a statement about
		// the *shape*, not a claim about the entropy.
		//
		// Four draws of thirty-two bits each. About 190 nanoseconds, since every
		// draw is a full SHA-256 compression — fine for naming a thing, wrong
		// inside a per-tick loop, and `core/Random.hpp` says why.
		void GenerateGuid(ScriptCall &call) {
			// **Braces by default**, which is Roblox's default and the surprising
			// half of its signature. A script that passes nothing gets
			// `{XXXXXXXX-...}`.
			//
			// **The two languages disagree about `GenerateGUID(0)`**, and the
			// interface says why: this is each language's own truthiness, and
			// zero is truthy in one and falsy in the other.
			const bool braces = call.OptionalBoolean(0, true);

			const uint32_t salt = SaltOf(call.World().Name());
			const auto index = static_cast<uint32_t>(call.NextGuid());

			std::array<uint8_t, 16> bytes{};
			for (uint32_t word = 0; word < 4; word++) {
				const uint32_t bits = core::Random::Bits(index, salt + word);
				bytes[word * 4 + 0] = static_cast<uint8_t>(bits >> 24);
				bytes[word * 4 + 1] = static_cast<uint8_t>(bits >> 16);
				bytes[word * 4 + 2] = static_cast<uint8_t>(bits >> 8);
				bytes[word * 4 + 3] = static_cast<uint8_t>(bits);
			}

			// RFC 4122's version 4 and its variant, in the two nibbles that
			// carry them.
			bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
			bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);

			// Upper case, because Roblox's is upper case and a script comparing
			// one against a literal it copied out of the output window would
			// otherwise fail on nothing.
			static constexpr char HEX[] = "0123456789ABCDEF";
			std::string guid;
			guid.reserve(38);

			if (braces) {
				guid.push_back('{');
			}
			for (size_t byte = 0; byte < bytes.size(); byte++) {
				if (byte == 4 || byte == 6 || byte == 8 || byte == 10) {
					guid.push_back('-');
				}
				guid.push_back(HEX[bytes[byte] >> 4]);
				guid.push_back(HEX[bytes[byte] & 0x0F]);
			}
			if (braces) {
				guid.push_back('}');
			}

			call.ReturnString(guid);
		}

		// HttpService:UrlEncode(text) -> string
		//
		// **RFC 3986's unreserved set through unchanged, every other byte as
		// `%XX`.** A space is `%20` and not `+`: the plus form belongs to
		// `application/x-www-form-urlencoded` and means a literal plus everywhere
		// else in a URL, so an encoder that used it produces a query string that
		// is right in a form body and wrong in a path.
		//
		// Byte by byte, so a UTF-8 string comes out as one `%XX` per byte, which
		// is what a URL carries. Upper-case hex digits, which RFC 3986 §2.1
		// prefers.
		void UrlEncode(ScriptCall &call) {
			const std::string text = call.AsString(0);

			static constexpr char HEX[] = "0123456789ABCDEF";
			std::string encoded;
			encoded.reserve(text.size());

			for (size_t at = 0; at < text.size(); at++) {
				const auto byte = static_cast<unsigned char>(text[at]);
				const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
										(byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
										byte == '.' || byte == '~';

				if (unreserved) {
					encoded.push_back(static_cast<char>(byte));
					continue;
				}

				encoded.push_back('%');
				encoded.push_back(HEX[byte >> 4]);
				encoded.push_back(HEX[byte & 0x0F]);
			}

			call.ReturnString(encoded);
		}

		// **Four methods, and the three that are missing are missing on
		// purpose.** Do not add `RequestAsync`, `GetAsync` or `PostAsync` here —
		// this file's header says what would have to be decided first.
		constexpr std::array<ServiceMethod, 4> METHODS{{
			{"JSONEncode", JsonEncode},
			{"JSONDecode", JsonDecode},
			{"GenerateGUID", GenerateGuid},
			{"UrlEncode", UrlEncode},
		}};
	}

	const ServiceSurface &HttpServiceSurface() {
		static const ServiceSurface SURFACE = [] {
			ServiceSurface surface;
			surface.Name = "HttpService";
			surface.Methods = METHODS;
			return surface;
		}();
		return SURFACE;
	}
}
