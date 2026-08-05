#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>
#include <engine/net/http/Message.hpp>
#include <engine/net/http/Server.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.net.http.transfer")
TEST_DEPENDS("engine.net.http.message")

using engine::net::Endpoint;
using engine::net::http::Client;
using engine::net::http::ClientSettings;
using engine::net::http::FetchId;
using engine::net::http::FetchState;
using engine::net::http::Listen;
using engine::net::http::MakeClient;
using engine::net::http::Method;
using engine::net::http::Request;
using engine::net::http::Response;
using engine::net::http::Server;
using engine::net::http::ServeReport;
using engine::net::http::ServerSettings;
using engine::net::http::Status;

namespace {
	// How many polls a case will spend before giving up.
	//
	// A bound rather than a wait: both halves are non-blocking and driven by
	// the same thread, so a transfer that has not finished in this many turns
	// of the loop is not going to. Nothing here sleeps, which is why the suite
	// runs in milliseconds rather than in timeouts.
	constexpr int MAXIMUM_POLLS = 20000;

	std::vector<std::byte> Raw(std::string_view text) {
		std::vector<std::byte> bytes(text.size());
		for (size_t index = 0; index < text.size(); ++index) {
			bytes[index] = static_cast<std::byte>(text[index]);
		}
		return bytes;
	}

	std::string Text(const std::vector<std::byte> &bytes) {
		std::string out;
		out.reserve(bytes.size());
		for (const std::byte value : bytes) {
			out.push_back(static_cast<char>(value));
		}
		return out;
	}

	// Both halves in one process, on one thread, over a real socket.
	//
	// A real loopback socket rather than an in-memory pair on purpose: the
	// message layer is already tested without one, and what is left to check
	// here is precisely the part a pair cannot express — partial writes, a
	// kernel send buffer smaller than the body, and a connection that has to
	// survive being read across several polls.
	struct Wire {
		std::unique_ptr<Server> Listener;
		std::unique_ptr<Client> Fetcher;
		Endpoint Address;

		static std::optional<Wire>
		Open(const ServerSettings &server = {}, const ClientSettings &client = {}) {
			Wire wire;
			wire.Listener = Listen(0, server);
			if (!wire.Listener) {
				return std::nullopt;
			}
			wire.Fetcher = MakeClient(client);
			// The server binds on every interface; the client has to be told
			// somewhere to reach it, and loopback is the one address that is
			// always there.
			wire.Address = Endpoint::LoopbackIPv4(wire.Listener->Local().Port);
			return wire;
		}

		// Drives both halves until the fetch settles.
		FetchState Settle(FetchId id, const Server::Handler &handler) {
			for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
				Listener->Pump(handler);
				Fetcher->Pump();
				const FetchState state = Fetcher->StateOf(id);
				if (state != FetchState::Pending) {
					return state;
				}
			}
			return FetchState::Pending;
		}
	};

	Request Get(std::string target) {
		Request request;
		request.Verb = Method::Get;
		request.Target = std::move(target);
		return request;
	}

	Server::Handler Answering(std::string body, Status code = Status::Ok) {
		return [body = std::move(body), code](const Request &) {
			Response response;
			response.Code = code;
			response.Body = Raw(body);
			return response;
		};
	}
}

TEST_CASE("a request crosses a real socket and the response comes back", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/health"), "origin");
	REQUIRE(id.IsValid());

	REQUIRE(wire->Settle(id, Answering("ready")) == FetchState::Ready);

	const std::optional<Response> answer = wire->Fetcher->Take(id);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Ok);
	CHECK(Text(answer->Body) == "ready");
}

TEST_CASE("the handler sees the target and the headers that were sent", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	Request request = Get("/bundle/1a2b3c");
	request.Headers.push_back({.Name = "x-atomic-grant", .Value = "deadbeef"});

	std::string seenTarget;
	std::string seenGrant;
	const Server::Handler handler = [&](const Request &incoming) {
		seenTarget = incoming.Target;
		if (const std::optional<std::string_view> grant = incoming.Find("x-atomic-grant")) {
			seenGrant = std::string(*grant);
		}
		Response response;
		response.Code = Status::Ok;
		return response;
	};

	const FetchId id = wire->Fetcher->Submit(wire->Address, request, "origin");
	REQUIRE(id.IsValid());
	REQUIRE(wire->Settle(id, handler) == FetchState::Ready);

	CHECK(seenTarget == "/bundle/1a2b3c");
	CHECK(seenGrant == "deadbeef");
}

