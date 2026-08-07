#include <engine/net/http/Message.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>

// The whole of the wire format, and no socket in the file.
//
// **Strict rather than forgiving, and that is a security position rather than a
// style.** Postel's rule is the wrong one for a message parser sitting behind a
// port: every leniency is a spelling two implementations can read differently,
// and a chain of parsers that disagree about where a message ends is request
// smuggling. So a bare LF is refused where CRLF is required, a folded header
// line is refused, and two `Content-Length` fields are refused even when they
// agree — because "even when they agree" is a check somebody eventually
// loosens.
//
// **Nothing is sized from a length field before that field is bounded.** The
// body length is compared against `MessageLimits::BodyBytes` before anything is
// reserved, so a header claiming four gigabytes costs a comparison rather than
// an allocation.

namespace engine::net::http {
	namespace {
		constexpr std::string_view CRLF = "\r\n";
		constexpr std::string_view HEADER_END = "\r\n\r\n";
		constexpr std::string_view VERSION_1_1 = "HTTP/1.1";

		// The buffer as text. Every byte a message may legally contain is
		// ASCII, and the body is handled as bytes separately, so this is a view
		// rather than a conversion.
		std::string_view Text(std::span<const std::byte> buffer) {
			return {reinterpret_cast<const char *>(buffer.data()), buffer.size()};
		}

		bool IsSpace(char value) {
			return value == ' ' || value == '\t';
		}

		std::string_view Trim(std::string_view value) {
			while (!value.empty() && IsSpace(value.front())) {
				value.remove_prefix(1);
			}
			while (!value.empty() && IsSpace(value.back())) {
				value.remove_suffix(1);
			}
			return value;
		}

		char Lower(char value) {
			return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
		}

		std::string Lowered(std::string_view value) {
			std::string out(value);
			std::transform(out.begin(), out.end(), out.begin(), Lower);
			return out;
		}

		// A field name must be an RFC 9110 token. Refusing anything else is
		// what stops a name carrying a colon, a space or a CR — each of which
		// lets one field be read as two by a parser downstream.
		bool IsToken(std::string_view value) {
			if (value.empty()) {
				return false;
			}
			for (const char character : value) {
				const bool alphanumeric = (character >= 'a' && character <= 'z') ||
										  (character >= 'A' && character <= 'Z') ||
										  (character >= '0' && character <= '9');
				if (alphanumeric) {
					continue;
				}
				if (std::string_view("!#$%&'*+-.^_`|~").find(character) == std::string_view::npos) {
					return false;
				}
			}
			return true;
		}

		// A field value may not carry a control character. A CR or LF inside
		// one is header injection: the value ends the field early and whatever
		// follows is read as another one.
		bool IsFieldValue(std::string_view value) {
			return std::none_of(value.begin(), value.end(), [](char character) {
				const auto raw = static_cast<unsigned char>(character);
				return raw < 0x20 || raw == 0x7F;
			});
		}

		std::optional<uint64_t> ParseNumber(std::string_view text) {
			if (text.empty() || text.size() > 20) {
				return std::nullopt;
			}
			// Digits only. from_chars would accept a leading `+`, and a length
			// field that two parsers read differently is the whole problem.
			if (!std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
				return std::nullopt;
			}
			uint64_t value = 0;
			const auto *const first = text.data();
			const auto ending = std::from_chars(first, first + text.size(), value);
			if (ending.ec != std::errc() || ending.ptr != first + text.size()) {
				return std::nullopt;
			}
			return value;
		}

