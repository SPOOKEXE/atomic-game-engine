#include "ComputeGraph.hpp"

#include "GpuDrawPackets.hpp"
#include "GpuHeap.hpp"
#include "GpuQueueBatch.hpp"
#include "ShaderBinary.hpp"
#include "VulkanTimestamps.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/ShaderCompiler.hpp>
#include <engine/resources/Shaders.hpp>

#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <spirv_cross.hpp>
#include <stdexcept>
#include <thread>
#include <utility>

namespace engine::render {
	namespace computegraph_detail {
		namespace cg = computegraph;
		constexpr uint64_t MAXIMUM_DEVICE_BYTES = 256u * 1024u * 1024u;
		constexpr uint64_t MAXIMUM_CODE_BYTES = 16u * 1024u * 1024u;
		constexpr uint32_t MAXIMUM_SLOTS = 16;
		constexpr size_t MAXIMUM_JOBS = 8;
		constexpr uint32_t HOST_ABI_VERSION = 1;

		struct Parameters {
			std::array<uint32_t, 4> Dispatch{};
			std::array<std::array<uint32_t, 4>, MAXIMUM_SLOTS * 2> Buffers{};
		};
		static_assert(sizeof(Parameters) == 528);

		bool ValidName(std::string_view name) {
			return !name.empty() && name.size() <= 128 && name.find('\0') == std::string_view::npos;
		}
		template <typename Collection> uint32_t Find(const Collection &collection, std::string_view name) {
			for (size_t index = 0; index < collection.size(); ++index)
				if (collection[index].Name == name) return static_cast<uint32_t>(index);
			return UINT32_MAX;
		}
		bool Writes(cg::Access access) {
			return access != cg::Access::Read;
		}

		cg::RuntimeLimits Bounded(cg::RuntimeLimits limits) {
			limits.MaximumPending = std::min(limits.MaximumPending, MAXIMUM_JOBS);
			limits.MaximumUploadBytes = std::min(limits.MaximumUploadBytes, uint64_t{64u * 1024u * 1024u});
			limits.MaximumReadbackBytes =
				std::min(limits.MaximumReadbackBytes, uint64_t{16u * 1024u * 1024u});
			limits.MaximumResultBytes = std::min(limits.MaximumResultBytes, uint64_t{16u * 1024u * 1024u});
			return limits;
		}
		cg::DispatchLimits PortableLimits() {
			cg::DispatchLimits limits;
			// Vulkan's guaranteed minimum covers this one-dimensional kernel family.
			limits.MaximumLocal = {128, 1, 1};
			limits.MaximumThreads = 128;
			return limits;
		}

		struct Binary {
			std::vector<uint32_t> Words{};
			cg::KernelReflection Reflection{};
			bool Uniform = false;
		};
		bool UniformLayout(spirv_cross::Compiler &compiler, const spirv_cross::Resource &resource) {
			const auto &block = compiler.get_type(resource.base_type_id);
			if (compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) != 2 ||
				compiler.get_decoration(resource.id, spv::DecorationBinding) != 0 ||
				!compiler.get_type(resource.type_id).array.empty() || block.member_types.size() != 2 ||
				compiler.get_declared_struct_size(block) != sizeof(Parameters))
				return false;
			for (uint32_t member = 0; member < 2; ++member) {
				const auto &type = compiler.get_type(block.member_types[member]);
				if (type.basetype != spirv_cross::SPIRType::UInt || type.width != 32 || type.vecsize != 4 ||
					type.columns != 1 || compiler.type_struct_member_offset(block, member) != member * 16)
					return false;
				if (member == 0 && !type.array.empty()) return false;
				if (member == 1 && (type.array.size() != 1 || !type.array_size_literal[0] ||
									type.array[0] != MAXIMUM_SLOTS * 2 ||
									compiler.type_struct_member_array_stride(block, member) != 16))
					return false;
			}
			return true;
		}

