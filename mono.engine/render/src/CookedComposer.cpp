#include "ComposerCookResidency.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/msl/Translate.hpp>
#include <engine/render/CookedComposer.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#ifndef ATOMIC_COMPOSER_COOKER_IDENTITY
#define ATOMIC_COMPOSER_COOKER_IDENTITY ""
#endif
namespace engine::render::hlsl {
	std::string_view CookerIdentity() {
		return ATOMIC_COMPOSER_COOKER_IDENTITY;
	}

	namespace {
		std::string ArgumentKey(size_t index) {
			return std::string("composer.argument.") + char('0' + index / 10) + char('0' + index % 10);
		}
		const assets::ShaderVariant *StageOf(const assets::ShaderData &data, std::string_view name) {
			for (const auto &variant : data.Variants)
				if (variant.Name == name) return &variant;
			return nullptr;
		}
		const assets::ShaderPayload *
		PayloadOf(const assets::ShaderVariant &variant, std::string_view backend) {
			for (const auto &payload : variant.Payloads)
				if (payload.Backend == backend) return &payload;
			return nullptr;
		}
		void HashSize(assets::Hasher &hash, uint64_t size) {
			std::array<std::byte, 8> bytes{};
			for (size_t i = 0; i < 8; ++i)
				bytes[i] = std::byte(size >> (8 * i));
			hash.Update(bytes);
		}
		void HashText(assets::Hasher &hash, std::string_view text) {
			HashSize(hash, text.size());
			hash.Update(std::as_bytes(std::span(text.data(), text.size())));
		}
		std::optional<uint64_t> PayloadBytesOf(const assets::ShaderData &data) {
			if (!data.IsValid() || data.ShaderAbi != COOKED_ABI || data.SourceLanguage != "hlsl" ||
				data.TargetEnvironment != "vulkan1.0" || data.Variants.size() != 2)
				return {};
			uint64_t bytes = 0;
			for (const auto &variant : data.Variants)
				for (const auto &payload : variant.Payloads) {
					if (payload.Bytes.size() > MAXIMUM_OWNER_COOKED_BYTES - bytes) return {};
					bytes += payload.Bytes.size();
				}
			return bytes;
		}
		void ReadWords(const assets::ShaderPayload &payload, Stage &stage) {
			stage.SpirV.resize(payload.Bytes.size() / 4);
			for (size_t index = 0; index < stage.SpirV.size(); ++index) {
				uint32_t word = 0;
				for (size_t byte = 0; byte < 4; ++byte)
					word |= uint32_t(payload.Bytes[index * 4 + byte]) << (8 * byte);
				stage.SpirV[index] = word;
			}
		}
	} // namespace
	std::optional<uint64_t> CookedPairRetainedBytes(const CookedPair &pair, uint64_t maximumBytes) {
		uint64_t bytes = sizeof(pair);
		if (bytes > maximumBytes) return {};
		const auto add = [&](uint64_t amount) {
			if (amount > maximumBytes - bytes) return false;
			bytes += amount;
			return true;
		};
		const auto vector = [&](uint64_t count, uint64_t stride) {
			return count <= (maximumBytes - bytes) / stride && add(count * stride);
		};
		if (!vector(pair.SpirV.Vertex.SpirV.capacity(), sizeof(uint32_t)) ||
			!vector(pair.SpirV.Fragment.SpirV.capacity(), sizeof(uint32_t)) ||
			!vector(pair.VertexMsl.capacity(), 1) || !vector(pair.FragmentMsl.capacity(), 1) ||
			!vector(pair.SpirV.Arguments.capacity(), sizeof(Argument)) ||
			!vector(pair.SpirV.Members.capacity(), sizeof(Member)))
			return {};
		for (const auto &argument : pair.SpirV.Arguments)
			if (argument.Name.capacity() == SIZE_MAX || !add(argument.Name.capacity() + 1)) return {};
		for (const auto &member : pair.SpirV.Members)
			if (member.Name.capacity() == SIZE_MAX || !add(member.Name.capacity() + 1)) return {};
		return bytes;
	}
	std::optional<std::string>
	DefinitionFingerprint(const Definition &definition, assets::ContentHash &output) {
		if (definition.Arguments.size() > MAXIMUM_ARGUMENTS)
			return "Composer declaration count exceeds budget";
		uint64_t bytes = 0;
		assets::Hasher hash;
		HashText(hash, COOKED_ABI);
		for (const auto text :
			 {definition.Vertex, definition.Main, definition.Global, definition.Libraries}) {
			if (text.size() > MAXIMUM_SOURCE_BYTES - bytes)
				return "Composer source exceeds fingerprint budget";
			bytes += text.size();
			HashText(hash, text);
		}
		HashSize(hash, definition.Arguments.size());
		for (const auto &argument : definition.Arguments) {
			if (argument.Name.size() > 128 || size_t(argument.Kind) > 8)
				return "Composer declaration is invalid";
			HashText(hash, argument.Name);
			HashSize(hash, uint64_t(argument.Kind));
		}
		output = hash.Finish();
		return {};
	}
	std::optional<std::string> CookArtifact(
		const Definition &definition,
		std::span<const Library> libraries,
		std::string_view compilerIdentity,
		bool includeMsl,
		assets::ShaderData &output,
		std::string_view translatorIdentity,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE_CAT("composer artifact cook", core::ProfileCategory::Assets);
		if (includeMsl &&
			(translatorIdentity.empty() || translatorIdentity.size() > assets::Shader::MAXIMUM_NAME))
			return "Offline Composer MSL needs explicit translator provenance";
		const auto previousBytes = assets::ShaderRetainedPayloadBytes(output, maximumBytes);
		if (!previousBytes) return "Composer prior artifact exceeds operation budget";
		const uint64_t available = maximumBytes - *previousBytes;
		assets::ContentHash fingerprint;
		if (const auto failure = DefinitionFingerprint(definition, fingerprint)) return failure;
		Source assembled;
		if (const auto failure = Assemble(definition, libraries, assembled, available)) return failure;
		const uint64_t assembledBytes = detail::SourceRetainedBytes(assembled);
		if (assembledBytes > available) return "Composer assembled source exceeds operation budget";
		Program program;
		if (const auto failure = Cook(definition, assembled, program, available - assembledBytes))
			return failure;
		const uint64_t programBytes = detail::ProgramRetainedBytes(program);
		if (programBytes > available - assembledBytes) return "Composer program exceeds operation budget";
		const uint64_t containerMaximum = available - assembledBytes - programBytes;
		assets::ShaderData candidate;
		if (const auto failure = BuildContainer(program, compilerIdentity, candidate, containerMaximum))
			return failure;
		const auto beforeFeatures = assets::ShaderRetainedPayloadBytes(candidate, containerMaximum);
		uint64_t additions = 2 * (candidate.CompilerOptions.size() + 1) * sizeof(assets::ShaderFeature) + 256;
		for (const auto &variant : candidate.Variants) {
			additions +=
				2 * (variant.Features.size() + definition.Arguments.size()) * sizeof(assets::ShaderFeature);
			for (const auto &argument : definition.Arguments)
				additions += 2 * (argument.Name.size() + 64);
		}
		if (!beforeFeatures || additions > containerMaximum - *beforeFeatures)
			return "Composer artifact feature copies exceed operation budget";
		candidate.CompilerOptions.reserve(candidate.CompilerOptions.size() + 1);
		candidate.CompilerOptions.push_back({"composer.definition", fingerprint.ToHex()});
		for (auto &variant : candidate.Variants) {
			variant.Features.reserve(variant.Features.size() + definition.Arguments.size());
			for (size_t index = 0; index < definition.Arguments.size(); ++index) {
				const auto &argument = definition.Arguments[index];
				variant.Features.push_back(
					{ArgumentKey(index), std::to_string(uint8_t(argument.Kind)) + ":" + argument.Name}
				);
			}
			if (!includeMsl) continue;
			const auto &words = variant.Stage == "vertex" ? program.Vertex.SpirV : program.Fragment.SpirV;
			const auto translation = msl::Translate(words);
			if (translation.Failed || translation.Source.empty())
				return "Composer offline MSL translation failed: " + translation.Error;
			if (translation.Source.size() > assets::Shader::MAXIMUM_PAYLOAD_BYTES)
				return "Composer MSL payload exceeds budget";
			const auto retained = assets::ShaderRetainedPayloadBytes(candidate, containerMaximum);
			const uint64_t translationBytes = translation.Source.capacity() + 1;
			const uint64_t payloadCopy = translation.Source.size() + sizeof(assets::ShaderPayload) + 128 +
										 2 * (variant.Payloads.size() + 1) * sizeof(assets::ShaderPayload);
			if (!retained || translationBytes > containerMaximum - *retained ||
				payloadCopy > containerMaximum - *retained - translationBytes)
				return "Composer translated payload copy exceeds operation budget";
			variant.Payloads.reserve(variant.Payloads.size() + 1);
			assets::ShaderPayload payload;
			payload.Backend = "msl";
			payload.EntryPoint = msl::ENTRY_POINT;
			payload.Target = "metal2.0";
			const auto bytes = std::as_bytes(std::span(translation.Source.data(), translation.Source.size()));
			payload.Bytes.assign(bytes.begin(), bytes.end());
			variant.Payloads.insert(variant.Payloads.begin(), std::move(payload));
		}
		if (includeMsl) {
			const auto retained = assets::ShaderRetainedPayloadBytes(candidate, containerMaximum);
			if (!retained || 2 * (translatorIdentity.size() + 1) > containerMaximum - *retained)
				return "Composer translator provenance exceeds operation budget";
			candidate.TranslatorVersion = translatorIdentity;
		}
		if (!assets::ShaderRetainedPayloadBytes(candidate, containerMaximum))
			return "Composer actual artifact capacity exceeds operation budget";
		if (!candidate.IsValid()) return "Composer artifact metadata is invalid";
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer artifact cook allocation failed";
	}
	std::optional<std::string>
	AdmitArtifact(const assets::ShaderData &data, CookedPair &output, uint64_t maximumBytes) try {
		ENGINE_PROFILE_CAT("composer artifact admission", core::ProfileCategory::Assets);
		const auto bytes = PayloadBytesOf(data);
		if (!bytes) return "Composer artifact ABI or payload budget is invalid";
		const auto inputBytes = assets::ShaderRetainedPayloadBytes(data, maximumBytes);
		const auto previousBytes = CookedPairRetainedBytes(output, maximumBytes);
		// Pair clones have at most64 named arguments/members; reflection's own
		// container admission separately bounds every name and row before copying.
		constexpr uint64_t candidateMetadataAllowance =
			sizeof(CookedPair) + MAXIMUM_ARGUMENTS * (sizeof(Argument) + sizeof(Member) + 2 * (2 * 130 + 32));
		if (!inputBytes || !previousBytes || *inputBytes > maximumBytes - *previousBytes ||
			candidateMetadataAllowance > maximumBytes - *previousBytes - *inputBytes ||
			*bytes > maximumBytes - *previousBytes - *inputBytes - candidateMetadataAllowance)
			return "Composer admission exceeds operation residency budget";
		const auto *vertex = StageOf(data, "hlsl.vertex"), *fragment = StageOf(data, "hlsl.fragment");
		if (!vertex || !fragment || vertex->Stage != "vertex" || fragment->Stage != "fragment" ||
			!vertex->Specializations.empty() || !fragment->Specializations.empty() ||
			vertex->Features != fragment->Features)
			return "Composer artifact requires its declared unspecialized stage pair";
		const auto fingerprint =
			std::find_if(data.CompilerOptions.begin(), data.CompilerOptions.end(), [](const auto &option) {
				return option.Name == "composer.definition";
			});
		if (fingerprint == data.CompilerOptions.end()) return "Composer definition fingerprint is missing";
		const auto hash = assets::ContentHash::FromHex(fingerprint->Value);
		if (!hash || hash->IsZero()) return "Composer definition fingerprint is invalid";
		CookedPair candidate;
		candidate.PayloadBytes = *bytes;
		candidate.DefinitionFingerprint = *hash;
		if (vertex->Features.size() > MAXIMUM_ARGUMENTS) return "Composer declarations exceed budget";
		candidate.SpirV.Arguments.reserve(vertex->Features.size());
		candidate.SpirV.Members.reserve(vertex->Features.size());
		for (size_t index = 0; index < vertex->Features.size(); ++index) {
			const auto &feature = vertex->Features[index];
			if (feature.Name != ArgumentKey(index) || feature.Value.size() < 2 || feature.Value[0] < '0' ||
				feature.Value[0] > '8' || feature.Value[1] != ':' || feature.Value.size() > 130)
				return "Composer ordered argument declaration is invalid";
			candidate.SpirV.Arguments.push_back(
				{feature.Value.substr(2), ArgumentKind(feature.Value[0] - '0')}
			);
		}
		for (uint32_t stage = 0; stage < 2; ++stage) {
			const auto &variant = stage ? *fragment : *vertex;
			const auto *spirv = PayloadOf(variant, "spirv"), *metal = PayloadOf(variant, "msl");
			if (!spirv || spirv->EntryPoint != "main" || spirv->Target != "vulkan1.0" ||
				spirv->Bytes.size() < 20 || spirv->Bytes.size() % 4)
				return "Composer retained SPIR-V payload is invalid";
			if (metal &&
				(metal->EntryPoint != msl::ENTRY_POINT || metal->Target != "metal2.0" ||
				 std::find(metal->Bytes.begin(), metal->Bytes.end(), std::byte{}) != metal->Bytes.end()))
				return "Composer retained MSL payload is invalid";
			ReadWords(*spirv, stage ? candidate.SpirV.Fragment : candidate.SpirV.Vertex);
			if (metal) (stage ? candidate.FragmentMsl : candidate.VertexMsl) = metal->Bytes;
		}
		if (vertex->Parameters.size() != 1) return "Composer fixed vertex matrix metadata is missing";
		candidate.VertexRowMajor = vertex->Parameters[0].RowMajor;
		if (candidate.VertexMsl.empty() != candidate.FragmentMsl.empty())
			return "Composer MSL stage pair is incomplete";
		candidate.SpirV.SamplerCount = 1;
		for (const auto &argument : candidate.SpirV.Arguments)
			if (!argument.Name.empty() && argument.Kind == ArgumentKind::Sampler2D)
				++candidate.SpirV.SamplerCount;
		for (const auto &resource : fragment->Resources)
			if (resource.Kind == "uniform-buffer") {
				if (resource.MinimumBytes > 16384) return "Composer uniform block exceeds budget";
				candidate.SpirV.UniformBytes = uint32_t(resource.MinimumBytes);
			}
		// Parameters are canonical-name sorted; executable block order follows
		// authored declarations.
		for (const auto &argument : candidate.SpirV.Arguments) {
			const auto found = std::find_if(
				fragment->Parameters.begin(), fragment->Parameters.end(), [&](const auto &parameter) {
					return parameter.Name == argument.Name;
				}
			);
			if (found != fragment->Parameters.end())
				candidate.SpirV.Members.push_back(
					{argument.Name,
					 argument.Kind,
					 found->Offset,
					 found->Bytes,
					 found->MatrixStride,
					 found->RowMajor}
				);
		}
		if (const auto failure = Admit(candidate.SpirV)) return failure;
		const auto candidateBeforeReflection = CookedPairRetainedBytes(candidate, maximumBytes);
		if (!candidateBeforeReflection ||
			*candidateBeforeReflection > maximumBytes - *inputBytes - *previousBytes)
			return "Composer candidate capacities exceed reflection scratch budget";
		assets::ShaderData reflected;
		if (const auto failure = BuildContainer(
				candidate.SpirV,
				data.CompilerVersion,
				reflected,
				maximumBytes - *inputBytes - *previousBytes - *candidateBeforeReflection
			))
			return failure;
		for (const auto &actual : reflected.Variants) {
			const auto *declared = StageOf(data, actual.Name);
			if (!declared || declared->Resources != actual.Resources ||
				declared->Parameters != actual.Parameters || declared->Inputs != actual.Inputs ||
				declared->Outputs != actual.Outputs)
				return "Composer retained reflection differs from executable bytes";
		}
		const auto candidateBytes = CookedPairRetainedBytes(candidate, maximumBytes);
		const auto reflectedBytes = assets::ShaderRetainedPayloadBytes(reflected, maximumBytes);
		if (!candidateBytes || !reflectedBytes ||
			*candidateBytes > maximumBytes - *inputBytes - *previousBytes ||
			*reflectedBytes > maximumBytes - *inputBytes - *previousBytes - *candidateBytes)
			return "Composer admitted capacities exceed operation residency budget";
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer artifact admission allocation failed";
	}
	std::optional<std::string> CookedComposerLibrary::Install(
		core::Name owner, core::Name name, const assets::ShaderData &data, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE_CAT("composer cooked install", core::ProfileCategory::Assets);
		if (!owner.IsValid() || !name.IsValid()) return "Composer cooked owner and name must be explicit";
		const auto bytes = PayloadBytesOf(data);
		if (!bytes) return "Composer cooked payload is invalid";
		auto found = std::find_if(Entries.begin(), Entries.end(), [&](const auto &entry) {
			return entry.Owner == owner && entry.Name == name;
		});
		uint64_t held = 0;
		size_t count = 0, ownerCount = 0;
		std::array<core::Name, MAXIMUM_LIBRARY_OWNERS> owners{};
		for (const auto &entry : Entries) {
			if (std::find(owners.begin(), owners.begin() + ownerCount, entry.Owner) ==
				owners.begin() + ownerCount) {
				if (ownerCount == owners.size()) return "Composer library owner count exceeded";
				owners[ownerCount++] = entry.Owner;
			}
			if (entry.Owner == owner) {
				held += entry.Pair.PayloadBytes;
				++count;
			}
		}
		if (count == 0 && ownerCount == MAXIMUM_LIBRARY_OWNERS)
			return "Composer library owner count exceeded";
		if (found != Entries.end()) held -= found->Pair.PayloadBytes;
		if ((found == Entries.end() &&
			 (count >= MAXIMUM_OWNER_PROGRAMS || Entries.size() >= MAXIMUM_LIBRARY_PROGRAMS)) ||
			held > MAXIMUM_OWNER_COOKED_BYTES || *bytes > MAXIMUM_OWNER_COOKED_BYTES - held)
			return "Composer owner cooked backing payload budget exceeded";
		const bool immutable = name.Text().size() == 72 && name.Text().ends_with(".ashader") &&
							   assets::ContentHash::FromHex(name.Text().substr(0, 64)).has_value();
		const auto inputBytes = assets::ShaderRetainedPayloadBytes(data, maximumBytes);
		const auto retainedLibrary = RetainedBytes(maximumBytes);
		const auto encodedBytes = assets::Shader::EncodedBytes(data);
		const uint64_t encoderStorage = sizeof(core::ByteWriter) + sizeof(std::vector<std::byte>);
		if (!inputBytes || !retainedLibrary || !encodedBytes ||
			*inputBytes > maximumBytes - *retainedLibrary ||
			(immutable &&
			 (encoderStorage > maximumBytes - *inputBytes - *retainedLibrary ||
			  *encodedBytes > (maximumBytes - *inputBytes - *retainedLibrary - encoderStorage) / 3)))
			return "Composer install exceeds operation residency budget";
		if (immutable) {
			// The codec builds its body while the explicitly reserved final writer remains alive.
			core::ByteWriter encoded{size_t(*encodedBytes), size_t(*encodedBytes)};
			if (!assets::Shader::Write(encoded, data)) return "Composer immutable artifact encoding failed";
			auto identity = encoded.TakeBytes();
			if (identity.capacity() > maximumBytes - *inputBytes - *retainedLibrary - encoderStorage)
				return "Composer actual encoded identity exceeds operation budget";
			if (assets::Hasher::Of(identity).ToHex() != name.Text().substr(0, 64))
				return "Composer immutable artifact name mismatches ASH1 bytes";
			core::Metrics::Count("shader.composer.identity_encoded_bytes", identity.size());
		}

		CookedPair candidate;
		if (const auto failure = AdmitArtifact(data, candidate, maximumBytes - *retainedLibrary))
			return failure;
		if (immutable && found != Entries.end()) {
			if (found->Pair != candidate)
				return "Composer immutable artifact cannot replace a different pair";
			return {};
		}
		if (found != Entries.end() && found->Revision == std::numeric_limits<uint64_t>::max())
			return "Composer cooked revision exhausted";
		const auto installedBytes = CookedPairRetainedBytes(candidate, maximumBytes);
		if (!installedBytes || *installedBytes > maximumBytes - *retainedLibrary - *inputBytes)
			return "Composer installed pair overlaps operation residency budget";
		static_assert(std::is_nothrow_move_constructible_v<Entry>);
		static_assert(std::is_nothrow_move_assignable_v<CookedPair>);
		if (found == Entries.end()) {
			if (Entries.size() == Entries.capacity()) {
				std::vector<Entry> grown;
				if (sizeof(grown) > maximumBytes - *retainedLibrary - *inputBytes - *installedBytes)
					return "Composer entry growth scratch exceeds operation budget";
				const uint64_t scratch = *inputBytes + *installedBytes + sizeof(grown);
				const uint64_t oldBacking = Entries.capacity() * sizeof(Entry);
				const size_t wanted = Entries.size() + 1;
				if (!detail::AdmitComposerVectorGrowth(
						oldBacking, wanted * sizeof(Entry), *retainedLibrary, scratch, maximumBytes
					))
					return "Composer entry backing growth exceeds operation budget";
				grown.reserve(wanted);
				if (!detail::AdmitComposerVectorGrowth(
						oldBacking, grown.capacity() * sizeof(Entry), *retainedLibrary, scratch, maximumBytes
					))
					return "Composer actual entry backing capacity exceeds operation budget";
				core::Metrics::Count("shader.composer.entry_backing_bytes", grown.capacity() * sizeof(Entry));
				core::Metrics::Count("shader.composer.entry_allocations", 1);
				for (auto &entry : Entries)
					grown.push_back(std::move(entry));
				grown.push_back({owner, name, 1, std::move(candidate)});
				Entries.swap(grown);
			} else
				Entries.push_back({owner, name, 1, std::move(candidate)});
		} else {
			found->Pair = std::move(candidate);
			++found->Revision;
		}
		core::Metrics::Count("shader.composer.installed_backing_payload_bytes", *bytes);
		if (installedBytes)
			core::Metrics::Count("shader.composer.installed_retained_capacity_bytes", *installedBytes);
		core::Metrics::Count("shader.composer.cooked_installations", 1);
		return {};
	} catch (const std::bad_alloc &) {
		return "Composer cooked install allocation failed";
	}
	const CookedPair *
	CookedComposerLibrary::Find(core::Name owner, core::Name name, uint64_t &revision) const {
		for (const auto &entry : Entries)
			if (entry.Owner == owner && entry.Name == name) {
				revision = entry.Revision;
				return &entry.Pair;
			}
		revision = 0;
		return nullptr;
	}
	bool CookedComposerLibrary::Remove(core::Name owner, core::Name name, uint64_t installedRevision) {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const auto &entry) {
			return entry.Owner == owner && entry.Name == name && entry.Revision == installedRevision;
		});
		if (found == Entries.end()) return false;
		Entries.erase(found);
		return true;
	}
	void CookedComposerLibrary::RemoveOwner(core::Name owner) {
		std::erase_if(Entries, [&](const auto &entry) { return entry.Owner == owner; });
	}
	std::optional<uint64_t> CookedComposerLibrary::RetainedBytes(uint64_t maximumBytes) const {
		uint64_t bytes = sizeof(*this);
		if (bytes > maximumBytes || Entries.capacity() > (maximumBytes - bytes) / sizeof(Entry)) return {};
		bytes += Entries.capacity() * sizeof(Entry);
		for (const auto &entry : Entries) {
			const auto pair = CookedPairRetainedBytes(entry.Pair, maximumBytes);
			if (!pair || *pair - sizeof(CookedPair) > maximumBytes - bytes) return {};
			bytes += *pair - sizeof(CookedPair);
		}
		return bytes;
	}
	uint64_t CookedComposerLibrary::PayloadBytes(core::Name owner) const {
		uint64_t bytes = 0;
		for (const auto &entry : Entries)
			if (entry.Owner == owner) bytes += entry.Pair.PayloadBytes;
		return bytes;
	}
} // namespace engine::render::hlsl
