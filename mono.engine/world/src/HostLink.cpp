#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/world/HostLink.hpp>

#include <array>
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
		case HostSignal::PresentationBindings:
			return "presentation-bindings";
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
		case HostSignal::Presentation:
			return "presentation";
		case HostSignal::PresentationDirectory:
			return "presentation-directory";
		case HostSignal::PresentationRoutes:
			return "presentation-routes";
		case HostSignal::TickExchangeCommand:
			return "tick-exchange-command";
		case HostSignal::TickExchangeResult:
			return "tick-exchange-result";
		}
		// No default label, so adding a signal is a compiler warning here.
		return "?";
	}

	namespace {
		struct BindingEndpointView {
			std::string_view World, Channel;
			uint64_t Session = 0, Generation = 0;
			bool operator==(const BindingEndpointView &) const = default;
		};
		bool ValidBindingEndpoint(const auto &endpoint) {
			const auto validName = [](std::string_view name) {
				return !name.empty() && name.size() <= MAX_PRESENTATION_NAME &&
					   name.find('\0') == std::string_view::npos;
			};
			return validName(endpoint.World) && validName(endpoint.Channel) && endpoint.Session &&
				   endpoint.Generation;
		}
		bool ValidBindingRows(const auto &rows) {
			for (size_t index = 0; index < rows.size(); ++index) {
				if (!ValidBindingEndpoint(rows[index].Local) || !ValidBindingEndpoint(rows[index].Published))
					return false;
				for (size_t previous = 0; previous < index; ++previous)
					if (rows[index].Local == rows[previous].Local ||
						rows[index].Published == rows[previous].Published)
						return false;
			}
			return true;
		}
		bool DisjointBindingLocals(const auto &exports, const auto &returns) {
			for (const auto &exported : exports)
				for (const auto &returned : returns)
					if (exported.Local == returned.Local) return false;
			return true;
		}
	}
	bool WritePresentationBindings(core::ByteWriter &writer, const PresentationBindings &bindings) {
		if (!bindings.Session || !bindings.Revision || bindings.Exports.size() > MAX_PRESENTATION_DIRECTORY ||
			bindings.Returns.size() > MAX_PRESENTATION_DIRECTORY - bindings.Exports.size() ||
			!ValidBindingRows(bindings.Exports) || !ValidBindingRows(bindings.Returns) ||
			!DisjointBindingLocals(bindings.Exports, bindings.Returns))
			return false;
		writer.WriteUInt32(0x31424250u); // PBB1
		writer.WriteUInt64(bindings.Session);
		writer.WriteUInt64(bindings.Revision);
		writer.WriteUInt32(static_cast<uint32_t>(bindings.Exports.size()));
		writer.WriteUInt32(static_cast<uint32_t>(bindings.Returns.size()));
		for (const auto *rows : {&bindings.Exports, &bindings.Returns})
			for (const auto &binding : *rows)
				for (const auto *endpoint : {&binding.Local, &binding.Published}) {
					writer.WriteString(endpoint->World);
					writer.WriteString(endpoint->Channel);
					writer.WriteUInt64(endpoint->Session);
					writer.WriteUInt64(endpoint->Generation);
				}
		return true;
	}
	bool ReadPresentationBindings(core::ByteReader &reader, PresentationBindings &bindings) {
		const auto magic = reader.ReadUInt32();
		const auto session = reader.ReadUInt64(), revision = reader.ReadUInt64();
		const auto exports = reader.ReadUInt32(), returns = reader.ReadUInt32();
		if (reader.Failed() || magic != 0x31424250u || !session || !revision ||
			exports > MAX_PRESENTATION_DIRECTORY || returns > MAX_PRESENTATION_DIRECTORY - exports) {
			reader.Fail();
			return false;
		}
		struct BindingView {
			BindingEndpointView Local, Published;
		};
		std::array<BindingView, MAX_PRESENTATION_DIRECTORY> rows{};
		for (uint32_t index = 0; index < exports + returns; ++index)
			for (auto *endpoint : {&rows[index].Local, &rows[index].Published})
				*endpoint = {
					reader.ReadString(), reader.ReadString(), reader.ReadUInt64(), reader.ReadUInt64()
				};
		if (reader.Failed() || !ValidBindingRows(std::span(rows).first(exports)) ||
			!ValidBindingRows(std::span(rows).subspan(exports, returns)) ||
			!DisjointBindingLocals(
				std::span(rows).first(exports), std::span(rows).subspan(exports, returns)
			)) {
			reader.Fail();
			return false;
		}
		PresentationBindings decoded{session, revision, {}, {}};
		const auto copy = [](const BindingEndpointView &endpoint) {
			return PresentationAddress{
				std::string(endpoint.World),
				std::string(endpoint.Channel),
				endpoint.Session,
				endpoint.Generation
			};
		};
		for (uint32_t index = 0; index < exports + returns; ++index)
			(index < exports ? decoded.Exports : decoded.Returns)
				.push_back({copy(rows[index].Local), copy(rows[index].Published)});
		bindings = std::move(decoded);
		return true;
	}

	void WriteHostFrame(core::ByteWriter &writer, const HostFrame &frame) {
		if (frame.Signal == HostSignal::PresentationBindings) {
			core::ByteWriter payload;
			if (!WritePresentationBindings(payload, frame.Bindings)) return;
			writer.WriteUInt32(FRAME_MAGIC);
			writer.WriteUInt8(static_cast<uint8_t>(frame.Signal));
			writer.WriteRaw(payload.Bytes().data(), payload.Size());
			return;
		}
		if (frame.Signal == HostSignal::TickExchangeCommand ||
			frame.Signal == HostSignal::TickExchangeResult) {
			core::ByteWriter payload;
			if (!(frame.Signal == HostSignal::TickExchangeCommand
					  ? WriteTickExchangeControl(payload, frame.ExchangeCommand)
					  : WriteTickExchangeControl(payload, frame.ExchangeResult)))
				return;
			writer.WriteUInt32(FRAME_MAGIC);
			writer.WriteUInt8(static_cast<uint8_t>(frame.Signal));
			writer.WriteRaw(payload.Bytes().data(), payload.Size());
			return;
		}
		if (frame.Signal == HostSignal::Presentation || frame.Signal == HostSignal::PresentationDirectory ||
			frame.Signal == HostSignal::PresentationRoutes) {
			core::ByteWriter payload;
			if (!(frame.Signal == HostSignal::Presentation
					  ? WritePresentationMessage(payload, frame.Presentation)
					  : (frame.Signal == HostSignal::PresentationRoutes
							 ? WritePresentationRoutes(payload, frame.Directory)
							 : WritePresentationDirectory(payload, frame.Directory)))) {
				return;
			}
			writer.WriteUInt32(FRAME_MAGIC);
			writer.WriteUInt8(static_cast<uint8_t>(frame.Signal));
			writer.WriteRaw(payload.Bytes().data(), payload.Size());
			return;
		}
		writer.WriteUInt32(FRAME_MAGIC);
		writer.WriteUInt8(static_cast<uint8_t>(frame.Signal));
		writer.WriteName(frame.Host);
		writer.WriteName(frame.World);
		writer.WriteUInt64(frame.Tick);
		writer.WriteFloat(frame.Milliseconds);
		writer.WriteUInt16(frame.Port);

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
		if (signal > static_cast<uint8_t>(HostSignal::PresentationBindings)) {
			return false;
		}
		read.Signal = static_cast<HostSignal>(signal);
		if (read.Signal == HostSignal::PresentationBindings) {
			if (!ReadPresentationBindings(reader, read.Bindings) || reader.Remaining() != 0) {
				reader.Fail();
				return false;
			}
			frame = std::move(read);
			return true;
		}
		if (read.Signal == HostSignal::TickExchangeCommand || read.Signal == HostSignal::TickExchangeResult) {
			if (!(read.Signal == HostSignal::TickExchangeCommand
					  ? ReadTickExchangeControl(reader, read.ExchangeCommand)
					  : ReadTickExchangeControl(reader, read.ExchangeResult)))
				return false;
			frame = std::move(read);
			return true;
		}
		if (read.Signal == HostSignal::PresentationDirectory ||
			read.Signal == HostSignal::PresentationRoutes) {
			if (!(read.Signal == HostSignal::PresentationRoutes
					  ? ReadPresentationRoutes(reader, read.Directory)
					  : ReadPresentationDirectory(reader, read.Directory)) ||
				reader.Remaining() != 0) {
				reader.Fail();
				return false;
			}
			frame = std::move(read);
			return true;
		}
		if (read.Signal == HostSignal::Presentation) {
			if (!ReadPresentationMessage(reader, read.Presentation) || reader.Remaining() != 0) {
				reader.Fail();
				return false;
			}
			frame = std::move(read);
			return true;
		}

		read.Host = reader.ReadName();
		read.World = reader.ReadName();
		read.Tick = reader.ReadUInt64();
		read.Milliseconds = reader.ReadFloat();
		read.Port = reader.ReadUInt16();

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
		if (writer.Empty()) {
			Dropped_++;
			return false;
		}

		const parallel::ChannelStatus status = Channel_->Send(writer.Bytes());
		if (status == parallel::ChannelStatus::Ok) {
			if (frame.Signal == HostSignal::Presentation) {
				core::Metrics::Count("world.presentation.transport.sent.messages", 1);
				core::Metrics::Count(
					"world.presentation.transport.sent.bytes", static_cast<double>(writer.Size())
				);
			}
			if (frame.Signal == HostSignal::TickExchangeCommand ||
				frame.Signal == HostSignal::TickExchangeResult) {
				core::Metrics::Count("world.tickexchange.transport.sent.messages", 1);
				core::Metrics::Count(
					"world.tickexchange.transport.sent.bytes", static_cast<double>(writer.Size())
				);
			}
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

	bool HostLink::PublishPresentationDirectory(const PresentationDirectory &directory) {
		return PublishDirectory(directory, false);
	}
	bool HostLink::PublishPresentationRoutes(const PresentationDirectory &directory) {
		return PublishDirectory(directory, true);
	}
	bool HostLink::PublishPresentationBindings(const PresentationBindings &bindings) {
		if (!Connected() || !bindings.Session || !bindings.Revision ||
			bindings.Session < PublishedBindingsSession ||
			(bindings.Session == PublishedBindingsSession && bindings.Revision < PublishedBindingsRevision))
			return false;
		if (bindings.Session == PublishedBindingsSession && bindings.Revision == PublishedBindingsRevision)
			return true;
		HostFrame frame;
		frame.Signal = HostSignal::PresentationBindings;
		frame.Bindings = bindings;
		if (!Send(frame)) return false;
		PublishedBindingsSession = bindings.Session;
		PublishedBindingsRevision = bindings.Revision;
		return true;
	}

	bool HostLink::PublishDirectory(const PresentationDirectory &directory, bool routes) {
		auto &session = routes ? PublishedRoutesSession : PublishedDirectorySession;
		auto &revision = routes ? PublishedRoutesRevision : PublishedDirectoryRevision;
		if (!Connected() || directory.Session == 0 || directory.Revision == 0 ||
			directory.Session < session || (directory.Session == session && directory.Revision < revision)) {
			return false;
		}
		if (directory.Session == session && directory.Revision == revision) {
			return true;
		}
		HostFrame frame;
		frame.Signal = routes ? HostSignal::PresentationRoutes : HostSignal::PresentationDirectory;
		frame.Directory = directory;
		if (!Send(frame)) return false;
		session = directory.Session;
		revision = directory.Revision;
		return true;
	}
	bool HostLink::SendPresentation(const PresentationMessage &message) {
		HostFrame frame;
		frame.Signal = HostSignal::Presentation;
		frame.Presentation = message;
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

		// A producer may keep publishing while this owner drains. Bound each
		// poll so presentation traffic cannot retain unbounded decoded frames.
		constexpr size_t MAXIMUM_POLL_FRAMES = 64;
		constexpr size_t MAXIMUM_POLL_BYTES = 32u * 1024u * 1024u;
		size_t taken = 0;
		size_t attempted = 0;
		size_t bytes = 0;
		while (attempted < MAXIMUM_POLL_FRAMES && bytes < MAXIMUM_POLL_BYTES &&
			   Channel_->Receive(Scratch) == parallel::ChannelStatus::Ok) {
			attempted++;
			bytes += Scratch.size();
			core::ByteReader reader(Scratch);

			HostFrame frame;
			if (!ReadHostFrame(reader, frame)) {
				Malformed_++;
				ENGINE_WARN("host link '{}': discarded an unreadable frame.", Name_.Text());
				continue;
			}

			if (frame.Signal == HostSignal::Presentation) {
				core::Metrics::Count("world.presentation.transport.received.messages", 1);
				core::Metrics::Count(
					"world.presentation.transport.received.bytes", static_cast<double>(Scratch.size())
				);
			}
			if (frame.Signal == HostSignal::TickExchangeCommand ||
				frame.Signal == HostSignal::TickExchangeResult) {
				core::Metrics::Count("world.tickexchange.transport.received.messages", 1);
				core::Metrics::Count(
					"world.tickexchange.transport.received.bytes", static_cast<double>(Scratch.size())
				);
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
