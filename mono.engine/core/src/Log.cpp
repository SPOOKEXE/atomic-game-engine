#include <engine/core/Clock.hpp>
#include <engine/core/Log.hpp>
#include <engine/core/Name.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace engine::core {

	namespace {
		// The macros compare `ENGINE_LOG_COMPILED_LEVEL` against the numbers in
		// `Log.hpp` and the runtime compares `LogLevel` values. The two orders
		// have to be the same order or a build compiles out a different set of
		// statements than it filters.
		static_assert(static_cast<uint8_t>(LogLevel::Trace) == ENGINE_LOG_LEVEL_TRACE);
		static_assert(static_cast<uint8_t>(LogLevel::Debug) == ENGINE_LOG_LEVEL_DEBUG);
		static_assert(static_cast<uint8_t>(LogLevel::Info) == ENGINE_LOG_LEVEL_INFO);
		static_assert(static_cast<uint8_t>(LogLevel::Warning) == ENGINE_LOG_LEVEL_WARNING);
		static_assert(static_cast<uint8_t>(LogLevel::Error) == ENGINE_LOG_LEVEL_ERROR);
		static_assert(static_cast<uint8_t>(LogLevel::Off) == ENGINE_LOG_LEVEL_OFF);

		// The relaxed store that pairs with `LogCategory::Load`.
		//
		// The header explains why the pair is written with builtins rather than
		// with `std::atomic`: `<atomic>` is 7,452 preprocessed lines and
		// `Log.hpp` has a stated budget. This half could use `std::atomic_ref`
		// and deliberately does not, because a pair whose two halves are
		// written differently is a pair somebody has to reason about twice.
		void Store(unsigned char *slot, uint8_t value) {
#if defined(__GNUC__) || defined(__clang__)
			__atomic_store_n(slot, value, __ATOMIC_RELAXED);
#else
			*static_cast<volatile unsigned char *>(slot) = value;
#endif
		}

		uint8_t Load(const unsigned char *slot) {
#if defined(__GNUC__) || defined(__clang__)
			return __atomic_load_n(slot, __ATOMIC_RELAXED);
#else
			return *static_cast<const volatile unsigned char *>(slot);
#endif
		}

		// One registered category.
		//
		// **Held in a deque and never removed**, for `core::Name`'s reason: a
		// `LogCategory` handed out at static-initialisation time keeps a pointer
		// to `Threshold`, and a container that reallocated would dangle every
		// call site in the program at once.
		struct CategoryRecord {
			Name Id;
			unsigned char Threshold = 0;
		};

		struct CategoryRegistry {
			std::mutex Guard;
			std::deque<CategoryRecord> Categories;

			// The floor a category gets when it is first registered, and what
			// `SetLevel(level)` writes to every category that already exists.
			unsigned char Default = static_cast<unsigned char>(LogLevel::Info);

			// Whether `file:line` is appended to the levels that carry it.
			unsigned char SourceLocation = 1;
		};

		// A function-local static, because a log statement can run during
		// another translation unit's static initialisation.
		CategoryRegistry &Categories() {
			static CategoryRegistry registry;
			return registry;
		}

		std::shared_ptr<spdlog::logger> &Instance() {
			static std::shared_ptr<spdlog::logger> logger;
			return logger;
		}

		// One mapping, used by `Write` and `Initialise` alike. It was a switch
		// inside `SetLevel` when that was the only caller; three copies of a
		// four-way mapping is how two of them end up disagreeing.
		spdlog::level::level_enum Severity(LogLevel level) {
			switch (level) {
			case LogLevel::Trace:
				return spdlog::level::trace;
			case LogLevel::Debug:
				return spdlog::level::debug;
			case LogLevel::Info:
				return spdlog::level::info;
			case LogLevel::Warning:
				return spdlog::level::warn;
			case LogLevel::Error:
				return spdlog::level::err;
			case LogLevel::Off:
				return spdlog::level::off;
			}
			return spdlog::level::info;
		}

		// The record for `name`, creating it at the current default if new.
		//
		// The caller holds no lock; this one takes it. Registration happens once
		// per call site per process, so the lock is off every hot path.
		CategoryRecord &Register(std::string_view name) {
			CategoryRegistry &registry = Categories();
			const Name id(name);

			std::lock_guard lock(registry.Guard);
			for (CategoryRecord &record : registry.Categories) {
				if (record.Id == id) {
					return record;
				}
			}

			registry.Categories.push_back(CategoryRecord{id, registry.Default});
			return registry.Categories.back();
		}

		// The last path component of `__FILE__`.
		//
		// **The basename rather than two components**, because the category
		// already names the module: a line reading `[render] Renderer.cpp:3225`
		// cannot be confused with `[client] main.cpp:171`, and the pair is
		// shorter than any path that would disambiguate them on its own.
		std::string_view FileOf(const char *path) {
			const std::string_view whole(path == nullptr ? "" : path);
			const size_t slash = whole.find_last_of("/\\");
			return slash == std::string_view::npos ? whole : whole.substr(slash + 1);
		}

		// One `category=level` or bare `level` out of a settings string.
		struct Term {
			std::string_view Whole;	   // As written, for the message a caller composes.
			std::string_view Category; // Empty for a bare level.
			std::string_view Level;
		};

		std::string_view TrimmedSpecification(std::string_view text) {
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
				text.remove_prefix(1);
			}
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
				text.remove_suffix(1);
			}
			return text;
		}

		// Splits a settings string into its terms.
		//
		// A vector rather than a callback, because `Configure` walks the same
		// list three times - once to refuse a misspelling before anything is
		// applied, once for the bare term and once for the named ones - and a
		// splitter written three times is a splitter that disagrees with itself
		// about whitespace. This runs at startup, at most once per source.
		std::vector<Term> TermsOf(std::string_view specification) {
			std::vector<Term> terms;
			size_t start = 0;
			while (start <= specification.size()) {
				const size_t comma = specification.find(',', start);
				const size_t length =
					comma == std::string_view::npos ? std::string_view::npos : comma - start;
				const std::string_view whole = TrimmedSpecification(specification.substr(start, length));
				start = comma == std::string_view::npos ? specification.size() + 1 : comma + 1;
				if (whole.empty()) {
					continue;
				}

				const size_t equals = whole.find('=');
				if (equals == std::string_view::npos) {
					terms.push_back(Term{whole, std::string_view{}, whole});
				} else {
					terms.push_back(
						Term{
							whole,
							TrimmedSpecification(whole.substr(0, equals)),
							TrimmedSpecification(whole.substr(equals + 1))
						}
					);
				}
			}
			return terms;
		}
	}

	const char *Describe(LogLevel level) {
		switch (level) {
		case LogLevel::Trace:
			return "trace";
		case LogLevel::Debug:
			return "debug";
		case LogLevel::Info:
			return "info";
		case LogLevel::Warning:
			return "warning";
		case LogLevel::Error:
			return "error";
		case LogLevel::Off:
			return "off";
		}
		return "info";
	}

	bool LevelFromName(std::string_view text, LogLevel &level) {
		std::string lowered;
		lowered.reserve(text.size());
		for (const char letter : text) {
			lowered.push_back(
				(letter >= 'A' && letter <= 'Z') ? static_cast<char>(letter - 'A' + 'a') : letter
			);
		}

		if (lowered == "trace") {
			level = LogLevel::Trace;
		} else if (lowered == "debug") {
			level = LogLevel::Debug;
		} else if (lowered == "info") {
			level = LogLevel::Info;
		} else if (lowered == "warning" || lowered == "warn") {
			level = LogLevel::Warning;
		} else if (lowered == "error") {
			level = LogLevel::Error;
		} else if (lowered == "off" || lowered == "none") {
			level = LogLevel::Off;
		} else {
			return false;
		}
		return true;
	}

	LogCategory::LogCategory(std::string_view name) {
		CategoryRecord &record = Register(name);
		Threshold = &record.Threshold;
		Identifier = record.Id.Id();
	}

	std::string_view LogCategory::Text() const {
		return Identifier == 0xFFFFFFFFu ? std::string_view{} : Name::FromId(Identifier).Text();
	}

	uint64_t LogThrottle::Due(double seconds) {
		const uint64_t now = Clock::Nanoseconds();

		// **Relaxed and racy on purpose.** Two threads arriving together may
		// both write a line; a lock on a path whose whole job is to write
		// nothing would cost more than the duplicate it prevents. The counter
		// is the same trade: a lost increment understates the suppressed total
		// by one and never turns a real burst into silence.
#if defined(__GNUC__) || defined(__clang__)
		const uint64_t next = __atomic_load_n(&NextNanoseconds, __ATOMIC_RELAXED);
		if (next != 0 && now < next) {
			__atomic_fetch_add(&Suppressed, 1, __ATOMIC_RELAXED);
			return 0;
		}

		const auto gap = static_cast<uint64_t>(seconds <= 0.0 ? 0.0 : seconds * 1e9);
		__atomic_store_n(&NextNanoseconds, now + gap, __ATOMIC_RELAXED);
		return __atomic_exchange_n(&Suppressed, 0, __ATOMIC_RELAXED) + 1;
#else
		if (NextNanoseconds != 0 && now < NextNanoseconds) {
			Suppressed++;
			return 0;
		}

		const auto gap = static_cast<uint64_t>(seconds <= 0.0 ? 0.0 : seconds * 1e9);
		NextNanoseconds = now + gap;
		const uint64_t suppressed = Suppressed;
		Suppressed = 0;
		return suppressed + 1;
#endif
	}

	void Log::Initialise(std::string_view program) {
		auto &logger = Instance();
		if (logger) {
			return;
		}

		logger = spdlog::stdout_color_mt(std::string(program));

		// `%t` is the thread id, which `docs/ARCH_REVIEW.md` §G1 asked for and
		// which the assert path depends on: an invariant that failed on a job
		// worker has to say which worker, or the line is a report about a
		// program with one thread.
		logger->set_pattern("[%T.%e] [%^%l%$] [t%t] %v");

		// **Left at `trace` for the life of the process, and the filtering
		// happens above it.** A level on the logger is one number for every
		// category, which is exactly what per-category levels exist to replace;
		// leaving it here as well would mean `--log net=trace` was silently
		// capped by a second setting the caller never asked about.
		logger->set_level(spdlog::level::trace);
	}

	void Log::SetLevel(LogLevel level) {
		CategoryRegistry &registry = Categories();
		std::lock_guard lock(registry.Guard);
		registry.Default = static_cast<unsigned char>(level);
		for (CategoryRecord &record : registry.Categories) {
			Store(&record.Threshold, static_cast<uint8_t>(level));
		}
	}

	LogLevel Log::Level() {
		CategoryRegistry &registry = Categories();
		std::lock_guard lock(registry.Guard);
		return static_cast<LogLevel>(registry.Default);
	}

	void Log::SetLevel(std::string_view category, LogLevel level) {
		CategoryRecord &record = Register(category);
		Store(&record.Threshold, static_cast<uint8_t>(level));
	}

	LogLevel Log::LevelOf(std::string_view category) {
		return static_cast<LogLevel>(Load(&Register(category).Threshold));
	}

	bool Log::Configure(std::string_view specification, std::string_view *unknown) {
		// **Checked whole before anything is applied.** A misspelling in the
		// third term that leaves the first two in force is a setting that half
		// worked, and half worked is harder to notice than not at all.
		for (const Term &term : TermsOf(specification)) {
			LogLevel level = LogLevel::Info;
			if (!LevelFromName(term.Level, level)) {
				if (unknown != nullptr) {
					*unknown = term.Whole;
				}
				return false;
			}
		}

		// **The bare term first, whatever order it was written in.** A bare
		// level sets every category and the default for categories not yet
		// seen, so `net=trace,warning` and `warning,net=trace` would otherwise
		// mean different things - and nobody reading either of them expects
		// that.
		for (const Term &term : TermsOf(specification)) {
			LogLevel level = LogLevel::Info;
			if (term.Category.empty() && LevelFromName(term.Level, level)) {
				SetLevel(level);
			}
		}
		for (const Term &term : TermsOf(specification)) {
			LogLevel level = LogLevel::Info;
			if (!term.Category.empty() && LevelFromName(term.Level, level)) {
				SetLevel(term.Category, level);
			}
		}

		return true;
	}

	LogLevel Log::CompiledFloor() {
		return static_cast<LogLevel>(ENGINE_LOG_COMPILED_LEVEL);
	}

	uint32_t Log::CategoryCount() {
		CategoryRegistry &registry = Categories();
		std::lock_guard lock(registry.Guard);
		return static_cast<uint32_t>(registry.Categories.size());
	}

	LogCategory Log::CategoryAt(uint32_t index) {
		CategoryRegistry &registry = Categories();
		std::string_view name;
		{
			std::lock_guard lock(registry.Guard);
			if (index >= registry.Categories.size()) {
				return LogCategory{};
			}
			name = registry.Categories[index].Id.Text();
		}
		return LogCategory(name);
	}

	bool Log::Enabled(LogLevel level, std::string_view category) {
		return static_cast<uint8_t>(level) >= Load(&Register(category).Threshold);
	}

	void Log::SetSourceLocationShown(bool shown) {
		CategoryRegistry &registry = Categories();
		Store(&registry.SourceLocation, shown ? 1 : 0);
	}

	void Log::Write(
		LogLevel level,
		const LogCategory &category,
		const LogSite &site,
		uint64_t suppressed,
		fmt::string_view format,
		fmt::format_args arguments
	) {
		// **Into a stack buffer rather than a `std::string`.** `fmt::memory_buffer`
		// carries 500 bytes inline, so the common line costs no allocation at
		// all, and spdlog is handed a view of it rather than a second copy.
		fmt::memory_buffer message;
		const std::string_view name = category.Text();
		if (!name.empty()) {
			fmt::format_to(std::back_inserter(message), "[{}] ", name);
		}

		fmt::vformat_to(std::back_inserter(message), format, arguments);

		if (suppressed > 0) {
			fmt::format_to(std::back_inserter(message), " (+{} suppressed)", suppressed);
		}

		// **Not on `Info`, whatever the switch says.** `Info` is the level a
		// person reads as prose and the level `just stress` scrapes out of a
		// server's output; a `file:line` suffix on it would break a reader and
		// a script at once. Every other level is a diagnostic, and a diagnostic
		// that does not say where it came from costs somebody a grep.
		if (level != LogLevel::Info && Load(&Categories().SourceLocation) != 0) {
			fmt::format_to(std::back_inserter(message), "  ({}:{})", FileOf(site.File), site.Line);
		}

		Logger().log(Severity(level), spdlog::string_view_t(message.data(), message.size()));
	}

	void Log::WriteUnfiltered(const LogSite &site, fmt::string_view format, fmt::format_args arguments) {
		fmt::memory_buffer message;
		fmt::vformat_to(std::back_inserter(message), format, arguments);
		fmt::format_to(
			std::back_inserter(message),
			"  ({}:{} in {})",
			FileOf(site.File),
			site.Line,
			site.Function == nullptr ? "?" : site.Function
		);

		// `critical` rather than `err`, so that a sink somebody filtered to
		// errors and above still shows an invariant that failed.
		Logger().log(spdlog::level::critical, spdlog::string_view_t(message.data(), message.size()));
	}

	void Log::Flush() {
		Logger().flush();
	}

	spdlog::logger &Log::Logger() {
		auto &logger = Instance();
		if (!logger) {
			// A log line before Initialise is a bug, but losing it is worse
			// than the bug. Fall back rather than crash.
			Log::Initialise("engine");
		}
		return *Instance();
	}
}
