// PortalImageProducer construction, shadow access, rename idleness and clearing.
#include "portal/PortalImageProducerState.hpp"

namespace engine::render {
	PortalImageProducer::PortalImageProducer(
		world::Universe &universe,
		Renderer &renderer,
		world::WorldId world,
		world::PresentationAddress requests,
		PortalResidentImages *resident,
		ShaderLibrary *shaders,
		bool postProcessing
	)
		: State(
			  std::make_unique<Impl>(
				  universe, renderer, world, std::move(requests), universe.NameOf(world), resident
			  )
		  ) {
		State->Shaders = shaders;
		State->PostProcessing = postProcessing;
	}
	PortalImageProducer::~PortalImageProducer() {
		Clear();
	}
	void PortalImageProducer::SetRetainedBodyAuthorization(RetainedBodyAuthorization authorize) {
		Clear();
		State->AuthorizeRetainedBody = std::move(authorize);
	}
	std::optional<PortalShadowImage> PortalImageProducer::TakeShadow(
		const world::PresentationAddress &requester, const PortalExchangeKey &eye, Time now
	) {
		auto &state = *State;
		if (!Clock(state.LastTime, now)) return {};
		state.ExpireShadows(now);
		for (auto &shadow : state.Shadows) {
			if (!shadow || !shadow->Ready || shadow->Requester != requester ||
				shadow->Image.Snapshot.Eye != eye)
				continue;
			core::Metrics::Count("render.portal_shadow.taken_bytes", shadow->Image.Depth.size());
			auto image = std::move(shadow->Image);
			state.ReleaseProducedShadow(shadow);
			return image;
		}
		return {};
	}
	PortalProducerShadowUsage PortalImageProducer::ShadowUsage() const {
		PortalProducerShadowUsage usage;
		for (const auto &shadow : State->Shadows) {
			if (!shadow) continue;
			++usage.Images;
			usage.Ready += shadow->Ready;
			usage.ReservedBytes += PORTAL_SHADOW_BYTES;
			usage.CpuBytes += shadow->Image.Depth.capacity() + Impl::ShadowSnapshotBytes(*shadow);
			if (shadow->FitImage) usage.CpuBytes += shadow->FitImage->Depth.capacity();
		}
		const auto addressBytes = [](const auto &address) {
			return address.World.capacity() + address.Channel.capacity();
		};
		for (const auto &routes : State->ShadowRoutes) {
			if (!routes) continue;
			++usage.Routes;
			usage.ReadyRoutes += routes->Ready;
			usage.RouteMetadataBytes += sizeof(Impl::ProducedShadowRoutes) + addressBytes(routes->Requester) +
										addressBytes(routes->Producer) +
										routes->ParentEye.PortalKey.capacity() +
										routes->RetainedBodyPlayer.capacity();
			for (const auto &target : std::span(routes->Targets).first(routes->Count))
				usage.RouteMetadataBytes +=
					addressBytes(target.Producer) + target.Eye.PortalKey.capacity() +
					addressBytes(target.Hop.Requester) + addressBytes(target.Hop.Producer) +
					target.Hop.ParentEye.PortalKey.capacity() + addressBytes(target.HopProducer);
		}
		for (const auto &transfer : State->Transfers)
			usage.Transfers += transfer.has_value();
		usage.TransferPacketBytes = State->TransferBytes();
		return usage;
	}
	std::optional<PortalShadowRoute> PortalImageProducer::ResolveShadowRoute(
		const world::PresentationAddress &parentRequester,
		const PortalExchangeKey &parentEye,
		const PortalCaptureTreeEndpoint &targetProducer,
		const PortalExchangeKey &targetEye,
		Time now
	) {
		auto &state = *State;
		if (!Clock(state.LastTime, now)) return {};
		state.ExpireShadows(now);
		for (const auto &routes : state.ShadowRoutes) {
			if (!routes || !routes->Ready || routes->Requester != parentRequester ||
				routes->ParentEye != parentEye)
				continue;
			for (const auto &target : std::span(routes->Targets).first(routes->Count))
				if (target.Producer == targetProducer && target.Eye == targetEye) return target.Hop;
		}
		return {};
	}
	void PortalImageProducer::SetEndpointBindings(const world::PresentationBindings *bindings) {
		if (State->EndpointBindings == bindings) return;
		Clear();
		State->EndpointBindings = bindings;
	}
	void PortalImageProducer::SetContentOwner(core::Name owner, std::span<const WorldContentOwner> foreign) {
		if (State->ContentOwner != owner) {
			State->EditableMeshes.ForgetOwner(State->ContentOwner);
		}
		State->ContentOwner = owner;
		State->ForeignContentOwners.assign(foreign.begin(), foreign.end());
	}

