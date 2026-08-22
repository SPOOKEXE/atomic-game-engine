#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/core/SecureWipe.hpp>
#include <engine/net/quic/Tls.hpp>

#include <array>
#include <cryptopp/donna.h>
#include <cryptopp/osrng.h>
#include <cstring>
#include <string_view>
#include <utility>

namespace engine::net::quic {

	namespace {
		// TLS 1.3 handshake message types, RFC 8446 §4. The ones this subset
		// produces or accepts, and no others - an unexpected type is a refusal.
		//@{
		constexpr uint8_t CLIENT_HELLO = 1;
		constexpr uint8_t SERVER_HELLO = 2;
		constexpr uint8_t ENCRYPTED_EXTENSIONS = 8;
		constexpr uint8_t CERTIFICATE = 11;
		constexpr uint8_t CERTIFICATE_VERIFY = 15;
		constexpr uint8_t FINISHED = 20;
		//@}

		// Extension numbers. RFC 8446 for most, RFC 7250 for the certificate
		// type and RFC 9001 §8.2 for the transport parameters.
		//@{
		constexpr uint16_t EXTENSION_SUPPORTED_GROUPS = 10;
		constexpr uint16_t EXTENSION_SIGNATURE_ALGORITHMS = 13;
		constexpr uint16_t EXTENSION_ALPN = 16;
		constexpr uint16_t EXTENSION_SERVER_CERTIFICATE_TYPE = 20;
		constexpr uint16_t EXTENSION_SUPPORTED_VERSIONS = 43;
		constexpr uint16_t EXTENSION_KEY_SHARE = 51;
		constexpr uint16_t EXTENSION_QUIC_TRANSPORT_PARAMETERS = 57;
		//@}

		// The only group, the only two suites and the only signature scheme.
		// Each omission is argued in `Tls.hpp`; what is here is the numbers.
		//@{
		constexpr uint16_t GROUP_X25519 = 0x001d;
		constexpr uint16_t SUITE_AES_128_GCM_SHA256 = 0x1301;
		constexpr uint16_t SUITE_CHACHA20_POLY1305_SHA256 = 0x1303;
		constexpr uint16_t SCHEME_ED25519 = 0x0807;
		constexpr uint16_t VERSION_TLS_1_3 = 0x0304;
		constexpr uint16_t VERSION_LEGACY = 0x0303;
		constexpr uint8_t CERTIFICATE_TYPE_RAW_PUBLIC_KEY = 2;
		//@}

		// TLS alert descriptions, RFC 8446 §6.2. QUIC carries them as a
		// `CRYPTO_ERROR` of 0x100 plus the value.
		//@{
		constexpr uint8_t ALERT_UNEXPECTED_MESSAGE = 10;
		constexpr uint8_t ALERT_HANDSHAKE_FAILURE = 40;
		constexpr uint8_t ALERT_BAD_CERTIFICATE = 42;
		constexpr uint8_t ALERT_CERTIFICATE_UNKNOWN = 46;
		constexpr uint8_t ALERT_ILLEGAL_PARAMETER = 47;
		constexpr uint8_t ALERT_DECODE_ERROR = 50;
		constexpr uint8_t ALERT_DECRYPT_ERROR = 51;
		constexpr uint8_t ALERT_PROTOCOL_VERSION = 70;
		constexpr uint8_t ALERT_INTERNAL_ERROR = 80;
		constexpr uint8_t ALERT_MISSING_EXTENSION = 109;
		//@}