		bool Reflect(Binary &binary, std::string_view name, std::string &error) {
			try {
				spirv_cross::Compiler compiler(binary.Words.data(), binary.Words.size());
				const auto entries = compiler.get_entry_points_and_stages();
				if (entries.size() != 1 || entries[0].name != "main" ||
					entries[0].execution_model != spv::ExecutionModelGLCompute) {
					error = "compute kernels require one main compute entry point";
					return false;
				}
				const auto resources = compiler.get_shader_resources();
				if (!resources.sampled_images.empty() || !resources.separate_images.empty() ||
					!resources.separate_samplers.empty() || !resources.storage_images.empty() ||
					!resources.push_constant_buffers.empty() || !resources.subpass_inputs.empty() ||
					!resources.atomic_counters.empty() || !resources.acceleration_structures.empty() ||
					!resources.shader_record_buffers.empty() || !resources.gl_plain_uniforms.empty() ||
					!resources.tensors.empty() || resources.uniform_buffers.size() > 1 ||
					resources.storage_buffers.size() > MAXIMUM_SLOTS * 2) {
					error =
						"compute kernels accept only bounded typed storage buffers and the host uniform ABI";
					return false;
				}
				binary.Uniform = !resources.uniform_buffers.empty();
				if (binary.Uniform && !UniformLayout(compiler, resources.uniform_buffers.front())) {
					error = "compute uniform block differs from host ABI v1";
					return false;
				}
				auto &reflection = binary.Reflection;
				reflection.Name = name;
				reflection.Version = HOST_ABI_VERSION;
				reflection.Local = {
					compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0),
					compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1),
					compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2),
				};
				for (const auto &resource : resources.storage_buffers) {
					const auto &block = compiler.get_type(resource.base_type_id);
					if (!compiler.get_type(resource.type_id).array.empty() ||
						block.member_types.size() != 1 || compiler.type_struct_member_offset(block, 0) != 0) {
						error = "typed storage blocks require one array member at byte zero";
						return false;
					}
					const auto &type = compiler.get_type(block.member_types[0]);
					if (type.array.size() != 1 || !type.array_size_literal[0] || type.columns != 1 ||
						(type.basetype != spirv_cross::SPIRType::Struct && type.width != 32)) {
						error = "unsupported typed storage element or descriptor array";
						return false;
					}
					const auto flags = compiler.get_buffer_block_flags(resource.id);
					cg::KernelPort port;
					port.Name = compiler.get_name(resource.base_type_id);
					if (port.Name.empty()) port.Name = resource.name;
					port.Permission = flags.get(spv::DecorationNonWritable)	  ? cg::Access::Read
									  : flags.get(spv::DecorationNonReadable) ? cg::Access::Write
																			  : cg::Access::ReadWrite;
					if (flags.get(spv::DecorationNonWritable) && flags.get(spv::DecorationNonReadable)) {
						error = "storage binding is neither readable nor writable";
						return false;
					}
					if (type.basetype == spirv_cross::SPIRType::UInt && type.vecsize == 1)
						port.Type = cg::ElementType::U32;
					else if (type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 1)
						port.Type = cg::ElementType::F32;
					else if (type.basetype == spirv_cross::SPIRType::Float && type.vecsize == 4)
						port.Type = cg::ElementType::F32x4;
					else if (type.basetype == spirv_cross::SPIRType::Struct &&
							 port.Name == "engineInstanceBuffer" && port.Permission == cg::Access::Read)
						port.Type = cg::ElementType::EngineInstance;
					else {
						error = "unsupported compute element layout";
						return false;
					}
					port.Stride = compiler.type_struct_member_array_stride(block, 0);
					port.Set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
					port.Binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
					port.MinimumBytes = compiler.get_declared_struct_size_runtime_array(block, 0);
					if (!ValidName(port.Name) || port.Binding >= MAXIMUM_SLOTS) {
						error = "compute buffer identity or descriptor slot exceeds its bound";
						return false;
					}
					reflection.Ports.push_back(std::move(port));
				}
				return true;
			} catch (const spirv_cross::CompilerError &failure) {
				error = std::string(failure.what()).substr(0, 4096);
				return false;
			}
		}

		bool Load(const ComputeKernelCode &code, Binary &binary, std::string &error) {
			if (!ValidName(code.Name) || code.Source.empty() == code.BuiltIn.empty()) {
				error = "each compute kernel needs exactly one authored source or built-in module";
				return false;
			}
			if (!code.Source.empty()) {
				if (code.Source.size() > 1024u * 1024u) {
					error = "compute source exceeds 1 MiB";
					return false;
				}
				ShaderCompiler compiler;
				auto compiled = compiler.Compile(code.Source, ShaderStage::Compute, code.Name);
				if (compiled.Failed) {
					error = compiled.Error.substr(0, 4096);
					return false;
				}
				binary.Words = std::move(compiled.SpirV);
			} else {
				if (!ValidName(code.BuiltIn) || !code.BuiltIn.ends_with(".comp") ||
					code.BuiltIn.find_first_not_of(
						"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
					) != std::string::npos) {
					error = "built-in compute modules require a staged shader filename";
					return false;
				}
				std::ifstream file(
					resources::Shader(code.BuiltIn, resources::ShaderForm::SpirV),
					std::ios::binary | std::ios::ate
				);
				const auto bytes = file ? file.tellg() : std::streampos(-1);
				if (bytes <= 0 || uint64_t(bytes) > MAXIMUM_CODE_BYTES || uint64_t(bytes) % 4 != 0) {
					error = "built-in compute module is missing or exceeds its binary bound";
					return false;
				}
				binary.Words.resize(static_cast<size_t>(bytes) / 4);
				file.seekg(0);
				if (!file.read(
						reinterpret_cast<char *>(binary.Words.data()), static_cast<std::streamsize>(bytes)
					)) {
					error = "could not read built-in compute module";
					return false;
				}
			}
			return !binary.Words.empty() && binary.Words.size() <= MAXIMUM_CODE_BYTES / 4 &&
				   Reflect(binary, code.Name, error);
		}

		struct Pipeline {
			SDL_GPUComputePipeline *Handle = nullptr;
			uint32_t Readonly = 0;
			uint32_t Writable = 0;
			bool Uniform = false;
		};
		struct Package {
			SDL_GPUDevice *Device = nullptr;
			cg::CompiledGraph Plan{};
			std::vector<Pipeline> Pipelines{};
			std::vector<uint32_t> Counts{};
			std::vector<uint32_t> Errors{};
			~Package() {
				for (const auto &pipeline : Pipelines)
					if (pipeline.Handle) SDL_ReleaseGPUComputePipeline(Device, pipeline.Handle);
			}
		};

		enum class SegmentKind : uint8_t { Output, Count, Error };
		struct Download {
			uint32_t Buffer = 0;
			uint32_t Offset = 0;
			uint32_t Bytes = 0;
			SegmentKind Kind = SegmentKind::Output;
		};
		struct Upload {
			uint32_t Buffer = 0;
			uint32_t Offset = 0;
			uint32_t Bytes = 0;
			uint32_t Input = UINT32_MAX;
		};
		struct Job {
			SDL_GPUDevice *Device = nullptr;
			uint64_t Ticket = 0;
			uint64_t DeviceBytes = 0;
			uint64_t UploadBytes = 0;
			uint64_t DownloadBytes = 0;
			bool Canceled = false;
			bool Finished = false;
			bool PendingSubmission = false;
			bool SubmissionFailed = false;
			bool FailurePublished = false;
			bool Batched = false;
			uint32_t TimingSlot = VulkanTimestamps::NO_SLOT;
			uint32_t TimingFrom = VulkanTimestamps::MARKS;
			uint32_t TimingTo = VulkanTimestamps::MARKS;
			bool TimingResolved = false;
			std::shared_ptr<const GpuRetirementProof> Retirement;
			bool BatchMetricsPending = false;
			uint64_t InstanceCopyBytes = 0;
			uint64_t Workgroups = 0;
			uint32_t Dispatches = 0;
			SDL_GPUFence *Fence = nullptr;
			SDL_GPUTransferBuffer *Source = nullptr;
			SDL_GPUTransferBuffer *Target = nullptr;
			std::vector<SDL_GPUBuffer *> Buffers{};
			std::vector<Upload> Uploads{};
			std::vector<Download> Downloads{};
			~Job() {
				if (Fence) SDL_ReleaseGPUFence(Device, Fence);
				for (auto *buffer : Buffers)
					if (buffer) gpu::ReleaseBuffer(Device, buffer);
				if (Source) gpu::ReleaseTransferBuffer(Device, Source);
				if (Target) gpu::ReleaseTransferBuffer(Device, Target);
			}
		};
		struct Profiling {
			VulkanTimestamps Queries;
			std::array<ComputeGraphTiming, MAXIMUM_JOBS> Records{};
			ComputeGraphStatistics Statistics{.Enabled = true};
			const AtomicGpuQueuesV1 *Api = nullptr;
			bool UncertainSubmission = false;
			ComputeGraphTiming *Find(uint64_t ticket) {
				for (auto &record : Records)
					if (record.Ticket == ticket) return &record;
				return nullptr;
			}
			void Erase(uint64_t ticket) {
				if (auto *record = Find(ticket)) *record = {};
			}
			static void SDLCALL FinishCleanup(void *userdata, bool resourcesRetired) noexcept {
				auto *profiling = static_cast<Profiling *>(userdata);
				profiling->Queries.Shutdown(resourcesRetired);
				delete profiling;
			}
		};
		struct QueuePacket {
			struct UploadCommand {
				SDL_GPUTransferBufferLocation Source;
				SDL_GPUBufferRegion Target;
			};
			struct BufferCopy {
				SDL_GPUBufferLocation Source;
				SDL_GPUBufferLocation Target;
				uint32_t Bytes = 0;
			};
			struct DispatchCommand {
				Pipeline Shader;
				std::array<SDL_GPUBuffer *, MAXIMUM_SLOTS> Inputs{};
				std::array<SDL_GPUStorageBufferReadWriteBinding, MAXIMUM_SLOTS> Outputs{};
				Parameters Uniforms;
				std::array<uint32_t, 3> Groups{};
			};
			struct DownloadCommand {
				SDL_GPUBufferRegion Source;
				SDL_GPUTransferBufferLocation Target;
			};

			// Transfer buffers and installed shaders outlive deferred recording and owner-side settlement.
			std::shared_ptr<Job> Owner;
			std::shared_ptr<const Package> Program;
			FrozenGpuResources Resources;
			std::vector<UploadCommand> Uploads;
			std::vector<BufferCopy> Copies;
			std::vector<DispatchCommand> Dispatches;
			std::vector<DownloadCommand> Downloads;

			QueuePacket(
				SDL_GPUDevice *device,
				const AtomicGpuQueuesV1 &api,
				std::shared_ptr<Job> owner,
				std::shared_ptr<const Package> program
			)
				: Owner(std::move(owner)), Program(std::move(program)), Resources(device, api) {}

			bool Upload(SDL_GPUCommandBuffer *command) const {
				if (Uploads.empty() && Copies.empty()) return true;
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (!copy) return false;
				for (const auto &upload : Uploads)
					SDL_UploadToGPUBuffer(copy, &upload.Source, &upload.Target, false);
				for (const auto &buffer : Copies)
					SDL_CopyGPUBufferToBuffer(copy, &buffer.Source, &buffer.Target, buffer.Bytes, false);
				SDL_EndGPUCopyPass(copy);
				return true;
			}
			bool Compute(SDL_GPUCommandBuffer *command) const {
				for (const auto &dispatch : Dispatches) {
					auto *pass = SDL_BeginGPUComputePass(
						command, nullptr, 0, dispatch.Outputs.data(), dispatch.Shader.Writable
					);
					if (!pass) return false;
					SDL_BindGPUComputePipeline(pass, dispatch.Shader.Handle);
					if (dispatch.Shader.Readonly)
						SDL_BindGPUComputeStorageBuffers(
							pass, 0, dispatch.Inputs.data(), dispatch.Shader.Readonly
						);
					if (dispatch.Shader.Uniform)
						SDL_PushGPUComputeUniformData(
							command, 0, &dispatch.Uniforms, sizeof(dispatch.Uniforms)
						);
					SDL_DispatchGPUCompute(pass, dispatch.Groups[0], dispatch.Groups[1], dispatch.Groups[2]);
					SDL_EndGPUComputePass(pass);
				}
				return true;
			}
			bool Download(SDL_GPUCommandBuffer *command) const {
				if (Downloads.empty()) return true;
				auto *copy = SDL_BeginGPUCopyPass(command);
				if (!copy) return false;
				for (const auto &download : Downloads)
					SDL_DownloadFromGPUBuffer(copy, &download.Source, &download.Target);
				SDL_EndGPUCopyPass(copy);
				return true;
			}
		};

		GpuQueueBatch *ActiveBatch(SDL_GPUDevice *device) {
			if (!device) return nullptr;
			auto *batch = static_cast<GpuQueueBatch *>(SDL_GetPointerProperty(
				SDL_GetGPUDeviceProperties(device), GpuQueueBatch::DEVICE_PROPERTY, nullptr
			));
			return batch && batch->Active() ? batch : nullptr;
		}
		struct Mapping {
			SDL_GPUDevice *Device = nullptr;
			SDL_GPUTransferBuffer *Transfer = nullptr;
			void *Data = nullptr;
			Mapping(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer)
				: Device(device), Transfer(transfer),
				  Data(SDL_MapGPUTransferBuffer(device, transfer, false)) {}
			~Mapping() {
				if (Data) SDL_UnmapGPUTransferBuffer(Device, Transfer);
			}
		};
		uint32_t Word(const std::byte *bytes) {
			uint32_t value = 0;
			std::memcpy(&value, bytes, sizeof(value));
			return value;
		}
	}

	struct ComputeGraph::State {
		SDL_GPUDevice *Device = nullptr;
		std::thread::id Thread = std::this_thread::get_id();
		computegraph::RuntimeLimits Bounds{};
		computegraph::Queue Queue;
		std::shared_ptr<computegraph_detail::Package> Installed{};
		std::vector<std::shared_ptr<computegraph_detail::Job>> Jobs{};
		std::unique_ptr<computegraph_detail::Profiling> Profiling{};
		std::string Error{};
		uint64_t OutsideBytes = 0;
		uint64_t DeviceBytes = 0;
		SDL_GPUBuffer *Instances = nullptr;
		uint32_t InstanceCapacity = 0;
		uint32_t InstanceStride = 0;

		State(void *device, computegraph::Owner owner, computegraph::RuntimeLimits limits)
			: Device(static_cast<SDL_GPUDevice *>(device)), Bounds(computegraph_detail::Bounded(limits)),
			  Queue(std::move(owner), Bounds) {
			Jobs.reserve(computegraph_detail::MAXIMUM_JOBS);
		}
		void RequireThread() const {
			if (std::this_thread::get_id() == Thread) return;
			ENGINE_ERROR("compute graph used outside its device owner thread");
			std::abort();
		}
		computegraph::Status Refuse(
			computegraph::Diagnostic &diagnostic,
			computegraph::Status status,
			std::string_view subject = {},
			std::string_view error = {}
		) {
			diagnostic = {status, std::string(subject.substr(0, 128))};
			Error = error.empty() ? computegraph::Describe(status) : std::string(error.substr(0, 4096));
			core::Metrics::Count("render.compute.refusals", 1);
			return status;
		}
		computegraph::Status Backend(computegraph::Diagnostic &diagnostic) {
			return Refuse(diagnostic, computegraph::Status::DeviceFailure, {}, SDL_GetError());
		}
		const computegraph::Submission *Submission(uint64_t ticket) const {
			for (const auto &pending : Queue.Submissions())
				if (pending.Ticket == ticket) return &pending;
			return nullptr;
		}
		computegraph::Status
		Install(const ComputeGraphProgram &program, computegraph::Diagnostic &diagnostic);
		computegraph::Status Prepare(
			computegraph_detail::Job &job,
			const computegraph::Submission &submission,
			computegraph::Diagnostic &diagnostic
		);
		computegraph::Status Record(
			computegraph_detail::Job &job,
			const computegraph::Submission &submission,
			computegraph::Diagnostic &diagnostic
		);
		computegraph::Status RecordBatch(
			std::shared_ptr<computegraph_detail::Job> job,
			const computegraph::Submission &submission,
			GpuQueueBatch &batch,
			computegraph::Diagnostic &diagnostic
		);
		bool Publish(computegraph_detail::Job &job);
	};

	computegraph::Status
	ComputeGraph::State::Install(const ComputeGraphProgram &program, computegraph::Diagnostic &diagnostic) {
		ENGINE_PROFILE_CAT("compute graph compile", core::ProfileCategory::Render);
		if (!Device) return Refuse(diagnostic, computegraph::Status::DeviceUnavailable);
		if (!Jobs.empty()) return Refuse(diagnostic, computegraph::Status::NotReady);
		if (program.Kernels.size() != program.Definition.Kernels.size() || program.Kernels.size() > 64 ||
			program.ErrorBuffers.size() > program.Definition.Buffers.size())
			return Refuse(diagnostic, computegraph::Status::LimitExceeded);
		std::vector<computegraph_detail::Binary> binaries;
		std::vector<computegraph::KernelReflection> reflections;
		binaries.reserve(program.Kernels.size());
		reflections.reserve(program.Kernels.size());
		uint64_t codeBytes = 0;
		for (const auto &kernel : program.Definition.Kernels) {
			const auto source = computegraph_detail::Find(program.Kernels, kernel.Name);
			if (source == UINT32_MAX)
				return Refuse(diagnostic, computegraph::Status::UnknownKernel, kernel.Name);
			computegraph_detail::Binary binary;
			if (!computegraph_detail::Load(program.Kernels[source], binary, Error))
				return Refuse(diagnostic, computegraph::Status::ReflectionMismatch, kernel.Name, Error);
			const uint64_t bytes = binary.Words.size() * sizeof(uint32_t);
			if (bytes > computegraph_detail::MAXIMUM_CODE_BYTES - codeBytes)
				return Refuse(diagnostic, computegraph::Status::LimitExceeded, kernel.Name);
			codeBytes += bytes;
			reflections.push_back(binary.Reflection);
			binaries.push_back(std::move(binary));
		}
		auto package = std::make_shared<computegraph_detail::Package>();
		package->Device = Device;
		const auto status = computegraph::Compile(
			program.Definition,
			reflections,
			{},
			computegraph_detail::PortableLimits(),
			package->Plan,
			diagnostic
		);
		if (status != computegraph::Status::Ok) return Refuse(diagnostic, status, diagnostic.Subject);
		for (const auto &buffer : package->Plan.Definition.Buffers) {
			if (buffer.CountBuffer.empty()) continue;
			const auto count =
				computegraph_detail::Find(package->Plan.Definition.Buffers, buffer.CountBuffer);
			if (std::find(package->Counts.begin(), package->Counts.end(), count) == package->Counts.end())
				package->Counts.push_back(count);
		}
		for (const auto &name : program.ErrorBuffers) {
			const auto index = computegraph_detail::Find(package->Plan.Definition.Buffers, name);
			if (index == UINT32_MAX) return Refuse(diagnostic, computegraph::Status::UnknownBuffer, name);
			const auto &buffer = package->Plan.Definition.Buffers[index];
			if (buffer.Type != computegraph::ElementType::U32 || buffer.External ||
				!buffer.CountBuffer.empty() ||
				std::find(package->Counts.begin(), package->Counts.end(), index) != package->Counts.end() ||
				std::find(package->Errors.begin(), package->Errors.end(), index) != package->Errors.end())
				return Refuse(diagnostic, computegraph::Status::InvalidType, name);
			bool writer = false;
			for (const auto &pass : package->Plan.Passes)
				for (const auto &binding : pass.Bindings) {
					const auto &port = package->Plan.Definition.Kernels[pass.Kernel].Ports[binding.Port];
					if (binding.Buffer != index || !computegraph_detail::Writes(port.Permission)) continue;
					const auto capacity = package->Plan.Definition.Buffers[pass.DispatchBuffer].Capacity;
					const auto local = package->Plan.Definition.Kernels[pass.Kernel].Local[0];
					const auto groups = capacity / local + uint32_t(capacity % local != 0);
					if (buffer.LogicalCount < groups)
						return Refuse(diagnostic, computegraph::Status::InvalidCount, name);
					writer = true;
				}
			if (!writer) return Refuse(diagnostic, computegraph::Status::MissingProducer, name);
			package->Errors.push_back(index);
		}
		package->Pipelines.reserve(binaries.size());
		const auto binaryFormat = ShaderBinaryFor(Device);
		for (const auto &binary : binaries) {
			computegraph_detail::Pipeline pipeline;
			pipeline.Uniform = binary.Uniform;
			for (const auto &port : binary.Reflection.Ports) {
				if (computegraph_detail::Writes(port.Permission))
					pipeline.Writable++;
				else
					pipeline.Readonly++;
			}
			if (pipeline.Readonly > computegraph_detail::MAXIMUM_SLOTS ||
				pipeline.Writable > computegraph_detail::MAXIMUM_SLOTS)
				return Refuse(diagnostic, computegraph::Status::LimitExceeded, binary.Reflection.Name);
			SDL_GPUComputePipelineCreateInfo info{};
			info.code = reinterpret_cast<const Uint8 *>(binary.Words.data());
			info.code_size = binary.Words.size() * sizeof(uint32_t);
			info.entrypoint = binaryFormat.EntryPoint;
			info.format = binaryFormat.Format;
			info.num_readonly_storage_buffers = pipeline.Readonly;
			info.num_readwrite_storage_buffers = pipeline.Writable;
			info.num_uniform_buffers = binary.Uniform ? 1 : 0;
			info.threadcount_x = binary.Reflection.Local[0];
			info.threadcount_y = binary.Reflection.Local[1];
			info.threadcount_z = binary.Reflection.Local[2];
			pipeline.Handle = SDL_CreateGPUComputePipeline(Device, &info);
			if (!pipeline.Handle) return Backend(diagnostic);
			package->Pipelines.push_back(pipeline);
		}
		Installed = std::move(package);
		diagnostic = {};
		Error.clear();
		return computegraph::Status::Ok;
	}

	computegraph::Status ComputeGraph::State::Record(
		computegraph_detail::Job &job,
		const computegraph::Submission &submission,
		computegraph::Diagnostic &diagnostic
	) {
		const auto cancel = [](SDL_GPUCommandBuffer *command) { SDL_CancelGPUCommandBuffer(command); };
		std::unique_ptr<SDL_GPUCommandBuffer, decltype(cancel)> command(
			SDL_AcquireGPUCommandBuffer(Device), cancel
		);
		if (!command) return Backend(diagnostic);
		struct TimingRecording {
			computegraph_detail::Profiling *Profile;
			computegraph_detail::Job &Work;
			bool HandedOff = false;
			~TimingRecording() {
				if (Profile && !HandedOff) Profile->Queries.Abandon(Work.TimingSlot);
			}
		} timing{Profiling.get(), job};
		if (Profiling && !Profiling->UncertainSubmission) {
			job.TimingSlot = Profiling->Queries.Begin(command.get());
			if (job.TimingSlot != VulkanTimestamps::NO_SLOT)
				job.TimingFrom = Profiling->Queries.Mark(command.get());
		}
		const auto &plan = submission.Plan;
		const bool importsInstances = std::any_of(
			plan.Definition.Buffers.begin(), plan.Definition.Buffers.end(), [](const auto &buffer) {
				return buffer.Type == computegraph::ElementType::EngineInstance && buffer.Capacity != 0;
			}
		);
		if (!job.Uploads.empty() || importsInstances) {
			auto *copy = SDL_BeginGPUCopyPass(command.get());
			if (!copy) return Backend(diagnostic);
			for (const auto &upload : job.Uploads) {
				if (upload.Bytes == 0) continue;
				const SDL_GPUTransferBufferLocation source{job.Source, upload.Offset};
				const SDL_GPUBufferRegion target{job.Buffers[upload.Buffer], 0, upload.Bytes};
				SDL_UploadToGPUBuffer(copy, &source, &target, false);
			}
			for (size_t index = 0; index < plan.Definition.Buffers.size(); ++index) {
				const auto &buffer = plan.Definition.Buffers[index];
				if (buffer.Type != computegraph::ElementType::EngineInstance || buffer.Capacity == 0)
					continue;
				const SDL_GPUBufferLocation source{Instances, 0}, target{job.Buffers[index], 0};
				SDL_CopyGPUBufferToBuffer(
					copy, &source, &target, static_cast<uint32_t>(plan.Allocations[index].Bytes), false
				);
				if (Profiling) job.InstanceCopyBytes += plan.Allocations[index].Bytes;
				core::Metrics::Count("render.compute.instance_copy_bytes", plan.Allocations[index].Bytes);
			}
			SDL_EndGPUCopyPass(copy);
			core::Metrics::Count("render.compute.upload_bytes", job.UploadBytes);
		}
		for (const auto index : plan.Topology.Order) {
			const auto &work = plan.Passes[index];
			if (work.Work.Elements == 0) continue;
			const auto &kernel = plan.Definition.Kernels[work.Kernel];
			const auto &pipeline = Installed->Pipelines[work.Kernel];
			std::array<SDL_GPUBuffer *, computegraph_detail::MAXIMUM_SLOTS> inputs{};
			std::array<SDL_GPUStorageBufferReadWriteBinding, computegraph_detail::MAXIMUM_SLOTS> outputs{};
			computegraph_detail::Parameters parameters;
			parameters.Dispatch[0] = work.Work.Elements;
			for (const auto &binding : work.Bindings) {
				const auto &port = kernel.Ports[binding.Port];
				const auto &buffer = plan.Definition.Buffers[binding.Buffer];
				const auto row =
					(computegraph_detail::Writes(port.Permission) ? computegraph_detail::MAXIMUM_SLOTS : 0) +
					port.Binding;
				parameters.Buffers[row] = {
					buffer.LogicalCount, buffer.Capacity, buffer.Stride, buffer.CountBuffer.empty() ? 0u : 1u
				};
				if (computegraph_detail::Writes(port.Permission)) {
					outputs[port.Binding].buffer = job.Buffers[binding.Buffer];
					outputs[port.Binding].cycle = false;
				} else
					inputs[port.Binding] = job.Buffers[binding.Buffer];
			}
			// Distinct passes are the SDL storage hazard boundaries for the compiled resource edges.
			auto *pass =
				SDL_BeginGPUComputePass(command.get(), nullptr, 0, outputs.data(), pipeline.Writable);
			if (!pass) return Backend(diagnostic);
			SDL_BindGPUComputePipeline(pass, pipeline.Handle);
			if (pipeline.Readonly)
				SDL_BindGPUComputeStorageBuffers(pass, 0, inputs.data(), pipeline.Readonly);
			if (pipeline.Uniform)
				SDL_PushGPUComputeUniformData(command.get(), 0, &parameters, sizeof(parameters));
			SDL_DispatchGPUCompute(pass, work.Work.Groups[0], work.Work.Groups[1], work.Work.Groups[2]);
			if (Profiling) {
				++job.Dispatches;
				job.Workgroups += uint64_t(work.Work.Groups[0]) * work.Work.Groups[1] * work.Work.Groups[2];
			}
			SDL_EndGPUComputePass(pass);
			core::Metrics::Count("render.compute.dispatches", 1);
			core::Metrics::Count("render.compute.workgroups", work.Work.Groups[0]);
		}
		if (job.DownloadBytes != 0) {
			auto *copy = SDL_BeginGPUCopyPass(command.get());
			if (!copy) return Backend(diagnostic);
			for (const auto &download : job.Downloads) {
				if (download.Bytes == 0) continue;
				const SDL_GPUBufferRegion source{job.Buffers[download.Buffer], 0, download.Bytes};
				const SDL_GPUTransferBufferLocation target{job.Target, download.Offset};
				SDL_DownloadFromGPUBuffer(copy, &source, &target);
			}
			SDL_EndGPUCopyPass(copy);
			core::Metrics::Count("render.compute.readback_bytes", job.DownloadBytes);
		}
		core::Metrics::Count("render.compute.submit_calls", 1);
		if (Profiling && job.TimingSlot != VulkanTimestamps::NO_SLOT) {
			job.TimingTo = Profiling->Queries.Mark(command.get());
			Profiling->Queries.Submitted(job.TimingSlot);
		}
		timing.HandedOff = true;
		job.Fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command.release());
		if (!job.Fence) {
			if (Profiling && job.TimingSlot != VulkanTimestamps::NO_SLOT)
				Profiling->UncertainSubmission = true;
			return Backend(diagnostic);
		}
		return computegraph::Status::Ok;
	}

	computegraph::Status ComputeGraph::State::RecordBatch(
		std::shared_ptr<computegraph_detail::Job> job,
		const computegraph::Submission &submission,
		GpuQueueBatch &batch,
		computegraph::Diagnostic &diagnostic
	) {
		const auto &plan = submission.Plan;
		const bool importsInstances = std::any_of(
			plan.Definition.Buffers.begin(), plan.Definition.Buffers.end(), [](const auto &buffer) {
				return buffer.Type == computegraph::ElementType::EngineInstance && buffer.Capacity != 0;
			}
		);
		if (!batch.HasCapacity(importsInstances ? 4 : 3))
			return Refuse(diagnostic, computegraph::Status::LimitExceeded);

		struct Admission {
			GpuQueueBatch &Batch;
			bool Started = false;
			bool Complete = false;
			~Admission() {
				if (Started && !Complete) Batch.Refuse();
			}
		} admission{batch};
		try {
			auto packet =
				std::make_shared<computegraph_detail::QueuePacket>(Device, *batch.Api(), job, Installed);
			std::vector<SDL_GPUBuffer *> buffers;
			buffers.reserve(job->Buffers.size());
			for (auto *buffer : job->Buffers)
				buffers.push_back(
					static_cast<SDL_GPUBuffer *>(packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, buffer))
				);
			packet->Uploads.reserve(job->Uploads.size());
			for (const auto &upload : job->Uploads) {
				if (upload.Bytes == 0) continue;
				packet->Uploads.push_back(
					{{job->Source, upload.Offset}, {buffers[upload.Buffer], 0, upload.Bytes}}
				);
			}
			uint64_t instanceBytes = 0;
			if (importsInstances) {
				auto *instances = static_cast<SDL_GPUBuffer *>(
					packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_BUFFER, Instances)
				);
				for (size_t index = 0; index < plan.Definition.Buffers.size(); ++index) {
					const auto &buffer = plan.Definition.Buffers[index];
					if (buffer.Type != computegraph::ElementType::EngineInstance || buffer.Capacity == 0)
						continue;
					const auto bytes = static_cast<uint32_t>(plan.Allocations[index].Bytes);
					packet->Copies.push_back({{instances, 0}, {buffers[index], 0}, bytes});
					instanceBytes += bytes;
				}
			}
			uint64_t workgroups = 0;
			packet->Dispatches.reserve(plan.Topology.Order.size());
			for (const auto index : plan.Topology.Order) {
				const auto &work = plan.Passes[index];
				if (work.Work.Elements == 0) continue;
				const auto &kernel = plan.Definition.Kernels[work.Kernel];
				computegraph_detail::QueuePacket::DispatchCommand dispatch;
				dispatch.Shader = Installed->Pipelines[work.Kernel];
				dispatch.Shader.Handle = static_cast<SDL_GPUComputePipeline *>(
					packet->Resources.Freeze(ATOMIC_GPU_RESOURCE_COMPUTE_PIPELINE, dispatch.Shader.Handle)
				);
				dispatch.Uniforms.Dispatch[0] = work.Work.Elements;
				dispatch.Groups = work.Work.Groups;
				for (const auto &binding : work.Bindings) {
					const auto &port = kernel.Ports[binding.Port];
					const auto &buffer = plan.Definition.Buffers[binding.Buffer];
					const auto row =
						(computegraph_detail::Writes(port.Permission) ? computegraph_detail::MAXIMUM_SLOTS
																	  : 0) +
						port.Binding;
					dispatch.Uniforms.Buffers[row] = {
						buffer.LogicalCount,
						buffer.Capacity,
						buffer.Stride,
						buffer.CountBuffer.empty() ? 0u : 1u
					};
					if (computegraph_detail::Writes(port.Permission)) {
						dispatch.Outputs[port.Binding].buffer = buffers[binding.Buffer];
						dispatch.Outputs[port.Binding].cycle = false;
					} else
						dispatch.Inputs[port.Binding] = buffers[binding.Buffer];
				}
				workgroups += work.Work.Groups[0];
				packet->Dispatches.push_back(std::move(dispatch));
			}
			packet->Downloads.reserve(job->Downloads.size());
			for (const auto &download : job->Downloads) {
				if (download.Bytes == 0) continue;
				packet->Downloads.push_back(
					{{buffers[download.Buffer], 0, download.Bytes}, {job->Target, download.Offset}}
				);
			}
			job->InstanceCopyBytes = instanceBytes;
			job->Workgroups = workgroups;
			job->Dispatches = static_cast<uint32_t>(packet->Dispatches.size());
			const std::shared_ptr<const computegraph_detail::QueuePacket> ready = std::move(packet);

			std::array<uint32_t, 1> prefix{};
			std::span<const uint32_t> uploadDependencies;
			if (importsInstances) {
				admission.Started = true;
				prefix[0] = batch.Prefix();
				if (prefix[0] == GpuQueueBatch::INVALID)
					return Refuse(
						diagnostic,
						computegraph::Status::DeviceFailure,
						{},
						"resident instance import needs an ordered owner prefix"
					);
				uploadDependencies = prefix;
			}
			admission.Started = true;
			job->PendingSubmission = true;
			const auto upload = batch.Add(
				ATOMIC_GPU_QUEUE_TRANSFER,
				"compute graph upload",
				[ready](SDL_GPUCommandBuffer *command) { return ready->Upload(command); },
				uploadDependencies
			);
			if (upload == GpuQueueBatch::INVALID) return Backend(diagnostic);
			const std::array uploadEdge{upload};
			const auto compute = batch.Add(
				ATOMIC_GPU_QUEUE_COMPUTE,
				"compute graph dispatch",
				[ready](SDL_GPUCommandBuffer *command) { return ready->Compute(command); },
				uploadEdge
			);
			if (compute == GpuQueueBatch::INVALID) return Backend(diagnostic);
			const std::array computeEdge{compute};
			const auto download = batch.Add(
				ATOMIC_GPU_QUEUE_TRANSFER,
				"compute graph download",
				[ready](SDL_GPUCommandBuffer *command) { return ready->Download(command); },
				computeEdge,
				[ready](GpuTaskResult result) noexcept {
					auto &pending = *ready->Owner;
					pending.Fence = result.Fence;
					pending.Retirement = std::move(result.Retirement);
					pending.PendingSubmission = false;
					pending.BatchMetricsPending = true;
					if (!pending.Fence) {
						pending.Finished = pending.Retirement && pending.Retirement->Complete;
						pending.SubmissionFailed = true;
						return;
					}
				}
			);
			if (download == GpuQueueBatch::INVALID) return Backend(diagnostic);
			admission.Complete = true;
			return computegraph::Status::Ok;
		} catch (const std::bad_alloc &) {
			return Refuse(diagnostic, computegraph::Status::AllocationFailed);
		} catch (const std::runtime_error &error) {
			return Refuse(diagnostic, computegraph::Status::DeviceFailure, {}, error.what());
		}
	}

	bool ComputeGraph::State::Publish(computegraph_detail::Job &job) {
		const auto *submission = Submission(job.Ticket);
		if (!submission) return true;
		computegraph::Completion completion;
		completion.Ticket = job.Ticket;
		completion.Identity = submission->Data.Identity;
		std::unique_ptr<computegraph_detail::Mapping> mapped;
		if (job.DownloadBytes != 0) {
			mapped = std::make_unique<computegraph_detail::Mapping>(Device, job.Target);
			if (!mapped->Data) {
				completion.Code = computegraph::Status::DeviceFailure;
				Error = SDL_GetError();
			}
		}
		const auto *bytes = mapped ? static_cast<const std::byte *>(mapped->Data) : nullptr;
		const auto &plan = submission->Plan;
		std::vector<uint32_t> counts(plan.Definition.Buffers.size(), 0);
		if (completion.Code == computegraph::Status::Ok) {
			for (const auto &download : job.Downloads) {
				if (download.Kind == computegraph_detail::SegmentKind::Count)
					counts[download.Buffer] = computegraph_detail::Word(bytes + download.Offset);
				if (download.Kind == computegraph_detail::SegmentKind::Error)
					for (uint32_t offset = 0; offset < download.Bytes; offset += 4)
						if (computegraph_detail::Word(bytes + download.Offset + offset) != 0)
							completion.Code = computegraph::Status::OutputOverflow;
			}
			for (size_t index = 0; index < plan.Allocations.size(); ++index) {
				const auto count = plan.Allocations[index].CountBuffer;
				if (count != UINT32_MAX && counts[count] > plan.Definition.Buffers[index].Capacity)
					completion.Code = computegraph::Status::OutputOverflow;
			}
			for (const auto &output : submission->Data.Outputs) {
				const auto index = computegraph_detail::Find(plan.Definition.Buffers, output.Buffer);
				const auto &buffer = plan.Definition.Buffers[index];
				const auto countBuffer = plan.Allocations[index].CountBuffer;
				const auto count = countBuffer == UINT32_MAX ? buffer.LogicalCount : counts[countBuffer];
				if (count > output.MaximumElements) completion.Code = computegraph::Status::OutputOverflow;
			}
		}
		if (completion.Code == computegraph::Status::Ok) {
			for (const auto &output : submission->Data.Outputs) {
				const auto index = computegraph_detail::Find(plan.Definition.Buffers, output.Buffer);
				const auto &buffer = plan.Definition.Buffers[index];
				const auto countBuffer = plan.Allocations[index].CountBuffer;
				const auto count = countBuffer == UINT32_MAX ? buffer.LogicalCount : counts[countBuffer];
				computegraph::ResultBuffer result{.Buffer = buffer.Name, .Type = buffer.Type, .Count = count};
				result.Bytes.resize(static_cast<size_t>(count) * buffer.Stride);
				if (!result.Bytes.empty()) {
					const auto segment =
						std::find_if(job.Downloads.begin(), job.Downloads.end(), [&](const auto &download) {
							return download.Buffer == index &&
								   download.Kind == computegraph_detail::SegmentKind::Output;
						});
					std::memcpy(result.Bytes.data(), bytes + segment->Offset, result.Bytes.size());
				}
				completion.Outputs.push_back(std::move(result));
			}
		} else if (completion.Code == computegraph::Status::OutputOverflow) {
			Error = "compute output count, arithmetic or diagnostic exceeded its admitted contract";
		}
		computegraph::Diagnostic diagnostic;
		const auto status = Queue.Complete(std::move(completion), diagnostic);
		if (status == computegraph::Status::AllocationFailed) return false;
		if (status != computegraph::Status::Ok) {
			Error = computegraph::Describe(status);
			Queue.Cancel(job.Ticket);
		}
		return true;
	}

	ComputeGraph::ComputeGraph(void *device, computegraph::Owner owner, computegraph::RuntimeLimits limits)
		: Held(std::make_unique<State>(device, std::move(owner), limits)) {}
	ComputeGraph::~ComputeGraph() {
		Shutdown();
	}

	computegraph::Status
	ComputeGraph::Install(const ComputeGraphProgram &program, computegraph::Diagnostic &diagnostic) try {
		Held->RequireThread();
		return Held->Install(program, diagnostic);
	} catch (const std::bad_alloc &) {
		diagnostic.Subject.clear();
		diagnostic.Code = computegraph::Status::AllocationFailed;
		return diagnostic.Code;
	}

	computegraph::Status ComputeGraph::Submit(
		const computegraph::Request &request, uint64_t &ticketOut, computegraph::Diagnostic &diagnostic
	) {
		ENGINE_PROFILE_CAT("compute graph submit", core::ProfileCategory::Render);
		Held->RequireThread();
		uint64_t ticket = 0;
		try {
			if (!Held->Device) return Held->Refuse(diagnostic, computegraph::Status::DeviceUnavailable);
			if (!Held->Installed) return Held->Refuse(diagnostic, computegraph::Status::NotReady);
			if (Held->Jobs.size() >= Held->Bounds.MaximumPending ||
				Held->Installed->Plan.Bytes >
					computegraph_detail::MAXIMUM_DEVICE_BYTES - Held->OutsideBytes - Held->DeviceBytes)
				return Held->Refuse(diagnostic, computegraph::Status::LimitExceeded);
			auto status = Held->Queue.Submit(Held->Installed->Plan, request, ticket, diagnostic);
			if (status != computegraph::Status::Ok)
				return Held->Refuse(diagnostic, status, diagnostic.Subject);
			if (Held->Profiling) {
				auto *record = Held->Profiling->Find(0);
				if (!record) {
					Held->Queue.Cancel(ticket);
					return Held->Refuse(diagnostic, computegraph::Status::LimitExceeded);
				}
				record->Ticket = ticket;
			}
			auto job = std::make_shared<computegraph_detail::Job>();
			const auto *submission = Held->Submission(ticket);
			status = Held->Prepare(*job, *submission, diagnostic);
			if (status == computegraph::Status::Ok) {
				if (auto *batch = computegraph_detail::ActiveBatch(Held->Device))
					status = Held->RecordBatch(job, *submission, *batch, diagnostic);
				else
					status = Held->Record(*job, *submission, diagnostic);
			}
			if (status != computegraph::Status::Ok) {
				Held->Queue.Cancel(ticket);
				if (Held->Profiling) Held->Profiling->Erase(ticket);
				return status;
			}
			if (Held->Profiling) {
				job->Batched = job->PendingSubmission;
				auto &profile = *Held->Profiling;
				auto &record = *profile.Find(ticket);
				record.Dispatches = job->Dispatches;
				record.Workgroups = job->Workgroups;
				record.UploadBytes = job->UploadBytes;
				record.InstanceCopyBytes = job->InstanceCopyBytes;
				record.ReadbackBytes = job->DownloadBytes;
				++profile.Statistics.AcceptedJobs;
				if (job->Batched) ++profile.Statistics.BatchedJobs;
			}
			Held->DeviceBytes += job->DeviceBytes;
			Held->Jobs.push_back(std::move(job));
			ticketOut = ticket;
			diagnostic = {};
			Held->Error.clear();
			return computegraph::Status::Ok;
		} catch (const std::bad_alloc &) {
			if (ticket) Held->Queue.Cancel(ticket);
			if (ticket && Held->Profiling) Held->Profiling->Erase(ticket);
			diagnostic.Subject.clear();
			diagnostic.Code = computegraph::Status::AllocationFailed;
			return diagnostic.Code;
		}
	}

	void ComputeGraph::Poll() {
		ENGINE_PROFILE_CAT("compute graph poll", core::ProfileCategory::Render);
		Held->RequireThread();
		for (size_t index = 0; index < Held->Jobs.size();) {
			auto &job = *Held->Jobs[index];
			if (job.PendingSubmission) {
				++index;
				continue;
			}
			if (job.BatchMetricsPending) {
				job.BatchMetricsPending = false;
				try {
					if (job.SubmissionFailed) {
						core::Metrics::Count("render.compute.failed_batches", 1);
					} else {
						core::Metrics::Count("render.compute.instance_copy_bytes", job.InstanceCopyBytes);
						core::Metrics::Count("render.compute.upload_bytes", job.UploadBytes);
						core::Metrics::Count("render.compute.dispatches", job.Dispatches);
						core::Metrics::Count("render.compute.workgroups", job.Workgroups);
						core::Metrics::Count("render.compute.readback_bytes", job.DownloadBytes);
						core::Metrics::Count("render.compute.submit_calls", 3);
					}
				} catch (const std::bad_alloc &) {
					// Missing counters cannot change fence ownership or retry already reported amounts.
				}
			}
			if (job.SubmissionFailed) {
				try {
					if (!job.Canceled && !job.FailurePublished) {
						const auto *submission = Held->Submission(job.Ticket);
						if (submission) {
							computegraph::Completion completion;
							completion.Ticket = job.Ticket;
							completion.Identity = submission->Data.Identity;
							completion.Code = computegraph::Status::DeviceFailure;
							computegraph::Diagnostic diagnostic;
							if (Held->Queue.Complete(std::move(completion), diagnostic) ==
								computegraph::Status::AllocationFailed) {
								++index;
								continue;
							}
						}
						job.FailurePublished = true;
						Held->Error = "compute recording batch did not submit";
					}
				} catch (const std::bad_alloc &) {
					++index;
					continue;
				}
				if (!job.Fence && (!job.Retirement || !job.Retirement->Complete)) {
					++index;
					continue;
				}
				if (!job.Fence) job.Finished = true;
			}
			if (!job.Finished) {
				if (!SDL_QueryGPUFence(Held->Device, job.Fence)) {
					++index;
					continue;
				}
				if (!SDL_WaitForGPUFences(Held->Device, true, &job.Fence, 1)) {
					// Query success alone does not retire SDL's command references. Keep
					// the fence, resources and timestamp slot until that proof succeeds.
					job.SubmissionFailed = true;
					Held->Error = SDL_GetError();
					++index;
					continue;
				}
				SDL_ReleaseGPUFence(Held->Device, job.Fence);
				job.Fence = nullptr;
				job.Finished = true;
			}
			if (Held->Profiling && !job.TimingResolved) {
				auto &profile = *Held->Profiling;
				if (!job.Batched) {
					std::array<double, VulkanTimestamps::MARKS> times{};
					uint32_t count = 0;
					const bool available = job.TimingSlot != VulkanTimestamps::NO_SLOT &&
										   profile.Queries.Collect(job.TimingSlot, times.data(), count) &&
										   job.TimingFrom < job.TimingTo && job.TimingTo < count;
					if (available) {
						++profile.Statistics.TimedJobs;
						if (auto *record = profile.Find(job.Ticket)) {
							record->Available = true;
							record->CommandNanoseconds =
								VulkanTimestamps::Between(times.data(), job.TimingFrom, job.TimingTo);
						}
					} else {
						++profile.Statistics.DroppedTimings;
						profile.Queries.Abandon(job.TimingSlot);
					}
				}
				job.TimingResolved = true;
			}
			try {
				if (!job.Canceled && !job.FailurePublished && !Held->Publish(job)) {
					++index;
					continue;
				}
			} catch (const std::bad_alloc &) {
				++index;
				continue;
			}
			Held->DeviceBytes -= job.DeviceBytes;
			if (Held->Profiling) {
				++Held->Profiling->Statistics.RetiredJobs;
				if (job.Canceled) ++Held->Profiling->Statistics.CanceledRetirements;
			}
			Held->Jobs.erase(Held->Jobs.begin() + static_cast<std::ptrdiff_t>(index));
			core::Metrics::Count("render.compute.retired", 1);
		}
	}

	computegraph::Status ComputeGraph::Take(
		uint64_t ticket,
		computegraph::Completion &out,
		computegraph::Diagnostic &diagnostic,
		ComputeGraphTiming *timing
	) {
		Held->RequireThread();
		const auto status = Held->Queue.Take(ticket, out, diagnostic);
		if (status == computegraph::Status::Ok) {
			if (timing) {
				*timing = {.Ticket = ticket};
				if (Held->Profiling)
					if (const auto *record = Held->Profiling->Find(ticket)) *timing = *record;
			}
			if (Held->Profiling) Held->Profiling->Erase(ticket);
		}
		return status;
	}
	bool ComputeGraph::EnableGpuTimings(bool enabled) {
		Held->RequireThread();
		if (Held->Profiling && Held->Profiling->UncertainSubmission) return false;
		if (!Held->Device || !Held->Jobs.empty() || !Held->Queue.Submissions().empty() ||
			computegraph_detail::ActiveBatch(Held->Device))
			return false;
		if (bool(Held->Profiling) == enabled) return true;
		if (!enabled) {
			Held->Profiling.reset();
			return true;
		}
		const auto *api = static_cast<const AtomicGpuQueuesV1 *>(SDL_GetPointerProperty(
			SDL_GetGPUDeviceProperties(Held->Device), ATOMIC_GPU_QUEUES_DEVICE_PROPERTY, nullptr
		));
		if (!api || api->AbiVersion != ATOMIC_GPU_QUEUES_ABI_VERSION ||
			api->StructSize <
				offsetof(AtomicGpuQueuesV1, DeferDeviceCleanup) + sizeof(api->DeferDeviceCleanup) ||
			!api->DeferDeviceCleanup)
			return false;
		try {
			auto profile = std::make_unique<computegraph_detail::Profiling>();
			profile->Api = api;
			if (!profile->Queries.Probe(Held->Device, ATOMIC_GPU_QUEUE_GRAPHICS)) return false;
			Held->Profiling = std::move(profile);
			return true;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}
	ComputeGraphStatistics ComputeGraph::Statistics() const {
		Held->RequireThread();
		return Held->Profiling ? Held->Profiling->Statistics : ComputeGraphStatistics{};
	}
	bool ComputeGraph::Cancel(uint64_t ticket) {
		Held->RequireThread();
		const bool canceled = Held->Queue.Cancel(ticket);
		if (canceled && Held->Profiling) Held->Profiling->Erase(ticket);
		for (auto &job : Held->Jobs)
			if (job->Ticket == ticket) job->Canceled = true;
		return canceled;
	}
	void ComputeGraph::Reset(computegraph::Owner owner) {
		Held->RequireThread();
		for (auto &job : Held->Jobs)
			job->Canceled = true;
		Held->Queue.Reset(std::move(owner));
		if (Held->Profiling) Held->Profiling->Records = {};
	}
	bool ComputeGraph::SetOutsideDeviceBytes(uint64_t bytes) {
		Held->RequireThread();
		if (bytes > computegraph_detail::MAXIMUM_DEVICE_BYTES - Held->DeviceBytes) return false;
		Held->OutsideBytes = bytes;
		return true;
	}
	bool ComputeGraph::SetInstanceBuffer(void *buffer, uint32_t capacity, uint32_t stride) {
		Held->RequireThread();
		if (!Held->Jobs.empty() ||
			(buffer && (stride == 0 || uint64_t(capacity) * stride > 64u * 1024u * 1024u)))
			return false;
		Held->Instances = static_cast<SDL_GPUBuffer *>(buffer);
		Held->InstanceCapacity = capacity;
		Held->InstanceStride = stride;
		return true;
	}
	uint64_t ComputeGraph::ReservedDeviceBytes() const {
		Held->RequireThread();
		return Held->DeviceBytes;
	}
	size_t ComputeGraph::PendingGpuCount() const {
		Held->RequireThread();
		return Held->Jobs.size();
	}
	const std::string &ComputeGraph::LastError() const {
		Held->RequireThread();
		return Held->Error;
	}
	void ComputeGraph::Shutdown() {
		if (!Held) return;
		Held->RequireThread();
		const bool deferred = std::any_of(Held->Jobs.begin(), Held->Jobs.end(), [](const auto &job) {
			return job->PendingSubmission;
		});
		if (deferred)
			if (auto *batch = computegraph_detail::ActiveBatch(Held->Device)) batch->Abort();
		auto *batch =
			Held->Device
				? static_cast<GpuQueueBatch *>(SDL_GetPointerProperty(
					  SDL_GetGPUDeviceProperties(Held->Device), GpuQueueBatch::DEVICE_PROPERTY, nullptr
				  ))
				: nullptr;
		if (batch) (void)batch->RetireFailed();
		bool timingsRetired = !Held->Profiling || !Held->Profiling->UncertainSubmission;
		std::array<bool, computegraph_detail::MAXIMUM_JOBS> failedWaits{};
		for (size_t index = 0; index < Held->Jobs.size(); ++index) {
			auto &job = Held->Jobs[index];
			if (job->SubmissionFailed && !job->Fence && job->Retirement && !job->Retirement->Complete &&
				batch)
				batch->RetainUntilDeviceDestruction(job, job->DeviceBytes);
			if (job->Fence) {
				ENGINE_PROFILE_CAT("compute graph retirement wait", core::ProfileCategory::Idle);
				if (!SDL_WaitForGPUFences(Held->Device, true, &job->Fence, 1)) {
					timingsRetired = false;
					failedWaits[index] = true;
				}
			}
			Held->Queue.Cancel(job->Ticket);
		}
		if (!timingsRetired) {
			ENGINE_PROFILE_CAT("compute graph failed retirement drain", core::ProfileCategory::Idle);
			timingsRetired = SDL_WaitForGPUIdle(Held->Device);
			if (!timingsRetired && batch)
				for (size_t index = 0; index < Held->Jobs.size(); ++index)
					if (failedWaits[index])
						batch->RetainUntilDeviceDestruction(
							Held->Jobs[index], Held->Jobs[index]->DeviceBytes
						);
		}
		Held->Jobs.clear();
		Held->Queue.Reset({});
		Held->Installed.reset();
		if (Held->Profiling) {
			// Shutdown may block, but normal Poll never does. An unconfirmed device
			// drain leaves query pools owned by SDL's bounded destruction callback.
			if (timingsRetired) {
				Held->Profiling.reset();
			} else {
				auto *profile = Held->Profiling.release();
				if (!profile->Api->DeferDeviceCleanup(
						Held->Device, computegraph_detail::Profiling::FinishCleanup, profile
					)) {
					ENGINE_ERROR("could not register compute timestamp cleanup: {}", SDL_GetError());
					computegraph_detail::Profiling::FinishCleanup(profile, false);
				}
			}
		}
		Held->DeviceBytes = 0;
		Held->Instances = nullptr;
		Held->Device = nullptr;
	}

	computegraph::Status ComputeGraph::State::Prepare(
		computegraph_detail::Job &job,
		const computegraph::Submission &submission,
		computegraph::Diagnostic &diagnostic
	) {
		const auto &plan = submission.Plan;
		job.Device = Device;
		job.Ticket = submission.Ticket;
		job.DeviceBytes = plan.Bytes;
		job.Buffers.resize(plan.Definition.Buffers.size(), nullptr);
		for (const auto count : Installed->Counts)
			if (plan.Definition.Buffers[count].LogicalCount != 1)
				return Refuse(
					diagnostic, computegraph::Status::InvalidCount, plan.Definition.Buffers[count].Name
				);
		for (size_t inputIndex = 0; inputIndex < submission.Data.Inputs.size(); ++inputIndex) {
			const auto &input = submission.Data.Inputs[inputIndex];
			if (input.Bytes.empty()) continue;
			job.Uploads.push_back(
				{computegraph_detail::Find(plan.Definition.Buffers, input.Buffer),
				 static_cast<uint32_t>(job.UploadBytes),
				 static_cast<uint32_t>(input.Bytes.size()),
				 static_cast<uint32_t>(inputIndex)}
			);
			job.UploadBytes += input.Bytes.size();
		}
		const auto initialize = [&](uint32_t index) {
			const auto &buffer = plan.Definition.Buffers[index];
			if (buffer.External) return;
			const auto bytes = uint64_t(buffer.LogicalCount) * buffer.Stride;
			job.Uploads.push_back(
				{index, static_cast<uint32_t>(job.UploadBytes), static_cast<uint32_t>(bytes), UINT32_MAX}
			);
			job.UploadBytes += bytes;
		};
		for (const auto index : Installed->Counts)
			initialize(index);
		for (const auto index : Installed->Errors)
			initialize(index);
		const auto download = [&](uint32_t index, uint32_t bytes, computegraph_detail::SegmentKind kind) {
			job.Downloads.push_back({index, static_cast<uint32_t>(job.DownloadBytes), bytes, kind});
			job.DownloadBytes += bytes;
		};
		for (const auto index : Installed->Counts)
			download(index, 4, computegraph_detail::SegmentKind::Count);
		for (const auto index : Installed->Errors) {
			const auto &buffer = plan.Definition.Buffers[index];
			download(index, buffer.LogicalCount * buffer.Stride, computegraph_detail::SegmentKind::Error);
		}
		for (const auto &output : submission.Data.Outputs) {
			const auto index = computegraph_detail::Find(plan.Definition.Buffers, output.Buffer);
			download(
				index,
				output.MaximumElements * plan.Definition.Buffers[index].Stride,
				computegraph_detail::SegmentKind::Output
			);
		}
		uint64_t uploads = 0, downloads = 0;
		for (const auto &pending : Jobs) {
			uploads += pending->UploadBytes;
			downloads += pending->DownloadBytes;
		}
		if (job.UploadBytes > Bounds.MaximumUploadBytes - uploads ||
			job.DownloadBytes > Bounds.MaximumReadbackBytes - downloads)
			return Refuse(diagnostic, computegraph::Status::LimitExceeded);
		for (size_t index = 0; index < plan.Definition.Buffers.size(); ++index) {
			const auto &buffer = plan.Definition.Buffers[index];
			if (buffer.Type == computegraph::ElementType::EngineInstance &&
				(!Instances || buffer.Capacity > InstanceCapacity || buffer.Stride != InstanceStride))
				return Refuse(
					diagnostic,
					computegraph::Status::DeviceUnavailable,
					buffer.Name,
					"resident instance import is unavailable or incompatible"
				);
			SDL_GPUBufferCreateInfo info{};
			info.usage = 0;
			if (buffer.Permission != computegraph::Access::Write)
				info.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
			if (buffer.Permission != computegraph::Access::Read)
				info.usage |= SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
			info.size = static_cast<uint32_t>(plan.Allocations[index].Bytes);
			job.Buffers[index] = gpu::CreateBuffer(Device, &info);
			if (!job.Buffers[index]) return Backend(diagnostic);
		}
		if (job.UploadBytes != 0) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
			info.size = static_cast<uint32_t>(job.UploadBytes);
			job.Source = gpu::CreateTransferBuffer(Device, &info);
			if (!job.Source) return Backend(diagnostic);
			computegraph_detail::Mapping mapped(Device, job.Source);
			if (!mapped.Data) return Backend(diagnostic);
			for (const auto &upload : job.Uploads) {
				auto *target = static_cast<std::byte *>(mapped.Data) + upload.Offset;
				if (upload.Input == UINT32_MAX)
					std::memset(target, 0, upload.Bytes);
				else
					std::memcpy(target, submission.Data.Inputs[upload.Input].Bytes.data(), upload.Bytes);
			}
		}
		if (job.DownloadBytes != 0) {
			SDL_GPUTransferBufferCreateInfo info{};
			info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
			info.size = static_cast<uint32_t>(job.DownloadBytes);
			job.Target = gpu::CreateTransferBuffer(Device, &info);
			if (!job.Target) return Backend(diagnostic);
		}
		return computegraph::Status::Ok;
	}
}
