#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/world/PresentationStream.hpp>

#include <algorithm>
#include <limits>

namespace engine::world {
	namespace {
		constexpr uint32_t MAGIC = 0x31535041;
		constexpr size_t HEADER_BYTES = 20;
		constexpr PresentationLimits LIMITS;
		bool Decode(std::span<const std::byte> bytes, PresentationStreamFrame &frame) {
			core::ByteReader reader(bytes);
			frame.Kind = static_cast<PresentationStreamKind>(reader.ReadUInt8());
			bool valid = false;
			switch (frame.Kind) {
			case PresentationStreamKind::Directory:
				valid = ReadPresentationDirectory(reader, frame.Directory);
				break;
			case PresentationStreamKind::Routes:
				valid = ReadPresentationRoutes(reader, frame.Directory);
				break;
			case PresentationStreamKind::Message:
				valid = ReadPresentationMessage(reader, frame.Message);
				break;
			default:
				return false;
			}
			return valid && !reader.Failed() && reader.Remaining() == 0;
		}
	}
	PresentationStatus PresentationStream::Queue(const PresentationStreamFrame &frame) {
		if (!Live) return PresentationStatus::Invalid;
		if (SendSize.Messages >= LIMITS.Messages) return PresentationStatus::Full;
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(frame.Kind));
		bool valid = false;
		switch (frame.Kind) {
		case PresentationStreamKind::Directory:
			valid = WritePresentationDirectory(writer, frame.Directory);
			break;
		case PresentationStreamKind::Routes:
			valid = WritePresentationRoutes(writer, frame.Directory);
			break;
		case PresentationStreamKind::Message:
			valid = WritePresentationMessage(writer, frame.Message);
			break;
		default:
			break;
		}
		if (!valid || writer.Size() > MAXIMUM_FRAME_BYTES) return PresentationStatus::Invalid;
		if (writer.Size() > LIMITS.Bytes - SendSize.Bytes) return PresentationStatus::Full;
		Sending.emplace_back(writer.Bytes().begin(), writer.Bytes().end());
		SendSize.Messages++;
		SendSize.Bytes += writer.Size();
		core::Metrics::Count("world.presentation.stream.queued.bytes", static_cast<double>(writer.Size()));
		return PresentationStatus::Ok;
	}
	size_t PresentationStream::Flush(
		const std::function<bool(std::span<const std::byte>)> &send, size_t maximumPackets
	) {
		ENGINE_PROFILE("presentation stream send");
		if (!Live || !send) return 0;
		size_t packets = 0, frames = 0;
		while (frames < Sending.size() && packets < maximumPackets) {
			const auto &frame = Sending[frames];
			const auto count = std::min(PACKET_BYTES - HEADER_BYTES, frame.size() - SendingOffset);
			Packet.Clear();
			Packet.WriteUInt32(MAGIC);
			Packet.WriteUInt64(SendingSequence);
			Packet.WriteUInt32(static_cast<uint32_t>(frame.size()));
			Packet.WriteUInt32(static_cast<uint32_t>(SendingOffset));
			Packet.WriteRaw(frame.data() + SendingOffset, count);
			if (!send(Packet.Bytes())) break;
			core::Metrics::Count("world.presentation.stream.sent.bytes", static_cast<double>(Packet.Size()));
			core::Metrics::Count("world.presentation.stream.sent.packets", 1);
			SendingOffset += count;
			packets++;
			if (SendingOffset == frame.size()) {
				SendSize.Bytes -= frame.size();
				SendSize.Messages--;
				frames++;
				SendingOffset = 0;
				if (SendingSequence == std::numeric_limits<uint64_t>::max()) {
					Close();
					return packets;
				}
				SendingSequence++;
			}
		}
		Sending.erase(Sending.begin(), Sending.begin() + frames);
		return packets;
	}
	PresentationStreamReceive PresentationStream::Fail() {
		Refusals++;
		core::Metrics::Count("world.presentation.stream.refused", 1);
		Close();
		return PresentationStreamReceive::Refused;
	}
	PresentationStreamReceive PresentationStream::Receive(std::span<const std::byte> packet) {
		core::ByteReader reader(packet);
		if (packet.size() < sizeof(MAGIC) || reader.ReadUInt32() != MAGIC)
			return PresentationStreamReceive::Other;
		if (!Live || packet.size() <= HEADER_BYTES || packet.size() > PACKET_BYTES) return Fail();
		const auto sequence = reader.ReadUInt64();
		const auto total = reader.ReadUInt32();
		const auto offset = reader.ReadUInt32();
		const auto payload = packet.subspan(HEADER_BYTES);
		if (reader.Failed() || sequence != ReceivingSequence || total == 0 || total > MAXIMUM_FRAME_BYTES ||
			offset != Assembling.size() || offset > total || payload.size() > total - offset)
			return Fail();
		if (Assembling.empty()) {
			if (ReceivedSize.Messages >= LIMITS.Messages || total > LIMITS.Bytes - ReceivedSize.Bytes)
				return Fail();
			ReceivingBytes = total;
			ReceivedSize.Messages++;
			ReceivedSize.Bytes += total;
			Assembling.reserve(total);
		} else if (total != ReceivingBytes)
			return Fail();
		Assembling.insert(Assembling.end(), payload.begin(), payload.end());
		core::Metrics::Count("world.presentation.stream.received.bytes", static_cast<double>(packet.size()));
		core::Metrics::Count("world.presentation.stream.received.packets", 1);
		if (Assembling.size() == ReceivingBytes) {
			PresentationStreamFrame frame;
			if (!Decode(Assembling, frame)) return Fail();
			Received.push_back(std::move(frame));
			Assembling.clear();
			ReceivingBytes = 0;
			if (ReceivingSequence == std::numeric_limits<uint64_t>::max()) return Fail();
			ReceivingSequence++;
		}
		return PresentationStreamReceive::Accepted;
	}
	bool PresentationStream::Recognizes(std::span<const std::byte> packet) {
		core::ByteReader reader(packet);
		return packet.size() >= sizeof(MAGIC) && reader.ReadUInt32() == MAGIC;
	}
	std::vector<PresentationStreamFrame> PresentationStream::Take() {
		ReceivedSize = {ReceivingBytes, ReceivingBytes != 0 ? 1u : 0u};
		return std::exchange(Received, {});
	}
	void PresentationStream::Close() {
		Live = false;
		Sending.clear();
		Received.clear();
		std::vector<std::byte>{}.swap(Assembling);
		SendSize = {};
		ReceivedSize = {};
		ReceivingBytes = 0;
		SendingOffset = 0;
	}
}
