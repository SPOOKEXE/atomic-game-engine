#include "Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <testrunner/Process.hpp>
#include <testrunner/Report.hpp>
#include <testrunner/Runner.hpp>
#include <thread>
#include <utility>

namespace testrunner {

	namespace fs = std::filesystem;

	namespace {

		std::vector<std::string> Split(std::string_view text, char separator) {
			std::vector<std::string> parts;
			size_t start = 0;
			for (;;) {
				const size_t found = text.find(separator, start);
				if (found == std::string_view::npos) {
					parts.emplace_back(text.substr(start));
					return parts;
				}
				parts.emplace_back(text.substr(start, found - start));
				start = found + 1;
			}
		}

		bool IsExecutable(const fs::path &path) {
			std::error_code error;
			if (!fs::is_regular_file(path, error)) {
				return false;
			}
			const auto permissions = fs::status(path, error).permissions();
			if (error) {
				return false;
			}
			return (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
				   fs::perms::none;
		}

		fs::path Normalise(const fs::path &path) {
			std::error_code error;
			auto canonical = fs::weakly_canonical(path, error);
			return error ? path : canonical;
		}

		// The module a suite belongs to, or empty when it does not sit in one.
		//
		// Every library in this repository is `<area>/{src,include,tests}` -
		// `mono_add_library` requires it - so the directory holding the `tests`
		// folder a suite's source is under is the module it tests.
		fs::path ModuleOf(const fs::path &source) {
			for (fs::path directory = source.parent_path(); directory.has_relative_path();
				 directory = directory.parent_path()) {
				if (directory.filename() == "tests") {
					return directory.parent_path();
				}
			}
			return {};
		}

		// Every first-party source and header under a module.
		//
		// **This is the half a header closure cannot see, and without it the
		// cascade was blind to the thing it is asked about most.** A suite is
		// signed over its own translation unit's `.d` closure - the test file
		// and the headers it includes - and a module's `src/*.cpp` is in no
		// test's closure, because a test includes the header and links the
		// object. So editing an implementation and nothing else moved no
		// signature at all: `just test` re-ran nothing, and the one thing it is
		// relied on for is noticing that an implementation changed.
		//
		// Directory contents rather than link inputs, and that is a deliberate
		// approximation. `ninja -t inputs` would give the exact set, at a
		// process per binary and a walk of every vendor source in the graph;
		// this is a `readdir` of two directories, and the cost of it being
		// coarse is a suite that occasionally re-runs when a sibling file it
		// does not use changed. Over-running is the safe direction - the whole
		// point of the tool is that under-running is silent.
		//
		// Cross-module reach is `TEST_DEPENDS`, and it works now for the same
		// reason: a dependency's signature covers its own module's sources, so
		// it moves when they do and carries every suite above it.
		std::vector<fs::path> ModuleSources(const fs::path &module) {
			std::vector<fs::path> sources;
			if (module.empty()) {
				return sources;
			}

			for (const char *area : {"src", "include"}) {
				std::error_code error;
				const fs::path root = module / area;
				if (!fs::is_directory(root, error)) {
					continue;
				}

				for (fs::recursive_directory_iterator entry(root, error), end; entry != end;
					 entry.increment(error)) {
					if (error) {
						break;
					}
					if (!entry->is_regular_file(error)) {
						continue;
					}

					const auto extension = entry->path().extension();
					if (extension == ".cpp" || extension == ".hpp" || extension == ".h" ||
						extension == ".c" || extension == ".inl") {
						sources.push_back(Normalise(entry->path()));
					}
				}
			}

			return sources;
		}

		std::string HashFile(const fs::path &path, std::map<fs::path, std::string> &cache) {
			const auto existing = cache.find(path);
			if (existing != cache.end()) {
				return existing->second;
			}

			Sha256 hash;
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				// A generated header that is not there any more. Recording its
				// absence is itself a fact worth hashing: the signature changes
				// again when the file comes back.
				hash.Update("<missing>");
			} else {
				char buffer[64 * 1024];
				while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
					hash.Update(buffer, static_cast<size_t>(file.gcount()));
				}
			}