TEST_CASE("a body larger than a kernel send buffer arrives whole", "[http]") {
	// This is the case a memory pair cannot express and the reason the suite
	// opens a socket at all: the write completes over several polls, and a
	// server that restarted its write rather than resuming it would corrupt
	// exactly here. Four megabytes is comfortably past any default send buffer.
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	std::string large;
	large.reserve(4u * 1024u * 1024u);
	for (size_t index = 0; large.size() < 4u * 1024u * 1024u; ++index) {
		large.push_back(static_cast<char>('a' + (index % 26)));
	}

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/large"), "origin");
	REQUIRE(id.IsValid());
	REQUIRE(wire->Settle(id, Answering(large)) == FetchState::Ready);

	const std::optional<Response> answer = wire->Fetcher->Take(id);
	REQUIRE(answer.has_value());
	REQUIRE(answer->Body.size() == large.size());
	CHECK(Text(answer->Body) == large);
}

TEST_CASE("several fetches run concurrently over their own connections", "[http]") {
	// CDN.md §5: groups stream in parallel so a slow one does not hold up the
	// others. Sharing a socket would put them back in one queue.
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const Server::Handler handler = [](const Request &incoming) {
		Response response;
		response.Code = Status::Ok;
		response.Body = Raw(incoming.Target);
		return response;
	};

	std::vector<FetchId> fetches;
	for (int index = 0; index < 4; ++index) {
		const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/group/" + std::to_string(index)), "o");
		REQUIRE(id.IsValid());
		fetches.push_back(id);
	}
	CHECK(wire->Fetcher->Outstanding() == 4);

	for (size_t index = 0; index < fetches.size(); ++index) {
		REQUIRE(wire->Settle(fetches[index], handler) == FetchState::Ready);
		const std::optional<Response> answer = wire->Fetcher->Take(fetches[index]);
		REQUIRE(answer.has_value());
		CHECK(Text(answer->Body) == "/group/" + std::to_string(index));
	}
	CHECK(wire->Fetcher->Outstanding() == 0);
}

TEST_CASE("a connection is reused for a second request", "[http]") {
	// Keep-alive is HTTP/1.1's default, and a client that opened a socket per
	// fetch to the same origin would pay a handshake per group.
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const FetchId first = wire->Fetcher->Submit(wire->Address, Get("/one"), "origin");
	REQUIRE(wire->Settle(first, Answering("1")) == FetchState::Ready);
	REQUIRE(wire->Fetcher->Take(first).has_value());

	const FetchId second = wire->Fetcher->Submit(wire->Address, Get("/two"), "origin");
	REQUIRE(wire->Settle(second, Answering("2")) == FetchState::Ready);
	REQUIRE(wire->Fetcher->Take(second).has_value());
}

TEST_CASE("a head request answers the length and no body", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	Request request = Get("/large");
	request.Verb = Method::Head;

	const FetchId id = wire->Fetcher->Submit(wire->Address, request, "origin");
	REQUIRE(wire->Settle(id, Answering("0123456789")) == FetchState::Ready);

	const std::optional<Response> answer = wire->Fetcher->Take(id);
	REQUIRE(answer.has_value());
	CHECK(answer->Body.empty());
	REQUIRE(answer->Find("content-length").has_value());
	// The length still describes what a GET would have returned.
	CHECK(*answer->Find("content-length") == "10");
}

TEST_CASE("an unimplemented verb is answered rather than dropped", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	Request request = Get("/upload");
	request.Verb = Method::Unknown;
	// Method::Unknown writes as "UNKNOWN", which is a well-formed token and
	// therefore a well-formed request that this subset does not implement.

	const FetchId id = wire->Fetcher->Submit(wire->Address, request, "origin");
	REQUIRE(wire->Settle(id, Answering("never")) == FetchState::Ready);

	const std::optional<Response> answer = wire->Fetcher->Take(id);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::NotImplemented);
}

TEST_CASE("a refusal carries its status and no reason", "[http]") {
	// cdn::Gate's rule reaching the wire: a reason returned to a client is an
	// oracle.
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/bundle/nope"), "origin");
	REQUIRE(wire->Settle(id, Answering("", Status::Forbidden)) == FetchState::Ready);

	const std::optional<Response> answer = wire->Fetcher->Take(id);
	REQUIRE(answer.has_value());
	CHECK(answer->Code == Status::Forbidden);
	CHECK(answer->Body.empty());
}

