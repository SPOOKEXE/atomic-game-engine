#include <engine/game/PortalIslandScene.hpp>
#include <engine/game/PortalSeamCoordinator.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>

namespace engine::game {
	PortalSeamCoordinator::PortalSeamCoordinator(Collect collect, Resolve resolve, Apply apply)
		: Barrier{std::move(collect), std::move(resolve), std::move(apply)} {}

	bool PortalSeamCoordinator::Tick(world::Universe &worlds, float frameSeconds) {
		if (!Barrier.Valid()) return false;
		const int rounds = worlds.BeginTickExchangeFrame(frameSeconds);
		if (rounds < 0) return false;
		for (int round = 0; round < rounds; ++round) {
			std::vector<world::TickExchangeRequest> requests;
			std::vector<world::TickExchangeReply> replies;
			std::vector<world::FixedStepBarrierRecord> records;
			std::vector<world::FixedStepBarrierRecord> results;
			if (!worlds.BeginTickExchangeRound() || !worlds.CollectTickExchangeRequests(requests) ||
				!worlds.ServeTickExchangeRequests(requests, replies) ||
				!worlds.ApplyTickExchangeReplies(replies) || !worlds.AdvanceTickExchangeRoundToPhysics() ||
				!worlds.CollectFixedStepBarrier(
					[&](world::WorldId, ecs::Store &store, std::vector<std::byte> &out) {
						Barrier.Collect(store, out);
					},
					records
				) ||
				!Barrier.Resolve(records, results) ||
				!worlds.ApplyFixedStepBarrier(
					[&](world::WorldId, ecs::Store &store, std::span<const std::byte> bytes) {
						return Barrier.Apply(store, bytes);
					},
					results
				) ||
				!worlds.FinishTickExchangeRound()) {
				worlds.CancelTickExchangeFrame();
				return false;
			}
		}
		return worlds.EndTickExchangeFrame();
	}

	PortalSeamCoordinator MakePortalIslandCoordinator() {
		return PortalSeamCoordinator(
			[](ecs::Store &store, std::vector<std::byte> &out) {
				std::vector<physics::PortalIslandBody> bodies;
				CollectPortalIslandBodies(store, bodies);
				physics::PortalIslandPacket packet{std::move(bodies), {}};
				CollectPortalIslandContacts(store, packet);
				if (packet.Bodies.empty()) return;
				(void)physics::WritePortalIslandPacket(out, packet);
			},
			[](std::span<const world::FixedStepBarrierRecord> records,
			   std::vector<world::FixedStepBarrierRecord> &out) {
				std::vector<physics::PortalIslandPacket> packets(records.size());
				std::vector<physics::PortalIslandBody> bodies;
				std::vector<physics::PortalIslandContact> contacts;
				for (size_t at = 0; at < records.size(); ++at) {
					if (!physics::ReadPortalIslandPacket(records[at].Payload, packets[at])) return false;
					bodies.insert(bodies.end(), packets[at].Bodies.begin(), packets[at].Bodies.end());
					contacts.insert(contacts.end(), packets[at].Contacts.begin(), packets[at].Contacts.end());
				}
				std::sort(bodies.begin(), bodies.end(), [](const auto &left, const auto &right) {
					return left.Id < right.Id;
				});
				bodies.erase(
					std::unique(
						bodies.begin(),
						bodies.end(),
						[](const auto &left, const auto &right) { return left.Id == right.Id; }
					),
					bodies.end()
				);
				std::vector<physics::PortalIslandResult> results;
				if (physics::SolvePortalIsland(bodies, std::move(contacts), results) !=
					physics::PortalIslandStatus::Complete)
					return false;
				out.clear();
				for (size_t at = 0; at < records.size(); ++at) {
					physics::PortalIslandPacket reply;
					for (const auto &body : packets[at].Bodies) {
						if (!body.Owned) continue;
						auto found = std::lower_bound(
							results.begin(), results.end(), body.Id, [](const auto &result, const auto &id) {
								return result.Id < id;
							}
						);
						if (found == results.end() || found->Id != body.Id) return false;
						physics::PortalIslandBody updated = body;
						updated.LinearVelocity = found->LinearVelocity;
						updated.AngularVelocity = found->AngularVelocity;
						reply.Bodies.push_back(updated);
					}
					std::vector<std::byte> bytes;
					if (!physics::WritePortalIslandPacket(bytes, reply)) return false;
					out.push_back({records[at].World, std::move(bytes)});
				}
				return true;
			},
			[](ecs::Store &store, std::span<const std::byte> bytes) {
				if (bytes.empty()) return true;
				physics::PortalIslandPacket packet;
				if (!physics::ReadPortalIslandPacket(bytes, packet) || !packet.Contacts.empty()) return false;
				std::vector<physics::PortalIslandResult> results;
				results.reserve(packet.Bodies.size());
				for (const auto &body : packet.Bodies)
					results.push_back({body.Id, body.LinearVelocity, body.AngularVelocity});
				return ApplyPortalIslandResults(store, results);
			}
		);
	}
}
