#include <algorithm>
#include <shadercheck/Spirv.hpp>
#include <unordered_map>

namespace shadercheck {

	namespace {

		// SPIR-V 1.x, from the specification's own headers. Only the opcodes a
		// resource declaration can appear in are named; everything else is
		// stepped over by its word count.
		constexpr uint32_t MAGIC = 0x07230203u;

		constexpr uint32_t OP_NAME = 5;
		constexpr uint32_t OP_ENTRY_POINT = 15;
		constexpr uint32_t OP_CAPABILITY = 17;
		constexpr uint32_t OP_TYPE_IMAGE = 25;
		constexpr uint32_t OP_TYPE_SAMPLED_IMAGE = 27;
		constexpr uint32_t OP_TYPE_ARRAY = 28;
		constexpr uint32_t OP_TYPE_RUNTIME_ARRAY = 29;
		constexpr uint32_t OP_TYPE_POINTER = 32;
		constexpr uint32_t OP_VARIABLE = 59;
		constexpr uint32_t OP_DECORATE = 71;

		constexpr uint32_t STORAGE_UNIFORM_CONSTANT = 0;
		constexpr uint32_t STORAGE_UNIFORM = 2;
		constexpr uint32_t STORAGE_STORAGE_BUFFER = 12;

		constexpr uint32_t DECORATION_BLOCK = 2;
		constexpr uint32_t DECORATION_BUFFER_BLOCK = 3;
		constexpr uint32_t DECORATION_BINDING = 33;
		constexpr uint32_t DECORATION_DESCRIPTOR_SET = 34;

		constexpr uint32_t EXECUTION_MODEL_VERTEX = 0;
		constexpr uint32_t EXECUTION_MODEL_FRAGMENT = 4;
		constexpr uint32_t EXECUTION_MODEL_COMPUTE = 5;

		// The `Sampled` operand of `OpTypeImage`: 1 is read through a sampler, 2
		// is a storage image. 0 means "either", which only a kernel produces, and
		// which is read here as the sampled case because a graphics module has no
		// other use for it.
		constexpr uint32_t IMAGE_SAMPLED_AS_STORAGE = 2;

		// The five-word header every module opens with.
		constexpr size_t HEADER_WORDS = 5;

		// A literal string in SPIR-V is four characters per word, NUL-terminated
		// and NUL-padded to the word.
		std::string ReadString(std::span<const uint32_t> words, size_t from) {
			std::string text;
			for (size_t index = from; index < words.size(); ++index) {
				const uint32_t word = words[index];
				for (int byte = 0; byte < 4; ++byte) {
					const char character = static_cast<char>((word >> (byte * 8)) & 0xFFu);
					if (character == '\0') {
						return text;
					}
					text.push_back(character);
				}
			}
			return text;
		}

		Stage StageOf(uint32_t executionModel) {
			switch (executionModel) {
			case EXECUTION_MODEL_VERTEX:
				return Stage::Vertex;
			case EXECUTION_MODEL_FRAGMENT:
				return Stage::Fragment;
			case EXECUTION_MODEL_COMPUTE:
				return Stage::Compute;
			default:
				return Stage::Unsupported;
			}
		}

		// The parse is two passes over the same words: types and decorations
		// first, because a variable names a type declared before it but is
		// decorated by instructions that may appear either side of it, then the
		// variables themselves.
		struct Tables {
			// Image type id to its `Sampled` operand.
			std::unordered_map<uint32_t, uint32_t> Images;
			// Pointer id to the pair it carries.
			std::unordered_map<uint32_t, uint32_t> PointerStorage;
			std::unordered_map<uint32_t, uint32_t> PointerPointee;
			// Array or runtime-array id to its element type.
			std::unordered_map<uint32_t, uint32_t> ArrayElement;
			std::unordered_map<uint32_t, uint32_t> SampledImageOf;
			std::unordered_map<uint32_t, std::string> Names;
			std::unordered_map<uint32_t, uint32_t> DescriptorSets;
			std::unordered_map<uint32_t, uint32_t> Bindings;
			// Ids carrying Block or BufferBlock, which is how a uniform buffer
			// and a storage buffer are told apart before SPIR-V 1.3.
			std::unordered_map<uint32_t, bool> BlockKind;
		};

		// Peel arrays until something concrete is left. An array of samplers is
		// one binding holding several, and it is the element that says what kind
		// of resource the binding is.
		uint32_t Peel(const Tables &tables, uint32_t type) {
			for (int depth = 0; depth < 8; ++depth) {
				const auto element = tables.ArrayElement.find(type);
				if (element == tables.ArrayElement.end()) {
					return type;
				}
				type = element->second;
			}
			return type;
		}