TEST_CASE("a connection to nothing fails rather than hanging", "[http]") {
	// The non-blocking connect's failure path: refused must be detected on a
	// later poll rather than waited out by an idle bound.
	ClientSettings settings;
	settings.IdlePolls = MAXIMUM_POLLS;
	std::unique_ptr<Client> fetcher = MakeClient(settings);

	// A port nothing is listening on. Port 1 is privileged and unused on every
	// platform this builds for.
	const FetchId id = fetcher->Submit(Endpoint::LoopbackIPv4(1), Get("/health"), "origin");
	REQUIRE(id.IsValid());

	FetchState state = FetchState::Pending;
	for (int poll = 0; poll < MAXIMUM_POLLS && state == FetchState::Pending; ++poll) {
		fetcher->Pump();
		state = fetcher->StateOf(id);
	}
	CHECK(state == FetchState::Failed);
}

TEST_CASE("submitting past the outstanding bound is refused rather than queued", "[http]") {
	ClientSettings settings;
	settings.MaximumOutstanding = 2;
	std::unique_ptr<Client> fetcher = MakeClient(settings);

	CHECK(fetcher->Submit(Endpoint::LoopbackIPv4(1), Get("/a"), "o").IsValid());
	CHECK(fetcher->Submit(Endpoint::LoopbackIPv4(1), Get("/b"), "o").IsValid());
	CHECK_FALSE(fetcher->Submit(Endpoint::LoopbackIPv4(1), Get("/c"), "o").IsValid());
}

TEST_CASE("a cancelled fetch releases its slot", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/group"), "origin");
	REQUIRE(id.IsValid());
	CHECK(wire->Fetcher->Cancel(id));
	CHECK(wire->Fetcher->Outstanding() == 0);
	CHECK(wire->Fetcher->StateOf(id) == FetchState::Unknown);
	// Cancelling twice is false rather than an abort: a caller racing its own
	// cancel against a completion is ordinary.
	CHECK_FALSE(wire->Fetcher->Cancel(id));
}

TEST_CASE("a response is taken exactly once", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/one"), "origin");
	REQUIRE(wire->Settle(id, Answering("body")) == FetchState::Ready);

	CHECK(wire->Fetcher->Take(id).has_value());
	CHECK_FALSE(wire->Fetcher->Take(id).has_value());
	CHECK(wire->Fetcher->StateOf(id) == FetchState::Unknown);
}

TEST_CASE("the connection ceiling refuses rather than filling the backlog", "[http]") {
	ServerSettings bounded;
	bounded.MaximumConnections = 1;

	std::optional<Wire> wire = Wire::Open(bounded);
	REQUIRE(wire.has_value());

	const FetchId first = wire->Fetcher->Submit(wire->Address, Get("/a"), "o");
	const FetchId second = wire->Fetcher->Submit(wire->Address, Get("/b"), "o");
	REQUIRE(first.IsValid());
	REQUIRE(second.IsValid());

	// One of the two is served and the other is closed on arrival. Which is
	// which depends on the accept order, so the case asserts what is
	// guaranteed: neither is left pending for ever.
	const Server::Handler handler = Answering("served");
	for (int poll = 0; poll < MAXIMUM_POLLS; ++poll) {
		wire->Listener->Pump(handler);
		wire->Fetcher->Pump();
		const bool settled = wire->Fetcher->StateOf(first) != FetchState::Pending &&
							 wire->Fetcher->StateOf(second) != FetchState::Pending;
		if (settled) {
			break;
		}
	}
	CHECK(wire->Fetcher->StateOf(first) != FetchState::Pending);
	CHECK(wire->Fetcher->StateOf(second) != FetchState::Pending);
	CHECK(wire->Listener->Connections() <= 1);
}

TEST_CASE("received bytes are counted at the wire", "[http]") {
	// The measurement that answers "did this actually travel compressed", so
	// it has to be the socket's count rather than the body's length.
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	CHECK(wire->Fetcher->ReceivedBytes() == 0);

	const FetchId id = wire->Fetcher->Submit(wire->Address, Get("/one"), "origin");
	REQUIRE(wire->Settle(id, Answering(std::string(1024, 'x'))) == FetchState::Ready);
	REQUIRE(wire->Fetcher->Take(id).has_value());

	CHECK(wire->Fetcher->ReceivedBytes() >= 1024);
}

TEST_CASE("a closed server stops answering", "[http]") {
	std::optional<Wire> wire = Wire::Open();
	REQUIRE(wire.has_value());

	CHECK(wire->Listener->Open());
	wire->Listener->Close();
	CHECK_FALSE(wire->Listener->Open());
	CHECK_FALSE(wire->Listener->Local().IsValid());

	// Pumping a closed server is a no-op rather than an abort: shutdown races
	// a pump on any loop that has both.
	const ServeReport idle = wire->Listener->Pump(Answering("never"));
	CHECK(idle.Accepted == 0);
	CHECK(idle.Served == 0);
}
