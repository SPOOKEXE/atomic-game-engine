#include "ScriptComputeBridge.hpp"

#include "ComputeGraph.hpp"

#include <engine/core/Clock.hpp>
#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace engine::render {
	namespace script_compute_detail {
		constexpr uint64_t DEVICE_BYTES = 256u * 1024u * 1024u;
		constexpr uint64_t UPLOAD_BYTES = 64u * 1024u * 1024u;
		constexpr uint64_t RESULT_BYTES = 16u * 1024u * 1024u;
		constexpr uint64_t PROGRAM_BYTES = 64u * 1024u;
		constexpr size_t MAXIMUM_ENTRIES = 8;
		namespace cg = computegraph;
		bool Text(std::string_view value) {
			return !value.empty() && value.size() <= 128 && value.find('\0') == std::string_view::npos;
		}
		bool Bounded(const cg::Program &program) {
			const auto &d = program.Definition;
			if (d.Buffers.capacity() > 128 || d.Kernels.capacity() > 64 || d.Passes.capacity() > 256 ||
				program.Kernels.size() != d.Kernels.size() || program.ErrorBuffers.capacity() > 128 ||
				program.Kernels.capacity() > 64)
				return false;
			uint64_t bytes = sizeof(cg::Program) + d.Buffers.capacity() * sizeof(cg::BufferDesc) +
							 d.Kernels.capacity() * sizeof(cg::KernelDesc) +
							 d.Passes.capacity() * sizeof(cg::PassDesc) +
							 program.Kernels.capacity() * sizeof(cg::KernelCode) +
							 program.ErrorBuffers.capacity() * sizeof(std::string);
			const auto add = [&](const std::string &text) {
				bytes += text.capacity();
				return bytes <= PROGRAM_BYTES;
			};
			for (const auto &b : d.Buffers)
				if (!add(b.Name) || !add(b.CountBuffer) || !add(b.Owner)) return false;
			size_t ports = 0, bindings = 0;
			for (const auto &k : d.Kernels) {
				ports += k.Ports.size();
				bytes += k.Ports.capacity() * sizeof(cg::KernelPort);
				if (ports > 512 || !add(k.Name)) return false;
				for (const auto &p : k.Ports)
					if (!add(p.Name)) return false;
			}
			for (const auto &p : d.Passes) {
				bindings += p.Bindings.size();
				bytes += p.Bindings.capacity() * sizeof(cg::Binding);
				if (bindings > 1024 || !add(p.Name) || !add(p.Kernel) || !add(p.DispatchBuffer)) return false;
				for (const auto &b : p.Bindings)
					if (!add(b.Port) || !add(b.Buffer)) return false;
			}
			for (const auto &k : program.Kernels)
				if (!add(k.Name) || !add(k.Source) || !add(k.BuiltIn)) return false;
			for (const auto &e : program.ErrorBuffers)
				if (!add(e)) return false;
			return bytes <= PROGRAM_BYTES;
		}
		struct Graph {
			cg::Owner Owner{};
			std::string Name{};
			cg::Document Definition{};
			uint64_t ControlUpload = 0;
			uint64_t ControlReadback = 0;
			std::unique_ptr<ComputeGraph> Executor{};
			bool TimingsEnabled = false;
		};
		struct Entry {
			uint64_t Ticket = 0;
			cg::Owner Owner{};
			std::string Name{};
			bool Definition = false;
			bool Started = false;
			bool CancelRequested = false;
			bool Discarded = false;
			bool InFlight = false;
			uint64_t UploadCharge = 0;
			uint64_t ResultCharge = 0;
			uint64_t ReadbackCharge = 0;
			cg::Program Program{};
			cg::Request Request{};
			script::ComputePoll Reply{};
			cg::Completion Result{};
			std::shared_ptr<Graph> Against{};
		};
		struct Flight {
			std::shared_ptr<Entry> Request{};
			std::shared_ptr<Graph> Against{};
			uint64_t Ticket = 0;
			bool Cancelled = false;
			bool Collected = false;
			cg::Status Code = cg::Status::NotReady;
			cg::Completion Result{};
			ComputeGraphTiming Timing{};
		};
	}
	struct ScriptComputeBridge::State {
		using Entry = script_compute_detail::Entry;
		using Graph = script_compute_detail::Graph;
		void *Device = nullptr;
		std::thread::id Thread = std::this_thread::get_id();
		mutable std::mutex Mutex;
		bool Active = true;
		uint64_t NextTicket = 1;
		std::vector<computegraph::Owner> Owners{};
		std::vector<std::shared_ptr<Entry>> Entries{};
		std::vector<std::shared_ptr<Graph>> Graphs{};
		std::vector<script_compute_detail::Flight> Flights{};
		std::unique_ptr<ScriptComputeProfile> Profiling;
		ScriptComputeJobProfile *JobProfile(const computegraph::Owner &owner, uint64_t ticket) {
			if (!Profiling) return nullptr;
			for (auto &job : Profiling->Jobs)
				if (job.Owner == owner && job.Ticket == ticket) return &job;
			return nullptr;
		}
		// Caller holds Mutex. Physical retirement is distinct from CPU cancellation/readiness.
		void RetireProfile(const Entry &entry, bool cancelled) {
			if (!Profiling || entry.Definition) return;
			auto *job = JobProfile(entry.Owner, entry.Ticket);
			if (job && job->Retired) return;
			++Profiling->RetiredJobs;
			if (job) {
				job->Retired = true;
				job->Cancelled = job->Cancelled || cancelled || entry.CancelRequested;
				if (!job->SubmitToReadyNanoseconds)
					job->SubmitToReadyNanoseconds = core::Clock::Nanoseconds() - job->QueuedAt;
			}
			if (cancelled) ++Profiling->GpuUnavailable;
		}
		void ProfileCharges() {
			if (!Profiling) return;
			auto &p = *Profiling;
			p.CurrentEntries = Entries.size();
			p.CurrentUploadBytes = p.CurrentReadbackBytes = p.CurrentResultBytes = 0;
			for (const auto &entry : Entries) {
				p.CurrentUploadBytes += entry->UploadCharge;
				p.CurrentReadbackBytes += entry->ReadbackCharge;
				p.CurrentResultBytes += entry->ResultCharge;
			}
			p.PeakEntries = std::max(p.PeakEntries, p.CurrentEntries);
			p.PeakUploadBytes = std::max(p.PeakUploadBytes, p.CurrentUploadBytes);
			p.PeakReadbackBytes = std::max(p.PeakReadbackBytes, p.CurrentReadbackBytes);
			p.PeakResultBytes = std::max(p.PeakResultBytes, p.CurrentResultBytes);
		}
		explicit State(void *device) : Device(device) {
			Owners.reserve(64);
			Entries.reserve(8);
			Graphs.reserve(8);
			Flights.reserve(8);
		}
		struct AdmissionCount {
			State &Host;
			bool Accepted = false;
			~AdmissionCount() {
				if (Accepted) return;
				std::lock_guard lock(Host.Mutex);
				if (!Accepted && Host.Profiling) ++Host.Profiling->Refused;
			}
		};
		void RequireThread() const {
			if (Thread != std::this_thread::get_id()) std::abort();
		}
		bool Owns(const computegraph::Owner &owner) const {
			return Active && std::find(Owners.begin(), Owners.end(), owner) != Owners.end();
		}
		std::shared_ptr<Entry> Find(const computegraph::Owner &owner, uint64_t ticket) const {
			for (const auto &entry : Entries)
				if (!entry->Discarded && entry->Owner == owner && entry->Ticket == ticket) return entry;
			return {};
		}
		void Detach(const computegraph::Owner &owner) {
			std::lock_guard lock(Mutex);
			std::erase(Owners, owner);
			for (auto &entry : Entries)
				if (entry->Owner == owner) {
					entry->CancelRequested = true;
					entry->Discarded = true;
					if (auto *job = JobProfile(owner, entry->Ticket)) {
						job->Released = true;
						job->Cancelled = true;
					}
					if (!entry->Started) RetireProfile(*entry, true);
				}
		}
		struct Endpoint final : script::ComputeBridge {
			std::weak_ptr<State> Host;
			computegraph::Owner Owner;
			Endpoint(std::weak_ptr<State> host, computegraph::Owner owner)
				: Host(std::move(host)), Owner(std::move(owner)) {}
			const computegraph::Owner &Identity() const override {
				return Owner;
			}
			computegraph::Status Define(
				std::string name, computegraph::Program program, uint64_t &ticket, std::string &detail
			) override;
			computegraph::Status Submit(
				std::string name, computegraph::Request request, uint64_t &ticket, std::string &detail
			) override;
			computegraph::Status Poll(uint64_t ticket, script::ComputePoll &out) override try {
				auto host = Host.lock();
				if (!host) return computegraph::Status::DeviceUnavailable;
				std::lock_guard lock(host->Mutex);
				if (!host->Owns(Owner)) return computegraph::Status::StaleOwner;
				auto entry = host->Find(Owner, ticket);
				if (!entry) return computegraph::Status::UnknownTicket;
				if (host->Profiling) ++host->Profiling->PollCalls;
				auto candidate = entry->Reply;
				out = std::move(candidate);
				return computegraph::Status::Ok;
			} catch (const std::bad_alloc &) {
				return computegraph::Status::AllocationFailed;
			}
			computegraph::Status Read(
				uint64_t ticket,
				std::string_view name,
				uint64_t offset,
				size_t maximum,
				std::vector<std::byte> &out
			) override try {
				auto host = Host.lock();
				if (!host) return computegraph::Status::DeviceUnavailable;
				std::lock_guard lock(host->Mutex);
				if (!host->Owns(Owner)) return computegraph::Status::StaleOwner;
				auto entry = host->Find(Owner, ticket);
				if (!entry) return computegraph::Status::UnknownTicket;
				if (!entry->Reply.Ready) return computegraph::Status::NotReady;
				if (entry->Reply.Cancelled) return computegraph::Status::StaleOwner;
				if (entry->Reply.Code != computegraph::Status::Ok) return entry->Reply.Code;
				if (maximum > 65536) return computegraph::Status::LimitExceeded;
				for (const auto &buffer : entry->Result.Outputs)
					if (buffer.Buffer == name) {
						if (offset > buffer.Bytes.size()) return computegraph::Status::InvalidCount;
						const auto size = std::min<uint64_t>(maximum, buffer.Bytes.size() - offset);
						std::vector<std::byte> bytes(
							buffer.Bytes.begin() + static_cast<size_t>(offset),
							buffer.Bytes.begin() + static_cast<size_t>(offset + size)
						);
						out = std::move(bytes);
						if (host->Profiling) {
							++host->Profiling->ReadCalls;
							host->Profiling->ReadBytes += size;
							if (auto *job = host->JobProfile(Owner, ticket)) {
								++job->ReadCalls;
								job->ReadBytes += size;
							}
						}
						return computegraph::Status::Ok;
					}
				return computegraph::Status::UnknownBuffer;
			} catch (const std::bad_alloc &) {
				return computegraph::Status::AllocationFailed;
			}
			bool Cancel(uint64_t ticket) override {
				auto host = Host.lock();
				if (!host) return false;
				std::lock_guard lock(host->Mutex);
				if (!host->Owns(Owner)) return false;
				auto entry = host->Find(Owner, ticket);
				if (!entry || entry->Reply.Ready) return false;
				entry->CancelRequested = true;
				if (host->Profiling) {
					++host->Profiling->Cancelled;
					if (auto *job = host->JobProfile(Owner, ticket)) {
						job->Cancelled = true;
						job->SubmitToReadyNanoseconds = core::Clock::Nanoseconds() - job->QueuedAt;
					}
					if (!entry->Started) host->RetireProfile(*entry, true);
				}
				entry->Reply.Ready = true;
				entry->Reply.Cancelled = true;
				entry->Reply.Outputs.clear();
				return true;
			}
			bool Release(uint64_t ticket) override {
				auto host = Host.lock();
				if (!host) return false;
				std::lock_guard lock(host->Mutex);
				auto entry = host->Find(Owner, ticket);
				if (!entry || !entry->Reply.Ready) return false;
				entry->Discarded = true;
				if (host->Profiling) {
					++host->Profiling->Released;
					if (auto *job = host->JobProfile(Owner, ticket)) {
						job->Released = true;
						job->SubmitToReleaseNanoseconds = core::Clock::Nanoseconds() - job->QueuedAt;
					}
				}
				return true;
			}
			void Detach() override {
				if (auto host = Host.lock()) host->Detach(Owner);
			}
		};
		computegraph::Status Admit(std::shared_ptr<Entry> entry, uint64_t &ticket, std::string &detail) {
			if (!Owns(entry->Owner)) return computegraph::Status::StaleOwner;
			if (!Device) return computegraph::Status::DeviceUnavailable;
			if (Entries.size() >= 8 || NextTicket == 0) return computegraph::Status::LimitExceeded;
			uint64_t upload = 0, result = 0, readback = 0;
			for (const auto &pending : Entries) {
				upload += pending->UploadCharge;
				result += pending->ResultCharge;
				readback += pending->ReadbackCharge;
			}
			if (entry->UploadCharge > script_compute_detail::UPLOAD_BYTES - upload ||
				entry->ResultCharge > script_compute_detail::RESULT_BYTES - result ||
				entry->ReadbackCharge > script_compute_detail::RESULT_BYTES - readback)
				return computegraph::Status::LimitExceeded;
			std::optional<ScriptComputeJobProfile> profileRecord;
			if (Profiling && !entry->Definition && Profiling->Jobs.size() < 16) {
				profileRecord.emplace();
				profileRecord->Owner = entry->Owner;
			}
			entry->Ticket = NextTicket++;
			Entries.push_back(entry);
			if (Profiling) {
				if (entry->Definition)
					++Profiling->Definitions;
				else {
					++Profiling->Submitted;
					if (Profiling->Jobs.size() == 16)
						++Profiling->DroppedRecords;
					else {
						profileRecord->Ticket = entry->Ticket;
						profileRecord->QueuedAt = core::Clock::Nanoseconds();
						static_assert(std::is_nothrow_move_constructible_v<ScriptComputeJobProfile>);
						Profiling->Jobs.push_back(std::move(*profileRecord));
					}
				}
				ProfileCharges();
			}
			ticket = entry->Ticket;
			detail.clear();
			return computegraph::Status::Ok;
		}
	};
	computegraph::Status ScriptComputeBridge::State::Endpoint::Define(
		std::string name, computegraph::Program program, uint64_t &ticket, std::string &detail
	) try {
		auto host = Host.lock();
		if (!host) return computegraph::Status::DeviceUnavailable;
		State::AdmissionCount admission{*host};
		if (!script_compute_detail::Text(name) || name.capacity() > 128)
			return computegraph::Status::InvalidName;
		if (!script_compute_detail::Bounded(program)) return computegraph::Status::LimitExceeded;
		auto entry = std::make_shared<Entry>();
		entry->Owner = Owner;
		entry->Name = std::move(name);
		entry->Definition = true;
		entry->Program = std::move(program);
		std::lock_guard lock(host->Mutex);
		const auto status = host->Admit(std::move(entry), ticket, detail);
		admission.Accepted = status == computegraph::Status::Ok;
		return status;
	} catch (const std::bad_alloc &) {
		detail.clear();
		return computegraph::Status::AllocationFailed;
	}
	computegraph::Status ScriptComputeBridge::State::Endpoint::Submit(
		std::string name, computegraph::Request request, uint64_t &ticket, std::string &detail
	) try {
		auto host = Host.lock();
		if (!host) return computegraph::Status::DeviceUnavailable;
		State::AdmissionCount admission{*host};
		if (!script_compute_detail::Text(name) || name.capacity() > 128)
			return computegraph::Status::InvalidName;
		std::lock_guard lock(host->Mutex);
		if (!host->Owns(Owner)) return computegraph::Status::StaleOwner;
		std::shared_ptr<Graph> graph;
		for (const auto &candidate : host->Graphs)
			if (candidate->Owner == Owner && candidate->Name == name) graph = candidate;
		if (!graph) return computegraph::Status::NotReady;
		if (request.Inputs.capacity() > 128 || request.Outputs.capacity() > 128)
			return computegraph::Status::LimitExceeded;
		uint64_t uploads = 0, results = 0;
		for (const auto &input : request.Inputs) {
			// Includes caller marshalling, inbox and executor queue copies, retained through retirement.
			if (!script_compute_detail::Text(input.Buffer) || input.Buffer.capacity() > 128 ||
				input.Bytes.capacity() > script_compute_detail::UPLOAD_BYTES / 3)
				return computegraph::Status::LimitExceeded;
			uploads += input.Bytes.capacity() * 3;
		}
		for (const auto &output : request.Outputs) {
			if (!script_compute_detail::Text(output.Buffer) || output.Buffer.capacity() > 128)
				return computegraph::Status::InvalidName;
			auto buffer = std::find_if(
				graph->Definition.Buffers.begin(), graph->Definition.Buffers.end(), [&](const auto &b) {
					return b.Name == output.Buffer;
				}
			);
			if (buffer == graph->Definition.Buffers.end()) return computegraph::Status::UnknownBuffer;
			if (output.MaximumElements > buffer->Capacity) return computegraph::Status::InvalidCount;
			results += uint64_t(output.MaximumElements) * buffer->Stride;
		}
		// Two result copies may coexist while the executor publishes and the inbox takes ownership.
		if (results > script_compute_detail::RESULT_BYTES / 2) return computegraph::Status::LimitExceeded;
		auto entry = std::make_shared<Entry>();
		entry->Owner = Owner;
		entry->Name = std::move(name);
		request.Identity = Owner;
		entry->Request = std::move(request);
		entry->Against = std::move(graph);
		entry->UploadCharge = uploads + entry->Against->ControlUpload;
		entry->ReadbackCharge = results + entry->Against->ControlReadback;
		entry->ResultCharge = results * 2;
		const auto status = host->Admit(std::move(entry), ticket, detail);
		admission.Accepted = status == computegraph::Status::Ok;
		return status;
	} catch (const std::bad_alloc &) {
		detail.clear();
		return computegraph::Status::AllocationFailed;
	}
	ScriptComputeBridge::ScriptComputeBridge(void *device) : Held(std::make_shared<State>(device)) {}
	ScriptComputeBridge::~ScriptComputeBridge() {
		Shutdown();
	}
	script::ComputeBridgeFactory ScriptComputeBridge::ForWorld(std::string world, uint32_t view) {
		if (!script_compute_detail::Text(world) || world.capacity() > 128) return {};
		std::lock_guard lock(Held->Mutex);
		if (!Held->Active) return {};
		return [host = std::weak_ptr<State>(Held),
				world = std::move(world),
				view](uint64_t runtime, uint64_t generation) {
			auto state = host.lock();
			if (!state || runtime == 0 || generation == 0) return std::shared_ptr<script::ComputeBridge>{};
			std::lock_guard guard(state->Mutex);
			computegraph::Owner owner{world, runtime, generation, view};
			if (!state->Active || state->Owners.size() >= 64 ||
				std::find(state->Owners.begin(), state->Owners.end(), owner) != state->Owners.end())
				return std::shared_ptr<script::ComputeBridge>{};
			auto endpoint = std::make_shared<State::Endpoint>(state, owner);
			state->Owners.push_back(std::move(owner));
			return std::shared_ptr<script::ComputeBridge>(std::move(endpoint));
		};
	}
	uint64_t ScriptComputeBridge::ReservedDeviceBytes() const {
		Held->RequireThread();
		uint64_t bytes = 0;
		for (const auto &graph : Held->Graphs)
			bytes += graph->Executor->ReservedDeviceBytes();
		for (const auto &flight : Held->Flights)
			if (std::find(Held->Graphs.begin(), Held->Graphs.end(), flight.Against) == Held->Graphs.end()) {
				// Replaced graphs may have several flights; charge each retired executor once.
				bool seen = false;
				for (const auto &other : Held->Flights) {
					if (&other == &flight) break;
					if (other.Against == flight.Against) {
						seen = true;
						break;
					}
				}
				if (!seen) bytes += flight.Against->Executor->ReservedDeviceBytes();
			}
		return bytes;
	}
	void ScriptComputeBridge::Pump(uint64_t outsideBytes) try {
		ENGINE_PROFILE_CAT("script compute pump", core::ProfileCategory::Render);
		Held->RequireThread();
		// Owner-only executor records stay alive in Flights after cancellation or replacement.
		for (auto &flight : Held->Flights) {
			bool cancel = false;
			{
				std::lock_guard lock(Held->Mutex);
				cancel = flight.Request->CancelRequested || !Held->Owns(flight.Request->Owner);
			}
			if (cancel && !flight.Cancelled) {
				flight.Against->Executor->Cancel(flight.Ticket);
				flight.Cancelled = true;
			}
			flight.Against->Executor->Poll();
			if (!flight.Cancelled && !flight.Collected) {
				computegraph::Diagnostic diagnostic;
				flight.Code = flight.Against->Executor->Take(
					flight.Ticket, flight.Result, diagnostic, Held->Profiling ? &flight.Timing : nullptr
				);
				if (flight.Code == computegraph::Status::NotReady) continue;
				flight.Collected = true;
			}
			if (flight.Cancelled && flight.Against->Executor->PendingGpuCount() != 0) continue;
			std::lock_guard lock(Held->Mutex);
			auto &entry = *flight.Request;
			if (!entry.CancelRequested && Held->Owns(entry.Owner)) {
				script::ComputePoll reply;
				reply.Ready = true;
				reply.Code = flight.Code == computegraph::Status::Ok ? flight.Result.Code : flight.Code;
				if (reply.Code != computegraph::Status::Ok)
					reply.Detail = flight.Against->Executor->LastError();
				if (reply.Code == computegraph::Status::Ok)
					for (const auto &buffer : flight.Result.Outputs)
						reply.Outputs.push_back(
							{buffer.Buffer, buffer.Type, buffer.Count, buffer.Bytes.size()}
						);
				if (Held->Profiling) {
					++Held->Profiling->Completed;
					if (auto *job = Held->JobProfile(entry.Owner, entry.Ticket)) {
						job->SubmitToReadyNanoseconds = core::Clock::Nanoseconds() - job->QueuedAt;
						job->Device = flight.Timing;
						job->DeviceCountsAvailable = flight.Against->TimingsEnabled;
					}
					if (!flight.Against->TimingsEnabled)
						++Held->Profiling->GpuUnavailable;
					else if (flight.Timing.Available)
						++Held->Profiling->GpuTimed;
					else
						++Held->Profiling->GpuDropped;
				}
				entry.Result = std::move(flight.Result);
				entry.Reply = std::move(reply);
			}
			Held->RetireProfile(entry, flight.Cancelled);
			entry.InFlight = false;
		}
		std::erase_if(Held->Flights, [](const auto &flight) { return !flight.Request->InFlight; });
		std::vector<std::shared_ptr<State::Entry>> work;
		{
			std::lock_guard lock(Held->Mutex);
			std::erase_if(Held->Entries, [](const auto &entry) {
				return entry->Discarded && !entry->InFlight;
			});
			std::erase_if(Held->Graphs, [&](const auto &graph) {
				return !Held->Owns(graph->Owner) && graph->Executor->PendingGpuCount() == 0;
			});
			for (auto &entry : Held->Entries)
				if (!entry->Started) {
					entry->Started = true;
					work.push_back(entry);
				}
		}
		for (auto &entry : work) {
			{
				std::lock_guard lock(Held->Mutex);
				if (entry->CancelRequested || !Held->Owns(entry->Owner)) {
					entry->Reply.Ready = true;
					entry->Reply.Cancelled = true;
					entry->Request = {};
					entry->Program = {};
					Held->RetireProfile(*entry, true);
					continue;
				}
			}
			computegraph::Diagnostic diagnostic;
			computegraph::Status status = computegraph::Status::Ok;
			std::string detail;
			if (entry->Definition) {
				auto graph = std::make_shared<State::Graph>();
				graph->Owner = entry->Owner;
				graph->Name = entry->Name;
				graph->Definition = entry->Program.Definition;
				// Installed and retired packages together are bounded, including pending operation snapshots.
				if (Held->Graphs.size() + Held->Flights.size() >= script_compute_detail::MAXIMUM_ENTRIES)
					status = computegraph::Status::LimitExceeded;
				else {
					graph->Executor = std::make_unique<ComputeGraph>(Held->Device, entry->Owner);
					status = graph->Executor->Install(entry->Program, diagnostic);
					detail = graph->Executor->LastError();
					if (status == computegraph::Status::Ok && Held->Profiling)
						graph->TimingsEnabled = graph->Executor->EnableGpuTimings(true);
					if (status == computegraph::Status::Ok) {
						std::vector<std::string> counts;
						for (const auto &buffer : graph->Definition.Buffers)
							if (!buffer.CountBuffer.empty() &&
								std::find(counts.begin(), counts.end(), buffer.CountBuffer) == counts.end())
								counts.push_back(buffer.CountBuffer);
						for (const auto &buffer : graph->Definition.Buffers) {
							const bool count =
								std::find(counts.begin(), counts.end(), buffer.Name) != counts.end();
							const bool error = std::find(
												   entry->Program.ErrorBuffers.begin(),
												   entry->Program.ErrorBuffers.end(),
												   buffer.Name
											   ) != entry->Program.ErrorBuffers.end();
							if (count || error) {
								const uint64_t bytes = uint64_t(buffer.LogicalCount) * buffer.Stride;
								if (!buffer.External) graph->ControlUpload += bytes;
								graph->ControlReadback += count ? 4 : bytes;
							}
						}
					}
				}
				std::lock_guard lock(Held->Mutex);
				if (status == computegraph::Status::Ok && Held->Owns(entry->Owner) &&
					!entry->CancelRequested) {
					std::erase_if(Held->Graphs, [&](const auto &old) {
						return old->Owner == graph->Owner && old->Name == graph->Name;
					});
					Held->Graphs.push_back(std::move(graph));
				}
				entry->Program = {};
			} else {
				const auto total = ReservedDeviceBytes();
				const auto own = entry->Against->Executor->ReservedDeviceBytes();
				if (outsideBytes > script_compute_detail::DEVICE_BYTES ||
					total > script_compute_detail::DEVICE_BYTES - outsideBytes ||
					!entry->Against->Executor->SetOutsideDeviceBytes(outsideBytes + total - own))
					status = computegraph::Status::LimitExceeded;
				uint64_t ticket = 0;
				if (status == computegraph::Status::Ok) {
					{
						std::lock_guard lock(Held->Mutex);
						if (auto *job = Held->JobProfile(entry->Owner, entry->Ticket))
							job->QueueNanoseconds = core::Clock::Nanoseconds() - job->QueuedAt;
					}
					status = entry->Against->Executor->Submit(entry->Request, ticket, diagnostic);
				}
				if (status != computegraph::Status::Ok) detail = entry->Against->Executor->LastError();
				if (status == computegraph::Status::Ok) {
					std::lock_guard lock(Held->Mutex);
					entry->InFlight = true;
					Held->Flights.push_back({.Request = entry, .Against = entry->Against, .Ticket = ticket});
					if (Held->Profiling)
						Held->Profiling->PeakDeviceBytes =
							std::max(Held->Profiling->PeakDeviceBytes, ReservedDeviceBytes());
				}
				entry->Request = {};
			}
			if (entry->Definition || status != computegraph::Status::Ok) {
				std::lock_guard lock(Held->Mutex);
				if (status != computegraph::Status::Ok && Held->Profiling) {
					++Held->Profiling->Refused;
					Held->RetireProfile(*entry, false);
				}
				if (!entry->CancelRequested && Held->Owns(entry->Owner)) {
					entry->Reply.Ready = true;
					entry->Reply.Code = status;
					if (status != computegraph::Status::Ok)
						entry->Reply.Detail =
							detail.empty() ? computegraph::Describe(status) : std::move(detail);
				}
			}
		}
		if (Held->Profiling) {
			std::lock_guard lock(Held->Mutex);
			Held->ProfileCharges();
			Held->Profiling->CurrentDeviceBytes = ReservedDeviceBytes();
		}
	} catch (const std::bad_alloc &) {
		// A collected completion stays in its Flight until metadata publication succeeds.
		// Work not submitted yet remains retryable; a normal pump never waits for memory or GPU work.
		std::lock_guard lock(Held->Mutex);
		for (auto &entry : Held->Entries)
			if (!entry->InFlight && !entry->Reply.Ready) entry->Started = false;
	}

	bool ScriptComputeBridge::EnableProfiling(script::ComputeBridge &endpoint, bool enabled) {
		auto *native = dynamic_cast<State::Endpoint *>(&endpoint);
		if (!native) return false;
		auto host = native->Host.lock();
		if (!host) return false;
		host->RequireThread();
		std::lock_guard lock(host->Mutex);
		if (!host->Entries.empty() || !host->Flights.empty() || !host->Graphs.empty()) return false;
		if (!enabled) {
			host->Profiling.reset();
			return true;
		}
		auto candidate = std::make_unique<ScriptComputeProfile>();
		candidate->Enabled = true;
		candidate->Jobs.reserve(16);
		host->Profiling = std::move(candidate);
		return true;
	}
	ScriptComputeProfile ScriptComputeBridge::TakeProfile(script::ComputeBridge &endpoint) {
		auto *native = dynamic_cast<State::Endpoint *>(&endpoint);
		if (!native) return {};
		auto host = native->Host.lock();
		if (!host) return {};
		host->RequireThread();
		std::lock_guard lock(host->Mutex);
		if (!host->Profiling) return {};
		auto snapshot = *host->Profiling;
		std::erase_if(snapshot.Jobs, [](const auto &job) { return !job.Released || !job.Retired; });
		std::erase_if(host->Profiling->Jobs, [](const auto &job) { return job.Released && job.Retired; });
		return snapshot;
	}

	void ScriptComputeBridge::Shutdown() {
		if (!Held) return;
		Held->RequireThread();
		{
			std::lock_guard lock(Held->Mutex);
			if (!Held->Active) return;
			Held->Active = false;
			Held->Owners.clear();
		}
		for (auto &flight : Held->Flights)
			flight.Against->Executor->Shutdown();
		for (auto &graph : Held->Graphs)
			graph->Executor->Shutdown();
		Held->Flights.clear();
		Held->Graphs.clear();
		{
			std::lock_guard lock(Held->Mutex);
			Held->Entries.clear();
			Held->Device = nullptr;
		}
	}
}
