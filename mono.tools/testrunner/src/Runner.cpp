#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <testrunner/Process.hpp>
#include <testrunner/Runner.hpp>
#include <testrunner/Sha256.hpp>

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

	std::vector<Suite> ReadSuites(const fs::path &binary) {
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

			// id <TAB> source <TAB> comma-separated dependencies
			const auto fields = Split(line, '\t');
			if (fields.size() < 2 || fields[0].empty()) {
				continue;
			}

			Suite suite;
			suite.Id = fields[0];
			suite.Source = Normalise(fields[1]);
			suite.Binary = binary;

			if (fields.size() > 2 && !fields[2].empty()) {
				for (auto &dependency : Split(fields[2], ',')) {
					if (!dependency.empty()) {
						suite.Depends.push_back(dependency);
					}
				}
			}

			suites.push_back(std::move(suite));
		}

		return suites;
	}

	std::string CatchFilter(const Suite &suite, bool gpuTests) {
		std::string filter = "[#" + suite.Source.stem().string() + "]";
		if (!gpuTests) {
			filter += "~[gpu]";
		}
		return filter;
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
			current.push_back(Normalise(build / line.substr(start)));
		}
		flush();

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