		// The random a server puts in a ServerHello to say "send another
		// ClientHello with a different group". RFC 8446 §4.1.3.
		//
		// Detected rather than ignored: with one group there is nothing to
		// retry with, so this end has to refuse rather than loop.
		constexpr std::array<uint8_t, 32> HELLO_RETRY_REQUEST{0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11,
															  0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
															  0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e,
															  0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

		// The DER that wraps an Ed25519 public key as a SubjectPublicKeyInfo,
		// RFC 8410 §4. Forty-four bytes total, of which this is the twelve that
		// never change - an algorithm identifier for OID 1.3.101.112 and a bit
		// string header. Written out rather than produced by an encoder, because
		// there is exactly one shape and an encoder would be a second thing to
		// get wrong.
		constexpr std::array<uint8_t, 12> ED25519_SPKI_PREFIX{
			0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00
		};

		// The string a CertificateVerify signature covers, RFC 8446 §4.4.3. The
		// 64 spaces exist so that a signature made for TLS cannot be replayed as
		// one made for anything else, and the direction is in the text so a
		// server's signature is not a client's.
		constexpr std::string_view SERVER_SIGNATURE_CONTEXT = "TLS 1.3, server CertificateVerify";

		// Builds TLS's big-endian structures, with the length prefixes opened
		// and closed rather than counted in advance. A length computed ahead of
		// the bytes it describes is a length that is wrong the first time
		// somebody adds a field.
		struct Writer {
			std::vector<std::byte> Bytes;

			void U8(uint8_t value) {
				Bytes.push_back(static_cast<std::byte>(value));
			}

			void U16(uint16_t value) {
				U8(static_cast<uint8_t>(value >> 8));
				U8(static_cast<uint8_t>(value & 0xff));
			}

			void Raw(std::span<const std::byte> data) {
				Bytes.insert(Bytes.end(), data.begin(), data.end());
			}

			void Text(std::string_view text) {
				for (const char letter : text) {
					U8(static_cast<uint8_t>(letter));
				}
			}

			size_t Open(size_t width) {
				const size_t at = Bytes.size();
				for (size_t index = 0; index < width; index++) {
					U8(0);
				}
				return at;
			}

			void Close(size_t at, size_t width) {
				const size_t length = Bytes.size() - at - width;
				for (size_t index = 0; index < width; index++) {
					const auto shift = static_cast<unsigned>(8 * (width - 1 - index));
					Bytes[at + index] = static_cast<std::byte>((length >> shift) & 0xff);
				}
			}
		};

		// Reads TLS's big-endian structures. Every read is bounds-checked and a
		// failed reader stays failed, so a parser can be written straight
		// through and checked once at the end - the shape `Packet::Read` uses
		// and for the same reason.
		struct Reader {
			std::span<const std::byte> Bytes;
			size_t At = 0;
			bool Broken = false;

			size_t Left() const {
				return Broken ? 0 : Bytes.size() - At;
			}

			bool Done() const {
				return !Broken && At == Bytes.size();
			}

			uint8_t U8() {
				if (Left() < 1) {
					Broken = true;
					return 0;
				}
				return static_cast<uint8_t>(Bytes[At++]);
			}

			uint16_t U16() {
				const uint16_t high = U8();
				const uint16_t low = U8();
				return static_cast<uint16_t>((high << 8) | low);
			}

			std::span<const std::byte> Take(size_t length) {
				if (Left() < length) {
					Broken = true;
					return {};
				}
				const std::span<const std::byte> slice = Bytes.subspan(At, length);
				At += length;
				return slice;
			}

			// A length-prefixed block, which is most of TLS.
			std::span<const std::byte> Block(size_t width) {
				size_t length = 0;
				for (size_t index = 0; index < width; index++) {
					length = (length << 8) | U8();
				}
				return Take(length);
			}
		};

		// Random bytes, or a refusal.
		//
		// Protecting against one thing, and it is the one `Handshake::Begin`
		// protects against: the operating system refusing entropy - no
		// /dev/urandom inside a sandbox, or a handle exhausted. A fallback to a
		// weaker source would be a session anybody can decrypt, reported as a
		// success.
		bool RandomBytes(std::span<std::byte> out) {
			try {
				CryptoPP::OS_GenerateRandomBlock(
					false, reinterpret_cast<CryptoPP::byte *>(out.data()), out.size()
				);
			} catch (const CryptoPP::Exception &) {
				core::Metrics::Count("net.quic.tls.no_entropy", 1.0);
				return false;
			}
			return true;
		}

		std::span<const std::byte> AsBytes(std::span<const uint8_t> raw) {
			return {reinterpret_cast<const std::byte *>(raw.data()), raw.size()};
		}

		const CryptoPP::byte *Raw(std::span<const std::byte> bytes) {
			return reinterpret_cast<const CryptoPP::byte *>(bytes.data());
		}

		// Walks an extension block, calling `visit(type, body)` for each.
		//
		// @return `false` when the block is malformed.
		template <typename Visit> bool ForEachExtension(std::span<const std::byte> block, Visit visit) {
			Reader reader{block};
			while (!reader.Done()) {
				const uint16_t type = reader.U16();
				const std::span<const std::byte> body = reader.Block(2);
				if (reader.Broken) {
					return false;
				}
				if (!visit(type, body)) {
					return false;
				}
			}
			return !reader.Broken;
		}

		// The header a handshake message carries: a type and a 24-bit length.
		constexpr size_t MESSAGE_HEADER_BYTES = 4;
	}

	const char *Describe(Level level) {
		switch (level) {
		case Level::Initial:
			return "initial";
		case Level::Handshake:
			return "handshake";
		case Level::Application:
			return "application";
		}
		return "unknown";
	}

	std::array<std::byte, IDENTITY_BYTES> IdentityFor(std::span<const std::byte> seed) {
		std::array<std::byte, IDENTITY_BYTES> identity{};
		if (seed.size() != IDENTITY_SEED_BYTES) {
			return identity;
		}
		// Donna takes the public key first and the seed second, which is the
		// opposite of how the call reads aloud.
		CryptoPP::Donna::ed25519_publickey(reinterpret_cast<CryptoPP::byte *>(identity.data()), Raw(seed));
		return identity;
	}

	Tls::Tls(Role role, TlsSettings settings) : Side(role), Settings(std::move(settings)) {
		if (Side == Role::Server && !Settings.HasSeed) {
			// A server with no identity cannot sign the transcript, which means
			// it cannot authenticate itself, which means the pinning D00006
			// exists for would be a check with nothing to check against. Refused
			// here rather than three messages in.
			Refuse(ALERT_INTERNAL_ERROR, "a server needs an identity seed");
			return;
		}
		if (Settings.Protocol.empty()) {
			Refuse(ALERT_INTERNAL_ERROR, "a handshake needs an application protocol");
			return;
		}
		if (Side == Role::Server) {
			Identity = IdentityFor(Settings.Seed);
			HasIdentity = true;
		}
	}

	Tls::~Tls() {
		Wipe();
	}

	Tls::Tls(Tls &&other) noexcept
		: Side(other.Side), Phase(other.Phase), Settings(std::move(other.Settings)), Chosen(other.Chosen),
		  ChosenHeader(other.ChosenHeader), EphemeralSecret(other.EphemeralSecret),
		  EphemeralPublic(other.EphemeralPublic), PeerShare(other.PeerShare), Identity(other.Identity),
		  HasIdentity(other.HasIdentity), Transcript(std::move(other.Transcript)),
		  HandshakeSecret(other.HandshakeSecret), ClientHandshakeSecret(other.ClientHandshakeSecret),
		  ServerHandshakeSecret(other.ServerHandshakeSecret),
		  ClientApplicationSecret(other.ClientApplicationSecret),
		  ServerApplicationSecret(other.ServerApplicationSecret),
		  LocalParameters(std::move(other.LocalParameters)),
		  RemoteParameters(std::move(other.RemoteParameters)), Incoming(std::move(other.Incoming)),
		  Events(std::move(other.Events)), AlertCode(other.AlertCode), Reason(other.Reason) {
		// The source is wiped and retired rather than merely left alone, for the
		// reason `Cipher::Sealer` and `assets::SigningKey` both are: a moved-from
		// object whose private key is still in its storage is a copy of that key
		// nobody believes exists.
		other.Wipe();
		other.Phase = Stage::Failed;
	}