		// `bytes=0-499`, `bytes=500-` or `bytes=-500`. One range only:
		// multipart range responses are a second body framing, and this file's
		// standing position is that one framing has nothing to disagree with.
		std::optional<ByteRange> ParseRange(std::string_view value) {
			constexpr std::string_view UNIT = "bytes=";
			if (!value.starts_with(UNIT)) {
				return std::nullopt;
			}
			value.remove_prefix(UNIT.size());
			if (value.find(',') != std::string_view::npos) {
				return std::nullopt;
			}

			const size_t dash = value.find('-');
			if (dash == std::string_view::npos) {
				return std::nullopt;
			}
			const std::string_view firstText = value.substr(0, dash);
			const std::string_view lastText = value.substr(dash + 1);

			if (firstText.empty()) {
				const std::optional<uint64_t> suffix = ParseNumber(lastText);
				if (!suffix || *suffix == 0) {
					return std::nullopt;
				}
				return ByteRange{.First = *suffix, .Last = 0, .Suffix = true};
			}

			const std::optional<uint64_t> first = ParseNumber(firstText);
			if (!first) {
				return std::nullopt;
			}
			if (lastText.empty()) {
				// An open range. Resolve clamps it to the entity.
				return ByteRange{.First = *first, .Last = UINT64_MAX, .Suffix = false};
			}
			const std::optional<uint64_t> last = ParseNumber(lastText);
			if (!last || *last < *first) {
				return std::nullopt;
			}
			return ByteRange{.First = *first, .Last = *last, .Suffix = false};
		}

		// What both parsers share: the header block between the first line and
		// the blank line, and the body length it declares.
		struct HeaderBlock {
			std::vector<Header> Fields;
			uint64_t BodyBytes = 0;
			bool HasLength = false;
		};

		ParseResult ParseHeaders(std::string_view block, const MessageLimits &limits, HeaderBlock &out) {
			size_t seenLength = 0;
			while (!block.empty()) {
				const size_t lineEnd = block.find(CRLF);
				if (lineEnd == std::string_view::npos) {
					return ParseResult::Malformed;
				}
				const std::string_view line = block.substr(0, lineEnd);
				block.remove_prefix(lineEnd + CRLF.size());

				// A line opening with whitespace is an obsolete folded value.
				// Refused: it is the classic way to make two parsers disagree
				// about where a field ends.
				if (!line.empty() && IsSpace(line.front())) {
					return ParseResult::Malformed;
				}
				const size_t colon = line.find(':');
				if (colon == std::string_view::npos) {
					return ParseResult::Malformed;
				}
				// No space is allowed before the colon; one there means the
				// name is not a token and the field is ambiguous.
				const std::string_view name = line.substr(0, colon);
				const std::string_view value = Trim(line.substr(colon + 1));
				if (!IsToken(name) || !IsFieldValue(value)) {
					return ParseResult::Malformed;
				}
				if (out.Fields.size() >= limits.HeaderCount) {
					return ParseResult::TooLarge;
				}

				Header field{.Name = Lowered(name), .Value = std::string(value)};

				// One framing, and only one. A message carrying both a length
				// and a transfer encoding is the smuggling primitive; a message
				// carrying two lengths is the same primitive with fewer steps,
				// and both are refused before either value is acted on.
				if (field.Name == "transfer-encoding") {
					return ParseResult::Malformed;
				}
				if (field.Name == "content-length") {
					if (++seenLength > 1) {
						return ParseResult::Malformed;
					}
					const std::optional<uint64_t> length = ParseNumber(field.Value);
					if (!length) {
						return ParseResult::Malformed;
					}
					if (*length > limits.BodyBytes) {
						return ParseResult::TooLarge;
					}
					out.BodyBytes = *length;
					out.HasLength = true;
				}

				out.Fields.push_back(std::move(field));
			}
			return ParseResult::Ok;
		}

		// Finds the end of the header block, or says why there is not one yet.
		//
		// The bound is applied to what has arrived rather than to what was
		// promised, which is the only order that works: a peer that never sends
		// a blank line is bounded by how much it has sent.
		ParseResult FindHeaderEnd(std::string_view text, size_t ceiling, size_t &end) {
			const size_t found = text.find(HEADER_END);
			if (found == std::string_view::npos) {
				return text.size() > ceiling ? ParseResult::TooLarge : ParseResult::Incomplete;
			}
			if (found + HEADER_END.size() > ceiling) {
				return ParseResult::TooLarge;
			}
			end = found;
			return ParseResult::Ok;
		}
	}

