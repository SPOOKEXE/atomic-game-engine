#pragma once

// Hand-built SPIR-V for the suites, one instruction at a time.
//
// **The inputs are written rather than compiled, and that is the point.** Every
// case worth checking is a shader somebody has not written yet - a resource with
// no binding, a gap in a descriptor set, a fragment shader in a vertex shader's
// sets - and compiling one of those needs GLSL that a compiler will accept and a
// compiler in the test's dependency list. Assembling the words directly gives
// the negative cases for the same cost as the positive one, and makes the suite
// hermetic: it runs in a preset with no shader compiler configured at all.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace shadercheck::testing {

	// Opcodes and enumerants the suites name, spelled here so a test reads as
	// the instruction it is building.
	enum : uint32_t {
		OpName = 5,
		OpEntryPoint = 15,
		OpCapability = 17,
		OpTypeImage = 25,
		OpTypeSampledImage = 27,
		OpTypeStruct = 30,
		OpTypePointer = 32,
		OpVariable = 59,
		OpDecorate = 71,
	};

	enum : uint32_t {
		CapabilityShader = 1,
		CapabilityGeometry = 2,
		CapabilityFloat64 = 10,
	};

	enum : uint32_t {
		ModelVertex = 0,
		ModelFragment = 4,
		ModelGeometry = 3,
	};

	enum : uint32_t {
		ClassUniformConstant = 0,
		ClassUniform = 2,
		ClassStorageBuffer = 12,
	};

	enum : uint32_t {
		DecorationBlock = 2,
		DecorationBinding = 33,
		DecorationDescriptorSet = 34,
	};

	// Four characters per word, NUL-terminated and NUL-padded, as the
	// specification stores a literal string.
	inline std::vector<uint32_t> Literal(std::string_view text) {
		std::vector<uint32_t> words((text.size() / 4) + 1, 0);
		for (size_t index = 0; index < text.size(); ++index) {
			words[index / 4] |= static_cast<uint32_t>(static_cast<unsigned char>(text[index]))
								<< ((index % 4) * 8);
		}
		return words;
	}

	// A module under construction. It opens with the five-word header every
	// module has; `Instruction` appends, and `Words` hands over what a reflector
	// would be given.
	class Builder {
	  public:
		Builder &Instruction(uint32_t opcode, std::initializer_list<uint32_t> operands) {
			return Instruction(opcode, std::vector<uint32_t>(operands));
		}

		Builder &Instruction(uint32_t opcode, const std::vector<uint32_t> &operands) {
			const uint32_t wordCount = static_cast<uint32_t>(operands.size() + 1);
			Stream.push_back((wordCount << 16) | opcode);
			Stream.insert(Stream.end(), operands.begin(), operands.end());
			return *this;
		}

		// An instruction whose last operand is a literal string.
		Builder &Text(uint32_t opcode, std::initializer_list<uint32_t> leading, std::string_view text) {
			std::vector<uint32_t> operands(leading);
			const std::vector<uint32_t> literal = Literal(text);
			operands.insert(operands.end(), literal.begin(), literal.end());
			return Instruction(opcode, operands);
		}

		// `OpEntryPoint <model> %1 "<name>"`, and nothing else a module needs to
		// be reflectable.
		Builder &Entry(uint32_t executionModel, std::string_view name) {
			return Text(OpEntryPoint, {executionModel, 1}, name);
		}

		// A combined image sampler at one set and binding - GLSL's `sampler2D`.
		// `id` must be unique in the module; the three type ids derived from it
		// are what keeps two of these from colliding.
		Builder &SampledTexture(uint32_t id, uint32_t set, uint32_t binding, std::string_view name) {
			const uint32_t image = id + 100;
			const uint32_t sampled = id + 200;
			const uint32_t pointer = id + 300;
			Text(OpName, {id}, name);
			Decorate(id, set, binding);
			// result, sampled type, Dim, Depth, Arrayed, MS, Sampled, Format.
			Instruction(OpTypeImage, {image, 2, 1, 0, 0, 0, 1, 0});
			Instruction(OpTypeSampledImage, {sampled, image});
			Instruction(OpTypePointer, {pointer, ClassUniformConstant, sampled});
			return Instruction(OpVariable, {pointer, id, ClassUniformConstant});
		}

		// A `Block`-decorated struct behind a `Uniform` variable - GLSL's
		// `layout(...) uniform Name { ... }`.
		Builder &UniformBuffer(uint32_t id, uint32_t set, uint32_t binding, std::string_view name) {
			const uint32_t block = id + 100;
			const uint32_t pointer = id + 300;
			Text(OpName, {id}, name);
			Decorate(id, set, binding);
			Instruction(OpDecorate, {block, DecorationBlock});
			Instruction(OpTypeStruct, {block, 2});
			Instruction(OpTypePointer, {pointer, ClassUniform, block});
			return Instruction(OpVariable, {pointer, id, ClassUniform});
		}

		// A `StorageBuffer`-class variable - GLSL's `buffer Name { ... }`.
		Builder &StorageBuffer(uint32_t id, uint32_t set, uint32_t binding, std::string_view name) {
			const uint32_t block = id + 100;
			const uint32_t pointer = id + 300;
			Text(OpName, {id}, name);
			Decorate(id, set, binding);
			Instruction(OpTypeStruct, {block, 2});
			Instruction(OpTypePointer, {pointer, ClassStorageBuffer, block});
			return Instruction(OpVariable, {pointer, id, ClassStorageBuffer});
		}

		// The same, with the two decorations left off. A shader whose GLSL says
		// `uniform Frame { ... } frame;` and no `layout`.
		Builder &UndecoratedUniformBuffer(uint32_t id, std::string_view name) {
			const uint32_t block = id + 100;
			const uint32_t pointer = id + 300;
			Text(OpName, {id}, name);
			Instruction(OpDecorate, {block, DecorationBlock});
			Instruction(OpTypeStruct, {block, 2});
			Instruction(OpTypePointer, {pointer, ClassUniform, block});
			return Instruction(OpVariable, {pointer, id, ClassUniform});
		}

		Builder &Decorate(uint32_t id, uint32_t set, uint32_t binding) {
			Instruction(OpDecorate, {id, DecorationDescriptorSet, set});
			return Instruction(OpDecorate, {id, DecorationBinding, binding});
		}

		const std::vector<uint32_t> &Words() const {
			return Stream;
		}

	  private:
		// Magic, version 1.0, generator, id bound, schema.
		std::vector<uint32_t> Stream = {0x07230203u, 0x00010000u, 0, 1024, 0};
	};

	// A well-formed fragment shader: one sampled texture in set 2, one uniform
	// buffer in set 3. `interface.frag` in miniature, and the baseline every
	// negative case in the contract suite is a single change away from.
	inline Builder GoodFragment() {
		Builder builder;
		builder.Instruction(OpCapability, {CapabilityShader});
		builder.Entry(ModelFragment, "main");
		builder.SampledTexture(10, 2, 0, "interfaceTexture");
		builder.UniformBuffer(20, 3, 0, "flipbook");
		return builder;
	}
}
