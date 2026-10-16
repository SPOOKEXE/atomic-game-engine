#include "ProceduralSceneState.hpp"
#include "ProceduralState.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/ProceduralService.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <new>
#include <utility>

namespace engine::script {
	namespace procedural_service_detail {
		struct Lease {
			uint64_t Runtime = 0;
			std::atomic_bool Attached{true};
		};
		struct Entry {
			uint64_t Ticket = 0;
			std::shared_ptr<Lease> Owner{};
			ProceduralSceneTarget Target{};
			bool Releasing = false;
		};
		struct WorldHost {
			uint64_t Identity = 0;
			ProceduralWorldMode Mode = ProceduralWorldMode::AuthoritativeSerial;
			ProceduralJobs Jobs;
			std::vector<Entry> Entries{};
			std::vector<std::weak_ptr<Lease>> Endpoints{};
			WorldHost(const ecs::Store &store, ProceduralWorldMode mode)
				: Identity(store.Identity()), Mode(mode), Jobs(store) {
				Entries.reserve(ProceduralHostLimits::MaximumRetainedRequests);
				Endpoints.reserve(64);
			}
			~WorldHost() {
				for (auto &endpoint : Endpoints)
					if (auto lease = endpoint.lock()) lease->Attached.store(false, std::memory_order_release);
			}
			void Pump() {
				for (auto &entry : Entries)
					if (!entry.Owner->Attached.load(std::memory_order_acquire)) {
						Jobs.Cancel(entry.Ticket);
						entry.Releasing = true;
					}
				Jobs.Poll();
				std::erase_if(Entries, [&](auto &entry) {
					return entry.Releasing && Jobs.Release(entry.Ticket);
				});
				std::erase_if(Endpoints, [](const auto &entry) {
					auto lease = entry.lock();
					return !lease || !lease->Attached.load(std::memory_order_acquire);
				});
			}
			Entry *Owned(uint64_t ticket, const std::shared_ptr<Lease> &lease) {
				const auto entry = std::find_if(Entries.begin(), Entries.end(), [&](const auto &row) {
					return row.Ticket == ticket && row.Owner == lease && !row.Releasing;
				});
				return entry == Entries.end() ? nullptr : &*entry;
			}
		};
		// This owner is private and transient. No endpoint or snapshot can keep the
		// ECS resource alive, and no host contains a Store pointer or mutable facts.
		struct QueueOwner {
			std::shared_ptr<WorldHost> Host{};
		};
	}
	struct ProceduralEndpoint::State {
		std::weak_ptr<procedural_service_detail::WorldHost> Host{};
		std::shared_ptr<procedural_service_detail::Lease> Lease{};
		std::shared_ptr<procedural_service_detail::WorldHost>
		Require(ecs::Store &store, procedural::Diagnostic &diagnostic) {
			auto host = Host.lock();
			if (!host || !Lease || !Lease->Attached.load(std::memory_order_acquire) ||
				!store.IsOnOwningThread() || store.AdoptOnly() || store.Identity() != host->Identity) {
				procedural_detail::Refuse(diagnostic, procedural::Status::StaleArtifact, "service.owner");
				return {};
			}
			host->Pump();
			diagnostic = {};
			return host;
		}
	};
	ProceduralEndpoint::ProceduralEndpoint(std::unique_ptr<State> state) : Held(std::move(state)) {}
	ProceduralEndpoint::~ProceduralEndpoint() {
		Detach();
	}
	bool ProceduralEndpoint::BelongsTo(const ecs::Store &store, uint64_t runtimeIdentity) const {
		const auto host = Held->Host.lock();
		return host && Held->Lease && Held->Lease->Attached.load(std::memory_order_acquire) &&
			   Held->Lease->Runtime == runtimeIdentity && host->Identity == store.Identity();
	}

