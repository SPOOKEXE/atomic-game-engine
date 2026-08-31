#include <engine/core/Profiling.hpp>
#include <engine/render/ShaderCompiler.hpp>

#include <algorithm>
#include <shaderc/shaderc.hpp>
#include <spirv-tools/optimizer.hpp>
#include <spirv_cross.hpp>
#include <utility>

namespace engine::render {

	namespace {
		constexpr uint32_t SPIRV_MAGIC = 0x07230203u;
		constexpr size_t SPIRV_HEADER_WORDS = 5;

		shaderc_shader_kind ToKind(ShaderStage stage) {
			switch (stage) {
			case ShaderStage::Vertex:
				return shaderc_glsl_vertex_shader;
			case ShaderStage::Fragment:
				return shaderc_glsl_fragment_shader;
			case ShaderStage::Compute:
				return shaderc_glsl_compute_shader;
			}
			return shaderc_glsl_fragment_shader;
		}

		uint32_t InstructionCount(std::span<const uint32_t> words) {
			if (words.size() < SPIRV_HEADER_WORDS || words[0] != SPIRV_MAGIC) {
				return 0;
			}
			uint32_t count = 0;
			for (size_t at = SPIRV_HEADER_WORDS; at < words.size();) {
				const uint32_t length = words[at] >> 16;
				if (length == 0 || at + length > words.size()) {
					return 0;
				}
				count++;
				at += length;
			}
			return count;
		}

		bool Arithmetic(spv::Op opcode) {
			switch (opcode) {
			case spv::OpSNegate:
			case spv::OpFNegate:
			case spv::OpIAdd:
			case spv::OpFAdd:
			case spv::OpISub:
			case spv::OpFSub:
			case spv::OpIMul:
			case spv::OpFMul:
			case spv::OpUDiv:
			case spv::OpSDiv:
			case spv::OpFDiv:
			case spv::OpUMod:
			case spv::OpSRem:
			case spv::OpSMod:
			case spv::OpFRem:
			case spv::OpFMod:
			case spv::OpVectorTimesScalar:
			case spv::OpMatrixTimesScalar:
			case spv::OpVectorTimesMatrix:
			case spv::OpMatrixTimesVector:
			case spv::OpMatrixTimesMatrix:
			case spv::OpDot:
				return true;
			default:
				return false;
			}
		}

		bool Texture(spv::Op opcode) {
			return opcode >= spv::OpImageSampleImplicitLod && opcode <= spv::OpImageQuerySamples;
		}

		bool Memory(spv::Op opcode) {
			switch (opcode) {
			case spv::OpLoad:
			case spv::OpStore:
			case spv::OpCopyMemory:
			case spv::OpCopyMemorySized:
			case spv::OpAccessChain:
			case spv::OpInBoundsAccessChain:
			case spv::OpPtrAccessChain:
			case spv::OpArrayLength:
				return true;
			default:
				return opcode >= spv::OpAtomicLoad && opcode <= spv::OpAtomicXor;
			}
		}

		bool ControlFlow(spv::Op opcode) {
			switch (opcode) {
			case spv::OpPhi:
			case spv::OpLoopMerge:
			case spv::OpSelectionMerge:
			case spv::OpLabel:
			case spv::OpBranch:
			case spv::OpBranchConditional:
			case spv::OpSwitch:
			case spv::OpKill:
			case spv::OpReturn:
			case spv::OpReturnValue:
			case spv::OpUnreachable:
			case spv::OpFunctionCall:
				return true;
			default:
				return false;
			}
		}

		ShaderStage StageOf(spv::ExecutionModel model) {
			switch (model) {
			case spv::ExecutionModelVertex:
				return ShaderStage::Vertex;
			case spv::ExecutionModelGLCompute:
				return ShaderStage::Compute;
			default:
				return ShaderStage::Fragment;
			}
		}