		bool Classify(const Tables &tables, uint32_t storageClass, uint32_t pointee, ResourceKind &kind) {
			const uint32_t type = Peel(tables, pointee);

			if (storageClass == STORAGE_UNIFORM_CONSTANT) {
				const auto sampled = tables.SampledImageOf.find(type);
				if (sampled != tables.SampledImageOf.end()) {
					kind = ResourceKind::SampledTexture;
					return true;
				}
				const auto image = tables.Images.find(type);
				if (image != tables.Images.end()) {
					kind = image->second == IMAGE_SAMPLED_AS_STORAGE ? ResourceKind::StorageTexture
																	 : ResourceKind::SampledTexture;
					return true;
				}
				// A bare `OpTypeSampler`, or something this reader does not
				// model. It is still a binding, and reporting it as a sampled
				// texture would be a guess; reporting nothing would hide it.
				return false;
			}

			if (storageClass == STORAGE_STORAGE_BUFFER) {
				kind = ResourceKind::StorageBuffer;
				return true;
			}

			if (storageClass == STORAGE_UNIFORM) {
				// `BufferBlock` on the struct is what makes a `Uniform` variable
				// a storage buffer in the pre-1.3 spelling glslc emits.
				const auto block = tables.BlockKind.find(type);
				const bool bufferBlock = block != tables.BlockKind.end() && block->second;
				kind = bufferBlock ? ResourceKind::StorageBuffer : ResourceKind::UniformBuffer;
				return true;
			}

			return false;
		}
	}

	Module Reflect(std::span<const uint32_t> words) {
		Module module;

		if (words.size() < HEADER_WORDS) {
			module.Error = "not SPIR-V: shorter than the five-word header";
			return module;
		}
		if (words[0] != MAGIC) {
			module.Error = "not SPIR-V: wrong magic number";
			return module;
		}
		module.Version = words[1];

		Tables tables;
		std::vector<std::pair<uint32_t, uint32_t>> variables; // result id, then its pointer type

		size_t at = HEADER_WORDS;
		while (at < words.size()) {
			const uint32_t wordCount = words[at] >> 16;
			const uint32_t opcode = words[at] & 0xFFFFu;
			if (wordCount == 0 || at + wordCount > words.size()) {
				module.Error = "truncated SPIR-V: an instruction runs past the end of the file";
				return module;
			}
			const std::span<const uint32_t> operands = words.subspan(at + 1, wordCount - 1);

			switch (opcode) {
			case OP_CAPABILITY:
				if (!operands.empty()) {
					module.Capabilities.push_back(operands[0]);
				}
				break;

			case OP_ENTRY_POINT:
				++module.EntryPointCount;
				if (module.EntryPointCount == 1 && operands.size() >= 3) {
					module.EntryStage = StageOf(operands[0]);
					module.EntryPointName = ReadString(operands, 2);
				}
				break;

			case OP_NAME:
				if (operands.size() >= 2) {
					tables.Names[operands[0]] = ReadString(operands, 1);
				}
				break;

			case OP_DECORATE:
				if (operands.size() >= 2) {
					const uint32_t target = operands[0];
					const uint32_t decoration = operands[1];
					if (decoration == DECORATION_DESCRIPTOR_SET && operands.size() >= 3) {
						tables.DescriptorSets[target] = operands[2];
					} else if (decoration == DECORATION_BINDING && operands.size() >= 3) {
						tables.Bindings[target] = operands[2];
					} else if (decoration == DECORATION_BLOCK) {
						tables.BlockKind[target] = false;
					} else if (decoration == DECORATION_BUFFER_BLOCK) {
						tables.BlockKind[target] = true;
					}
				}
				break;

			case OP_TYPE_IMAGE:
				// result, sampled type, Dim, Depth, Arrayed, MS, Sampled, Format.
				if (operands.size() >= 7) {
					tables.Images[operands[0]] = operands[6];
				}
				break;

			case OP_TYPE_SAMPLED_IMAGE:
				if (operands.size() >= 2) {
					tables.SampledImageOf[operands[0]] = operands[1];
				}
				break;

			case OP_TYPE_ARRAY:
			case OP_TYPE_RUNTIME_ARRAY:
				if (operands.size() >= 2) {
					tables.ArrayElement[operands[0]] = operands[1];
				}
				break;

			case OP_TYPE_POINTER:
				if (operands.size() >= 3) {
					tables.PointerStorage[operands[0]] = operands[1];
					tables.PointerPointee[operands[0]] = operands[2];
				}
				break;

			case OP_VARIABLE:
				// result type, result id, storage class.
				if (operands.size() >= 3) {
					variables.emplace_back(operands[1], operands[0]);
				}
				break;

			default:
				break;
			}

			at += wordCount;
		}

		for (const auto &[id, pointerType] : variables) {
			const auto storage = tables.PointerStorage.find(pointerType);
			const auto pointee = tables.PointerPointee.find(pointerType);
			if (storage == tables.PointerStorage.end() || pointee == tables.PointerPointee.end()) {
				continue;
			}

			ResourceKind kind = ResourceKind::UniformBuffer;
			if (!Classify(tables, storage->second, pointee->second, kind)) {
				continue;
			}

			Resource resource;
			resource.Kind = kind;
			const auto name = tables.Names.find(id);
			resource.Name = name != tables.Names.end() && !name->second.empty() ? name->second
																				: "id " + std::to_string(id);

			// The decorations may sit on the variable or, for a block, on the
			// struct type. glslc puts them on the variable; a hand-written
			// module need not.
			const auto set = tables.DescriptorSets.find(id);
			if (set != tables.DescriptorSets.end()) {
				resource.HasSet = true;
				resource.Set = set->second;
			}
			const auto binding = tables.Bindings.find(id);
			if (binding != tables.Bindings.end()) {
				resource.HasBinding = true;
				resource.Binding = binding->second;
			}

			module.Resources.push_back(std::move(resource));
		}

		std::sort(module.Resources.begin(), module.Resources.end(), [](const Resource &a, const Resource &b) {
			if (a.Set != b.Set) {
				return a.Set < b.Set;
			}
			return a.Binding < b.Binding;
		});

		return module;
	}

