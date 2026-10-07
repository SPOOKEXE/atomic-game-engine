#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace benchrunner {

	using DemoJson = nlohmann::json;

	struct DemoStatistics {
		double Mean = 0;
		double Deviation = 0;
		double Minimum = 0;
		double Maximum = 0;
	};

	struct DemoWorkload {
		std::string Status;
		std::string Reason;
		DemoJson Basis = DemoJson::object();
		std::map<std::string, DemoStatistics> Metrics;
	};

	struct DemoCollection {
		DemoJson Manifest;
		std::map<std::string, DemoWorkload> Workloads;
	};

	struct DemoLimits {
		double Percent = 15;
		double Bytes = 1048576;
		double BytesPerFrame = 1024;
		double BytesPerSecond = 65536;
		double Milliseconds = 0.1;
		double MillisecondsPerSecond = 5;
		double Allocations = 100;
		double AllocationsPerSecond = 1000;
		bool Advisory = false;
	};

	inline DemoStatistics DemoAverage(const std::vector<double> &samples) {
		if (samples.empty()) throw std::runtime_error("empty demo sample set");
		DemoStatistics statistics;
		statistics.Minimum = *std::min_element(samples.begin(), samples.end());
		statistics.Maximum = *std::max_element(samples.begin(), samples.end());
		for (double sample : samples)
			statistics.Mean += sample / static_cast<double>(samples.size());
		for (double sample : samples) {
			const double difference = sample - statistics.Mean;
			statistics.Deviation += difference * difference / static_cast<double>(samples.size());
		}
		statistics.Deviation = std::sqrt(statistics.Deviation);
		return statistics;
	}

	inline DemoJson DemoReadJson(const std::filesystem::path &path) {
		std::ifstream input(path);
		if (!input) throw std::runtime_error("cannot read " + path.string());
		return DemoJson::parse(input);
	}

	inline double DemoNumber(const DemoJson &object, std::string_view name) {
		const std::string key(name);
		if (!object.contains(key) || !object.at(key).is_number()) {
			throw std::runtime_error("missing numeric demo field: " + key);
		}
		const double number = object.at(key).get<double>();
		if (!std::isfinite(number) || number < 0) {
			throw std::runtime_error("invalid demo field: " + key);
		}
		return number;
	}

	inline std::string DemoCell(std::string text) {
		for (char &character : text) {
			if (character == '|' || character == '\n' || character == '\r') character = ' ';
		}
		return text;
	}

	inline bool DemoChurnMetric(std::string_view name) {
		return name == "cpu_allocated_bytes" || name == "cpu_allocations" || name == "gpu_allocated_bytes" ||
			   name == "gpu_resources_created" || name == "uploaded_bytes";
	}

	inline bool DemoDiagnostic(std::string_view name) {
		return name == "draw_calls_per_frame" || name == "submitted_frames" || name == "frame_count" ||
			   name == "duration_seconds" ||
			   (name.ends_with("_per_frame") && name.find("_ms_") == std::string_view::npos) ||
			   name.starts_with("framegraph_") || name == "tick_count" || name == "tick_rate_hz" ||
			   name == "tick_overruns" || name == "clients_admitted" ||
			   name.find("packets") != std::string_view::npos || DemoChurnMetric(name);
	}

	inline void DemoValidateManifest(const DemoJson &manifest) {
		if (manifest.at("schema") != 1 || !manifest.at("source_revision").is_string() ||
			manifest.at("source_revision").get<std::string>().empty() ||
			!manifest.at("source_dirty").is_boolean() || !manifest.at("machine").is_object() ||
			manifest.at("machine").empty() || !manifest.at("settings").is_object()) {
			throw std::runtime_error("invalid demo manifest metadata");
		}
		const DemoJson &settings = manifest.at("settings");
		if (settings.at("runs") != 3 || settings.at("seconds") != 5 || settings.at("preset") != "profile" ||
			DemoNumber(settings, "width") == 0 || DemoNumber(settings, "height") == 0) {
			throw std::runtime_error("demo collection requires three five-second profile runs");
		}
		if (!manifest.at("workloads").is_array() || manifest.at("workloads").empty()) {
			throw std::runtime_error("demo manifest contains no workloads");
		}
	}

	inline DemoCollection DemoLoad(const std::filesystem::path &directory) {
		DemoCollection collection;
		collection.Manifest = DemoReadJson(directory / "manifest.json");
		DemoValidateManifest(collection.Manifest);
		const DemoJson &manifest = collection.Manifest;
		for (const DemoJson &entry : manifest.at("workloads")) {
			const std::string name = entry.at("name").get<std::string>();
			if (name.empty() || collection.Workloads.contains(name)) {
				throw std::runtime_error("empty or duplicate demo workload: " + name);
			}
			DemoWorkload workload;
			workload.Status = entry.at("status").get<std::string>();
			if (workload.Status == "unavailable") {
				workload.Reason = entry.value("reason", "unavailable on this source revision");
				collection.Workloads.emplace(name, std::move(workload));
				continue;
			}
			if (workload.Status != "measured" || !entry.at("reports").is_array() ||
				entry.at("reports").size() != 3) {
				throw std::runtime_error("demo workload requires exactly three reports: " + name);
			}
			std::map<std::string, std::vector<double>> samples;
			std::set<std::string> reportNames;
			std::set<std::string> metricNames;
			for (const DemoJson &reportName : entry.at("reports")) {
				const std::filesystem::path relative = reportName.get<std::string>();
				if (relative.empty() || relative.is_absolute() ||
					!reportNames.insert(relative.generic_string()).second) {
					throw std::runtime_error("invalid or repeated demo report path: " + name);
				}
				for (const auto &part : relative) {
					if (part == "..") throw std::runtime_error("demo report escapes collection directory");
				}
				const DemoJson report = DemoReadJson(directory / relative);
				const double frames = DemoNumber(report, "frame_count");
				const double seconds = DemoNumber(report, "duration_seconds");
				const double dropped = DemoNumber(report, "dropped_spans");
				const bool hasOwnerDrops = report.contains("owner_dropped_spans");
				const bool hasOffThreadDrops = report.contains("off_thread_dropped_spans");
				if (hasOwnerDrops != hasOffThreadDrops)
					throw std::runtime_error("incomplete dropped-span provenance");
				const double ownerDropped =
					hasOwnerDrops ? DemoNumber(report, "owner_dropped_spans") : dropped;
				const double offThreadDropped =
					hasOffThreadDrops ? DemoNumber(report, "off_thread_dropped_spans") : 0;
				if (report.at("schema") != 1 || report.at("heap_compiled") != true || frames == 0 ||
					seconds < 5 || ownerDropped != 0 || ownerDropped + offThreadDropped != dropped ||
					DemoNumber(report, "heap_dropped_scopes") != 0 || !report.at("metrics").is_object()) {
					throw std::runtime_error("incomplete demo measurement: " + relative.string());
				}
				const std::filesystem::path rssPath = directory / (relative.string() + ".rss");
				const bool hasRss = std::filesystem::exists(rssPath);
				DemoJson basis = DemoJson::object();
				if (hasRss) basis["process_peak_rss_basis"] = "whole_process_lifetime";
				for (const char *field :
					 {"cpu_peak_basis", "gpu_peak_basis", "replication_timing_basis", "frame_basis"}) {
					if (report.contains(field)) basis[field] = report.at(field);
				}
				if (!reportNames.empty() && reportNames.size() > 1 && workload.Basis != basis) {
					throw std::runtime_error("inconsistent measurement basis between demo runs: " + name);
				}
				workload.Basis = std::move(basis);
				DemoJson metrics = report.at("metrics");
				if (entry.value("kind", "") == "server-replica" &&
					DemoNumber(metrics, "clients_admitted") < 1) {
					throw std::runtime_error("server replication measurement admitted no client: " + name);
				}
				if (entry.value("kind", "").starts_with("client") &&
					DemoNumber(metrics, "submitted_frames") < 1) {
					throw std::runtime_error("client demo submitted no GPU work: " + name);
				}
				metrics["framegraph_dropped_spans"] = dropped;
				metrics["framegraph_owner_dropped_spans"] = ownerDropped;
				metrics["framegraph_off_thread_dropped_spans"] = offThreadDropped;
				if (hasRss) {
					std::ifstream rssInput(rssPath);
					std::string rssText;
					std::string extra;
					if (!(rssInput >> rssText) || rssInput >> extra || rssText.empty() ||
						rssText.find_first_not_of("0123456789") != std::string::npos) {
						throw std::runtime_error("invalid process RSS measurement: " + rssPath.string());
					}
					size_t consumed = 0;
					const double rssKibibytes = std::stod(rssText, &consumed);
					if (consumed != rssText.size() || !std::isfinite(rssKibibytes) || rssKibibytes <= 0 ||
						!std::isfinite(rssKibibytes * 1024))
						throw std::runtime_error("invalid process RSS value");
					metrics["process_peak_rss_bytes"] = rssKibibytes * 1024;
				}
				for (const std::string_view required :
					 {"cpu_live_bytes",
					  "cpu_peak_bytes",
					  "cpu_allocated_bytes",
					  "cpu_allocations",
					  "render_ms_per_frame",
					  "physics_ms_per_frame",
					  "replication_ms_per_frame",
					  "script_ms_per_frame",
					  "imagegraph_ms_per_frame",
					  "frame_ms_per_frame"}) {
					(void)DemoNumber(metrics, required);
				}
				std::set<std::string> currentNames;
				for (const auto &[key, value] : metrics.items()) {
					(void)value;
					currentNames.insert(key);
					const double metric = DemoNumber(metrics, key);
					samples[key].push_back(metric);
					if (DemoChurnMetric(key) && !metrics.contains(key + "_per_frame")) {
						samples[key + "_per_frame"].push_back(metric / frames);
					}
				}
				for (const auto &[key, value] : metrics.items()) {
					(void)value;
					if (DemoChurnMetric(key) && !metrics.contains(key + "_per_second"))
						samples[key + "_per_second"].push_back(DemoNumber(metrics, key) / seconds);
					if (key.ends_with("_ms_per_frame")) {
						const std::string perSecond = key.substr(0, key.size() - 10) + "_per_second";
						if (!metrics.contains(perSecond))
							samples[perSecond].push_back(DemoNumber(metrics, key) * frames / seconds);
					}
				}
				if (!metricNames.empty() && metricNames != currentNames) {
					throw std::runtime_error("inconsistent metric keys between demo runs: " + name);
				}
				metricNames = std::move(currentNames);
				samples["frame_count"].push_back(frames);
				samples["duration_seconds"].push_back(seconds);
			}
			for (const auto &[metric, values] : samples)
				workload.Metrics.emplace(metric, DemoAverage(values));
			collection.Workloads.emplace(name, std::move(workload));
		}
		return collection;
	}

	inline DemoJson DemoAggregateJson(const DemoCollection &collection) {
		DemoValidateManifest(collection.Manifest);
		double maximumOffThreadDrops = 0;
		for (const auto &[name, workload] : collection.Workloads) {
			(void)name;
			if (workload.Status == "measured")
				maximumOffThreadDrops = std::max(
					maximumOffThreadDrops, workload.Metrics.at("framegraph_off_thread_dropped_spans").Maximum
				);
		}
		DemoJson aggregate = {
			{"schema", 1},
			{"heap_compiled", true},
			{"dropped_spans", maximumOffThreadDrops},
			{"owner_dropped_spans", 0},
			{"off_thread_dropped_spans", maximumOffThreadDrops},
			{"heap_dropped_scopes", 0},
			{"manifest", collection.Manifest},
			{"workloads", DemoJson::object()}
		};
		for (const auto &[name, workload] : collection.Workloads) {
			DemoJson row = {
				{"status", workload.Status},
				{"reason", workload.Reason},
				{"basis", workload.Basis},
				{"metrics", DemoJson::object()}
			};
			for (const auto &[metric, statistics] : workload.Metrics) {
				row["metrics"][metric] = {
					{"mean", statistics.Mean},
					{"stddev", statistics.Deviation},
					{"min", statistics.Minimum},
					{"max", statistics.Maximum}
				};
			}
			aggregate["workloads"][name] = std::move(row);
		}
		return aggregate;
	}

	inline DemoCollection DemoLoadAggregate(const DemoJson &aggregate) {
		if (aggregate.at("schema") != 1 || aggregate.at("heap_compiled") != true ||
			DemoNumber(aggregate, "owner_dropped_spans") != 0 ||
			DemoNumber(aggregate, "dropped_spans") != DemoNumber(aggregate, "off_thread_dropped_spans") ||
			DemoNumber(aggregate, "heap_dropped_scopes") != 0 || !aggregate.at("workloads").is_object()) {
			throw std::runtime_error("invalid embedded baseline schema");
		}
		DemoCollection collection;
		collection.Manifest = aggregate.at("manifest");
		DemoValidateManifest(collection.Manifest);
		std::set<std::string> manifestNames;
		for (const DemoJson &entry : collection.Manifest.at("workloads")) {
			const std::string name = entry.at("name").get<std::string>();
			if (name.empty() || !manifestNames.insert(name).second)
				throw std::runtime_error("duplicate embedded workload");
			const DemoJson &row = aggregate.at("workloads").at(name);
			DemoWorkload workload;
			workload.Status = row.at("status").get<std::string>();
			workload.Reason = row.value("reason", "");
			workload.Basis = row.at("basis");
			if (!workload.Basis.is_object() || workload.Status != entry.at("status").get<std::string>()) {
				throw std::runtime_error("inconsistent embedded workload metadata");
			}
			if (workload.Status != "measured" && workload.Status != "unavailable")
				throw std::runtime_error("invalid embedded workload status");
			if (!row.at("metrics").is_object()) throw std::runtime_error("invalid embedded metrics");
			for (const auto &[metric, statistics] : row.at("metrics").items()) {
				DemoStatistics parsed{
					DemoNumber(statistics, "mean"),
					DemoNumber(statistics, "stddev"),
					DemoNumber(statistics, "min"),
					DemoNumber(statistics, "max")
				};
				if (parsed.Minimum > parsed.Mean || parsed.Mean > parsed.Maximum)
					throw std::runtime_error("invalid embedded metric range");
				workload.Metrics.emplace(metric, parsed);
			}
			if (workload.Metrics.contains("process_peak_rss_bytes") !=
					workload.Basis.contains("process_peak_rss_basis") ||
				(workload.Basis.contains("process_peak_rss_basis") &&
				 workload.Basis.at("process_peak_rss_basis") != "whole_process_lifetime")) {
				throw std::runtime_error("invalid embedded process RSS basis");
			}
			if (workload.Status == "measured") {
				if (!entry.at("reports").is_array() || entry.at("reports").size() != 3)
					throw std::runtime_error("embedded workload requires three run references");
				for (const char *required :
					 {"cpu_live_bytes",
					  "cpu_peak_bytes",
					  "cpu_allocated_bytes",
					  "cpu_allocations",
					  "render_ms_per_frame",
					  "physics_ms_per_frame",
					  "replication_ms_per_frame",
					  "script_ms_per_frame",
					  "imagegraph_ms_per_frame",
					  "frame_ms_per_frame",
					  "cpu_allocations_per_frame",
					  "cpu_allocated_bytes_per_frame",
					  "frame_count",
					  "duration_seconds",
					  "framegraph_owner_dropped_spans",
					  "framegraph_off_thread_dropped_spans",
					  "framegraph_dropped_spans",
					  "cpu_allocations_per_second",
					  "cpu_allocated_bytes_per_second",
					  "render_ms_per_second",
					  "physics_ms_per_second",
					  "replication_ms_per_second",
					  "script_ms_per_second",
					  "imagegraph_ms_per_second",
					  "frame_ms_per_second"}) {
					if (!workload.Metrics.contains(required))
						throw std::runtime_error("missing embedded metric: " + std::string(required));
				}
				if (workload.Metrics.at("framegraph_owner_dropped_spans").Maximum != 0 ||
					workload.Metrics.at("framegraph_off_thread_dropped_spans").Maximum !=
						workload.Metrics.at("framegraph_dropped_spans").Maximum)
					throw std::runtime_error("invalid embedded dropped-span provenance");
				if (workload.Metrics.at("frame_count").Minimum <= 0 ||
					workload.Metrics.at("duration_seconds").Minimum < 5) {
					throw std::runtime_error("incomplete embedded runtime window");
				}
			} else if (!workload.Metrics.empty())
				throw std::runtime_error("unavailable workload contains embedded measurements");
			collection.Workloads.emplace(name, std::move(workload));
		}
		if (manifestNames.size() != aggregate.at("workloads").size())
			throw std::runtime_error("unexpected embedded workloads");
		return collection;
	}

	inline void DemoEmbeddedBaseline(std::ostream &document, const DemoCollection &collection) {
		document << "<details>\n<summary>Exact reusable baseline aggregates</summary>\n\n"
				 << "<!-- VERSION_IMPACT_BASELINE_START -->\n```json\n"
				 << DemoAggregateJson(collection).dump(2)
				 << "\n```\n<!-- VERSION_IMPACT_BASELINE_END -->\n\n</details>\n\n";
	}

	inline DemoCollection DemoLoadBaseline(const std::filesystem::path &path) {
		if (std::filesystem::is_directory(path)) return DemoLoad(path);
		std::ifstream input(path);
		if (!input) throw std::runtime_error("cannot read baseline " + path.string());
		const std::string document((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		const std::string startMarker = "<!-- VERSION_IMPACT_BASELINE_START -->";
		const std::string endMarker = "<!-- VERSION_IMPACT_BASELINE_END -->";
		const size_t start = document.find(startMarker);
		const size_t end = document.find(endMarker, start == std::string::npos ? 0 : start);
		if (start == std::string::npos || end == std::string::npos ||
			document.find(startMarker, start + startMarker.size()) != std::string::npos) {
			throw std::runtime_error("missing or duplicate embedded baseline markers");
		}
		const size_t fence = document.find("```json\n", start + startMarker.size());
		const size_t close = document.find("```", fence == std::string::npos ? end : fence + 8);
		if (fence == std::string::npos || fence >= end || close == std::string::npos || close >= end) {
			throw std::runtime_error("invalid embedded baseline JSON fence");
		}
		return DemoLoadAggregate(DemoJson::parse(document.substr(fence + 8, close - fence - 8)));
	}

	inline double DemoFloor(std::string_view metric, const DemoLimits &limits) {
		if (metric.find("bytes") != std::string_view::npos) {
			if (metric.ends_with("_per_second")) return limits.BytesPerSecond;
			return metric.ends_with("_per_frame") ? limits.BytesPerFrame : limits.Bytes;
		}
		if (metric.find("ms_per_second") != std::string_view::npos) return limits.MillisecondsPerSecond;
		if (metric.find("ms_per_frame") != std::string_view::npos) return limits.Milliseconds;
		if (metric.find("allocations") != std::string_view::npos ||
			metric.find("blocks") != std::string_view::npos ||
			metric.find("resources_created") != std::string_view::npos)
			return metric.ends_with("_per_second") ? limits.AllocationsPerSecond : limits.Allocations;
		return 0;
	}

	inline double DemoCeiling(double baseline, std::string_view metric, const DemoLimits &limits) {
		return baseline + std::max(baseline * limits.Percent / 100, DemoFloor(metric, limits));
	}

	inline std::string DemoFormat(double number) {
		std::ostringstream text;
		text << std::fixed << std::setprecision(3) << number;
		return text.str();
	}

	inline void
	DemoMetadata(std::ostream &document, const DemoCollection &collection, const std::string &label) {
		document << "### " << DemoCell(label) << "\n\nSource: `"
				 << DemoCell(collection.Manifest.at("source_revision").get<std::string>()) << "`"
				 << (collection.Manifest.at("source_dirty").get<bool>() ? " (dirty working tree)"
																		: " (clean)")
				 << ". Source diff SHA-256: `"
				 << DemoCell(collection.Manifest.value("source_diff_sha256", "unrecorded"))
				 << "`. Three independent five-second runs.\n\n```json\n"
				 << collection.Manifest.at("machine").dump(2) << "\n"
				 << collection.Manifest.at("settings").dump(2) << "\n```\n\n";
	}

	inline void DemoBaselineTable(std::ostream &document, const DemoCollection &collection) {
		document << "| Demo | Metric | Mean | Population stddev | Minimum | Maximum |\n"
				 << "|---|---|---:|---:|---:|---:|\n";
		for (const auto &[name, workload] : collection.Workloads) {
			if (workload.Status != "measured") {
				document << "| " << DemoCell(name) << " | Unavailable: " << DemoCell(workload.Reason)
						 << " | | | | |\n";
				continue;
			}
			for (const auto &[metric, statistics] : workload.Metrics) {
				document << "| " << DemoCell(name) << " | " << DemoCell(metric) << " | "
						 << DemoFormat(statistics.Mean) << " | " << DemoFormat(statistics.Deviation) << " | "
						 << DemoFormat(statistics.Minimum) << " | " << DemoFormat(statistics.Maximum)
						 << " |\n";
			}
		}
		document << '\n';
	}

	// Missing coverage is a failed comparison. New workloads remain visible without inventing old
	// measurements.
	inline std::string DemoAssetHash(const DemoCollection &collection, const std::string &name) {
		for (const DemoJson &entry : collection.Manifest.at("workloads")) {
			if (entry.at("name").get<std::string>() == name) return entry.value("asset_sha256", "unrecorded");
		}
		return "unrecorded";
	}

	inline bool DemoCompareTable(
		std::ostream &document,
		const DemoCollection &baseline,
		const DemoCollection &current,
		const DemoLimits &limits,
		bool *coverageValid = nullptr
	) {
		bool passed = true;
		if (coverageValid) *coverageValid = true;
		document << "| Demo | Metric | Baseline mean | Current mean | Impact | Ceiling | Result |\n"
				 << "|---|---|---:|---:|---:|---:|---|\n";
		for (const auto &[name, oldWorkload] : baseline.Workloads) {
			const auto candidate = current.Workloads.find(name);
			if (candidate == current.Workloads.end() ||
				(oldWorkload.Status == "measured" && candidate->second.Status != "measured")) {
				document << "| " << DemoCell(name) << " | Missing measurement | | | | | FAIL |\n";
				passed = false;
				if (coverageValid) *coverageValid = false;
				continue;
			}
			if (oldWorkload.Status != "measured") {
				if (candidate->second.Status != "measured") {
					document << "| " << DemoCell(name) << " | Missing current measurement | | | | | FAIL |\n";
					passed = false;
					if (coverageValid) *coverageValid = false;
					continue;
				}
				document << "| " << DemoCell(name) << " | "
						 << (candidate->second.Status == "measured" ? "Added" : "Unavailable")
						 << " | | | | | "
						 << (candidate->second.Status == "measured" ? "Added" : "Unavailable") << " |\n";
				continue;
			}
			const std::string baselineHash = DemoAssetHash(baseline, name);
			const std::string currentHash = DemoAssetHash(current, name);
			if (baselineHash != currentHash) {
				document << "| " << DemoCell(name)
						 << " | Demo input SHA-256 changed: " << DemoCell(baselineHash) << " to "
						 << DemoCell(currentHash) << " | | | | | Input warning |\n";
			}
			const DemoWorkload &newWorkload = candidate->second;
			if (oldWorkload.Basis != newWorkload.Basis) {
				document << "| " << DemoCell(name) << " | Measurement basis changed | | | | | FAIL |\n";
				passed = false;
				if (coverageValid) *coverageValid = false;
			}

			for (const auto &[metric, oldStatistics] : oldWorkload.Metrics) {
				const auto measurement = newWorkload.Metrics.find(metric);
				if (measurement == newWorkload.Metrics.end()) {
					document << "| " << DemoCell(name) << " | " << DemoCell(metric)
							 << " | | | | | FAIL: missing metric |\n";
					passed = false;
					if (coverageValid) *coverageValid = false;
					continue;
				}
				const double currentMean = measurement->second.Mean;
				const double delta = currentMean - oldStatistics.Mean;
				const bool diagnostic = DemoDiagnostic(metric);
				const double ceiling = DemoCeiling(oldStatistics.Mean, metric, limits);
				const bool within = diagnostic || currentMean <= ceiling;
				passed = passed && within;
				document << "| " << DemoCell(name) << " | " << DemoCell(metric) << " | "
						 << DemoFormat(oldStatistics.Mean) << " | " << DemoFormat(currentMean) << " | "
						 << (delta > 0 ? "+" : "") << DemoFormat(delta);
				if (oldStatistics.Mean > 0)
					document << " (" << DemoFormat(delta / oldStatistics.Mean * 100) << "%)";
				else
					document << (currentMean == 0 ? " (0%)" : " (new nonzero)");
				document << " | " << (diagnostic ? "Diagnostic" : DemoFormat(ceiling)) << " | "
						 << (diagnostic ? "Diagnostic"
							 : within	? "PASS"
										: "FAIL")
						 << " |\n";
			}
			for (const auto &[metric, statistics] : newWorkload.Metrics) {
				if (!oldWorkload.Metrics.contains(metric)) {
					document << "| " << DemoCell(name) << " | " << DemoCell(metric) << " | | "
							 << DemoFormat(statistics.Mean) << " | | | Added |\n";
				}
			}
		}
		for (const auto &[name, workload] : current.Workloads) {
			if (workload.Status != "measured") {
				passed = false;
				if (coverageValid) *coverageValid = false;
			}
			if (!baseline.Workloads.contains(name))
				document << "| " << DemoCell(name) << " | "
						 << (workload.Status == "measured" ? "Added workload | | | | | Added"
														   : "Missing current measurement | | | | | FAIL")
						 << " |\n";
		}
		document << '\n';
		return passed;
	}

	inline int DemoReportMain(int argc, char **argv) {
		try {
			std::filesystem::path baselineDirectory;
			std::filesystem::path currentDirectory;
			std::filesystem::path documentPath;
			std::string baselineLabel;
			std::string currentLabel;
			DemoLimits limits;
			bool comparing = false;
			for (int index = 1; index < argc; ++index) {
				const std::string option = argv[index];
				auto value = [&]() -> std::string {
					if (++index >= argc) throw std::runtime_error("missing value for " + option);
					return argv[index];
				};
				if (option == "--demo-compare") {
					comparing = true;
					baselineDirectory = value();
					currentDirectory = value();
				} else if (option == "--demo-table")
					baselineDirectory = value();
				else if (option == "--demo-document")
					documentPath = value();
				else if (option == "--baseline-label" || option == "--demo-label")
					baselineLabel = value();
				else if (option == "--current-label")
					currentLabel = value();
				else if (option == "--demo-advisory")
					limits.Advisory = true;
				else if (option == "--demo-limit-percent" || option == "--demo-byte-floor" ||
						 option == "--demo-byte-per-frame-floor" ||
						 option == "--demo-byte-per-second-floor" || option == "--demo-ms-per-second-floor" ||
						 option == "--demo-allocation-per-second-floor" || option == "--demo-ms-floor" ||
						 option == "--demo-allocation-floor") {
					const std::string text = value();
					size_t consumed = 0;
					const double number = std::stod(text, &consumed);
					if (consumed != text.size() || !std::isfinite(number) || number < 0)
						throw std::runtime_error("invalid limit: " + text);
					if (option == "--demo-limit-percent")
						limits.Percent = number;
					else if (option == "--demo-byte-floor")
						limits.Bytes = number;
					else if (option == "--demo-byte-per-frame-floor")
						limits.BytesPerFrame = number;
					else if (option == "--demo-byte-per-second-floor")
						limits.BytesPerSecond = number;
					else if (option == "--demo-ms-per-second-floor")
						limits.MillisecondsPerSecond = number;
					else if (option == "--demo-allocation-per-second-floor")
						limits.AllocationsPerSecond = number;
					else if (option == "--demo-ms-floor")
						limits.Milliseconds = number;
					else
						limits.Allocations = number;
				} else
					throw std::runtime_error("unknown demo report argument: " + option);
			}
			const DemoCollection baseline = DemoLoadBaseline(baselineDirectory);
			if (baselineLabel.empty()) baselineLabel = baseline.Manifest.value("label", "Baseline");
			std::ostringstream document;
			document
				<< "# Version performance impact\n\nMemory byte rows describe tracked payload. CPU "
				   "allocation overhead is included separately. "
				<< "Render, physics, replication, script and imagegraph timings are CPU self "
				   "milliseconds per frame; "
				<< "Wall timing rows sum outermost inclusive owner-thread scopes for each subsystem, "
				   "including nested waits. "
				<< "replication includes Network category work. GPU memory is logical payload, not "
				   "driver residency. "
				<< "Timings are gated per iteration and per wall-clock second. Allocation churn is gated per "
				   "wall-clock "
				   "second. Per-iteration allocation values, raw "
				   "totals and throughput are diagnostic. External peak process RSS covers the whole "
				   "process lifetime, "
				   "including startup and shutdown. cpu_peak_bytes is sampled at frame boundaries; "
				   "cpu_process_peak_bytes and GPU peaks cover process lifetime. CPU timings cover the "
				   "owning thread only; background workers are "
				   "excluded. "
				   "Off-thread rejected spans are visible as diagnostics. Owner-thread drops invalidate "
				   "measurements.\n\n"
				<< "Run `just demo-bench` to collect three five-second samples per available demo. "
				   "Run `just demo-impact` to compare with this reusable baseline, `just "
				   "demo-impact-report <collection-directory>` to "
				   "refresh this document from saved samples, and `just regression-check` for the regression "
				   "process. "
				   "New workloads appear as Added because an older revision has no measurement to "
				   "compare.\n\n";
			DemoMetadata(document, baseline, baselineLabel);
			DemoBaselineTable(document, baseline);
			DemoEmbeddedBaseline(document, baseline);
			bool passed = true;
			bool coverageValid = true;
			if (comparing) {
				const DemoCollection current = DemoLoad(currentDirectory);
				if (currentLabel.empty()) currentLabel = current.Manifest.value("label", "Current");
				const bool compatible = baseline.Manifest.at("machine") == current.Manifest.at("machine") &&
										baseline.Manifest.at("settings") == current.Manifest.at("settings");
				document
					<< "## " << DemoCell(baselineLabel) << " vs " << DemoCell(currentLabel) << "\n\n"
					<< "Ceiling = baseline mean + max(" << DemoFormat(limits.Percent)
					<< "% of baseline, absolute floor). "
					<< "Floors: " << DemoFormat(limits.Bytes) << " bytes (memory), "
					<< DemoFormat(limits.BytesPerFrame) << " bytes/frame (churn), "
					<< DemoFormat(limits.Milliseconds) << " ms/frame, " << DemoFormat(limits.BytesPerSecond)
					<< " bytes/second, " << DemoFormat(limits.MillisecondsPerSecond) << " ms/second, "
					<< DemoFormat(limits.AllocationsPerSecond) << " allocations/second; "
					<< DemoFormat(limits.Allocations)
					<< " allocations or resources. These are absolute increments in each metric row's unit, "
					   "including bytes/frame and allocations/frame for normalized churn. "
					<< "A zero baseline uses the absolute floor. Three-run population standard "
					   "deviation and range follow below.\n\n";
				if (!compatible)
					document << "Machine or settings mismatch: "
							 << (limits.Advisory ? "explicit advisory override" : "FAIL") << ".\n\n";
				if (limits.Advisory)
					document << "Advisory mode: cost ceilings do not determine the process exit status. "
								"Invalid or missing measurements still fail.\n\n";
				passed = DemoCompareTable(document, baseline, current, limits, &coverageValid) && compatible;
				DemoMetadata(document, current, currentLabel);
				DemoBaselineTable(document, current);
			}
			if (!documentPath.empty()) {
				if (!documentPath.parent_path().empty())
					std::filesystem::create_directories(documentPath.parent_path());
				std::ofstream output(documentPath);
				output << document.str();
				if (!output) throw std::runtime_error("cannot write " + documentPath.string());
			}
			std::cout << document.str();
			return coverageValid && (passed || limits.Advisory) ? 0 : 1;
		} catch (const std::exception &exception) {
			std::cerr << "demo report: " << exception.what() << '\n';
			return 2;
		}
	}
}