		void AddResources(
			spirv_cross::Compiler &compiler,
			const spirv_cross::SmallVector<spirv_cross::Resource> &resources,
			ShaderResourceKind kind,
			ShaderCapabilities &out
		) {
			for (const spirv_cross::Resource &resource : resources) {
				ShaderResourceEstimate estimate;
				estimate.Name = resource.name.empty() ? "id " + std::to_string(resource.id) : resource.name;
				estimate.Kind = kind;
				estimate.Set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
				estimate.Binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
				if (kind == ShaderResourceKind::UniformBuffer || kind == ShaderResourceKind::StorageBuffer ||
					kind == ShaderResourceKind::PushConstants) {
					try {
						estimate.MinimumBytes =
							compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id));
					} catch (const spirv_cross::CompilerError &) {
						estimate.MinimumBytes = 0;
					}
					out.DeclaredBufferBytes += estimate.MinimumBytes;
				}
				out.Resources.push_back(std::move(estimate));
			}
		}

		ShaderCapabilities Inspect(std::span<const uint32_t> words) {
			ShaderCapabilities result;
			result.SpirVBytes = words.size_bytes();
			result.Instructions = InstructionCount(words);
			if (result.Instructions == 0) {
				return result;
			}

			for (size_t at = SPIRV_HEADER_WORDS; at < words.size();) {
				const uint32_t length = words[at] >> 16;
				const spv::Op opcode = static_cast<spv::Op>(words[at] & 0xFFFFu);
				if (opcode == spv::OpCapability && length >= 2) {
					result.RequiredCapabilities.push_back(words[at + 1]);
				}
				result.ArithmeticInstructions += Arithmetic(opcode) ? 1u : 0u;
				result.TextureInstructions += Texture(opcode) ? 1u : 0u;
				result.MemoryInstructions += Memory(opcode) ? 1u : 0u;
				result.ControlFlowInstructions += ControlFlow(opcode) ? 1u : 0u;
				at += length;
			}

			try {
				spirv_cross::Compiler compiler(words.data(), words.size());
				result.Stage = StageOf(compiler.get_execution_model());
				const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
				result.Inputs = static_cast<uint32_t>(resources.stage_inputs.size());
				result.Outputs = static_cast<uint32_t>(resources.stage_outputs.size());
				AddResources(compiler, resources.sampled_images, ShaderResourceKind::SampledTexture, result);
				AddResources(
					compiler, resources.separate_images, ShaderResourceKind::SeparateTexture, result
				);
				AddResources(compiler, resources.separate_samplers, ShaderResourceKind::Sampler, result);
				AddResources(compiler, resources.storage_images, ShaderResourceKind::StorageTexture, result);
				AddResources(compiler, resources.uniform_buffers, ShaderResourceKind::UniformBuffer, result);
				AddResources(compiler, resources.storage_buffers, ShaderResourceKind::StorageBuffer, result);
				AddResources(
					compiler, resources.push_constant_buffers, ShaderResourceKind::PushConstants, result
				);
				if (result.Stage == ShaderStage::Compute) {
					result.WorkgroupX = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 0);
					result.WorkgroupY = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 1);
					result.WorkgroupZ = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, 2);
				}
			} catch (const spirv_cross::CompilerError &) {
				// shaderc produced and SPIRV-Tools validated the module. Reflection is
				// advisory, so an unsupported construct leaves the basic counts intact.
			}
			return result;
		}

		template <typename Register>
		bool Optimize(
			std::vector<uint32_t> &words,
			ShaderOptimizationKind kind,
			Register registerPasses,
			ShaderOptimizationStep &step,
			std::string &error
		) {
			step.Kind = kind;
			step.BeforeInstructions = InstructionCount(words);
			spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_0);
			std::string diagnostic;
			optimizer.SetMessageConsumer(
				[&](spv_message_level_t, const char *, const spv_position_t &, const char *message) {
					if (!diagnostic.empty()) {
						diagnostic += '\n';
					}
					diagnostic += message;
				}
			);
			registerPasses(optimizer);
			std::vector<uint32_t> optimized;
			if (!optimizer.Run(words.data(), words.size(), &optimized)) {
				error = diagnostic.empty() ? "SPIR-V optimization failed with no diagnostic" : diagnostic;
				return false;
			}
			step.Changed = optimized != words;
			step.AfterInstructions = InstructionCount(optimized);
			words = std::move(optimized);
			return true;
		}
	}

	struct ShaderCompiler::Impl {
		shaderc::Compiler Compiler;
		shaderc::CompileOptions Options;
		bool Optimise = false;
	};

	const char *Describe(ShaderStage stage) {
		switch (stage) {
		case ShaderStage::Vertex:
			return "vertex";
		case ShaderStage::Fragment:
			return "fragment";
		case ShaderStage::Compute:
			return "compute";
		}
		return "unknown";
	}

	const char *Describe(ShaderResourceKind kind) {
		switch (kind) {
		case ShaderResourceKind::SampledTexture:
			return "sampled texture";
		case ShaderResourceKind::SeparateTexture:
			return "texture";
		case ShaderResourceKind::Sampler:
			return "sampler";
		case ShaderResourceKind::StorageTexture:
			return "storage texture";
		case ShaderResourceKind::UniformBuffer:
			return "uniform buffer";
		case ShaderResourceKind::StorageBuffer:
			return "storage buffer";
		case ShaderResourceKind::PushConstants:
			return "push constants";
		}
		return "unknown";
	}

	std::string ShaderCapabilityName(uint32_t capability) {
		return spv::CapabilityToString(static_cast<spv::Capability>(capability));
	}

	const char *Describe(ShaderOptimizationKind kind) {
		switch (kind) {
		case ShaderOptimizationKind::ConstantFolding:
			return "constant folding";
		case ShaderOptimizationKind::CommonSubexpressionElimination:
			return "common-subexpression elimination";
		}
		return "unknown";
	}

	ShaderCapabilities InspectShaderCapabilities(std::span<const uint32_t> spirv) {
		return Inspect(spirv);
	}

	ShaderCompiler::ShaderCompiler() : State(std::make_unique<Impl>()) {
		// Vulkan 1.0 / SPIR-V 1.0 is the floor every backend SDL's GPU API
		// targets can accept. Raising it would compile shaders some drivers
		// then refuse, which surfaces as a pipeline that will not create
		// rather than as a compile error.
		State->Options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
		State->Options.SetSourceLanguage(shaderc_source_language_glsl);
	}

	// Out of line, and that is what makes the `unique_ptr` in the header legal:
	// this is the point at which `Impl` is complete and the deleter is
	// instantiated.
	ShaderCompiler::~ShaderCompiler() = default;

	void ShaderCompiler::SetOptimise(bool optimise) {
		State->Optimise = optimise;
		State->Options.SetOptimizationLevel(shaderc_optimization_level_zero);
	}

	ShaderCompilation
	ShaderCompiler::Compile(std::string_view source, ShaderStage stage, std::string_view name) {
		ENGINE_PROFILE_CAT("ShaderCompiler::Compile", core::ProfileCategory::Render);

		ShaderCompilation result;

		// Both take a length, so neither needs a null-terminated copy of what
		// may be a view into a larger buffer.
		const std::string label(name);
		const auto compiled = State->Compiler.CompileGlslToSpv(
			source.data(), source.size(), ToKind(stage), label.c_str(), "main", State->Options
		);

		result.Warnings = static_cast<uint32_t>(compiled.GetNumWarnings());

		if (compiled.GetCompilationStatus() != shaderc_compilation_status_success) {
			result.Failed = true;
			result.Error = compiled.GetErrorMessage();

			// A failure with no message would be indistinguishable from a
			// success to anyone checking only the string, which is exactly the
			// stub-versus-compiler confusion this class has to avoid.
			if (result.Error.empty()) {
				result.Error = "shader compilation failed with no diagnostic";
			}
			return result;
		}

		result.SpirV.assign(compiled.cbegin(), compiled.cend());
		if (State->Optimise) {
			ShaderOptimizationStep folding;
			if (!Optimize(
					result.SpirV,
					ShaderOptimizationKind::ConstantFolding,
					[](spvtools::Optimizer &optimizer) {
						optimizer.RegisterPass(spvtools::CreateFoldSpecConstantOpAndCompositePass());
						optimizer.RegisterPass(spvtools::CreateCCPPass());
						optimizer.RegisterPass(spvtools::CreateSimplificationPass());
						optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
					},
					folding,
					result.Error
				)) {
				result.Failed = true;
				result.SpirV.clear();
				return result;
			}
			result.Optimizations.push_back(folding);

			ShaderOptimizationStep cse;
			if (!Optimize(
					result.SpirV,
					ShaderOptimizationKind::CommonSubexpressionElimination,
					[](spvtools::Optimizer &optimizer) {
						optimizer.RegisterPass(spvtools::CreateLocalRedundancyEliminationPass());
						optimizer.RegisterPass(spvtools::CreateRedundancyEliminationPass());
						optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
						optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
					},
					cse,
					result.Error
				)) {
				result.Failed = true;
				result.SpirV.clear();
				return result;
			}
			result.Optimizations.push_back(cse);
		}
		result.Capabilities = InspectShaderCapabilities(result.SpirV);
		return result;
	}
}
