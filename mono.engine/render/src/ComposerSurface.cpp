#include <engine/core/Profiling.hpp>
#include <engine/render/ComposerSurface.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace engine::render::hlsl {
	namespace {
		const engine::imagegraph::Value *
		ValueOf(const engine::imagegraph::EvaluationSnapshot &snapshot, std::string_view name) {
			for (const auto &input : snapshot.Values())
				if (input.Port == name) return &input.Data;
			return nullptr;
		}
		const engine::imagegraph::Image *
		ImageOf(const engine::imagegraph::EvaluationSnapshot &snapshot, std::string_view name) {
			for (const auto &input : snapshot.Images())
				if (input.Port == name) return &input.Data;
			return nullptr;
		}
		const engine::imagegraph::Value *
		ValueOf(const engine::imagegraph::HostNodeInvocation &invocation, std::string_view name) {
			for (const auto &input : invocation.Inputs)
				if (input.Port == name) return &input.Data;
			return nullptr;
		}
		const engine::imagegraph::Image *
		ImageOf(const engine::imagegraph::HostNodeInvocation &invocation, std::string_view name) {
			for (const auto &input : invocation.Images)
				if (input.Port == name) return input.Data;
			return nullptr;
		}
		uint64_t RetainedBytesOf(const engine::imagegraph::EvaluationSnapshot &snapshot) {
			return snapshot.RetainedBytes();
		}
		uint64_t RetainedBytesOf(const engine::imagegraph::HostNodeInvocation &) {
			return 0;
		}
		uint64_t CaptureLimitOf(const engine::imagegraph::EvaluationSnapshot &) {
			return MAXIMUM_SURFACE_JOB_BYTES;
		}
		uint64_t CaptureLimitOf(const engine::imagegraph::HostNodeInvocation &invocation) {
			return std::min(MAXIMUM_SURFACE_JOB_BYTES, invocation.MaximumOperationBytes);
		}
		int64_t InterpolationOf(const engine::imagegraph::EvaluationSnapshot &snapshot) {
			return snapshot.InheritedInterpolation();
		}
		int64_t InterpolationOf(const engine::imagegraph::HostNodeInvocation &invocation) {
			return invocation.Interpolation;
		}
		std::optional<double> NumberOf(const engine::imagegraph::Value &value) {
			if (const auto *number = std::get_if<double>(&value)) return *number;
			if (const auto *number = std::get_if<int64_t>(&value)) return double(*number);
			if (const auto *number = std::get_if<engine::imagegraph::EnumValue>(&value)) return number->Value;
			return {};
		}
		std::optional<std::string> Numbers(
			const engine::imagegraph::Value *value,
			ArgumentKind kind,
			std::array<double, 16> &numbers,
			size_t &count
		) {
			if (!value) return "Composer argument value is missing";
			count = kind == ArgumentKind::Float || kind == ArgumentKind::Int ? 1
					: kind == ArgumentKind::Vec2							 ? 2
					: kind == ArgumentKind::Vec3							 ? 3
					: kind == ArgumentKind::Mat3							 ? 9
					: kind == ArgumentKind::Mat4							 ? 16
																			 : 4;
			if (count == 1) {
				const auto number = NumberOf(*value);
				if (!number) return "Composer scalar argument type mismatch";
				numbers[0] = *number;
				if (kind == ArgumentKind::Int) {
					if (const auto *integer = std::get_if<int64_t>(value))
						numbers[0] = double(std::bit_cast<int32_t>(uint32_t(*integer)));
					else {
						if (!std::isfinite(*number)) return "Composer integer source value is not finite";
						double wrapped = std::fmod(std::trunc(*number), 4294967296.0);
						if (wrapped < 0) wrapped += 4294967296.0;
						numbers[0] = wrapped >= 2147483648.0 ? wrapped - 4294967296.0 : wrapped;
					}
				}
			} else if (kind == ArgumentKind::Color) {
				if (const auto *color = std::get_if<engine::imagegraph::Colour>(value)) {
					numbers[0] = color->Red / 255.0;
					numbers[1] = color->Green / 255.0;
					numbers[2] = color->Blue / 255.0;
					numbers[3] = color->Alpha / 255.0;
				} else {
					uint32_t packed = 0;
					if (const auto *integer = std::get_if<int64_t>(value))
						packed = uint32_t(*integer);
					else {
						const auto number = NumberOf(*value);
						if (!number || !std::isfinite(*number) || *number < -9223372036854775808.0 ||
							*number >= 9223372036854775808.0)
							return "Composer packed color argument type mismatch";
						packed = uint32_t(int64_t(*number));
					}
					for (size_t channel = 0; channel < 4; ++channel)
						numbers[channel] = ((packed >> (8 * channel)) & 255) / 255.0;
				}
			} else if (const auto *array = std::get_if<engine::imagegraph::ArrayValue>(value)) {
				if (!array->Nested.empty() || !array->Items.empty() || array->Elements.size() != count)
					return "Composer vector or matrix requires its exact flat numeric "
						   "component count";
				for (size_t i = 0; i < count; ++i) {
					if (const auto *n = std::get_if<double>(&array->Elements[i]))
						numbers[i] = *n;
					else if (const auto *n = std::get_if<int64_t>(&array->Elements[i]))
						numbers[i] = double(*n);
					else
						return "Composer component type mismatch";
				}
			} else if (kind == ArgumentKind::Vec2 &&
					   std::holds_alternative<engine::imagegraph::Vector2>(*value)) {
				const auto &v = std::get<engine::imagegraph::Vector2>(*value);
				numbers[0] = v.X;
				numbers[1] = v.Y;
			} else if (kind == ArgumentKind::Vec3 &&
					   std::holds_alternative<engine::imagegraph::Vector3>(*value)) {
				const auto &v = std::get<engine::imagegraph::Vector3>(*value);
				numbers[0] = v.X;
				numbers[1] = v.Y;
				numbers[2] = v.Z;
			} else if (kind == ArgumentKind::Vec4 &&
					   std::holds_alternative<engine::imagegraph::Vector4>(*value)) {
				const auto &v = std::get<engine::imagegraph::Vector4>(*value);
				numbers[0] = v.X;
				numbers[1] = v.Y;
				numbers[2] = v.Z;
				numbers[3] = v.W;
			} else
				return "Composer argument requires the declared source components";
			return {};
		}
	} // namespace
	template <class Inputs>
	std::optional<std::string> CaptureDefinitionImpl(
		const engine::imagegraph::Node &node, const Inputs &snapshot, CapturedDefinition &output
	) try {
		if (node.Type != "pc.hlsl") return "Cooked Composer surface requires pc.hlsl";
		CapturedDefinition candidate;
		uint64_t bytes = 0;
		for (auto [port, target] : std::array<std::pair<std::string_view, std::string *>, 4>{
				 {{"vertex", &candidate.Vertex},
				  {"main", &candidate.Main},
				  {"global", &candidate.Global},
				  {"libraries", &candidate.Libraries}}
			 }) {
			const auto *value = ValueOf(snapshot, port);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			if (!text) return "Composer source socket requires one resolved text row: " + std::string(port);
			if (text->size() > MAXIMUM_SOURCE_BYTES - bytes) return "Composer source text exceeds budget";
			bytes += text->size();
			*target = *text;
		}
		std::array<bool, MAXIMUM_ARGUMENTS> seen{};
		for (const auto &input : node.DynamicInputs) {
			constexpr std::string_view prefix = "argument_name_";
			if (!input.Id.starts_with(prefix)) continue;
			const auto suffix = std::string_view(input.Id).substr(prefix.size());
			size_t index = 0;
			if (suffix.empty() || (suffix.size() > 1 && suffix.front() == '0'))
				return "Composer argument index is invalid";
			for (const auto digit : suffix) {
				if (digit < '0' || digit > '9' || index > MAXIMUM_ARGUMENTS / 10)
					return "Composer argument index exceeds budget";
				index = index * 10 + size_t(digit - '0');
			}
			if (index >= MAXIMUM_ARGUMENTS || seen[index])
				return "Composer argument index exceeds budget or repeats";
			seen[index] = true;
		}
		const size_t count = std::count(seen.begin(), seen.end(), true);
		candidate.Arguments.reserve(count);
		for (size_t index = 0; index < count; ++index) {
			if (!seen[index])
				return "Composer arguments must form an ordered complete declaration "
					   "sequence";
			const auto suffix = std::to_string(index);
			const auto *value = ValueOf(snapshot, "argument_name_" + suffix);
			const auto *name = value ? std::get_if<std::string>(value) : nullptr;
			const auto *type = ValueOf(snapshot, "argument_type_" + suffix);
			const auto number = type ? NumberOf(*type) : std::nullopt;
			if (!name || name->size() > 128 || !number || !std::isfinite(*number) ||
				std::trunc(*number) != *number || *number < 0 || *number > 8)
				return "Composer argument name or kind is invalid";
			candidate.Arguments.push_back({*name, ArgumentKind(uint8_t(*number))});
		}
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer source capture allocation failed";
	}
	std::optional<std::string> CaptureDefinition(
		const engine::imagegraph::Node &node,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		CapturedDefinition &output
	) {
		return CaptureDefinitionImpl(node, snapshot, output);
	}
	std::optional<std::string> CookNode(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::Node &node,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		std::span<const Library> libraries,
		std::string_view compilerIdentity,
		bool includeMsl,
		core::Name shaderAsset,
		engine::imagegraph::Document &outputDocument,
		assets::ShaderData &outputShader,
		std::string_view translatorIdentity,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE_CAT("composer node cook", core::ProfileCategory::Assets);
		if (!shaderAsset.IsValid() || shaderAsset.Text().size() > assets::Shader::MAXIMUM_NAME)
			return "Composer cooked asset name is invalid";
		const auto bytes = engine::imagegraph::DocumentRetainedPayloadBytes(document);
		if (!bytes || *bytes > engine::imagegraph::Limits::MaximumDocumentBytes)
			return "Composer authoring document copy exceeds budget";
		const auto target =
			std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &value) {
				return value.Id == node.Id && value.Type == node.Type;
			});
		if (target == document.Nodes.end()) return "Composer cook node does not belong to the document";
		const auto existing = std::find_if(
			target->SourceProperties.begin(), target->SourceProperties.end(), [](const auto &property) {
				return property.Port == COOKED_SELECTOR;
			}
		);
		if (existing == target->SourceProperties.end() &&
			target->SourceProperties.size() >= engine::imagegraph::Limits::MaximumPropertiesPerNode)
			return "Composer cook metadata property budget exceeded";
		const auto previousDocument = engine::imagegraph::DocumentRetainedPayloadBytes(outputDocument);
		const auto previousShader = assets::ShaderRetainedPayloadBytes(outputShader, maximumBytes);
		if (!previousDocument || !previousShader || *previousDocument > maximumBytes ||
			*previousShader > maximumBytes - *previousDocument)
			return "Composer previous cook outputs exceed operation budget";
		const uint64_t available = maximumBytes - *previousDocument - *previousShader;
		const uint64_t annotationBytes =
			2 * (target->SourceProperties.size() + 1) * sizeof(engine::imagegraph::AuthoredValue) +
			2 * (shaderAsset.Text().size() + COOKED_SELECTOR.size() + 2);
		if (*bytes > available || annotationBytes > available - *bytes)
			return "Composer native document candidate exceeds operation budget";
		const uint64_t documentReservation = *bytes + annotationBytes;
		uint64_t captureReservation =
			sizeof(CapturedDefinition) + MAXIMUM_ARGUMENTS * (sizeof(Argument) + 256);
		for (const auto port : {"vertex", "main", "global", "libraries"}) {
			const auto *value = ValueOf(snapshot, port);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			if (!text || text->size() > MAXIMUM_SOURCE_BYTES)
				return "Composer cook source capture is missing or oversized";
			captureReservation += 2 * (text->size() + 16);
		}
		if (captureReservation > available - documentReservation)
			return "Composer source capture exceeds operation budget";
		CapturedDefinition captured;
		if (const auto failure = CaptureDefinition(node, snapshot, captured)) return failure;
		uint64_t capturedBytes = sizeof(captured) + captured.Arguments.capacity() * sizeof(Argument);
		for (const auto *text : {&captured.Vertex, &captured.Main, &captured.Global, &captured.Libraries})
			capturedBytes += text->capacity() + 1;
		for (const auto &argument : captured.Arguments)
			capturedBytes += argument.Name.capacity() + 1;
		if (capturedBytes > available - documentReservation)
			return "Composer actual source capture exceeds operation budget";
		assets::ShaderData shader;
		if (const auto failure = CookArtifact(
				captured.View(),
				libraries,
				compilerIdentity,
				includeMsl,
				shader,
				translatorIdentity,
				available - documentReservation - capturedBytes
			))
			return failure;
		engine::imagegraph::Document candidate = document;
		auto &copy = candidate.Nodes[size_t(target - document.Nodes.begin())];
		std::erase_if(copy.SourceProperties, [](const auto &property) {
			return property.Port == COOKED_SELECTOR;
		});
		copy.SourceProperties.push_back({std::string(COOKED_SELECTOR), std::string(shaderAsset.Text())});
		candidate.FormatVersion = std::max(candidate.FormatVersion, uint32_t(9));
		const auto candidateBytes = engine::imagegraph::DocumentRetainedPayloadBytes(candidate);
		const auto shaderBytes = assets::ShaderRetainedPayloadBytes(shader, available);
		if (!candidateBytes || !shaderBytes || *candidateBytes > available - capturedBytes ||
			*shaderBytes > available - capturedBytes - *candidateBytes)
			return "Composer actual cook candidates exceed operation budget";
		outputDocument = std::move(candidate);
		outputShader = std::move(shader);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer native cook transaction allocation failed";
	}
	uint64_t SurfaceSourceBytes(const SurfaceRequest &request) {
		const auto pair = CookedPairRetainedBytes(request.Pair);
		if (!pair || *pair > UINT64_MAX - sizeof(request)) return UINT64_MAX;
		uint64_t bytes = sizeof(request) - sizeof(request.Pair) + *pair;
		const auto add = [&](uint64_t amount) {
			if (amount > UINT64_MAX - bytes) return false;
			bytes += amount;
			return true;
		};
		if (request.Textures.capacity() > (UINT64_MAX - bytes) / sizeof(engine::imagegraph::Image) ||
			!add(request.Textures.capacity() * sizeof(engine::imagegraph::Image)) ||
			!add(request.Uniforms.capacity()))
			return UINT64_MAX;
		for (const auto &texture : request.Textures)
			if (!add(texture.Pixels.capacity())) return UINT64_MAX;
		return bytes;
	}
	uint64_t SurfaceScratchBytes(const SurfaceRequest &request) {
		if (request.Textures.empty()) return UINT64_MAX;
		const auto &base = request.Textures[0];
		uint64_t bytes = uint64_t(base.Width) * base.Height * 4;
		if (request.Format == assets::TextureFormat::RGBA8) bytes *= 5;
		for (const auto &image : request.Textures) {
			const auto format = engine::imagegraph::DescribeSurfaceFormat(image.Format);
			if (!format) return UINT64_MAX;
			const uint64_t stride = image.Format == engine::imagegraph::SurfaceFormat::RGBA4Unorm ||
											image.Format == engine::imagegraph::SurfaceFormat::R8Unorm
										? 4
										: format->BytesPerPixel;
			const uint64_t upload = uint64_t(image.Width) * image.Height * stride;
			if (upload > (UINT64_MAX - bytes) / 2) return UINT64_MAX;
			bytes += upload * 2;
		}
		return bytes <= UINT64_MAX - 160 ? bytes + 160 : UINT64_MAX;
	}
	std::array<float, 48> VertexMatrices(const CookedPair &pair, uint32_t width, uint32_t height) {
		std::array<float, 48> matrices{};
		for (size_t matrix = 0; matrix < 3; ++matrix) {
			const auto put = [&](size_t row, size_t column, float value) {
				matrices[matrix * 16 + (pair.VertexRowMajor ? row * 4 + column : column * 4 + row)] = value;
			};
			put(0, 0, matrix < 2 ? float(width) : 2.f);
			put(1, 1, matrix < 2 ? float(height) : -2.f);
			put(2, 2, 1);
			put(3, 3, 1);
			if (matrix == 2) {
				put(0, 3, -1);
				put(1, 3, 1);
			}
		}
		return matrices;
	}
	std::optional<std::string> ValidateSurfaceRequest(const SurfaceRequest &request) {
		if (!request.Shader.IsValid() || request.ShaderRevision == 0 ||
			request.Pair.DefinitionFingerprint.IsZero() ||
			request.Textures.size() != request.Pair.SpirV.SamplerCount || request.Textures.empty() ||
			request.Uniforms.size() != request.Pair.SpirV.UniformBytes ||
			request.Pair.PayloadBytes !=
				(request.Pair.SpirV.Vertex.SpirV.size() + request.Pair.SpirV.Fragment.SpirV.size()) * 4ull +
					request.Pair.VertexMsl.size() + request.Pair.FragmentMsl.size() ||
			(request.Format != assets::TextureFormat::RGBA8 &&
			 request.Format != assets::TextureFormat::RGBA8_LINEAR))
			return "Composer surface request is invalid";
		for (const auto &texture : request.Textures)
			if (!engine::imagegraph::ValidSurfaceLayout(texture, 4096, MAXIMUM_SURFACE_JOB_BYTES) ||
				!engine::imagegraph::FiniteSurfaceSamples(texture))
				return "Composer surface texture is invalid";
		if (SurfaceSourceBytes(request) > MAXIMUM_SURFACE_JOB_BYTES ||
			SurfaceScratchBytes(request) > 128ull * 1024 * 1024)
			return "Composer surface job exceeds byte budget";
		return Admit(request.Pair.SpirV);
	}
	template <class Inputs>
	std::optional<std::string> BuildSurfaceRequestImpl(
		const engine::imagegraph::Node &node,
		const Inputs &snapshot,
		const CookedComposerLibrary &library,
		core::Name owner,
		bool displayColorSpace,
		SurfaceRequest &output
	) try {
		ENGINE_PROFILE("composer surface capture");
		const uint64_t previousBytes = SurfaceSourceBytes(output);
		const uint64_t limit = CaptureLimitOf(snapshot);
		if (previousBytes > limit || RetainedBytesOf(snapshot) > limit - previousBytes)
			return "Composer previous output and snapshot exceed replacement budget";
		uint64_t definitionBytes = sizeof(CapturedDefinition) + MAXIMUM_ARGUMENTS * (sizeof(Argument) + 129);
		for (const auto port : {"vertex", "main", "global", "libraries"}) {
			const auto *value = ValueOf(snapshot, port);
			const auto *text = value ? std::get_if<std::string>(value) : nullptr;
			if (!text || text->size() > MAXIMUM_SOURCE_BYTES ||
				text->size() > UINT64_MAX - definitionBytes - 16)
				return "Composer source capture is missing or over budget";
			definitionBytes += std::max(uint64_t(text->size() + 1), uint64_t(16));
		}
		if (definitionBytes > limit - previousBytes - RetainedBytesOf(snapshot))
			return "Composer definition scratch exceeds replacement budget";
		CapturedDefinition definition;
		if (const auto failure = CaptureDefinitionImpl(node, snapshot, definition)) return failure;
		core::Name shader;
		for (const auto &property : node.SourceProperties)
			if (property.Port == COOKED_SELECTOR) {
				if (shader.IsValid()) return "Composer cooked asset selector repeats";
				const auto *name = std::get_if<std::string>(&property.Data);
				if (!name || name->empty() || name->size() > assets::Shader::MAXIMUM_NAME)
					return "Composer cooked asset selector must be a bounded name";
				shader = core::Name(*name);
			}
		uint64_t revision = 0;
		const auto *pair = library.Find(owner, shader, revision);
		if (!pair) return "Composer cooked asset has not been installed for this owner";
		assets::ContentHash fingerprint;
		if (const auto failure = DefinitionFingerprint(definition.View(), fingerprint)) return failure;
		if (fingerprint != pair->DefinitionFingerprint || definition.Arguments != pair->SpirV.Arguments)
			return "Composer source changed after cook";
		std::array<const engine::imagegraph::Image *, MAXIMUM_SAMPLERS> textures{};
		std::vector<SamplerBinding> active;
		if (const auto failure = SamplerBindings(pair->SpirV, "spirv", active)) return failure;
		const engine::imagegraph::Image inactive{1, 1, std::vector<uint8_t>(4, 0), 0};
		textures[0] = ImageOf(snapshot, "base_texture");
		if (!textures[0]) return "Composer base texture requires one image row";
		size_t textureCount = 1, numericCount = 0;
		std::array<std::array<double, 16>, MAXIMUM_ARGUMENTS> components{};
		std::array<Value, MAXIMUM_ARGUMENTS> values{};
		for (size_t index = 0; index < definition.Arguments.size(); ++index) {
			const auto &argument = definition.Arguments[index];
			if (argument.Name.empty()) continue;
			const auto port = "argument_value_" + std::to_string(index);
			if (argument.Kind == ArgumentKind::Sampler2D) {
				if (textureCount >= textures.size()) return "Composer sampler count exceeds budget";
				textures[textureCount++] = ImageOf(snapshot, port);
				if (!textures[textureCount - 1]) {
					const bool used = std::any_of(active.begin(), active.end(), [&](const auto &binding) {
						return binding.SourceSlot == textureCount - 1;
					});
					if (used) return "Composer active sampler requires one image row: " + port;
					textures[textureCount - 1] = &inactive;
				}
			} else {
				size_t count = 0;
				if (const auto failure =
						Numbers(ValueOf(snapshot, port), argument.Kind, components[numericCount], count))
					return failure;
				values[numericCount] = {argument.Name, std::span(components[numericCount].data(), count)};
				++numericCount;
			}
		}
		const auto pairBytes = CookedPairRetainedBytes(*pair, MAXIMUM_SURFACE_JOB_BYTES);
		if (!pairBytes) return "Composer pair retained capacity exceeds capture budget";
		uint64_t bytes = sizeof(SurfaceRequest) - sizeof(CookedPair) + *pairBytes + pair->SpirV.UniformBytes +
						 textureCount * sizeof(engine::imagegraph::Image);
		if (bytes > MAXIMUM_SURFACE_JOB_BYTES) return "Composer cooked pair exceeds capture budget";
		for (size_t index = 0; index < textureCount; ++index) {
			if (!engine::imagegraph::ValidSurfaceLayout(*textures[index], 4096, MAXIMUM_SURFACE_JOB_BYTES) ||
				!engine::imagegraph::FiniteSurfaceSamples(*textures[index]) ||
				textures[index]->Pixels.size() > MAXIMUM_SURFACE_JOB_BYTES - bytes)
				return "Composer textures and cooked pair exceed capture budget";
			bytes += textures[index]->Pixels.size();
		}
		if (definitionBytes > limit - previousBytes - RetainedBytesOf(snapshot) ||
			bytes > limit - previousBytes - RetainedBytesOf(snapshot) - definitionBytes)
			return "Composer snapshot and owned request exceed capture budget";
		SurfaceRequest candidate;
		candidate.Shader = shader;
		candidate.ShaderRevision = revision;
		if (const auto failure =
				Pack(pair->SpirV, std::span(values.data(), numericCount), candidate.Uniforms))
			return failure;
		candidate.Pair = *pair;
		candidate.Textures.reserve(textureCount);
		for (size_t index = 0; index < textureCount; ++index)
			candidate.Textures.push_back(*textures[index]);
		candidate.Format =
			displayColorSpace ? assets::TextureFormat::RGBA8 : assets::TextureFormat::RGBA8_LINEAR;
		candidate.LinearSampling = InterpolationOf(snapshot) > 1;
		if (const auto failure = ValidateSurfaceRequest(candidate)) return failure;
		if (SurfaceSourceBytes(candidate) >
			limit - previousBytes - RetainedBytesOf(snapshot) - definitionBytes)
			return "Composer actual owned capacities exceed replacement budget";
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer surface capture allocation failed";
	}
	std::optional<std::string> BuildSurfaceRequest(
		const engine::imagegraph::Node &node,
		const engine::imagegraph::EvaluationSnapshot &snapshot,
		const CookedComposerLibrary &library,
		core::Name owner,
		bool displayColorSpace,
		SurfaceRequest &output
	) {
		return BuildSurfaceRequestImpl(node, snapshot, library, owner, displayColorSpace, output);
	}
	std::optional<std::string> BuildSurfaceRequest(
		const engine::imagegraph::HostNodeInvocation &invocation,
		const CookedComposerLibrary &library,
		core::Name owner,
		bool displayColorSpace,
		SurfaceRequest &output
	) {
		return BuildSurfaceRequestImpl(
			invocation.Authored, invocation, library, owner, displayColorSpace, output
		);
	}
	std::optional<std::string>
	BuildSurfaceJob(const SurfaceRequest &request, SurfaceJob &output, bool captureReadback) try {
		if (const auto failure = ValidateSurfaceRequest(request)) return failure;
		SurfaceJob candidate;
		graph::Node upload{
			.Name = core::Name("composer.upload"),
			.Kind = core::Name("composer-upload"),
			.Scope = graph::NodeScope::Frame
		};
		graph::Node raster{
			.Name = core::Name("composer.surface"),
			.Kind = core::Name("composer-surface"),
			.Scope = graph::NodeScope::Frame
		};
		for (size_t index = 0; index < request.Textures.size(); ++index) {
			const auto &image = request.Textures[index];
			const auto format =
				image.Format == engine::imagegraph::SurfaceFormat::RGBA16Float
					? graph::ResourceFormat::RGBA16F
				: image.Format == engine::imagegraph::SurfaceFormat::RGBA32Float
					? graph::ResourceFormat::RGBA32F
				: image.Format == engine::imagegraph::SurfaceFormat::R16Float ? graph::ResourceFormat::R16F
				: image.Format == engine::imagegraph::SurfaceFormat::R32Float ? graph::ResourceFormat::R32F
																			  : graph::ResourceFormat::RGBA8;
			const auto id = candidate.Graph.AddResource(
				{.Name = core::Name("composer.texture." + std::to_string(index)),
				 .Kind = graph::ResourceKind::Texture,
				 .Format = format,
				 .Width = image.Width,
				 .Height = image.Height}
			);
			if (!id.IsValid()) return "Composer input graph resource was refused";
			upload.Writes.push_back(id);
			raster.Reads.push_back(id);
		}
		const auto &base = request.Textures[0];
		const bool display = request.Format == assets::TextureFormat::RGBA8 || captureReadback;
		const auto rendered = candidate.Graph.AddResource(
			{.Name = core::Name(display ? "composer.rendered" : "composer.output"),
			 .Width = base.Width,
			 .Height = base.Height,
			 .External = !display}
		);
		raster.Writes.push_back(rendered);
		if (!rendered.IsValid() || !candidate.Graph.AddNode(std::move(upload)).IsValid() ||
			!candidate.Graph.AddNode(std::move(raster)).IsValid())
			return "Composer graph nodes were refused";
		candidate.Output = rendered;
		if (display) {
			candidate.Output = candidate.Graph.AddResource(
				{.Name = core::Name("composer.display.bytes"),
				 .Kind = graph::ResourceKind::Buffer,
				 .Format = graph::ResourceFormat::RGBA8,
				 .Width = base.Width,
				 .Height = base.Height,
				 .External = true,
				 .BufferStride = 4}
			);
			if (!candidate.Output.IsValid() || !candidate.Graph
													.AddNode(
														{.Name = core::Name("composer.display-download"),
														 .Kind = core::Name("composer-display-download"),
														 .Reads = {rendered},
														 .Writes = {candidate.Output},
														 .Scope = graph::NodeScope::Frame}
													)
													.IsValid())
				return "Composer display download graph was refused";
		}
		core::Name offender;
		if (candidate.Graph.Compile(candidate.Schedule, offender) != graph::GraphStatus::Ok)
			return "Composer graph failed to compile: " + std::string(offender.Text());
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer surface graph allocation failed";
	}
	std::optional<std::string>
	BuildSurfaceDisplayUploadJob(uint32_t width, uint32_t height, uint64_t bytes, SurfaceJob &output) try {
		if (width == 0 || height == 0 || width > 4096 || height > 4096 ||
			bytes != uint64_t(width) * height * 4 || bytes > MAXIMUM_SURFACE_JOB_BYTES)
			return "Composer display upload dimensions or byte budget are invalid";
		SurfaceJob candidate;
		const auto input = candidate.Graph.AddResource(
			{.Name = core::Name("composer.display.bytes"),
			 .Kind = graph::ResourceKind::Buffer,
			 .Format = graph::ResourceFormat::RGBA8,
			 .Width = width,
			 .Height = height,
			 .External = true,
			 .BufferStride = 4}
		);
		candidate.Output = candidate.Graph.AddResource(
			{.Name = core::Name("composer.output"),
			 .Format = graph::ResourceFormat::RGBA8_SRGB,
			 .Width = width,
			 .Height = height,
			 .External = true}
		);
		if (!input.IsValid() || !candidate.Output.IsValid() ||
			!candidate.Graph
				 .AddNode(
					 {.Name = core::Name("composer.display-upload"),
					  .Kind = core::Name("composer-display-upload"),
					  .Reads = {input},
					  .Writes = {candidate.Output},
					  .Scope = graph::NodeScope::Frame}
				 )
				 .IsValid())
			return "Composer display upload graph was refused";
		core::Name offender;
		if (candidate.Graph.Compile(candidate.Schedule, offender) != graph::GraphStatus::Ok)
			return "Composer display upload graph failed to compile";
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer display graph allocation failed";
	}

} // namespace engine::render::hlsl
