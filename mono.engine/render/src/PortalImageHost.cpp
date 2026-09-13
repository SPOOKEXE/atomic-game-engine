#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/PortalImageHost.hpp>
#include <engine/render/PortalResidentImages.hpp>
#include <engine/render/PortalShadowTransport.hpp>
#include <engine/render/ShaderLibrary.hpp>
#include <engine/world/HostLink.hpp>
#include <engine/world/Universe.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {
	struct PortalImageHost::Impl {
		Impl(world::Universe &universe, Renderer &renderer, ShaderLibrary *shaders, bool postProcessing)
			: Universe(universe), Render(renderer), Resident(renderer), Topology(universe), Shaders(shaders) {
			PostProcessing = postProcessing;
		}
		world::Universe &Universe;
		bool EyePipelineInstalled = false;
		Renderer &Render;
		PortalResidentImages Resident;
		PortalTopologyHost Topology;
		std::unique_ptr<ShaderLibrary> OwnedShaders;
		ShaderLibrary *Shaders = nullptr;
		bool PostProcessing = true;
		struct DemandedPortal {
			core::Name Name;
			world::WorldId Destination;
		};
		struct ShadowRoute {
			uint64_t Tree = 0;
			world::WorldId World;
			world::PresentationAddress Address, Producer;
			PortalExchangeKey Eye;
			Time Deadline{};
			PortalCaptureTreeEndpoint PublishedProducer;
		};
		struct Composition {
			core::Name Portal;
			uint64_t Image = 0;
			world::PresentationAddress Producer;
			std::optional<ShadowRoute> Route;
			uint64_t Preparation = 0;
			Time Deadline{};
			world::PresentationAddress Address;
		};
		struct Source {
			world::WorldId World;
			size_t Slot;
			world::PresentationAddress Address;
			std::unique_ptr<PortalImageSource> Runtime;
			std::vector<DemandedPortal> Portals;
			std::vector<Composition> Compositions;
		};
		void RetireCompositions(
			Source &source, bool all = false, const std::vector<core::Name> *revokedPortals = nullptr
		) {
			if (BodyJob && BodyJob->Slot == source.Slot && (all || !BodySource()))
				AbortBody(LastTime.value_or(Time{}));
			std::erase_if(source.Compositions, [&](const Composition &image) {
				const bool revoked =
					revokedPortals != nullptr &&
					std::find(revokedPortals->begin(), revokedPortals->end(), image.Portal) !=
						revokedPortals->end();
				if (!all && !revoked) {
					const auto prepared =
						std::find_if(PreparedBodies.begin(), PreparedBodies.end(), [&](const auto &entry) {
							return entry.PreparationToken == image.Preparation &&
								   LastTime.value_or(Time{}) < entry.Deadline &&
								   CurrentEndpoint(entry.Address) && CurrentEndpoint(entry.Producer);
						});
					if (prepared != PreparedBodies.end()) return false;
					if (image.Preparation && LastTime.value_or(Time{}) < image.Deadline &&
						CurrentEndpoint(image.Address) && CurrentEndpoint(image.Producer))
						return false;
					const auto capture = source.Runtime->Capture(image.Portal.Text());
					if (capture && (capture->Tree != 0 || capture->TransparentImages[0] != 0) &&
						capture->Producer == image.Producer &&
						(!image.Route || (capture->Tree == image.Route->Tree &&
										  capture->Binding.Expected == image.Route->Eye &&
										  LastTime.value_or(Time{}) < image.Route->Deadline)))
						return false;
				}
				if (image.Route) ReleaseShadowRoute(*image.Route, LastTime.value_or(Time{}));
				if (image.Preparation) Render.CancelPortalCaptureTreePreparation(image.Preparation);
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
		struct ContentBinding {
			world::WorldId World;
			core::Name Owner;
			std::vector<WorldContentOwner> Foreign;
		};
		std::vector<ContentBinding> ContentBindings;
		struct RetainedBodyPolicy {
			world::WorldId World;
			uint64_t StoreIdentity = 0;
			PortalImageProducer::RetainedBodyAuthorization Authorize;
		};
		std::vector<RetainedBodyPolicy> RetainedBodyPolicies;
		uint64_t LocalStoreIdentity(world::WorldId world) {
			if (!Universe.NameOf(world).IsValid() || Universe.IsRemote(world)) return 0;
			uint64_t identity = 0;
			if (Universe.Enter(world, [&](ecs::Store &store) { identity = store.Identity(); }) !=
				world::WorldStatus::Ok)
				return 0;
			return identity;
		}

		void PruneRetiredProducers() {
			std::erase_if(Producers, [&](const Producer &producer) {
				// The presentation endpoint belongs to the world incarnation. Store
				// identity also changes after a valid replicated snapshot, so it cannot
				// distinguish a restored store from a destroyed and recycled world slot.
				if (Universe.LookupPresentation(producer.World, producer.Address.Channel) == producer.Address)
					return false;
				for (auto &source : Sources)
					source.Runtime->InvalidateEndpoint(producer.Address);
				(void)Universe.ClosePresentation(producer.Address);
				return true;
			});
			std::erase_if(RetainedBodyPolicies, [&](const RetainedBodyPolicy &policy) {
				return LocalStoreIdentity(policy.World) != policy.StoreIdentity;
			});
		}

		std::vector<world::PresentationOutbound> DriverOutbound;
		std::vector<world::WorldId> DriverRouteWorlds;
		world::PresentationBindings DriverBindings;
		bool DriverBindingsEnabled = false;
		std::optional<Time> LastTime;
		enum class BodyStage { Pull, AdmitManifest, Commit, Submitted, Complete };
		struct BodyComposition {
			uint64_t Token = 0, PreparationToken = 0, Tree = 0, Correlation = 0;
			size_t Slot = 0;
			world::WorldId World;
			world::PresentationAddress Address, Producer;
			core::Name Portal;
			PortalExchangeKey Eye;
			Time Deadline{}, Retry{};
			BodyStage Stage = BodyStage::Submitted;
			PortalTreeCompositionStatus Status = PortalTreeCompositionStatus::Pending;
			std::optional<PortalTreeShadowRequest> Request;
			std::optional<PortalShadowSnapshot> Manifest;
			std::optional<ShadowRoute> Route;
			PortalShadowPull Pull;
			std::vector<std::byte> Wire;
			uint64_t Image = 0;
			bool Preparation = false;
		};
		std::optional<BodyComposition> BodyJob;
		struct PreparationTicket {
			uint64_t Token = 0, Tree = 0;
			size_t Slot = 0;
			core::Name Portal;
			PortalExchangeKey Eye;
			bool Visible = true;
		};
		std::vector<PreparationTicket> PreparationTickets;
		uint64_t NextPreparationTicket = 1;
		// A completed immutable source set owns its capture pin until explicit
		// retirement or replacement. At most the renderer's two prepared trees fit.
		std::vector<BodyComposition> PreparedBodies;
		struct ShadowCancellation {
			world::WorldId World;
			world::PresentationAddress From, To;
			uint64_t Correlation = 0;
			Time Deadline{}, Retry{};
			PortalShadowPull Pull;
			std::vector<std::byte> Wire;
		};
		std::vector<ShadowCancellation> PendingShadowCancellations;
		uint64_t NextShadowCorrelation = 0;
		uint64_t ShadowCorrelation() {
			if (++NextShadowCorrelation == 0) ++NextShadowCorrelation;
			return NextShadowCorrelation;
		}
		bool CurrentEndpoint(const world::PresentationAddress &address) {
			const auto world = Universe.Find(core::Name(address.World));
			return world.IsValid() && Universe.LookupPresentation(world, address.Channel) == address;
		}
		Source *BodySource() {
			if (!BodyJob) return nullptr;
			for (auto &source : Sources) {
				if (source.Slot != BodyJob->Slot || source.Address != BodyJob->Address) continue;
				const auto capture = source.Runtime->Capture(BodyJob->Portal.Text());
				if (capture && capture->Tree == BodyJob->Tree && capture->Producer == BodyJob->Producer &&
					capture->Binding.Expected == BodyJob->Eye)
					return &source;
			}
			return nullptr;
		}
		void SendShadowCancellation(Time now) {
			std::erase_if(PendingShadowCancellations, [&](ShadowCancellation &cancel) {
				if (now >= cancel.Deadline || !CurrentEndpoint(cancel.From) || !CurrentEndpoint(cancel.To))
					return true;
				if (now < cancel.Retry) return false;
				const auto sent = Universe.SendPresentation(
					cancel.World, cancel.From, cancel.To, cancel.Correlation, cancel.Wire
				);
				if (sent != world::PresentationStatus::Ok && sent != world::PresentationStatus::Full)
					return true;
				cancel.Retry = now + std::chrono::milliseconds(100);
				return false;
			});
		}
		void ReleaseShadowRoute(const ShadowRoute &route, Time now) {
			for (auto &source : Sources)
				if (source.Address == route.Address) source.Runtime->UnpinCapture(route.Tree);
			SendShadowCancellation(now);
			if (PendingShadowCancellations.size() >= MAX_IMPORTED_PORTAL_IMAGES) return;
			PortalShadowPull pull{
				route.Eye, route.PublishedProducer, route.Eye, {}, PORTAL_SHADOW_CANCEL_PART
			};
			ShadowCancellation cancel{
				route.World,
				route.Address,
				route.Producer,
				ShadowCorrelation(),
				now + std::chrono::seconds(1),
				now,
				{},
				{}
			};
			cancel.Pull = pull;
			std::string error;
			if (EncodePortalShadowPull(pull, cancel.Wire, error))
				PendingShadowCancellations.push_back(std::move(cancel));
			SendShadowCancellation(now);
		}
		void AbortBody(Time now) {
			if (!BodyJob) return;
			if (BodyJob->Preparation)
				Render.CancelPortalCaptureTreePreparation(BodyJob->Token);
			else
				Render.CancelPortalCaptureTreeComposition(BodyJob->Token);
			if (BodyJob->PreparationToken)
				Render.CancelPortalCaptureTreePreparation(BodyJob->PreparationToken);
			if (BodyJob->Route) ReleaseShadowRoute(*BodyJob->Route, now);
			if (BodyJob->Stage == BodyStage::Complete) {
				for (auto &source : Sources)
					for (auto &composition : source.Compositions)
						if (composition.Image == BodyJob->Image && composition.Route) {
							ReleaseShadowRoute(*composition.Route, now);
							composition.Route.reset();
						}
			}
			BodyJob.reset();
		}
		bool PrepareShadowPull(uint8_t part, Time now) {
			auto &job = *BodyJob;
			job.Pull = {job.Eye, job.Request->Producer, job.Request->Eye, job.Request->BodyBounds, part};
			job.Correlation = ShadowCorrelation();
			job.Retry = now;
			job.Stage = BodyStage::Pull;
			std::string error;
			return EncodePortalShadowPull(job.Pull, job.Wire, error);
		}
		void AcceptShadowReply(const world::PresentationMessage &message, Time now) {
			for (auto cancel = PendingShadowCancellations.begin(); cancel != PendingShadowCancellations.end();
				 ++cancel) {
				if (message.From != cancel->To || message.To != cancel->From ||
					message.Correlation != cancel->Correlation)
					continue;
				PortalShadowPacket packet;
				std::string error;
				if (!DecodePortalShadowPacket(message.Payload, packet, error) || packet.Pull != cancel->Pull)
					return;
				if (packet.Status == PortalImageStatus::BudgetExceeded)
					cancel->Retry = now + std::chrono::milliseconds(100);
				else
					PendingShadowCancellations.erase(cancel);
				return;
			}
			if (!BodyJob || BodyJob->Stage != BodyStage::Pull) return;
			if (!BodySource() || !CurrentEndpoint(BodyJob->Address) || !CurrentEndpoint(BodyJob->Producer) ||
				now >= BodyJob->Deadline) {
				AbortBody(now);
				return;
			}
			auto &job = *BodyJob;
			if (message.From != job.Producer || message.To != job.Address ||
				message.Correlation != job.Correlation)
				return;
			PortalShadowPacket packet;
			std::string error;
			if (!DecodePortalShadowPacket(message.Payload, packet, error) || packet.Pull != job.Pull) return;
			if (packet.Status == PortalImageStatus::BudgetExceeded) {
				job.Status = PortalTreeCompositionStatus::BudgetExceeded;
				job.Retry = now + std::chrono::milliseconds(100);
				return;
			}
			if (packet.Status != PortalImageStatus::Ok) {
				AbortBody(now);
				return;
			}
			job.Status = PortalTreeCompositionStatus::Pending;
			if (packet.Pull.Part == PORTAL_SHADOW_MANIFEST_PART) {
				PortalShadowSnapshot manifest;
				if (!DecodePortalShadowManifest(packet.Payload, manifest, error) ||
					!MatchesPortalTreeShadowRequest(*job.Request, manifest)) {
					AbortBody(now);
					return;
				}
				job.Manifest = std::move(manifest);
				job.Stage = BodyStage::AdmitManifest;
				job.Wire.clear();
				return;
			}
			job.Status =
				job.Preparation
					? Render.AcceptPortalPreparedShadowTile(job.Token, job.Request->Node, packet.Payload)
					: Render.AcceptPortalCaptureTreeShadowTile(job.Token, job.Request->Node, packet.Payload);
			if (job.Status == PortalTreeCompositionStatus::Invalid) {
				AbortBody(now);
				return;
			}
			if (packet.Pull.Part == PORTAL_SHADOW_TILE_COUNT) {
				job.Stage = job.Status == PortalTreeCompositionStatus::BudgetExceeded ? BodyStage::Commit
																					  : BodyStage::Submitted;
				job.Wire.clear();
			} else if (!PrepareShadowPull(packet.Pull.Part + 1, now))
				AbortBody(now);
		}
		void AdvanceBody(Time now) {
			SendShadowCancellation(now);
			std::erase_if(PreparedBodies, [&](BodyComposition &prepared) {
				if (now < prepared.Deadline && CurrentEndpoint(prepared.Address) &&
					CurrentEndpoint(prepared.Producer))
					return false;
				Render.CancelPortalCaptureTreePreparation(prepared.PreparationToken);
				if (prepared.Route) ReleaseShadowRoute(*prepared.Route, now);
				return true;
			});
			if (!BodyJob) return;
			if (!BodySource() || !CurrentEndpoint(BodyJob->Address) || !CurrentEndpoint(BodyJob->Producer) ||
				(BodyJob->Stage != BodyStage::Complete && now >= BodyJob->Deadline)) {
				AbortBody(now);
				return;
			}
			auto &job = *BodyJob;
			if (job.Stage == BodyStage::Complete) return;
			if (job.Stage == BodyStage::AdmitManifest) {
				job.Status =
					job.Preparation
						? Render.BeginPortalCaptureTreePreparationShadowAssembly(job.Token, *job.Manifest)
						: Render.BeginPortalCaptureTreeShadowAssembly(job.Token, *job.Manifest);
				if (job.Status == PortalTreeCompositionStatus::Invalid) {
					AbortBody(now);
					return;
				}
				if (job.Status == PortalTreeCompositionStatus::BudgetExceeded) return;
				job.Manifest.reset();
				if (!PrepareShadowPull(1, now)) {
					AbortBody(now);
					return;
				}
			}
			if (job.Stage == BodyStage::Commit) {
				job.Status = job.Preparation
								 ? Render.CommitPortalPreparedShadowAssembly(job.Token, job.Request->Node)
								 : Render.CommitPortalCaptureTreeShadowAssembly(job.Token, job.Request->Node);
				if (job.Status == PortalTreeCompositionStatus::Invalid) {
					AbortBody(now);
					return;
				}
				if (job.Status == PortalTreeCompositionStatus::BudgetExceeded) return;
				job.Stage = BodyStage::Submitted;
			}
			if (job.Stage == BodyStage::Submitted) {
				auto progress = job.Preparation ? Render.PollPortalCaptureTreePreparation(job.Token)
												: Render.PollPortalCaptureTreeComposition(job.Token);
				job.Status = progress.Status;
				if (progress.Status == PortalTreeCompositionStatus::Invalid) {
					AbortBody(now);
					return;
				}
				if (progress.Status == PortalTreeCompositionStatus::Complete) {
					if (job.Preparation) {
						PreparedBodies.push_back(std::move(job));
						BodyJob.reset();
						return;
					}
					auto *source = BodySource();
					const uint64_t preparation = job.PreparationToken;
					if (preparation) {
						BodyComposition retained = job;
						retained.Token = preparation;
						retained.Preparation = true;
						retained.Stage = BodyStage::Complete;
						retained.Image = 0;
						PreparedBodies.push_back(std::move(retained));
						job.Route.reset();
					}
					const auto previous = std::find_if(
						source->Compositions.begin(),
						source->Compositions.end(),
						[&](const Composition &entry) { return entry.Portal == job.Portal; }
					);
					if (previous == source->Compositions.end())
						source->Compositions.push_back(
							{job.Portal,
							 progress.Image,
							 job.Producer,
							 {},
							 job.PreparationToken,
							 job.Deadline,
							 job.Address}
						);
					else {
						if (previous->Image != progress.Image) Render.DropPortalImage(previous->Image);
						if (previous->Route) ReleaseShadowRoute(*previous->Route, now);
						*previous = {
							job.Portal,
							progress.Image,
							job.Producer,
							{},
							job.PreparationToken,
							job.Deadline,
							job.Address
						};
					}
					job.Image = progress.Image;
					job.Stage = BodyStage::Complete;
					job.Route.reset();
					return;
				}
				if (!progress.Request) return;
				job.Request = std::move(progress.Request);
				if (!PrepareShadowPull(PORTAL_SHADOW_MANIFEST_PART, now)) {
					AbortBody(now);
					return;
				}
			}
			if (job.Stage != BodyStage::Pull || now < job.Retry) return;
			const auto sent =
				Universe.SendPresentation(job.World, job.Address, job.Producer, job.Correlation, job.Wire);
			if (sent != world::PresentationStatus::Ok && sent != world::PresentationStatus::Full) {
				AbortBody(now);
				return;
			}
			job.Retry = now + std::chrono::milliseconds(100);
		}

		bool Clock(Time now) {
			if ((LastTime && now < *LastTime) || now > Time::max() - std::chrono::seconds(1)) {
				return false;
			}
			LastTime = now;
			return true;
		}
		world::PresentationAddress Destination(const PortalImageDestination &destination) {
			PruneRetiredProducers();
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
			if (Shaders == nullptr) {
				OwnedShaders = std::make_unique<ShaderLibrary>();
				Shaders = OwnedShaders.get();
			}
			auto runtime = std::make_unique<PortalImageProducer>(
				Universe, Render, destination.World, opened.Address, &Resident, Shaders, PostProcessing
			);
			if (DriverBindingsEnabled) runtime->SetEndpointBindings(&DriverBindings);
			const auto storeIdentity = LocalStoreIdentity(destination.World);
			for (const auto &policy : RetainedBodyPolicies)
				if (policy.World == destination.World && policy.StoreIdentity == storeIdentity)
					runtime->SetRetainedBodyAuthorization(policy.Authorize);

			for (const auto &binding : ContentBindings) {
				if (binding.World == destination.World)
					runtime->SetContentOwner(binding.Owner, binding.Foreign);
			}
			Producers.push_back({destination.World, opened.Address, std::move(runtime)});
			return opened.Address;
		}
	};
	PortalImageHost::PortalImageHost(
		world::Universe &universe, Renderer &renderer, ShaderLibrary *shaders, bool postProcessing
	)
		: State(std::make_unique<Impl>(universe, renderer, shaders, postProcessing)) {}
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
		if (!state.DriverBindingsEnabled) {
			state.DriverBindingsEnabled = true;
			for (auto &source : state.Sources) {
				state.RetireCompositions(source, true);
				source.Runtime->SetEndpointBindings(&state.DriverBindings);
			}
			for (auto &producer : state.Producers)
				producer.Runtime->SetEndpointBindings(&state.DriverBindings);
		}
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
			if (frame.Signal == world::HostSignal::PresentationBindings) {
				const auto &incoming = frame.Bindings;
				if (incoming.Session < state.DriverBindings.Session ||
					(incoming.Session == state.DriverBindings.Session &&
					 incoming.Revision <= state.DriverBindings.Revision))
					continue;
				const auto invalidate = [&](const world::PresentationEndpointBinding &binding) {
					for (auto &source : state.Sources) {
						source.Runtime->InvalidateEndpoint(binding.Local);
						source.Runtime->InvalidateEndpoint(binding.Published);
						state.RetireCompositions(source);
					}
					for (auto &producer : state.Producers)
						if (producer.Address == binding.Local) producer.Runtime->Clear();
				};
				const auto changes = [&](const auto &previous, const auto &next) {
					for (const auto &binding : previous)
						if (std::find(next.begin(), next.end(), binding) == next.end()) invalidate(binding);
					for (const auto &binding : next)
						if (std::find(previous.begin(), previous.end(), binding) == previous.end())
							invalidate(binding);
				};
				changes(state.DriverBindings.Exports, incoming.Exports);
				changes(state.DriverBindings.Returns, incoming.Returns);
				state.DriverBindings = incoming;
			} else if (frame.Signal == world::HostSignal::PresentationRoutes) {
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
		if (!destination.Authored.IsValid()) {
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

	void PortalImageHost::SetContentOwner(
		world::WorldId world, core::Name owner, std::span<const WorldContentOwner> foreign
	) {
		if (!world.IsValid()) return;
		auto found = std::find_if(
			State->ContentBindings.begin(),
			State->ContentBindings.end(),
			[world](const Impl::ContentBinding &binding) { return binding.World == world; }
		);
		if (found == State->ContentBindings.end()) {
			State->ContentBindings.push_back({world, owner, {foreign.begin(), foreign.end()}});
		} else {
			const bool same = found->Owner == owner &&
							  std::equal(
								  found->Foreign.begin(),
								  found->Foreign.end(),
								  foreign.begin(),
								  foreign.end(),
								  [](const WorldContentOwner &left, const WorldContentOwner &right) {
									  return left.World == right.World && left.Owner == right.Owner;
								  }
							  );
			if (same) return;
			found->Owner = owner;
			found->Foreign.assign(foreign.begin(), foreign.end());
		}
		for (auto &producer : State->Producers) {
			if (producer.World == world) producer.Runtime->SetContentOwner(owner, foreign);
		}
	}

	bool PortalImageHost::SetRetainedBodyAuthorization(
		world::WorldId world, PortalImageProducer::RetainedBodyAuthorization authorize
	) {
		State->PruneRetiredProducers();
		const auto identity = State->LocalStoreIdentity(world);
		if (!identity) return false;
		auto found = std::find_if(
			State->RetainedBodyPolicies.begin(),
			State->RetainedBodyPolicies.end(),
			[world](const auto &policy) { return policy.World == world; }
		);
		if (authorize) {
			if (found == State->RetainedBodyPolicies.end()) {
				if (State->RetainedBodyPolicies.size() >= MAX_IMPORTED_PORTAL_IMAGES) return false;
				State->RetainedBodyPolicies.push_back({world, identity, authorize});
			} else
				*found = {world, identity, authorize};
		} else if (found != State->RetainedBodyPolicies.end()) {
			State->RetainedBodyPolicies.erase(found);
		}
		for (auto &producer : State->Producers)
			if (producer.World == world) producer.Runtime->SetRetainedBodyAuthorization(authorize);
		if (!authorize) {
			State->AbortBody(State->LastTime.value_or(Time{}));
			for (auto prepared = State->PreparedBodies.begin(); prepared != State->PreparedBodies.end();) {
				State->Render.CancelPortalCaptureTreePreparation(prepared->PreparationToken);
				if (prepared->Route)
					State->ReleaseShadowRoute(*prepared->Route, State->LastTime.value_or(Time{}));
				prepared = State->PreparedBodies.erase(prepared);
			}
			// A retained body is authorization-bound even after its capture reached the
			// source. Drop each routed preview before a later frame can reuse its tree.
			for (auto &source : State->Sources) {
				std::vector<core::Name> revokedPortals;
				std::erase_if(source.Portals, [&](const Impl::DemandedPortal &portal) {
					if (portal.Destination != world) return false;
					revokedPortals.push_back(portal.Name);
					source.Runtime->InvalidatePortal(portal.Name.Text());
					return true;
				});
				State->RetireCompositions(source, false, &revokedPortals);
			}
		}
		return true;
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
			if (state.DriverBindingsEnabled) runtime->SetEndpointBindings(&state.DriverBindings);
			state.Sources.push_back({source, viewSlot, opened.Address, std::move(runtime), {}, {}});
			found = std::prev(state.Sources.end());
		}
		const auto previousPortals = found->Portals;
		for (const auto portal : previousPortals) {
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
				// A topology or replica lookup can miss for one frame. Keep a live route so its
				// owned image stays visible while the same capture remains in flight.
				const auto retained = std::find_if(
					previousPortals.begin(), previousPortals.end(), [&](const Impl::DemandedPortal &portal) {
						return portal.Name == demand.Binding.Portal &&
							   state.Universe.NameOf(portal.Destination).IsValid();
					}
				);
				if (retained != previousPortals.end()) found->Portals.push_back(*retained);
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
		state.PruneRetiredProducers();
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
			while (auto reply = source.Runtime->TakeShadowReply())
				state.AcceptShadowReply(*reply, now);
		}
		state.AdvanceBody(now);
		state.Topology.Pump(now);
		return total;
	}
	bool PortalImageHost::HasPendingUploads() const {
		for (const auto &source : State->Sources)
			if (source.Runtime->HasPendingUploads()) return true;
		for (const auto &producer : State->Producers)
			if (producer.Runtime->HasPendingShadowFits()) return true;
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
		const auto previous = std::find_if(
			source->Compositions.begin(), source->Compositions.end(), [&](const Impl::Composition &entry) {
				return entry.Portal == portal;
			}
		);
		const auto retained = [&]() {
			return previous != source->Compositions.end() && previous->Producer == capture->Producer
					   ? previous->Image
					   : uint64_t{};
		};
		if (state.BodyJob) return retained();
		const auto composed = state.Render.ComposePortalBodyImage(*capture, body);
		if (composed == 0) return retained();
		if (previous == source->Compositions.end())
			source->Compositions.push_back(
				{.Portal = portal,
				 .Image = composed,
				 .Producer = capture->Producer,
				 .Route = std::nullopt,
				 .Preparation = 0,
				 .Deadline = {},
				 .Address = {}}
			);
		else {
			if (previous->Route) {
				state.ReleaseShadowRoute(*previous->Route, state.LastTime.value_or(Time{}));
				previous->Route.reset();
			}
			if (previous->Image != composed) state.Render.DropPortalImage(previous->Image);
			previous->Image = composed;
			previous->Producer = capture->Producer;
		}
		return composed;
	}
	PortalTreeCompositionStatus
	PortalImageHost::BeginBodyComposition(core::Name portal, const View &body, Time now, uint64_t &token) {
		token = 0;
		auto &state = *State;
		if (now > Time::max() - std::chrono::seconds(10) || !state.Clock(now))
			return PortalTreeCompositionStatus::Invalid;
		state.AdvanceBody(now);
		if (state.BodyJob || !state.PendingShadowCancellations.empty())
			return PortalTreeCompositionStatus::BudgetExceeded;
		const auto source =
			std::find_if(state.Sources.begin(), state.Sources.end(), [&](const Impl::Source &entry) {
				return entry.Slot == body.Slot;
			});
		if (source == state.Sources.end()) return PortalTreeCompositionStatus::Invalid;
		const auto capture = source->Runtime->Capture(portal.Text());
		if (!capture || !capture->Tree || !state.CurrentEndpoint(source->Address) ||
			!state.CurrentEndpoint(capture->Producer))
			return PortalTreeCompositionStatus::Invalid;
		const auto *tree = state.Render.FindPortalCaptureTree(capture->Tree);
		if (!tree || tree->Nodes.empty()) return PortalTreeCompositionStatus::Invalid;
		const auto publishedProducer = tree->Nodes.front().Producer;
		const auto status = state.Render.BeginPortalCaptureTreeComposition(capture->Tree, body, token);
		if (status != PortalTreeCompositionStatus::Pending) return status;
		const auto deadline = source->Runtime->PinCapture(
			portal.Text(), capture->Tree, capture->Binding.Expected, now, now + std::chrono::seconds(10)
		);
		if (!deadline) {
			state.Render.CancelPortalCaptureTreeComposition(token);
			token = 0;
			return PortalTreeCompositionStatus::Invalid;
		}
		for (auto &composition : source->Compositions)
			if (composition.Route && composition.Route->Tree == capture->Tree) composition.Route.reset();
		Impl::BodyComposition job;
		job.Token = token;
		job.Tree = capture->Tree;
		job.Slot = source->Slot;
		job.World = source->World;
		job.Address = source->Address;
		job.Producer = capture->Producer;
		job.Portal = portal;
		job.Eye = capture->Binding.Expected;
		job.Deadline = *deadline;
		job.Route = Impl::ShadowRoute{
			capture->Tree,
			source->World,
			source->Address,
			capture->Producer,
			capture->Binding.Expected,
			*deadline,
			publishedProducer
		};
		state.BodyJob = std::move(job);
		state.AdvanceBody(now);
		return state.BodyJob ? state.BodyJob->Status : PortalTreeCompositionStatus::Invalid;
	}
	PortalTreeCompositionProgress PortalImageHost::PollBodyComposition(uint64_t token, Time now) {
		auto &state = *State;
		if (!token || !state.Clock(now)) return {};
		state.AdvanceBody(now);
		if (!state.BodyJob || state.BodyJob->Token != token) return {};
		PortalTreeCompositionProgress progress{.Status = state.BodyJob->Status};
		if (state.BodyJob->Stage == Impl::BodyStage::Complete) {
			progress.Status = PortalTreeCompositionStatus::Complete;
			progress.Image = state.BodyJob->Image;
			state.BodyJob.reset();
		}
		return progress;
	}
	void PortalImageHost::CancelBodyComposition(uint64_t token) {
		if (State->BodyJob && State->BodyJob->Token == token)
			State->AbortBody(State->LastTime.value_or(Time{}));
	}
	PortalTreeCompositionStatus PortalImageHost::BeginBodyPreparation(
		core::Name portal, const View &referenceBody, Time now, uint64_t &token
	) {
		token = 0;
		auto &state = *State;
		if (now > Time::max() - std::chrono::seconds(10) || !state.Clock(now))
			return PortalTreeCompositionStatus::Invalid;
		state.AdvanceBody(now);
		if (state.BodyJob || state.PreparedBodies.size() >= 2 || !state.PendingShadowCancellations.empty())
			return PortalTreeCompositionStatus::BudgetExceeded;
		const auto source = std::find_if(state.Sources.begin(), state.Sources.end(), [&](const auto &entry) {
			return entry.Slot == referenceBody.Slot;
		});
		if (source == state.Sources.end()) return PortalTreeCompositionStatus::Invalid;
		const auto capture = source->Runtime->Capture(portal.Text());
		if (!capture || !capture->Tree || !state.CurrentEndpoint(source->Address) ||
			!state.CurrentEndpoint(capture->Producer))
			return PortalTreeCompositionStatus::Invalid;
		const auto *tree = state.Render.FindPortalCaptureTree(capture->Tree);
		if (!tree || tree->Nodes.empty()) return PortalTreeCompositionStatus::Invalid;
		const auto status =
			state.Render.BeginPortalCaptureTreePreparation(capture->Tree, referenceBody, token);
		if (status != PortalTreeCompositionStatus::Pending) return status;
		const auto deadline = source->Runtime->PinCapture(
			portal.Text(), capture->Tree, capture->Binding.Expected, now, now + std::chrono::seconds(10)
		);
		if (!deadline) {
			state.Render.CancelPortalCaptureTreePreparation(token);
			token = 0;
			return PortalTreeCompositionStatus::Invalid;
		}
		Impl::BodyComposition job;
		job.Token = token;
		job.PreparationToken = token;
		job.Tree = capture->Tree;
		job.Slot = source->Slot;
		job.World = source->World;
		job.Address = source->Address;
		job.Producer = capture->Producer;
		job.Portal = portal;
		job.Eye = capture->Binding.Expected;
		job.Deadline = *deadline;
		job.Preparation = true;
		job.Route = Impl::ShadowRoute{
			capture->Tree,
			source->World,
			source->Address,
			capture->Producer,
			capture->Binding.Expected,
			*deadline,
			tree->Nodes.front().Producer
		};
		state.BodyJob = std::move(job);
		state.AdvanceBody(now);
		return state.BodyJob ? state.BodyJob->Status : PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionProgress PortalImageHost::PollBodyPreparation(uint64_t token, Time now) {
		auto &state = *State;
		if (!token || !state.Clock(now)) return {};
		state.AdvanceBody(now);
		const auto found = std::find_if(
			state.PreparedBodies.begin(), state.PreparedBodies.end(), [token](const auto &entry) {
				return entry.Token == token;
			}
		);
		if (found != state.PreparedBodies.end()) return {.Status = PortalTreeCompositionStatus::Complete};
		if (state.BodyJob && (state.BodyJob->Token == token || state.BodyJob->PreparationToken == token))
			return {.Status = state.BodyJob->Status};
		return {};
	}
	void PortalImageHost::CancelBodyPreparation(uint64_t token) {
		auto &state = *State;
		if (state.BodyJob && (state.BodyJob->Token == token || state.BodyJob->PreparationToken == token)) {
			state.AbortBody(state.LastTime.value_or(Time{}));
			return;
		}
		const auto found = std::find_if(
			state.PreparedBodies.begin(), state.PreparedBodies.end(), [token](const auto &entry) {
				return entry.Token == token;
			}
		);
		if (found == state.PreparedBodies.end()) return;
		state.Render.CancelPortalCaptureTreePreparation(token);
		if (found->Route) state.ReleaseShadowRoute(*found->Route, state.LastTime.value_or(Time{}));
		state.PreparedBodies.erase(found);
	}
	PortalTreeCompositionStatus PortalImageHost::BeginPreparedBodyComposition(
		uint64_t preparation, const View &body, Time now, uint64_t &jobToken
	) {
		jobToken = 0;
		auto &state = *State;
		if (!state.Clock(now) || state.BodyJob) return PortalTreeCompositionStatus::BudgetExceeded;
		const auto found = std::find_if(
			state.PreparedBodies.begin(), state.PreparedBodies.end(), [preparation](const auto &entry) {
				return entry.Token == preparation;
			}
		);
		if (found == state.PreparedBodies.end() || now >= found->Deadline ||
			!state.CurrentEndpoint(found->Address) || !state.CurrentEndpoint(found->Producer))
			return PortalTreeCompositionStatus::Invalid;
		const auto status =
			state.Render.BeginPreparedPortalCaptureTreeComposition(preparation, body, jobToken);
		if (status != PortalTreeCompositionStatus::Pending) return status;
		found->Preparation = false;
		found->Token = jobToken;
		found->Stage = Impl::BodyStage::Submitted;
		found->Status = PortalTreeCompositionStatus::Pending;
		found->Image = 0;
		state.BodyJob = std::move(*found);
		state.PreparedBodies.erase(found);
		return PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionStatus
	PortalImageHost::QueueBodyPreparation(core::Name portal, size_t slot, Time now, uint64_t &ticket) {
		ticket = 0;
		auto &state = *State;
		if (!state.Clock(now) || state.PreparationTickets.size() >= MAX_IMPORTED_PORTAL_IMAGES)
			return PortalTreeCompositionStatus::BudgetExceeded;
		auto source = std::find_if(state.Sources.begin(), state.Sources.end(), [slot](const auto &entry) {
			return entry.Slot == slot;
		});
		if (source == state.Sources.end()) return PortalTreeCompositionStatus::Invalid;
		const auto capture = source->Runtime->Capture(portal.Text());
		if (!capture || !capture->Tree || !state.NextPreparationTicket)
			return PortalTreeCompositionStatus::Invalid;
		ticket = state.NextPreparationTicket++;
		state.PreparationTickets.push_back({ticket, capture->Tree, slot, portal, capture->Binding.Expected});
		return PortalTreeCompositionStatus::Pending;
	}
	PortalTreeCompositionProgress PortalImageHost::PollBodyPreparationTicket(uint64_t ticket, Time now) {
		auto &state = *State;
		if (!ticket || !state.Clock(now)) return {};
		const auto found = std::find_if(
			state.PreparationTickets.begin(), state.PreparationTickets.end(), [ticket](const auto &entry) {
				return entry.Token == ticket;
			}
		);
		if (found == state.PreparationTickets.end()) return {};
		return {
			.Status = (&*found == &state.PreparationTickets.front() && !state.BodyJob &&
					   state.PreparedBodies.size() < 2)
						  ? PortalTreeCompositionStatus::Complete
						  : PortalTreeCompositionStatus::Pending
		};
	}
	PortalTreeCompositionStatus PortalImageHost::BeginQueuedBodyPreparation(
		uint64_t ticket, const View &body, Time now, uint64_t &preparation
	) {
		auto &state = *State;
		preparation = 0;
		if (!state.Clock(now) || state.PreparationTickets.empty() ||
			state.PreparationTickets.front().Token != ticket)
			return PortalTreeCompositionStatus::Invalid;
		const auto entry = state.PreparationTickets.front();
		if (body.Slot != entry.Slot) return PortalTreeCompositionStatus::Invalid;
		const auto source =
			std::find_if(state.Sources.begin(), state.Sources.end(), [&](const auto &candidate) {
				return candidate.Slot == entry.Slot;
			});
		const auto capture = source == state.Sources.end() ? std::optional<PortalImageCapture>{}
														   : source->Runtime->Capture(entry.Portal.Text());
		if (!capture || capture->Tree != entry.Tree || capture->Binding.Expected != entry.Eye)
			return PortalTreeCompositionStatus::Invalid;
		state.PreparationTickets.erase(state.PreparationTickets.begin());
		return BeginBodyPreparation(entry.Portal, body, now, preparation);
	}
	void PortalImageHost::CancelBodyPreparationTicket(uint64_t ticket) {
		std::erase_if(State->PreparationTickets, [ticket](const auto &entry) {
			return entry.Token == ticket;
		});
	}
	void PortalImageHost::ReleaseBodyPreparation(uint64_t preparation) {
		auto &state = *State;
		const auto found = std::find_if(
			state.PreparedBodies.begin(), state.PreparedBodies.end(), [preparation](const auto &entry) {
				return entry.PreparationToken == preparation;
			}
		);
		if (found == state.PreparedBodies.end()) return;
		state.Render.CancelPortalCaptureTreePreparation(preparation);
		if (found->Route) state.ReleaseShadowRoute(*found->Route, state.LastTime.value_or(Time{}));
		state.PreparedBodies.erase(found);
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
		State->AbortBody(State->LastTime.value_or(Time{}));
		for (auto &source : State->Sources) {
			for (auto &composition : source.Compositions)
				if (composition.Route) {
					State->ReleaseShadowRoute(*composition.Route, State->LastTime.value_or(Time{}));
					composition.Route.reset();
				}
			source.Runtime->RestartRequests();
		}
		State->Topology.RestartRequests();
	}

	void PortalImageHost::RemoveViewport(size_t viewSlot) {
		for (auto prepared = State->PreparedBodies.begin(); prepared != State->PreparedBodies.end();) {
			if (prepared->Slot != viewSlot) {
				++prepared;
				continue;
			}
			State->Render.CancelPortalCaptureTreePreparation(prepared->PreparationToken);
			if (prepared->Route)
				State->ReleaseShadowRoute(*prepared->Route, State->LastTime.value_or(Time{}));
			prepared = State->PreparedBodies.erase(prepared);
		}
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
		// A removed world may own a nested node or body assets, not just the root reply.
		State->AbortBody(State->LastTime.value_or(Time{}));
		for (auto prepared = State->PreparedBodies.begin(); prepared != State->PreparedBodies.end();) {
			if (prepared->World != world) {
				++prepared;
				continue;
			}
			State->Render.CancelPortalCaptureTreePreparation(prepared->PreparationToken);
			if (prepared->Route)
				State->ReleaseShadowRoute(*prepared->Route, State->LastTime.value_or(Time{}));
			prepared = State->PreparedBodies.erase(prepared);
		}
		if (State->OwnedShaders) {
			auto owner = State->Universe.NameOf(world);
			for (const auto &binding : State->ContentBindings)
				if (binding.World == world) owner = binding.Owner;
			State->OwnedShaders->DropOwner(owner);
		}
		std::erase_if(State->ContentBindings, [world](const Impl::ContentBinding &binding) {
			return binding.World == world;
		});
		std::erase_if(State->RetainedBodyPolicies, [world](const auto &policy) {
			return policy.World == world;
		});
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
		State->AbortBody(State->LastTime.value_or(Time{}));
		for (auto &prepared : State->PreparedBodies) {
			State->Render.CancelPortalCaptureTreePreparation(prepared.PreparationToken);
			if (prepared.Route) State->ReleaseShadowRoute(*prepared.Route, State->LastTime.value_or(Time{}));
		}
		State->PreparedBodies.clear();
		State->DriverOutbound.clear();
		while (!State->Sources.empty()) {
			RemoveViewport(State->Sources.back().Slot);
		}
		while (!State->Producers.empty()) {
			RemoveWorld(State->Producers.back().World);
		}
		State->ContentBindings.clear();
		State->RetainedBodyPolicies.clear();
		State->Resident.Clear();
		State->Topology.Clear();
		for (const auto remote : State->DriverRouteWorlds)
			(void)State->Universe.Destroy(remote);
		State->DriverRouteWorlds.clear();
		State->DriverBindings = {};
		State->PendingShadowCancellations.clear();
	}
}
