#pragma once

// arch-waiver public-header: forward net API. HTTP adapters exchange this
// complete message contract without a second wire representation.

// HTTP/1.1 requests and responses as values, with no socket anywhere near them.
//
// This module owns an `http/` sub-area for userland networking and the
// server's own asset serving, and a content origin is what needs it: bulk
// bytes go over a request/response protocol rather than over a game datagram
// channel with a per-tick packet budget. A group is megabytes;
// `Packet::MAXIMUM_PAYLOAD_BYTES` is 1200.
//
// **The protocol is split from the socket, and that split is the design.**
// Everything here is parsing and formatting over spans, so the whole of the
// wire format is exercised by a suite that opens no port and waits for nothing.
// `Server.hpp` and `Client.hpp` are the thin halves that own file descriptors,
// and they are the halves a test cannot run in microseconds.
//
// **The subset is deliberately small, and each omission closes something.**
// This is a content origin's protocol, not a web framework:
//
// - **`GET` and `HEAD` only.** An origin serves. Upload is `control/`'s, in
//   TypeScript, over its own API.
// - **`Content-Length` framing only. No `Transfer-Encoding`.** A body that can
//   be framed two ways is request smuggling: two parsers in a chain disagree
//   about where one message ends and the next begins, and the disagreement is
//   the attack. One framing has nothing to disagree about.
// - **No trailers, no continuation lines, no pipelining beyond one in flight.**
//   Each is a parsing subtlety with a documented desync behind it.
//
// **Every byte parsed here is hostile.** A request arrives from anyone who can
// reach the port and a response arrives from an origin anyone can run. Nothing here allocates from a length
// field it has not bounded, and a malformed message is refused whole rather than half-read into a partly
// filled value - the shape a caller uses by accident.
//
// @tier L11 · shared

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::net::http {

	// What a request asks for.
	//
	// Anything outside this list parses as `Unknown` and is answered `501`
	// rather than being guessed at.
	//
	// @since v0.9
	enum class Method : uint8_t {
		// Not a verb this subset implements.
		Unknown,

		// Fetch the target.
		Get,

		// Fetch the target's headers and no body. What a client uses to learn
		// a bundle's size before deciding to spend the bandwidth.
		Head,

		// Store the body at the target.
		//
		// **The one verb here that carries a body, and the only one that ever
		// will.** A `GET` or a `HEAD` with a `content-length` is refused rather
		// than read - see `ParseRequest` - because a body on a verb whose
		// framing nobody agrees about is precisely the request-smuggling shape:
		// two hops that disagree about where the message ended see two
		// different next requests. `PUT` is unambiguous, so `PUT` is where a
		// body is allowed and no other verb is.
		//
		// **Idempotent by the target being a content address.** `cdn::Service`
		// ingests at `/ingest/<hash>` and hashes what arrives, so the same file
		// sent twice is the same file, and a retry after a dropped socket is
		// free rather than a duplicate.
		//
		// @since v0.10
		Put,
	};

	// Returns a stable, human-readable name for a method.
	//
	// @param method The method to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Method method);

	// The status codes this subset produces.
	//
	// A closed list rather than a bare `uint16_t`, so an origin cannot invent a
	// code no client was written against. Parsing accepts any three-digit code
	// - a client must cope with whatever an origin answers - and maps the ones
	// outside this list to `Unknown`.
	//
	// @since v0.9
	enum class Status : uint16_t {
		// Not one of the codes below.
		Unknown = 0,

		// The whole entity follows.
		Ok = 200,

		// The requested byte range follows. What makes a large transfer
		// resumable rather than restartable.
		PartialContent = 206,

		// The request is not valid HTTP, or not valid in this subset.
		BadRequest = 400,

		// The grant does not admit this content. **Never says which check
		// failed** - a reason returned to a client is an oracle, which is the
		// rule `cdn::Gate` already holds.
		Forbidden = 403,

		// No such content here.
		NotFound = 404,

		// The request line or the headers are larger than the bound.
		ContentTooLarge = 413,

		// The target is longer than the bound.
		UriTooLong = 414,

		// The range does not overlap the entity at all.
		RangeNotSatisfiable = 416,

		// This end failed. Never carries a diagnostic in the body.
		InternalError = 500,

		// A verb or a feature this subset does not implement.
		NotImplemented = 501,

		// Nothing is published, or the origin is not ready to serve.
		ServiceUnavailable = 503,
	};

	// Returns the reason phrase for a status.
	//
	// @param status The status to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Status status);

	// One header field.
	//
	// **The name is lowercased when it is parsed**, once, so every later
	// comparison is a plain string compare. HTTP field names are
	// case-insensitive, and a codebase that compares them case-sensitively
	// works against every client that happens to spell them the way the author
	// did and fails against the first one that does not.
	//
	// @since v0.9
	struct Header {
		// The field name, lowercase.
		std::string Name;

		// The field value, with surrounding whitespace removed.
		std::string Value;
	};

	// A byte range from a `Range: bytes=` header.
	//
	// Half of what makes a multi-megabyte group survive a dropped connection:
	// a client that has 6 MB of a 16 MB bundle asks for the rest rather than
	// paying for the first 6 MB twice.
	//
	// @since v0.9
	struct ByteRange {
		// The first byte wanted, counting from zero.
		uint64_t First = 0;

		// The last byte wanted, inclusive. HTTP ranges are inclusive at both
		// ends, which is off by one from every other range in this codebase -
		// hence saying so here rather than leaving it to be rediscovered.
		uint64_t Last = 0;

		// Whether this was written as a suffix range - `bytes=-500`, meaning
		// the last 500 bytes.
		//
		// Kept rather than resolved at parse time, because resolving it needs
		// the entity's length and the parser does not have one. `Resolve` is
		// where the two meet.
		bool Suffix = false;

		// Turns this into absolute first/last positions against an entity.
		//
		// @param entityBytes The whole entity's length.
		// @return The resolved range, or nothing when it does not overlap the
		//         entity at all - which is a `416` rather than an empty body.
		std::optional<ByteRange> Resolve(uint64_t entityBytes) const;
	};

	// The bounds every parse here is held to.
	//
	// Bounds rather than "until it ends", because the length is the only thing
	// standing between an open port and an allocator. A peer that opens a
	// connection and sends header bytes forever is an ordinary denial of
	// service, and a parser with no ceiling is what makes it work.
	//
	// @since v0.9
	struct MessageLimits {
		// The longest request line - verb, target and version.
		size_t RequestLineBytes = 8u * 1024u;

		// The largest header block, all fields together.
		size_t HeaderBytes = 32u * 1024u;

		// The most header fields.
		size_t HeaderCount = 64;

		// The largest body this will accept, in either direction.
		//
		// Mostly this bounds a *response*, which is a compressed group and is
		// genuinely large. The delivery client also checks the length against
		// the signed manifest before it believes it - the decompression-bomb
		// rule - so this is the transport's backstop rather than the real check.
		//
		// Since v0.10 it also bounds a `Put` request's body. **That is not the
		// binding limit on an upload** and should not be mistaken for one:
		// `ServerSettings::ConnectionBufferBytes` is far smaller and is what an
		// upload actually has to fit inside, because a request is buffered
		// whole before it parses.
		uint64_t BodyBytes = 512ull * 1024u * 1024u;
	};

	// A parsed request.
	//
	// @since v0.9
	struct Request {
		// The verb.
		Method Verb = Method::Unknown;

		// The request target, as written - `/bundle/1a2b...`. Percent-decoding
		// is the caller's, and there is no path resolution here at all: a
		// target never becomes a filesystem path in this module, because
		// `cdn::ContentRoot` is the one place that turns a name into a path and
		// a second one is a second chance to get traversal wrong.
		std::string Target;

		// The headers, in the order they arrived, with lowercase names.
		std::vector<Header> Headers;

		// The `Range` header, if one parsed.
		std::optional<ByteRange> Range;

		// The body, empty for every verb but `Put`.
		//
		// **Bounded by `MessageLimits::BodyBytes` before a byte is kept**, the
		// same as a response's, and by `ServerSettings::ConnectionBufferBytes`
		// while it is still arriving. The second is the one that bites: a whole
		// request is buffered before it parses, so an origin that accepts
		// uploads has to be told it may hold one - `cdn::IngestSettings` sizes
		// it, and a file past that is `413` rather than a connection that fills
		// memory.
		//
		// @since v0.10
		std::vector<std::byte> Body;

		// The value of a header, or nothing.
		//
		// @param name The field name, lowercase.
		// @return The value, or nothing when the field is absent.
		std::optional<std::string_view> Find(std::string_view name) const;
	};

	// A parsed or assembled response.
	//
	// @since v0.9
	struct Response {
		// The status.
		Status Code = Status::Unknown;

		// The headers, with lowercase names. `content-length` is written by
		// `WriteResponse` from the body rather than taken from here, so the
		// framing cannot disagree with what is being framed.
		std::vector<Header> Headers;

		// The body.
		std::vector<std::byte> Body;

		// The value of a header, or nothing.
		//
		// @param name The field name, lowercase.
		// @return The value, or nothing when the field is absent.
		std::optional<std::string_view> Find(std::string_view name) const;

		// Adds a header, replacing any field of the same name.
		//
		// Replacing rather than appending, because two `content-type` fields
		// are a message two parsers can read differently.
		//
		// @param name The field name. Lowercased on the way in.
		// @param value The field value.
		void Set(std::string_view name, std::string_view value);
	};

	// How a parse ended.
	//
	// @since v0.9
	enum class ParseResult : uint8_t {
		// A whole message was parsed.
		Ok,

		// What is here so far is valid and there is not enough of it. Read
		// more and call again - the buffer is not consumed.
		Incomplete,

		// Not a message this subset accepts. **The connection is finished**:
		// once framing is in doubt there is no way to find where the next
		// message starts, and guessing is exactly the desync that makes
		// smuggling work.
		Malformed,

		// A bound in `MessageLimits` was exceeded.
		TooLarge,
	};

	// Returns a stable, human-readable name for a parse result.
	//
	// @param result The result to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(ParseResult result);

	// Parses one request from the front of a buffer.
	//
	// @param buffer The bytes received so far. Not consumed - `consumed` says
	//        how many belonged to this message.
	// @param limits The bounds to hold the parse to.
	// @param[out] request Filled only on `Ok`. Untouched otherwise, so a
	//        caller cannot use a half-parsed one by accident.
	// @param[out] consumed How many bytes the message occupied. Set only on
	//        `Ok`.
	// @return What happened.
	ParseResult ParseRequest(
		std::span<const std::byte> buffer, const MessageLimits &limits, Request &request, size_t &consumed
	);

	// Parses one response from the front of a buffer.
	//
	// A response with no `content-length` is refused rather than read to the
	// end of the connection. "Until the peer hangs up" is a framing an origin
	// can use to make a truncated group look complete, and the client would
	// then hash short bytes and report content corruption for what was a
	// transport failure.
	//
	// @param buffer The bytes received so far.
	// @param limits The bounds to hold the parse to.
	// @param bodyOmitted Whether the request this answers was a `Head`, whose
	//        response carries a length and no body. **The reader has to be told
	//        rather than work it out**: the length field describes what a `Get`
	//        would have returned, so a parser that believed it would wait for
	//        bytes that are never coming. This is the one place the response
	//        format is not self-describing, and it is why HTTP couples the two
	//        halves of an exchange at all.
	// @param[out] response Filled only on `Ok`.
	// @param[out] consumed How many bytes the message occupied. Set only on
	//        `Ok`.
	// @return What happened.
	ParseResult ParseResponse(
		std::span<const std::byte> buffer,
		const MessageLimits &limits,
		bool bodyOmitted,
		Response &response,
		size_t &consumed
	);

	// Writes a request.
	//
	// @param request What to send. A `Range` is written when present.
	// @param host The `Host` header's value, which HTTP/1.1 requires.
	// @param[out] out Appended to.
	void WriteRequest(const Request &request, std::string_view host, std::vector<std::byte> &out);

	// Writes a response.
	//
	// `content-length` is written from `Body` rather than from the header list,
	// so the framing and the thing being framed cannot disagree. A `Head`
	// response therefore sets the length and sends no body, which is what the
	// verb means.
	//
	// @param response What to send.
	// @param bodyOmitted Whether to write the headers and no body - the `Head`
	//        case. The length still describes what a `Get` would have returned.
	// @param[out] out Appended to.
	void WriteResponse(const Response &response, bool bodyOmitted, std::vector<std::byte> &out);
}
