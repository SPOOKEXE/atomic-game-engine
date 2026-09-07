#include <engine/world/PresentationPeer.hpp>

#include <algorithm>
#include <limits>

namespace engine::world {
	PresentationPeer::PresentationPeer(
		Universe &universe, WorldId world, uint32_t firstSlot, PresentationPeerChannels channels
	)
		: Worlds(universe), World(world), FirstSlot(firstSlot), Channels(std::move(channels)) {}
	PresentationPeer::~PresentationPeer() {
		Close();
	}
	std::string PresentationPeer::Consumer(std::string_view channel) const {
		if (std::find(Channels.Consumers.begin(), Channels.Consumers.end(), channel) !=
			Channels.Consumers.end())
			return std::string(channel);
		for (const auto &prefix : Channels.ConsumerPrefixes)
			if (!prefix.empty() && prefix.back() == '/' && channel.starts_with(prefix) &&
				channel.size() > prefix.size())
				return prefix.substr(0, prefix.size() - 1);
		return {};
	}
	bool PresentationPeer::Producer(std::string_view channel) const {
		return std::find(Channels.Producers.begin(), Channels.Producers.end(), channel) !=
			   Channels.Producers.end();
	}
	PresentationStatus PresentationPeer::Apply(const PresentationDirectory &directory) {
		core::ByteWriter validation;
		if (Closed || Worlds.IsRemote(World) || !Worlds.NameOf(World).IsValid() ||
			FirstSlot > std::numeric_limits<uint32_t>::max() - 63 ||
			!WritePresentationDirectory(validation, directory))
			return PresentationStatus::Invalid;
		if (directory.Session < PeerSession ||
			(directory.Session == PeerSession && directory.Revision <= PeerRevision))
			return PresentationStatus::StaleEndpoint;
		for (const auto &address : directory.Endpoints)
			if (Consumer(address.Channel).empty()) return PresentationStatus::WrongHost;
		std::vector<Endpoint> endpoints;
		std::vector<PresentationAddress> opened;
		const auto rollback = [&] {
			for (const auto &address : opened)
				(void)Worlds.ClosePresentation(address);
		};
		for (const auto &address : directory.Endpoints) {
			const auto existing = std::find_if(Endpoints.begin(), Endpoints.end(), [&](const auto &item) {
				return item.Original == address;
			});
			if (existing != Endpoints.end()) {
				endpoints.push_back(*existing);
				continue;
			}
			const auto base = Consumer(address.Channel);
			PresentationOpen receipt;
			for (uint32_t slot = 0; slot < 64; ++slot) {
				const auto channel = base + '/' + std::to_string(FirstSlot + slot);
				if (Worlds.LookupPresentation(World, channel).Session) continue;
				receipt = Worlds.OpenPresentation(World, core::Name(channel));
				break;
			}
			if (receipt.Status != PresentationStatus::Ok) {
				rollback();
				return PresentationStatus::Full;
			}
			opened.push_back(receipt.Address);
			endpoints.push_back({address, receipt.Address, 0});
		}
		for (const auto &old : Endpoints)
			if (std::none_of(endpoints.begin(), endpoints.end(), [&](const auto &item) {
					return item.Alias == old.Alias;
				}))
				(void)Worlds.ClosePresentation(old.Alias);
		Endpoints = std::move(endpoints);
		PeerSession = directory.Session;
		PeerRevision = directory.Revision;
		return PresentationStatus::Ok;
	}
	PresentationStatus PresentationPeer::Accept(const PresentationMessage &message) {
		if (Closed) return PresentationStatus::Invalid;
		const auto endpoint = std::find_if(Endpoints.begin(), Endpoints.end(), [&](const auto &item) {
			return item.Original == message.From;
		});
		if (endpoint == Endpoints.end() || !Producer(message.To.Channel))
			return PresentationStatus::WrongHost;
		if (message.Sequence <= endpoint->ReceivedSequence) return PresentationStatus::StaleEndpoint;
		const auto status =
			Worlds.SendPresentation(World, endpoint->Alias, message.To, message.Correlation, message.Payload);
		if (status == PresentationStatus::Ok) endpoint->ReceivedSequence = message.Sequence;
		return status;
	}
	std::vector<PresentationMessage> PresentationPeer::Take() {
		std::vector<PresentationMessage> replies;
		for (const auto &endpoint : Endpoints)
			for (auto &message : Worlds.TakePresentation(endpoint.Alias)) {
				if (!Producer(message.From.Channel)) continue;
				message.To = endpoint.Original;
				replies.push_back(std::move(message));
			}
		return replies;
	}
	PresentationDirectory PresentationPeer::Routes() const {
		auto directory = Worlds.PresentationRoutesFor({});
		const auto local = Worlds.LocalPresentationDirectory();
		directory.Endpoints.insert(directory.Endpoints.end(), local.Endpoints.begin(), local.Endpoints.end());
		std::erase_if(directory.Endpoints, [&](const auto &endpoint) {
			return Closed || !Producer(endpoint.Channel);
		});
		return directory;
	}
	void PresentationPeer::Close() {
		if (Closed) return;
		Closed = true;
		for (const auto &endpoint : Endpoints)
			(void)Worlds.ClosePresentation(endpoint.Alias);
		Endpoints.clear();
	}
}
