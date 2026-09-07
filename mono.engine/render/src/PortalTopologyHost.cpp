#include <engine/ecs/Store.hpp>
#include <engine/render/PortalTopologyHost.hpp>
#include <engine/scene/CameraPortalTopology.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>

namespace engine::render {
	namespace {
		constexpr size_t MAX_WORLDS = 16;
		constexpr auto LIFETIME = std::chrono::seconds(1);
		constexpr auto REFRESH = std::chrono::milliseconds(250);
		constexpr uint32_t REQUEST = 0x31515443;
		constexpr uint32_t RENEWAL = 0x31525443;
		bool ReplyChannel(std::string_view channel) {
			if (channel == PORTAL_TOPOLOGY_REPLIES) return true;
			if (!channel.starts_with(PORTAL_TOPOLOGY_REPLIES)) return false;
			channel.remove_prefix(PORTAL_TOPOLOGY_REPLIES.size());
			if (channel.size() < 2 || channel.front() != '/' || channel[1] == '0') return false;
			channel.remove_prefix(1);
			uint32_t slot = 0;
			const auto [end, error] = std::from_chars(channel.data(), channel.data() + channel.size(), slot);
			return error == std::errc{} && end == channel.data() + channel.size();
		}
		std::vector<std::byte> Control(uint32_t kind, uint64_t revision) {
			core::ByteWriter writer;
			writer.WriteUInt32(kind);
			writer.WriteUInt64(revision);
			return {writer.Bytes().begin(), writer.Bytes().end()};
		}
		std::optional<uint64_t> Revision(std::span<const std::byte> bytes, uint32_t kind) {
			core::ByteReader reader(bytes);
			if (reader.ReadUInt32() != kind) return {};
			const auto revision = reader.ReadUInt64();
			return !reader.Failed() && reader.AtEnd() ? std::optional(revision) : std::nullopt;
		}
	}
	struct PortalTopologyHost::Impl {
		explicit Impl(world::Universe &universe) : Universe(universe) {}
		world::Universe &Universe;
		struct Producer {
			world::WorldId World;
			world::PresentationAddress Address;
			scene::CameraPortalTopology Topology;
			std::vector<std::byte> Bytes;
		};
		struct Source {
			world::WorldId World;
			world::PresentationAddress Address;
		};
		struct Destination {
			world::WorldId World;
			world::WorldId SourceWorld;
			world::PresentationAddress Source;
			world::PresentationAddress Producer;
			uint64_t Pending = 0;
			Time Deadline{};
			Time Expires{};
			Time RefreshAt{};
			PortalTopologySnapshot Snapshot;
		};
		std::vector<Producer> Producers;
		std::vector<Source> Sources;
		std::vector<Destination> Destinations;
		uint64_t Next = 1;
		std::optional<Time> Last;
		bool Clock(Time now) {
			if ((Last && now < *Last) || now > Time::max() - LIFETIME) return false;
			Last = now;
			return true;
		}
		bool Current(const Destination &destination) const {
			return Universe.LookupPresentation(destination.World, PORTAL_TOPOLOGY_REQUESTS) ==
					   destination.Producer &&
				   Universe.LookupPresentation(destination.SourceWorld, PORTAL_TOPOLOGY_REPLIES) ==
					   destination.Source;
		}
		void Prune() {
			std::erase_if(Destinations, [&](const auto &entry) { return !Current(entry); });
			std::erase_if(Producers, [&](const auto &entry) {
				return Universe.LookupPresentation(entry.World, PORTAL_TOPOLOGY_REQUESTS) != entry.Address;
			});
			std::erase_if(Sources, [&](const auto &entry) {
				return Universe.LookupPresentation(entry.World, PORTAL_TOPOLOGY_REPLIES) != entry.Address;
			});
		}
		bool Capture(Producer &producer) {
			scene::CameraPortalTopology topology{producer.Address.World, producer.Topology.Revision, {}};
			bool valid = true;
			const auto entered = Universe.Enter(producer.World, [&](ecs::Store &store) {
				std::vector<scene::PortalSeam> seams;
				scene::GatherPortalSeams(store, seams);
				for (const auto &seam : seams) {
					if (!seam.Crosses) continue;
					if (topology.Mouths.size() == scene::MAX_CAMERA_PORTAL_SEAMS) {
						valid = false;
						break;
					}
					scene::CameraPortalMouth mouth;
					std::string error;
					if (!scene::CopyCameraPortalMouth(store.GetFullName(seam.Camera), seam, mouth, error)) {
						valid = false;
						break;
					}
					topology.Mouths.push_back(std::move(mouth));
				}
			});
			if (!valid || entered != world::WorldStatus::Ok) return false;
			std::sort(topology.Mouths.begin(), topology.Mouths.end(), [](const auto &a, const auto &b) {
				return a.Name < b.Name;
			});
			if (topology == producer.Topology && !producer.Bytes.empty()) return true;
			if (topology.Revision == std::numeric_limits<uint64_t>::max()) return false;
			++topology.Revision;
			std::string error;
			std::vector<std::byte> bytes;
			if (!scene::EncodeCameraPortalTopology(topology, bytes, error)) return false;
			producer.Topology = std::move(topology);
			producer.Bytes = std::move(bytes);
			return true;
		}
	};
	PortalTopologyHost::PortalTopologyHost(world::Universe &universe)
		: State(std::make_unique<Impl>(universe)) {}
	PortalTopologyHost::~PortalTopologyHost() {
		Clear();
	}
	world::PresentationAddress PortalTopologyHost::Serve(world::WorldId world) {
		auto &state = *State;
		state.Prune();
		if (!state.Universe.NameOf(world).IsValid() || state.Universe.IsRemote(world)) return {};
		for (const auto &producer : state.Producers)
			if (producer.World == world) return producer.Address;
		if (state.Producers.size() == MAX_WORLDS) return {};
		const auto opened = state.Universe.OpenPresentation(world, core::Name(PORTAL_TOPOLOGY_REQUESTS));
		if (opened.Status != world::PresentationStatus::Ok) return {};
		state.Producers.push_back({world, opened.Address, {}, {}});
		return opened.Address;
	}
	void PortalTopologyHost::RestartRequests() {
		for (auto &destination : State->Destinations) {
			destination.Pending = 0;
			destination.RefreshAt = Time::min();
		}
	}