	Tls &Tls::operator=(Tls &&other) noexcept {
		if (this != &other) {
			Wipe();
			Side = other.Side;
			Phase = other.Phase;
			Settings = std::move(other.Settings);
			Chosen = other.Chosen;
			ChosenHeader = other.ChosenHeader;
			EphemeralSecret = other.EphemeralSecret;
			EphemeralPublic = other.EphemeralPublic;
			PeerShare = other.PeerShare;
			Identity = other.Identity;
			HasIdentity = other.HasIdentity;
			Transcript = std::move(other.Transcript);
			HandshakeSecret = other.HandshakeSecret;
			ClientHandshakeSecret = other.ClientHandshakeSecret;
			ServerHandshakeSecret = other.ServerHandshakeSecret;
			ClientApplicationSecret = other.ClientApplicationSecret;
			ServerApplicationSecret = other.ServerApplicationSecret;
			LocalParameters = std::move(other.LocalParameters);
			RemoteParameters = std::move(other.RemoteParameters);
			Incoming = std::move(other.Incoming);
			Events = std::move(other.Events);
			AlertCode = other.AlertCode;
			Reason = other.Reason;
			other.Wipe();
			other.Phase = Stage::Failed;
		}
		return *this;
	}

	void Tls::Wipe() {
		core::SecureWipe(EphemeralSecret);
		core::SecureWipe(Settings.Seed);
		core::SecureWipe(HandshakeSecret);
		core::SecureWipe(ClientHandshakeSecret);
		core::SecureWipe(ServerHandshakeSecret);
		core::SecureWipe(ClientApplicationSecret);
		core::SecureWipe(ServerApplicationSecret);
	}

	bool Tls::Refuse(uint8_t alert, const char *reason) {
		if (Phase != Stage::Failed) {
			Phase = Stage::Failed;
			AlertCode = alert;
			Reason = reason;
			// **A refusal here is a relay, a downgrade attempt, or a peer that is
			// not the one this end pinned**, and none of those is an ordinary
			// event. The counter is deliberately one an operator can alarm on,
			// the same argument `assets::VerifySessionTranscript` makes.
			core::Metrics::Count("net.quic.tls.refused", 1.0);
		}
		return false;
	}

	void Tls::ClearPending() {
		Events.clear();
	}

	std::span<const std::byte> Tls::PeerIdentity() const {
		if (Side == Role::Server || !HasIdentity) {
			return {};
		}
		return Identity;
	}

	void Tls::SetTransportParameters(std::span<const std::byte> parameters) {
		LocalParameters.assign(parameters.begin(), parameters.end());
	}

	void Tls::Absorb(std::span<const std::byte> message) {
		Transcript.insert(Transcript.end(), message.begin(), message.end());
	}

	std::array<std::byte, SECRET_BYTES> Tls::TranscriptHash() const {
		return Digest(Transcript);
	}

	std::array<std::byte, SECRET_BYTES>
	Tls::DeriveSecret(std::span<const std::byte> secret, std::string_view label) const {
		std::array<std::byte, SECRET_BYTES> out{};
		ExpandLabel(secret, label, TranscriptHash(), out);
		return out;
	}

	std::array<std::byte, SECRET_BYTES> Tls::FinishedTag(std::span<const std::byte> base) const {
		// RFC 8446 §4.4.4. The key is derived once per direction and the tag is
		// an HMAC over the transcript *before* the Finished message itself, which
		// is why every caller computes this before absorbing.
		std::array<std::byte, SECRET_BYTES> key{};
		ExpandLabel(base, "finished", key);
		const auto tag = Hmac(key, TranscriptHash());
		core::SecureWipe(key);
		return tag;
	}

	bool Tls::DeriveHandshakeSecrets() {
		std::array<std::byte, 32> shared{};
		if (CryptoPP::Donna::curve25519_mult(
				reinterpret_cast<CryptoPP::byte *>(shared.data()), Raw(EphemeralSecret), Raw(PeerShare)
			) != 0) {
			// Donna answers non-zero when the agreement is the all-zero value,
			// which is what a small-order public key produces. A peer sending one
			// is choosing the shared secret for both ends.
			return Refuse(ALERT_ILLEGAL_PARAMETER, "the peer's key share is degenerate");
		}

		// RFC 8446 §7.1's schedule. The early secret has no pre-shared key in it
		// because this offers none, so both inputs are the zeros the RFC names.
		const std::array<std::byte, SECRET_BYTES> zeros{};
		const auto early = Extract({}, zeros);

		std::array<std::byte, SECRET_BYTES> derived{};
		ExpandLabel(early, "derived", Digest({}), derived);
		HandshakeSecret = Extract(derived, shared);

		ClientHandshakeSecret = DeriveSecret(HandshakeSecret, "c hs traffic");
		ServerHandshakeSecret = DeriveSecret(HandshakeSecret, "s hs traffic");

		core::SecureWipe(shared);
		core::SecureWipe(derived);
		return true;
	}

	void Tls::DeriveApplicationSecrets() {
		const std::array<std::byte, SECRET_BYTES> zeros{};
		std::array<std::byte, SECRET_BYTES> derived{};
		ExpandLabel(HandshakeSecret, "derived", Digest({}), derived);
		const auto master = Extract(derived, zeros);

		ClientApplicationSecret = DeriveSecret(master, "c ap traffic");
		ServerApplicationSecret = DeriveSecret(master, "s ap traffic");
		core::SecureWipe(derived);
	}

	// --- the client's opening move ------------------------------------------

