#pragma once

// Shared typed scenario checks. Runtime adapters retain their native loops and
// publish owned observations after joining their work. No live runtime or test
// assertion framework crosses this boundary.
//
// @tier L3 shared

#include <engine/parallel/Jobs.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace engine::testing {

	// Complete stable identity of one runtime history and its selected checks.
	// Unused features may use empty strings; relevant choices must be canonical.
	struct ScenarioKey {
		// Scene, content and initial state identity.
		//@{
		std::string Scene = {};
		std::string ContentRevision = {};
		std::string Configuration = {};
		uint64_t Seed = 0;
		//@}
		// Ordered native advancement and actions.
		//@{
		std::string TickSchedule = {};
		std::string FrameSchedule = {};
		std::string Actions = {};
		//@}
		// Transport, faults, capture provenance and schema/check revisions.
		//@{
		std::string Transport = {};
		std::string Faults = {};
		std::string CaptureConfiguration = {};
		std::string ObservationSchema = {};
		std::string CheckSet = {};
		//@}
		// Compares every identity field without hashing or delimiter ambiguity.
		bool operator==(const ScenarioKey &) const = default;
	};

	// One named required hook with a positive minimum delivered sample count.
	struct HookRequirement {
		// Adapter-defined stable hook identity.
		std::string Hook = {};
		// Zero is invalid: absence cannot satisfy a required hook.
		size_t MinimumSamples = 1;
		// Compares the complete requirement.
		bool operator==(const HookRequirement &) const = default;
	};

	// Evidence and retention needed by one independent check.
	struct CheckRequirements {
		// Every declared hook must be present and complete, with no lost records.
		std::vector<HookRequirement> Hooks;
		// Minimum total matching observations; zero is invalid.
		size_t MinimumSamples = 1;
		// Retains matching observations for the completion callback.
		bool RetainTimeline = false;
		// Compares all declared evidence requirements.
		bool operator==(const CheckRequirements &) const = default;
	};

	// Attributed value finding, reported by the calling test after worker joins.
	struct Finding {
		// Stable property name and useful failure context.
		//@{
		std::string Property = {};
		std::string Message = {};
		//@}
		// Whether this property held.
		bool Passed = false;
		// Compares findings for deterministic serial/parallel parity checks.
		bool operator==(const Finding &) const = default;
	};

	// Independently owned output slot for one named check.
	struct CheckResult {
		// Attribution preserved through a shared scenario run.
		//@{
		std::string Name = {};
		std::string SourceSuite = {};
		//@}
		// False after any failed finding, missing requirement or exception.
		bool Passed = true;
		// Exact number of matching delivered observations.
		size_t Samples = 0;
		// Findings remain in publication order, then completion order.
		std::vector<Finding> Findings;
		// Records one property and preserves any prior failure.
		void Record(std::string_view property, bool passed, std::string_view message = {});
		// Exact bounded-output loss. Any overflow makes the check fail.
		//@{
		size_t DroppedFindings = 0;
		size_t DroppedFindingBytes = 0;
		//@}
		// Logical owned finding text bytes currently retained.
		size_t FindingBytes() const;
		// Compares semantic outputs, without execution timing.
		bool operator==(const CheckResult &) const = default;

	  private:
		template <typename> friend class ScenarioSession;
		size_t MaximumFindings = 4096;
		size_t MaximumFindingBytes = 1024u * 1024u;
		size_t RetainedFindingBytes = 0;
	};

	// Actual work counters, distinct from a cache hit's avoided execution.
	struct ScenarioMetrics {
		// Driver initializations and immutable publications.
		//@{
		size_t SetupCount = 0;
		size_t PublishedSamples = 0;
		size_t PublishedBytes = 0;
		//@}
		// Explicit bounded-retention loss and peak retained payload.
		//@{
		size_t DroppedSamples = 0;
		size_t DroppedBytes = 0;
		size_t PeakRetainedSamples = 0;
		size_t PeakRetainedBytes = 0;
		//@}
		// Fork-join dispatch work and owner wall time in nanoseconds.
		//@{
		size_t CheckDispatches = 0;
		uint64_t CheckNanoseconds = 0;
		uint64_t DriverNanoseconds = 0;
		//@}
	};

	// Completed report. Contains no retained observations or live driver state.
	struct ScenarioReport {
		// Exact scenario identity.
		ScenarioKey Key;
		// Named check outputs in declaration order.
		std::vector<CheckResult> Checks;
		// Driver, cleanup or declaration failures in occurrence order.
		std::vector<std::string> Failures;
		// Counts and timings from actual execution, zero on cache hits.
		ScenarioMetrics Metrics;
		// Marks reuse of a completed report rather than a new execution.
		bool Reused = false;
		// Exact failure diagnostic overflow, which cannot become success.
		//@{
		size_t DroppedFailures = 0;
		size_t DroppedFailureBytes = 0;
		//@}
		// Appends bounded owner-side failure text and preserves overflow evidence.
		void
		RecordFailure(std::string_view message, size_t maximumBytes = 8192, std::string_view prefix = {});
		// Succeeds only when execution and every named check succeeded.
		bool Passed() const;
	};

	// Bounds and owner-side cancellation. Timeouts are cooperative, not preemption.
	struct ScenarioOptions {
		// Whole independent checks are dispatch units; host owns the job pool.
		parallel::JobContext Context = parallel::JobContext::Threaded;
		// Maximum retained timeline payload and observation count.
		//@{
		size_t MaximumRetainedSamples = 256;
		size_t MaximumRetainedBytes = 64u * 1024u * 1024u;
		//@}
		// Maximum single publication payload plus hook/failure text, including streams.
		size_t MaximumObservationBytes = 64u * 1024u * 1024u;
		// Maximum per-check retained findings and total property/message text bytes.
		//@{
		size_t MaximumFindingsPerCheck = 4096;
		size_t MaximumFindingBytesPerCheck = 1024u * 1024u;
		//@}
		// Maximum total key, check attribution and required-hook text bytes.
		size_t MaximumIdentityBytes = 64u * 1024u;
		// Maximum retained owner failure text bytes; at most eight messages.
		size_t MaximumFailureBytes = 8192;
		// Absolute deadline is calculated once when the session begins.
		std::chrono::milliseconds Timeout{30'000};
		// Checked before/after native driver phases and each publication.
		std::stop_token Cancellation;
	};

	// Owned copied sample. Payload must not hide borrowed or mutable aliases.
	template <typename Sample> struct Observation {
		// Adapter hook and native logical tick/frame stamp.
		//@{
		std::string Hook = {};
		uint64_t Tick = 0;
		//@}
		// Copied facts and actual owned payload byte count, including nested buffers.
		//@{
		Sample Payload;
		size_t RetainedBytes = sizeof(Sample);
		//@}
		// Hook/readback completeness, exact upstream loss and diagnostic.
		//@{
		bool Available = true;
		bool Complete = true;
		size_t LostSamples = 0;
		std::string Failure = {};
		//@}
	};

	// One independent consumer. Captures may own check-local state only.
	template <typename Sample> struct ScenarioCheck {
		// Stable name and original suite identity.
		//@{
		std::string Name = {};
		std::string SourceSuite = {};
		//@}
		// Explicit evidence requirements; an empty hook list accepts all hooks.
		CheckRequirements Requirements;
		// Called once for each matching immutable publication.
		std::function<void(const Observation<Sample> &, CheckResult &)> Observe = {};
		// Called after native driver completion, with the bounded retained timeline.
		std::function<void(std::span<const std::shared_ptr<const Observation<Sample>>>, CheckResult &)>
			Complete = {};
	};

	template <typename Sample> class ScenarioSession;
	template <typename Sample> class ScenarioCache;

	// Native adapter callbacks. Finish is invoked exactly once after any start,
	// including preparation failure. All three run on the calling owner thread.
	template <typename Sample> struct ScenarioDriver {
		// Optional initialization before entering the native loop.
		std::function<void(ScenarioSession<Sample> &)> Prepare = {};
		// Required driver; advances the existing runtime and publishes copies.
		std::function<void(ScenarioSession<Sample> &)> Run = {};
		// Optional cleanup; exceptions become report failures.
		std::function<void()> Finish = {};
	};

	namespace detail {
		// Borrows the active exception text inside its caller catch block only.
		std::string_view ScenarioException();
		// Saturating accounting cannot wrap a byte counter into a plausible small value.
		size_t ScenarioSum(size_t left, size_t right);
		// Logical key/report storage, including fixed objects and owned text.
		size_t ScenarioKeyBytes(const ScenarioKey &key);
		// Logical retained report bytes; allocator capacity is not claimed.
		size_t ScenarioReportBytes(const ScenarioReport &report);
	}

	// One owner-bound driver invocation with independent fork-joined check slots.
	template <typename Sample> class ScenarioSession {
	  public:
		// Captures declarations by value so callbacks have independent lifetime.
		ScenarioSession(
			ScenarioKey key, std::vector<ScenarioCheck<Sample>> checks, ScenarioOptions options = {}
		)
			: Checks(std::move(checks)), Options(std::move(options)), Owner(std::this_thread::get_id()),
			  Deadline(std::chrono::steady_clock::now() + Options.Timeout) {
			size_t identityBytes = detail::ScenarioKeyBytes(key) - sizeof(ScenarioKey);
			for (const auto &check : Checks) {
				identityBytes = detail::ScenarioSum(identityBytes, check.Name.size());
				identityBytes = detail::ScenarioSum(identityBytes, check.SourceSuite.size());
				for (const auto &hook : check.Requirements.Hooks) {
					identityBytes = detail::ScenarioSum(identityBytes, hook.Hook.size());
				}
			}
			if (identityBytes > Options.MaximumIdentityBytes ||
				Checks.size() > Options.MaximumIdentityBytes / 2 ||
				std::any_of(Checks.begin(), Checks.end(), [&](const auto &check) {
					return check.Requirements.Hooks.size() > Options.MaximumIdentityBytes;
				})) {
				Report.RecordFailure(
					"scenario identity exceeds declared text budget", Options.MaximumFailureBytes
				);
				return;
			}
			Report.Key = std::move(key);
			Report.Checks.reserve(Checks.size());
			HookCounts.resize(Checks.size());
			Disabled.resize(Checks.size(), 0);
			RetentionDrops.resize(Checks.size(), 0);
			for (const auto &check : Checks) {
				CheckResult output;
				output.Name = check.Name;
				output.SourceSuite = check.SourceSuite;
				output.MaximumFindings = Options.MaximumFindingsPerCheck;
				output.MaximumFindingBytes = Options.MaximumFindingBytesPerCheck;
				Report.Checks.push_back(std::move(output));
				HookCounts[Report.Checks.size() - 1].resize(check.Requirements.Hooks.size(), 0);
			}
		}

		// A session is single-use and never shared with workers as mutable state.
		ScenarioSession(const ScenarioSession &) = delete;
		// A session cannot acquire another session's owner affinity.
		ScenarioSession &operator=(const ScenarioSession &) = delete;

		// Cooperatively aborts at a native owner-side barrier.
		void Checkpoint() const {
			RequireOwner();
			if (Options.Cancellation.stop_requested()) {
				throw std::runtime_error("scenario cancelled");
			}
			if (std::chrono::steady_clock::now() >= Deadline) {
				throw std::runtime_error("scenario deadline exceeded");
			}
		}

		// Moves one owned observation into immutable publication, then joins all
		// eligible checks before the driver can mutate its runtime again.
		void Publish(Observation<Sample> observation) {
			Checkpoint();
			if (!Running) {
				throw std::logic_error("scenario publication outside native driver");
			}
			if (observation.Hook.empty()) {
				throw std::invalid_argument("scenario observation hook is empty");
			}
			const size_t metadataBytes =
				detail::ScenarioSum(observation.Hook.size(), observation.Failure.size());
			if (metadataBytes > Options.MaximumObservationBytes ||
				observation.RetainedBytes > Options.MaximumObservationBytes - metadataBytes) {
				throw std::length_error("scenario observation exceeds byte limit");
			}
			const size_t publicationBytes = observation.RetainedBytes + metadataBytes;
			const auto published = std::make_shared<const Observation<Sample>>(std::move(observation));
			Report.Metrics.PublishedSamples++;
			Report.Metrics.PublishedBytes =
				detail::ScenarioSum(Report.Metrics.PublishedBytes, publicationBytes);
			bool retain = false;
			for (const auto &check : Checks) {
				retain = retain || (check.Requirements.RetainTimeline && Matches(check, published->Hook));
			}
			if (retain) {
				const bool room = Retained.size() < Options.MaximumRetainedSamples &&
								  publicationBytes <= Options.MaximumRetainedBytes - RetainedBytes;
				if (room) {
					Retained.push_back(published);
					RetainedBytes += publicationBytes;
					Report.Metrics.PeakRetainedSamples = Retained.size();
					Report.Metrics.PeakRetainedBytes = RetainedBytes;
				} else {
					Report.Metrics.DroppedSamples++;
					Report.Metrics.DroppedBytes =
						detail::ScenarioSum(Report.Metrics.DroppedBytes, publicationBytes);
					for (size_t index = 0; index < Checks.size(); index++) {
						if (Checks[index].Requirements.RetainTimeline &&
							Matches(Checks[index], published->Hook)) {
							RetentionDrops[index]++;
						}
					}
				}
			}
			Dispatch([&](size_t index) {
				auto &check = Checks[index];
				if (!Matches(check, published->Hook)) {
					return;
				}
				auto &output = Report.Checks[index];
				output.Samples++;
				for (size_t hook = 0; hook < check.Requirements.Hooks.size(); hook++) {
					if (check.Requirements.Hooks[hook].Hook == published->Hook) {
						HookCounts[index][hook]++;
					}
				}
				if (!published->Available || !published->Complete || published->LostSamples > 0 ||
					!published->Failure.empty()) {
					output.Record(
						published->Hook,
						false,
						published->Failure.empty() ? std::string_view("incomplete observation")
												   : std::string_view(published->Failure)
					);
					if (published->LostSamples > 0) {
						output.Record("lost samples", false, std::to_string(published->LostSamples));
					}
				}
				if (!Disabled[index] && check.Observe) {
					check.Observe(*published, output);
				}
			});
			Checkpoint();
		}

		// Runs the native driver once, always cleans up, finalizes checks and
		// releases every retained observation before returning a value report.
		ScenarioReport Run(const ScenarioDriver<Sample> &driver) {
			RequireOwner();
			if (Started) {
				throw std::logic_error("scenario session already run");
			}
			Started = true;
			const auto began = std::chrono::steady_clock::now();
			try {
				if (!DeclarationsValidated) {
					Validate(driver);
				}
				Checkpoint();
				Report.Metrics.SetupCount = 1;
				Running = true;
				if (driver.Prepare) {
					driver.Prepare(*this);
				}
				Checkpoint();
				driver.Run(*this);
				Checkpoint();
			} catch (...) {
				// Native failures become report data; cleanup must still run.
				Report.RecordFailure(detail::ScenarioException(), Options.MaximumFailureBytes);
			}
			Running = false;
			try {
				if (driver.Finish) {
					driver.Finish();
				}
			} catch (...) {
				// Cleanup failure cannot suppress the original driver diagnosis.
				Report.RecordFailure(detail::ScenarioException(), Options.MaximumFailureBytes, "cleanup: ");
			}
			Report.Metrics.DriverNanoseconds = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began)
					.count()
			);
			if (Report.Checks.size() == Checks.size()) {
				Finalize();
			}
			Retained.clear();
			RetainedBytes = 0;
			return std::move(Report);
		}

	  private:
		template <typename> friend class ScenarioCache;

		bool Matches(const ScenarioCheck<Sample> &check, const std::string &hook) const {
			return check.Requirements.Hooks.empty() ||
				   std::any_of(
					   check.Requirements.Hooks.begin(),
					   check.Requirements.Hooks.end(),
					   [&](const HookRequirement &required) { return required.Hook == hook; }
				   );
		}

		void RequireOwner() const {
			if (std::this_thread::get_id() != Owner) {
				throw std::logic_error("scenario operation requires the native owner thread");
			}
		}

		void Validate(const ScenarioDriver<Sample> &driver) {
			if (!Report.Failures.empty() || Report.DroppedFailures > 0) {
				throw std::invalid_argument("scenario identity was refused");
			}
			if (Report.Key.Scene.empty() || Report.Key.ObservationSchema.empty()) {
				throw std::invalid_argument("scenario needs scene and observation schema identities");
			}
			if (!driver.Run || Checks.empty() || Options.Context == parallel::JobContext::Processed) {
				throw std::invalid_argument("scenario needs a driver and local independent checks");
			}
			for (size_t index = 0; index < Checks.size(); index++) {
				const auto &check = Checks[index];
				if (check.Name.empty() || check.SourceSuite.empty() ||
					check.Requirements.MinimumSamples == 0 || (!check.Observe && !check.Complete)) {
					throw std::invalid_argument("scenario check declaration is incomplete");
				}
				for (size_t prior = 0; prior < index; prior++) {
					if (Checks[prior].Name == check.Name && Checks[prior].SourceSuite == check.SourceSuite) {
						throw std::invalid_argument("duplicate scenario check identity");
					}
				}
				for (size_t hook = 0; hook < check.Requirements.Hooks.size(); hook++) {
					const auto &required = check.Requirements.Hooks[hook];
					if (required.Hook.empty() || required.MinimumSamples == 0) {
						throw std::invalid_argument("required scenario hook needs positive evidence");
					}
					for (size_t prior = 0; prior < hook; prior++) {
						if (check.Requirements.Hooks[prior].Hook == required.Hook) {
							throw std::invalid_argument("duplicate scenario hook requirement");
						}
					}
				}
			}
			DeclarationsValidated = true;
		}

		template <typename Body> void Dispatch(const Body &body) {
			const auto began = std::chrono::steady_clock::now();
			// One index is a whole check, rather than a cheap row. The explicit
			// floor permits the requested concurrent check path; adapters measure
			// its workload before making speed claims.
			const auto context = Options.Context == parallel::JobContext::Processed
									 ? parallel::JobContext::Serial
									 : Options.Context;
			parallel::Jobs::For(
				context,
				Checks.size(),
				1,
				[&](size_t begin, size_t end) {
					for (size_t index = begin; index < end; index++) {
						try {
							body(index);
						} catch (...) {
							// Isolate one check while every other result slot completes.
							Disabled[index] = 1;
							Report.Checks[index].Record("exception", false, detail::ScenarioException());
						}
					}
				},
				2
			);
			Report.Metrics.CheckDispatches++;
			Report.Metrics.CheckNanoseconds += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began)
					.count()
			);
		}

		void Finalize() {
			Dispatch([&](size_t index) {
				auto &check = Checks[index];
				auto &output = Report.Checks[index];
				if (output.Samples < check.Requirements.MinimumSamples) {
					output.Record("evidence", false, "insufficient required observations");
				}
				for (size_t hook = 0; hook < check.Requirements.Hooks.size(); hook++) {
					const auto &required = check.Requirements.Hooks[hook];
					if (HookCounts[index][hook] < required.MinimumSamples) {
						output.Record(required.Hook, false, "missing required hook samples");
					}
				}
				if (RetentionDrops[index] > 0) {
					output.Record("retention", false, "timeline exceeded declared retention budget");
				}
				if (!Report.Failures.empty() || Report.DroppedFailures > 0) {
					output.Record("driver", false, "scenario execution or cleanup failed");
				}
				if (!Disabled[index] && check.Complete) {
					std::vector<std::shared_ptr<const Observation<Sample>>> matching;
					if (check.Requirements.RetainTimeline) {
						for (const auto &observation : Retained) {
							if (Matches(check, observation->Hook)) {
								matching.push_back(observation);
							}
						}
					}
					check.Complete(matching, output);
				}
			});
		}

		std::vector<ScenarioCheck<Sample>> Checks;
		ScenarioOptions Options;
		std::thread::id Owner;
		std::chrono::steady_clock::time_point Deadline;
		ScenarioReport Report;
		std::vector<std::vector<size_t>> HookCounts;
		std::vector<uint8_t> Disabled;
		std::vector<size_t> RetentionDrops;
		std::vector<std::shared_ptr<const Observation<Sample>>> Retained;
		size_t RetainedBytes = 0;
		bool DeclarationsValidated = false;
		bool Started = false;
		bool Running = false;
	};

	// Executes a fresh scenario with no process-global ownership or cache.
	template <typename Sample>
	ScenarioReport RunScenario(
		ScenarioKey key,
		std::vector<ScenarioCheck<Sample>> checks,
		const ScenarioDriver<Sample> &driver,
		ScenarioOptions options = {}
	) {
		ScenarioSession<Sample> session(std::move(key), std::move(checks), std::move(options));
		return session.Run(driver);
	}

	// Bounded owner-local completed-report reuse. Runtime/sample state is never
	// cached. CheckSet must be nonempty and change with callback semantics.
	template <typename Sample> class ScenarioCache {
	  public:
		// Zero entries or bytes disables retention. Bytes measure logical owned
		// objects and text, not allocator spare capacity or heap commitment.
		explicit ScenarioCache(size_t maximumEntries = 16, size_t maximumBytes = 64u * 1024u * 1024u)
			: MaximumEntries(maximumEntries), MaximumBytes(maximumBytes), Owner(std::this_thread::get_id()) {}
		// Owner affinity cannot be transferred or copied.
		ScenarioCache(const ScenarioCache &) = delete;
		// Owner affinity cannot be transferred or copied.
		ScenarioCache &operator=(const ScenarioCache &) = delete;

		// Validates each request before looking up complete keys. Conflicting
		// declarations fail without invoking a native driver. Eviction is FIFO.
		ScenarioReport
		Run(ScenarioKey key,
			std::vector<ScenarioCheck<Sample>> checks,
			const ScenarioDriver<Sample> &driver,
			ScenarioOptions options = {}) {
			RequireOwner();
			// Transfer call-owned inputs once. Validation borrows declarations;
			// neither a refusal nor a cache hit clones callback captures.
			ScenarioSession<Sample> session(std::move(key), std::move(checks), options);
			try {
				session.Validate(driver);
				session.Checkpoint();
				if (session.Report.Key.CheckSet.empty()) {
					throw std::invalid_argument("cacheable scenario needs a check-set revision");
				}
			} catch (...) {
				// A malformed cache request must never bypass validation via a hit.
				ScenarioReport refused;
				refused.Key = std::move(session.Report.Key);
				refused.RecordFailure(detail::ScenarioException(), options.MaximumFailureBytes);
				return refused;
			}
			std::vector<Declaration> declarations;
			for (const auto &check : session.Checks) {
				declarations.push_back(
					{check.Name,
					 check.SourceSuite,
					 check.Requirements,
					 static_cast<bool>(check.Observe),
					 static_cast<bool>(check.Complete)}
				);
			}
			const Bounds bounds{
				options.Context,
				options.MaximumRetainedSamples,
				options.MaximumRetainedBytes,
				options.MaximumObservationBytes,
				options.MaximumFindingsPerCheck,
				options.MaximumFindingBytesPerCheck,
				options.MaximumIdentityBytes,
				options.MaximumFailureBytes,
				options.Timeout
			};
			for (const auto &entry : Entries) {
				if (!(entry.Report.Key == session.Report.Key)) continue;
				if (entry.Declarations != declarations || !(entry.Limits == bounds)) {
					ScenarioReport refused;
					refused.Key = std::move(session.Report.Key);
					refused.RecordFailure(
						"conflicting scenario key reuse: declarations or limits differ",
						options.MaximumFailureBytes
					);
					return refused;
				}
				auto reused = entry.Report;
				reused.Reused = true;
				reused.Metrics = {};
				return reused;
			}
			auto report = session.Run(driver);
			const size_t bytes = EntryBytes(report, declarations);
			if (MaximumEntries > 0 && bytes <= MaximumBytes && report.Passed()) {
				while (!Entries.empty() &&
					   (Entries.size() >= MaximumEntries || bytes > MaximumBytes - RetainedBytes)) {
					RetainedBytes -= Entries.front().Bytes;
					Entries.erase(Entries.begin());
				}
				Entries.push_back({report, std::move(declarations), bounds, bytes});
				RetainedBytes += bytes;
			}
			return report;
		}

		// Releases completed reports at the end of a selected execution group.
		void Clear() {
			RequireOwner();
			Entries.clear();
			RetainedBytes = 0;
		}
		// Current bounded completed-report count, on the owner thread.
		size_t Size() const {
			RequireOwner();
			return Entries.size();
		}
		// Logical retained cache bytes, including identities and declarations.
		size_t Bytes() const {
			RequireOwner();
			return RetainedBytes;
		}

	  private:
		struct Declaration {
			std::string Name = {};
			std::string SourceSuite = {};
			CheckRequirements Requirements;
			bool Observe = false;
			bool Complete = false;
			bool operator==(const Declaration &) const = default;
		};
		struct Bounds {
			parallel::JobContext Context;
			size_t Samples;
			size_t Bytes;
			size_t ObservationBytes;
			size_t Findings;
			size_t FindingBytes;
			size_t IdentityBytes;
			size_t FailureBytes;
			std::chrono::milliseconds Timeout;
			bool operator==(const Bounds &) const = default;
		};
		struct Entry {
			ScenarioReport Report;
			std::vector<Declaration> Declarations;
			Bounds Limits;
			size_t Bytes;
		};
		static size_t EntryBytes(const ScenarioReport &report, const std::vector<Declaration> &declarations) {
			size_t bytes = detail::ScenarioSum(
				sizeof(Entry) - sizeof(ScenarioReport), detail::ScenarioReportBytes(report)
			);
			for (const auto &declaration : declarations) {
				bytes = detail::ScenarioSum(bytes, sizeof(Declaration));
				bytes = detail::ScenarioSum(bytes, declaration.Name.size());
				bytes = detail::ScenarioSum(bytes, declaration.SourceSuite.size());
				for (const auto &hook : declaration.Requirements.Hooks) {
					bytes = detail::ScenarioSum(bytes, sizeof(HookRequirement));
					bytes = detail::ScenarioSum(bytes, hook.Hook.size());
				}
			}
			return bytes;
		}
		void RequireOwner() const {
			if (std::this_thread::get_id() != Owner) {
				throw std::logic_error("scenario cache requires its owner thread");
			}
		}
		size_t MaximumEntries;
		size_t MaximumBytes;
		size_t RetainedBytes = 0;
		std::thread::id Owner;
		std::vector<Entry> Entries;
	};
}
