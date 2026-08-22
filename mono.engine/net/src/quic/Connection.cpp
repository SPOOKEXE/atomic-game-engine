#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/net/quic/Connection.hpp>

#include <algorithm>
#include <array>
#include <cryptopp/osrng.h>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <ngtcp2/ngtcp2.h>

namespace engine::net::quic {

	namespace {
		// The one QUIC version this speaks. Version 2 (RFC 9369) is the same
		// protocol with different salts and different packet-type numbers, and
		// supporting it would be a second set of constants exercised by nobody.
		constexpr uint32_t VERSION = NGTCP2_PROTO_VER_V1;

		// A stamp is never zero, because ngtcp2 reads zero as "no time yet" in
		// several places and `UINT64_MAX` as "no expiry". One second of headroom
		// costs nothing and removes both edges.
		constexpr uint64_t STAMP_BASE = 1000000000ULL;

		// The length prefix in front of every message on a stream. A QUIC stream
		// is bytes; a caller above sends messages, and this is where the two
		// meet.
		constexpr size_t LENGTH_BYTES = 4;

		// Something non-null to hang off `ngtcp2_crypto_aead::native_handle` and
		// its friends. ngtcp2's core only reads `max_overhead` from an aead, but
		// its own code treats a null handle as "no context installed", so the
		// field has to be something. The suite itself travels in the *context*,
		// which is where a key already is.
		const int SUITE_TAG = 0;

		// The key material behind one `ngtcp2_crypto_aead_ctx`.
		//
		// Allocated per installed key and freed in `delete_crypto_aead_ctx`,
		// which is the lifetime ngtcp2 owns. Holding the key here rather than in
		// the connection is what makes a key update a swap of one pointer.
		struct AeadContext {
			Aead Suite = Aead::Aes128Gcm;
			std::array<std::byte, MAXIMUM_KEY_BYTES> Key{};
			size_t Length = 0;
		};

		// The key material behind one `ngtcp2_crypto_cipher_ctx`.
		struct HeaderContext {
			HeaderCipher Cipher = HeaderCipher::Aes128;
			std::array<std::byte, MAXIMUM_KEY_BYTES> Key{};
			size_t Length = 0;
		};

		ngtcp2_tstamp Stamp(double seconds) {
			if (!(seconds > 0.0)) {
				return STAMP_BASE;
			}
			return STAMP_BASE + static_cast<uint64_t>(seconds * 1e9);
		}

		double Unstamp(ngtcp2_tstamp stamp) {
			if (stamp <= STAMP_BASE) {
				return 0.0;
			}
			return static_cast<double>(stamp - STAMP_BASE) / 1e9;
		}

		Level LevelOf(ngtcp2_encryption_level level) {
			switch (level) {
			case NGTCP2_ENCRYPTION_LEVEL_INITIAL:
				return Level::Initial;
			case NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE:
				return Level::Handshake;
			default:
				return Level::Application;
			}
		}

		ngtcp2_encryption_level LevelOf(Level level) {
			switch (level) {
			case Level::Initial:
				return NGTCP2_ENCRYPTION_LEVEL_INITIAL;
			case Level::Handshake:
				return NGTCP2_ENCRYPTION_LEVEL_HANDSHAKE;
			case Level::Application:
				return NGTCP2_ENCRYPTION_LEVEL_1RTT;
			}
			return NGTCP2_ENCRYPTION_LEVEL_1RTT;
		}

		// RFC 9001 §6.6's confidentiality and integrity limits.
		//
		// **Not decoration.** They are the point at which a key must be retired,
		// and ngtcp2 enforces them for us once they are stated - so leaving them
		// at zero would be asking for the one failure mode a key update exists to
		// prevent.
		uint64_t EncryptionLimit(Aead suite) {
			return suite == Aead::ChaCha20Poly1305 ? (1ULL << 62) : (1ULL << 23);
		}

		uint64_t DecryptionFailureLimit(Aead suite) {
			return suite == Aead::ChaCha20Poly1305 ? (1ULL << 36) : (1ULL << 52);
		}

		bool RandomBytes(std::span<std::byte> out) {
			try {
				CryptoPP::OS_GenerateRandomBlock(
					false, reinterpret_cast<CryptoPP::byte *>(out.data()), out.size()
				);
			} catch (const CryptoPP::Exception &) {
				core::Metrics::Count("net.quic.no_entropy", 1.0);
				return false;
			}
			return true;
		}

		// An address ngtcp2 can compare, built from one this module already had.
		//
		// ngtcp2 identifies a path by these bytes so that it can notice a peer
		// moving. Nothing here migrates - `disable_active_migration` is set - so
		// what matters is only that the same endpoint always produces the same
		// bytes.
		ngtcp2_socklen ToSockaddr(const Endpoint &endpoint, ngtcp2_sockaddr_union &out) {
			std::memset(&out, 0, sizeof(out));
			if (endpoint.Family == AddressFamily::IPv6) {
				out.in6.sin6_family = NGTCP2_AF_INET6;
				out.in6.sin6_port = static_cast<uint16_t>((endpoint.Port >> 8) | (endpoint.Port << 8));
				std::memcpy(&out.in6.sin6_addr, endpoint.Address.data(), 16);
				return sizeof(out.in6);
			}
			out.in.sin_family = NGTCP2_AF_INET;
			out.in.sin_port = static_cast<uint16_t>((endpoint.Port >> 8) | (endpoint.Port << 8));
			std::memcpy(&out.in.sin_addr, endpoint.Address.data(), 4);
			return sizeof(out.in);
		}
	}

	const char *Describe(ConnectionState state) {
		switch (state) {
		case ConnectionState::Handshaking:
			return "handshaking";
		case ConnectionState::Established:
			return "established";
		case ConnectionState::Closing:
			return "closing";
		case ConnectionState::Closed:
			return "closed";
		}
		return "unknown";
	}

	// Everything ngtcp2-shaped, so that no vendor type reaches a public header.
	// `net/AGENTS.md`'s rule for asio, kept for this.
	struct Connection::Internals {
		ngtcp2_conn *Conn = nullptr;
		ngtcp2_callbacks Callbacks{};
		ngtcp2_settings Options{};
		ngtcp2_transport_params Params{};
		ngtcp2_path_storage Path{};
		ngtcp2_crypto_ctx InitialContext{};
		ngtcp2_crypto_ctx Context{};

		Tls Handshake;
		ConnectionSettings Config;
		std::vector<Arrival> Arrived;
		bool Server = false;
		bool ContextSet = false;
		ConnectionState Phase = ConnectionState::Handshaking;
		const char *Reason = "";
		ngtcp2_tstamp Last = STAMP_BASE;

		// One outbound stream per channel, opened lazily.
		//
		// **The chunks are a deque and never a vector**, and that is load-bearing
		// rather than a preference: ngtcp2 does not copy stream data, it keeps
		// pointers into what it was handed until the peer acknowledges it. A
		// container that moved its elements on a push or a pop would leave those
		// pointers aimed at freed memory, and the failure would be a corrupted
		// retransmission rather than a crash.
		struct Outbound {
			int64_t Stream = -1;
			std::deque<std::vector<std::byte>> Chunks;

