#include <engine/net/websocket/Server.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.net.bench.websocket")

namespace {
	constexpr size_t CONNECTIONS = 10'000;
	constexpr size_t BATCH_SIZE = 256;

	const std::string &Request() {
		static const std::string request =
			"GET /benchmark HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
			"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
			"Sec-WebSocket-Version: 13\r\n\r\n";
		return request;
	}
}

BENCH_PER_ITEM("async accept and handshake, 10k peers", CONNECTIONS) {
	std::atomic<size_t> opened = 0;
	engine::net::websocket::Callbacks callbacks;
	callbacks.Open = [&](engine::net::websocket::ConnectionId) { opened.fetch_add(1); };
	const engine::net::websocket::ServerSettings settings{
		.MaximumConnections = CONNECTIONS,
		.MaximumFrameBytes = 1024u * 1024u,
		.WorkerThreads = 4,
	};
	const auto server = engine::net::websocket::Listen(0, std::move(callbacks), settings);
	if (server == nullptr) {
		throw std::runtime_error("WebSocket benchmark could not bind a loopback port");
	}

	asio::io_context context;
	for (size_t first = 0; first < CONNECTIONS; first += BATCH_SIZE) {
		std::vector<std::unique_ptr<asio::ip::tcp::socket>> clients;
		clients.reserve(std::min(BATCH_SIZE, CONNECTIONS - first));
		for (size_t index = first; index < std::min(CONNECTIONS, first + BATCH_SIZE); index++) {
		auto client = std::make_unique<asio::ip::tcp::socket>(context);
		std::error_code failure;
		client->connect(
			asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server->Local().Port), failure
		);
		if (failure) {
			throw std::runtime_error("WebSocket benchmark client could not connect");
		}
		asio::write(*client, asio::buffer(Request()), failure);
		if (failure) {
			throw std::runtime_error("WebSocket benchmark client could not send handshake");
		}
		asio::streambuf response;
		asio::read_until(*client, response, "\r\n\r\n", failure);
		if (failure) {
			throw std::runtime_error("WebSocket benchmark handshake did not complete");
		}
		clients.push_back(std::move(client));
		}
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (opened.load() != CONNECTIONS && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	if (opened.load() != CONNECTIONS) {
		throw std::runtime_error("WebSocket benchmark did not open all peers");
	}
}
