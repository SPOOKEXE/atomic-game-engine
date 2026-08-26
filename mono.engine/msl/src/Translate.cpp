#include <engine/core/Log.hpp>
#include <engine/msl/Translate.hpp>

#include <algorithm>
#include <spirv_msl.hpp>
#include <vector>

namespace engine::msl {

	namespace {

		// One resource, reduced to what decides its Metal index.
		struct Slot {
			uint32_t Set = 0;
			uint32_t Binding = 0;
			// Whether it is a combined image sampler, which takes an index in
			// the texture space and the matching one in the sampler space.
			bool Combined = false;
		};

		void Collect(
			const spirv_cross::Compiler &compiler,
			const spirv_cross::SmallVector<spirv_cross::Resource> &group,
			bool combined,
			std::vector<Slot> &out
		) {
			for (const spirv_cross::Resource &resource : group) {
				out.push_back(
					Slot{
						compiler.get_decoration(resource.id, spv::DecorationDescriptorSet),
						compiler.get_decoration(resource.id, spv::DecorationBinding),
						combined
					}
				);
			}
		}

		void SortBySlot(std::vector<Slot> &slots) {
			std::sort(slots.begin(), slots.end(), [](const Slot &left, const Slot &right) {
				return left.Set != right.Set ? left.Set < right.Set : left.Binding < right.Binding;
			});
		}

		// The Metal index every resource lands on, told to SPIRV-Cross rather
		// than left to it.
		//
		// From `SDL_gpu.h`, `SDL_CreateGPUShader`: `[[texture]]` is sampled
		// textures then storage textures, `[[sampler]]` takes the index of the
		// sampled texture it belongs to, and `[[buffer]]` is uniform buffers
		// then storage buffers. Within one of those, the descriptor set and then
		// the binding decide.
		void AssignBindings(spirv_cross::CompilerMSL &compiler) {
			const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
			const spv::ExecutionModel stage = compiler.get_execution_model();

			std::vector<Slot> textures;
			Collect(compiler, resources.sampled_images, true, textures);
			Collect(compiler, resources.separate_images, false, textures);
			SortBySlot(textures);

			std::vector<Slot> storageTextures;
			Collect(compiler, resources.storage_images, false, storageTextures);
			SortBySlot(storageTextures);
			textures.insert(textures.end(), storageTextures.begin(), storageTextures.end());

			std::vector<Slot> buffers;
			Collect(compiler, resources.uniform_buffers, false, buffers);
			SortBySlot(buffers);

			std::vector<Slot> storageBuffers;
			Collect(compiler, resources.storage_buffers, false, storageBuffers);
			SortBySlot(storageBuffers);
			buffers.insert(buffers.end(), storageBuffers.begin(), storageBuffers.end());

			// A sampler declared apart from its texture. Nothing in this engine
			// writes one - every shader samples through a combined `sampler2D` -
			// so this counter continues after the combined ones and has never
			// been exercised. Said here rather than assumed correct.
			std::vector<Slot> samplers;
			Collect(compiler, resources.separate_samplers, false, samplers);
			SortBySlot(samplers);

			// The first shader in this engine to reach the untested branch says
			// so, because the symptom otherwise is a sampler on the wrong index
			// on one platform and a correct picture on the other.
			if (!samplers.empty()) {
				ENGINE_WARN(
					"{} separate sampler(s): the sampler index path below has never run in this engine",
					samplers.size()
				);
			}

			uint32_t textureIndex = 0;
			uint32_t samplerIndex = 0;
			for (const Slot &slot : textures) {
				spirv_cross::MSLResourceBinding binding;
				binding.stage = stage;
				binding.desc_set = slot.Set;
				binding.binding = slot.Binding;
				binding.msl_texture = textureIndex++;
				if (slot.Combined) {
					binding.msl_sampler = samplerIndex++;
				}
				compiler.add_msl_resource_binding(binding);

				// The one fact this module produces, and the one nothing can
				// read back off a machine with no Metal device. A texture on the
				// wrong index samples another map on macOS and is right on
				// Linux, so the assignment is traced rather than inferred.
				ENGINE_TRACE(
					"set {} binding {} -> [[texture({})]]{}",
					slot.Set,
					slot.Binding,
					binding.msl_texture,
					slot.Combined ? " with its own sampler" : ""
				);
			}

			for (const Slot &slot : samplers) {
				spirv_cross::MSLResourceBinding binding;
				binding.stage = stage;
				binding.desc_set = slot.Set;
				binding.binding = slot.Binding;
				binding.msl_sampler = samplerIndex++;
				compiler.add_msl_resource_binding(binding);
				ENGINE_TRACE(
					"set {} binding {} -> [[sampler({})]]", slot.Set, slot.Binding, binding.msl_sampler
				);
			}

			uint32_t bufferIndex = 0;
			for (const Slot &slot : buffers) {
				spirv_cross::MSLResourceBinding binding;
				binding.stage = stage;
				binding.desc_set = slot.Set;
				binding.binding = slot.Binding;
				binding.msl_buffer = bufferIndex++;
				compiler.add_msl_resource_binding(binding);
				ENGINE_TRACE(
					"set {} binding {} -> [[buffer({})]]", slot.Set, slot.Binding, binding.msl_buffer
				);
			}

			// The counts are what a caller compares against `shadercheck`'s
			// independent derivation when the two disagree.
			ENGINE_DEBUG(
				"execution model {}: {} texture(s), {} sampler(s), {} buffer(s)",
				static_cast<int>(stage),
				textures.size(),
				samplerIndex,
				buffers.size()
			);
		}
	}