			// The stream offset the front chunk starts at.
			uint64_t Base = 0;

			// The stream offset everything below which has been handed to
			// ngtcp2.
			uint64_t Submitted = 0;

			// The stream offset everything below which the peer has
			// acknowledged.
			uint64_t Acknowledged = 0;

			// Whether the channel byte has been queued.
			bool Opened = false;
		};

		std::array<Outbound, MAXIMUM_CHANNELS> Channels;
		std::map<int64_t, uint8_t> StreamChannel;

		// Where the next packet's scan for something to send starts.
		//
		// **Round-robin, and it is the point rather than a nicety.** Scanning
		// from zero every time lets the lowest-numbered channel with anything
		// queued take every packet, so a megabyte snapshot would still hold up a
		// door opening - through scheduling instead of through ordering, which
		// is the same stall wearing a different hat. Separate streams remove the
		// head-of-line blocking; taking turns is what makes the removal visible.
		uint8_t NextChannel = 0;

		// One inbound stream per stream the peer opened.
		struct Inbound {
			bool KnowsChannel = false;
			uint8_t Channel = 0;
			std::vector<std::byte> Bytes;
		};

		std::map<int64_t, Inbound> Reading;

		// Unreliable messages waiting for a packet with room.
		std::deque<std::vector<std::byte>> PendingDatagrams;
		uint64_t NextDatagramId = 1;

		// Handshake bytes handed to ngtcp2, kept alive for the connection.
		//
		// ngtcp2 does not copy CRYPTO data either, and a handshake is a few
		// kilobytes that live as long as the connection anyway - so this is a
		// deque that is never popped rather than a lifetime to reason about.
		std::deque<std::vector<std::byte>> CryptoHeld;

		std::vector<std::array<std::byte, CONNECTION_ID_BYTES>> Ids;
		std::vector<std::byte> ScratchDatagram;
		Statistics Counters;

		Internals(Tls::Role role, ConnectionSettings settings)
			: Handshake(role, settings.Tls), Config(std::move(settings)) {}

		~Internals() {
			if (Conn != nullptr) {
				ngtcp2_conn_del(Conn);
			}
		}

		void Fail(const char *reason) {
			if (Phase != ConnectionState::Closed) {
				Phase = ConnectionState::Closed;
				Reason = reason;
				core::Metrics::Count("net.quic.failed", 1.0);
			}
		}

		// Installs one direction's keys at one level.
		int InstallKey(Level level, bool reading, std::span<const std::byte> secret);

		// Acts on everything the handshake asked for, in the order it asked.
		int ApplyHandshake();

		bool DeriveInitial(std::span<const std::byte> destination);
	};

	namespace {
		bool SetLocalParameters(ngtcp2_conn *conn, Connection::Internals *inside);

		Connection::Internals *Of(void *user) {
			return static_cast<Connection::Internals *>(user);
		}

		// --- the crypto callbacks, which are the whole seam ------------------

		int OnEncrypt(
			uint8_t *dest,
			const ngtcp2_crypto_aead *aead,
			const ngtcp2_crypto_aead_ctx *context,
			const uint8_t *plaintext,
			size_t plaintextLength,
			const uint8_t *nonce,
			size_t nonceLength,
			const uint8_t *associated,
			size_t associatedLength
		) {
			(void)aead;
			const auto *key = static_cast<const AeadContext *>(context->native_handle);
			if (key == nullptr) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			const bool sealed = Seal(
				key->Suite,
				{key->Key.data(), key->Length},
				{reinterpret_cast<const std::byte *>(nonce), nonceLength},
				{reinterpret_cast<const std::byte *>(associated), associatedLength},
				{reinterpret_cast<const std::byte *>(plaintext), plaintextLength},
				{reinterpret_cast<std::byte *>(dest), plaintextLength + TAG_BYTES}
			);
			return sealed ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
		}

		int OnDecrypt(
			uint8_t *dest,
			const ngtcp2_crypto_aead *aead,
			const ngtcp2_crypto_aead_ctx *context,
			const uint8_t *ciphertext,
			size_t ciphertextLength,
			const uint8_t *nonce,
			size_t nonceLength,
			const uint8_t *associated,
			size_t associatedLength
		) {
			(void)aead;
			const auto *key = static_cast<const AeadContext *>(context->native_handle);
			if (key == nullptr || ciphertextLength < TAG_BYTES) {
				return NGTCP2_ERR_DECRYPT;
			}
			const bool opened = Open(
				key->Suite,
				{key->Key.data(), key->Length},
				{reinterpret_cast<const std::byte *>(nonce), nonceLength},
				{reinterpret_cast<const std::byte *>(associated), associatedLength},
				{reinterpret_cast<const std::byte *>(ciphertext), ciphertextLength},
				{reinterpret_cast<std::byte *>(dest), ciphertextLength - TAG_BYTES}
			);
			// **`NGTCP2_ERR_DECRYPT` and not a callback failure.** A packet that
			// does not open is ordinary - it is somebody else's, or a forgery -
			// and ngtcp2 discards it and carries on; a callback failure would
			// tear the connection down, which is exactly what an attacker who can
			// write to this address would want.
			return opened ? 0 : NGTCP2_ERR_DECRYPT;
		}

		int OnHeaderMask(
			uint8_t *dest,
			const ngtcp2_crypto_cipher *cipher,
			const ngtcp2_crypto_cipher_ctx *context,
			const uint8_t *sample
		) {
			(void)cipher;
			const auto *key = static_cast<const HeaderContext *>(context->native_handle);
			if (key == nullptr) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			const bool masked = Mask(
				key->Cipher,
				{key->Key.data(), key->Length},
				{reinterpret_cast<const std::byte *>(sample), SAMPLE_BYTES},
				{reinterpret_cast<std::byte *>(dest), MASK_BYTES}
			);
			return masked ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
		}

		void OnDeleteAead(ngtcp2_conn *conn, ngtcp2_crypto_aead_ctx *context, void *user) {
			(void)conn;
			(void)user;
			delete static_cast<AeadContext *>(context->native_handle);
			context->native_handle = nullptr;
		}

		void OnDeleteCipher(ngtcp2_conn *conn, ngtcp2_crypto_cipher_ctx *context, void *user) {
			(void)conn;
			(void)user;
			delete static_cast<HeaderContext *>(context->native_handle);
			context->native_handle = nullptr;
		}

		void OnRandom(uint8_t *dest, size_t length, const ngtcp2_rand_ctx *context) {
			(void)context;
			if (!RandomBytes({reinterpret_cast<std::byte *>(dest), length})) {
				// There is no way to refuse from here - the signature returns
				// void - and returning predictable bytes would be worse than
				// anything this is used for. Zeroed and counted, and the
				// handshake that needed entropy has already refused.
				std::memset(dest, 0, length);
			}
		}

