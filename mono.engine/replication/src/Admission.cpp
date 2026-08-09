#include <engine/net/Packet.hpp>
#include <engine/replication/Admission.hpp>
#include <engine/replication/Protocol.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace engine::replication {

	namespace {
		void WriteFront(core::ByteWriter &writer, AdmissionKind kind) {
			writer.WriteUInt16(PROTOCOL_VERSION);
			writer.WriteUInt8(static_cast<uint8_t>(kind));
		}

		template <size_t N> void WriteBlock(core::ByteWriter &writer, const std::array<std::byte, N> &block) {
			writer.WriteRaw(block.data(), block.size());
		}

		template <size_t N> bool ReadBlock(core::ByteReader &reader, std::array<std::byte, N> &into) {
			return reader.ReadRaw(into.data(), into.size());
		}
	}

	const char *Describe(AdmissionKind kind) {
		switch (kind) {
		case AdmissionKind::Hello:
			return "hello";
		case AdmissionKind::Challenge:
			return "challenge";
		case AdmissionKind::Answer:
			return "answer";
		case AdmissionKind::Welcome:
			return "welcome";
		}
		return "?";
	}

	void WriteAdmission(core::ByteWriter &writer, const Hello &hello) {
		WriteFront(writer, AdmissionKind::Hello);
		WriteBlock(writer, hello.PublicKey);
	}

	void WriteAdmission(core::ByteWriter &writer, const Challenge &challenge) {
		WriteFront(writer, AdmissionKind::Challenge);
		WriteBlock(writer, challenge.Cookie);
	}

	void WriteAdmission(core::ByteWriter &writer, const Answer &answer) {
		WriteFront(writer, AdmissionKind::Answer);
		WriteBlock(writer, answer.PublicKey);
		WriteBlock(writer, answer.Cookie);
	}

	void WriteAdmission(core::ByteWriter &writer, const Welcome &welcome) {
		WriteFront(writer, AdmissionKind::Welcome);
		WriteBlock(writer, welcome.PublicKey);
		writer.WriteUInt64(welcome.Counter);
		WriteBlock(writer, welcome.Confirmation);
		WriteBlock(writer, welcome.Identity);
	}

	bool ReadAdmission(core::ByteReader &reader, Admission &message) {
		if (reader.ReadUInt16() != PROTOCOL_VERSION) {
			return false;
		}

		const uint8_t kind = reader.ReadUInt8();
		if (reader.Failed() || kind > static_cast<uint8_t>(AdmissionKind::Welcome)) {
			return false;
		}

		Admission read;
		read.Kind = static_cast<AdmissionKind>(kind);

		switch (read.Kind) {
		case AdmissionKind::Hello:
			if (!ReadBlock(reader, read.Hello.PublicKey)) {
				return false;
			}
			break;

		case AdmissionKind::Challenge:
			if (!ReadBlock(reader, read.Challenge.Cookie)) {
				return false;
			}
			break;

		case AdmissionKind::Answer:
			if (!ReadBlock(reader, read.Answer.PublicKey) || !ReadBlock(reader, read.Answer.Cookie)) {
				return false;
			}
			break;

		case AdmissionKind::Welcome:
			if (!ReadBlock(reader, read.Welcome.PublicKey)) {
				return false;
			}
			read.Welcome.Counter = reader.ReadUInt64();
			if (reader.Failed() || !ReadBlock(reader, read.Welcome.Confirmation)) {
				return false;
			}
			if (!ReadBlock(reader, read.Welcome.Identity)) {
				return false;
			}
			break;
		}

		// Fixed-size handshake messages must consume the entire datagram.
		if (!reader.AtEnd()) {
			return false;
		}

		message = read;
		return true;
	}

	bool FrameAdmission(core::ByteWriter &writer, std::span<const std::byte> payload) {
		net::PacketHeader header;
		header.Channel = net::ChannelKind::Handshake;

		return net::Packet::Write(writer, header, payload);
	}

	std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> AdmissionTranscript(
		std::span<const std::byte> clientKey,
		std::span<const std::byte> serverKey,
		std::span<const std::byte> cookie
	) {
		std::array<std::byte, 2 * net::Handshake::MESSAGE_BYTES + net::Cookie::COOKIE_BYTES> transcript{};

		const auto place = [&transcript](size_t offset, std::span<const std::byte> bytes, size_t width) {
			const size_t take = std::min(bytes.size(), width);
			if (take > 0) {
				std::memcpy(transcript.data() + offset, bytes.data(), take);
			}
		};

		place(0, clientKey, net::Handshake::MESSAGE_BYTES);
		place(net::Handshake::MESSAGE_BYTES, serverKey, net::Handshake::MESSAGE_BYTES);
		place(2 * net::Handshake::MESSAGE_BYTES, cookie, net::Cookie::COOKIE_BYTES);
		return transcript;
	}
}