	Translation Translate(std::span<const uint32_t> spirv) {
		Translation translation;
		if (spirv.empty()) {
			translation.Failed = true;
			translation.Error = "no SPIR-V to translate";
			ENGINE_WARN("asked to translate an empty module");
			return translation;
		}

		try {
			spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());

			spirv_cross::CompilerMSL::Options options = compiler.get_msl_options();

			// **Discrete bindings rather than an argument buffer, which is the
			// default and is deliberate anyway.** SDL's Metal backend binds each
			// texture, sampler and buffer to its own index, so a shader that
			// expected its resources packed into one argument buffer would find
			// nothing bound at all.
			options.argument_buffers = false;

			// MSL 2.0, which is macOS 10.13 and iOS 11. Chosen rather than
			// derived: it is the floor SDL's Metal backend requires, and asking
			// for less would refuse constructs SPIRV-Cross emits for ordinary
			// SPIR-V while asking for more would refuse hardware SDL supports.
			options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 0);

			// One emitter for both Apple platforms. `platform` decides a handful
			// of availability spellings and iOS is the narrower of the two, so
			// an iOS build switches this rather than translating differently -
			// recorded because nothing here builds for iOS yet.
			options.platform = spirv_cross::CompilerMSL::Options::macOS;

			compiler.set_msl_options(options);
			AssignBindings(compiler);
			translation.Source = compiler.compile();
			ENGINE_DEBUG(
				"translated {} words of SPIR-V into {} bytes of MSL", spirv.size(), translation.Source.size()
			);
		} catch (const spirv_cross::CompilerError &failure) {
			// **The exception stops here and becomes a string.** One caller is a
			// build step that turns it into a failed build and the other is a
			// frame that has to keep running with a diagnostic for whoever wrote
			// the shader, and neither wants to write a `catch` of its own.
			translation.Failed = true;
			translation.Error = failure.what();
			translation.Source.clear();

			// `Debug` rather than `Warning`: one caller fails a build with this
			// and the other shows it to whoever wrote the shader, so the line
			// here is for the developer watching both, not a second report.
			ENGINE_DEBUG("SPIRV-Cross refused {} words: {}", spirv.size(), translation.Error);
		}

		return translation;
	}
}
