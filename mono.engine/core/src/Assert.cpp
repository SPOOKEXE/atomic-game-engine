#include <engine/core/Assert.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdlib>
#include <iterator>

namespace engine::core {

	namespace {
		// **Flushes before it aborts.** The sink buffers, and a line describing
		// the invariant that broke is worth nothing if the process dies with it
		// still in a buffer. This is the whole reason the default is a function
		// here rather than `std::abort` itself.
		void AbortAfterReporting(std::string_view, const LogSite &) {
			Log::Flush();
			std::abort();
		}

		std::atomic<AssertHandler> &Handler() {
			static std::atomic<AssertHandler> handler{&AbortAfterReporting};
			return handler;
		}

		std::atomic<uint64_t> &Count() {
			static std::atomic<uint64_t> count{0};
			return count;
		}
	}

	bool Assert::IsCompiledIn() {
		return ENGINE_ASSERTS_ENABLED != 0;
	}

	void Assert::Fail(std::string_view expression, const LogSite &site) {
		Count().fetch_add(1, std::memory_order_relaxed);
		Log::WriteUnfiltered(site, "assertion failed: {}", fmt::make_format_args(expression));
		Handler().load(std::memory_order_relaxed)(expression, site);
	}

	void Assert::FailFormatted(
		std::string_view expression, const LogSite &site, fmt::string_view format, fmt::format_args arguments
	) {
		Count().fetch_add(1, std::memory_order_relaxed);

		// **The detail is formatted first and the whole line written once**, so
		// that an assert firing on a job worker is one `log()` call under the
		// sink's lock rather than two that another thread can get between.
		fmt::memory_buffer detail;
		fmt::vformat_to(std::back_inserter(detail), format, arguments);
		std::string_view rendered(detail.data(), detail.size());

		Log::WriteUnfiltered(site, "assertion failed: {} · {}", fmt::make_format_args(expression, rendered));
		Handler().load(std::memory_order_relaxed)(expression, site);
	}

	bool Assert::Report(std::string_view expression, const LogSite &site, LogThrottle &throttle) {
		Count().fetch_add(1, std::memory_order_relaxed);

		// One a second per call site. A check that fails every frame is the
		// case worth having, and sixty identical lines a second is how it stops
		// being read.
		uint64_t due = throttle.Due(1.0);
		if (due == 0) {
			return false;
		}

		uint64_t suppressed = due - 1;
		if (suppressed == 0) {
			Log::WriteUnfiltered(site, "check failed: {}", fmt::make_format_args(expression));
		} else {
			Log::WriteUnfiltered(
				site, "check failed: {} (+{} suppressed)", fmt::make_format_args(expression, suppressed)
			);
		}
		return false;
	}

	AssertHandler Assert::SetHandler(AssertHandler handler) {
		return Handler().exchange(
			handler == nullptr ? &AbortAfterReporting : handler, std::memory_order_relaxed
		);
	}

	uint64_t Assert::Failures() {
		return Count().load(std::memory_order_relaxed);
	}
}
