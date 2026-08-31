#include <engine/net/websocket/Server.hpp>

#include <algorithm>
#include <array>
#include <asio/bind_executor.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/strand.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <cctype>
#include <cryptopp/base64.h>
#include <cryptopp/filters.h>
#include <cryptopp/sha.h>
#include <deque>
#include <istream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::net::websocket {
	namespace {
		using asio::ip::tcp;

		std::string Lower(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		std::string Header(std::string_view request, std::string_view wanted) {
			const std::string sought = Lower(std::string(wanted));
			size_t line = 0;
			while (line < request.size()) {
				const size_t end = request.find("\r\n", line);
				const size_t limit = end == std::string_view::npos ? request.size() : end;
				const size_t colon = request.find(':', line);
				if (colon != std::string_view::npos && colon < limit &&
					Lower(std::string(request.substr(line, colon - line))) == sought) {
					size_t value = colon + 1;
					while (value < limit && (request[value] == ' ' || request[value] == '\t')) {
						value++;
					}
					return std::string(request.substr(value, limit - value));
				}
				if (end == std::string_view::npos) {
					break;
				}
				line = end + 2;
			}
			return {};
		}

		bool HasToken(std::string value, std::string_view wanted) {
			value = Lower(std::move(value));
			const std::string sought = Lower(std::string(wanted));
			size_t start = 0;
			while (start < value.size()) {
				const size_t end = value.find(',', start);
				size_t limit = end == std::string::npos ? value.size() : end;
				size_t first = start;
				while (first < limit && value[first] == ' ') {
					first++;
				}
				while (first < limit && (value[limit - 1] == ' ' || value[limit - 1] == '\t')) {
					--limit;
				}
				if (value.substr(first, limit - first) == sought) {
					return true;
				}
				if (end == std::string::npos) {
					break;
				}
				start = end + 1;
			}
			return false;
		}

		std::string AcceptKey(std::string_view key) {
			static constexpr std::string_view MAGIC = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
			const std::string source = std::string(key) + std::string(MAGIC);
			std::array<CryptoPP::byte, CryptoPP::SHA1::DIGESTSIZE> digest{};
			CryptoPP::SHA1 hash;
			hash.CalculateDigest(
				digest.data(), reinterpret_cast<const CryptoPP::byte *>(source.data()), source.size()
			);
			std::string encoded;
			CryptoPP::StringSource(
				digest.data(),
				digest.size(),
				true,
				new CryptoPP::Base64Encoder(new CryptoPP::StringSink(encoded), false)
			);
			return encoded;
		}

		std::vector<std::byte> Frame(std::span<const std::byte> payload, bool binary) {
			const size_t size = payload.size();
			const size_t extra = size < 126 ? 0 : size <= 0xFFFF ? 2 : 8;
			std::vector<std::byte> frame(2 + extra + size);
			frame[0] = static_cast<std::byte>(0x80u | (binary ? 0x02u : 0x01u));
			if (size < 126) {
				frame[1] = static_cast<std::byte>(size);
			} else if (size <= 0xFFFF) {
				frame[1] = static_cast<std::byte>(126);
				frame[2] = static_cast<std::byte>((size >> 8) & 0xffu);
				frame[3] = static_cast<std::byte>(size & 0xffu);
			} else {
				frame[1] = static_cast<std::byte>(127);
				for (size_t at = 0; at < 8; at++) {
					frame[2 + at] = static_cast<std::byte>((size >> (56 - at * 8)) & 0xffu);
				}
			}
			std::copy(payload.begin(), payload.end(), frame.begin() + 2 + extra);
			return frame;
		}

		class TcpServer;

		class Session final : public std::enable_shared_from_this<Session> {
		  public:
			Session(TcpServer &owner, tcp::socket socket, ConnectionId id, const ServerSettings &settings)
				: Owner(owner), Socket(std::move(socket)), Id(id), Limits(settings) {}

			void Start();
			void Send(std::vector<std::byte> frame);
			void Stop();

		  private:
			void Handshake();
			void Read();
			void ParseFrames();
			void Write();
			void Finish();

			TcpServer &Owner;
			tcp::socket Socket;
			ConnectionId Id;
			ServerSettings Limits;
			asio::streambuf Request;
			std::array<std::byte, 16u * 1024u> ReadBuffer{};
			std::vector<std::byte> Inbox;
			std::deque<std::vector<std::byte>> Outbox;
			bool Handshaken = false;
			bool Writing = false;
			bool Finished = false;
		};

		class TcpServer final : public Server {
		  public:
			TcpServer(uint16_t port, Callbacks callbacks, ServerSettings settings)
				: Limits(settings), Callbacks_(std::move(callbacks)), Strand(Context.get_executor()),
				  Acceptor(Context) {
				std::error_code failure;
				Acceptor.open(tcp::v4(), failure);
				if (!failure) {
					Acceptor.set_option(asio::socket_base::reuse_address(true), failure);
				}
				if (!failure) {
					Acceptor.bind(tcp::endpoint(tcp::v4(), port), failure);
				}
				if (!failure) {
					Acceptor.listen(asio::socket_base::max_listen_connections, failure);
				}
				if (failure) {
					return;
				}
				const tcp::endpoint local = Acceptor.local_endpoint(failure);
				if (!failure) {
					Address = Endpoint::FromIPv4(local.address().to_v4().to_bytes(), local.port());
					Running.store(true, std::memory_order_release);
					Accept();
				}
			}

			~TcpServer() override {
				Stop();
			}

			Endpoint Local() const override {
				return Address;
			}
			bool Open() const override {
				return Running.load(std::memory_order_acquire);
			}
			size_t Connections() const override {
				return ConnectionCount.load(std::memory_order_acquire);
			}

			bool Send(ConnectionId id, std::span<const std::byte> payload, bool binary) override {
				if (!Open() || payload.size() > Limits.MaximumFrameBytes) {
					return false;
				}
				std::vector<std::byte> frame = Frame(payload, binary);
				asio::post(Strand, [this, id, frame = std::move(frame)]() mutable {
					const auto found = Sessions.find(id);
					if (found != Sessions.end()) {
						found->second->Send(std::move(frame));
					}
				});
				return true;
			}

			void Close(ConnectionId id) override {
				asio::post(Strand, [this, id] {
					const auto found = Sessions.find(id);
					if (found != Sessions.end()) {
						found->second->Stop();
					}
				});
			}

			void Remove(ConnectionId id) {
				const auto found = Sessions.find(id);
				if (found == Sessions.end()) {
					return;
				}
				Sessions.erase(found);
				ConnectionCount.fetch_sub(1, std::memory_order_relaxed);
				if (Callbacks_.Close) {
					Callbacks_.Close(id);
				}
			}

			Callbacks &Handlers() {
				return Callbacks_;
			}

			asio::strand<asio::io_context::executor_type> &Dispatcher() {
				return Strand;
			}

			void StartWorkers() {
				const size_t workers = std::max<size_t>(1, Limits.WorkerThreads);
				for (size_t at = 0; at < workers; at++) {
					Workers.emplace_back([this] { Context.run(); });
				}
			}

		  private:
			void Accept() {
				Acceptor.async_accept(
					asio::bind_executor(Strand, [this](std::error_code failure, tcp::socket socket) {
						if (!failure && Running.load(std::memory_order_relaxed)) {
							if (Sessions.size() < Limits.MaximumConnections) {
								const ConnectionId id = NextId++;
								auto session =
									std::make_shared<Session>(*this, std::move(socket), id, Limits);
								Sessions.emplace(id, session);
								ConnectionCount.fetch_add(1, std::memory_order_relaxed);
								session->Start();
							} else {
								std::error_code ignored;
								socket.close(ignored);
							}
						}
						if (Running.load(std::memory_order_relaxed)) {
							Accept();
						}
					})
				);
			}

			void Stop() {
				if (!Running.exchange(false, std::memory_order_acq_rel)) {
					return;
				}
				asio::post(Strand, [this] {
					std::error_code ignored;
					Acceptor.close(ignored);
					std::vector<std::shared_ptr<Session>> sessions;
					sessions.reserve(Sessions.size());
					for (const auto &[id, session] : Sessions) {
						(void)id;
						sessions.push_back(session);
					}
					for (const auto &session : sessions) {
						session->Stop();
					}
					Sessions.clear();
					Context.stop();
				});
				for (std::thread &worker : Workers) {
					if (worker.joinable()) {
						worker.join();
					}
				}
			}

			ServerSettings Limits;
			Callbacks Callbacks_;
			asio::io_context Context;
			asio::strand<asio::io_context::executor_type> Strand;
			tcp::acceptor Acceptor;
			Endpoint Address;
			std::atomic<bool> Running{false};
			std::unordered_map<ConnectionId, std::shared_ptr<Session>> Sessions;
			std::atomic<size_t> ConnectionCount{0};
			ConnectionId NextId = 1;
			std::vector<std::thread> Workers;
		};

		void Session::Start() {
			Handshake();
		}

		void Session::Handshake() {
			auto self = shared_from_this();
			asio::async_read_until(
				Socket,
				Request,
				"\r\n\r\n",
				asio::bind_executor(self->Owner.Dispatcher(), [self](std::error_code failure, size_t bytes) {
					if (failure || bytes > 16u * 1024u) {
						self->Finish();
						return;
					}
					std::string request(bytes, '\0');
					std::istream input(&self->Request);
					input.read(request.data(), static_cast<std::streamsize>(bytes));
					const size_t buffered = self->Request.size();
					if (buffered != 0) {
						std::vector<std::byte> remainder(buffered);
						input.read(
							reinterpret_cast<char *>(remainder.data()), static_cast<std::streamsize>(buffered)
						);
						self->Inbox.insert(self->Inbox.end(), remainder.begin(), remainder.end());
					}
					const size_t firstSpace = request.find(' ');
					const size_t secondSpace = request.find(' ', firstSpace + 1);
					const bool get = request.starts_with("GET ") && firstSpace != std::string::npos &&
									 secondSpace != std::string::npos;
					const std::string upgrade = Header(request, "upgrade");
					const std::string connection = Header(request, "connection");
					const std::string key = Header(request, "sec-websocket-key");
					if (!get || Lower(upgrade) != "websocket" || !HasToken(connection, "upgrade") ||
						key.empty()) {
						self->Finish();
						return;
					}

					const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
												 "Upgrade: websocket\r\n"
												 "Connection: Upgrade\r\n";
					const std::string complete =
						response + "Sec-WebSocket-Accept: " + AcceptKey(key) + "\r\n\r\n";
					self->Handshaken = true;
					self->Outbox.emplace_back(
						reinterpret_cast<const std::byte *>(complete.data()),
						reinterpret_cast<const std::byte *>(complete.data() + complete.size())
					);
					self->Write();
					if (self->Owner.Handlers().Open) {
						self->Owner.Handlers().Open(self->Id);
					}
					self->Read();
				})
			);
		}

		void Session::Read() {
			auto self = shared_from_this();
			Socket.async_read_some(
				asio::buffer(ReadBuffer),
				asio::bind_executor(self->Owner.Dispatcher(), [self](std::error_code failure, size_t bytes) {
					if (failure || bytes == 0) {
						self->Finish();
						return;
					}
					self->Inbox.insert(
						self->Inbox.end(), self->ReadBuffer.begin(), self->ReadBuffer.begin() + bytes
					);
					self->ParseFrames();
					if (!self->Finished) {
						self->Read();
					}
				})
			);
		}

		void Session::ParseFrames() {
			while (Inbox.size() >= 2 && !Finished) {
				const uint8_t first = static_cast<uint8_t>(Inbox[0]);
				const uint8_t second = static_cast<uint8_t>(Inbox[1]);
				if ((first & 0x80u) == 0 || (first & 0x70u) != 0 || (second & 0x80u) == 0) {
					Finish();
					return;
				}
				const uint8_t opcode = first & 0x0Fu;
				size_t length = second & 0x7Fu;
				size_t header = 2;
				if (length == 126) {
					if (Inbox.size() < 4) return;
					length = (static_cast<size_t>(static_cast<uint8_t>(Inbox[2])) << 8) |
							 static_cast<size_t>(static_cast<uint8_t>(Inbox[3]));
					header = 4;
				} else if (length == 127) {
					if (Inbox.size() < 10 || (static_cast<uint8_t>(Inbox[2]) & 0x80u) != 0) {
						Finish();
						return;
					}
					length = 0;
					for (size_t at = 0; at < 8; at++) {
						length = (length << 8) | static_cast<size_t>(static_cast<uint8_t>(Inbox[2 + at]));
					}
					header = 10;
				}
				if (length > Limits.MaximumFrameBytes || Inbox.size() < header + 4 ||
					Inbox.size() < header + 4 + length) {
					if (length > Limits.MaximumFrameBytes) Finish();
					return;
				}

				std::array<uint8_t, 4> mask{};
				for (size_t at = 0; at < mask.size(); at++) {
					mask[at] = static_cast<uint8_t>(Inbox[header + at]);
				}
				std::vector<std::byte> payload(length);
				for (size_t at = 0; at < length; at++) {
					payload[at] =
						static_cast<std::byte>(static_cast<uint8_t>(Inbox[header + 4 + at]) ^ mask[at % 4]);
				}
				Inbox.erase(Inbox.begin(), Inbox.begin() + header + 4 + length);

				switch (opcode) {
				case 0x1:
				case 0x2:
					if ((first & 0x80u) == 0) {
						Finish();
						return;
					}
					if (Owner.Handlers().Message) {
						Owner.Handlers().Message(Id, payload, opcode == 0x2);
					}
					break;
				case 0x8:
					Finish();
					return;
				case 0x9:
					if (payload.size() <= 125) {
						Send(Frame(payload, false));
					}
					break;
				case 0xA:
					break;
				default:
					Finish();
					return;
				}
			}
		}

		void Session::Send(std::vector<std::byte> frame) {
			if (Finished) return;
			Outbox.push_back(std::move(frame));
			Write();
		}

		void Session::Write() {
			if (Writing || Outbox.empty() || Finished) return;
			Writing = true;
			auto self = shared_from_this();
			asio::async_write(
				Socket,
				asio::buffer(Outbox.front()),
				asio::bind_executor(self->Owner.Dispatcher(), [self](std::error_code failure, size_t) {
					if (failure) {
						self->Finish();
						return;
					}
					self->Outbox.pop_front();
					self->Writing = false;
					self->Write();
				})
			);
		}

		void Session::Stop() {
			Finish();
		}

		void Session::Finish() {
			if (Finished) return;
			Finished = true;
			std::error_code ignored;
			Socket.close(ignored);
			Owner.Remove(Id);
		}
	}

	std::unique_ptr<Server> Listen(uint16_t port, Callbacks callbacks, const ServerSettings &settings) {
		auto server = std::make_unique<TcpServer>(port, std::move(callbacks), settings);
		if (!server->Local().IsValid()) {
			return nullptr;
		}
		server->StartWorkers();
		return server;
	}
}