	const char *Describe(Method method) {
		switch (method) {
		case Method::Get:
			return "GET";
		case Method::Head:
			return "HEAD";
		case Method::Put:
			return "PUT";
		case Method::Unknown:
			break;
		}
		return "UNKNOWN";
	}

	const char *Describe(Status status) {
		switch (status) {
		case Status::Ok:
			return "OK";
		case Status::PartialContent:
			return "Partial Content";
		case Status::BadRequest:
			return "Bad Request";
		case Status::Forbidden:
			return "Forbidden";
		case Status::NotFound:
			return "Not Found";
		case Status::ContentTooLarge:
			return "Content Too Large";
		case Status::UriTooLong:
			return "URI Too Long";
		case Status::RangeNotSatisfiable:
			return "Range Not Satisfiable";
		case Status::InternalError:
			return "Internal Server Error";
		case Status::NotImplemented:
			return "Not Implemented";
		case Status::ServiceUnavailable:
			return "Service Unavailable";
		case Status::Unknown:
			break;
		}
		return "Unknown";
	}

	const char *Describe(ParseResult result) {
		switch (result) {
		case ParseResult::Ok:
			return "ok";
		case ParseResult::Incomplete:
			return "incomplete";
		case ParseResult::Malformed:
			return "malformed";
		case ParseResult::TooLarge:
			return "too-large";
		}
		return "unknown";
	}

	std::optional<ByteRange> ByteRange::Resolve(uint64_t entityBytes) const {
		if (entityBytes == 0) {
			return std::nullopt;
		}
		if (Suffix) {
			// `First` holds the suffix length here. A suffix longer than the
			// entity is the whole entity rather than a refusal, which is what
			// RFC 9110 says and what a resuming client relies on.
			const uint64_t length = std::min(First, entityBytes);
			return ByteRange{.First = entityBytes - length, .Last = entityBytes - 1, .Suffix = false};
		}
		if (First >= entityBytes) {
			return std::nullopt;
		}
		return ByteRange{.First = First, .Last = std::min(Last, entityBytes - 1), .Suffix = false};
	}

	std::optional<std::string_view> Request::Find(std::string_view name) const {
		for (const Header &field : Headers) {
			if (field.Name == name) {
				return field.Value;
			}
		}
		return std::nullopt;
	}

	std::optional<std::string_view> Response::Find(std::string_view name) const {
		for (const Header &field : Headers) {
			if (field.Name == name) {
				return field.Value;
			}
		}
		return std::nullopt;
	}

	void Response::Set(std::string_view name, std::string_view value) {
		const std::string lowered = Lowered(name);
		for (Header &field : Headers) {
			if (field.Name == lowered) {
				field.Value = std::string(value);
				return;
			}
		}
		Headers.push_back(Header{.Name = lowered, .Value = std::string(value)});
	}

