#include <cstring>
#include <discord/Frame.hpp>

namespace discord {

	namespace {
		// The five opcodes, as a question rather than a cast.
		//
		// A number off a socket is not an `Opcode` until something says it is,
		// and `static_cast` to an enum class with an unlisted value is exactly
		// the kind of quiet lie that turns a corrupt frame into a switch
		// falling through to whatever comes first.
		bool Known(uint32_t value) {
			return value <= static_cast<uint32_t>(Opcode::Pong);
		}

		// Little-endian, because that is what the protocol says rather than
		// what this machine happens to be.
		void PutU32(std::vector<std::byte> &into, uint32_t value) {
			into.push_back(static_cast<std::byte>(value & 0xFFu));
			into.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
			into.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
			into.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
		}

		uint32_t TakeU32(const std::byte *from) {
			return static_cast<uint32_t>(from[0]) | (static_cast<uint32_t>(from[1]) << 8) |
				   (static_cast<uint32_t>(from[2]) << 16) | (static_cast<uint32_t>(from[3]) << 24);
		}
	}

	std::vector<std::byte> EncodeFrame(Opcode op, std::string_view payload) {
		if (payload.size() + FRAME_HEADER_BYTES > MAXIMUM_FRAME_BYTES) {
			// Refused whole rather than truncated, for `engine::parallel`'s
			// reason: half a frame is worse than none, because the reader
			// cannot tell.
			return {};
		}

		std::vector<std::byte> frame;
		frame.reserve(FRAME_HEADER_BYTES + payload.size());
		PutU32(frame, static_cast<uint32_t>(op));
		PutU32(frame, static_cast<uint32_t>(payload.size()));
		for (const char letter : payload) {
			frame.push_back(static_cast<std::byte>(static_cast<unsigned char>(letter)));
		}
		return frame;
	}

	DecodeResult DecodeFrame(std::vector<std::byte> &buffer, DecodedFrame &out) {
		if (buffer.size() < FRAME_HEADER_BYTES) {
			return DecodeResult::Incomplete;
		}

		const uint32_t op = TakeU32(buffer.data());
		const uint32_t length = TakeU32(buffer.data() + 4);

		// **Checked before the length is trusted for anything.** This number
		// arrived from outside the process, and the whole point of the cap is
		// that it is applied before it is used to reserve, resize or index.
		if (!Known(op) || length > MAXIMUM_FRAME_BYTES - FRAME_HEADER_BYTES) {
			return DecodeResult::Corrupt;
		}

		const size_t whole = FRAME_HEADER_BYTES + length;
		if (buffer.size() < whole) {
			// Nothing consumed. The caller reads more and asks again, which is
			// what makes a frame split across two reads reassemble without any
			// state living between calls.
			return DecodeResult::Incomplete;
		}

		out.Op = static_cast<Opcode>(op);
		out.Payload.assign(reinterpret_cast<const char *>(buffer.data() + FRAME_HEADER_BYTES), length);
		buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(whole));
		return DecodeResult::Ok;
	}
}
