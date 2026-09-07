#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/world/Driver.hpp>

#include <algorithm>

namespace engine::world {

	Driver::Driver(const DriverSettings &settings)
		: Settings_(settings), Universe_(settings.Universe), LocalExchange(Universe_),
		  Supervisor_(settings.Hosts) {}

	Driver::~Driver() {
		Stop();
	}

	size_t Driver::Start(const std::vector<WorldSettings> &remote) {
		const std::vector<HostPlan> plans = Settings_.Hosts.SharedHosts > 0
												? PlanHostsAcross(remote, Settings_.Hosts.SharedHosts)
												: PlanHosts(remote, Settings_.Hosts.WorldsPerHost);

		// Registered before anything is spawned, so the buses can address a
		// remote world from the first barrier rather than from whenever its
		// host happens to answer. A subscription or a teleport arriving in
		// between is queued for it like any other.
		for (const HostPlan &plan : plans) {
			for (const core::Name world : plan.Worlds) {
				const auto found =
					std::find_if(remote.begin(), remote.end(), [world](const WorldSettings &settings) {
						return settings.Name == world;
					});
				if (found == remote.end()) {
					continue;
				}

				WorldStatus status = WorldStatus::Ok;
				Universe_.CreateRemote(*found, plan.Name, &status);
				if (status != WorldStatus::Ok) {
					ENGINE_ERROR(
						"driver: could not register remote world '{}': {}", world.Text(), Describe(status)
					);
				}
			}
		}

		return Supervisor_.Start(plans);
	}

	void Driver::Tick(float frameSeconds, double now) {
		ENGINE_PROFILE_CAT("Driver::Tick", core::ProfileCategory::Simulation);

		Stats = DriverStatistics{};

		// --- 1. what the hosts said ---
		Stats.FramesReceived = Supervisor_.Pump(now);

		// --- 2. their traffic, checked against who sent it ---
		//
		// Grouped by host so the check runs per host: the driver's directory
		// says which worlds a host holds, and an envelope claiming to come from
		// any other world is refused rather than routed. The traffic is already
		// in arrival order and hosts are drained one at a time, so a host's
		// slice is contiguous and the grouping is a scan rather than a sort.
		{
			const std::vector<HostTraffic> traffic = Supervisor_.TakeTraffic();

			size_t index = 0;
			while (index < traffic.size()) {
				const core::Name host = traffic[index].Host;

				size_t end = index;
				Envelopes.clear();
				while (end < traffic.size() && traffic[end].Host == host) {
					Envelopes.push_back(traffic[end].Message);
					end++;
				}

				const size_t accepted = Universe_.IngestTraffic(host, Envelopes);
				Stats.TrafficAccepted += accepted;
				Stats.TrafficRefused += Envelopes.size() - accepted;
				index = end;
			}
		}

		// --- 3. one barrier, local and remote worlds together ---
		if (Settings_.CoordinateHostTicks)
			Stats.TickExchangeFailed = !TickHosts(frameSeconds, now);
		else
			Universe_.Tick(frameSeconds);

		// --- 4. what the barrier decided for worlds that live elsewhere ---
		const std::vector<RemoteDelivery> outbound = Universe_.TakeOutbound();
		if (!outbound.empty()) {
			// Grouped by host, so a host with twenty deliveries gets one frame
			// rather than twenty. The list is already in barrier order and the
			// grouping is stable, so each host's slice keeps that order.
			core::Name current;
			Batch.clear();

			const auto flush = [this, &current] {
				if (Batch.empty()) {
					return;
				}
				if (Supervisor_.DeliverTo(current, Batch)) {
					Stats.DeliveriesSent += Batch.size();
				} else {
					// A host that died between the barrier and this line. The
					// deliveries go nowhere, and it is worth counting rather
					// than swallowing: a restored host restores from a snapshot
					// and these were never in one.
					Stats.DeliveriesDropped += Batch.size();
				}
				Batch.clear();
			};

			for (const RemoteDelivery &delivery : outbound) {
				if (delivery.Host != current) {
					flush();
					current = delivery.Host;
				}
				Batch.push_back(HostDelivery{delivery.World, delivery.Message});
			}
			flush();
		}

		// --- 5. restart what died ---
		Stats.Restarted = Supervisor_.Poll(now);

		// --- what the hosts said their tick cost ---
		//
		// **A driver cannot time another address space**, so it plots the
		// latest figure each host measured and sent. That is a barrier behind,
		// which is the price of the only honest number available - and being a
		// barrier behind about a host beats the alternative, which is a frame
		// graph on which a process holding half the universe does not appear.
		for (const HostStatus &host : Supervisor_.Hosts()) {
			if (host.Milliseconds > 0.0f) {
				core::FrameGraph::ReportNamed(
					"host", host.Name.Text(), core::ProfileCategory::ECS, host.Milliseconds
				);
			}
		}
	}

	PresentationPumpResult Driver::PumpPresentation(double now) {
		ENGINE_PROFILE_CAT("Driver::PumpPresentation", core::ProfileCategory::Render);
		PresentationPumpResult result;
		(void)Supervisor_.Pump(now);
		const uint64_t totalDrops = Supervisor_.PresentationDropped();
		result.Dropped = totalDrops - ObservedPresentationDrops;
		ObservedPresentationDrops = totalDrops;
		for (const auto host : Supervisor_.TakeReplacedPresentationHosts()) {
			Universe_.RetirePresentationHost(host);
		}
		for (const auto &host : Supervisor_.Hosts()) {
			if (!host.Linked) Universe_.RetirePresentationHost(host.Name);
		}
		for (const auto &incoming : Supervisor_.TakePresentationDirectories()) {
			if (Universe_.ApplyPresentationDirectory(incoming.Host, incoming.Directory) !=
				PresentationStatus::Ok) {
				result.Refused++;
			}
		}
		for (const HostPresentation &incoming : Supervisor_.TakePresentationTraffic()) {
			if (Universe_.IngestPresentation(incoming.Host, incoming.Message) == PresentationStatus::Ok) {
				result.Accepted++;
				result.AcceptedPayloadBytes += incoming.Message.Payload.size();
			} else {
				result.Refused++;
			}
		}
		bool routesReady = true;
		for (const auto &host : Supervisor_.Hosts()) {
			if (Supervisor_.WantsPresentationRoutes(host.Name)) {
				routesReady = Supervisor_.PublishPresentationRoutes(
								  host.Name, Universe_.PresentationRoutesFor(host.Name)
							  ) &&
							  routesReady;
			}
		}
		// Keep the bounded image queue owned by the bus until route control is queued first.
		if (!routesReady) return result;
		for (const PresentationOutbound &outbound : Universe_.TakePresentationOutbound()) {
			if (Supervisor_.SendPresentation(outbound.Host, outbound.Message)) {
				result.Sent++;
				result.SentPayloadBytes += outbound.Message.Payload.size();
			} else {
				result.Dropped++;
			}
		}
		return result;
	}

	WorldStatus Driver::Present(WorldId id, float frameSeconds, float alpha) {
		if (Universe_.IsRemote(id)) {
			// A remote world draws in its own host, or does not draw at all.
			// Presenting it here would run its `PreRender` phase against an
			// empty store, which is a frame of nothing rather than an error and
			// is therefore worth refusing loudly.
			return WorldStatus::NoSuchWorld;
		}
		return Universe_.Present(id, frameSeconds, alpha);
	}

	void Driver::Stop() {
		Supervisor_.StopAll();
	}
}
