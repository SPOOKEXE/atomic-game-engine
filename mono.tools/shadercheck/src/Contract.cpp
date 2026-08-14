#include <algorithm>
#include <map>
#include <shadercheck/Contract.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace shadercheck {

	namespace {

		// Capabilities that survive translation to MSL.
		//
		// **An allowlist rather than a denylist, and that is the whole design.**
		// A shader that declares something new fails here and somebody decides,
		// which is the only order that works when the machine that would find
		// out otherwise does not exist. Adding a line is cheap; the cost of the
		// other arrangement is a shader that compiles everywhere and renders on
		// one platform.
		const std::unordered_set<uint32_t> &Allowed() {
			static const std::unordered_set<uint32_t> ALLOWED = {
				0,	// Matrix
				1,	// Shader
				9,	// Float16
				22, // Int16
				25, // ImageGatherExtended
				32, // ClipDistance
				34, // ImageCubeArray
				35, // SampleRateShading
				39, // Int8
				42, // MinLod
				43, // Sampled1D
				44, // Image1D
				45, // SampledCubeArray
				46, // SampledBuffer
				47, // ImageBuffer
				49, // StorageImageExtendedFormats
				50, // ImageQuery
				51, // DerivativeControl
				55, // StorageImageReadWithoutFormat
				56, // StorageImageWriteWithoutFormat
			};
			return ALLOWED;
		}

		// Why a particular refusal is a refusal, for the ones somebody is likely
		// to hit. Everything else gets the generic sentence, which is honest:
		// nobody has decided about it yet.
		std::string WhyRefused(uint32_t capability) {
			static const std::unordered_map<uint32_t, std::string_view> REASONS = {
				{2, "SDL's GPU API has no geometry stage and Metal has none either"},
				{3, "SDL's GPU API has no tessellation stage"},
				{10, "MSL has no double"},
				{11, "64-bit integers need Metal 2.2 and SPIRV-Cross emulates them partially"},
				{12, "Metal has no 64-bit atomics"},
				{27, "Metal cannot write to a multisampled storage image"},
				{33, "Metal has no cull distance"},
				{40, "SDL's GPU API has no subpass inputs"},
				{41, "Metal has no sparse residency"},
				{53, "Metal has no transform feedback"},
			};

			const auto found = REASONS.find(capability);
			if (found != REASONS.end()) {
				return std::string(found->second);
			}
			return "nobody has decided whether Metal can express it";
		}

		// The descriptor set SDL's SPIR-V contract puts a resource of this kind
		// in, for this stage. `-1` means the stage has no home for it.
		//
		// From `SDL_gpu.h`, `SDL_CreateGPUShader`: a vertex shader binds sampled
		// textures, storage textures and storage buffers in set 0 and uniform
		// buffers in set 1; a fragment shader uses 2 and 3. From
		// `SDL_CreateGPUComputePipeline`: a compute shader reads from set 0,
		// writes through set 1, and takes uniforms in set 2.
		//
		// **The compute split between 0 and 1 is by read-only against
		// read-write, and this does not check which.** That needs `NonWritable`
		// tracking through the struct members, and there is no compute shader in
		// the engine to check it against - a rule written for no caller is a rule
		// nobody has run. Both sets are accepted for storage resources, and this
		// comment is where the gap is recorded rather than in somebody's memory.
		int SetFor(Stage stage, ResourceKind kind, bool &eitherOfTwo) {
			eitherOfTwo = false;
			switch (stage) {
			case Stage::Vertex:
				return kind == ResourceKind::UniformBuffer ? 1 : 0;
			case Stage::Fragment:
				return kind == ResourceKind::UniformBuffer ? 3 : 2;
			case Stage::Compute:
				if (kind == ResourceKind::UniformBuffer) {
					return 2;
				}
				if (kind == ResourceKind::SampledTexture) {
					return 0;
				}
				eitherOfTwo = true;
				return 0;
			default:
				return -1;
			}
		}

		std::string Describe(const Resource &resource) {
			std::string text = std::string(KindName(resource.Kind)) + " '" + resource.Name + "'";
			if (resource.HasSet && resource.HasBinding) {
				text += " at set " + std::to_string(resource.Set) + " binding " +
						std::to_string(resource.Binding);
			}
			return text;
		}
	}

	std::vector<Finding> Check(const Module &module, Stage expected) {
		std::vector<Finding> findings;
		const auto report = [&findings](std::string message) {
			findings.push_back(Finding{std::move(message)});
		};

		if (!module.Parsed()) {
			report(module.Error);
			return findings;
		}

		if (module.EntryPointCount != 1) {
			report(
				"has " + std::to_string(module.EntryPointCount) +
				" entry points; SDL_GPUShaderCreateInfo names exactly one"
			);
			return findings;
		}
		if (module.EntryPointName != SPIRV_ENTRY_POINT) {
			report(
				"entry point is '" + module.EntryPointName + "'; the renderer asks for '" +
				std::string(SPIRV_ENTRY_POINT) + "' on this format"
			);
		}
		if (module.EntryStage == Stage::Unsupported) {
			report("runs at a stage SDL's GPU API does not have");
		} else if (module.EntryStage != expected) {
			report(
				"is a " + std::string(StageName(module.EntryStage)) + " shader; its name says " +
				std::string(StageName(expected))
			);
		}

		for (const uint32_t capability : module.Capabilities) {
			if (Allowed().count(capability) == 0) {
				report(
					"declares " + CapabilityName(capability) + " - " + WhyRefused(capability) +
					". If it can, add it to ALLOWED in Contract.cpp with the reason."
				);
			}
		}

		// Grouped by set so that contiguity and ordering are asked per set,
		// which is how SDL numbers them.
		std::map<uint32_t, std::vector<const Resource *>> bySet;

		for (const Resource &resource : module.Resources) {
			if (!resource.HasSet || !resource.HasBinding) {
				report(
					Describe(resource) + " has no explicit layout(set = ..., binding = ...); every backend "
										 "then picks a slot of its own"
				);
				continue;
			}

			bool eitherOfTwo = false;
			const int wanted = SetFor(module.EntryStage, resource.Kind, eitherOfTwo);
			if (wanted < 0) {
				continue;
			}
			const bool placed = eitherOfTwo ? resource.Set == 0 || resource.Set == 1
											: resource.Set == static_cast<uint32_t>(wanted);
			if (!placed) {
				report(
					Describe(resource) + " belongs in set " + std::to_string(wanted) + " for a " +
					std::string(StageName(module.EntryStage)) + " shader"
				);
			}

			bySet[resource.Set].push_back(&resource);
		}

		for (const auto &[set, resources] : bySet) {
			uint32_t expectedBinding = 0;
			ResourceKind previous = ResourceKind::SampledTexture;
			for (const Resource *resource : resources) {
				if (resource->Binding != expectedBinding) {
					// **Contiguity is not tidiness.** Every other shader format
					// numbers by counting: the nth texture is `[[texture(n)]]`
					// in MSL and `t[n]` in HLSL, so a gap in the SPIR-V bindings
					// shifts everything after it by one and binds each resource
					// to its neighbour.
					report(
						"set " + std::to_string(set) + " jumps from binding " +
						std::to_string(expectedBinding) + " to " + std::to_string(resource->Binding) +
						" at " + Describe(*resource) +
						"; every other format numbers these by counting, so a gap "
						"shifts everything after it"
					);
				}
				expectedBinding = resource->Binding + 1;

				if (resource->Kind < previous) {
					report(
						Describe(*resource) + " comes after a " + std::string(KindName(previous)) +
						"; a set is ordered sampled textures, storage textures, then storage buffers"
					);
				}
				previous = resource->Kind;
			}
		}

		return findings;
	}

	std::vector<uint32_t> MetalIndices(const std::vector<Resource> &resources) {
		std::vector<uint32_t> indices(resources.size(), 0);

		// Two independent spaces, each filled in the order SDL documents:
		// textures are sampled then storage, buffers are uniform then storage.
		// Within one kind the descriptor binding decides, and `Reflect` has
		// already sorted by set and binding.
		uint32_t texture = 0;
		uint32_t buffer = 0;

		const auto assign = [&](ResourceKind kind, uint32_t &counter) {
			for (size_t index = 0; index < resources.size(); ++index) {
				if (resources[index].Kind == kind) {
					indices[index] = counter++;
				}
			}
		};

		assign(ResourceKind::SampledTexture, texture);
		assign(ResourceKind::StorageTexture, texture);
		assign(ResourceKind::UniformBuffer, buffer);
		assign(ResourceKind::StorageBuffer, buffer);

		return indices;
	}
}
