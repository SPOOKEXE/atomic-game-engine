#include <engine/net/http/Message.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.http.message")

using engine::net::http::ByteRange;
using engine::net::http::Header;
using engine::net::http::MessageLimits;
using engine::net::http::Method;
using engine::net::http::ParseRequest;
using engine::net::http::ParseResponse;
using engine::net::http::ParseResult;
using engine::net::http::Request;
using engine::net::http::Response;
using engine::net::http::Status;
using engine::net::http::WriteRequest;
using engine::net::http::WriteResponse;

namespace {
	std::vector<std::byte> Raw(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		for (size_t index = 0; index < text.size(); ++index) {
			bytes[index] = static_cast<std::byte>(text[index]);
		}
		return bytes;
	}

	std::string Text(std::span<const std::byte> bytes) {
		std::string out;
		out.reserve(bytes.size());
		for (const std::byte value : bytes) {
			out.push_back(static_cast<char>(value));
		}
		return out;
	}

	ParseResult Parse(std::string_view wire, Request &request, const MessageLimits &limits = {}) {
		const std::vector<std::byte> bytes = Raw(wire);
		size_t consumed = 0;
		return ParseRequest(bytes, limits, request, consumed);
	}
}

TEST_CASE("a minimal request parses", "[http]") {
	Request request;
	REQUIRE(Parse("GET /health HTTP/1.1\r\nhost: origin\r\n\r\n", request) == ParseResult::Ok);
	CHECK(request.Verb == Method::Get);
	CHECK(request.Target == "/health");
	REQUIRE(request.Find("host").has_value());
	CHECK(*request.Find("host") == "origin");
}

TEST_CASE("a request with no headers at all parses", "[http]") {
	// The block between the request line and the blank line is empty, which is
	// the boundary the two slice bounds meet at.
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\n\r\n", request) == ParseResult::Ok);
	CHECK(request.Target == "/");
	CHECK(request.Headers.empty());
}

TEST_CASE("consumed covers exactly one message", "[http]") {
	const std::vector<std::byte> bytes = Raw("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");
	Request first;
	size_t consumed = 0;
	REQUIRE(ParseRequest(bytes, {}, first, consumed) == ParseResult::Ok);
	CHECK(first.Target == "/a");

	// The second message parses from where the first ended, which is the
	// property a connection depends on to stay in step.
	Request second;
	size_t again = 0;
	const std::span<const std::byte> rest(bytes.data() + consumed, bytes.size() - consumed);
	REQUIRE(ParseRequest(rest, {}, second, again) == ParseResult::Ok);
	CHECK(second.Target == "/b");
}

TEST_CASE("a partial request is incomplete rather than malformed", "[http]") {
	Request request;
	CHECK(Parse("GET /health HTTP/1.1\r\nhost: ori", request) == ParseResult::Incomplete);
	CHECK(Parse("GET /heal", request) == ParseResult::Incomplete);
}

