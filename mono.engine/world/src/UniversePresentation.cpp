#include <engine/world/Universe.hpp>

namespace engine::world {
	WorldId Universe::FindPresentationWorld(std::string_view name) const {
		// Control-plane lookup only. Untrusted wire names are never interned.
		for (const auto &world : Registry) {
			if (world != nullptr && world->Name().Text() == name) {
				return world->Id();
			}
		}
		return {};
	}

	bool Universe::ConfigurePresentation(uint64_t session, const PresentationLimits &limits) {
		RequireDriverThread("ConfigurePresentation");
		return !Ticking && PresentationMessages.Reset(session, limits);
	}
	PresentationOpen Universe::OpenPresentation(WorldId world, core::Name channel) {
		RequireDriverThread("OpenPresentation");
		if (Ticking || Reach(world) == nullptr || IsRemote(world)) {
			return {PresentationStatus::WrongHost, {}};
		}
		return PresentationMessages.Open(NameOf(world), channel);
	}
	PresentationAddress Universe::LookupPresentation(WorldId world, std::string_view channel) const {
		RequireDriverThread("LookupPresentation");
		const auto name = NameOf(world);
		return name.IsValid() ? PresentationMessages.Lookup(name.Text(), channel) : PresentationAddress{};
	}

	PresentationStatus
	Universe::RegisterRemotePresentation(core::Name host, const PresentationAddress &address) {
		RequireDriverThread("RegisterRemotePresentation");
		if (Ticking || !host.IsValid() || HostOf(FindPresentationWorld(address.World)) != host) {
			return PresentationStatus::WrongHost;
		}
		return PresentationMessages.RegisterRemote(host, address);
	}
	PresentationDirectory Universe::LocalPresentationDirectory() const {
		RequireDriverThread("LocalPresentationDirectory");
		return PresentationMessages.LocalDirectory();
	}
	void Universe::RetirePresentationHost(core::Name host) {
		RequireDriverThread("RetirePresentationHost");
		if (!Ticking && host.IsValid()) PresentationMessages.RetireHost(host);
	}
	PresentationDirectory Universe::PresentationRoutesFor(core::Name consumer) const {
		RequireDriverThread("PresentationRoutesFor");
		return PresentationMessages.RoutesFor(consumer);
	}
	PresentationStatus Universe::AcceptPresentationRoutesFromDriver(const PresentationDirectory &directory) {
		RequireDriverThread("AcceptPresentationRoutesFromDriver");
		if (Ticking) return PresentationStatus::WrongHost;
		for (const auto &address : directory.Endpoints) {
			const auto world = FindPresentationWorld(address.World);
			if (!world.IsValid() || !IsRemote(world)) return PresentationStatus::WrongHost;
		}
		return PresentationMessages.ApplyRoutesFromDriver(directory);
	}
	PresentationStatus
	Universe::ApplyPresentationDirectory(core::Name host, const PresentationDirectory &directory) {
		RequireDriverThread("ApplyPresentationDirectory");
		if (Ticking || !host.IsValid()) return PresentationStatus::WrongHost;
		for (const auto &address : directory.Endpoints) {
			if (HostOf(FindPresentationWorld(address.World)) != host) return PresentationStatus::WrongHost;
		}
		return PresentationMessages.ApplyDirectory(host, directory);
	}
	PresentationStatus Universe::ClosePresentation(const PresentationAddress &address) {
		RequireDriverThread("ClosePresentation");
		return Ticking ? PresentationStatus::Invalid : PresentationMessages.Close(address);
	}
	PresentationStatus Universe::SendPresentation(
		WorldId source,
		const PresentationAddress &from,
		const PresentationAddress &to,
		uint64_t correlation,
		std::span<const std::byte> payload
	) {
		RequireDriverThread("SendPresentation");
		if (Ticking || NameOf(source).Text() != from.World || IsRemote(source)) {
			return PresentationStatus::WrongHost;
		}
		return PresentationMessages.Send(from, to, correlation, payload);
	}
	PresentationStatus Universe::IngestPresentation(core::Name host, const PresentationMessage &message) {
		RequireDriverThread("IngestPresentation");
		if (Ticking || !host.IsValid()) {
			return PresentationStatus::WrongHost;
		}
		return PresentationMessages.Receive(host, message);
	}
	PresentationStatus Universe::AcceptPresentationFromDriver(const PresentationMessage &message) {
		RequireDriverThread("AcceptPresentationFromDriver");
		if (Ticking) {
			return PresentationStatus::WrongHost;
		}
		return PresentationMessages.ReceiveFromDriver(message);
	}
	std::vector<PresentationMessage> Universe::TakePresentation(const PresentationAddress &address) {
		RequireDriverThread("TakePresentation");
		return Ticking ? std::vector<PresentationMessage>{} : PresentationMessages.Take(address);
	}
	std::vector<PresentationOutbound> Universe::TakePresentationOutbound() {
		RequireDriverThread("TakePresentationOutbound");
		return Ticking ? std::vector<PresentationOutbound>{} : PresentationMessages.TakeOutbound();
	}
	PresentationQueueSize Universe::PresentationQueueUsage() const {
		RequireDriverThread("PresentationQueueUsage");
		return PresentationMessages.Queued();
	}
	PresentationTraffic Universe::PresentationTrafficCounts() const {
		RequireDriverThread("PresentationTrafficCounts");
		return PresentationMessages.Traffic();
	}

}
