#pragma once

// @tier L12 · shared

#include <engine/assets/Signature.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/net/Cipher.hpp>
#include <engine/net/Cookie.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/Handshake.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace engine::replication {

	// Which of the four an admission message is.
	//
	// @since v0.3
	enum class AdmissionKind : uint8_t {
		Hello,

		Challenge,

		Answer,

		Welcome,
	};

	// Returns a stable, human-readable name for an admission message kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(AdmissionKind kind);

	// A client asking to connect.
	//
	// @since v0.3
	struct Hello {
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};
	};

	// A server asking the client to prove where it is.
	//
	// @since v0.3
	struct Challenge {
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A client answering the challenge.
	//
	// @since v0.3
	struct Answer {
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		// The cookie the server issued.
		std::array<std::byte, net::Cookie::COOKIE_BYTES> Cookie{};
	};

	// A server admitting the client, and proving the keys agree.
	//
	// @since v0.3
	struct Welcome {
		std::array<std::byte, net::Handshake::MESSAGE_BYTES> PublicKey{};

		uint64_t Counter = 0;

		std::array<std::byte, net::Cipher::TAG_BYTES> Confirmation{};

		// @since v0.9
		std::array<std::byte, assets::SignatureBytes::BYTES> Identity{};
	};

	// What a successful read produced.
	//
	// @since v0.3
	struct Admission {
		AdmissionKind Kind = AdmissionKind::Hello;

		replication::Hello Hello;

		replication::Challenge Challenge;

		replication::Answer Answer;

		replication::Welcome Welcome;
	};

	// Writes a hello.
	//
	// @param writer Where the bytes go.
	// @param hello  The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Hello &hello);

	// Writes a challenge.
	//
	// @param writer    Where the bytes go.
	// @param challenge The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Challenge &challenge);

	// Writes an answer.
	//
	// @param writer Where the bytes go.
	// @param answer The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Answer &answer);

	// Writes a welcome.
	//
	// @param writer  Where the bytes go.
	// @param welcome The message to write.
	// @since v0.3
	void WriteAdmission(core::ByteWriter &writer, const Welcome &welcome);

	// Reads an admission message, refusing anything that is not exactly one.
	//
	// @param reader  The bytes to parse.
	// @param message Filled in on success, untouched otherwise.
	// @return `false` on anything malformed. Drop it and count it.
	// @since v0.3
	bool ReadAdmission(core::ByteReader &reader, Admission &message);

	// Wraps an admission payload as a datagram on the handshake channel.
	//
	// @param writer  Where the datagram goes.
	// @param payload The encoded admission message.
	// @return `false` when the payload does not fit a packet, which for these
	//         fixed-size messages cannot happen and is checked anyway.
	// @since v0.3
	bool FrameAdmission(core::ByteWriter &writer, std::span<const std::byte> payload);

	// The transcript a `Welcome`'s tag is computed over.
	//
	// @param clientKey The client's key exchange message.
	// @param serverKey The server's key exchange message.
	// @param cookie    The cookie the answer carried.
	// @return The bytes to pass as associated data.
	// @since v0.3
	std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> AdmissionTranscript(
		std::span<const std::byte> clientKey,
		std::span<const std::byte> serverKey,
		std::span<const std::byte> cookie
	);

	// A peer that has answered the challenge and is asking to be let in.
	//
	// @since v0.3
	struct Applicant {
		net::Endpoint From;

		size_t Connected = 0;

		double NowSeconds = 0.0;
	};

	// Decides who is allowed to connect at all.
	//
	// @since v0.3
	using AdmissionPolicy = std::function<bool(const Applicant &)>;
}
