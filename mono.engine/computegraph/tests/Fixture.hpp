#pragma once

#include <engine/computegraph/Compile.hpp>

namespace engine::computegraph::tests {
	inline Document CompactChain(uint32_t capacity = 257, uint32_t count = 257) {
		Document document;
		document.Buffers = {
			{.Name = "input",
			 .Capacity = capacity,
			 .LogicalCount = count,
			 .Permission = Access::Read,
			 .External = true},
			{.Name = "packed", .Capacity = capacity, .CountBuffer = "count"},
			{.Name = "count", .Capacity = 1, .LogicalCount = 1},
			{.Name = "doubled", .Capacity = capacity, .CountBuffer = "count"},
		};
		document.Kernels = {
			{.Name = "compact-nonzero-u32",
			 .Ports =
				 {
					 {.Name = "values"},
					 {.Name = "packed", .Permission = Access::Write, .Set = 1},
					 {.Name = "count",
					  .Permission = Access::Write,
					  .Set = 1,
					  .Binding = 1,
					  .MinimumBytes = 4},
				 }},
			{.Name = "double-u32",
			 .Ports = {
				 {.Name = "values"},
				 {.Name = "count", .Binding = 1, .MinimumBytes = 4},
				 {.Name = "result", .Permission = Access::Write, .Set = 1},
			 }},
		};
		document.Passes = {
			{.Name = "compact",
			 .Kernel = "compact-nonzero-u32",
			 .DispatchBuffer = "input",
			 .Bindings =
				 {
					 {"values", "input"},
					 {"packed", "packed"},
					 {"count", "count"},
				 }},
			{.Name = "consume",
			 .Kernel = "double-u32",
			 .DispatchBuffer = "packed",
			 .Bindings = {
				 {"values", "packed"},
				 {"count", "count"},
				 {"result", "doubled"},
			 }},
		};
		return document;
	}
	inline std::vector<KernelReflection> Reflections(const Document &document) {
		std::vector<KernelReflection> result;
		for (const auto &kernel : document.Kernels)
			result.push_back({kernel.Name, kernel.Version, kernel.Local, kernel.Ports});
		return result;
	}
}
