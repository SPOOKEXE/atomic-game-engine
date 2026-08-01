#include <engine/core/Profiling.hpp>
#include <engine/render/ShaderCompiler.hpp>

#include <shaderc/shaderc.hpp>

namespace engine::render {

	namespace {
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
	}

	struct ShaderCompiler::Impl {
		shaderc::Compiler Compiler;
		shaderc::CompileOptions Options;
	};

	ShaderCompiler::ShaderCompiler() : State(new Impl) {
		// Vulkan 1.0 / SPIR-V 1.0 is the floor every backend SDL's GPU API
		// targets can accept. Raising it would compile shaders some drivers
		// then refuse, which surfaces as a pipeline that will not create
		// rather than as a compile error.
		State->Options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
		State->Options.SetSourceLanguage(shaderc_source_language_glsl);
	}

	ShaderCompiler::~ShaderCompiler() {
		delete State;
	}

	void ShaderCompiler::SetOptimise(bool optimise) {
		State->Options.SetOptimizationLevel(
			optimise ? shaderc_optimization_level_performance : shaderc_optimization_level_zero
		);
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
		return result;
	}
}
