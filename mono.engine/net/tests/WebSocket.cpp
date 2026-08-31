#include <engine/net/websocket/Server.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.net.websocket")

namespace {
	std::vector<std::byte> MaskedText(std::string_view text) {
		static constexpr std::array<uint8_t, 4> MASK = {0x12, 0x34, 0x56, 0x78};
		std::vector<std::byte> frame(2 + MASK.size() + text.size());
		frame[0] = std::byte{0x81};
		frame[1] = static_cast<std::byte>(0x80u | text.size());
		for (size_t at = 0; at < MASK.size(); at++) {
			frame[2 + at] = static_cast<std::byte>(MASK[at]);
		}
		for (size_t at = 0; at < text.size(); at++) {
			frame[2 + MASK.size() + at] = static_cast<std::byte>(text[at] ^ MASK[at % MASK.size()]);
		}
		return frame;
	}
}

TEST_CASE("websocket performs an async handshake and delivers masked text", "[net][websocket]") {
	std::mutex guard;
	std::condition_variable changed;
	bool opened = false;
	bool binaryMessage = false;
	std::string received;

	engine::net::websocket::Callbacks callbacks;
	callbacks.Open = [&](engine::net::websocket::ConnectionId) {
		std::lock_guard lock(guard);
		opened = true;
		changed.notify_all();
	};
	callbacks.Message =
		[&](engine::net::websocket::ConnectionId, std::span<const std::byte> payload, bool binary) {
			std::lock_guard lock(guard);
			binaryMessage = binary;
			received.assign(reinterpret_cast<const char *>(payload.data()), payload.size());
			changed.notify_all();
		};

	const auto server = engine::net::websocket::Listen(0, std::move(callbacks));
	REQUIRE(server != nullptr);

	asio::io_context context;
	asio::ip::tcp::socket client(context);
	std::error_code failure;
	client.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server->Local().Port), failure);
	REQUIRE_FALSE(failure);

	const std::string request = "GET /socket HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
								"Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
								"Sec-WebSocket-Version: 13\r\n\r\n";
	asio::write(client, asio::buffer(request), failure);
	REQUIRE_FALSE(failure);

	asio::streambuf response;
	asio::read_until(client, response, "\r\n\r\n", failure);
	REQUIRE_FALSE(failure);
	std::string responseText((std::istreambuf_iterator<char>(&response)), std::istreambuf_iterator<char>());
	CHECK(responseText.find("101 Switching Protocols") != std::string::npos);

	const std::vector<std::byte> frame = MaskedText("hello");
	asio::write(client, asio::buffer(frame), failure);
	REQUIRE_FALSE(failure);

	{
		std::unique_lock lock(guard);
		REQUIRE(changed.wait_for(lock, std::chrono::seconds(1), [&] {
			return opened && received == "hello";
		}));
		CHECK_FALSE(binaryMessage);
	}
}