	bool Tls::Begin() {
		ENGINE_PROFILE_CAT("quic::Tls::Begin", core::ProfileCategory::Network);

		if (Side != Role::Client || Phase != Stage::Fresh) {
			return Refuse(ALERT_INTERNAL_ERROR, "only a fresh client opens a handshake");
		}
		if (LocalParameters.empty()) {
			return Refuse(ALERT_INTERNAL_ERROR, "the transport parameters were not set");
		}

		std::array<std::byte, 32> random{};
		if (!RandomBytes(random) || !RandomBytes(EphemeralSecret)) {
			return Refuse(ALERT_INTERNAL_ERROR, "the operating system refused entropy");
		}
		if (CryptoPP::Donna::curve25519_mult(
				reinterpret_cast<CryptoPP::byte *>(EphemeralPublic.data()), Raw(EphemeralSecret)
			) != 0) {
			return Refuse(ALERT_INTERNAL_ERROR, "the ephemeral key would not multiply");
		}

		Writer message;
		message.U8(CLIENT_HELLO);
		const size_t body = message.Open(3);

		message.U16(VERSION_LEGACY);
		message.Raw(random);

		// **Zero-length, and RFC 9001 §8.4 requires it.** The session id is
		// TLS-over-TCP's middlebox compatibility trick; QUIC has no middlebox to
		// fool and a non-empty one is a protocol violation.
		message.U8(0);

		const size_t suites = message.Open(2);
		message.U16(SUITE_CHACHA20_POLY1305_SHA256);
		message.U16(SUITE_AES_128_GCM_SHA256);
		message.Close(suites, 2);

		// One compression method, `null`. TLS 1.3 removed compression; the field
		// survives as a constant.
		message.U8(1);
		message.U8(0);

		const size_t extensions = message.Open(2);

		message.U16(EXTENSION_SUPPORTED_VERSIONS);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(1);
			message.U16(VERSION_TLS_1_3);
			message.Close(list, 1);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_SUPPORTED_GROUPS);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(2);
			message.U16(GROUP_X25519);
			message.Close(list, 2);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_KEY_SHARE);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(2);
			message.U16(GROUP_X25519);
			const size_t share = message.Open(2);
			message.Raw(EphemeralPublic);
			message.Close(share, 2);
			message.Close(list, 2);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_SIGNATURE_ALGORITHMS);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(2);
			message.U16(SCHEME_ED25519);
			message.Close(list, 2);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_SERVER_CERTIFICATE_TYPE);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(1);
			message.U8(CERTIFICATE_TYPE_RAW_PUBLIC_KEY);
			message.Close(list, 1);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_ALPN);
		{
			const size_t at = message.Open(2);
			const size_t list = message.Open(2);
			const size_t name = message.Open(1);
			message.Text(Settings.Protocol);
			message.Close(name, 1);
			message.Close(list, 2);
			message.Close(at, 2);
		}

		message.U16(EXTENSION_QUIC_TRANSPORT_PARAMETERS);
		{
			const size_t at = message.Open(2);
			message.Raw(LocalParameters);
			message.Close(at, 2);
		}

		message.Close(extensions, 2);
		message.Close(body, 3);

		Absorb(message.Bytes);
		Events.push_back({Event::Kind::Send, Level::Initial, std::move(message.Bytes), {}});
		Phase = Stage::AwaitingServerHello;
		return true;
	}

	// --- taking bytes off the CRYPTO stream ---------------------------------

	bool Tls::Receive(Level level, std::span<const std::byte> data) {
		ENGINE_PROFILE_CAT("quic::Tls::Receive", core::ProfileCategory::Network);

		if (Phase == Stage::Failed) {
			return false;
		}
		auto &buffer = Incoming[static_cast<size_t>(level)];
		buffer.insert(buffer.end(), data.begin(), data.end());
		return ConsumeMessages(level);
	}

	bool Tls::ConsumeMessages(Level level) {
		auto &buffer = Incoming[static_cast<size_t>(level)];

		size_t consumed = 0;
		while (buffer.size() - consumed >= MESSAGE_HEADER_BYTES) {
			const std::span<const std::byte> rest = std::span<const std::byte>(buffer).subspan(consumed);
			const auto type = static_cast<uint8_t>(rest[0]);
			const size_t length = (static_cast<size_t>(rest[1]) << 16) | (static_cast<size_t>(rest[2]) << 8) |
								  static_cast<size_t>(rest[3]);
			if (rest.size() < MESSAGE_HEADER_BYTES + length) {
				// A CRYPTO frame splits wherever the sender's packet ended, so a
				// partial message is ordinary rather than an error. Hold it.
				break;
			}

			const std::span<const std::byte> whole = rest.subspan(0, MESSAGE_HEADER_BYTES + length);
			const std::span<const std::byte> body = whole.subspan(MESSAGE_HEADER_BYTES);
			consumed += whole.size();

			bool handled = false;
			switch (type) {
			case CLIENT_HELLO:
				handled = Side == Role::Server && Phase == Stage::Fresh && level == Level::Initial &&
						  OnClientHello(body, whole);
				break;
			case SERVER_HELLO:
				handled = Side == Role::Client && Phase == Stage::AwaitingServerHello &&
						  level == Level::Initial && OnServerHello(body, whole);
				break;
			case ENCRYPTED_EXTENSIONS:
				handled = Side == Role::Client && Phase == Stage::AwaitingServerHandshake &&
						  level == Level::Handshake && OnEncryptedExtensions(body, whole);
				break;
			case CERTIFICATE:
				handled = Side == Role::Client && Phase == Stage::AwaitingServerHandshake &&
						  level == Level::Handshake && OnCertificate(body, whole);
				break;
			case CERTIFICATE_VERIFY:
				handled = Side == Role::Client && Phase == Stage::AwaitingServerHandshake &&
						  level == Level::Handshake && OnCertificateVerify(body, whole);
				break;
			case FINISHED:
				handled = level == Level::Handshake && OnFinished(body, whole);
				break;
			default:
				break;
			}

			if (!handled) {
				if (Phase != Stage::Failed) {
					// A message that is a real TLS type arriving at the wrong
					// level or in the wrong order is exactly what
					// `unexpected_message` is for, and it is refused rather than
					// skipped: a handshake that ignores what it did not expect is
					// one an attacker can steer.
					Refuse(ALERT_UNEXPECTED_MESSAGE, "a handshake message arrived out of order");
				}
				return false;
			}
		}

		buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
		return true;
	}

	// --- the server's side ---------------------------------------------------

	bool Tls::OnClientHello(std::span<const std::byte> body, std::span<const std::byte> whole) {
		if (LocalParameters.empty()) {
			return Refuse(ALERT_INTERNAL_ERROR, "the transport parameters were not set");
		}

		Reader reader{body};
		if (reader.U16() != VERSION_LEGACY) {
			return Refuse(ALERT_PROTOCOL_VERSION, "the legacy version is not 1.2");
		}
		reader.Take(32);
		if (!reader.Block(1).empty()) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "a QUIC ClientHello carries no session id");
		}

		const std::span<const std::byte> suites = reader.Block(2);
		const std::span<const std::byte> compression = reader.Block(1);
		const std::span<const std::byte> extensions = reader.Block(2);
		if (reader.Broken || !reader.Done()) {
			return Refuse(ALERT_DECODE_ERROR, "the ClientHello is malformed");
		}
		if (compression.size() != 1 || compression[0] != std::byte{0}) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "compression is not offered");
		}

		// **ChaCha20-Poly1305 first when it is offered.** It is the engine's own
		// suite for the reason `Cipher.hpp` gives - constant time on hardware
		// with no AES instructions, which is a phone - and the preference is this
		// end's rather than the client's, which is how TLS 1.3 works and is the
		// half that stops a client steering a server onto a weaker choice.
		bool hasChaCha = false;
		bool hasAes = false;
		{
			Reader offered{suites};
			while (!offered.Done()) {
				const uint16_t suite = offered.U16();
				hasChaCha = hasChaCha || suite == SUITE_CHACHA20_POLY1305_SHA256;
				hasAes = hasAes || suite == SUITE_AES_128_GCM_SHA256;
			}
			if (offered.Broken) {
				return Refuse(ALERT_DECODE_ERROR, "the cipher suite list is malformed");
			}
		}
		if (hasChaCha) {
			Chosen = Aead::ChaCha20Poly1305;
			ChosenHeader = HeaderCipher::ChaCha20;
		} else if (hasAes) {
			Chosen = Aead::Aes128Gcm;
			ChosenHeader = HeaderCipher::Aes128;
		} else {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "no cipher suite in common");
		}

		bool sawVersion = false;
		bool sawShare = false;
		bool sawProtocol = false;
		bool sawRawKeys = false;
		bool sawSignature = false;
		bool malformed = false;
		std::vector<std::byte> peerParameters;

		const bool walked = ForEachExtension(extensions, [&](uint16_t type, std::span<const std::byte> ext) {
			Reader inner{ext};
			switch (type) {
			case EXTENSION_SUPPORTED_VERSIONS: {
				Reader list{inner.Block(1)};
				while (!list.Done()) {
					sawVersion = sawVersion || list.U16() == VERSION_TLS_1_3;
				}
				malformed = malformed || list.Broken;
				break;
			}
			case EXTENSION_KEY_SHARE: {
				Reader list{inner.Block(2)};
				while (!list.Done()) {
					const uint16_t group = list.U16();
					const std::span<const std::byte> share = list.Block(2);
					if (group == GROUP_X25519 && share.size() == PeerShare.size() && !sawShare) {
						std::memcpy(PeerShare.data(), share.data(), PeerShare.size());
						sawShare = true;
					}
				}
				malformed = malformed || list.Broken;
				break;
			}
			case EXTENSION_ALPN: {
				Reader list{inner.Block(2)};
				while (!list.Done()) {
					const std::span<const std::byte> name = list.Block(1);
					const std::string_view text(reinterpret_cast<const char *>(name.data()), name.size());
					sawProtocol = sawProtocol || text == Settings.Protocol;
				}
				malformed = malformed || list.Broken;
				break;
			}
			case EXTENSION_SERVER_CERTIFICATE_TYPE: {
				Reader list{inner.Block(1)};
				while (!list.Done()) {
					sawRawKeys = sawRawKeys || list.U8() == CERTIFICATE_TYPE_RAW_PUBLIC_KEY;
				}
				malformed = malformed || list.Broken;
				break;
			}
			case EXTENSION_SIGNATURE_ALGORITHMS: {
				Reader list{inner.Block(2)};
				while (!list.Done()) {
					sawSignature = sawSignature || list.U16() == SCHEME_ED25519;
				}
				malformed = malformed || list.Broken;
				break;
			}
			case EXTENSION_QUIC_TRANSPORT_PARAMETERS:
				peerParameters.assign(ext.begin(), ext.end());
				break;
			default:
				// Unknown extensions are ignored, which TLS requires and which
				// is the one place Postel's rule survives here: a peer that
				// offers something this end has never heard of is not an
				// attacker, and refusing would make every future extension a
				// flag day.
				break;
			}
			return true;
		});

		if (!walked || malformed) {
			return Refuse(ALERT_DECODE_ERROR, "an extension is malformed");
		}
		if (!sawVersion) {
			return Refuse(ALERT_PROTOCOL_VERSION, "the client did not offer TLS 1.3");
		}
		if (!sawShare) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the client offered no X25519 share");
		}
		if (!sawSignature) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the client does not accept Ed25519");
		}
		if (!sawRawKeys) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the client does not accept raw public keys");
		}
		if (!sawProtocol) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the client offered another application protocol");
		}
		if (peerParameters.empty()) {
			return Refuse(ALERT_MISSING_EXTENSION, "the client sent no transport parameters");
		}

		Absorb(whole);

		std::array<std::byte, 32> random{};
		if (!RandomBytes(random) || !RandomBytes(EphemeralSecret)) {
			return Refuse(ALERT_INTERNAL_ERROR, "the operating system refused entropy");
		}
		if (CryptoPP::Donna::curve25519_mult(
				reinterpret_cast<CryptoPP::byte *>(EphemeralPublic.data()), Raw(EphemeralSecret)
			) != 0) {
			return Refuse(ALERT_INTERNAL_ERROR, "the ephemeral key would not multiply");
		}

		Writer hello;
		hello.U8(SERVER_HELLO);
		const size_t helloBody = hello.Open(3);
		hello.U16(VERSION_LEGACY);
		hello.Raw(random);
		hello.U8(0);
		hello.U16(
			Chosen == Aead::ChaCha20Poly1305 ? SUITE_CHACHA20_POLY1305_SHA256 : SUITE_AES_128_GCM_SHA256
		);
		hello.U8(0);
		{
			const size_t extensionsAt = hello.Open(2);

			hello.U16(EXTENSION_SUPPORTED_VERSIONS);
			{
				const size_t at = hello.Open(2);
				hello.U16(VERSION_TLS_1_3);
				hello.Close(at, 2);
			}

			hello.U16(EXTENSION_KEY_SHARE);
			{
				const size_t at = hello.Open(2);
				hello.U16(GROUP_X25519);
				const size_t share = hello.Open(2);
				hello.Raw(EphemeralPublic);
				hello.Close(share, 2);
				hello.Close(at, 2);
			}

			hello.Close(extensionsAt, 2);
		}
		hello.Close(helloBody, 3);

		Absorb(hello.Bytes);
		Events.push_back({Event::Kind::Send, Level::Initial, std::move(hello.Bytes), {}});

		if (!DeriveHandshakeSecrets()) {
			return false;
		}

		// **The client's parameters are acted on now and the server's own are
		// not held back**, which is the asymmetry `Event::Kind::PeerParameters`
		// documents: a server has no identity to check the client against, so
		// there is no later point that would tell it anything more.
		RemoteParameters = std::move(peerParameters);
		Events.push_back({Event::Kind::PeerParameters, Level::Handshake, RemoteParameters, {}});
		Events.push_back({Event::Kind::ReadKey, Level::Handshake, {}, ClientHandshakeSecret});
		Events.push_back({Event::Kind::WriteKey, Level::Handshake, {}, ServerHandshakeSecret});

		// EncryptedExtensions, Certificate, CertificateVerify and Finished go out
		// together. One `Send` rather than four, because they are one flight and
		// a caller splitting them across CRYPTO frames buys nothing.
		Writer flight;

		flight.U8(ENCRYPTED_EXTENSIONS);
		{
			const size_t at = flight.Open(3);
			const size_t extensionsAt = flight.Open(2);

			flight.U16(EXTENSION_ALPN);
			{
				const size_t ext = flight.Open(2);
				const size_t list = flight.Open(2);
				const size_t name = flight.Open(1);
				flight.Text(Settings.Protocol);
				flight.Close(name, 1);
				flight.Close(list, 2);
				flight.Close(ext, 2);
			}

			flight.U16(EXTENSION_SERVER_CERTIFICATE_TYPE);
			{
				const size_t ext = flight.Open(2);
				flight.U8(CERTIFICATE_TYPE_RAW_PUBLIC_KEY);
				flight.Close(ext, 2);
			}

			flight.U16(EXTENSION_QUIC_TRANSPORT_PARAMETERS);
			{
				const size_t ext = flight.Open(2);
				flight.Raw(LocalParameters);
				flight.Close(ext, 2);
			}

			flight.Close(extensionsAt, 2);
			flight.Close(at, 3);
		}
		Absorb(std::span<const std::byte>(flight.Bytes));

		const size_t certificateFrom = flight.Bytes.size();
		flight.U8(CERTIFICATE);
		{
			const size_t at = flight.Open(3);
			// No request context: this certificate answers the handshake rather
			// than a CertificateRequest, and RFC 8446 §4.4.2 says it is empty.
			flight.U8(0);
			const size_t list = flight.Open(3);
			const size_t entry = flight.Open(3);
			flight.Raw(AsBytes(std::span<const uint8_t>(ED25519_SPKI_PREFIX)));
			flight.Raw(Identity);
			flight.Close(entry, 3);
			// Per-certificate extensions, of which there are none.
			flight.U16(0);
			flight.Close(list, 3);
			flight.Close(at, 3);
		}
		Absorb(std::span<const std::byte>(flight.Bytes).subspan(certificateFrom));

		// The signature covers the transcript through the Certificate, which is
		// what binds the identity to this connection's key exchange rather than
		// to any connection the same key ever made.
		std::vector<std::byte> content;
		content.assign(64, std::byte{0x20});
		for (const char letter : SERVER_SIGNATURE_CONTEXT) {
			content.push_back(static_cast<std::byte>(letter));
		}
		content.push_back(std::byte{0});
		{
			const auto hash = TranscriptHash();
			content.insert(content.end(), hash.begin(), hash.end());
		}

		std::array<std::byte, IDENTITY_SIGNATURE_BYTES> signature{};
		CryptoPP::Donna::ed25519_sign(
			Raw(content),
			content.size(),
			Raw(Settings.Seed),
			Raw(Identity),
			reinterpret_cast<CryptoPP::byte *>(signature.data())
		);

		const size_t verifyFrom = flight.Bytes.size();
		flight.U8(CERTIFICATE_VERIFY);
		{
			const size_t at = flight.Open(3);
			flight.U16(SCHEME_ED25519);
			const size_t bytes = flight.Open(2);
			flight.Raw(signature);
			flight.Close(bytes, 2);
			flight.Close(at, 3);
		}
		Absorb(std::span<const std::byte>(flight.Bytes).subspan(verifyFrom));

		const auto tag = FinishedTag(ServerHandshakeSecret);
		const size_t finishedFrom = flight.Bytes.size();
		flight.U8(FINISHED);
		{
			const size_t at = flight.Open(3);
			flight.Raw(tag);
			flight.Close(at, 3);
		}
		Absorb(std::span<const std::byte>(flight.Bytes).subspan(finishedFrom));

		Events.push_back({Event::Kind::Send, Level::Handshake, std::move(flight.Bytes), {}});

		// Both 1-RTT secrets come out of the transcript through the server's own
		// Finished - the client's Finished is not in it - so a server can install
		// both before the client has answered. The read key is installed early on
		// purpose: a reordered network can deliver a client's first 1-RTT packet
		// ahead of its Finished.
		DeriveApplicationSecrets();
		Events.push_back({Event::Kind::ReadKey, Level::Application, {}, ClientApplicationSecret});
		Events.push_back({Event::Kind::WriteKey, Level::Application, {}, ServerApplicationSecret});

		Phase = Stage::AwaitingClientFinished;
		return true;
	}

	// --- the client's side ---------------------------------------------------

	bool Tls::OnServerHello(std::span<const std::byte> body, std::span<const std::byte> whole) {
		Reader reader{body};
		if (reader.U16() != VERSION_LEGACY) {
			return Refuse(ALERT_PROTOCOL_VERSION, "the legacy version is not 1.2");
		}

		const std::span<const std::byte> random = reader.Take(32);
		if (reader.Broken) {
			return Refuse(ALERT_DECODE_ERROR, "the ServerHello is malformed");
		}
		if (SameBytes(random, AsBytes(std::span<const uint8_t>(HELLO_RETRY_REQUEST)))) {
			// One group is offered, so there is nothing a second ClientHello
			// could carry that the first did not. Refused rather than looped.
			return Refuse(ALERT_HANDSHAKE_FAILURE, "a HelloRetryRequest has nothing to retry with");
		}

		if (!reader.Block(1).empty()) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "a QUIC ServerHello echoes no session id");
		}

		const uint16_t suite = reader.U16();
		if (suite == SUITE_CHACHA20_POLY1305_SHA256) {
			Chosen = Aead::ChaCha20Poly1305;
			ChosenHeader = HeaderCipher::ChaCha20;
		} else if (suite == SUITE_AES_128_GCM_SHA256) {
			Chosen = Aead::Aes128Gcm;
			ChosenHeader = HeaderCipher::Aes128;
		} else {
			// A suite this end did not offer. Refused rather than accepted,
			// which is the check that stops a server picking something weaker
			// than what was on the table.
			return Refuse(ALERT_ILLEGAL_PARAMETER, "the server chose a suite that was not offered");
		}

		if (reader.U8() != 0) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "compression is not offered");
		}

		const std::span<const std::byte> extensions = reader.Block(2);
		if (reader.Broken || !reader.Done()) {
			return Refuse(ALERT_DECODE_ERROR, "the ServerHello is malformed");
		}

		bool sawVersion = false;
		bool sawShare = false;
		bool malformed = false;
		const bool walked = ForEachExtension(extensions, [&](uint16_t type, std::span<const std::byte> ext) {
			Reader inner{ext};
			switch (type) {
			case EXTENSION_SUPPORTED_VERSIONS:
				sawVersion = inner.U16() == VERSION_TLS_1_3;
				malformed = malformed || inner.Broken;
				break;
			case EXTENSION_KEY_SHARE: {
				const uint16_t group = inner.U16();
				const std::span<const std::byte> share = inner.Block(2);
				if (group == GROUP_X25519 && share.size() == PeerShare.size()) {
					std::memcpy(PeerShare.data(), share.data(), PeerShare.size());
					sawShare = true;
				}
				malformed = malformed || inner.Broken;
				break;
			}
			default:
				break;
			}
			return true;
		});

		if (!walked || malformed) {
			return Refuse(ALERT_DECODE_ERROR, "an extension is malformed");
		}
		if (!sawVersion) {
			return Refuse(ALERT_PROTOCOL_VERSION, "the server did not select TLS 1.3");
		}
		if (!sawShare) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the server sent no X25519 share");
		}

		Absorb(whole);
		if (!DeriveHandshakeSecrets()) {
			return false;
		}

		Events.push_back({Event::Kind::ReadKey, Level::Handshake, {}, ServerHandshakeSecret});
		Events.push_back({Event::Kind::WriteKey, Level::Handshake, {}, ClientHandshakeSecret});
		Phase = Stage::AwaitingServerHandshake;
		return true;
	}

	bool Tls::OnEncryptedExtensions(std::span<const std::byte> body, std::span<const std::byte> whole) {
		if (!RemoteParameters.empty()) {
			return Refuse(ALERT_UNEXPECTED_MESSAGE, "EncryptedExtensions arrived twice");
		}

		Reader reader{body};
		const std::span<const std::byte> extensions = reader.Block(2);
		if (reader.Broken || !reader.Done()) {
			return Refuse(ALERT_DECODE_ERROR, "EncryptedExtensions is malformed");
		}

		bool confirmedProtocol = false;
		bool confirmedRawKeys = false;
		bool malformed = false;
		std::vector<std::byte> parameters;

		const bool walked = ForEachExtension(extensions, [&](uint16_t type, std::span<const std::byte> ext) {
			Reader inner{ext};
			switch (type) {
			case EXTENSION_ALPN: {
				Reader list{inner.Block(2)};
				const std::span<const std::byte> name = list.Block(1);
				const std::string_view text(reinterpret_cast<const char *>(name.data()), name.size());
				confirmedProtocol = !list.Broken && text == Settings.Protocol;
				break;
			}
			case EXTENSION_SERVER_CERTIFICATE_TYPE:
				confirmedRawKeys = inner.U8() == CERTIFICATE_TYPE_RAW_PUBLIC_KEY;
				malformed = malformed || inner.Broken;
				break;
			case EXTENSION_QUIC_TRANSPORT_PARAMETERS:
				parameters.assign(ext.begin(), ext.end());
				break;
			default:
				break;
			}
			return true;
		});

		if (!walked || malformed) {
			return Refuse(ALERT_DECODE_ERROR, "an extension is malformed");
		}
		if (!confirmedProtocol) {
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the server did not confirm the protocol");
		}
		if (!confirmedRawKeys) {
			// Without this the server is free to answer with an X.509 chain,
			// which nothing here can build a trust path for - so the refusal is
			// the honest one rather than a certificate check that always passes.
			return Refuse(ALERT_HANDSHAKE_FAILURE, "the server did not confirm raw public keys");
		}
		if (parameters.empty()) {
			return Refuse(ALERT_MISSING_EXTENSION, "the server sent no transport parameters");
		}

		// Held rather than emitted. They are acted on when the signature below
		// says who wrote them - see `Event::Kind::PeerParameters`.
		RemoteParameters = std::move(parameters);
		Absorb(whole);
		return true;
	}

	bool Tls::OnCertificate(std::span<const std::byte> body, std::span<const std::byte> whole) {
		if (RemoteParameters.empty()) {
			return Refuse(ALERT_UNEXPECTED_MESSAGE, "a Certificate arrived before EncryptedExtensions");
		}
		if (HasIdentity) {
			return Refuse(ALERT_UNEXPECTED_MESSAGE, "a Certificate arrived twice");
		}

		Reader reader{body};
		if (!reader.Block(1).empty()) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "the certificate request context is not empty");
		}

		Reader list{reader.Block(3)};
		const std::span<const std::byte> entry = list.Block(3);
		list.Block(2);
		if (reader.Broken || list.Broken || !reader.Done()) {
			return Refuse(ALERT_DECODE_ERROR, "the Certificate is malformed");
		}

		// A raw public key, RFC 7250: the entry is a SubjectPublicKeyInfo rather
		// than a certificate, and for Ed25519 there is exactly one encoding of
		// one. Compared against the fixed prefix rather than parsed, because an
		// ASN.1 parser here would be attack surface for a structure with no
		// variability in it.
		const auto prefix = AsBytes(std::span<const uint8_t>(ED25519_SPKI_PREFIX));
		if (entry.size() != prefix.size() + IDENTITY_BYTES ||
			!SameBytes(entry.subspan(0, prefix.size()), prefix)) {
			return Refuse(ALERT_BAD_CERTIFICATE, "the key is not an Ed25519 SubjectPublicKeyInfo");
		}
		std::memcpy(Identity.data(), entry.data() + prefix.size(), IDENTITY_BYTES);
		HasIdentity = true;

		// **`D00006`'s pinning, and it happens here rather than above this
		// module.** An X25519 agreement with no identity bound to it is safe
		// against a listener and not against a relay, which can hold one exchange
		// with each side and read everything.
		if (Settings.PinIdentity && !SameBytes(Identity, Settings.Expected)) {
			core::Metrics::Count("net.quic.tls.identity_rejected", 1.0);
			return Refuse(ALERT_CERTIFICATE_UNKNOWN, "the server is not the one this client pinned");
		}

		Absorb(whole);
		return true;
	}

	bool Tls::OnCertificateVerify(std::span<const std::byte> body, std::span<const std::byte> whole) {
		if (!HasIdentity) {
			return Refuse(ALERT_UNEXPECTED_MESSAGE, "a CertificateVerify arrived before a Certificate");
		}

		Reader reader{body};
		if (reader.U16() != SCHEME_ED25519) {
			return Refuse(ALERT_ILLEGAL_PARAMETER, "the signature is not Ed25519");
		}
		const std::span<const std::byte> signature = reader.Block(2);
		if (reader.Broken || !reader.Done() || signature.size() != IDENTITY_SIGNATURE_BYTES) {
			return Refuse(ALERT_DECODE_ERROR, "the CertificateVerify is malformed");
		}

		std::vector<std::byte> content;
		content.assign(64, std::byte{0x20});
		for (const char letter : SERVER_SIGNATURE_CONTEXT) {
			content.push_back(static_cast<std::byte>(letter));
		}
		content.push_back(std::byte{0});
		{
			const auto hash = TranscriptHash();
			content.insert(content.end(), hash.begin(), hash.end());
		}

		// Donna answers 0 for a good signature, which is the C convention and the
		// opposite of what the name reads as. Inverted once, here - the same note
		// `assets::VerifyManifestRoot` carries.
		if (CryptoPP::Donna::ed25519_sign_open(Raw(content), content.size(), Raw(Identity), Raw(signature)) !=
			0) {
			return Refuse(ALERT_DECRYPT_ERROR, "the server's signature does not verify");
		}

		Absorb(whole);
		return true;
	}

	// --- the Finished on both sides -----------------------------------------

	bool Tls::OnFinished(std::span<const std::byte> body, std::span<const std::byte> whole) {
		const bool asClient = Side == Role::Client;
		if (asClient) {
			if (Phase != Stage::AwaitingServerHandshake || !HasIdentity) {
				return Refuse(ALERT_UNEXPECTED_MESSAGE, "a Finished arrived out of order");
			}
		} else if (Phase != Stage::AwaitingClientFinished) {
			return Refuse(ALERT_UNEXPECTED_MESSAGE, "a Finished arrived out of order");
		}

		const auto expected = FinishedTag(asClient ? ServerHandshakeSecret : ClientHandshakeSecret);
		if (!SameBytes(body, expected)) {
			// The one check that says the whole transcript both ends saw was the
			// same one. A mismatch is a rewritten handshake, not a dull error.
			return Refuse(ALERT_DECRYPT_ERROR, "the peer's Finished does not verify");
		}

		Absorb(whole);

		if (!asClient) {
			Phase = Stage::Done;
			Events.push_back({Event::Kind::Complete, Level::Application, {}, {}});
			core::Metrics::Count("net.quic.tls.completed", 1.0);
			return true;
		}

		// The application secrets come out of the transcript through the server's
		// Finished, which is now the last thing in it. The client's own Finished
		// goes out afterwards and is deliberately not part of them.
		DeriveApplicationSecrets();

		Events.push_back({Event::Kind::PeerParameters, Level::Application, RemoteParameters, {}});
		Events.push_back({Event::Kind::ReadKey, Level::Application, {}, ServerApplicationSecret});

		const auto tag = FinishedTag(ClientHandshakeSecret);
		Writer message;
		message.U8(FINISHED);
		const size_t at = message.Open(3);
		message.Raw(tag);
		message.Close(at, 3);
		Absorb(message.Bytes);
		Events.push_back({Event::Kind::Send, Level::Handshake, std::move(message.Bytes), {}});

		// The write key goes in after the bytes that must travel under the old
		// one, which is the ordering `Event` documents: the Finished above is a
		// Handshake-level message, and a 1-RTT key installed first would tempt a
		// caller into sending it at the wrong level.
		Events.push_back({Event::Kind::WriteKey, Level::Application, {}, ClientApplicationSecret});
		Events.push_back({Event::Kind::Complete, Level::Application, {}, {}});

		Phase = Stage::Done;
		core::Metrics::Count("net.quic.tls.completed", 1.0);
		return true;
	}
}
