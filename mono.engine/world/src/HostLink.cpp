#include <engine/core/Log.hpp>
#include <engine/world/HostLink.hpp>

#include <utility>

namespace engine::world {

	namespace {
		// Recognises a frame before anything else is read.
		//
		// Both ends are the same binary, so this is not a compatibility check -
		// it is a check that the handle really is a link, because the cost of
		// finding out otherwise is a driver interpreting somebody's log file as
		// bus traffic.
		constexpr uint32_t FRAME_MAGIC = 0x4B4E'4C48u; // "HLNK"

		// The most envelopes one frame may carry.
		//
		// A bound on what a malformed count can make this allocate. Far above
		// any real barrier's traffic and far below anything that would hurt.
		constexpr uint32_t MAXIMUM_TRAFFIC = 1u << 20u;
	}

	const char *Describe(HostSignal signal) {
		switch (signal) {
		case HostSignal::Ready:
			return "ready";
		case HostSignal::Heartbeat:
			return "heartbeat";
		case HostSignal::Traffic:
			return "traffic";
		case HostSignal::Deliveries:
			return "deliveries";
		case HostSignal::Stop:
			return "stop";
		case HostSignal::Faulted:
			return "faulted";
		}
		// No default label, so adding a signal is a compiler warning here.
		return "?";
	}

	void WriteHostFrame(core::ByteWriter &writer, const HostFrame &frame) {
		writer.WriteUInt32(FRAME_MAGIC);
		writer.WriteUInt8(static_cast<uint8_t>(frame.Signal));
		writer.WriteName(frame.Host);
		writer.WriteName(frame.World);
		writer.WriteUInt64(frame.Tick);
		writer.WriteFloat(frame.Milliseconds);

		writer.WriteUInt32(static_cast<uint32_t>(frame.Traffic.size()));
		for (const Envelope &envelope : frame.Traffic) {
			WriteEnvelope(writer, envelope);
		}

		writer.WriteUInt32(static_cast<uint32_t>(frame.Deliveries.size()));
		for (const HostDelivery &delivery : frame.Deliveries) {
			writer.WriteName(delivery.World);
			WriteDelivery(writer, delivery.Message);
		}
	}

	bool ReadHostFrame(core::ByteReader &reader, HostFrame &frame) {
		if (reader.ReadUInt32() != FRAME_MAGIC) {
			return false;
		}

		HostFrame read;
		const auto signal = reader.ReadUInt8();
		if (signal > static_cast<uint8_t>(HostSignal::Faulted)) {
			return false;
		}
		read.Signal = static_cast<HostSignal>(signal);

		read.Host = reader.ReadName();
		read.World = reader.ReadName();
		read.Tick = reader.ReadUInt64();
		read.Milliseconds = reader.ReadFloat();

		const uint32_t count = reader.ReadUInt32();
		if (reader.Failed() || count > MAXIMUM_TRAFFIC) {
			return false;
		}

		read.Traffic.reserve(count);
		for (uint32_t index = 0; index < count; index++) {
			read.Traffic.push_back(ReadEnvelope(reader));
			if (reader.Failed()) {
				return false;
			}
		}

		const uint32_t deliveries = reader.ReadUInt32();
		if (reader.Failed() || deliveries > MAXIMUM_TRAFFIC) {
			return false;
		}

		read.Deliveries.reserve(deliveries);
		for (uint32_t index = 0; index < deliveries; index++) {
			HostDelivery delivery;
			delivery.World = reader.ReadName();
			delivery.Message = ReadDelivery(reader);
			if (reader.Failed()) {
				return false;
			}
			read.Deliveries.push_back(std::move(delivery));
		}

		if (reader.Failed()) {
			return false;
		}

		// Assigned last, so a frame that failed part-way leaves the caller's
		// object as it was rather than half-filled.
		frame = std::move(read);
		return true;
	}

	HostLink::HostLink(std::unique_ptr<parallel::Channel> channel, core::Name name)
		: Channel_(std::move(channel)), Name_(name) {}

	bool HostLink::Connected() const {
		return Channel_ != nullptr && Channel_->Open();
	}

	bool HostLink::Send(const HostFrame &frame) {
		if (Channel_ == nullptr) {
			return false;
		}

		HostFrame stamped = frame;
		if (!stamped.Host.IsValid()) {
			// Stamped here rather than trusted from the caller, the same way
			// the driver stamps `Envelope::From`: an endpoint does not get to
			// say who it is when everything downstream keys on the answer.
			stamped.Host = Name_;
		}

		core::ByteWriter writer;
		WriteHostFrame(writer, stamped);

		const parallel::ChannelStatus status = Channel_->Send(writer.Bytes());
		if (status == parallel::ChannelStatus::Ok) {
			return true;
		}

		Dropped_++;
		ENGINE_WARN(
			"host link '{}': dropped a {} frame: {}",
			Name_.Text(),
			Describe(frame.Signal),
			parallel::Describe(status)
		);
		return false;
	}

	bool HostLink::Heartbeat(uint64_t tick, float milliseconds) {
		HostFrame frame;
		frame.Signal = HostSignal::Heartbeat;
		frame.Tick = tick;
		frame.Milliseconds = milliseconds;
		return Send(frame);
	}

	bool HostLink::SendTraffic(std::span<const Envelope> traffic) {
		if (traffic.empty()) {
			// A universe is quiet most ticks. A frame per quiet tick is a frame
			// per tick, and the other end has to read every one of them.
			return true;
		}

		HostFrame frame;
		frame.Signal = HostSignal::Traffic;
		frame.Traffic.assign(traffic.begin(), traffic.end());
		return Send(frame);
	}

	bool HostLink::SendDeliveries(std::span<const HostDelivery> deliveries) {
		if (deliveries.empty()) {
			return true;
		}

		HostFrame frame;
		frame.Signal = HostSignal::Deliveries;
		frame.Deliveries.assign(deliveries.begin(), deliveries.end());
		return Send(frame);
	}

	size_t HostLink::Receive(std::vector<HostFrame> &frames) {
		if (Channel_ == nullptr) {
			return 0;
		}

		size_t taken = 0;
		while (Channel_->Receive(Scratch) == parallel::ChannelStatus::Ok) {
			core::ByteReader reader(Scratch);

			HostFrame frame;
			if (!ReadHostFrame(reader, frame)) {
				Malformed_++;
				ENGINE_WARN("host link '{}': discarded an unreadable frame.", Name_.Text());
				continue;
			}

			frames.push_back(std::move(frame));
			taken++;
		}
		return taken;
	}

	void HostLink::Close() {
		if (Channel_ != nullptr) {
			Channel_->Close();
		}
	}
}
