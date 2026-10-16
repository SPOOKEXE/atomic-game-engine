#include "ComputeGraphService.hpp"

#include <engine/computegraph/Stock.hpp>
#include <engine/script/ComputeBridge.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace engine::script {
	namespace compute_service_detail {
		namespace cg = computegraph;
		using V = ScriptValue;
		V Number(double n) {
			V value(ValueTag::Number);
			value.Number = n;
			return value;
		}
		V Text(std::string_view s) {
			V value(ValueTag::String);
			value.Text = s;
			return value;
		}
		V Boolean(bool b) {
			V value(b ? ValueTag::True : ValueTag::False);
			value.Boolean = b;
			return value;
		}
		V Map(std::initializer_list<std::pair<std::string, V>> fields) {
			V value(ValueTag::Map);
			value.Entries = fields;
			return value;
		}
		V Array() {
			return V(ValueTag::Array);
		}
		const V &Field(ScriptCall &call, const V &record, std::string_view key, bool required = true) {
			static const V missing;
			if (record.Tag != ValueTag::Map && !(record.Tag == ValueTag::Array && record.Items.empty()))
				call.Raise("compute record expected");
			const V *found = nullptr;
			for (const auto &[name, value] : record.Entries)
				if (name == key) {
					if (found) call.Raise("duplicate compute field");
					found = &value;
				}
			if (found) return *found;
			if (required) call.Raise(("missing compute field " + std::string(key)).c_str());
			return missing;
		}
		void Keys(ScriptCall &call, const V &record, std::initializer_list<std::string_view> keys) {
			if (record.Tag != ValueTag::Map && !(record.Tag == ValueTag::Array && record.Items.empty()))
				call.Raise("compute record expected");
			for (const auto &[name, value] : record.Entries) {
				(void)value;
				bool known = false;
				for (const auto key : keys)
					if (name == key) known = true;
				if (!known) call.Raise(("unknown compute field " + name).c_str());
			}
		}
		const std::vector<V> &Items(ScriptCall &call, const V &value, size_t maximum) {
			if ((value.Tag != ValueTag::Array && !(value.Tag == ValueTag::Map && value.Entries.empty())) ||
				value.Items.size() > maximum)
				call.Raise("bounded compute array expected");
			return value.Items;
		}
		std::string String(ScriptCall &call, const V &value, size_t maximum = 128, bool empty = false) {
			if (value.Tag != ValueTag::String || value.Text.size() > maximum ||
				(!empty && value.Text.empty()) || value.Text.find('\0') != std::string::npos)
				call.Raise("bounded compute string expected");
			return value.Text;
		}
		uint32_t Uint(ScriptCall &call, const V &value, uint32_t fallback = UINT32_MAX) {
			if (value.Tag == ValueTag::Nil && fallback != UINT32_MAX) return fallback;
			if (value.Tag != ValueTag::Number || !std::isfinite(value.Number) || value.Number < 0 ||
				value.Number > UINT32_MAX || std::floor(value.Number) != value.Number)
				call.Raise("compute integer out of range");
			return static_cast<uint32_t>(value.Number);
		}
		bool Bool(ScriptCall &call, const V &value, bool fallback = false) {
			if (value.Tag == ValueTag::Nil) return fallback;
			if (value.Tag != ValueTag::True && value.Tag != ValueTag::False)
				call.Raise("compute boolean expected");
			return value.Tag == ValueTag::True;
		}
		cg::ElementType Type(ScriptCall &call, const V &value) {
			const auto text = String(call, value);
			for (const auto type : {cg::ElementType::U32, cg::ElementType::F32, cg::ElementType::F32x4})
				if (text == cg::Describe(type)) return type;
			call.Raise("compute element type must be u32, f32 or f32x4");
			return cg::ElementType::U32;
		}
		cg::Access Access(ScriptCall &call, const V &value, cg::Access fallback) {
			if (value.Tag == ValueTag::Nil) return fallback;
			const auto text = String(call, value);
			if (text == "Read") return cg::Access::Read;
			if (text == "Write") return cg::Access::Write;
			if (text == "ReadWrite") return cg::Access::ReadWrite;
			call.Raise("compute access must be Read, Write or ReadWrite");
			return fallback;
		}
		const char *AccessName(cg::Access value) {
			switch (value) {
			case cg::Access::Read:
				return "Read";
			case cg::Access::Write:
				return "Write";
			case cg::Access::ReadWrite:
				return "ReadWrite";
			}
			return "invalid";
		}
		V ReadRecord(ScriptCall &call, size_t index) {
			V value;
			CodecStatus why{};
			if (!call.ReadBoundedValue(index, value, why, 512u * 1024u))
				call.Raise("compute definition exceeds the bounded record contract");
			// The owned tree has its own cap; the shared encoder enforces the wire cap.
			std::vector<std::byte> encoded;
			if (Encode(value, encoded) != CodecStatus::Ok)
				call.Raise("compute record exceeds 64 KiB encoding contract");
			return value;
		}
		cg::Program Program(ScriptCall &call, const V &record) {
			Keys(call, record, {"Buffers", "Kernels", "Passes", "ErrorBuffers"});
			cg::Program program;
			for (const auto &row : Items(call, Field(call, record, "Buffers"), 128)) {
				Keys(call, row, {"Name", "Type", "Capacity", "Count", "CountBuffer", "Access", "External"});
				cg::BufferDesc b;
				b.Name = String(call, Field(call, row, "Name"));
				b.Type = Type(call, Field(call, row, "Type"));
				b.Stride = cg::ElementStride(b.Type);
				b.Capacity = Uint(call, Field(call, row, "Capacity"));
				b.LogicalCount = Uint(call, Field(call, row, "Count", false), 0);
				const auto &count = Field(call, row, "CountBuffer", false);
				if (count.Tag != ValueTag::Nil) b.CountBuffer = String(call, count);
				b.Permission = Access(call, Field(call, row, "Access", false), cg::Access::ReadWrite);
				b.External = Bool(call, Field(call, row, "External", false));
				program.Definition.Buffers.push_back(std::move(b));
			}
			for (const auto &row : Items(call, Field(call, record, "Kernels"), 64)) {
				Keys(call, row, {"Name", "Version", "Local", "Ports", "Source", "BuiltIn"});
				cg::KernelDesc k;
				k.Name = String(call, Field(call, row, "Name"));
				k.Version = Uint(call, Field(call, row, "Version", false), 1);
				const auto &local = Items(call, Field(call, row, "Local"), 3);
				if (local.size() != 3) call.Raise("compute local dimensions require three integers");
				for (size_t i = 0; i < 3; ++i)
					k.Local[i] = Uint(call, local[i]);
				for (const auto &port : Items(call, Field(call, row, "Ports"), 32)) {
					Keys(call, port, {"Name", "Type", "Access", "Set", "Binding", "MinimumBytes"});
					cg::KernelPort p;
					p.Name = String(call, Field(call, port, "Name"));
					p.Type = Type(call, Field(call, port, "Type"));
					p.Stride = cg::ElementStride(p.Type);
					p.Permission = Access(call, Field(call, port, "Access", false), cg::Access::Read);
					p.Set = Uint(call, Field(call, port, "Set"));
					p.Binding = Uint(call, Field(call, port, "Binding"));
					p.MinimumBytes = Uint(call, Field(call, port, "MinimumBytes", false), 0);
					k.Ports.push_back(std::move(p));
				}
				cg::KernelCode code;
				code.Name = k.Name;
				const auto &source = Field(call, row, "Source", false),
						   &builtIn = Field(call, row, "BuiltIn", false);
				if (source.Tag != ValueTag::Nil) code.Source = String(call, source, 48u * 1024u);
				if (builtIn.Tag != ValueTag::Nil) {
					code.BuiltIn = String(call, builtIn);
					// Only this fixed stock family is selectable by script resource name.
					if (code.BuiltIn != "compute-classify.comp" && code.BuiltIn != "compute-scan.comp" &&
						code.BuiltIn != "compute-add-offsets.comp" && code.BuiltIn != "compute-scatter.comp")
						call.Raise("unknown stock compute kernel");
				}
				if (code.Source.empty() == code.BuiltIn.empty())
					call.Raise("compute kernel needs exactly one Source or BuiltIn");
				program.Definition.Kernels.push_back(std::move(k));
				program.Kernels.push_back(std::move(code));
			}
			for (const auto &row : Items(call, Field(call, record, "Passes"), 256)) {
				Keys(call, row, {"Name", "Kernel", "DispatchBuffer", "Bindings"});
				cg::PassDesc pass;
				pass.Name = String(call, Field(call, row, "Name"));
				pass.Kernel = String(call, Field(call, row, "Kernel"));
				pass.DispatchBuffer = String(call, Field(call, row, "DispatchBuffer"));
				for (const auto &binding : Items(call, Field(call, row, "Bindings"), 32)) {
					Keys(call, binding, {"Port", "Buffer"});
					pass.Bindings.push_back(
						{String(call, Field(call, binding, "Port")),
						 String(call, Field(call, binding, "Buffer"))}
					);
				}
				program.Definition.Passes.push_back(std::move(pass));
			}
			const auto &errors = Field(call, record, "ErrorBuffers", false);
			if (errors.Tag != ValueTag::Nil)
				for (const auto &error : Items(call, errors, 128))
					program.ErrorBuffers.push_back(String(call, error));
			return program;
		}
		V ProgramValue(const cg::Program &program) {
			V buffers = Array(), kernels = Array(), passes = Array(), errors = Array();
			for (const auto &b : program.Definition.Buffers) {
				auto row = Map(
					{{"Name", Text(b.Name)},
					 {"Type", Text(cg::Describe(b.Type))},
					 {"Capacity", Number(b.Capacity)},
					 {"Count", Number(b.LogicalCount)},
					 {"Access", Text(AccessName(b.Permission))},
					 {"External", Boolean(b.External)}}
				);
				if (!b.CountBuffer.empty()) row.Entries.emplace_back("CountBuffer", Text(b.CountBuffer));
				buffers.Items.push_back(std::move(row));
			}
			for (size_t i = 0; i < program.Definition.Kernels.size(); ++i) {
				const auto &k = program.Definition.Kernels[i];
				V ports = Array(), local = Array();
				for (const auto dim : k.Local)
					local.Items.push_back(Number(dim));
				for (const auto &p : k.Ports)
					ports.Items.push_back(Map(
						{{"Name", Text(p.Name)},
						 {"Type", Text(cg::Describe(p.Type))},
						 {"Access", Text(AccessName(p.Permission))},
						 {"Set", Number(p.Set)},
						 {"Binding", Number(p.Binding)},
						 {"MinimumBytes", Number(static_cast<double>(p.MinimumBytes))}}
					));
				auto row = Map(
					{{"Name", Text(k.Name)},
					 {"Version", Number(k.Version)},
					 {"Local", std::move(local)},
					 {"Ports", std::move(ports)}}
				);
				const auto &code = program.Kernels[i];
				if (!code.Source.empty()) row.Entries.emplace_back("Source", Text(code.Source));
				if (!code.BuiltIn.empty()) row.Entries.emplace_back("BuiltIn", Text(code.BuiltIn));
				kernels.Items.push_back(std::move(row));
			}
			for (const auto &p : program.Definition.Passes) {
				V bindings = Array();
				for (const auto &b : p.Bindings)
					bindings.Items.push_back(Map({{"Port", Text(b.Port)}, {"Buffer", Text(b.Buffer)}}));
				passes.Items.push_back(Map(
					{{"Name", Text(p.Name)},
					 {"Kernel", Text(p.Kernel)},
					 {"DispatchBuffer", Text(p.DispatchBuffer)},
					 {"Bindings", std::move(bindings)}}
				));
			}
			for (const auto &e : program.ErrorBuffers)
				errors.Items.push_back(Text(e));
			return Map(
				{{"Buffers", std::move(buffers)},
				 {"Kernels", std::move(kernels)},
				 {"Passes", std::move(passes)},
				 {"ErrorBuffers", std::move(errors)}}
			);
		}
		ComputeBridge &Bridge(ScriptCall &call) {
			const auto &bridge = call.Compute();
			if (!bridge) call.Raise(cg::Describe(cg::Status::DeviceUnavailable));
			if (call.RuntimeIdentity() == 0 || bridge->Identity().Runtime != call.RuntimeIdentity())
				call.Raise(cg::Describe(cg::Status::StaleOwner));
			if (bridge->Identity().Generation != call.World().Identity() ||
				bridge->Identity().World != call.World().Name()) {
				bridge->Detach();
				call.Raise(cg::Describe(cg::Status::StaleOwner));
			}
			return *bridge;
		}
		uint64_t Ticket(ScriptCall &call) {
			const auto text = call.AsBoundedString(0, 20);
			uint64_t ticket = 0;
			auto result = std::from_chars(text.data(), text.data() + text.size(), ticket);
			if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || ticket == 0)
				call.Raise("invalid compute ticket");
			return ticket;
		}
		void Check(ScriptCall &call, cg::Status status, std::string_view detail = {}) {
			if (status != cg::Status::Ok)
				call.Raise(detail.empty() ? cg::Describe(status) : std::string(detail).c_str());
		}
		void StockGraph(ScriptCall &call) {
			if (call.AsBoundedString(0, 128) != "StableCompactU32") call.Raise("unknown stock compute graph");
			const double n = call.AsNumber(1);
			V value = Number(n);
			cg::Program program;
			cg::Diagnostic diagnostic;
			Check(call, cg::StockStableCompactU32(Uint(call, value), program, diagnostic));
			call.ReturnValue(ProgramValue(program));
		}
		void DefineGraph(ScriptCall &call) {
			auto &bridge = Bridge(call);
			auto name = call.AsBoundedString(0, 128);
			auto program = Program(call, ReadRecord(call, 1));
			uint64_t ticket = 0;
			std::string detail;
			const auto status = bridge.Define(std::move(name), std::move(program), ticket, detail);
			Check(call, status, detail);
			call.ReturnString(std::to_string(ticket));
		}
		void SubmitGraph(ScriptCall &call) {
			auto &bridge = Bridge(call);
			auto name = call.AsBoundedString(0, 128);
			const auto record = ReadRecord(call, 1);
			Keys(call, record, {"Inputs", "Outputs"});
			cg::Request request;
			const auto &inputs = Items(call, Field(call, record, "Inputs"), 128);
			if (call.Arguments() != inputs.size() + 2)
				call.Raise("compute input descriptors and byte buffers differ");
			uint64_t bytes = 0;
			for (size_t i = 0; i < inputs.size(); ++i) {
				const auto &row = inputs[i];
				Keys(call, row, {"Name", "Type", "Count"});
				cg::Upload upload;
				upload.Buffer = String(call, Field(call, row, "Name"));
				upload.Type = Type(call, Field(call, row, "Type"));
				upload.Count = Uint(call, Field(call, row, "Count"));
				const auto expected = uint64_t(upload.Count) * cg::ElementStride(upload.Type);
				if (expected > 64u * 1024u * 1024u / 3 - bytes) call.Raise("compute upload budget exceeded");
				upload.Bytes = call.AsBytes(i + 2, static_cast<size_t>(expected));
				if (upload.Bytes.size() != expected)
					call.Raise("compute input byte length differs from typed count");
				bytes += expected;
				request.Inputs.push_back(std::move(upload));
			}
			for (const auto &row : Items(call, Field(call, record, "Outputs"), 128)) {
				Keys(call, row, {"Name", "MaximumElements"});
				request.Outputs.push_back(
					{String(call, Field(call, row, "Name")), Uint(call, Field(call, row, "MaximumElements"))}
				);
			}
			uint64_t ticket = 0;
			std::string detail;
			const auto status = bridge.Submit(std::move(name), std::move(request), ticket, detail);
			Check(call, status, detail);
			call.ReturnString(std::to_string(ticket));
		}
		void PollGraph(ScriptCall &call) {
			auto &bridge = Bridge(call);
			script::ComputePoll reply;
			Check(call, bridge.Poll(Ticket(call), reply));
			V outputs = Array();
			for (const auto &b : reply.Outputs)
				outputs.Items.push_back(Map(
					{{"Name", Text(b.Name)},
					 {"Type", Text(cg::Describe(b.Type))},
					 {"Count", Number(b.Count)},
					 {"Bytes", Number(static_cast<double>(b.Bytes))}}
				));
			call.ReturnValue(Map(
				{{"Status",
				  Text(
					  !reply.Ready					 ? "pending"
					  : reply.Cancelled				 ? "cancelled"
					  : reply.Code == cg::Status::Ok ? "complete"
													 : "failed"
				  )},
				 {"Detail", Text(reply.Detail)},
				 {"Code", Text(cg::Describe(reply.Code))},
				 {"Outputs", std::move(outputs)}}
			));
		}
		void ReadGraphResult(ScriptCall &call) {
			auto &bridge = Bridge(call);
			const auto ticket = Ticket(call);
			auto name = call.AsBoundedString(1, 128);
			V offset = Number(call.AsNumber(2)), maximum = Number(call.AsNumber(3));
			std::vector<std::byte> bytes;
			Check(call, bridge.Read(ticket, name, Uint(call, offset), Uint(call, maximum), bytes));
			call.ReturnBytes(bytes);
		}
		void CancelGraph(ScriptCall &call) {
			auto &bridge = Bridge(call);
			call.ReturnBoolean(bridge.Cancel(Ticket(call)));
		}
		void ReleaseGraph(ScriptCall &call) {
			auto &bridge = Bridge(call);
			call.ReturnBoolean(bridge.Release(Ticket(call)));
		}
		template <void (*Method)(ScriptCall &)> void Guard(ScriptCall &call) {
			try {
				Method(call);
			} catch (const std::bad_alloc &) {
				call.Raise(cg::Describe(cg::Status::AllocationFailed));
			}
		}
		constexpr std::array<ServiceMethod, 7> METHODS{
			{{"StockGraph", Guard<StockGraph>},
			 {"DefineGraph", Guard<DefineGraph>},
			 {"SubmitGraph", Guard<SubmitGraph>},
			 {"PollGraph", Guard<PollGraph>},
			 {"ReadGraphResult", Guard<ReadGraphResult>},
			 {"CancelGraph", Guard<CancelGraph>},
			 {"ReleaseGraph", Guard<ReleaseGraph>}}
		};
	}
	std::span<const ServiceMethod> ComputeGraphServiceMethods() {
		return compute_service_detail::METHODS;
	}
}