	ParseResult ParseRequest(
		std::span<const std::byte> buffer, const MessageLimits &limits, Request &request, size_t &consumed
	) {
		const std::string_view text = Text(buffer);

		size_t headerEnd = 0;
		const ParseResult found =
			FindHeaderEnd(text, limits.RequestLineBytes + limits.HeaderBytes, headerEnd);
		if (found != ParseResult::Ok) {
			return found;
		}

		const size_t lineEnd = text.find(CRLF);
		if (lineEnd == std::string_view::npos || lineEnd > headerEnd) {
			return ParseResult::Malformed;
		}
		if (lineEnd > limits.RequestLineBytes) {
			return ParseResult::TooLarge;
		}

		// METHOD SP TARGET SP VERSION, with exactly one space at each break.
		const std::string_view line = text.substr(0, lineEnd);
		const size_t firstSpace = line.find(' ');
		if (firstSpace == std::string_view::npos) {
			return ParseResult::Malformed;
		}
		const size_t secondSpace = line.find(' ', firstSpace + 1);
		if (secondSpace == std::string_view::npos) {
			return ParseResult::Malformed;
		}
		const std::string_view verb = line.substr(0, firstSpace);
		const std::string_view target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
		const std::string_view version = line.substr(secondSpace + 1);

		if (version != VERSION_1_1) {
			return ParseResult::Malformed;
		}
		if (target.empty() || target.front() != '/') {
			// Origin-form only. An absolute-form target is what a request to a
			// forward proxy looks like, and this is not one — accepting it
			// would make the origin's target space depend on a host field.
			return ParseResult::Malformed;
		}
		if (!IsFieldValue(target)) {
			return ParseResult::Malformed;
		}

		Request parsed;
		// An unrecognised verb parses rather than refusing, so the caller can
		// answer 501 on a well-formed request instead of dropping a connection
		// that did nothing wrong.
		if (verb == "GET") {
			parsed.Verb = Method::Get;
		} else if (verb == "HEAD") {
			parsed.Verb = Method::Head;
		} else if (verb == "PUT") {
			parsed.Verb = Method::Put;
		} else if (!IsToken(verb)) {
			return ParseResult::Malformed;
		}
		parsed.Target = std::string(target);

		// The block runs from just past the request line to just past the last
		// header's CRLF — `headerEnd` indexes the first of the four bytes that
		// end the block, so the last field's own terminator is included and
		// every line the loop sees ends the same way. With no headers at all
		// the two bounds meet and the block is empty.
		const size_t blockStart = lineEnd + CRLF.size();
		const size_t blockEnd = headerEnd + CRLF.size();
		const std::string_view block = text.substr(blockStart, blockEnd - blockStart);
		if (block.size() > limits.HeaderBytes) {
			return ParseResult::TooLarge;
		}

		HeaderBlock headers;
		const ParseResult fields = ParseHeaders(block, limits, headers);
		if (fields != ParseResult::Ok) {
			return fields;
		}

		// **A body is `Put`'s and no other verb's.** A `GET` or a `HEAD` that
		// declares a non-zero length is refused rather than skipped over,
		// because skipping it means trusting a length to find the next message
		// — and a body on a verb whose framing intermediaries disagree about is
		// the request-smuggling primitive itself, not merely adjacent to it.
		//
		// `Unknown` is held to the same rule so that a well-formed
		// `DELETE`/`POST` still reaches the `501` the caller owes it, rather
		// than dropping a connection that did nothing wrong.
		if (parsed.Verb != Method::Put) {
			if (headers.HasLength && headers.BodyBytes != 0) {
				return ParseResult::Malformed;
			}
		} else if (!headers.HasLength) {
			// **A `Put` states its length or it is not a `Put`.** The other
			// framing — `transfer-encoding` — is refused outright above, so
			// without a length there is no way to know where this message ends
			// and guessing "empty" would silently store nothing.
			return ParseResult::Malformed;
		}

		// The whole body has to be here before this parses, which is why an
		// origin that accepts uploads sizes `ConnectionBufferBytes` to hold
		// one. Waiting is `Incomplete` and not an error: it is the ordinary
		// state of a large upload halfway across a socket.
		const size_t bodyStart = headerEnd + HEADER_END.size();
		const uint64_t arriving = parsed.Verb == Method::Put ? headers.BodyBytes : 0;
		if (text.size() - bodyStart < arriving) {
			return ParseResult::Incomplete;
		}

		parsed.Body.assign(
			buffer.begin() + static_cast<ptrdiff_t>(bodyStart),
			buffer.begin() + static_cast<ptrdiff_t>(bodyStart + arriving)
		);

		parsed.Headers = std::move(headers.Fields);
		if (const std::optional<std::string_view> range = parsed.Find("range")) {
			parsed.Range = ParseRange(*range);
			if (!parsed.Range) {
				// An unparseable range is ignored rather than refused, which is
				// what RFC 9110 requires: the response is then the whole
				// entity. Refusing would break a client whose proxy rewrote it.
			}
		}

		consumed = bodyStart + arriving;
		request = std::move(parsed);
		return ParseResult::Ok;
	}

