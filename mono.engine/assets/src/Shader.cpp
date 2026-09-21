#include <engine/assets/Shader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace engine::assets {

	namespace {
		using core::ByteReader;
		using core::ByteWriter;

		bool Text(std::string_view text, bool empty = false) {
			return (empty || !text.empty()) && text.size() <= Shader::MAXIMUM_NAME &&
				   std::none_of(text.begin(), text.end(), [](unsigned char letter) {
					   return letter < 32 || letter == 127;
				   });
		}

		bool Named(std::string_view name, std::initializer_list<std::string_view> choices) {
			return std::find(choices.begin(), choices.end(), name) != choices.end();
		}

		template <typename T> bool Ordered(const std::vector<T> &rows, size_t maximum) {
			if (rows.size() > maximum) {
				return false;
			}
			std::string_view previous;
			for (const auto &row : rows) {
				if (!Text(row.Name) || (!previous.empty() && previous >= row.Name)) {
					return false;
				}
				previous = row.Name;
			}
			return true;
		}

		bool ValueType(std::string_view type) {
			return Named(type, {"bool",		"int",		"uint",		"float",	"int2",		"int3",
								"int4",		"uint2",	"uint3",	"uint4",	"float2",	"float3",
								"float4",	"float2x2", "float2x3", "float2x4", "float3x2", "float3x3",
								"float3x4", "float4x2", "float4x3", "float4x4", "rgb",		"rgba"});
		}

		uint32_t VectorWidth(std::string_view type) {
			if (type == "rgb") return 3;
			if (type == "rgba") return 4;
			return type.back() >= '2' && type.back() <= '4' ? static_cast<uint32_t>(type.back() - '0') : 1;
		}

		bool InterfaceValid(const ShaderVariant &variant) {
			if (!Named(variant.Stage, {"vertex", "fragment", "compute"}) ||
				!Ordered(variant.Resources, Shader::MAXIMUM_DECLARATIONS) ||
				!Ordered(variant.Parameters, Shader::MAXIMUM_DECLARATIONS) ||
				!Ordered(variant.Inputs, Shader::MAXIMUM_DECLARATIONS) ||
				!Ordered(variant.Outputs, Shader::MAXIMUM_DECLARATIONS) ||
				variant.RequiredCapabilities.size() > Shader::MAXIMUM_KEYS || variant.WorkgroupX == 0 ||
				variant.WorkgroupY == 0 || variant.WorkgroupZ == 0) {
				return false;
			}
			const uint64_t invocations = static_cast<uint64_t>(variant.WorkgroupX) * variant.WorkgroupY;
			if (invocations > std::numeric_limits<uint32_t>::max() / variant.WorkgroupZ ||
				(variant.Stage != "compute" &&
				 (variant.WorkgroupX != 1 || variant.WorkgroupY != 1 || variant.WorkgroupZ != 1))) {
				return false;
			}
			std::string_view previousCapability;
			for (const auto &capability : variant.RequiredCapabilities) {
				if (!Text(capability) || (!previousCapability.empty() && previousCapability >= capability)) {
					return false;
				}
				previousCapability = capability;
			}
			for (size_t index = 0; index < variant.Resources.size(); index++) {
				const ShaderResource &resource = variant.Resources[index];
				if (!Named(
						resource.Kind,
						{"sampled-texture",
						 "separate-texture",
						 "sampler",
						 "storage-texture",
						 "uniform-buffer",
						 "storage-buffer",
						 "push-constants"}
					) ||
					!Named(resource.Access, {"read", "write", "read-write"}) ||
					!Named(
						resource.Dimension,
						{"none", "1d", "2d", "3d", "cube", "1d-array", "2d-array", "cube-array", "buffer"}
					) ||
					!Text(resource.Format, true) ||
					(resource.DescriptorCount == 0) != resource.RuntimeArray ||
					!Named(resource.SampleType, {"none", "float", "int", "uint", "depth"})) {
					return false;
				}
				for (size_t prior = 0; prior < index; prior++) {
					const auto &other = variant.Resources[prior];
					if (resource.Kind == "push-constants" || other.Kind == "push-constants") {
						if (resource.Kind == other.Kind) {
							return false;
						}
						continue;
					}
					if (resource.Set == other.Set && resource.Binding == other.Binding) {
						return false;
					}
				}
			}
			for (size_t index = 0; index < variant.Parameters.size(); index++) {
				const ShaderParameter &parameter = variant.Parameters[index];
				const auto resource =
					std::find_if(variant.Resources.begin(), variant.Resources.end(), [&](const auto &row) {
						return row.Name == parameter.Resource;
					});
				if (!Text(parameter.Resource) || !ValueType(parameter.Type) || parameter.Bytes == 0 ||
					!std::has_single_bit(parameter.Alignment) ||
					parameter.Offset % parameter.Alignment != 0 || resource == variant.Resources.end() ||
					!Named(resource->Kind, {"uniform-buffer", "storage-buffer", "push-constants"}) ||
					static_cast<uint64_t>(parameter.Offset) + parameter.Bytes > resource->MinimumBytes) {
					return false;
				}
				if (parameter.ArrayCount == 0 || (parameter.ArrayCount > 1 && parameter.ArrayStride == 0)) {
					return false;
				}
				const bool matrix = parameter.Type.find('x') != std::string::npos;
				const uint32_t rows = VectorWidth(parameter.Type);
				const uint32_t columns = matrix ? static_cast<uint32_t>(parameter.Type[5] - '0') : 1;
				const uint32_t width = matrix && parameter.RowMajor ? columns : rows;
				const uint32_t vectors = parameter.RowMajor ? rows : columns;
				if ((matrix && parameter.MatrixStride < width * 4) ||
					(!matrix && (parameter.MatrixStride != 0 || parameter.RowMajor))) {
					return false;
				}
				const uint64_t elementBytes =
					matrix ? static_cast<uint64_t>(vectors - 1) * parameter.MatrixStride + width * 4
						   : width * 4;
				if ((parameter.ArrayCount > 1 && parameter.ArrayStride < elementBytes) ||
					static_cast<uint64_t>(parameter.ArrayCount - 1) * parameter.ArrayStride + elementBytes >
						parameter.Bytes) {
					return false;
				}
				for (size_t prior = 0; prior < index; prior++) {
					const auto &other = variant.Parameters[prior];
					if (parameter.Resource == other.Resource &&
						parameter.Offset < static_cast<uint64_t>(other.Offset) + other.Bytes &&
						other.Offset < static_cast<uint64_t>(parameter.Offset) + parameter.Bytes) {
						return false;
					}
				}
			}
			for (const auto *variables : {&variant.Inputs, &variant.Outputs}) {
				for (size_t index = 0; index < variables->size(); index++) {
					const auto &variable = (*variables)[index];
					if (!ValueType(variable.Type) || variable.Component > 3 || variable.LocationCount == 0 ||
						static_cast<uint64_t>(variable.Location) + variable.LocationCount >
							uint64_t{1} + std::numeric_limits<uint32_t>::max() ||
						!Named(variable.Interpolation, {"smooth", "flat", "noperspective"}) ||
						(variable.Centroid && variable.Sample)) {
						return false;
					}
					const bool matrix = variable.Type.find('x') != std::string::npos;
					const uint32_t columns = matrix ? static_cast<uint32_t>(variable.Type[5] - '0') : 1;
					if (variable.Component + VectorWidth(variable.Type) > 4 ||
						(matrix && (variable.Component != 0 || variable.LocationCount % columns != 0))) {
						return false;
					}
					for (size_t prior = 0; prior < index; prior++) {
						const auto &other = (*variables)[prior];
						if (other.Location <
								static_cast<uint64_t>(variable.Location) + variable.LocationCount &&
							variable.Location < static_cast<uint64_t>(other.Location) + other.LocationCount) {
							if (variable.Component < other.Component + VectorWidth(other.Type) &&
								other.Component < variable.Component + VectorWidth(variable.Type))
								return false;
						}
					}
				}
			}
			return true;
		}

		bool VariantValid(const ShaderVariant &variant) {
			if (!InterfaceValid(variant) || !Ordered(variant.Features, Shader::MAXIMUM_KEYS) ||
				!Ordered(variant.Specializations, Shader::MAXIMUM_KEYS) ||
				variant.Optimizations.size() > Shader::MAXIMUM_OPTIMIZATIONS || variant.Payloads.empty() ||
				variant.Payloads.size() > 2) {
				return false;
			}
			for (const auto &feature : variant.Features) {
				if (!Text(feature.Value)) {
					return false;
				}
			}
			for (size_t index = 0; index < variant.Specializations.size(); index++) {
				const auto &specialization = variant.Specializations[index];
				if (!Named(specialization.Type, {"bool", "int", "uint", "float"}) ||
					specialization.Value.size() != 4) {
					return false;
				}
				ByteReader value(specialization.Value);
				const uint32_t bits = value.ReadUInt32();
				if ((specialization.Type == "bool" && bits > 1) ||
					(specialization.Type == "float" && !std::isfinite(std::bit_cast<float>(bits)))) {
					return false;
				}
				for (size_t prior = 0; prior < index; prior++) {
					if (variant.Specializations[prior].ConstantId == specialization.ConstantId) {
						return false;
					}
				}
			}
			for (const auto &optimization : variant.Optimizations) {
				if (!Text(optimization.Name)) {
					return false;
				}
			}
			bool hasSpirv = false;
			std::string_view previousBackend;
			for (const auto &payload : variant.Payloads) {
				if (!Named(payload.Backend, {"msl", "spirv"}) || !Text(payload.EntryPoint) ||
					!Text(payload.Target) || payload.Bytes.empty() ||
					payload.Bytes.size() > Shader::MAXIMUM_PAYLOAD_BYTES ||
					previousBackend >= payload.Backend) {
					return false;
				}
				previousBackend = payload.Backend;
				if (payload.Backend == "spirv") {
					ByteReader words(payload.Bytes);
					if (payload.Bytes.size() < 20 || payload.Bytes.size() % 4 != 0 ||
						words.ReadUInt32() != 0x07230203u) {
						return false;
					}
					hasSpirv = true;
				} else if (std::find(payload.Bytes.begin(), payload.Bytes.end(), std::byte{0}) !=
						   payload.Bytes.end()) {
					return false;
				}
			}
			return hasSpirv;
		}

		// One bounded encoding walk serves writing and exact size accounting.
		struct Encoder {
			ByteWriter *Writer = nullptr;
			size_t Bytes = 0;
			bool Good = true;

			bool Add(size_t count) {
				if (!Good || count > Shader::MAXIMUM_BYTES - Bytes) {
					Good = false;
					return false;
				}
				Bytes += count;
				return true;
			}
			void U32(uint32_t number) {
				if (Add(4) && Writer) Writer->WriteUInt32(number);
			}
			void U64(uint64_t number) {
				if (Add(8) && Writer) Writer->WriteUInt64(number);
			}
			void Bool(bool flag) {
				if (Add(1) && Writer) Writer->WriteUInt8(flag ? 1 : 0);
			}
			void String(std::string_view text) {
				if (Add(4 + text.size()) && Writer) Writer->WriteString(text);
			}
			void Hash(const ContentHash &hash) {
				if (Add(ContentHash::BYTES) && Writer)
					Writer->WriteRaw(hash.Digest.data(), hash.Digest.size());
			}
			void Blob(std::span<const std::byte> bytes) {
				U32(static_cast<uint32_t>(bytes.size()));
				if (Add(bytes.size()) && Writer) Writer->WriteRaw(bytes.data(), bytes.size());
			}
		};

		void EncodeInterface(Encoder &encoder, const ShaderVariant &variant) {
			encoder.String(variant.Stage);
			encoder.U32(static_cast<uint32_t>(variant.Resources.size()));
			for (const auto &row : variant.Resources) {
				encoder.String(row.Name);
				encoder.String(row.Kind);
				encoder.U32(row.Set);
				encoder.U32(row.Binding);
				encoder.String(row.Access);
				encoder.String(row.Dimension);
				encoder.String(row.Format);
				encoder.U64(row.MinimumBytes);
				encoder.U32(row.DescriptorCount);
				encoder.Bool(row.RuntimeArray);
				encoder.String(row.SampleType);
			}
			encoder.U32(static_cast<uint32_t>(variant.Parameters.size()));
			for (const auto &row : variant.Parameters) {
				encoder.String(row.Name);
				encoder.String(row.Resource);
				encoder.String(row.Type);
				encoder.U32(row.Offset);
				encoder.U32(row.Bytes);
				encoder.U32(row.Alignment);
				encoder.U32(row.ArrayCount);
				encoder.U32(row.ArrayStride);
				encoder.U32(row.MatrixStride);
				encoder.Bool(row.RowMajor);
			}
			for (const auto *variables : {&variant.Inputs, &variant.Outputs}) {
				encoder.U32(static_cast<uint32_t>(variables->size()));
				for (const auto &row : *variables) {
					encoder.String(row.Name);
					encoder.String(row.Type);
					encoder.U32(row.Location);
					encoder.U32(row.Component);
					encoder.U32(row.LocationCount);
					encoder.String(row.Interpolation);
					encoder.Bool(row.Centroid);
					encoder.Bool(row.Sample);
				}
			}
			encoder.U32(static_cast<uint32_t>(variant.RequiredCapabilities.size()));
			for (const auto &capability : variant.RequiredCapabilities)
				encoder.String(capability);
			encoder.U32(variant.WorkgroupX);
			encoder.U32(variant.WorkgroupY);
			encoder.U32(variant.WorkgroupZ);
		}

		void Encode(Encoder &encoder, const ShaderData &data) {
			encoder.String(data.CompilerVersion);
			encoder.String(data.OptimizerVersion);
			encoder.String(data.TranslatorVersion);
			encoder.String(data.ShaderAbi);
			encoder.String(data.TargetEnvironment);
			encoder.String(data.SourceLanguage);
			encoder.String(data.CapabilityProfile);
			encoder.String(data.PolicyVersion);
			encoder.String(data.CookProfile);
			for (const auto *options :
				 {&data.CompilerOptions, &data.OptimizerOptions, &data.TranslatorOptions}) {
				encoder.U32(static_cast<uint32_t>(options->size()));
				for (const auto &option : *options) {
					encoder.String(option.Name);
					encoder.String(option.Value);
				}
			}
			encoder.U32(static_cast<uint32_t>(data.Dependencies.size()));
			for (const auto &row : data.Dependencies) {
				encoder.String(row.Name);
				encoder.Hash(row.Root);
			}
			encoder.U32(static_cast<uint32_t>(data.Variants.size()));
			for (const auto &variant : data.Variants) {
				encoder.String(variant.Name);
				encoder.U32(static_cast<uint32_t>(variant.Features.size()));
				for (const auto &row : variant.Features) {
					encoder.String(row.Name);
					encoder.String(row.Value);
				}
				encoder.U32(static_cast<uint32_t>(variant.Specializations.size()));
				for (const auto &row : variant.Specializations) {
					encoder.String(row.Name);
					encoder.String(row.Type);
					encoder.U32(row.ConstantId);
					encoder.Blob(row.Value);
				}
				EncodeInterface(encoder, variant);
				encoder.Hash(encoder.Writer ? Shader::InterfaceHash(variant) : ContentHash{});
				encoder.U32(variant.Instructions);
				encoder.U32(variant.ArithmeticInstructions);
				encoder.U32(variant.TextureInstructions);
				encoder.U32(variant.MemoryInstructions);
				encoder.U32(variant.ControlFlowInstructions);
				encoder.U32(static_cast<uint32_t>(variant.Optimizations.size()));
				for (const auto &row : variant.Optimizations) {
					encoder.String(row.Name);
					encoder.U32(row.BeforeInstructions);
					encoder.U32(row.AfterInstructions);
					encoder.Bool(row.Changed);
				}
				encoder.U32(static_cast<uint32_t>(variant.Payloads.size()));
				for (const auto &row : variant.Payloads) {
					encoder.String(row.Backend);
					encoder.String(row.EntryPoint);
					encoder.String(row.Target);
					encoder.Hash(encoder.Writer ? Hasher::Of(row.Bytes) : ContentHash{});
					encoder.Blob(row.Bytes);
				}
			}
		}

		bool ReadText(ByteReader &reader, std::string &out, bool empty = false) {
			const auto text = reader.ReadString();
			if (reader.Failed() || !Text(text, empty)) return false;
			out.assign(text);
			return true;
		}

		template <typename T, typename Read>
		bool ReadRows(ByteReader &reader, std::vector<T> &rows, size_t maximum, Read read) {
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > maximum || count > reader.Remaining() / 4) return false;
			rows.reserve(count);
			for (uint32_t index = 0; index < count; index++) {
				T row;
				if (!read(reader, row) || reader.Failed()) return false;
				rows.push_back(std::move(row));
			}
			return true;
		}

		bool ReadBlob(ByteReader &reader, std::vector<std::byte> &out, size_t maximum) {
			const uint32_t size = reader.ReadUInt32();
			if (reader.Failed() || size > maximum || size > reader.Remaining()) return false;
			const auto bytes = reader.ReadRawView(size);
			out.assign(bytes.begin(), bytes.end());
			return !reader.Failed();
		}

		bool ReadInterface(ByteReader &reader, ShaderVariant &variant) {
			if (!ReadText(reader, variant.Stage)) return false;
			if (!ReadRows(
					reader,
					variant.Resources,
					Shader::MAXIMUM_DECLARATIONS,
					[](ByteReader &input, ShaderResource &row) {
						if (!ReadText(input, row.Name) || !ReadText(input, row.Kind)) return false;
						row.Set = input.ReadUInt32();
						row.Binding = input.ReadUInt32();
						if (!ReadText(input, row.Access) || !ReadText(input, row.Dimension) ||
							!ReadText(input, row.Format, true))
							return false;
						row.MinimumBytes = input.ReadUInt64();
						row.DescriptorCount = input.ReadUInt32();
						const uint8_t runtime = input.ReadUInt8();
						row.RuntimeArray = runtime != 0;
						return runtime <= 1 && ReadText(input, row.SampleType) && !input.Failed();
					}
				))
				return false;
			if (!ReadRows(
					reader,
					variant.Parameters,
					Shader::MAXIMUM_DECLARATIONS,
					[](ByteReader &input, ShaderParameter &row) {
						if (!ReadText(input, row.Name) || !ReadText(input, row.Resource) ||
							!ReadText(input, row.Type))
							return false;
						row.Offset = input.ReadUInt32();
						row.Bytes = input.ReadUInt32();
						row.Alignment = input.ReadUInt32();
						row.ArrayCount = input.ReadUInt32();
						row.ArrayStride = input.ReadUInt32();
						row.MatrixStride = input.ReadUInt32();
						const uint8_t rowMajor = input.ReadUInt8();
						row.RowMajor = rowMajor != 0;
						return rowMajor <= 1 && !input.Failed();
					}
				))
				return false;
			for (auto *variables : {&variant.Inputs, &variant.Outputs}) {
				if (!ReadRows(
						reader,
						*variables,
						Shader::MAXIMUM_DECLARATIONS,
						[](ByteReader &input, ShaderInterfaceVariable &row) {
							if (!ReadText(input, row.Name) || !ReadText(input, row.Type)) return false;
							row.Location = input.ReadUInt32();
							row.Component = input.ReadUInt32();
							row.LocationCount = input.ReadUInt32();
							if (!ReadText(input, row.Interpolation)) return false;
							const uint8_t centroid = input.ReadUInt8();
							const uint8_t sample = input.ReadUInt8();
							row.Centroid = centroid != 0;
							row.Sample = sample != 0;
							return centroid <= 1 && sample <= 1 && !input.Failed();
						}
					))
					return false;
			}
			if (!ReadRows(
					reader,
					variant.RequiredCapabilities,
					Shader::MAXIMUM_KEYS,
					[](ByteReader &input, std::string &row) { return ReadText(input, row); }
				))
				return false;
			variant.WorkgroupX = reader.ReadUInt32();
			variant.WorkgroupY = reader.ReadUInt32();
			variant.WorkgroupZ = reader.ReadUInt32();
			ContentHash signature;
			return reader.ReadRaw(signature.Digest.data(), signature.Digest.size()) &&
				   InterfaceValid(variant) && Shader::InterfaceHash(variant) == signature;
		}

		bool ReadVariant(ByteReader &reader, ShaderVariant &variant) {
			if (!ReadText(reader, variant.Name)) return false;
			if (!ReadRows(
					reader,
					variant.Features,
					Shader::MAXIMUM_KEYS,
					[](ByteReader &input, ShaderFeature &row) {
						return ReadText(input, row.Name) && ReadText(input, row.Value);
					}
				))
				return false;
			if (!ReadRows(
					reader,
					variant.Specializations,
					Shader::MAXIMUM_KEYS,
					[](ByteReader &input, ShaderSpecialization &row) {
						if (!ReadText(input, row.Name) || !ReadText(input, row.Type)) return false;
						row.ConstantId = input.ReadUInt32();
						return ReadBlob(input, row.Value, 4);
					}
				))
				return false;
			if (!ReadInterface(reader, variant)) return false;
			variant.Instructions = reader.ReadUInt32();
			variant.ArithmeticInstructions = reader.ReadUInt32();
			variant.TextureInstructions = reader.ReadUInt32();
			variant.MemoryInstructions = reader.ReadUInt32();
			variant.ControlFlowInstructions = reader.ReadUInt32();
			if (!ReadRows(
					reader,
					variant.Optimizations,
					Shader::MAXIMUM_OPTIMIZATIONS,
					[](ByteReader &input, ShaderOptimization &row) {
						if (!ReadText(input, row.Name)) return false;
						row.BeforeInstructions = input.ReadUInt32();
						row.AfterInstructions = input.ReadUInt32();
						const uint8_t changed = input.ReadUInt8();
						row.Changed = changed != 0;
						return changed <= 1 && !input.Failed();
					}
				))
				return false;
			return ReadRows(reader, variant.Payloads, 2, [](ByteReader &input, ShaderPayload &row) {
				if (!ReadText(input, row.Backend) || !ReadText(input, row.EntryPoint) ||
					!ReadText(input, row.Target))
					return false;
				ContentHash digest;
				if (!input.ReadRaw(digest.Digest.data(), digest.Digest.size())) return false;
				return ReadBlob(input, row.Bytes, Shader::MAXIMUM_PAYLOAD_BYTES) &&
					   Hasher::Of(row.Bytes) == digest;
			});
		}
	}

	bool ShaderData::IsValid() const {
		if (!Text(CompilerVersion) || !Text(OptimizerVersion) || !Text(TranslatorVersion) ||
			!Text(ShaderAbi) || !Text(TargetEnvironment) || !Ordered(Dependencies, Shader::MAXIMUM_KEYS) ||
			Variants.empty() || !Ordered(Variants, Shader::MAXIMUM_VARIANTS))
			return false;
		for (const auto &dependency : Dependencies)
			if (dependency.Root.IsZero()) return false;
		if (!Text(SourceLanguage) || !Text(CapabilityProfile) || !Text(PolicyVersion) || !Text(CookProfile))
			return false;
		for (const auto *options : {&CompilerOptions, &OptimizerOptions, &TranslatorOptions}) {
			if (!Ordered(*options, Shader::MAXIMUM_KEYS)) return false;
			for (const auto &option : *options)
				if (!Text(option.Value)) return false;
		}
		for (size_t index = 0; index < Variants.size(); index++) {
			if (!VariantValid(Variants[index])) return false;
			const auto &variant = Variants[index];
			if (static_cast<uint64_t>(variant.ArithmeticInstructions) + variant.TextureInstructions +
					variant.MemoryInstructions + variant.ControlFlowInstructions >
				variant.Instructions)
				return false;
			for (size_t prior = 0; prior < index; prior++) {
				if (Variants[prior].Stage == Variants[index].Stage &&
					Variants[prior].Features == Variants[index].Features &&
					Variants[prior].Specializations == Variants[index].Specializations)
					return false;
			}
		}
		Encoder measured;
		Encode(measured, *this);
		return measured.Good && measured.Bytes <= Shader::MAXIMUM_BYTES - 10;
	}

	ContentHash Shader::InterfaceHash(const ShaderVariant &variant) {
		if (!InterfaceValid(variant)) return {};
		ByteWriter writer;
		Encoder encoder{&writer};
		encoder.String("atomic.shader.interface.v1");
		EncodeInterface(encoder, variant);
		return encoder.Good ? Hasher::Of(writer.Bytes()) : ContentHash{};
	}

	bool Shader::Write(core::ByteWriter &writer, const ShaderData &data) {
		if (!data.IsValid()) return false;
		ByteWriter body;
		Encoder encoder{&body};
		Encode(encoder, data);
		if (!encoder.Good) return false;
		writer.WriteUInt32(MAGIC);
		writer.WriteUInt16(VERSION);
		writer.WriteUInt32(static_cast<uint32_t>(body.Bytes().size()));
		writer.WriteRaw(body.Bytes().data(), body.Bytes().size());
		return true;
	}

	bool Shader::Read(core::ByteReader &reader, ShaderData &out) {
		const auto refuse = [&reader]() {
			reader.Fail();
			return false;
		};
		if (reader.ReadUInt32() != MAGIC || reader.ReadUInt16() != VERSION) return refuse();
		const uint32_t bodyBytes = reader.ReadUInt32();
		if (reader.Failed() || bodyBytes > MAXIMUM_BYTES - 10 || bodyBytes > reader.Remaining())
			return refuse();
		ByteReader body(reader.ReadRawView(bodyBytes));
		ShaderData parsed;
		if (!ReadText(body, parsed.CompilerVersion) || !ReadText(body, parsed.OptimizerVersion) ||
			!ReadText(body, parsed.TranslatorVersion) || !ReadText(body, parsed.ShaderAbi) ||
			!ReadText(body, parsed.TargetEnvironment))
			return refuse();
		if (!ReadText(body, parsed.SourceLanguage) || !ReadText(body, parsed.CapabilityProfile) ||
			!ReadText(body, parsed.PolicyVersion) || !ReadText(body, parsed.CookProfile))
			return refuse();
		for (auto *options : {&parsed.CompilerOptions, &parsed.OptimizerOptions, &parsed.TranslatorOptions}) {
			if (!ReadRows(body, *options, MAXIMUM_KEYS, [](ByteReader &input, ShaderFeature &row) {
					return ReadText(input, row.Name) && ReadText(input, row.Value);
				}))
				return refuse();
		}
		if (!ReadRows(body, parsed.Dependencies, MAXIMUM_KEYS, [](ByteReader &input, ShaderDependency &row) {
				return ReadText(input, row.Name) &&
					   input.ReadRaw(row.Root.Digest.data(), row.Root.Digest.size());
			}))
			return refuse();
		if (!ReadRows(body, parsed.Variants, MAXIMUM_VARIANTS, ReadVariant) || !body.AtEnd() ||
			!parsed.IsValid())
			return refuse();
		out = std::move(parsed);
		return true;
	}
}