		int OnPathChallenge(ngtcp2_conn *conn, uint8_t *data, void *user) {
			(void)conn;
			(void)user;
			return RandomBytes({reinterpret_cast<std::byte *>(data), NGTCP2_PATH_CHALLENGE_DATALEN})
					   ? 0
					   : NGTCP2_ERR_CALLBACK_FAILURE;
		}

		int OnNewConnectionId(
			ngtcp2_conn *conn, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token, size_t length, void *user
		) {
			(void)conn;
			auto *inside = Of(user);
			if (length > NGTCP2_MAX_CIDLEN) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			if (!RandomBytes({reinterpret_cast<std::byte *>(cid->data), length}) ||
				!RandomBytes({reinterpret_cast<std::byte *>(token->data), sizeof(token->data)})) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			cid->datalen = length;

			if (length == CONNECTION_ID_BYTES) {
				std::array<std::byte, CONNECTION_ID_BYTES> copy{};
				std::memcpy(copy.data(), cid->data, CONNECTION_ID_BYTES);
				inside->Ids.push_back(copy);
			}
			return 0;
		}

		int OnRemoveConnectionId(ngtcp2_conn *conn, const ngtcp2_cid *cid, void *user) {
			(void)conn;
			auto *inside = Of(user);
			if (cid->datalen != CONNECTION_ID_BYTES) {
				return 0;
			}
			std::array<std::byte, CONNECTION_ID_BYTES> copy{};
			std::memcpy(copy.data(), cid->data, CONNECTION_ID_BYTES);
			inside->Ids.erase(std::remove(inside->Ids.begin(), inside->Ids.end(), copy), inside->Ids.end());
			return 0;
		}

		int OnUpdateKey(
			ngtcp2_conn *conn,
			uint8_t *readSecret,
			uint8_t *writeSecret,
			ngtcp2_crypto_aead_ctx *readContext,
			uint8_t *readIv,
			ngtcp2_crypto_aead_ctx *writeContext,
			uint8_t *writeIv,
			const uint8_t *currentRead,
			const uint8_t *currentWrite,
			size_t length,
			void *user
		) {
			(void)conn;
			auto *inside = Of(user);
			if (length != SECRET_BYTES) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}

			// RFC 9001 §6: the AEAD key and the IV are replaced and **the
			// header-protection key is not**. That asymmetry is why this
			// callback hands back two contexts and no cipher context.
			const auto next =
				[&](const uint8_t *current, uint8_t *secretOut, uint8_t *ivOut, ngtcp2_crypto_aead_ctx *out) {
					const auto secret =
						NextSecret({reinterpret_cast<const std::byte *>(current), SECRET_BYTES});
					std::memcpy(secretOut, secret.data(), SECRET_BYTES);

					const PacketKeys keys =
						DeriveKeys(secret, inside->Handshake.Suite(), inside->Handshake.Header());
					std::memcpy(ivOut, keys.Iv.data(), NONCE_BYTES);

					auto *context = new AeadContext();
					context->Suite = inside->Handshake.Suite();
					context->Key = keys.Key;
					context->Length = keys.KeyLength;
					out->native_handle = context;
				};

			next(currentRead, readSecret, readIv, readContext);
			next(currentWrite, writeSecret, writeIv, writeContext);
			core::Metrics::Count("net.quic.key_update", 1.0);
			return 0;
		}

		// --- the handshake callbacks ------------------------------------------

		// Hands this end's transport parameters to the handshake.
		//
		// **Not at construction, and the reason is version information.** RFC
		// 9368's `version_information` parameter carries the version in force,
		// and ngtcp2 does not know it until the peer's first packet has settled
		// which one that is - so a set of parameters encoded any earlier carries
		// a chosen version of zero, which the far end rejects as malformed. That
		// is what this cost half an hour of the wrong hypothesis, and it is why
		// this happens in a callback rather than beside `ngtcp2_conn_server_new`.
		bool SetLocalParameters(ngtcp2_conn *conn, Connection::Internals *inside) {
			std::array<uint8_t, 512> encoded{};
			const ngtcp2_ssize written =
				ngtcp2_conn_encode_local_transport_params(conn, encoded.data(), encoded.size());
			if (written < 0) {
				return false;
			}
			inside->Handshake.SetTransportParameters(
				{reinterpret_cast<const std::byte *>(encoded.data()), static_cast<size_t>(written)}
			);
			return true;
		}

		int OnClientInitial(ngtcp2_conn *conn, void *user) {
			auto *inside = Of(user);

			const ngtcp2_cid *destination = ngtcp2_conn_get_dcid(conn);
			if (!inside->DeriveInitial(
					{reinterpret_cast<const std::byte *>(destination->data), destination->datalen}
				)) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}