	ParseResult ParseResponse(
		std::span<const std::byte> buffer,
		const MessageLimits &limits,
		bool bodyOmitted,
		Response &response,
		size_t &consumed
	) {
		const std::string_view text = Text(buffer);

		size_t headerEnd = 0;
		const ParseResult found =
			FindHeaderEnd(text, limits.RequestLineBytes + limits.HeaderBytes, headerEnd);
		if (found != ParseResult::Ok) {
			return found;
		}

		const size_t lineEnd = text.find(CRLF);
		if (lineEnd == std::string_view::npos || lineEnd > headerEnd) {
			return ParseResult::Malformed;
		}

		// VERSION SP CODE SP REASON, and the reason may be empty.
		const std::string_view line = text.substr(0, lineEnd);
		const size_t firstSpace = line.find(' ');
		if (firstSpace == std::string_view::npos) {
			return ParseResult::Malformed;
		}
		if (line.substr(0, firstSpace) != VERSION_1_1) {
			return ParseResult::Malformed;
		}
		const std::string_view rest = line.substr(firstSpace + 1);
		const size_t codeEnd = std::min(rest.find(' '), rest.size());
		const std::string_view codeText = rest.substr(0, codeEnd);
		if (codeText.size() != 3) {
			return ParseResult::Malformed;
		}
		const std::optional<uint64_t> code = ParseNumber(codeText);
		if (!code) {
			return ParseResult::Malformed;
		}

		const size_t blockStart = lineEnd + CRLF.size();
		const size_t blockEnd = headerEnd + CRLF.size();
		const std::string_view block = text.substr(blockStart, blockEnd - blockStart);
		if (block.size() > limits.HeaderBytes) {
			return ParseResult::TooLarge;
		}

		HeaderBlock headers;
		const ParseResult fields = ParseHeaders(block, limits, headers);
		if (fields != ParseResult::Ok) {
			return fields;
		}

		// **No length, no message.** Reading to end-of-connection would let an
		// origin present a truncated group as a complete one, and the client
		// would then hash short bytes and report content corruption for what
		// was a dropped socket.
		if (!headers.HasLength) {
			return ParseResult::Malformed;
		}

		// A `Head` answer's length describes what a `Get` would have returned,
		// so waiting for that many bytes waits for bytes nobody will send.
		const uint64_t arriving = bodyOmitted ? 0 : headers.BodyBytes;

		const size_t bodyStart = headerEnd + HEADER_END.size();
		if (text.size() - bodyStart < arriving) {
			return ParseResult::Incomplete;
		}

		Response parsed;
		switch (*code) {
		case 200:
			parsed.Code = Status::Ok;
			break;
		case 206:
			parsed.Code = Status::PartialContent;
			break;
		case 400:
			parsed.Code = Status::BadRequest;
			break;
		case 403:
			parsed.Code = Status::Forbidden;
			break;
		case 404:
			parsed.Code = Status::NotFound;
			break;
		case 413:
			parsed.Code = Status::ContentTooLarge;
			break;
		case 414:
			parsed.Code = Status::UriTooLong;
			break;
		case 416:
			parsed.Code = Status::RangeNotSatisfiable;
			break;
		case 500:
			parsed.Code = Status::InternalError;
			break;
		case 501:
			parsed.Code = Status::NotImplemented;
			break;
		case 503:
			parsed.Code = Status::ServiceUnavailable;
			break;
		default:
			// Parsed, not refused: a client must survive an origin or a
			// proxy answering something this subset never emits.
			parsed.Code = Status::Unknown;
			break;
		}
		parsed.Headers = std::move(headers.Fields);
		parsed.Body.assign(
			buffer.begin() + static_cast<ptrdiff_t>(bodyStart),
			buffer.begin() + static_cast<ptrdiff_t>(bodyStart + arriving)
		);

		consumed = bodyStart + arriving;
		response = std::move(parsed);
		return ParseResult::Ok;
	}

