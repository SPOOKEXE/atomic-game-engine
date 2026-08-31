#pragma once

// Bounded asynchronous computation owned by one script runtime.
//
// A request is plain data and a result is an owned float buffer. VM values,
// world rows and pointers never cross the worker boundary. Runtime heartbeat
// code polls this queue without waiting and resumes completed tickets on the
// owner thread. Destroying the queue cancels and joins or reaps every worker.
//
// @tier L9 · shared

#include <engine/parallel/Jobs.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace engine::script {

	// A rectangular batch of samples compatible with Luau's `math.noise`.
	// Rows advance Z and columns advance X. Y remains fixed.
	//
	// @since v0.20
	struct NoiseGridRequest {
		// Output dimensions.
		//@{
		uint32_t Width = 0;
		uint32_t Depth = 0;
		//@}

		// Sampling origin and spacing.
		//@{
		double OriginX = 0.0;
		double OriginY = 0.0;
		double OriginZ = 0.0;
		double Step = 1.0;
		//@}
	};

	// One completed asynchronous computation.
	//
	// @since v0.20
	struct ComputeCompletion {
		// Request identity, output samples, and refusal diagnostic.
		//@{
		uint64_t Ticket = 0;
		std::vector<float> Values;
		std::string Error;
		//@}
	};

	// Per-runtime owner of asynchronous typed jobs.
	//
	// @since v0.20
	class ComputeJobs {
	  public:
		// Hard resource and publication limits for one runtime.
		//@{
		static constexpr size_t MAXIMUM_SAMPLES = 1024u * 1024u;
		static constexpr size_t MAXIMUM_PENDING = 8;
		static constexpr size_t SAMPLES_PER_HEARTBEAT = 4096;
		//@}

		ComputeJobs();
		~ComputeJobs();

		ComputeJobs(const ComputeJobs &) = delete;
		ComputeJobs &operator=(const ComputeJobs &) = delete;
		ComputeJobs(ComputeJobs &&) = delete;
		ComputeJobs &operator=(ComputeJobs &&) = delete;

		// Submits one owned noise grid. Zero means the request was refused and
		// `LastError` names why.
		uint64_t SubmitNoise(const NoiseGridRequest &request, parallel::JobContext context);

		// Observes workers and publishes the completed prefix in ticket order.
		// Never waits for a thread or process.
		void Poll();

		// Completed requests published in ticket order.
		std::span<const ComputeCompletion> Completions() const;

		// Releases every published completion.
		void ClearCompletions();

		// Number of submitted requests not yet published.
		size_t PendingCount() const;

		// Most recent synchronous refusal diagnostic.
		const std::string &LastError() const;

	  private:
		struct State;
		std::unique_ptr<State> Held;
	};

	// Sets the executable used for `Processed` jobs. Programs call this once
	// during startup, before constructing runtimes.
	void ConfigureComputeWorkerProgram(
		const std::filesystem::path &program, std::vector<std::string> arguments = {"--engine-compute-worker"}
	);

	// Whether argv asks this executable to serve one inherited compute channel.
	bool ComputeWorkerRequested(int argc, char **argv);

	// Serves one inherited channel until its request is answered or its parent
	// closes. Returns a process exit code.
	int RunComputeWorker();
}