	bool PortalTopologyHost::Request(world::WorldId source, world::WorldId destination, Time now) {
		auto &state = *State;
		if (!state.Clock(now) || !state.Universe.NameOf(source).IsValid() ||
			state.Universe.IsRemote(source) || source == destination)
			return false;
		const auto producer = state.Universe.IsRemote(destination)
								  ? state.Universe.LookupPresentation(destination, PORTAL_TOPOLOGY_REQUESTS)
								  : Serve(destination);
		if (producer.Generation == 0) return false;
		auto entry =
			std::find_if(state.Destinations.begin(), state.Destinations.end(), [&](const auto &known) {
				return known.World == destination;
			});
		if (entry != state.Destinations.end() && !state.Current(*entry)) {
			state.Destinations.erase(entry);
			entry = state.Destinations.end();
		}
		if (entry != state.Destinations.end() &&
			((entry->Pending != 0 && now < entry->Deadline) || now < entry->RefreshAt))
			return false;
		if (entry == state.Destinations.end()) {
			if (state.Destinations.size() == MAX_WORLDS) return false;
			std::erase_if(state.Sources, [&](const auto &known) {
				return state.Universe.LookupPresentation(known.World, PORTAL_TOPOLOGY_REPLIES) !=
					   known.Address;
			});
			auto sender = std::find_if(state.Sources.begin(), state.Sources.end(), [&](const auto &known) {
				return known.World == source;
			});
			if (sender == state.Sources.end()) {
				if (state.Sources.size() == MAX_WORLDS) return false;
				const auto opened =
					state.Universe.OpenPresentation(source, core::Name(PORTAL_TOPOLOGY_REPLIES));
				if (opened.Status != world::PresentationStatus::Ok) return false;
				state.Sources.push_back({source, opened.Address});
				sender = std::prev(state.Sources.end());
			}
			Impl::Destination added;
			added.World = destination;
			added.SourceWorld = source;
			added.Source = sender->Address;
			added.Producer = producer;
			state.Destinations.push_back(std::move(added));
			entry = std::prev(state.Destinations.end());
		}
		if (state.Next == std::numeric_limits<uint64_t>::max()) return false;
		const auto revision = now < entry->Expires ? entry->Snapshot.Revision : 0;
		const auto payload = Control(REQUEST, revision);
		const auto correlation = state.Next++;
		if (state.Universe.SendPresentation(
				entry->SourceWorld, entry->Source, producer, correlation, payload
			) != world::PresentationStatus::Ok)
			return false;
		entry->Pending = correlation;
		entry->Deadline = now + LIFETIME;
		entry->RefreshAt = now + REFRESH;
		return true;
	}
	void PortalTopologyHost::Pump(Time now) {
		auto &state = *State;
		if (!state.Clock(now)) return;
		state.Prune();
		for (auto &producer : state.Producers) {
			const auto requests = state.Universe.TakePresentation(producer.Address);
			if (requests.empty()) continue;
			if (!state.Capture(producer)) continue;
			for (const auto &request : requests) {
				const auto known = Revision(request.Payload, REQUEST);
				if (!known || request.Correlation == 0 || !ReplyChannel(request.From.Channel)) continue;
				const auto renewal = Control(RENEWAL, producer.Topology.Revision);
				const auto &bytes = *known == producer.Topology.Revision ? renewal : producer.Bytes;
				(void)state.Universe.SendPresentation(
					producer.World, producer.Address, request.From, request.Correlation, bytes
				);
			}
		}
		for (const auto &source : state.Sources) {
			for (const auto &reply : state.Universe.TakePresentation(source.Address)) {
				auto entry = std::find_if(
					state.Destinations.begin(), state.Destinations.end(), [&](const auto &known) {
						return known.Pending != 0 && known.Pending == reply.Correlation &&
							   known.Source == reply.To && known.Producer == reply.From &&
							   now < known.Deadline;
					}
				);
				if (entry == state.Destinations.end()) continue;
				if (const auto renewal = Revision(reply.Payload, RENEWAL)) {
					// The authenticated pending request bounds freshness. Cache expiry hides the old
					// snapshot while waiting, but cannot invalidate the producer's new confirmation.
					if (*renewal == 0 || *renewal != entry->Snapshot.Revision) continue;
				} else {
					scene::CameraPortalTopology topology;
					std::string error;
					if (!scene::DecodeCameraPortalTopology(reply.Payload, topology, error) ||
						topology.World != reply.From.World || topology.Revision < entry->Snapshot.Revision)
						continue;
					PortalTopologySnapshot snapshot;
					snapshot.Revision = topology.Revision;
					if (!scene::ResolveCameraPortalTopology(topology, snapshot.Seams, error)) continue;
					entry->Snapshot = std::move(snapshot);
				}
				entry->Expires = now + LIFETIME;
				entry->Pending = 0;
			}
		}
	}
	const PortalTopologySnapshot *PortalTopologyHost::Snapshot(world::WorldId destination, Time now) const {
		const auto &state = *State;
		if (state.Last && now < *state.Last) return nullptr;
		for (const auto &entry : state.Destinations)
			if (entry.World == destination && entry.Snapshot.Revision != 0 && now < entry.Expires &&
				state.Current(entry))
				return &entry.Snapshot;
		return nullptr;
	}
	void PortalTopologyHost::RemoveWorld(world::WorldId world) {
		auto &state = *State;
		std::erase_if(state.Destinations, [&](const auto &entry) {
			return entry.World == world || entry.SourceWorld == world;
		});
		std::erase_if(state.Producers, [&](const auto &producer) {
			if (producer.World != world) return false;
			(void)state.Universe.ClosePresentation(producer.Address);
			return true;
		});
		std::erase_if(state.Sources, [&](const auto &source) {
			if (source.World != world) return false;
			(void)state.Universe.ClosePresentation(source.Address);
			return true;
		});
	}
	void PortalTopologyHost::Clear() {
		while (!State->Producers.empty())
			RemoveWorld(State->Producers.back().World);
		while (!State->Sources.empty())
			RemoveWorld(State->Sources.back().World);
		State->Destinations.clear();
	}
}