TEST_CASE("a half-parsed request is never handed back", "[http]") {
	// The out-parameter must be untouched on anything but Ok, because a partly
	// filled request is the shape a caller uses by accident.
	Request request;
	request.Target = "/untouched";
	CHECK(Parse("GET /new HTTP/1.1\r\nbad header\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(request.Target == "/untouched");
}

TEST_CASE("header names are lowercased so a comparison is a comparison", "[http]") {
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nX-Atomic-Grant: ABCD\r\n\r\n", request) == ParseResult::Ok);
	REQUIRE(request.Find("x-atomic-grant").has_value());
	// The value's case is content and is left alone.
	CHECK(*request.Find("x-atomic-grant") == "ABCD");
	CHECK_FALSE(request.Find("X-Atomic-Grant").has_value());
}

TEST_CASE("an unknown verb parses so it can be answered rather than dropped", "[http]") {
	Request request;
	REQUIRE(Parse("POST /upload HTTP/1.1\r\n\r\n", request) == ParseResult::Ok);
	CHECK(request.Verb == Method::Unknown);
}

// --- the refusals, each of which closes something --------------------------

TEST_CASE("transfer-encoding is refused outright", "[http]") {
	// A body framed two ways is request smuggling: two parsers in a chain
	// disagree about where one message ends and the next begins.
	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\ntransfer-encoding: chunked\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(Parse("GET / HTTP/1.1\r\nTransfer-Encoding: Chunked\r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("two content-length fields are refused even when they agree", "[http]") {
	// "Even when they agree" is the check somebody eventually loosens, so it is
	// pinned as written.
	Request request;
	CHECK(
		Parse("GET / HTTP/1.1\r\ncontent-length: 0\r\ncontent-length: 0\r\n\r\n", request) ==
		ParseResult::Malformed
	);
	CHECK(
		Parse("GET / HTTP/1.1\r\ncontent-length: 0\r\ncontent-length: 7\r\n\r\n", request) ==
		ParseResult::Malformed
	);
}

TEST_CASE("a folded header line is refused", "[http]") {
	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\nhost: a\r\n  continued\r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("a bare LF does not terminate a line", "[http]") {
	Request request;
	CHECK(Parse("GET / HTTP/1.1\nhost: a\n\n", request) == ParseResult::Incomplete);
}

TEST_CASE("a header name that is not a token is refused", "[http]") {
	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\nbad name: a\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(Parse("GET / HTTP/1.1\r\n: a\r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("a request that declares a body is refused", "[http]") {
	// Skipping a body means trusting a length field to find the next message.
	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\ncontent-length: 5\r\n\r\nhello", request) == ParseResult::Malformed);
	// Zero is fine: it declares no body.
	CHECK(Parse("GET / HTTP/1.1\r\ncontent-length: 0\r\n\r\n", request) == ParseResult::Ok);
}

TEST_CASE("a content-length that is not digits is refused", "[http]") {
	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\ncontent-length: +5\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(Parse("GET / HTTP/1.1\r\ncontent-length: 0x10\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(Parse("GET / HTTP/1.1\r\ncontent-length: \r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("an absolute-form target is refused", "[http]") {
	// That is what a request to a forward proxy looks like, and accepting one
	// would make the origin's target space depend on a host field.
	Request request;
	CHECK(Parse("GET http://elsewhere/a HTTP/1.1\r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("only HTTP/1.1 is spoken", "[http]") {
	Request request;
	CHECK(Parse("GET / HTTP/1.0\r\n\r\n", request) == ParseResult::Malformed);
	CHECK(Parse("GET / HTTP/2\r\n\r\n", request) == ParseResult::Malformed);
}

TEST_CASE("the bounds are enforced before anything is buffered", "[http]") {
	MessageLimits tight;
	tight.HeaderCount = 2;
	tight.RequestLineBytes = 32;

	Request request;
	CHECK(Parse("GET / HTTP/1.1\r\na: 1\r\nb: 2\r\nc: 3\r\n\r\n", request, tight) == ParseResult::TooLarge);

	const std::string longTarget = "GET /" + std::string(200, 'x') + " HTTP/1.1\r\n\r\n";
	CHECK(Parse(longTarget, request, tight) == ParseResult::TooLarge);
}

TEST_CASE("a peer that never sends a blank line is bounded by what it sent", "[http]") {
	MessageLimits tight;
	tight.RequestLineBytes = 16;
	tight.HeaderBytes = 16;

	Request request;
	const std::string flood = "GET / HTTP/1.1\r\n" + std::string(200, 'a');
	CHECK(Parse(flood, request, tight) == ParseResult::TooLarge);
}

// --- ranges ---------------------------------------------------------------

TEST_CASE("a byte range parses and resolves against an entity", "[http]") {
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: bytes=10-19\r\n\r\n", request) == ParseResult::Ok);
	REQUIRE(request.Range.has_value());

	const std::optional<ByteRange> resolved = request.Range->Resolve(100);
	REQUIRE(resolved.has_value());
	CHECK(resolved->First == 10);
	// HTTP ranges are inclusive at both ends, which is off by one from every
	// other range in this codebase.
	CHECK(resolved->Last == 19);
}

TEST_CASE("an open range runs to the end of the entity", "[http]") {
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: bytes=90-\r\n\r\n", request) == ParseResult::Ok);
	REQUIRE(request.Range.has_value());

	const std::optional<ByteRange> resolved = request.Range->Resolve(100);
	REQUIRE(resolved.has_value());
	CHECK(resolved->First == 90);
	CHECK(resolved->Last == 99);
}

TEST_CASE("a suffix range longer than the entity is the whole entity", "[http]") {
	// RFC 9110 says so, and a resuming client relies on it.
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: bytes=-500\r\n\r\n", request) == ParseResult::Ok);
	REQUIRE(request.Range.has_value());

	const std::optional<ByteRange> resolved = request.Range->Resolve(100);
	REQUIRE(resolved.has_value());
	CHECK(resolved->First == 0);
	CHECK(resolved->Last == 99);
}

TEST_CASE("a range past the end of the entity does not resolve", "[http]") {
	// Which is a 416 rather than an empty body.
	const ByteRange past{.First = 500, .Last = 600, .Suffix = false};
	CHECK_FALSE(past.Resolve(100).has_value());
	CHECK_FALSE(past.Resolve(0).has_value());
}

TEST_CASE("an unparseable range is ignored rather than refused", "[http]") {
	// RFC 9110 requires the whole entity in that case. Refusing would break a
	// client whose proxy rewrote the header.
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: furlongs=1-2\r\n\r\n", request) == ParseResult::Ok);
	CHECK_FALSE(request.Range.has_value());

	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: bytes=9-1\r\n\r\n", request) == ParseResult::Ok);
	CHECK_FALSE(request.Range.has_value());
}

TEST_CASE("a multipart range is refused, being a second body framing", "[http]") {
	Request request;
	REQUIRE(Parse("GET / HTTP/1.1\r\nrange: bytes=0-9,20-29\r\n\r\n", request) == ParseResult::Ok);
	CHECK_FALSE(request.Range.has_value());
}

// --- responses ------------------------------------------------------------

TEST_CASE("a response parses with its body", "[http]") {
	const std::vector<std::byte> wire = Raw("HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhello");
	Response response;
	size_t consumed = 0;
	REQUIRE(ParseResponse(wire, {}, false, response, consumed) == ParseResult::Ok);
	CHECK(response.Code == Status::Ok);
	CHECK(Text(response.Body) == "hello");
	CHECK(consumed == wire.size());
}

TEST_CASE("a response is incomplete until its whole body has arrived", "[http]") {
	const std::vector<std::byte> wire = Raw("HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\nhel");
	Response response;
	size_t consumed = 0;
	CHECK(ParseResponse(wire, {}, false, response, consumed) == ParseResult::Incomplete);
}

TEST_CASE("a response with no content-length is refused", "[http]") {
	// Reading to end-of-connection would let an origin present a truncated
	// group as a complete one, and the client would then hash short bytes and
	// report content corruption for what was a dropped socket.
	const std::vector<std::byte> wire = Raw("HTTP/1.1 200 OK\r\n\r\nhello");
	Response response;
	size_t consumed = 0;
	CHECK(ParseResponse(wire, {}, false, response, consumed) == ParseResult::Malformed);
}

TEST_CASE("a response body over the bound is refused before it is buffered", "[http]") {
	MessageLimits tight;
	tight.BodyBytes = 8;

	const std::vector<std::byte> wire = Raw("HTTP/1.1 200 OK\r\ncontent-length: 4294967296\r\n\r\n");
	Response response;
	size_t consumed = 0;
	CHECK(ParseResponse(wire, tight, false, response, consumed) == ParseResult::TooLarge);
}

TEST_CASE("an unrecognised status parses rather than failing the fetch", "[http]") {
	// A client must survive an origin or a proxy answering something this
	// subset never emits.
	const std::vector<std::byte> wire = Raw("HTTP/1.1 418 Teapot\r\ncontent-length: 0\r\n\r\n");
	Response response;
	size_t consumed = 0;
	REQUIRE(ParseResponse(wire, {}, false, response, consumed) == ParseResult::Ok);
	CHECK(response.Code == Status::Unknown);
}

// --- writing --------------------------------------------------------------

TEST_CASE("what is written parses back", "[http]") {
	Request request;
	request.Verb = Method::Get;
	request.Target = "/bundle/abcd";
	request.Headers.push_back(Header{.Name = "x-atomic-grant", .Value = "beef"});
	request.Range = ByteRange{.First = 4, .Last = 9, .Suffix = false};

	std::vector<std::byte> wire;
	WriteRequest(request, "origin:9080", wire);

	Request read;
	size_t consumed = 0;
	REQUIRE(ParseRequest(wire, {}, read, consumed) == ParseResult::Ok);
	CHECK(read.Target == "/bundle/abcd");
	REQUIRE(read.Find("host").has_value());
	CHECK(*read.Find("host") == "origin:9080");
	REQUIRE(read.Find("x-atomic-grant").has_value());
	CHECK(*read.Find("x-atomic-grant") == "beef");
	REQUIRE(read.Range.has_value());
	CHECK(read.Range->First == 4);
	CHECK(read.Range->Last == 9);
}

TEST_CASE("content-length is written from the body and not from the header list", "[http]") {
	// Two sources for one number is one number that will eventually be wrong,
	// and the symptom is a connection that desynchronises one message later.
	Response response;
	response.Code = Status::Ok;
	response.Body = Raw("hello");
	response.Set("content-length", "9999");

	std::vector<std::byte> wire;
	WriteResponse(response, false, wire);

	Response read;
	size_t consumed = 0;
	REQUIRE(ParseResponse(wire, {}, false, read, consumed) == ParseResult::Ok);
	CHECK(Text(read.Body) == "hello");
	REQUIRE(read.Find("content-length").has_value());
	CHECK(*read.Find("content-length") == "5");
}

TEST_CASE("a head response carries the length and no body", "[http]") {
	Response response;
	response.Code = Status::Ok;
	response.Body = Raw("0123456789");

	std::vector<std::byte> wire;
	WriteResponse(response, true, wire);

	CHECK(Text(wire).find("content-length: 10") != std::string::npos);
	CHECK(Text(wire).ends_with("\r\n\r\n"));
}

TEST_CASE("setting a header twice replaces it", "[http]") {
	// Two fields of one name is a message two parsers can read differently.
	Response response;
	response.Set("content-type", "application/octet-stream");
	response.Set("Content-Type", "text/plain");
	CHECK(response.Headers.size() == 1);
	REQUIRE(response.Find("content-type").has_value());
	CHECK(*response.Find("content-type") == "text/plain");
}