	namespace {
		void Append(std::vector<std::byte> &out, std::string_view text) {
			const auto *const first = reinterpret_cast<const std::byte *>(text.data());
			out.insert(out.end(), first, first + text.size());
		}
	}

	void WriteRequest(const Request &request, std::string_view host, std::vector<std::byte> &out) {
		Append(out, Describe(request.Verb));
		Append(out, " ");
		Append(out, request.Target);
		Append(out, " ");
		Append(out, VERSION_1_1);
		Append(out, CRLF);

		Append(out, "host: ");
		Append(out, host);
		Append(out, CRLF);

		if (request.Range) {
			Append(out, "range: bytes=");
			if (request.Range->Suffix) {
				Append(out, "-");
				Append(out, std::to_string(request.Range->First));
			} else {
				Append(out, std::to_string(request.Range->First));
				Append(out, "-");
				if (request.Range->Last != UINT64_MAX) {
					Append(out, std::to_string(request.Range->Last));
				}
			}
			Append(out, CRLF);
		}

		for (const Header &field : request.Headers) {
			// `host`, `range` and the framing fields are written from the
			// structure above. A caller that also put one in the list would
			// otherwise produce a duplicate, which this file refuses on the
			// way in and must not produce on the way out.
			if (field.Name == "host" || field.Name == "range" || field.Name == "content-length" ||
				field.Name == "transfer-encoding") {
				continue;
			}
			Append(out, field.Name);
			Append(out, ": ");
			Append(out, field.Value);
			Append(out, CRLF);
		}

		// **Written only for the verb that may carry one, and written from the
		// body rather than from the header list** — `WriteResponse`'s rule,
		// and for its reason: two sources for one number is one number that
		// will eventually be wrong, and the symptom is a connection that
		// desynchronises a message later.
		//
		// A `Get` gets no `content-length` at all rather than a zero, because
		// `ParseRequest` refuses a non-`Put` that declares one and a zero would
		// be one more field on every fetch this engine makes.
		if (request.Verb == Method::Put) {
			Append(out, "content-length: ");
			Append(out, std::to_string(request.Body.size()));
			Append(out, CRLF);
		}

		Append(out, CRLF);

		if (request.Verb == Method::Put) {
			out.insert(out.end(), request.Body.begin(), request.Body.end());
		}
	}

	void WriteResponse(const Response &response, bool bodyOmitted, std::vector<std::byte> &out) {
		Append(out, VERSION_1_1);
		Append(out, " ");
		Append(out, std::to_string(static_cast<uint16_t>(response.Code)));
		Append(out, " ");
		Append(out, Describe(response.Code));
		Append(out, CRLF);

		for (const Header &field : response.Headers) {
			if (field.Name == "content-length" || field.Name == "transfer-encoding") {
				continue;
			}
			Append(out, field.Name);
			Append(out, ": ");
			Append(out, field.Value);
			Append(out, CRLF);
		}

		// Written from the body, never from the header list. Two sources for
		// one number is one number that will eventually be wrong, and the
		// symptom is a connection that desynchronises one message later.
		Append(out, "content-length: ");
		Append(out, std::to_string(response.Body.size()));
		Append(out, CRLF);
		Append(out, CRLF);

		if (!bodyOmitted) {
			out.insert(out.end(), response.Body.begin(), response.Body.end());
		}
	}
}