			if (!SetLocalParameters(conn, inside)) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}

			if (!inside->Handshake.Begin()) {
				inside->Fail(inside->Handshake.Failure());
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			return inside->ApplyHandshake();
		}

		int OnRecvClientInitial(ngtcp2_conn *conn, const ngtcp2_cid *destination, void *user) {
			(void)conn;
			auto *inside = Of(user);
			return inside->DeriveInitial(
					   {reinterpret_cast<const std::byte *>(destination->data), destination->datalen}
				   )
					   ? 0
					   : NGTCP2_ERR_CALLBACK_FAILURE;
		}

		int OnRecvRetry(ngtcp2_conn *conn, const ngtcp2_pkt_hd *header, void *user) {
			(void)conn;
			auto *inside = Of(user);
			// A Retry restarts the Initial keys against the connection id the
			// server chose, which is what makes the old ones useless to anybody
			// who captured the first flight.
			return inside->DeriveInitial(
					   {reinterpret_cast<const std::byte *>(header->scid.data), header->scid.datalen}
				   )
					   ? 0
					   : NGTCP2_ERR_CALLBACK_FAILURE;
		}

		int OnRecvCrypto(
			ngtcp2_conn *conn,
			ngtcp2_encryption_level level,
			uint64_t offset,
			const uint8_t *data,
			size_t length,
			void *user
		) {
			(void)offset;
			auto *inside = Of(user);
			if (!inside->Handshake.Receive(
					LevelOf(level), {reinterpret_cast<const std::byte *>(data), length}
				)) {
				inside->Fail(inside->Handshake.Failure());
				// RFC 9001 §4.8: a TLS alert becomes a `CRYPTO_ERROR` of 0x100
				// plus the alert, so the peer is told what was wrong rather than
				// being left to guess at a closed connection.
				ngtcp2_conn_set_tls_error(
					conn, static_cast<int>(NGTCP2_CRYPTO_ERROR | inside->Handshake.Alert())
				);
				return NGTCP2_ERR_CRYPTO;
			}
			return inside->ApplyHandshake();
		}

		int OnHandshakeComplete(ngtcp2_conn *conn, void *user) {
			(void)conn;
			auto *inside = Of(user);
			inside->Phase = ConnectionState::Established;
			core::Metrics::Count("net.quic.established", 1.0);
			return 0;
		}

		// --- the traffic callbacks --------------------------------------------

		int OnRecvStreamData(
			ngtcp2_conn *conn,
			uint32_t flags,
			int64_t stream,
			uint64_t offset,
			const uint8_t *data,
			size_t length,
			void *user,
			void *streamUser
		) {
			(void)flags;
			(void)offset;
			(void)streamUser;
			auto *inside = Of(user);
			auto &reading = inside->Reading[stream];
			reading.Bytes.insert(
				reading.Bytes.end(),
				reinterpret_cast<const std::byte *>(data),
				reinterpret_cast<const std::byte *>(data) + length
			);

			// The channel is the first byte the stream ever carries. See
			// `Connection.hpp` on why it is stated rather than derived from the
			// stream id.
			if (!reading.KnowsChannel) {
				if (reading.Bytes.empty()) {
					return 0;
				}
				reading.Channel = static_cast<uint8_t>(reading.Bytes[0]);
				if (reading.Channel >= MAXIMUM_CHANNELS) {
					// A channel byte from the wire indexes an array, so it is
					// range-checked before the cast and refused rather than
					// clamped - the same rule `Packet::Read` keeps for its
					// channel byte.
					return NGTCP2_ERR_CALLBACK_FAILURE;
				}
				reading.KnowsChannel = true;
				reading.Bytes.erase(reading.Bytes.begin());
			}

			// Length-framed messages, taken whole or held.
			size_t at = 0;
			while (reading.Bytes.size() - at >= LENGTH_BYTES) {
				size_t size = 0;
				for (size_t index = 0; index < LENGTH_BYTES; index++) {
					size = (size << 8) | static_cast<size_t>(reading.Bytes[at + index]);
				}
				if (size > inside->Config.MaximumMessageBytes) {
					// The length is the peer's and an allocation sized from it
					// would be the peer's too.
					return NGTCP2_ERR_CALLBACK_FAILURE;
				}
				if (reading.Bytes.size() - at - LENGTH_BYTES < size) {
					break;
				}
				const auto from = reading.Bytes.begin() + static_cast<std::ptrdiff_t>(at + LENGTH_BYTES);
				inside->Arrived.push_back(
					{reading.Channel, true, {from, from + static_cast<std::ptrdiff_t>(size)}}
				);
				at += LENGTH_BYTES + size;
			}
			if (at > 0) {
				reading.Bytes.erase(
					reading.Bytes.begin(), reading.Bytes.begin() + static_cast<std::ptrdiff_t>(at)
				);
			}

			// Flow control only reopens for bytes that were consumed, which is
			// what makes it back-pressure rather than a formality.
			ngtcp2_conn_extend_max_stream_offset(conn, stream, length);
			ngtcp2_conn_extend_max_offset(conn, length);
			return 0;
		}

		int OnStreamClose(
			ngtcp2_conn *conn,
			uint32_t flags,
			int64_t stream,
			uint64_t readError,
			uint64_t writeError,
			void *user,
			void *streamUser
		) {
			(void)conn;
			(void)flags;
			(void)readError;
			(void)writeError;
			(void)streamUser;
			auto *inside = Of(user);
			inside->Reading.erase(stream);
			return 0;
		}

		int OnAckedStreamData(
			ngtcp2_conn *conn, int64_t stream, uint64_t offset, uint64_t length, void *user, void *streamUser
		) {
			(void)conn;
			(void)streamUser;
			auto *inside = Of(user);
			const auto found = inside->StreamChannel.find(stream);
			if (found == inside->StreamChannel.end()) {
				return 0;
			}

			auto &channel = inside->Channels[found->second];
			channel.Acknowledged = std::max(channel.Acknowledged, offset + length);

			// Only whole chunks are released, because a chunk is what ngtcp2 was
			// given a pointer to.
			while (!channel.Chunks.empty() &&
				   channel.Base + channel.Chunks.front().size() <= channel.Acknowledged) {
				channel.Base += channel.Chunks.front().size();
				channel.Chunks.pop_front();
			}
			return 0;
		}

		int
		OnRecvDatagram(ngtcp2_conn *conn, uint32_t flags, const uint8_t *data, size_t length, void *user) {
			(void)conn;
			(void)flags;
			auto *inside = Of(user);
			if (length < 1) {
				return 0;
			}
			const auto channel = static_cast<uint8_t>(data[0]);
			if (channel >= MAXIMUM_CHANNELS) {
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			inside->Arrived.push_back(
				{channel,
				 false,
				 {reinterpret_cast<const std::byte *>(data) + 1,
				  reinterpret_cast<const std::byte *>(data) + length}}
			);
			return 0;
		}

		int OnAckDatagram(ngtcp2_conn *conn, uint64_t id, void *user) {
			(void)conn;
			(void)id;
			// **The counter D00014 says the hand-rolled stack cannot have.** A
			// DATAGRAM frame is never retransmitted and its containing packet is
			// still acknowledged, so unreliable traffic reports its own delivery.
			Of(user)->Counters.DatagramsAcknowledged++;
			return 0;
		}

		int OnLostDatagram(ngtcp2_conn *conn, uint64_t id, void *user) {
			(void)conn;
			(void)id;
			Of(user)->Counters.DatagramsLost++;
			return 0;
		}
	}

	bool Connection::Internals::DeriveInitial(std::span<const std::byte> destination) {
		const InitialKeys keys = DeriveInitialKeys(destination);
		const PacketKeys &reading = Server ? keys.Client : keys.Server;
		const PacketKeys &writing = Server ? keys.Server : keys.Client;

		InitialContext.aead.native_handle = const_cast<int *>(&SUITE_TAG);
		InitialContext.aead.max_overhead = TAG_BYTES;
		InitialContext.md.native_handle = const_cast<int *>(&SUITE_TAG);
		InitialContext.hp.native_handle = const_cast<int *>(&SUITE_TAG);
		InitialContext.max_encryption = EncryptionLimit(Aead::Aes128Gcm);
		InitialContext.max_decryption_failure = DecryptionFailureLimit(Aead::Aes128Gcm);
		ngtcp2_conn_set_initial_crypto_ctx(Conn, &InitialContext);

		const auto aead = [](const PacketKeys &from) {
			auto *context = new AeadContext();
			context->Suite = Aead::Aes128Gcm;
			context->Key = from.Key;
			context->Length = from.KeyLength;
			return context;
		};
		const auto header = [](const PacketKeys &from) {
			auto *context = new HeaderContext();
			context->Cipher = HeaderCipher::Aes128;
			context->Key = from.HeaderKey;
			context->Length = from.HeaderKeyLength;
			return context;
		};

		ngtcp2_crypto_aead_ctx readAead{aead(reading)};
		ngtcp2_crypto_cipher_ctx readHeader{header(reading)};
		ngtcp2_crypto_aead_ctx writeAead{aead(writing)};
		ngtcp2_crypto_cipher_ctx writeHeader{header(writing)};

		return ngtcp2_conn_install_initial_key(
				   Conn,
				   &readAead,
				   reinterpret_cast<const uint8_t *>(reading.Iv.data()),
				   &readHeader,
				   &writeAead,
				   reinterpret_cast<const uint8_t *>(writing.Iv.data()),
				   &writeHeader,
				   NONCE_BYTES
			   ) == 0;
	}

	int Connection::Internals::InstallKey(Level level, bool reading, std::span<const std::byte> secret) {
		if (!ContextSet) {
			// The negotiated suite is known from the ServerHello onwards, which
			// is before any key at this level is installed. ngtcp2 needs it to
			// size its own buffers, so it is set once and never changed.
			Context.aead.native_handle = const_cast<int *>(&SUITE_TAG);
			Context.aead.max_overhead = TAG_BYTES;
			Context.md.native_handle = const_cast<int *>(&SUITE_TAG);
			Context.hp.native_handle = const_cast<int *>(&SUITE_TAG);
			Context.max_encryption = EncryptionLimit(Handshake.Suite());
			Context.max_decryption_failure = DecryptionFailureLimit(Handshake.Suite());
			ngtcp2_conn_set_crypto_ctx(Conn, &Context);
			ContextSet = true;
		}

		const PacketKeys keys = DeriveKeys(secret, Handshake.Suite(), Handshake.Header());

		auto *aead = new AeadContext();
		aead->Suite = Handshake.Suite();
		aead->Key = keys.Key;
		aead->Length = keys.KeyLength;

		auto *header = new HeaderContext();
		header->Cipher = Handshake.Header();
		header->Key = keys.HeaderKey;
		header->Length = keys.HeaderKeyLength;

		ngtcp2_crypto_aead_ctx aeadContext{aead};
		ngtcp2_crypto_cipher_ctx headerContext{header};
		const auto *iv = reinterpret_cast<const uint8_t *>(keys.Iv.data());

		int result = 0;
		if (level == Level::Handshake) {
			result = reading ? ngtcp2_conn_install_rx_handshake_key(
								   Conn, &aeadContext, iv, NONCE_BYTES, &headerContext
							   )
							 : ngtcp2_conn_install_tx_handshake_key(
								   Conn, &aeadContext, iv, NONCE_BYTES, &headerContext
							   );
		} else {
			const auto *material = reinterpret_cast<const uint8_t *>(secret.data());
			result = reading
						 ? ngtcp2_conn_install_rx_key(
							   Conn, material, secret.size(), &aeadContext, iv, NONCE_BYTES, &headerContext
						   )
						 : ngtcp2_conn_install_tx_key(
							   Conn, material, secret.size(), &aeadContext, iv, NONCE_BYTES, &headerContext
						   );
		}
		return result == 0 ? 0 : NGTCP2_ERR_CALLBACK_FAILURE;
	}

	int Connection::Internals::ApplyHandshake() {
		for (const Tls::Event &event : Handshake.Pending()) {
			switch (event.What) {
			case Tls::Event::Kind::Send: {
				// Kept alive for the connection: ngtcp2 holds the pointer until
				// the peer acknowledges it, and a handshake is a few kilobytes.
				CryptoHeld.push_back(event.Bytes);
				const std::vector<std::byte> &held = CryptoHeld.back();
				if (ngtcp2_conn_submit_crypto_data(
						Conn, LevelOf(event.At), reinterpret_cast<const uint8_t *>(held.data()), held.size()
					) != 0) {
					return NGTCP2_ERR_CALLBACK_FAILURE;
				}
				break;
			}
			case Tls::Event::Kind::ReadKey: {
				const int result = InstallKey(event.At, true, event.Secret);
				if (result != 0) {
					return result;
				}
				break;
			}
			case Tls::Event::Kind::WriteKey: {
				const int result = InstallKey(event.At, false, event.Secret);
				if (result != 0) {
					return result;
				}
				break;
			}
			case Tls::Event::Kind::PeerParameters:
				if (const int rv = ngtcp2_conn_decode_and_set_remote_transport_params(
						Conn, reinterpret_cast<const uint8_t *>(event.Bytes.data()), event.Bytes.size()
					);
					rv != 0) {
					Fail("the peer's transport parameters were refused");
					return NGTCP2_ERR_TRANSPORT_PARAM;
				}
				break;
			case Tls::Event::Kind::Complete:
				ngtcp2_conn_tls_handshake_completed(Conn);
				break;
			}
		}
		Handshake.ClearPending();

		// A server's flight waits for its own transport parameters, and those
		// wait for the client's - see `Tls::NeedsParameters`. The peer's have
		// just been applied above, so this is the first moment ngtcp2 can encode
		// a set with a version in it.
		if (Handshake.NeedsParameters()) {
			if (!SetLocalParameters(Conn, this) || !Handshake.Resume()) {
				Fail(Handshake.Failure());
				return NGTCP2_ERR_CALLBACK_FAILURE;
			}
			return ApplyHandshake();
		}
		return 0;
	}

	// --- construction --------------------------------------------------------

	Connection::Connection(Transport &transport, const Endpoint &peer, ConnectionSettings settings)
		: Wire(&transport), Address(peer) {
		(void)settings;
	}

	Connection::~Connection() = default;

	std::span<const Arrival> Connection::Inbound() const {
		return Inside->Arrived;
	}

	namespace {
		// Everything both ends set the same way.
		void FillCallbacks(ngtcp2_callbacks &callbacks, bool server) {
			callbacks.recv_crypto_data = OnRecvCrypto;
			callbacks.handshake_completed = OnHandshakeComplete;
			callbacks.encrypt = OnEncrypt;
			callbacks.decrypt = OnDecrypt;
			callbacks.hp_mask = OnHeaderMask;
			callbacks.recv_stream_data = OnRecvStreamData;
			callbacks.acked_stream_data_offset = OnAckedStreamData;
			callbacks.stream_close2 = OnStreamClose;
			callbacks.rand = OnRandom;
			callbacks.get_new_connection_id2 = OnNewConnectionId;
			callbacks.remove_connection_id = OnRemoveConnectionId;
			callbacks.update_key = OnUpdateKey;
			callbacks.delete_crypto_aead_ctx = OnDeleteAead;
			callbacks.delete_crypto_cipher_ctx = OnDeleteCipher;
			callbacks.get_path_challenge_data = OnPathChallenge;
			callbacks.recv_datagram = OnRecvDatagram;
			callbacks.ack_datagram = OnAckDatagram;
			callbacks.lost_datagram = OnLostDatagram;
			if (server) {
				callbacks.recv_client_initial = OnRecvClientInitial;
			} else {
				callbacks.client_initial = OnClientInitial;
				callbacks.recv_retry = OnRecvRetry;
			}
		}

		void FillParameters(ngtcp2_transport_params &params, const ConnectionSettings &settings) {
			ngtcp2_transport_params_default(&params);
			params.initial_max_stream_data_uni = settings.MaximumStreamData;
			params.initial_max_stream_data_bidi_local = settings.MaximumStreamData;
			params.initial_max_stream_data_bidi_remote = settings.MaximumStreamData;
			params.initial_max_data = settings.MaximumConnectionData;
			params.initial_max_streams_uni = MAXIMUM_CHANNELS;
			params.initial_max_streams_bidi = 0;
			params.max_idle_timeout = settings.IdleTimeoutMilliseconds * NGTCP2_MILLISECONDS;
			params.max_udp_payload_size = Transport::MAXIMUM_DATAGRAM_BYTES;
			// Nothing here migrates: one `Connection` is one peer at one
			// address, and a session that moved would be a new one.
			params.disable_active_migration = 1;
			params.max_datagram_frame_size = settings.AllowUnreliable ? Transport::MAXIMUM_DATAGRAM_BYTES : 0;
		}

		void FillOptions(ngtcp2_settings &options, double nowSeconds) {
			ngtcp2_settings_default(&options);
			options.initial_ts = Stamp(nowSeconds);
			options.max_tx_udp_payload_size = Transport::MAXIMUM_DATAGRAM_BYTES;
			// No probing beyond what the transport will carry. `Transport`
			// refuses an oversized datagram outright rather than fragmenting, so
			// a probe above its maximum would measure the wrong thing.
			options.no_pmtud = 1;
			options.handshake_timeout = 10 * NGTCP2_SECONDS;
		}

		bool RandomConnectionId(ngtcp2_cid &cid) {
			std::array<std::byte, CONNECTION_ID_BYTES> bytes{};
			if (!RandomBytes(bytes)) {
				return false;
			}
			std::memcpy(cid.data, bytes.data(), CONNECTION_ID_BYTES);
			cid.datalen = CONNECTION_ID_BYTES;
			return true;
		}
	}

	std::unique_ptr<Connection> Connection::Connect(
		Transport &transport, const Endpoint &peer, double nowSeconds, ConnectionSettings settings
	) {
		if (!peer.IsValid()) {
			return nullptr;
		}

		std::unique_ptr<Connection> connection(new Connection(transport, peer, settings));
		connection->Inside = std::make_unique<Internals>(Tls::Role::Client, settings);
		Internals &inside = *connection->Inside;
		inside.Server = false;
		if (inside.Handshake.Failed()) {
			return nullptr;
		}

		ngtcp2_cid local{};
		ngtcp2_cid remote{};
		if (!RandomConnectionId(local) || !RandomConnectionId(remote)) {
			return nullptr;
		}

		FillCallbacks(inside.Callbacks, false);
		FillParameters(inside.Params, inside.Config);
		FillOptions(inside.Options, nowSeconds);

		ngtcp2_sockaddr_union localAddress{};
		ngtcp2_sockaddr_union remoteAddress{};
		const ngtcp2_socklen localLength = ToSockaddr(Endpoint{{}, 0, peer.Family}, localAddress);
		const ngtcp2_socklen remoteLength = ToSockaddr(peer, remoteAddress);
		ngtcp2_path_storage_init(
			&inside.Path,
			reinterpret_cast<const ngtcp2_sockaddr *>(&localAddress),
			localLength,
			reinterpret_cast<const ngtcp2_sockaddr *>(&remoteAddress),
			remoteLength,
			nullptr
		);

		if (ngtcp2_conn_client_new(
				&inside.Conn,
				&remote,
				&local,
				&inside.Path.path,
				VERSION,
				&inside.Callbacks,
				&inside.Options,
				&inside.Params,
				nullptr,
				&inside
			) != 0) {
			return nullptr;
		}

		std::array<std::byte, CONNECTION_ID_BYTES> id{};
		std::memcpy(id.data(), local.data, CONNECTION_ID_BYTES);
		inside.Ids.push_back(id);
		inside.Last = Stamp(nowSeconds);
		return connection;
	}

	std::unique_ptr<Connection> Connection::Accept(
		Transport &transport,
		const Endpoint &peer,
		std::span<const std::byte> datagram,
		double nowSeconds,
		ConnectionSettings settings
	) {
		if (!peer.IsValid() || !settings.Tls.HasSeed) {
			return nullptr;
		}

		ngtcp2_pkt_hd header{};
		if (ngtcp2_accept(&header, reinterpret_cast<const uint8_t *>(datagram.data()), datagram.size()) !=
			0) {
			return nullptr;
		}

		std::unique_ptr<Connection> connection(new Connection(transport, peer, settings));
		connection->Inside = std::make_unique<Internals>(Tls::Role::Server, settings);
		Internals &inside = *connection->Inside;
		inside.Server = true;
		if (inside.Handshake.Failed()) {
			return nullptr;
		}

		ngtcp2_cid local{};
		if (!RandomConnectionId(local)) {
			return nullptr;
		}

		FillCallbacks(inside.Callbacks, true);
		FillParameters(inside.Params, inside.Config);
		FillOptions(inside.Options, nowSeconds);

		// **The client's original destination connection id goes in the
		// parameters and is checked by the client.** It is what binds the
		// server's answer to the packet the client actually sent, and leaving it
		// out would let anything on the path substitute its own.
		inside.Params.original_dcid = header.dcid;
		inside.Params.original_dcid_present = 1;

		ngtcp2_sockaddr_union localAddress{};
		ngtcp2_sockaddr_union remoteAddress{};
		const ngtcp2_socklen localLength = ToSockaddr(Endpoint{{}, 0, peer.Family}, localAddress);
		const ngtcp2_socklen remoteLength = ToSockaddr(peer, remoteAddress);
		ngtcp2_path_storage_init(
			&inside.Path,
			reinterpret_cast<const ngtcp2_sockaddr *>(&localAddress),
			localLength,
			reinterpret_cast<const ngtcp2_sockaddr *>(&remoteAddress),
			remoteLength,
			nullptr
		);

		if (ngtcp2_conn_server_new(
				&inside.Conn,
				&header.scid,
				&local,
				&inside.Path.path,
				header.version,
				&inside.Callbacks,
				&inside.Options,
				&inside.Params,
				nullptr,
				&inside
			) != 0) {
			return nullptr;
		}

		std::array<std::byte, CONNECTION_ID_BYTES> id{};
		std::memcpy(id.data(), local.data, CONNECTION_ID_BYTES);
		inside.Ids.push_back(id);
		inside.Last = Stamp(nowSeconds);

		if (!connection->Receive(datagram, nowSeconds)) {
			return nullptr;
		}
		return connection;
	}

	// --- traffic -------------------------------------------------------------

	bool Connection::Receive(std::span<const std::byte> datagram, double nowSeconds) {
		ENGINE_PROFILE_CAT("quic::Connection::Receive", core::ProfileCategory::Network);

		Internals &inside = *Inside;
		if (inside.Conn == nullptr || inside.Phase == ConnectionState::Closed) {
			return false;
		}

		// ngtcp2 requires a non-decreasing timestamp, and a caller passing the
		// tick's time gives one - the clamp is here so that a caller which
		// receives twice inside one tick, or hands over a slightly older number,
		// gets a refusal from nothing.
		inside.Last = std::max(inside.Last, Stamp(nowSeconds));

		ngtcp2_pkt_info info{};
		const int result = ngtcp2_conn_read_pkt(
			inside.Conn,
			&inside.Path.path,
			&info,
			reinterpret_cast<const uint8_t *>(datagram.data()),
			datagram.size(),
			inside.Last
		);
		if (result == 0) {
			return true;
		}

		inside.Counters.Refused++;
		switch (result) {
		case NGTCP2_ERR_DRAINING:
		case NGTCP2_ERR_DROP_CONN:
			inside.Phase = ConnectionState::Closed;
			inside.Reason = "the peer closed the connection";
			break;
		case NGTCP2_ERR_RETRY:
			// A server asking for address validation. Handled by the layer that
			// owns the socket, which is the only one that can answer without
			// remembering anything - `net/AGENTS.md`'s zero-bytes rule.
			break;
		default:
			if (ngtcp2_err_is_fatal(result)) {
				inside.Fail(ngtcp2_strerror(result));
			}
			break;
		}
		return false;
	}

	bool Connection::Send(uint8_t channel, std::span<const std::byte> message, double nowSeconds) {
		(void)nowSeconds;
		Internals &inside = *Inside;
		if (channel >= MAXIMUM_CHANNELS || inside.Conn == nullptr) {
			return false;
		}
		if (inside.Phase != ConnectionState::Handshaking && inside.Phase != ConnectionState::Established) {
			return false;
		}
		if (message.size() > inside.Config.MaximumMessageBytes) {
			return false;
		}

		auto &outbound = inside.Channels[channel];
		if (!outbound.Opened) {
			// The channel byte, once, before anything else on this stream.
			outbound.Chunks.push_back({static_cast<std::byte>(channel)});
			outbound.Opened = true;
		}

		std::vector<std::byte> framed;
		framed.reserve(LENGTH_BYTES + message.size());
		for (size_t index = 0; index < LENGTH_BYTES; index++) {
			const auto shift = static_cast<unsigned>(8 * (LENGTH_BYTES - 1 - index));
			framed.push_back(static_cast<std::byte>((message.size() >> shift) & 0xff));
		}
		framed.insert(framed.end(), message.begin(), message.end());
		outbound.Chunks.push_back(std::move(framed));
		inside.Counters.Messages++;
		return true;
	}

	bool Connection::SendUnreliable(uint8_t channel, std::span<const std::byte> message, double nowSeconds) {
		(void)nowSeconds;
		Internals &inside = *Inside;
		if (channel >= MAXIMUM_CHANNELS || inside.Conn == nullptr || !inside.Config.AllowUnreliable) {
			return false;
		}
		if (inside.Phase != ConnectionState::Established) {
			// An unreliable message before the handshake has nowhere to go and
			// nothing would resend it, so it is refused rather than queued into a
			// backlog that arrives all at once.
			inside.Counters.DatagramsRefused++;
			return false;
		}

		// The peer states the largest DATAGRAM *frame* it will take, tag and
		// length field included, so sixteen bytes come off the top for the frame
		// header before a payload is measured against it.
		const ngtcp2_transport_params *remote = ngtcp2_conn_get_remote_transport_params(inside.Conn);
		const uint64_t frame = remote != nullptr ? remote->max_datagram_frame_size : 0;
		const uint64_t room = frame > 16 ? frame - 16 : 0;
		if (room == 0 || message.size() + 1 > room) {
			// Refused whole rather than fragmented, for `Transport`'s reason: a
			// fragmented datagram is lost entirely when any one fragment is.
			inside.Counters.DatagramsRefused++;
			return false;
		}

		std::vector<std::byte> framed;
		framed.reserve(1 + message.size());
		framed.push_back(static_cast<std::byte>(channel));
		framed.insert(framed.end(), message.begin(), message.end());
		inside.PendingDatagrams.push_back(std::move(framed));
		return true;
	}

	size_t Connection::Flush(double nowSeconds) {
		ENGINE_PROFILE_CAT("quic::Connection::Flush", core::ProfileCategory::Network);

		Internals &inside = *Inside;
		if (inside.Conn == nullptr || inside.Phase == ConnectionState::Closed) {
			return 0;
		}
		inside.Last = std::max(inside.Last, Stamp(nowSeconds));

		// The expiry timer is driven off the tick rather than off a thread, which
		// is `docs/QUIC.md` §7 and is the whole reason ngtcp2 was the library.
		if (ngtcp2_conn_get_expiry(inside.Conn) <= inside.Last) {
			const int result = ngtcp2_conn_handle_expiry(inside.Conn, inside.Last);
			if (result != 0) {
				inside.Fail(ngtcp2_strerror(result));
				return 0;
			}
		}

		// Open a stream for every channel that has something and does not have
		// one yet. Before the handshake there is no credit, so this simply does
		// not succeed and the bytes wait.
		for (uint8_t channel = 0; channel < MAXIMUM_CHANNELS; channel++) {
			auto &outbound = inside.Channels[channel];
			if (outbound.Stream >= 0 || outbound.Chunks.empty()) {
				continue;
			}
			int64_t stream = -1;
			if (ngtcp2_conn_open_uni_stream(inside.Conn, &stream, nullptr) == 0) {
				outbound.Stream = stream;
				inside.StreamChannel[stream] = channel;
			}
		}

		inside.ScratchDatagram.resize(Transport::MAXIMUM_DATAGRAM_BYTES);
		size_t sent = 0;

		for (int guard = 0; guard < 256; guard++) {
			ngtcp2_pkt_info info{};
			ngtcp2_ssize written = 0;

			if (!inside.PendingDatagrams.empty()) {
				const std::vector<std::byte> &next = inside.PendingDatagrams.front();
				ngtcp2_vec vector{
					reinterpret_cast<uint8_t *>(const_cast<std::byte *>(next.data())), next.size()
				};
				int accepted = 0;
				written = ngtcp2_conn_writev_datagram(
					inside.Conn,
					nullptr,
					&info,
					reinterpret_cast<uint8_t *>(inside.ScratchDatagram.data()),
					inside.ScratchDatagram.size(),
					&accepted,
					NGTCP2_WRITE_DATAGRAM_FLAG_NONE,
					inside.NextDatagramId,
					&vector,
					1,
					inside.Last
				);
				if (accepted != 0) {
					inside.NextDatagramId++;
					inside.PendingDatagrams.pop_front();
					inside.Counters.Datagrams++;
				} else if (written == NGTCP2_ERR_INVALID_STATE) {
					// The peer offered no datagram support. Refused rather than
					// held, because nothing will ever carry it.
					inside.Counters.DatagramsRefused += static_cast<uint64_t>(inside.PendingDatagrams.size());
					inside.PendingDatagrams.clear();
					continue;
				}
			} else {
				// One stream per call, round-robined by taking the first with
				// unsubmitted bytes. ngtcp2 packs what it can and reports how
				// much of the vector it took.
				int64_t stream = -1;
				std::array<ngtcp2_vec, 4> vectors{};
				size_t count = 0;
				uint8_t chosen = 0;

				for (uint8_t step = 0; step < MAXIMUM_CHANNELS; step++) {
					const auto channel = static_cast<uint8_t>((inside.NextChannel + step) % MAXIMUM_CHANNELS);
					auto &outbound = inside.Channels[channel];
					if (outbound.Stream < 0) {
						continue;
					}
					uint64_t at = outbound.Base;
					for (const std::vector<std::byte> &chunk : outbound.Chunks) {
						if (count == vectors.size()) {
							break;
						}
						const uint64_t end = at + chunk.size();
						if (end > outbound.Submitted) {
							const size_t from =
								outbound.Submitted > at ? static_cast<size_t>(outbound.Submitted - at) : 0;
							vectors[count].base =
								reinterpret_cast<uint8_t *>(const_cast<std::byte *>(chunk.data() + from));
							vectors[count].len = chunk.size() - from;
							count++;
						}
						at = end;
					}
					if (count > 0) {
						stream = outbound.Stream;
						chosen = channel;
						inside.NextChannel = static_cast<uint8_t>((channel + 1) % MAXIMUM_CHANNELS);
						break;
					}
				}

				ngtcp2_ssize taken = 0;
				written = ngtcp2_conn_writev_stream(
					inside.Conn,
					nullptr,
					&info,
					reinterpret_cast<uint8_t *>(inside.ScratchDatagram.data()),
					inside.ScratchDatagram.size(),
					&taken,
					NGTCP2_WRITE_STREAM_FLAG_NONE,
					stream,
					count > 0 ? vectors.data() : nullptr,
					count,
					inside.Last
				);
				if (taken > 0) {
					inside.Channels[chosen].Submitted += static_cast<uint64_t>(taken);
				}
				if (written == NGTCP2_ERR_STREAM_DATA_BLOCKED || written == NGTCP2_ERR_STREAM_SHUT_WR) {
					// The peer's window is full for this channel. Nothing else
					// can go out for it until an acknowledgement opens it, and
					// the other channels are unaffected - which is exactly the
					// isolation the streams exist for.
					break;
				}
			}

			if (written == 0) {
				break;
			}
			if (written < 0) {
				if (ngtcp2_err_is_fatal(static_cast<int>(written))) {
					inside.Fail(ngtcp2_strerror(static_cast<int>(written)));
				}
				break;
			}

			const std::span<const std::byte> packet{
				inside.ScratchDatagram.data(), static_cast<size_t>(written)
			};
			const TransportStatus status = Wire->Send(Address, packet);
			if (status == TransportStatus::Ok) {
				inside.Counters.Sent++;
				sent++;
			} else {
				inside.Counters.Undeliverable++;
				break;
			}
		}

		// **Anything unreliable that did not fit is dropped rather than held.**
		// A late position update is worse than a dropped one, because the next
		// is already on its way and is more correct than the one being waited
		// for - which is the same sentence `net/AGENTS.md` writes about the
		// stale rule, arriving here as a queue that never carries over.
		if (!inside.PendingDatagrams.empty()) {
			inside.Counters.DatagramsRefused += static_cast<uint64_t>(inside.PendingDatagrams.size());
			inside.PendingDatagrams.clear();
		}

		ngtcp2_conn_update_pkt_tx_time(inside.Conn, inside.Last);
		if (inside.Phase == ConnectionState::Closing && ngtcp2_conn_in_draining_period(inside.Conn)) {
			inside.Phase = ConnectionState::Closed;
		}
		return sent;
	}

	double Connection::ExpirySeconds() const {
		const Internals &inside = *Inside;
		if (inside.Conn == nullptr) {
			return std::numeric_limits<double>::infinity();
		}
		const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(const_cast<ngtcp2_conn *>(inside.Conn));
		if (expiry == UINT64_MAX) {
			return std::numeric_limits<double>::infinity();
		}
		return Unstamp(expiry);
	}

	void Connection::Close(double nowSeconds) {
		Internals &inside = *Inside;
		if (inside.Conn == nullptr || inside.Phase == ConnectionState::Closing ||
			inside.Phase == ConnectionState::Closed) {
			return;
		}
		inside.Last = std::max(inside.Last, Stamp(nowSeconds));

		std::vector<std::byte> packet(Transport::MAXIMUM_DATAGRAM_BYTES);
		ngtcp2_ccerr error{};
		ngtcp2_ccerr_default(&error);
		ngtcp2_pkt_info info{};
		const ngtcp2_ssize written = ngtcp2_conn_write_connection_close(
			inside.Conn,
			nullptr,
			&info,
			reinterpret_cast<uint8_t *>(packet.data()),
			packet.size(),
			&error,
			inside.Last
		);
		if (written > 0) {
			// **A goodbye is not a formality.** Skipping it makes a peer that
			// left politely indistinguishable from one that crashed, and every
			// clean exit then costs the other end a full idle timeout.
			Wire->Send(Address, {packet.data(), static_cast<size_t>(written)});
			inside.Counters.Sent++;
		}
		inside.Phase = ConnectionState::Closing;
	}

	void Connection::ClearInbound() {
		Inside->Arrived.clear();
	}

	ConnectionState Connection::State() const {
		return Inside->Phase;
	}

	bool Connection::IsServer() const {
		return Inside->Server;
	}

	std::span<const std::array<std::byte, CONNECTION_ID_BYTES>> Connection::LocalIds() const {
		return Inside->Ids;
	}

	std::span<const std::byte> Connection::PeerIdentity() const {
		return Inside->Handshake.PeerIdentity();
	}

	bool Connection::Export(std::string_view label, std::span<std::byte> out) const {
		return Inside->Handshake.Export(label, out);
	}

	const char *Connection::Failure() const {
		return Inside->Reason;
	}

	Connection::Statistics Connection::Stats() const {
		Statistics stats = Inside->Counters;
		if (Inside->Conn != nullptr) {
			ngtcp2_conn_info info{};
			ngtcp2_conn_get_conn_info(const_cast<ngtcp2_conn *>(Inside->Conn), &info);
			stats.RoundTripMilliseconds = static_cast<double>(info.smoothed_rtt) / 1e6;
			stats.CongestionWindow = info.cwnd;
			stats.PacketsLost = info.pkt_lost;
		}
		return stats;
	}

	// --- what a listener needs before it has a connection --------------------

	bool Accepts(std::span<const std::byte> datagram) {
		ngtcp2_pkt_hd header{};
		return ngtcp2_accept(&header, reinterpret_cast<const uint8_t *>(datagram.data()), datagram.size()) ==
			   0;
	}

	bool RouteOf(std::span<const std::byte> datagram, std::array<std::byte, CONNECTION_ID_BYTES> &out) {
		ngtcp2_version_cid cid{};
		const int result = ngtcp2_pkt_decode_version_cid(
			&cid, reinterpret_cast<const uint8_t *>(datagram.data()), datagram.size(), CONNECTION_ID_BYTES
		);
		if (result != 0 || cid.dcidlen != CONNECTION_ID_BYTES) {
			return false;
		}
		std::memcpy(out.data(), cid.dcid, CONNECTION_ID_BYTES);
		return true;
	}
}