	Module ReflectBytes(std::span<const std::byte> bytes) {
		Module module;
		if (bytes.size() % 4 != 0) {
			module.Error = "not SPIR-V: the file is not a whole number of 32-bit words";
			return module;
		}

		std::vector<uint32_t> words(bytes.size() / 4);
		for (size_t index = 0; index < words.size(); ++index) {
			words[index] = static_cast<uint32_t>(bytes[index * 4 + 0]) |
						   static_cast<uint32_t>(bytes[index * 4 + 1]) << 8 |
						   static_cast<uint32_t>(bytes[index * 4 + 2]) << 16 |
						   static_cast<uint32_t>(bytes[index * 4 + 3]) << 24;
		}

		// **A module may be stored either way round and the magic number is what
		// says which.** Swapping is three lines; rejecting it would report "not
		// SPIR-V" for a file that is, which is the least useful error a reader
		// of binaries can give.
		const auto swap = [](uint32_t word) {
			return (word >> 24) | ((word >> 8) & 0x0000FF00u) | ((word << 8) & 0x00FF0000u) | (word << 24);
		};
		if (!words.empty() && words[0] == swap(MAGIC)) {
			for (uint32_t &word : words) {
				word = swap(word);
			}
		}

		return Reflect(words);
	}

	std::string_view StageName(Stage stage) {
		switch (stage) {
		case Stage::Vertex:
			return "vertex";
		case Stage::Fragment:
			return "fragment";
		case Stage::Compute:
			return "compute";
		default:
			return "unsupported";
		}
	}

	std::string_view KindName(ResourceKind kind) {
		switch (kind) {
		case ResourceKind::SampledTexture:
			return "sampled texture";
		case ResourceKind::StorageTexture:
			return "storage texture";
		case ResourceKind::StorageBuffer:
			return "storage buffer";
		default:
			return "uniform buffer";
		}
	}

	std::string CapabilityName(uint32_t capability) {
		// The ones a graphics shader can plausibly declare. A capability with no
		// name here is reported by number and fails the allowlist, which is the
		// intended outcome for anything nobody has thought about yet.
		static const std::unordered_map<uint32_t, std::string_view> NAMES = {
			{0, "Matrix"},
			{1, "Shader"},
			{2, "Geometry"},
			{3, "Tessellation"},
			{4, "Addresses"},
			{5, "Linkage"},
			{6, "Kernel"},
			{9, "Float16"},
			{10, "Float64"},
			{11, "Int64"},
			{12, "Int64Atomics"},
			{22, "Int16"},
			{23, "TessellationPointSize"},
			{24, "GeometryPointSize"},
			{25, "ImageGatherExtended"},
			{27, "StorageImageMultisample"},
			{32, "ClipDistance"},
			{33, "CullDistance"},
			{34, "ImageCubeArray"},
			{35, "SampleRateShading"},
			{39, "Int8"},
			{40, "InputAttachment"},
			{41, "SparseResidency"},
			{42, "MinLod"},
			{43, "Sampled1D"},
			{44, "Image1D"},
			{45, "SampledCubeArray"},
			{46, "SampledBuffer"},
			{47, "ImageBuffer"},
			{48, "ImageMSArray"},
			{49, "StorageImageExtendedFormats"},
			{50, "ImageQuery"},
			{51, "DerivativeControl"},
			{52, "InterpolationFunction"},
			{53, "TransformFeedback"},
			{54, "GeometryStreams"},
			{55, "StorageImageReadWithoutFormat"},
			{56, "StorageImageWriteWithoutFormat"},
			{57, "MultiViewport"},
		};

		const auto found = NAMES.find(capability);
		if (found != NAMES.end()) {
			return std::string(found->second);
		}
		return "capability " + std::to_string(capability);
	}
}