			auto digest = hash.Hex();
			cache.emplace(path, digest);
			return digest;
		}
	}

	std::vector<fs::path> FindTestBinaries(const fs::path &build) {
		return FindBinaries(build, "tests");
	}

	std::vector<fs::path> FindBinaries(const fs::path &build, const std::string &subdirectory) {
		std::vector<fs::path> binaries;

		const fs::path directory = build / subdirectory;
		std::error_code error;
		if (!fs::is_directory(directory, error)) {
			return binaries;
		}

		for (const auto &entry : fs::directory_iterator(directory, error)) {
			if (IsExecutable(entry.path())) {
				binaries.push_back(entry.path());
			}
		}

		std::sort(binaries.begin(), binaries.end());
		return binaries;
	}

	std::vector<Suite> ReadSuites(const fs::path &binary, const fs::path &build) {
		std::vector<Suite> suites;

		const auto listed = Run({binary.string(), "--mono-suites"});
		if (!listed.Started || listed.ExitCode != 0) {
			return suites;
		}

		std::istringstream lines(listed.Output);
		std::string line;
		while (std::getline(lines, line)) {
			if (line.empty()) {
				continue;
			}

			// id <TAB> source <TAB> comma-separated dependencies <TAB> tier <TAB> scenario group
			const auto fields = Split(line, '\t');
			if (fields.size() < 2 || fields[0].empty()) {
				continue;
			}

			Suite suite;
			suite.Id = fields[0];
			// Against the build directory, which is the base both this path
			// and Ninja's dependency paths are relative to. Resolving against
			// the runner's working directory escapes the tree - every suite
			// then misses its closure and signs over its source alone.
			suite.Source = Normalise(build / fields[1]);
			suite.Binary = binary;

			if (fields.size() > 2 && !fields[2].empty()) {
				for (auto &dependency : Split(fields[2], ',')) {
					if (!dependency.empty()) {
						suite.Depends.push_back(dependency);
					}
				}
			}

			if (fields.size() > 3) {
				suite.Tier = fields[3];
			}

			if (fields.size() > 4) suite.ScenarioGroup = fields[4];
			suites.push_back(std::move(suite));
		}

		return suites;
	}

	std::vector<Suite> ReadSuites(const std::vector<fs::path> &binaries, const fs::path &build, size_t jobs) {
		if (binaries.empty()) return {};

		std::vector<std::vector<Suite>> registries(binaries.size());
		std::vector<std::exception_ptr> failures(binaries.size());
		std::atomic<size_t> next{0};
		const auto read = [&] {
			for (size_t index = next++; index < binaries.size(); index = next++) {
				try {
					registries[index] = ReadSuites(binaries[index], build);
				} catch (...) {
					// Return filesystem/allocation failures to the caller after joining.
					failures[index] = std::current_exception();
				}
			}
		};

		const size_t width = std::min(std::max(jobs, size_t{1}), binaries.size());
		if (width == 1) {
			read();
		} else {
			// Tracy's startup calibration makes serial discovery expensive. Overlap
			// binary discovery without changing profiling or suite isolation.
			std::vector<std::thread> workers;
			workers.reserve(width);
			struct JoinOnExit {
				std::vector<std::thread> &Workers;
				~JoinOnExit() {
					for (auto &worker : Workers) {
						if (worker.joinable()) worker.join();
					}
				}
			} join{workers};
			for (size_t slot = 0; slot < width; slot++)
				workers.emplace_back(read);
		}

		std::vector<Suite> suites;
		for (size_t index = 0; index < binaries.size(); index++) {
			if (failures[index]) std::rethrow_exception(failures[index]);
			auto &registry = registries[index];
			suites.insert(
				suites.end(),
				std::make_move_iterator(registry.begin()),
				std::make_move_iterator(registry.end())
			);
		}
		return suites;
	}

	std::string CatchFilter(const Suite &suite, bool gpuTests, SlowCases slow) {
		const std::string filter = "[#" + suite.Source.stem().string() + "]";
		// Appended to every specification, since a union only narrows per term.
		const std::string slowTerm = slow == SlowCases::Skip   ? "~[slow]"
									 : slow == SlowCases::Only ? "[slow]"
															   : "";
		if (!gpuTests) return filter + "~[gpu]~[.]" + slowTerm;

		// A GPU run explicitly selects hidden GPU cases while leaving process
		// helpers hidden. Catch combines comma-separated specifications as a union.
		return filter + "~[.]" + slowTerm + "," + filter + "[gpu]" + slowTerm;
	}

	ReportScratchDirectory::ReportScratchDirectory(const fs::path &build) {
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 1024; attempt++) {
			const fs::path candidate =
				build / ("testrunner-report-" + std::to_string(nonce) + "-" + std::to_string(attempt));
			std::error_code error;
			if (fs::create_directory(candidate, error)) {
				Directory = candidate;
				return;
			}
			if (error && error != std::errc::file_exists) {
				FailureMessage = error.message();
				return;
			}
		}
		FailureMessage = "could not reserve a unique report directory";
	}

	ReportScratchDirectory::~ReportScratchDirectory() {
		if (!Directory.empty()) {
			std::error_code error;
			fs::remove_all(Directory, error);
		}
	}

	bool RunSuite(
		const Suite &suite,
		const fs::path &scratch,
		bool gpuTests,
		SlowCases slow,
		std::string &output,
		SuiteReport &report
	) {
		std::error_code error;
		fs::remove(scratch, error);
		if (error) {
			output = "could not clear suite report " + scratch.string() + ": " + error.message();
			return false;
		}

		// Keep console output for failures and write counts to the Mono report.
		// Process startup and static initialisation belong in the suite duration.
		const auto started = std::chrono::steady_clock::now();
		const auto result = Run(
			{suite.Binary.string(),
			 // A source file can contain only GPU cases, so an empty ordinary
			 // selection is a successful run with a zero-case Mono report.
			 "--allow-running-no-tests",
			 "-#",
			 CatchFilter(suite, gpuTests, slow),
			 "--reporter",
			 "console::out=-",
			 "--reporter",
			 "mono::out=" + scratch.string()}
		);
		report.Microseconds = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
				.count()
		);

		output = result.Output;
		if (!result.Started) {
			output = "could not start " + suite.Binary.string();
			return false;
		}

		SuiteReport parsed;
		if (ReadSuiteReport(scratch, parsed)) {
			report.FormatVersion = parsed.FormatVersion;
			report.FailureExitCode = parsed.FailureExitCode;
			report.ExpectedCases = parsed.ExpectedCases;
			report.CasesPassed = parsed.CasesPassed;
			report.CasesFailed = parsed.CasesFailed;
			report.CasesSkipped = parsed.CasesSkipped;
			report.AssertionsPassed = parsed.AssertionsPassed;
			report.AssertionsFailed = parsed.AssertionsFailed;
			report.Cases = std::move(parsed.Cases);
		} else if (result.ExitCode == 0) {
			if (!output.empty() && output.back() != '\n') output += '\n';
			output += "test binary exited successfully without a complete Mono report: " + scratch.string();
			return false;
		}
		return result.ExitCode == 0 && report.CasesFailed == 0 && report.AssertionsFailed == 0;
	}

	std::vector<std::vector<const Suite *>> PlanSuiteGroups(const std::vector<const Suite *> &selected) {
		std::vector<std::vector<const Suite *>> groups;
		for (const Suite *suite : selected) {
			auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &candidate) {
				const Suite &first = *candidate.front();
				return !suite->ScenarioGroup.empty() && first.ScenarioGroup == suite->ScenarioGroup &&
					   first.Binary == suite->Binary && first.Tier == suite->Tier;
			});
			if (group == groups.end())
				groups.push_back({suite});
			else
				group->push_back(suite);
		}
		return groups;
	}

	std::vector<SuiteReport> RunSuiteGroup(
		const std::vector<const Suite *> &suites,
		const fs::path &scratch,
		bool gpuTests,
		SlowCases slow,
		std::string &output
	) {
		std::vector<SuiteReport> reports(suites.size());
		for (size_t index = 0; index < suites.size(); index++) {
			reports[index].Id = suites[index]->Id;
			reports[index].Ran = true;
		}
		if (suites.empty()) return reports;
		if (suites.size() == 1) {
			reports[0].Passed = RunSuite(*suites[0], scratch, gpuTests, slow, output, reports[0]);
			return reports;
		}
		const auto reject = [&](const std::string &reason) {
			if (!output.empty() && output.back() != '\n') output += '\n';
			output += "invalid shared scenario report: " + reason;
			for (auto &report : reports)
				report.Passed = false;
		};
		if (PlanSuiteGroups(suites).size() != 1) {
			reject("incompatible suites");
			return reports;
		}
		std::map<fs::path, size_t> owners;
		std::set<std::string> stems;
		std::string filter;
		for (size_t index = 0; index < suites.size(); index++) {
			if (!owners.emplace(Normalise(suites[index]->Source), index).second ||
				!stems.insert(suites[index]->Source.stem().string()).second) {
				reject("ambiguous source ownership");
				return reports;
			}
			if (!filter.empty()) filter += ',';
			filter += CatchFilter(*suites[index], gpuTests, slow);
		}
		std::error_code error;
		fs::remove(scratch, error);
		if (error) {
			reject("could not clear " + scratch.string() + ": " + error.message());
			return reports;
		}
		const auto started = std::chrono::steady_clock::now();
		const auto result = Run(
			{suites.front()->Binary.string(),
			 "--allow-running-no-tests",
			 "-#",
			 filter,
			 "--reporter",
			 "console::out=-",
			 "--reporter",
			 "mono::out=" + scratch.string()}
		);
		const auto elapsed = static_cast<unsigned long long>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started)
				.count()
		);
		output = result.Output;
		// Divide only shared process overhead; each case keeps its measured cost.
		const auto apportion = [&](unsigned long long caseTime) {
			const auto overhead = elapsed > caseTime ? elapsed - caseTime : 0;
			for (size_t index = 0; index < reports.size(); index++)
				reports[index].Microseconds = reports[index].CaseMicroseconds() + overhead / reports.size() +
											  (index < overhead % reports.size() ? 1 : 0);
		};
		apportion(0);
		SuiteReport parsed;
		if (!result.Started || !ReadSuiteReport(scratch, parsed) || parsed.FormatVersion != 3) {
			reject("child did not produce a complete Mono report");
			return reports;
		}
		std::map<fs::path, unsigned> expectedSources;
		for (const auto &[source, expected] : parsed.ExpectedCases) {
			if (!expectedSources.emplace(Normalise(source), expected).second ||
				(expected != 0 && !owners.contains(Normalise(source)))) {
				reject("ambiguous or unselected manifest source: " + source.string());
				return reports;
			}
		}
		for (const auto &owner : owners) {
			const auto &source = owner.first;
			if (!expectedSources.contains(source)) {
				reject("selected source missing from workload manifest: " + source.string());
				return reports;
			}
		}

		for (const auto &entry : parsed.Cases) {
			const auto owner = owners.find(Normalise(entry.Source));
			if (entry.Source.empty() || owner == owners.end()) {
				reject("unknown or missing case source: " + entry.Source.string());
				return reports;
			}
			auto &report = reports[owner->second];
			report.Cases.push_back(entry);
			report.CasesPassed += !entry.Failed && !entry.Skipped;
			report.CasesFailed += entry.Failed;
			report.CasesSkipped += entry.Skipped;
			report.AssertionsPassed += entry.AssertionsPassed;
			report.AssertionsFailed += entry.AssertionsFailed;
		}
		apportion(parsed.CaseMicroseconds());
		// A non-test process failure must never make a sibling look green.
		const int expectedExit =
			parsed.AssertionsFailed > 0 || parsed.CasesFailed > 0 ? parsed.FailureExitCode : 0;
		if (result.ExitCode != expectedExit) {
			reject("child exit does not match its reported case failures");
			return reports;
		}
		for (auto &report : reports)
			report.Passed = report.CasesFailed == 0 && report.AssertionsFailed == 0;
		return reports;
	}

	DependencyClosures ReadDependencyClosures(const fs::path &build) {
		DependencyClosures closures;

		std::error_code error;
		if (!fs::exists(build / ".ninja_deps", error)) {
			return closures;
		}

		const auto dumped = Run({"ninja", "-C", build.string(), "-t", "deps"});
		if (!dumped.Started) {
			return closures;
		}

		// Entries look like:
		//   path/to.o: #deps 42, deps mtime 123 (VALID)
		//       /abs/path/Source.cpp
		//       /abs/path/Header.hpp
		std::istringstream lines(dumped.Output);
		std::string line;
		std::vector<fs::path> current;
		std::map<fs::path, fs::path> normalisedPaths;
		bool collecting = false;

		auto flush = [&] {
			if (!collecting || current.empty()) {
				return;
			}
			// The first entry of a translation unit's list is its own source,
			// which is the key a suite knows itself by.
			closures[current.front()] = current;
			current.clear();
		};

		for (; std::getline(lines, line);) {
			if (!line.empty() && line[0] != ' ') {
				flush();
				// An entry marked stale is data from before the last edit.
				// Using it would be worse than having none.
				collecting = line.find("(VALID)") != std::string::npos;
				continue;
			}

			if (!collecting) {
				continue;
			}

			const size_t start = line.find_first_not_of(" \t");
			if (start == std::string::npos) {
				flush();
				collecting = false;
				continue;
			}

			// Ninja prints paths relative to the build directory.
			fs::path rawPath = build / line.substr(start);
			const auto cached = normalisedPaths.find(rawPath);
			if (cached != normalisedPaths.end()) {
				current.push_back(cached->second);
			} else {
				fs::path normalisedPath = Normalise(rawPath);
				normalisedPaths.emplace(std::move(rawPath), normalisedPath);
				current.push_back(std::move(normalisedPath));
			}
		}
		flush();

		// Ask the build graph for harness inputs rather than keeping a file list.
		// Binary intersections keep a benchmark's registry independent of Catch's
		// test main and keep unrelated linked production sources out of signatures.
		const auto inputsFor = [&](const std::string &target) {
			std::set<fs::path> inputs;
			const auto result = Run({"ninja", "-C", build.string(), "-t", "inputs", target});
			if (!result.Started || result.ExitCode != 0) return inputs;
			std::istringstream paths(result.Output);
			std::string path;
			while (std::getline(paths, path))
				if (!path.empty()) inputs.insert(Normalise(build / path));
			return inputs;
		};
		std::set<fs::path> harnessSources;
		for (const auto *target : {"mono_testmain", "mono_suites"}) {
			const auto inputs = inputsFor(target);
			harnessSources.insert(inputs.begin(), inputs.end());
		}
		const auto configureInputs = inputsFor("build.ninja");
		auto binaries = FindBinaries(build, "tests");
		const auto benchmarks = FindBinaries(build, "bench");
		binaries.insert(binaries.end(), benchmarks.begin(), benchmarks.end());
		for (const auto &binary : binaries) {
			const auto binaryInputs = inputsFor(fs::relative(binary, build).generic_string());
			std::set<fs::path> common;
			for (const auto &source : harnessSources) {
				if (!binaryInputs.contains(source)) continue;
				const auto closure = closures.find(source);
				if (closure == closures.end()) continue;
				common.insert(closure->second.begin(), closure->second.end());
				for (const auto &configureInput : configureInputs)
					if (configureInput.parent_path() == source.parent_path()) common.insert(configureInput);
			}
			if (!common.empty()) closures[Normalise(binary)] = {common.begin(), common.end()};
		}

		return closures;
	}

	std::map<std::string, std::string> ComputeSignatures(
		const std::vector<Suite> &suites,
		const DependencyClosures &closures,
		std::vector<std::string> &warnings
	) {
		std::map<std::string, const Suite *> byId;
		for (const auto &suite : suites) {
			byId[suite.Id] = &suite;
		}

		std::map<fs::path, std::string> fileHashes;
		std::map<std::string, std::string> signatures;
		std::set<std::string> visiting;

		// One walk per module rather than per suite: `render` alone has five
		// suites, and they all sign over the same directory.
		std::map<fs::path, std::vector<fs::path>> moduleSources;

		// Recursive rather than a topological sort: the graph is tens of nodes
		// and hand-declared, so depth is bounded by how many layers a person
		// wrote down.
		auto signatureOf = [&](const std::string &id, auto &&self) -> std::string {
			const auto known = signatures.find(id);
			if (known != signatures.end()) {
				return known->second;
			}

			const auto found = byId.find(id);
			if (found == byId.end()) {
				warnings.push_back("unknown TEST_DEPENDS target '" + id + "'");
				// Stable, so an absent dependency does not make the signature
				// change every run.
				return Sha256::Of("absent:" + id);
			}

			if (!visiting.insert(id).second) {
				warnings.push_back("dependency cycle through '" + id + "'");
				return Sha256::Of("cycle:" + id);
			}

			const Suite &suite = *found->second;

			Sha256 hash;
			hash.Update(id);
			hash.Update("\nscenario-group:");
			hash.Update(suite.ScenarioGroup);
			hash.Update("\ntier:");
			hash.Update(suite.Tier);
			hash.Update("\nbinary:");
			hash.Update(Normalise(suite.Binary).string());

			const auto harness = closures.find(Normalise(suite.Binary));
			std::vector<std::string> harnessDigests;
			if (harness != closures.end())
				for (const auto &file : harness->second)
					harnessDigests.push_back(HashFile(file, fileHashes));
			if (!suite.Binary.empty() && harness == closures.end())
				warnings.push_back("no derived common harness inputs for '" + id + "'");
			std::sort(harnessDigests.begin(), harnessDigests.end());
			for (const auto &digest : harnessDigests)
				hash.Update(digest);

			const auto closure = closures.find(suite.Source);
			std::vector<std::string> digests;
			if (closure == closures.end()) {
				// No dependency data for this translation unit - a fresh build
				// tree, or a generator that is not Ninja. Falling back to the
				// source alone under-covers, so it is reported.
				warnings.push_back("no header closure for '" + id + "'; hashing its source only");
				digests.push_back(HashFile(suite.Source, fileHashes));
			} else {
				digests.reserve(closure->second.size());
				for (const auto &file : closure->second) {
					digests.push_back(HashFile(file, fileHashes));
				}
			}

			// And the module this suite is testing, which the closure above
			// cannot reach. See `ModuleSources` for why that gap was the whole
			// cascade going quiet on an implementation-only change.
			const fs::path module = ModuleOf(suite.Source);
			auto walked = moduleSources.find(module);
			if (walked == moduleSources.end()) {
				walked = moduleSources.emplace(module, ModuleSources(module)).first;
			}
			for (const auto &file : walked->second) {
				digests.push_back(HashFile(file, fileHashes));
			}

			// Sorted, so the digest does not depend on the order the compiler
			// happened to report includes in.
			std::sort(digests.begin(), digests.end());
			for (const auto &digest : digests) {
				hash.Update(digest);
			}

			auto dependencies = suite.Depends;
			std::sort(dependencies.begin(), dependencies.end());
			for (const auto &dependency : dependencies) {
				hash.Update(self(dependency, self));
			}

			visiting.erase(id);
			auto digest = hash.Hex();
			signatures[id] = digest;
			return digest;
		};

		for (const auto &suite : suites) {
			signatureOf(suite.Id, signatureOf);
		}

		return signatures;
	}

	// -----------------------------------------------------------------------
	// The cache
	// -----------------------------------------------------------------------

	namespace {
		// v2 added the per-suite counts and duration the report is built from.
		// The bump costs one full re-run on the way past, which is the correct
		// price: a v1 line carries neither, and inventing zeroes for it would
		// put a suite in test-output.md claiming to hold no tests and cost no
		// time.
		// v3: signatures now cover the module's own sources as well as the
		// suite's header closure, so a v2 number means something different and
		// reading it would skip suites on the strength of the old meaning.
		constexpr const char *CACHE_HEADER = "# atomic smart-tests cache v3";

		unsigned long long Count(const std::string &text) {
			unsigned long long value = 0;
			for (const char character : text) {
				if (character < '0' || character > '9') {
					return 0;
				}
				value = value * 10 + static_cast<unsigned long long>(character - '0');
			}
			return value;
		}
	}

	std::map<std::string, CacheEntry> LoadCache(const fs::path &path) {
		std::map<std::string, CacheEntry> cache;

		std::ifstream file(path);
		if (!file) {
			return cache;
		}

		std::string line;
		bool versionSeen = false;
		while (std::getline(file, line)) {
			if (line.empty()) {
				continue;
			}
			if (line[0] == '#') {
				// A cache written by a different version was computed
				// differently. Misreading it would skip suites on the strength
				// of signatures that do not mean the same thing.
				versionSeen = versionSeen || line == CACHE_HEADER;
				continue;
			}
			if (!versionSeen) {
				return {};
			}

			const auto fields = Split(line, '\t');
			if (fields.size() < 3) {
				continue;
			}

			CacheEntry entry;
			entry.Signature = fields[2];
			entry.Passed = fields[1] == "pass";

			// A line somebody hand-edited to force a re-run may well have lost
			// its tail. The counts are cosmetic, so a short line is read for
			// what it has rather than discarded.
			if (fields.size() >= 9) {
				entry.CasesPassed = static_cast<unsigned>(Count(fields[3]));
				entry.CasesFailed = static_cast<unsigned>(Count(fields[4]));
				entry.CasesSkipped = static_cast<unsigned>(Count(fields[5]));
				entry.AssertionsPassed = static_cast<unsigned>(Count(fields[6]));
				entry.AssertionsFailed = static_cast<unsigned>(Count(fields[7]));
				entry.Microseconds = Count(fields[8]);
			}

			cache[fields[0]] = std::move(entry);
		}

		return versionSeen ? cache : std::map<std::string, CacheEntry>{};
	}

	bool SaveCache(const fs::path &path, const std::map<std::string, CacheEntry> &cache) {
		std::error_code error;
		if (path.has_parent_path()) {
			fs::create_directories(path.parent_path(), error);
		}

		std::ofstream file(path, std::ios::trunc);
		if (!file) {
			return false;
		}

		file << CACHE_HEADER << '\n';
		file << "# suite\tstatus\tsignature\tcases pass\tfail\tskip\tassertions pass\tfail"
				"\tmicroseconds\n";
		// std::map iterates sorted, so the file is stable between runs and a
		// diff of it shows only what actually changed.
		for (const auto &[id, entry] : cache) {
			file << id << '\t' << (entry.Passed ? "pass" : "fail") << '\t' << entry.Signature << '\t'
				 << entry.CasesPassed << '\t' << entry.CasesFailed << '\t' << entry.CasesSkipped << '\t'
				 << entry.AssertionsPassed << '\t' << entry.AssertionsFailed << '\t' << entry.Microseconds
				 << '\n';
		}

		return file.good();
	}
}