	bool PortalImageProducer::HasPendingShadowFits() const {
		return std::any_of(State->Shadows.begin(), State->Shadows.end(), [](const auto &shadow) {
			return shadow && shadow->FitToken != 0;
		});
	}
	bool PortalImageProducer::RenameIdle() const {
		if (!State->Captures.empty() || !State->FailureReplies.empty()) return false;
		for (const auto &shadow : State->Shadows)
			if (shadow) return false;
		for (const auto &route : State->ShadowRoutes)
			if (route) return false;
		for (const auto &transfer : State->Transfers)
			if (transfer) return false;
		for (const auto &slot : State->Pending)
			if (slot.Work || (slot.Source && !slot.Source->RenameIdle())) return false;
		return true;
	}
	bool PortalImageProducer::RenameIdleFor(std::string_view world) const {
		if (State->Requests.World == world) return RenameIdle();
		for (const auto &capture : State->Captures) {
			// A leaf has no child producers; an ordered tree records all of them.
			if (!capture.WorldDependenciesKnown || (!capture.Tree && !capture.WorldProvenanceComplete) ||
				capture.ReplyTo.World == world || capture.Producer.World == world ||
				std::find(capture.WorldDependencies.begin(), capture.WorldDependencies.end(), world) !=
					capture.WorldDependencies.end() ||
				(capture.Tree && std::any_of(
									 capture.Tree->Nodes.begin(),
									 capture.Tree->Nodes.end(),
									 [world](const auto &node) { return node.Producer.World == world; }
								 )))
				return false;
		}
		const auto named = [world](core::Name name) { return name.IsValid() && name.Text() == world; };
		for (const auto &shadow : State->Shadows) {
			if (!shadow) continue;
			const View &view = shadow->Snapshot;
			if (shadow->Requester.World == world || shadow->Image.Snapshot.Producer.World == world ||
				named(view.WorldName) || named(view.ImageGraphWorldName) || named(view.ParticleWorldName) ||
				named(view.ContentOwner) || (view.LensContentOwner && named(*view.LensContentOwner)) ||
				std::any_of(
					shadow->ContentOwners.begin(),
					shadow->ContentOwners.end(),
					[&](const auto &binding) { return named(binding.World) || named(binding.Owner); }
				) ||
				std::any_of(
					shadow->Rows.begin(),
					shadow->Rows.end(),
					[&](const auto &row) { return named(row.SourceWorld); }
				) ||
				std::any_of(
					shadow->ForeignRows.begin(),
					shadow->ForeignRows.end(),
					[&](const auto &row) { return named(row.SourceWorld); }
				) ||
				std::any_of(shadow->Particles.begin(), shadow->Particles.end(), [&](const auto &particle) {
					return named(particle.SourceWorld);
				}))
				return false;
		}
		for (const auto &route : State->ShadowRoutes) {
			if (!route) continue;
			if (route->Requester.World == world || route->Producer.World == world) return false;
			for (size_t index = 0; index < route->Count; ++index) {
				const auto &target = route->Targets[index];
				if (target.Producer.World == world || target.HopProducer.World == world ||
					target.Hop.Requester.World == world || target.Hop.Producer.World == world)
					return false;
			}
		}
		for (const auto &transfer : State->Transfers) {
			if (!transfer) continue;
			if (transfer->Requester.World == world || transfer->Producer.World == world ||
				transfer->Pull.TargetProducer.World == world ||
				transfer->Forwarded.TargetProducer.World == world || transfer->Hop.Requester.World == world ||
				transfer->Hop.Producer.World == world)
				return false;
			for (size_t index = 0; index < transfer->CancelCount; ++index) {
				const auto &target = transfer->CancelHops[index];
				if (target.Producer.World == world || target.HopProducer.World == world ||
					target.Hop.Requester.World == world || target.Hop.Producer.World == world)
					return false;
			}
		}
		for (const auto &slot : State->Pending) {
			if (slot.Work) {
				const auto &job = *slot.Work;
				const auto &capture = job.Output;
				if (!job.Collected || !capture.WorldDependenciesKnown || capture.ReplyTo.World == world ||
					capture.Producer.World == world || named(State->ContentOwner) ||
					std::find(capture.WorldDependencies.begin(), capture.WorldDependencies.end(), world) !=
						capture.WorldDependencies.end() ||
					(job.Request.Entrance && job.Request.Entrance->SourceWorld == world) ||
					std::any_of(
						State->ForeignContentOwners.begin(),
						State->ForeignContentOwners.end(),
						[&](const auto &binding) { return named(binding.World) || named(binding.Owner); }
					))
					return false;
				for (const auto &child : job.Children) {
					if ((child.Demand.Request.RecursionDepth != 0 && !child.Tree) ||
						child.Producer.World == world || child.Demand.DestinationWorld.Text() == world ||
						(child.Demand.Request.Entrance &&
						 child.Demand.Request.Entrance->SourceWorld == world) ||
						(child.Tree && std::any_of(
										   child.Tree->Nodes.begin(),
										   child.Tree->Nodes.end(),
										   [world](const auto &node) { return node.Producer.World == world; }
									   )))
						return false;
				}
			}
			if (slot.Source && !slot.Source->RenameIdleForUnrelatedRoute(world)) return false;
		}
		for (const auto &reply : State->FailureReplies)
			if (reply.ReplyTo.World == world || reply.Producer.World == world ||
				(reply.Tree &&
				 std::any_of(reply.Tree->Nodes.begin(), reply.Tree->Nodes.end(), [world](const auto &node) {
					 return node.Producer.World == world;
				 })))
				return false;
		return true;
	}
	void PortalImageProducer::AppendWorldReceipts(std::vector<world::PresentationAddress> &receipts) const {
		for (const auto &slot : State->Pending)
			if (slot.Replies.Generation != 0 &&
				State->Universe.LookupPresentation(State->World, slot.Replies.Channel) == slot.Replies)
				receipts.push_back(slot.Replies);
	}

	void PortalImageProducer::Clear() {
		for (auto &transfer : State->Transfers)
			transfer.reset();
		if (State->Resident != nullptr) {
			State->Resident->Invalidate(State->Requests);
		}
		for (auto &capture : State->Captures) {
			State->Cancel(capture);
		}
		State->Captures.clear();
		State->FailureReplies.clear();
		for (auto &shadow : State->Shadows)
			State->ReleaseProducedShadow(shadow);
		for (auto &routes : State->ShadowRoutes)
			routes.reset();
		for (auto &pending : State->Pending) {
			pending.Source.reset();
			if (pending.Work) State->Cancel(pending.Work->Output);
			pending.Work.reset();
			if (pending.Replies.Generation != 0) (void)State->Universe.ClosePresentation(pending.Replies);
			pending.Replies = {};
		}
		State->Render.ForgetWorld(State->World.Index, State->OwnerName);
		State->EditableImages.ForgetOwner(State->OwnerName);
		State->EditableMeshes.ForgetOwner(State->ContentOwner);
		State->PipelineReady = {};
		for (auto &frame : State->Frames) {
			frame.Interface.Shutdown();
			frame.InterfaceReady = false;
			frame.Camera.Compiled.Invalidate();
		}
	}
}
