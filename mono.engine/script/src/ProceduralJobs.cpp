#include "ProceduralState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace engine::script {

	namespace procedural_job_detail {

		struct Job {
			uint64_t Ticket = 0;
			uint64_t StoreIdentity = 0;
			uint64_t AuthorityIncarnation = 0;
			std::string Owner{};
			procedural::CompiledRecipe Recipe{};
			procedural::RegionRequest Request{};
			std::vector<uint8_t> ExpectedKey{};
			procedural::GenerationControl Control{};
			ProceduralCompletion Completion{};
			std::atomic_bool Finished{false};
			std::thread Worker{};
			bool Observed = false;
		};

		uint64_t NextTicket() {
			static std::atomic<uint64_t> ticket{1};
			return procedural_detail::TakeSequence(ticket);
		}

		void Execute(Job &job) {
			ENGINE_PROFILE("Procedural::Generate");
			try {
				procedural::RegionResult result;
				const auto status = procedural::GenerateRegion(
					job.Recipe, job.Request, result, job.Completion.Diagnostic, &job.Control
				);
				if (status != procedural::Status::Ok) {
					job.Completion.Status = status == procedural::Status::Canceled
												? ProceduralJobStatus::Canceled
												: ProceduralJobStatus::Failed;
				} else if (job.Control.StopRequested.load(std::memory_order_acquire)) {
					job.Completion.Status = ProceduralJobStatus::Canceled;
				} else if (procedural::EncodeRegionResult(
							   result, job.Completion.ArtifactBytes, job.Completion.Diagnostic
						   ) != procedural::Status::Ok) {
					job.Completion.Status = ProceduralJobStatus::Failed;
				} else {
					job.Completion.ArtifactDigest = procedural_detail::Hash(job.Completion.ArtifactBytes);
					job.Completion.Status = ProceduralJobStatus::Ready;
				}
				if (job.Control.StopRequested.load(std::memory_order_acquire)) {
					job.Completion.ArtifactBytes.clear();
					job.Completion.Status = ProceduralJobStatus::Canceled;
					job.Completion.Diagnostic = {procedural::Status::Canceled, {}};
				}
			} catch (...) {
				// No worker exception may escape a thread entry point. The completion
				// remains owned and bounded even if diagnostic allocation also failed.
				job.Completion.ArtifactBytes.clear();
				job.Completion.Status = ProceduralJobStatus::Failed;
				job.Completion.Diagnostic.Code = procedural::Status::AllocationFailed;
				job.Completion.Diagnostic.Field.clear();
			}
			job.Finished.store(true, std::memory_order_release);
		}
	}

	struct ProceduralJobs::State {
		uint64_t StoreIdentity = 0;
		std::vector<std::unique_ptr<procedural_job_detail::Job>> Jobs{};
		std::vector<ProceduralCompletion> Completed{};
		procedural::Diagnostic Diagnostic{};
	};

	ProceduralJobs::ProceduralJobs(const ecs::Store &store) : Held(std::make_unique<State>()) {
		Held->StoreIdentity = store.Identity();
		Held->Jobs.reserve(ProceduralHostLimits::MaximumRetainedRequests);
		Held->Completed.reserve(ProceduralHostLimits::MaximumRetainedRequests);
	}

	ProceduralJobs::~ProceduralJobs() {
		for (const auto &job : Held->Jobs)
			job->Control.StopRequested.store(true, std::memory_order_release);
		for (const auto &job : Held->Jobs)
			if (job->Worker.joinable()) job->Worker.join();
	}

	uint64_t
	ProceduralJobs::Submit(ecs::Store &store, procedural::RegionScope scope, parallel::JobContext context) {
		using namespace procedural_detail;
		ENGINE_PROFILE("Procedural::Submit");
		Held->Diagnostic = {};
		if (!store.IsOnOwningThread() || store.Identity() != Held->StoreIdentity || store.AdoptOnly()) {
			Refuse(Held->Diagnostic, procedural::Status::InvalidInput, "request.owner");
			return 0;
		}
		if (context != parallel::JobContext::Serial && context != parallel::JobContext::Threaded) {
			Refuse(Held->Diagnostic, procedural::Status::InvalidInput, "request.context");
			return 0;
		}
		if (Held->Jobs.size() >= ProceduralHostLimits::MaximumRetainedRequests ||
			ReservedBytes() >
				ProceduralHostLimits::MaximumRetainedBytes - ProceduralHostLimits::BytesPerRequest) {
			Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "request.queue");
			return 0;
		}
		try {
			auto job = std::make_unique<procedural_job_detail::Job>();
			if (!CaptureProceduralRegion(store, scope, job->Recipe, job->Request, Held->Diagnostic)) return 0;
			const auto *authority = store.Resource<ProceduralAuthority>();
			job->StoreIdentity = store.Identity();
			job->AuthorityIncarnation = authority->Incarnation;
			job->Owner = authority->Owner;
			if (procedural::BuildRegionKey(job->Recipe, job->Request, job->ExpectedKey, Held->Diagnostic) !=
				procedural::Status::Ok)
				return 0;
			job->Ticket = procedural_job_detail::NextTicket();
			if (job->Ticket == 0) {
				Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "request.ticket");
				return 0;
			}
			job->Request.RequestSequence = job->Ticket;
			job->Completion.Ticket = job->Ticket;
			job->Completion.Scope = scope;
			job->Completion.GenerationKey = Hash(job->ExpectedKey);
			const uint64_t ticket = job->Ticket;
			// Instrumentation may allocate when a counter is first interned. Finish
			// it before retaining a request whose ticket must reach the caller.
			core::Metrics::Count("procedural.submissions", 1);
			if (context == parallel::JobContext::Serial) {
				procedural_job_detail::Execute(*job);
			} else {
				// Retain the request before starting a thread, so a throwing thread
				// constructor cannot leave a joinable worker without an owner.
				auto *owned = job.get();
				Held->Jobs.push_back(std::move(job));
				try {
					owned->Worker = std::thread([owned] { procedural_job_detail::Execute(*owned); });
				} catch (...) {
					Held->Jobs.pop_back();
					Refuse(Held->Diagnostic, procedural::Status::AllocationFailed, "request.worker");
					return 0;
				}
				return ticket;
			}
			Held->Jobs.push_back(std::move(job));
			return ticket;
		} catch (const std::bad_alloc &) {
			Refuse(Held->Diagnostic, procedural::Status::AllocationFailed, "request.bytes");
			return 0;
		}
	}

	void ProceduralJobs::Poll() {
		ENGINE_PROFILE("Procedural::Poll");
		for (const auto &job : Held->Jobs) {
			if (job->Observed) continue;
			if (!job->Finished.load(std::memory_order_acquire)) break;
			if (job->Control.StopRequested.load(std::memory_order_acquire)) {
				job->Completion.ArtifactBytes.clear();
				job->Completion.Status = ProceduralJobStatus::Canceled;
				job->Completion.Diagnostic = {procedural::Status::Canceled, {}};
			}
			Held->Completed.push_back(std::move(job->Completion));
			job->Observed = true;
		}
	}

	bool ProceduralJobs::Cancel(uint64_t ticket) {
		for (const auto &completion : Held->Completed)
			if (completion.Ticket == ticket && completion.Status == ProceduralJobStatus::Published)
				return false;
		const auto found = std::find_if(Held->Jobs.begin(), Held->Jobs.end(), [&](const auto &job) {
			return job->Ticket == ticket;
		});
		if (found == Held->Jobs.end()) return false;
		(*found)->Control.StopRequested.store(true, std::memory_order_release);
		for (auto &completion : Held->Completed) {
			if (completion.Ticket != ticket) continue;
			completion.Status = ProceduralJobStatus::Canceled;
			completion.ArtifactBytes.clear();
			completion.Diagnostic = {procedural::Status::Canceled, {}};
		}
		return true;
	}

	bool ProceduralJobs::Progress(uint64_t ticket, ProceduralProgress &out) const {
		const auto found = std::find_if(Held->Jobs.begin(), Held->Jobs.end(), [&](const auto &job) {
			return job->Ticket == ticket;
		});
		if (found == Held->Jobs.end()) return false;
		const auto &job = **found;
		ProceduralProgress progress;
		progress.Ticket = ticket;
		if (job.Control.StopRequested.load(std::memory_order_acquire))
			progress.Status = ProceduralJobStatus::Canceled;
		for (const auto &completion : Held->Completed)
			if (completion.Ticket == ticket) progress.Status = completion.Status;
		progress.CandidateCells = job.Control.CandidateCells.load(std::memory_order_relaxed);
		progress.WorkUnits = job.Control.WorkUnits.load(std::memory_order_relaxed);
		progress.SpatialTests = job.Control.SpatialTests.load(std::memory_order_relaxed);
		progress.Placements = job.Control.Placements.load(std::memory_order_relaxed);
		progress.Roads = job.Control.Roads.load(std::memory_order_relaxed);
		out = progress;
		return true;
	}

	std::span<const ProceduralCompletion> ProceduralJobs::Completions() const {
		return Held->Completed;
	}
	size_t ProceduralJobs::RetainedCount() const {
		return Held->Jobs.size();
	}
	size_t ProceduralJobs::ReservedBytes() const {
		return Held->Jobs.size() * ProceduralHostLimits::BytesPerRequest;
	}
	const procedural::Diagnostic &ProceduralJobs::LastDiagnostic() const {
		return Held->Diagnostic;
	}

	void ProceduralJobs::ClearCompletions() {
		for (const auto &job : Held->Jobs)
			if (job->Observed && job->Worker.joinable()) job->Worker.join();
		std::erase_if(Held->Jobs, [](const auto &job) { return job->Observed; });
		Held->Completed.clear();
	}

	bool ProceduralJobs::PublishBatch(
		ecs::Store &store, std::span<const uint64_t> tickets, uint64_t activationTick
	) {
		using namespace procedural_detail;
		ENGINE_PROFILE("Procedural::Publish");
		Held->Diagnostic = {};
		if (!store.IsOnOwningThread() || store.Identity() != Held->StoreIdentity || store.AdoptOnly() ||
			activationTick != store.Time().Tick)
			return Refuse(Held->Diagnostic, procedural::Status::InvalidInput, "publication.owner_or_tick");
		if (tickets.empty() || tickets.size() > ProceduralHostLimits::MaximumRetainedRequests)
			return Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "publication.tickets");
		const auto *authority = store.Resource<ProceduralAuthority>();
		const auto *published = store.Resource<ProceduralPublished>();
		if (!authority || !published)
			return Refuse(Held->Diagnostic, procedural::Status::MissingSource, "publication.state");
		if (published->Regions.size() > ProceduralHostLimits::MaximumRegions)
			return Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "publication.regions");
		size_t previousBytes = 0;
		for (const auto &row : published->Regions) {
			if (row.ArtifactBytes.size() > ProceduralHostLimits::MaximumPublishedBytes - previousBytes)
				return Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "publication.bytes");
			previousBytes += row.ArtifactBytes.size();
		}
		try {
			std::vector<ProceduralCompletion *> accepted;
			std::vector<uint64_t> acceptedRevisions;
			accepted.reserve(tickets.size());
			acceptedRevisions.reserve(tickets.size());
			for (const uint64_t ticket : tickets) {
				const auto job = std::find_if(Held->Jobs.begin(), Held->Jobs.end(), [&](const auto &value) {
					return value->Ticket == ticket;
				});
				const auto completion =
					std::find_if(Held->Completed.begin(), Held->Completed.end(), [&](const auto &value) {
						return value.Ticket == ticket;
					});
				if (job == Held->Jobs.end() || completion == Held->Completed.end() ||
					completion->Status != ProceduralJobStatus::Ready ||
					(*job)->StoreIdentity != store.Identity() ||
					(*job)->AuthorityIncarnation != authority->Incarnation ||
					(*job)->Owner != authority->Owner ||
					(*job)->Control.StopRequested.load(std::memory_order_acquire))
					return Refuse(Held->Diagnostic, procedural::Status::StaleArtifact, "publication.ticket");
				for (const auto *prior : accepted)
					if (SameScope(prior->Scope, completion->Scope))
						return Refuse(
							Held->Diagnostic, procedural::Status::InvalidInput, "publication.duplicate_scope"
						);
				procedural::CompiledRecipe compiled;
				procedural::RegionRequest current;
				if (!CaptureProceduralRegion(store, completion->Scope, compiled, current, Held->Diagnostic))
					return false;
				std::vector<uint8_t> currentKey;
				if (procedural::BuildRegionKey(compiled, current, currentKey, Held->Diagnostic) !=
					procedural::Status::Ok)
					return false;
				if (current.ExpectedSourceRevision != (*job)->Request.ExpectedSourceRevision ||
					currentKey != (*job)->ExpectedKey || Hash(currentKey) != completion->GenerationKey ||
					Hash(completion->ArtifactBytes) != completion->ArtifactDigest)
					return Refuse(Held->Diagnostic, procedural::Status::StaleArtifact, "publication.inputs");
				procedural::RegionResult decoded;
				if (procedural::DecodeRegionResult(
						completion->ArtifactBytes, decoded, Held->Diagnostic, currentKey
					) != procedural::Status::Ok)
					return false;
				accepted.push_back(&*completion);
				acceptedRevisions.push_back(current.ExpectedSourceRevision);
			}
			ProceduralPublished candidate = *published;
			for (size_t index = 0; index < accepted.size(); ++index) {
				const auto &completion = *accepted[index];
				ProceduralPublication replacement{
					completion.Scope,
					completion.GenerationKey,
					completion.ArtifactDigest,
					completion.ArtifactBytes,
					acceptedRevisions[index],
					activationTick
				};
				const auto target = std::lower_bound(
					candidate.Regions.begin(),
					candidate.Regions.end(),
					completion.Scope,
					[](const auto &row, auto scope) { return ScopeLess(row.Scope, scope); }
				);
				if (target != candidate.Regions.end() && SameScope(target->Scope, completion.Scope))
					*target = std::move(replacement);
				else
					candidate.Regions.insert(target, std::move(replacement));
			}
			if (candidate.Regions.size() > ProceduralHostLimits::MaximumRegions)
				return Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "publication.regions");
			size_t retained = 0;
			for (const auto &row : candidate.Regions) {
				if (row.ArtifactBytes.size() > ProceduralHostLimits::MaximumPublishedBytes - retained)
					return Refuse(Held->Diagnostic, procedural::Status::LimitExceeded, "publication.bytes");
				retained += row.ArtifactBytes.size();
			}
			// Mutable lookup and all allocations precede the single authoritative edit.
			// The existing resource's vector then swaps without allocating or calling
			// observers, so no rollback path can itself fail halfway through a batch.
			auto *destination = store.ResourceMutable<ProceduralPublished>();
			core::Metrics::Count("procedural.publications", static_cast<double>(accepted.size()));
			core::Metrics::Count("procedural.output_bytes", static_cast<double>(retained));
			static_assert(std::is_nothrow_swappable_v<decltype(destination->Regions)>);
			destination->Regions.swap(candidate.Regions);
			for (auto *completion : accepted)
				completion->Status = ProceduralJobStatus::Published;
			return true;
		} catch (const std::bad_alloc &) {
			return Refuse(Held->Diagnostic, procedural::Status::AllocationFailed, "publication.bytes");
		}
	}
}
