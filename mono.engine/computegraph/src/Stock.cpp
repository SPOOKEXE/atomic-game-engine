#include <engine/computegraph/Stock.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace engine::computegraph {
	namespace stock_detail {
		constexpr uint32_t GROUP = 64;
		uint32_t Groups(uint32_t count) {
			return count / GROUP + uint32_t(count % GROUP != 0);
		}
		KernelPort Read(std::string name, uint32_t slot, bool scalar = false) {
			return {.Name = std::move(name), .Binding = slot, .MinimumBytes = scalar ? 4u : 0u};
		}
		KernelPort Write(std::string name, uint32_t slot) {
			return {.Name = std::move(name), .Permission = Access::Write, .Set = 1, .Binding = slot};
		}
		Program CompactProgram(uint32_t capacity) {
			Program program;
			auto &document = program.Definition;
			const auto buffer = [&](std::string name, uint32_t count) {
				document.Buffers.push_back(
					{.Name = std::move(name), .Capacity = count, .LogicalCount = count}
				);
			};
			const auto error = [&](std::string name, uint32_t work) {
				buffer(name, std::max(1u, Groups(work)));
				program.ErrorBuffers.push_back(std::move(name));
			};
			document.Buffers.push_back(
				{.Name = "input", .Capacity = capacity, .Permission = Access::Read, .External = true}
			);
			const auto scratch = std::max(1u, capacity);
			buffer("flags", scratch);
			buffer("count", 1);
			document.Buffers.push_back({.Name = "packed", .Capacity = capacity, .CountBuffer = "count"});
			document.Kernels = {
				{.Name = "classify", .Ports = {Read("Values", 0), Write("Flags", 0)}},
				{.Name = "scan",
				 .Ports = {Read("Values", 0), Write("Prefix", 0), Write("Sums", 1), Write("Errors", 2)}},
				{.Name = "add-offsets",
				 .Ports = {Read("Local", 0), Read("Parent", 1), Write("Prefix", 0), Write("Errors", 1)}},
				{.Name = "scatter",
				 .Ports = {
					 Read("Values", 0),
					 Read("Flags", 1),
					 Read("Prefix", 2),
					 Read("Count", 3, true),
					 Write("Packed", 0),
					 Write("Errors", 1)
				 }},
			};
			program.Kernels = {
				{.Name = "classify", .BuiltIn = "compute-classify.comp"},
				{.Name = "scan", .BuiltIn = "compute-scan.comp"},
				{.Name = "add-offsets", .BuiltIn = "compute-add-offsets.comp"},
				{.Name = "scatter", .BuiltIn = "compute-scatter.comp"},
			};
			document.Passes.push_back(
				{.Name = "classify",
				 .Kernel = "classify",
				 .DispatchBuffer = "flags",
				 .Bindings = {{"Values", "input"}, {"Flags", "flags"}}}
			);
			std::vector<std::string> prefixes;
			std::vector<uint32_t> extents;
			std::string source = "flags";
			uint32_t extent = scratch;
			for (uint32_t level = 0;; ++level) {
				const auto label = std::to_string(level);
				const auto prefix = "local-" + label;
				const auto sums = Groups(extent) == 1 ? "count" : "sums-" + label;
				const auto errors = "scan-errors-" + label;
				buffer(prefix, extent);
				if (sums != "count") buffer(sums, Groups(extent));
				error(errors, extent);
				document.Passes.push_back(
					{.Name = "scan-" + label,
					 .Kernel = "scan",
					 .DispatchBuffer = source,
					 .Bindings = {{"Values", source}, {"Prefix", prefix}, {"Sums", sums}, {"Errors", errors}}}
				);
				prefixes.push_back(prefix);
				extents.push_back(extent);
				if (Groups(extent) == 1) break;
				source = sums;
				extent = Groups(extent);
			}
			std::string global = prefixes.back();
			for (size_t level = prefixes.size() - 1; level > 0;) {
				--level;
				const auto label = std::to_string(level);
				const auto next = "global-" + label;
				const auto errors = "add-errors-" + label;
				buffer(next, extents[level]);
				error(errors, extents[level]);
				document.Passes.push_back(
					{.Name = "add-" + label,
					 .Kernel = "add-offsets",
					 .DispatchBuffer = prefixes[level],
					 .Bindings = {
						 {"Local", prefixes[level]}, {"Parent", global}, {"Prefix", next}, {"Errors", errors}
					 }}
				);
				global = next;
			}
			error("scatter-errors", scratch);
			document.Passes.push_back(
				{.Name = "scatter",
				 .Kernel = "scatter",
				 .DispatchBuffer = "flags",
				 .Bindings = {
					 {"Values", "input"},
					 {"Flags", "flags"},
					 {"Prefix", global},
					 {"Count", "count"},
					 {"Packed", "packed"},
					 {"Errors", "scatter-errors"}
				 }}
			);
			return program;
		}

	}
	Status StockStableCompactU32(uint32_t capacity, Program &out, Diagnostic &diagnostic) try {
		// The portable host admits at most 65535 groups per one-dimensional dispatch.
		if (capacity > 65535u * 64u) {
			diagnostic = {Status::LimitExceeded, "capacity"};
			return diagnostic.Code;
		}
		auto candidate = stock_detail::CompactProgram(capacity);
		out = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic.Subject.clear();
		diagnostic.Code = Status::AllocationFailed;
		return diagnostic.Code;
	}
}
