#include <engine/core/Metrics.hpp>
#include <engine/world/PresentationBus.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace engine::world {

	namespace {
		constexpr uint32_t PRESENTATION_MAGIC = 0x31534250u; // PBS1
		bool ValidName(std::string_view name) {
			return !name.empty() && name.size() <= MAX_PRESENTATION_NAME &&
				   name.find('\0') == std::string_view::npos;
		}
		bool Valid(const PresentationAddress &address) {
			return ValidName(address.World) && ValidName(address.Channel) && address.Session != 0 &&
				   address.Generation != 0;
		}
		bool Valid(const PresentationMessage &message) {
			return Valid(message.From) && Valid(message.To) && message.Sequence != 0 &&
				   message.Payload.size() <= MAX_PRESENTATION_PAYLOAD;
		}
		void WriteAddress(core::ByteWriter &writer, const PresentationAddress &address) {
			writer.WriteString(address.World);
			writer.WriteString(address.Channel);
			writer.WriteUInt64(address.Session);
			writer.WriteUInt64(address.Generation);
		}
		struct ReadAddress {
			std::string_view World;
			std::string_view Channel;
			uint64_t Session = 0;
			uint64_t Generation = 0;
		};
		ReadAddress ReadEndpoint(core::ByteReader &reader) {
			return {reader.ReadString(), reader.ReadString(), reader.ReadUInt64(), reader.ReadUInt64()};
		}
		bool Valid(const ReadAddress &address) {
			return ValidName(address.World) && ValidName(address.Channel) && address.Session != 0 &&
				   address.Generation != 0;
		}
		PresentationAddress CopyAddress(const ReadAddress &address) {
			return {
				std::string(address.World), std::string(address.Channel), address.Session, address.Generation
			};
		}
	}

	bool WritePresentationMessage(core::ByteWriter &writer, const PresentationMessage &message) {
		if (!Valid(message)) {
			return false;
		}
		writer.WriteUInt32(PRESENTATION_MAGIC);
		WriteAddress(writer, message.From);
		WriteAddress(writer, message.To);
		writer.WriteUInt64(message.Sequence);
		writer.WriteUInt64(message.Correlation);
		writer.WriteUInt32(static_cast<uint32_t>(message.Payload.size()));
		writer.WriteRaw(message.Payload.data(), message.Payload.size());
		return true;
	}

	bool ReadPresentationMessage(core::ByteReader &reader, PresentationMessage &message) {
		const uint32_t magic = reader.ReadUInt32();
		const ReadAddress from = ReadEndpoint(reader);
		const ReadAddress to = ReadEndpoint(reader);
		const uint64_t sequence = reader.ReadUInt64();
		const uint64_t correlation = reader.ReadUInt64();
		const uint32_t count = reader.ReadUInt32();
		if (reader.Failed() || magic != PRESENTATION_MAGIC || !Valid(from) || !Valid(to) || sequence == 0 ||
			count > MAX_PRESENTATION_PAYLOAD || count > reader.Remaining()) {
			reader.Fail();
			return false;
		}
		const auto bytes = reader.ReadRawView(count);
		PresentationMessage decoded;
		decoded.From = CopyAddress(from);
		decoded.To = CopyAddress(to);
		decoded.Sequence = sequence;
		decoded.Correlation = correlation;
		decoded.Payload.assign(bytes.begin(), bytes.end());
		message = std::move(decoded);
		return true;
	}

	static bool
	WriteDirectory(core::ByteWriter &writer, const PresentationDirectory &directory, bool routes) {
		if (directory.Session == 0 || directory.Revision == 0 ||
			directory.Endpoints.size() > MAX_PRESENTATION_DIRECTORY)
			return false;
		for (size_t index = 0; index < directory.Endpoints.size(); ++index) {
			const auto &address = directory.Endpoints[index];
			if (!Valid(address) || (!routes && address.Session != directory.Session)) return false;
			for (size_t previous = 0; previous < index; ++previous) {
				const auto &other = directory.Endpoints[previous];
				if (other.World == address.World && other.Channel == address.Channel) return false;
			}
		}
		writer.WriteUInt32(routes ? 0x31524250u : 0x31444250u);
		writer.WriteUInt64(directory.Session);
		writer.WriteUInt64(directory.Revision);
		writer.WriteUInt32(static_cast<uint32_t>(directory.Endpoints.size()));
		for (const auto &address : directory.Endpoints)
			WriteAddress(writer, address);
		return true;
	}

	static bool ReadDirectory(core::ByteReader &reader, PresentationDirectory &directory, bool routes) {
		if (reader.ReadUInt32() != (routes ? 0x31524250u : 0x31444250u)) return false;
		PresentationDirectory decoded;
		decoded.Session = reader.ReadUInt64();
		decoded.Revision = reader.ReadUInt64();
		const auto count = reader.ReadUInt32();
		if (reader.Failed() || decoded.Session == 0 || decoded.Revision == 0 ||
			count > MAX_PRESENTATION_DIRECTORY)
			return false;
		for (uint32_t index = 0; index < count; ++index) {
			const auto address = ReadEndpoint(reader);
			if (reader.Failed() || !Valid(address) || (!routes && address.Session != decoded.Session))
				return false;
			for (const auto &previous : decoded.Endpoints) {
				if (previous.World == address.World && previous.Channel == address.Channel) return false;
			}
			decoded.Endpoints.push_back(CopyAddress(address));
		}
		directory = std::move(decoded);
		return true;
	}

	bool WritePresentationDirectory(core::ByteWriter &writer, const PresentationDirectory &directory) {
		return WriteDirectory(writer, directory, false);
	}
	bool ReadPresentationDirectory(core::ByteReader &reader, PresentationDirectory &directory) {
		return ReadDirectory(reader, directory, false);
	}
	bool WritePresentationRoutes(core::ByteWriter &writer, const PresentationDirectory &directory) {
		return WriteDirectory(writer, directory, true);
	}
	bool ReadPresentationRoutes(core::ByteReader &reader, PresentationDirectory &directory) {
		return ReadDirectory(reader, directory, true);
	}
	PresentationDirectory PresentationBus::RoutesFor(core::Name consumer) const {
		PresentationDirectory directory{Session, RoutesRevision, {}};
		for (const auto &endpoint : Endpoints) {
			if (endpoint.Host != consumer) directory.Endpoints.push_back(endpoint.Address);
		}
		return directory;
	}
	PresentationStatus PresentationBus::ApplyRoutesFromDriver(const PresentationDirectory &directory) {
		return ApplyDirectory(core::Name("presentation-driver"), directory, true);
	}
	PresentationStatus
	PresentationBus::ApplyDirectory(core::Name host, const PresentationDirectory &directory) {
		return ApplyDirectory(host, directory, false);
	}

	PresentationDirectory PresentationBus::LocalDirectory() const {
		PresentationDirectory directory{Session, DirectoryRevision, {}};
		for (const auto &endpoint : Endpoints) {
			if (!endpoint.Host.IsValid()) directory.Endpoints.push_back(endpoint.Address);
		}
		return directory;
	}

	PresentationStatus
	PresentationBus::ApplyDirectory(core::Name host, const PresentationDirectory &directory, bool routes) {
		core::ByteWriter validation;
		if (!host.IsValid() || !WriteDirectory(validation, directory, routes))
			return PresentationStatus::Invalid;
		auto version =
			std::find_if(DirectoryVersions.begin(), DirectoryVersions.end(), [host](const auto &entry) {
				return entry.Host == host;
			});
		if (version != DirectoryVersions.end() &&
			(directory.Session < version->Session ||
			 (directory.Session == version->Session &&
			  (version->Retired || directory.Revision <= version->Revision))))
			return PresentationStatus::StaleEndpoint;
		if (version == DirectoryVersions.end() && DirectoryVersions.size() >= Limits.Endpoints)
			return PresentationStatus::Full;
		const size_t retained =
			std::count_if(Endpoints.begin(), Endpoints.end(), [host](const auto &endpoint) {
				return endpoint.Host != host;
			});
		if (retained + directory.Endpoints.size() > Limits.Endpoints) return PresentationStatus::Full;
		for (const auto &address : directory.Endpoints) {
			const auto *existing = Find(address);
			if (existing != nullptr && existing->Host != host) return PresentationStatus::WrongHost;
		}
		std::erase_if(Endpoints, [&](const auto &endpoint) {
			if (endpoint.Host != host ||
				std::find(directory.Endpoints.begin(), directory.Endpoints.end(), endpoint.Address) !=
					directory.Endpoints.end())
				return false;
			Purge(endpoint.Address);
			RoutesRevision++;
			return true;
		});
		for (const auto &address : directory.Endpoints) {
			if (Find(address) == nullptr) Endpoints.push_back({address, host, 0});
		}
		if (version == DirectoryVersions.end())
			DirectoryVersions.push_back({host, directory.Session, directory.Revision});
		else
			*version = {host, directory.Session, directory.Revision};
		RoutesRevision++;
		return PresentationStatus::Ok;
	}

	void PresentationBus::RetireHost(core::Name host) {
		if (!host.IsValid()) return;
		for (auto &version : DirectoryVersions) {
			if (version.Host == host) version.Retired = true;
		}
		std::erase_if(Endpoints, [&](const auto &endpoint) {
			if (endpoint.Host != host) return false;
			Purge(endpoint.Address);
			RoutesRevision++;
			return true;
		});
	}

	PresentationBus::PresentationBus(uint64_t session, const PresentationLimits &limits)
		: Session(session), Limits(limits) {}

	PresentationBus::Endpoint *PresentationBus::Find(const PresentationAddress &address) {
		const auto found = std::find_if(Endpoints.begin(), Endpoints.end(), [&](const Endpoint &endpoint) {
			return endpoint.Address.World == address.World && endpoint.Address.Channel == address.Channel;
		});
		return found == Endpoints.end() ? nullptr : &*found;
	}

	const PresentationBus::Endpoint *PresentationBus::Find(const PresentationAddress &address) const {
		const auto found = std::find_if(Endpoints.begin(), Endpoints.end(), [&](const Endpoint &endpoint) {
			return endpoint.Address.World == address.World && endpoint.Address.Channel == address.Channel;
		});
		return found == Endpoints.end() ? nullptr : &*found;
	}

	PresentationOpen PresentationBus::Open(core::Name world, core::Name channel) {
		if (std::count_if(Endpoints.begin(), Endpoints.end(), [](const auto &endpoint) {
				return !endpoint.Host.IsValid();
			}) >= MAX_PRESENTATION_DIRECTORY) {
			return {PresentationStatus::Full, {}};
		}
		const PresentationAddress address{
			std::string(world.Text()), std::string(channel.Text()), Session, NextGeneration
		};
		if (!Valid(address) || NextGeneration == std::numeric_limits<uint64_t>::max()) {
			return {};
		}
		if (Find(address) != nullptr) {
			return {PresentationStatus::Duplicate, {}};
		}
		if (Endpoints.size() >= Limits.Endpoints) {
			return {PresentationStatus::Full, {}};
		}
		Endpoints.push_back({address, {}, 0});
		NextGeneration++;
		DirectoryRevision++;
		RoutesRevision++;
		return {PresentationStatus::Ok, address};
	}

	PresentationAddress PresentationBus::Lookup(std::string_view world, std::string_view channel) const {
		for (const auto &endpoint : Endpoints) {
			if (endpoint.Address.World == world && endpoint.Address.Channel == channel) {
				return endpoint.Address;
			}
		}
		return {};
	}

	PresentationStatus PresentationBus::RegisterRemote(core::Name host, const PresentationAddress &address) {
		if (!ValidName(host.Text()) || !Valid(address)) {
			return PresentationStatus::Invalid;
		}
		if (const Endpoint *existing = Find(address)) {
			return existing->Address == address && existing->Host == host ? PresentationStatus::Ok
																		  : PresentationStatus::Duplicate;
		}
		if (Endpoints.size() >= Limits.Endpoints) {
			return PresentationStatus::Full;
		}
		auto version =
			std::find_if(DirectoryVersions.begin(), DirectoryVersions.end(), [host](const auto &entry) {
				return entry.Host == host;
			});
		if (version != DirectoryVersions.end() && version->Retired && address.Session <= version->Session)
			return PresentationStatus::StaleEndpoint;
		if (version == DirectoryVersions.end()) {
			if (DirectoryVersions.size() >= Limits.Endpoints) return PresentationStatus::Full;
			DirectoryVersions.push_back({host, address.Session, 0});
		} else if (address.Session > version->Session) {
			*version = {host, address.Session, 0};
		}
		Endpoints.push_back({address, host, 0});
		RoutesRevision++;
		return PresentationStatus::Ok;
	}

	void PresentationBus::Purge(const PresentationAddress &address) {
		const auto before = Size;
		std::erase_if(Pending, [&](const PresentationOutbound &queued) {
			if (queued.Message.From != address && queued.Message.To != address) {
				return false;
			}
			Size.Bytes -= queued.Message.Payload.size();
			Size.Messages--;
			return true;
		});
		ReportRetired(before, true);
	}

	PresentationStatus PresentationBus::Close(const PresentationAddress &address) {
		Endpoint *found = Find(address);
		if (found == nullptr) {
			return PresentationStatus::NoEndpoint;
		}
		if (found->Address != address) {
			return PresentationStatus::StaleEndpoint;
		}
		if (!found->Host.IsValid()) DirectoryRevision++;
		RoutesRevision++;
		Purge(address);
		std::erase_if(Endpoints, [&](const Endpoint &endpoint) { return endpoint.Address == address; });
		return PresentationStatus::Ok;
	}

	void PresentationBus::RemoveWorld(core::Name world) {
		for (const Endpoint &endpoint : Endpoints) {
			if (endpoint.Address.World == world.Text()) {
				if (!endpoint.Host.IsValid()) DirectoryRevision++;
				RoutesRevision++;
				Purge(endpoint.Address);
			}
		}
		std::erase_if(Endpoints, [&](const Endpoint &endpoint) {
			return endpoint.Address.World == world.Text();
		});
	}

	bool PresentationBus::Reset(uint64_t session, const PresentationLimits &limits) {
		if (session == 0 || session <= Session) {
			return false;
		}
		const auto before = Size;
		Session = session;
		NextGeneration = 1;
		DirectoryRevision = 1;
		RoutesRevision = 1;
		DirectoryVersions.clear();
		Limits = limits;
		Endpoints.clear();
		Pending.clear();
		Size = {};
		ReportRetired(before, true);
		return true;
	}

	PresentationStatus
	PresentationBus::Route(const PresentationMessage &message, std::span<const std::byte> payload) {
		const Endpoint *destination = Find(message.To);
		if (destination == nullptr) {
			return PresentationStatus::NoEndpoint;
		}
		if (destination->Address != message.To) {
			return PresentationStatus::StaleEndpoint;
		}
		const uint64_t bytes = payload.size();
		if (bytes > Limits.MaximumPayload || bytes > MAX_PRESENTATION_PAYLOAD) {
			return PresentationStatus::TooLarge;
		}
		PresentationQueueSize endpointSize;
		for (const PresentationOutbound &queued : Pending) {
			if (queued.Message.To == message.To) {
				endpointSize.Bytes += queued.Message.Payload.size();
				endpointSize.Messages++;
			}
		}
		if (Size.Messages >= Limits.Messages || endpointSize.Messages >= Limits.MessagesPerEndpoint ||
			bytes > Limits.Bytes || Size.Bytes > Limits.Bytes - bytes || bytes > Limits.BytesPerEndpoint ||
			endpointSize.Bytes > Limits.BytesPerEndpoint - bytes) {
			return PresentationStatus::Full;
		}
		PresentationMessage owned{
			message.From, message.To, message.Sequence, message.Correlation, {payload.begin(), payload.end()}
		};
		Pending.push_back({destination->Host, std::move(owned)});
		Size.Bytes += bytes;
		Size.Messages++;
		TrafficTotals.EnqueuedMessages++;
		TrafficTotals.EnqueuedBytes += bytes;
		core::Metrics::Count("world.presentation.enqueued.messages", 1);
		core::Metrics::Count("world.presentation.enqueued.bytes", static_cast<double>(bytes));
		return PresentationStatus::Ok;
	}

	PresentationStatus PresentationBus::Send(
		const PresentationAddress &from,
		const PresentationAddress &to,
		uint64_t correlation,
		std::span<const std::byte> payload
	) {
		Endpoint *source = Find(from);
		if (source == nullptr) {
			return PresentationStatus::NoEndpoint;
		}
		if (source->Address != from) {
			return PresentationStatus::StaleEndpoint;
		}
		if (source->Host.IsValid()) {
			return PresentationStatus::WrongHost;
		}
		if (source->Sequence == std::numeric_limits<uint64_t>::max()) {
			return PresentationStatus::Invalid;
		}
		if (payload.size() > Limits.MaximumPayload || payload.size() > MAX_PRESENTATION_PAYLOAD) {
			return PresentationStatus::TooLarge;
		}
		PresentationMessage message{source->Address, to, source->Sequence + 1, correlation, {}};
		const PresentationStatus status = Route(message, payload);
		if (status == PresentationStatus::Ok) {
			source->Sequence++;
		}
		return status;
	}

	PresentationStatus
	PresentationBus::Accept(core::Name host, const PresentationMessage &message, bool driver) {
		if (!Valid(message)) {
			return PresentationStatus::Invalid;
		}
		Endpoint *source = Find(message.From);
		if (source == nullptr) {
			return PresentationStatus::NoEndpoint;
		}
		if (source->Address != message.From) {
			return PresentationStatus::StaleEndpoint;
		}
		if (!source->Host.IsValid() || (!driver && source->Host != host)) {
			return PresentationStatus::WrongHost;
		}
		if (message.Sequence <= source->Sequence) {
			return PresentationStatus::Duplicate;
		}
		if (driver) {
			const Endpoint *destination = Find(message.To);
			if (destination != nullptr && destination->Host.IsValid()) {
				return PresentationStatus::WrongHost;
			}
		}
		const PresentationStatus status = Route(message, message.Payload);
		if (status == PresentationStatus::Ok) {
			source->Sequence = message.Sequence;
		}
		return status;
	}

	PresentationStatus PresentationBus::Receive(core::Name host, const PresentationMessage &message) {
		return Accept(host, message, false);
	}
	PresentationStatus PresentationBus::ReceiveFromDriver(const PresentationMessage &message) {
		return Accept({}, message, true);
	}

	std::vector<PresentationMessage> PresentationBus::Take(const PresentationAddress &address) {
		const auto before = Size;
		std::vector<PresentationMessage> taken;
		const Endpoint *endpoint = Find(address);
		if (endpoint == nullptr || endpoint->Address != address || endpoint->Host.IsValid()) {
			return taken;
		}
		std::erase_if(Pending, [&](PresentationOutbound &queued) {
			if (queued.Message.To != address) {
				return false;
			}
			Size.Bytes -= queued.Message.Payload.size();
			Size.Messages--;
			taken.push_back(std::move(queued.Message));
			return true;
		});
		ReportRetired(before, false);
		return taken;
	}

	std::vector<PresentationOutbound> PresentationBus::TakeOutbound() {
		const auto before = Size;
		std::vector<PresentationOutbound> taken;
		std::erase_if(Pending, [&](PresentationOutbound &queued) {
			if (!queued.Host.IsValid()) {
				return false;
			}
			Size.Bytes -= queued.Message.Payload.size();
			Size.Messages--;
			taken.push_back(std::move(queued));
			return true;
		});
		ReportRetired(before, false);
		return taken;
	}

	PresentationQueueSize PresentationBus::Queued() const {
		return Size;
	}
	PresentationTraffic PresentationBus::Traffic() const {
		return TrafficTotals;
	}

	void PresentationBus::ReportRetired(PresentationQueueSize before, bool discarded) {
		const uint64_t messages = before.Messages - Size.Messages;
		const uint64_t bytes = before.Bytes - Size.Bytes;
		if (messages == 0) {
			return;
		}
		if (discarded) {
			TrafficTotals.DiscardedMessages += messages;
			TrafficTotals.DiscardedBytes += bytes;
			core::Metrics::Count("world.presentation.discarded.messages", static_cast<double>(messages));
			core::Metrics::Count("world.presentation.discarded.bytes", static_cast<double>(bytes));
			return;
		}
		TrafficTotals.TakenMessages += messages;
		TrafficTotals.TakenBytes += bytes;
		core::Metrics::Count("world.presentation.taken.messages", static_cast<double>(messages));
		core::Metrics::Count("world.presentation.taken.bytes", static_cast<double>(bytes));
	}

}
