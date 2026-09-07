#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {
	struct PortalImageHost::Impl {
		Impl(world::Universe &universe, Renderer &renderer)
			: Universe(universe), Render(renderer), Resident(renderer), Topology(universe) {}
		world::Universe &Universe;
		bool EyePipelineInstalled = false;
		Renderer &Render;
		PortalResidentImages Resident;
		PortalTopologyHost Topology;
		struct DemandedPortal {
			core::Name Name;
			world::WorldId Destination;
		};
		struct Composition {
			core::Name Portal;
			uint64_t Image = 0;
			world::PresentationAddress Producer;
		};
		struct Source {
			world::WorldId World;
			size_t Slot;
			world::PresentationAddress Address;
			std::unique_ptr<PortalImageSource> Runtime;
			std::vector<DemandedPortal> Portals;
			std::vector<Composition> Compositions;
		};
		void RetireCompositions(Source &source, bool all = false) {
			std::erase_if(source.Compositions, [&](const Composition &image) {
				if (!all) {
					const auto capture = source.Runtime->Capture(image.Portal.Text());
					if (capture && capture->TransparentImages[0] != 0 && capture->Producer == image.Producer)
						return false;
				}
				Render.DropPortalImage(image.Image);
				return true;
			});
		}
		struct Producer {
			world::WorldId World;
			world::PresentationAddress Address;
			std::unique_ptr<PortalImageProducer> Runtime;
		};
		std::vector<Source> Sources;
		std::vector<Producer> Producers;
		std::vector<world::PresentationOutbound> DriverOutbound;
		std::vector<world::WorldId> DriverRouteWorlds;
		std::optional<Time> LastTime;
		bool Clock(Time now) {
			if ((LastTime && now < *LastTime) || now > Time::max() - std::chrono::seconds(1)) {
				return false;
			}
			LastTime = now;
			return true;
		}
		world::PresentationAddress Destination(const PortalImageDestination &destination) {
			if (Universe.IsRemote(destination.World)) {
				return Universe.LookupPresentation(destination.World, PORTAL_REQUEST_CHANNEL);
			}
			for (const auto &producer : Producers) {
				if (producer.World == destination.World) {
					return producer.Address;
				}
			}
			if (Producers.size() >= MAX_IMPORTED_PORTAL_IMAGES) {
				return {};
			}
			auto opened = Universe.OpenPresentation(destination.World, core::Name(PORTAL_REQUEST_CHANNEL));
			if (opened.Status != world::PresentationStatus::Ok) {
				return {};
			}
			auto runtime = std::make_unique<PortalImageProducer>(
				Universe, Render, destination.World, opened.Address, &Resident
			);
			Producers.push_back({destination.World, opened.Address, std::move(runtime)});
			return opened.Address;
		}
	};
	PortalImageHost::PortalImageHost(world::Universe &universe, Renderer &renderer)
		: State(std::make_unique<Impl>(universe, renderer)) {}
	PortalImageHost::~PortalImageHost() {
		Clear();
	}
	world::PresentationStatus
	PortalImageHost::AcceptDriverRoutes(const world::PresentationDirectory &directory) {
		auto &state = *State;
		core::ByteWriter validation;
		if (!world::WritePresentationRoutes(validation, directory)) return world::PresentationStatus::Invalid;
		const size_t retained = state.DriverRouteWorlds.size();
		for (const auto &address : directory.Endpoints) {
			const core::Name name(address.World);
			if (state.Universe.Find(name).IsValid()) continue;
			world::WorldSettings settings;
			settings.Name = name;
			const auto remote = state.Universe.CreateRemote(settings, core::Name("presentation-driver"));
			if (remote.IsValid()) state.DriverRouteWorlds.push_back(remote);
		}
		const auto status = state.Universe.AcceptPresentationRoutesFromDriver(directory);
		if (status != world::PresentationStatus::Ok) {
			core::Metrics::Count("render.portal_driver.refused", 1);
			for (size_t at = retained; at < state.DriverRouteWorlds.size(); ++at)
				(void)state.Universe.Destroy(state.DriverRouteWorlds[at]);
			state.DriverRouteWorlds.resize(retained);
			return status;
		}
		std::erase_if(state.DriverRouteWorlds, [&](const auto remote) {
			const auto name = state.Universe.NameOf(remote).Text();
			if (std::any_of(
					directory.Endpoints.begin(), directory.Endpoints.end(), [name](const auto &address) {
						return address.World == name;
					}
				))
				return false;
			RemoveWorld(remote);
			(void)state.Universe.Destroy(remote);
			return true;
		});
		return world::PresentationStatus::Ok;
	}
	bool PortalImageHost::PumpDriverLink(world::HostLink &link) {
		auto &state = *State;
		if (!link.Connected()) {
			state.Universe.RetirePresentationHost(core::Name("presentation-driver"));
			Clear();
			return false;
		}
		std::vector<world::HostFrame> frames;
		link.Receive(frames);
		for (const auto &frame : frames) {
			if (frame.Signal == world::HostSignal::Stop) {
				Clear();
				(void)link.PublishPresentationDirectory(state.Universe.LocalPresentationDirectory());
				return false;
			}
			if (frame.Signal == world::HostSignal::PresentationRoutes) {
				(void)AcceptDriverRoutes(frame.Directory);
			} else if (frame.Signal == world::HostSignal::Presentation) {
				if (state.Universe.AcceptPresentationFromDriver(frame.Presentation) !=
					world::PresentationStatus::Ok)
					core::Metrics::Count("render.portal_driver.refused", 1);
			} else {
				core::Metrics::Count("render.portal_driver.refused", 1);
			}
		}
		if (!link.Connected()) {
			state.Universe.RetirePresentationHost(core::Name("presentation-driver"));
			Clear();
			return false;
		}
		if (!link.PublishPresentationDirectory(state.Universe.LocalPresentationDirectory())) return true;
		if (state.DriverOutbound.empty()) state.DriverOutbound = state.Universe.TakePresentationOutbound();
		size_t sent = 0;
		for (const auto &outgoing : state.DriverOutbound) {
			const auto &message = outgoing.Message;
			if (state.Universe.LookupPresentation(
					state.Universe.Find(core::Name(message.From.World)), message.From.Channel
				) != message.From ||
				state.Universe.LookupPresentation(
					state.Universe.Find(core::Name(message.To.World)), message.To.Channel
				) != message.To) {
				++sent;
				continue;
			}
			if (!link.SendPresentation(outgoing.Message)) break;
			++sent;
		}
		state.DriverOutbound.erase(state.DriverOutbound.begin(), state.DriverOutbound.begin() + sent);
		return true;
	}
	size_t PortalImageHost::SubmitEye(
		world::WorldId source,
		const PortalImageDestination &destination,
		View &view,
		const PortalImageDemandSettings &settings,
		Time now,
		const PortalEyeGeometrySource &geometry
	) {
		auto &state = *State;
		const auto sourceName = state.Universe.NameOf(source);
		if (!sourceName.IsValid() || state.Universe.IsRemote(source)) return 0;
		PortalImageDemand demand;
		const core::Name key("viewport-eye");
		if (BuildPortalEyeDemand(key, view, settings, demand) != PortalDemandStatus::Ready) return 0;
		const core::Name pipelineName("product-eye-view");
		if (!state.EyePipelineInstalled) {
			graph::RenderGraph pipeline;
			core::Name offender;
			if (graph::Build(graph::DefaultEyeDocument(), pipeline, offender) !=
					graph::PipelineDocumentStatus::Ok ||
				!state.Render.SetPipeline(pipelineName, pipeline))
				return 0;
			state.EyePipelineInstalled = true;
		}
		view.World = source.Index;
		view.WorldName = sourceName;
		view.Pipeline = pipelineName;
		view.EyeImageKey = key;
		view.EyeImage = 0;
		if (!destination.Authored.IsValid() || !state.Universe.NameOf(destination.World).IsValid()) {
			RemoveViewport(view.Slot);
			return 0;
		}
		if (!geometry.Instances.empty()) {
			bool ready = false;
			std::string error = "source presentation world unavailable";
			if (geometry.World.IsValid() && !state.Universe.IsRemote(geometry.World))
				state.Universe.Enter(geometry.World, [&](ecs::Store &store) {
					ready = CollectPortalEyeGeometry(
						store,
						destination.Authored,
						geometry.Instances,
						geometry.JointFrames,
						demand.Request.Geometry,
						error
					);
				});
			if (!ready) {
				core::Metrics::Count("render.portal_eye.geometry_refused", 1);
				ENGINE_DEBUG("eye geometry for viewport {} refused: {}", view.Slot, error);
				view.EyeImage = Image(view.Slot, key);
				return 0;
			}
		}
		demand.DestinationWorld = destination.Authored;
		demand.Binding.World = source.Index;
		demand.Binding.WorldName = sourceName;
		demand.Binding.ViewSlot = view.Slot;
		demand.Binding.Portal = key;
		const auto issued = Submit(source, view.Slot, std::span(&demand, 1), std::span(&destination, 1), now);
		view.EyeImage = Image(view.Slot, key);
		return issued;
	}

	world::PresentationAddress PortalImageHost::Serve(world::WorldId world) {
		if (State->Universe.IsRemote(world) || !State->Universe.NameOf(world).IsValid()) {
			return {};
		}
		const auto address = State->Destination({State->Universe.NameOf(world), world});
		if (address.Generation != 0) (void)State->Topology.Serve(world);
		return address;
	}
	bool PortalImageHost::RequestTopology(world::WorldId source, world::WorldId destination, Time now) {
		// Route selection follows this call. Consume already-delivered topology before
		// that selection, without running image captures or waiting for the network.
		State->Topology.Pump(now);
		return State->Topology.Request(source, destination, now);
	}
	const PortalTopologySnapshot *PortalImageHost::Topology(world::WorldId destination, Time now) const {
		return State->Topology.Snapshot(destination, now);
	}
	size_t PortalImageHost::Submit(
		world::WorldId source,
		size_t viewSlot,
		std::span<const PortalImageDemand> demands,
		std::span<const PortalImageDestination> destinations,
		Time now
	) {
		auto &state = *State;
		if (!state.Clock(now) || demands.size() > MAX_IMPORTED_PORTAL_IMAGES || !source.IsValid() ||
			state.Universe.IsRemote(source)) {
			return 0;
		}
		const auto sourceName = state.Universe.NameOf(source);
		if (!sourceName.IsValid()) {
			return 0;
		}
		if (demands.empty()) {
			RemoveViewport(viewSlot);
			return 0;
		}
		for (size_t index = 0; index < demands.size(); ++index) {
			const auto &demand = demands[index];
			if (demand.Binding.World != source.Index || demand.Binding.WorldName != sourceName ||
				demand.Binding.ViewSlot != viewSlot || !demand.DestinationWorld.IsValid() ||
				demand.Binding.Portal.Text() != demand.Request.Key.PortalKey) {
				return 0;
			}
			for (size_t previous = 0; previous < index; ++previous) {
				if (demands[previous].Binding.Portal == demand.Binding.Portal) {
					return 0;
				}
			}
		}
		auto found =
			std::find_if(state.Sources.begin(), state.Sources.end(), [viewSlot](const Impl::Source &entry) {
				return entry.Slot == viewSlot;
			});
		if (found != state.Sources.end() && found->World != source) {
			RemoveViewport(viewSlot);
			found = state.Sources.end();
		}
		if (found == state.Sources.end()) {
			if (state.Sources.size() >= MAX_IMPORTED_PORTAL_IMAGES) {
				return 0;
			}
			auto opened = state.Universe.OpenPresentation(source, PortalReplyChannel(viewSlot));
			if (opened.Status != world::PresentationStatus::Ok) {
				return 0;
			}
			auto runtime = std::make_unique<PortalImageSource>(
				state.Universe, state.Render, source, opened.Address, PortalInboxLimits{}, &state.Resident
			);
			state.Sources.push_back({source, viewSlot, opened.Address, std::move(runtime), {}, {}});
			found = std::prev(state.Sources.end());
		}
		for (const auto portal : found->Portals) {
			if (std::none_of(demands.begin(), demands.end(), [portal](const PortalImageDemand &demand) {
					return demand.Binding.Portal == portal.Name;
				})) {
				found->Runtime->InvalidatePortal(portal.Name.Text());
			}
		}
		found->Portals.clear();
		size_t issued = 0;
		for (const auto &demand : demands) {
			const auto destination = std::find_if(
				destinations.begin(), destinations.end(), [&](const PortalImageDestination &entry) {
					return entry.Authored == demand.DestinationWorld;
				}
			);
			if (destination == destinations.end() || !destination->World.IsValid()) {
				found->Runtime->InvalidatePortal(demand.Binding.Portal.Text());
				continue;
			}
			found->Portals.push_back({demand.Binding.Portal, destination->World});
			const auto address = state.Destination(*destination);
			if (address.Session == 0) {
				found->Runtime->InvalidatePortal(demand.Binding.Portal.Text());
				continue;
			}
			const auto result = found->Runtime->Issue(address, demand.Request, demand.Binding, now);
			issued += result.Status == PortalInboxStatus::Issued ? 1 : 0;
		}
		state.RetireCompositions(*found);
		return issued;
	}
	PortalProducerProgress
	PortalImageHost::Pump(float frameSeconds, float alpha, Time now, bool destinationPresented) {
		auto &state = *State;
		PortalProducerProgress total;
		if (!std::isfinite(frameSeconds) || frameSeconds < 0 || !std::isfinite(alpha) || alpha < 0 ||
			alpha > 1 || !state.Clock(now)) {
			return total;
		}
		for (auto &producer : state.Producers) {
			const auto result = producer.Runtime->Pump(frameSeconds, alpha, now, destinationPresented);
			total.Requests += result.Requests;
			total.Rendered += result.Rendered;
			total.Reused += result.Reused;
			total.Sent += result.Sent;
			total.Refused += result.Refused;
		}
		for (auto &source : state.Sources) {
			(void)source.Runtime->Poll(now);
			state.RetireCompositions(source);
		}
		state.Topology.Pump(now);
		return total;
	}
	bool PortalImageHost::HasPendingUploads() const {
		for (const auto &source : State->Sources)
			if (source.Runtime->HasPendingUploads()) return true;
		return false;
	}
	uint64_t PortalImageHost::Image(size_t viewSlot, core::Name portal) const {
		for (const auto &source : State->Sources) {
			if (source.Slot == viewSlot) {
				return source.Runtime->Image(portal.Text());
			}
		}
		return 0;
	}
	uint64_t PortalImageHost::ComposeBodyImage(core::Name portal, const View &body) {
		auto &state = *State;
		const auto source =
			std::find_if(state.Sources.begin(), state.Sources.end(), [&](const Impl::Source &entry) {
				return entry.Slot == body.Slot;
			});
		if (source == state.Sources.end()) return 0;
		const auto capture = source->Runtime->Capture(portal.Text());
		if (!capture) return 0;
		const auto composed = state.Render.ComposePortalBodyImage(*capture, body);
		if (composed == 0) return 0;
		const auto previous = std::find_if(
			source->Compositions.begin(), source->Compositions.end(), [&](const Impl::Composition &entry) {
				return entry.Portal == portal;
			}
		);
		if (previous == source->Compositions.end())
			source->Compositions.push_back({portal, composed, capture->Producer});
		else {
			if (previous->Image != composed) state.Render.DropPortalImage(previous->Image);
			previous->Image = composed;
			previous->Producer = capture->Producer;
		}
		return composed;
	}
	uint64_t PortalImageHost::CurrentImage(size_t viewSlot, core::Name portal) const {
		for (const auto &source : State->Sources)
			if (source.Slot == viewSlot) return source.Runtime->CurrentImage(portal.Text());
		return 0;
	}
	std::optional<PortalImageCapture> PortalImageHost::Capture(size_t viewSlot, core::Name portal) const {
		for (const auto &source : State->Sources)
			if (source.Slot == viewSlot) return source.Runtime->Capture(portal.Text());
		return {};
	}
	void PortalImageHost::RestartRequests() {
		for (auto &source : State->Sources)
			source.Runtime->RestartRequests();
		State->Topology.RestartRequests();
	}

	void PortalImageHost::RemoveViewport(size_t viewSlot) {
		std::erase_if(State->Sources, [&](Impl::Source &source) {
			if (source.Slot != viewSlot) {
				return false;
			}
			State->RetireCompositions(source, true);
			source.Runtime.reset();
			(void)State->Universe.ClosePresentation(source.Address);
			return true;
		});
	}
	void PortalImageHost::RemoveWorld(world::WorldId world) {
		State->Topology.RemoveWorld(world);
		for (auto &source : State->Sources) {
			std::erase_if(source.Portals, [&](const Impl::DemandedPortal &portal) {
				if (portal.Destination != world) {
					return false;
				}
				source.Runtime->InvalidatePortal(portal.Name.Text());
				return true;
			});
		}
		std::erase_if(State->Producers, [&](Impl::Producer &producer) {
			if (producer.World != world) {
				return false;
			}
			for (auto &source : State->Sources) {
				source.Runtime->InvalidateEndpoint(producer.Address);
			}
			producer.Runtime.reset();
			(void)State->Universe.ClosePresentation(producer.Address);
			return true;
		});
		for (auto &source : State->Sources)
			State->RetireCompositions(source);
		std::erase_if(State->Sources, [&](Impl::Source &source) {
			if (source.World != world) {
				return false;
			}
			State->RetireCompositions(source, true);
			source.Runtime.reset();
			(void)State->Universe.ClosePresentation(source.Address);
			return true;
		});
	}
	void PortalImageHost::Clear() {
		State->DriverOutbound.clear();
		while (!State->Sources.empty()) {
			RemoveViewport(State->Sources.back().Slot);
		}
		while (!State->Producers.empty()) {
			RemoveWorld(State->Producers.back().World);
		}
		State->Resident.Clear();
		State->Topology.Clear();
		for (const auto remote : State->DriverRouteWorlds)
			(void)State->Universe.Destroy(remote);
		State->DriverRouteWorlds.clear();
	}
}