	void ProceduralEndpoint::Detach() noexcept {
		if (Held && Held->Lease) Held->Lease->Attached.store(false, std::memory_order_release);
	}
	void RegisterProceduralServiceComponents() {
		ecs::Components::Register<procedural_service_detail::QueueOwner>(
			"script.ProceduralQueueOwner",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *owners = static_cast<procedural_service_detail::QueueOwner *>(destination);
				for (size_t i = 0; i < count; ++i)
					owners[i].Host.reset();
			},
			0
		);
	}
	std::shared_ptr<ProceduralEndpoint> OpenProceduralEndpoint(
		ecs::Store &store, uint64_t runtime, ProceduralWorldMode mode, procedural::Diagnostic &diagnostic
	) {
		using namespace procedural_service_detail;
		if (!store.IsOnOwningThread() || store.AdoptOnly() || runtime == 0 ||
			(mode != ProceduralWorldMode::AuthoritativeSerial &&
			 mode != ProceduralWorldMode::StudioPreview) ||
			!ecs::Components::Assigned<QueueOwner>().IsValid()) {
			procedural_detail::Refuse(diagnostic, procedural::Status::InvalidInput, "service.open");
			return {};
		}
		try {
			auto *owner = store.Resource<QueueOwner>();
			std::shared_ptr<WorldHost> host = owner ? owner->Host : nullptr;
			if (!host || host->Identity != store.Identity()) {
				host = std::make_shared<WorldHost>(store, mode);
				store.SetResource(QueueOwner{host});
			}
			host->Pump();
			if (host->Mode != mode || host->Endpoints.size() >= 64) {
				procedural_detail::Refuse(
					diagnostic, procedural::Status::LimitExceeded, "service.mode_or_endpoints"
				);
				return {};
			}
			for (const auto &endpoint : host->Endpoints)
				if (auto lease = endpoint.lock(); lease && lease->Runtime == runtime) {
					procedural_detail::Refuse(
						diagnostic, procedural::Status::InvalidInput, "service.runtime"
					);
					return {};
				}
			auto lease = std::make_shared<Lease>();
			lease->Runtime = runtime;
			auto state = std::make_unique<ProceduralEndpoint::State>();
			state->Host = host;
			state->Lease = lease;
			auto endpoint = std::shared_ptr<ProceduralEndpoint>(new ProceduralEndpoint(std::move(state)));
			host->Endpoints.push_back(lease);
			diagnostic = {};
			return endpoint;
		} catch (const std::bad_alloc &) {
			procedural_detail::Refuse(diagnostic, procedural::Status::AllocationFailed, "service.open");
			return {};
		}
	}
	bool ProceduralEndpoint::Configure(
		ecs::Store &store,
		const procedural::Recipe &recipe,
		std::span<const procedural::RegionScope> scopes,
		std::span<const ProceduralSource> sources,
		procedural::Diagnostic &diagnostic
	) {
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		return ConfigureProceduralWorld(store, recipe.Name, recipe, scopes, sources, diagnostic);
	}
	bool ProceduralEndpoint::SetSource(
		ecs::Store &store, const ProceduralSource &source, procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return SetProceduralSource(store, source, diagnostic);
	}
	bool ProceduralEndpoint::RemoveSource(
		ecs::Store &store,
		procedural::SourceKind kind,
		std::string_view name,
		procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return RemoveProceduralSource(store, kind, name, diagnostic);
	}
	bool ProceduralEndpoint::SetSpline(
		ecs::Store &store, const procedural::AuthoredSpline &spline, procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return SetProceduralSpline(store, spline, diagnostic);
	}
	bool ProceduralEndpoint::RemoveSpline(
		ecs::Store &store, std::string_view name, procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return RemoveProceduralSpline(store, name, diagnostic);
	}
	bool ProceduralEndpoint::SetOverride(
		ecs::Store &store, const procedural::PlacementOverride &value, procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return SetProceduralOverride(store, value, diagnostic);
	}
	bool ProceduralEndpoint::RemoveOverride(
		ecs::Store &store, std::string_view name, procedural::Diagnostic &diagnostic
	) {
		if (!Held->Require(store, diagnostic)) return false;
		return RemoveProceduralOverride(store, name, diagnostic);
	}
	bool ProceduralEndpoint::Generate(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		uint64_t &ticket,
		procedural::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("ProceduralService::Generate");
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		if (host->Entries.size() >= ProceduralHostLimits::MaximumRetainedRequests ||
			host->Jobs.ReservedBytes() >
				ProceduralHostLimits::MaximumRetainedBytes - ProceduralHostLimits::BytesPerRequest)
			return procedural_detail::Refuse(
				diagnostic, procedural::Status::LimitExceeded, "service.tickets"
			);
		ProceduralSceneTarget target;
		if (!CaptureProceduralSceneTarget(store, scope, mesh, part, target, diagnostic)) return false;
		const auto submitted = host->Jobs.Submit(
			store,
			scope,
			host->Mode == ProceduralWorldMode::AuthoritativeSerial ? parallel::JobContext::Serial
																   : parallel::JobContext::Threaded
		);
		if (!submitted) {
			diagnostic = host->Jobs.LastDiagnostic();
			return false;
		}
		host->Entries.push_back({submitted, Held->Lease, target, false});
		if (!procedural_scene_detail::ProvisionTarget(store, host->Entries.back().Target, diagnostic)) {
			host->Jobs.Cancel(submitted);
			host->Entries.back().Releasing = true;
			host->Pump();
			ticket = 0;
			return false;
		}
		host->Jobs.Poll();
		ticket = submitted;
		return true;
	}
	bool ProceduralEndpoint::Progress(
		ecs::Store &store, uint64_t ticket, ProceduralServiceProgress &out, procedural::Diagnostic &diagnostic
	) {
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		if (!host->Owned(ticket, Held->Lease))
			return procedural_detail::Refuse(diagnostic, procedural::Status::StaleArtifact, "service.ticket");
		ProceduralServiceProgress result;
		if (!host->Jobs.Progress(ticket, result.Work))
			return procedural_detail::Refuse(diagnostic, procedural::Status::StaleArtifact, "service.ticket");
		for (const auto &completion : host->Jobs.Completions())
			if (completion.Ticket == ticket) {
				result.Diagnostic = completion.Diagnostic;
				result.ArtifactDigest = completion.ArtifactDigest;
			}
		out = std::move(result);
		return true;
	}
	bool ProceduralEndpoint::Cancel(ecs::Store &store, uint64_t ticket, procedural::Diagnostic &diagnostic) {
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		if (!host->Owned(ticket, Held->Lease))
			return procedural_detail::Refuse(diagnostic, procedural::Status::StaleArtifact, "service.ticket");
		return host->Jobs.Cancel(ticket);
	}
	bool ProceduralEndpoint::Publish(
		ecs::Store &store, std::span<const uint64_t> tickets, procedural::Diagnostic &diagnostic
	) {
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		if (tickets.empty() || tickets.size() > ProceduralHostLimits::MaximumRetainedRequests)
			return procedural_detail::Refuse(
				diagnostic, procedural::Status::LimitExceeded, "service.tickets"
			);
		std::array<ProceduralSceneTarget, ProceduralHostLimits::MaximumRetainedRequests> targets{};
		for (size_t i = 0; i < tickets.size(); ++i) {
			const auto *entry = host->Owned(tickets[i], Held->Lease);
			if (!entry)
				return procedural_detail::Refuse(
					diagnostic, procedural::Status::StaleArtifact, "service.ticket"
				);
			targets[i] = entry->Target;
		}
		if (!host->Jobs.PublishSceneBatch(
				store, tickets, std::span(targets.data(), tickets.size()), store.Time().Tick
			)) {
			diagnostic = host->Jobs.LastDiagnostic();
			return false;
		}
		return true;
	}
	bool ProceduralEndpoint::Release(ecs::Store &store, uint64_t ticket, procedural::Diagnostic &diagnostic) {
		auto host = Held->Require(store, diagnostic);
		if (!host) return false;
		auto *entry = host->Owned(ticket, Held->Lease);
		if (!entry)
			return procedural_detail::Refuse(diagnostic, procedural::Status::StaleArtifact, "service.ticket");
		host->Jobs.Cancel(ticket);
		entry->Releasing = true;
		host->Pump();
		return true;
	}
}
