#include "ProceduralState.hpp"

#include <engine/assets/ContentHash.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/procedural/Artifact.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace engine::script {

	namespace procedural_detail {

		bool Refuse(procedural::Diagnostic &diagnostic, procedural::Status status, std::string_view field) {
			diagnostic.Code = status;
			diagnostic.Field.assign(field.substr(0, 128));
			return false;
		}

		bool SameScope(procedural::RegionScope left, procedural::RegionScope right) {
			return left.X == right.X && left.Y == right.Y && left.CellsPerSide == right.CellsPerSide;
		}

		bool ScopeLess(procedural::RegionScope left, procedural::RegionScope right) {
			return std::tie(left.X, left.Y, left.CellsPerSide) <
				   std::tie(right.X, right.Y, right.CellsPerSide);
		}

		bool Contains(procedural::Bounds bounds, procedural::Point point) {
			return point.X >= bounds.Min.X && point.X < bounds.Max.X && point.Y >= bounds.Min.Y &&
				   point.Y < bounds.Max.Y;
		}

		bool ValidSource(const ProceduralSource &source) {
			constexpr int64_t MAXIMUM_COORDINATE = int64_t{1} << 48;
			if (source.Name.size() > 128 || !procedural::ValidStableName(source.Name) ||
				source.Anchor.X < -MAXIMUM_COORDINATE || source.Anchor.X > MAXIMUM_COORDINATE ||
				source.Anchor.Y < -MAXIMUM_COORDINATE || source.Anchor.Y > MAXIMUM_COORDINATE)
				return false;
			if (source.Kind == procedural::SourceKind::Village) return source.TargetVillage.empty();
			return source.Kind == procedural::SourceKind::Entrance && source.TargetVillage.size() <= 128 &&
				   procedural::ValidStableName(source.TargetVillage);
		}

		bool SourceLess(const ProceduralSource &left, const ProceduralSource &right) {
			if (left.Kind != right.Kind) return left.Kind < right.Kind;
			return procedural::StableNameLess(left.Name, right.Name);
		}

		procedural::Digest Hash(std::span<const uint8_t> bytes) {
			const auto hash = assets::Hasher::Of(std::as_bytes(bytes));
			return {hash.Digest};
		}

		uint64_t TakeSequence(std::atomic<uint64_t> &next) {
			uint64_t observed = next.load(std::memory_order_relaxed);
			while (observed != 0) {
				const uint64_t successor =
					observed == std::numeric_limits<uint64_t>::max() ? 0 : observed + 1;
				if (next.compare_exchange_weak(observed, successor, std::memory_order_relaxed))
					return observed;
			}
			return 0;
		}

		uint64_t NextIncarnation() {
			static std::atomic<uint64_t> next{1};
			return TakeSequence(next);
		}

		bool CompileAuthority(
			const ProceduralAuthority &authority,
			procedural::CompiledRecipe &out,
			procedural::Diagnostic &diagnostic
		) {
			if (authority.Incarnation == 0)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.incarnation");
			if (authority.Owner.size() > 128 || !procedural::ValidStableName(authority.Owner) ||
				authority.Sources.size() > ProceduralHostLimits::MaximumSources ||
				authority.Regions.size() > ProceduralHostLimits::MaximumRegions ||
				authority.RecipeBytes.size() > 32768)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.limits");
			procedural::Recipe recipe;
			if (procedural::DecodeRecipe(authority.RecipeBytes, recipe, diagnostic) !=
					procedural::Status::Ok ||
				procedural::CompileForestVillage(recipe, out, diagnostic) != procedural::Status::Ok)
				return false;
			for (size_t index = 0; index < authority.Sources.size(); ++index) {
				if (!ValidSource(authority.Sources[index]) ||
					(index > 0 && !SourceLess(authority.Sources[index - 1], authority.Sources[index])))
					return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.sources");
			}
			for (size_t index = 0; index < authority.Regions.size(); ++index) {
				procedural::Bounds owner;
				procedural::Bounds query;
				const auto &region = authority.Regions[index];
				if (region.SourceRevision == 0 ||
					(index > 0 && !ScopeLess(authority.Regions[index - 1].Scope, region.Scope)))
					return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.regions");
				if (procedural::RequiredRegionBounds(out, region.Scope, owner, query, diagnostic) !=
					procedural::Status::Ok)
					return false;
			}
			diagnostic = {};
			return true;
		}

		namespace {
			void WriteScope(core::ByteWriter &writer, procedural::RegionScope scope) {
				writer.WriteInt32(scope.X);
				writer.WriteInt32(scope.Y);
				writer.WriteUInt16(scope.CellsPerSide);
			}
			procedural::RegionScope ReadScope(core::ByteReader &reader) {
				return {reader.ReadInt32(), reader.ReadInt32(), reader.ReadUInt16()};
			}
			bool KeyScopeMatches(std::span<const uint8_t> key, procedural::RegionScope scope) {
				core::ByteReader reader(std::as_bytes(key));
				if (reader.ReadUInt32() != 0x314b4752) return false;
				const auto decoded = ReadScope(reader);
				return !reader.Failed() && SameScope(decoded, scope);
			}
			bool ReadText(core::ByteReader &reader, std::string &out, bool empty = false) {
				const auto text = reader.ReadString();
				if (reader.Failed() || text.size() > 128 ||
					(!text.empty() && !procedural::ValidStableName(text)) || (!empty && text.empty()))
					return false;
				out.assign(text);
				return true;
			}
			std::vector<uint8_t> BytesOf(const core::ByteWriter &writer) {
				const auto bytes = writer.Bytes();
				const auto *first = reinterpret_cast<const uint8_t *>(bytes.data());
				return {first, first + bytes.size()};
			}
		}

		bool WriteAuthority(
			const ProceduralAuthority &authority,
			std::vector<uint8_t> &out,
			procedural::Diagnostic &diagnostic
		) {
			procedural::CompiledRecipe checked;
			if (!CompileAuthority(authority, checked, diagnostic)) return false;
			try {
				core::ByteWriter writer(0, ProceduralHostLimits::MaximumAuthorityBytes);
				writer.WriteUInt32(0x31485750); // PWH1, authored procedural world facts.
				writer.WriteString(authority.Owner);
				writer.WriteUInt32(static_cast<uint32_t>(authority.RecipeBytes.size()));
				writer.WriteRaw(authority.RecipeBytes.data(), authority.RecipeBytes.size());
				writer.WriteUInt16(static_cast<uint16_t>(authority.Sources.size()));
				for (const auto &source : authority.Sources) {
					writer.WriteString(
						source.Kind == procedural::SourceKind::Village ? "village" : "entrance"
					);
					writer.WriteString(source.Name);
					writer.WriteInt64(source.Anchor.X);
					writer.WriteInt64(source.Anchor.Y);
					writer.WriteString(source.TargetVillage);
				}
				writer.WriteUInt16(static_cast<uint16_t>(authority.Regions.size()));
				for (const auto &region : authority.Regions)
					WriteScope(writer, region.Scope);
				out = BytesOf(writer);
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.bytes");
			} catch (const std::length_error &) {
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.bytes");
			}
		}

		bool ReadAuthority(
			std::span<const uint8_t> bytes, ProceduralAuthority &out, procedural::Diagnostic &diagnostic
		) {
			if (bytes.size() > ProceduralHostLimits::MaximumAuthorityBytes)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.bytes");
			try {
				core::ByteReader reader(std::as_bytes(bytes));
				ProceduralAuthority candidate;
				if (reader.ReadUInt32() != 0x31485750 || !ReadText(reader, candidate.Owner))
					return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.header");
				const size_t recipeBytes = reader.ReadUInt32();
				if (recipeBytes > 32768 || recipeBytes > reader.Remaining())
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.recipe");
				const auto recipe = reader.ReadRawView(recipeBytes);
				candidate.RecipeBytes.assign(
					reinterpret_cast<const uint8_t *>(recipe.data()),
					reinterpret_cast<const uint8_t *>(recipe.data()) + recipe.size()
				);
				const size_t sourceCount = reader.ReadUInt16();
				if (sourceCount > ProceduralHostLimits::MaximumSources ||
					sourceCount > reader.Remaining() / 28)
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.sources");
				candidate.Sources.reserve(sourceCount);
				for (size_t index = 0; index < sourceCount; ++index) {
					ProceduralSource source;
					std::string kind;
					if (!ReadText(reader, kind) || (kind != "village" && kind != "entrance") ||
						!ReadText(reader, source.Name))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.source");
					source.Kind = kind == "village" ? procedural::SourceKind::Village
													: procedural::SourceKind::Entrance;
					source.Anchor = {reader.ReadInt64(), reader.ReadInt64()};
					if (!ReadText(reader, source.TargetVillage, true))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.source");
					candidate.Sources.push_back(std::move(source));
				}
				const size_t regionCount = reader.ReadUInt16();
				if (regionCount > ProceduralHostLimits::MaximumRegions ||
					regionCount > reader.Remaining() / 10)
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "authority.regions");
				candidate.Regions.reserve(regionCount);
				for (size_t index = 0; index < regionCount; ++index)
					candidate.Regions.push_back({ReadScope(reader), 1});
				procedural::CompiledRecipe compiled;
				if (reader.Failed() || reader.Remaining() != 0)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "authority.trailing");
				candidate.Incarnation = NextIncarnation();
				if (!CompileAuthority(candidate, compiled, diagnostic)) return false;
				out = std::move(candidate);
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "authority.bytes");
			}
		}

		bool WritePublished(
			const ProceduralPublished &published,
			std::vector<uint8_t> &out,
			procedural::Diagnostic &diagnostic
		) {
			if (published.Regions.size() > ProceduralHostLimits::MaximumRegions)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "published.regions");
			try {
				core::ByteWriter writer(0, ProceduralHostLimits::MaximumPublishedBytes + 8192);
				writer.WriteUInt32(0x31505750); // PWP1, accepted immutable region artifacts.
				writer.WriteUInt16(static_cast<uint16_t>(published.Regions.size()));
				size_t totalBytes = 0;
				for (size_t index = 0; index < published.Regions.size(); ++index) {
					const auto &region = published.Regions[index];
					if (region.Scope.CellsPerSide == 0 || region.Scope.CellsPerSide > 64 ||
						(index > 0 && !ScopeLess(published.Regions[index - 1].Scope, region.Scope)) ||
						region.ArtifactBytes.size() >
							ProceduralHostLimits::MaximumPublishedBytes - totalBytes ||
						Hash(region.ArtifactBytes) != region.ArtifactDigest)
						return Refuse(diagnostic, procedural::Status::InvalidInput, "published.artifact");
					procedural::RegionResult decoded;
					if (procedural::DecodeRegionResult(region.ArtifactBytes, decoded, diagnostic) !=
							procedural::Status::Ok ||
						Hash(decoded.KeyBytes) != region.GenerationKey ||
						!KeyScopeMatches(decoded.KeyBytes, region.Scope))
						return Refuse(diagnostic, procedural::Status::InvalidInput, "published.key");
					totalBytes += region.ArtifactBytes.size();
					WriteScope(writer, region.Scope);
					writer.WriteRaw(region.GenerationKey.Bytes.data(), region.GenerationKey.Bytes.size());
					writer.WriteRaw(region.ArtifactDigest.Bytes.data(), region.ArtifactDigest.Bytes.size());
					writer.WriteUInt64(region.AcceptedRevision);
					writer.WriteUInt64(region.ActivationTick);
					writer.WriteUInt32(static_cast<uint32_t>(region.ArtifactBytes.size()));
					writer.WriteRaw(region.ArtifactBytes.data(), region.ArtifactBytes.size());
				}
				out = BytesOf(writer);
				diagnostic = {};
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "published.bytes");
			} catch (const std::length_error &) {
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "published.bytes");
			}
		}

		bool ReadPublished(
			std::span<const uint8_t> bytes, ProceduralPublished &out, procedural::Diagnostic &diagnostic
		) {
			if (bytes.size() > ProceduralHostLimits::MaximumPublishedBytes + 8192)
				return Refuse(diagnostic, procedural::Status::LimitExceeded, "published.bytes");
			try {
				core::ByteReader reader(std::as_bytes(bytes));
				if (reader.ReadUInt32() != 0x31505750)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "published.header");
				const size_t count = reader.ReadUInt16();
				if (count > ProceduralHostLimits::MaximumRegions || count > reader.Remaining() / 94)
					return Refuse(diagnostic, procedural::Status::LimitExceeded, "published.regions");
				ProceduralPublished candidate;
				candidate.Regions.reserve(count);
				size_t totalBytes = 0;
				for (size_t index = 0; index < count; ++index) {
					ProceduralPublication publication;
					publication.Scope = ReadScope(reader);
					reader.ReadRaw(
						publication.GenerationKey.Bytes.data(), publication.GenerationKey.Bytes.size()
					);
					reader.ReadRaw(
						publication.ArtifactDigest.Bytes.data(), publication.ArtifactDigest.Bytes.size()
					);
					publication.AcceptedRevision = reader.ReadUInt64();
					publication.ActivationTick = reader.ReadUInt64();
					const size_t length = reader.ReadUInt32();
					if (length > 1048576 || length > reader.Remaining() ||
						length > ProceduralHostLimits::MaximumPublishedBytes - totalBytes)
						return Refuse(diagnostic, procedural::Status::LimitExceeded, "published.artifact");
					totalBytes += length;
					const auto artifact = reader.ReadRawView(length);
					publication.ArtifactBytes.assign(
						reinterpret_cast<const uint8_t *>(artifact.data()),
						reinterpret_cast<const uint8_t *>(artifact.data()) + artifact.size()
					);
					candidate.Regions.push_back(std::move(publication));
				}
				if (reader.Failed() || reader.Remaining() != 0)
					return Refuse(diagnostic, procedural::Status::InvalidInput, "published.trailing");
				std::vector<uint8_t> canonical;
				if (!WritePublished(candidate, canonical, diagnostic)) return false;
				out = std::move(candidate);
				return true;
			} catch (const std::bad_alloc &) {
				return Refuse(diagnostic, procedural::Status::AllocationFailed, "published.bytes");
			}
		}
	}

	namespace {
		template <class T, auto Encode>
		void WriteResource(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *values = static_cast<const T *>(source);
			for (size_t index = 0; index < count; ++index) {
				std::vector<uint8_t> bytes;
				procedural::Diagnostic diagnostic;
				if (!Encode(values[index], bytes, diagnostic))
					throw std::length_error("invalid procedural resource");
				writer.WriteUInt32(static_cast<uint32_t>(bytes.size()));
				writer.WriteRaw(bytes.data(), bytes.size());
			}
		}
		template <class T, auto Decode, size_t Maximum>
		void ReadResource(core::ByteReader &reader, void *destination, size_t count) {
			auto *values = static_cast<T *>(destination);
			for (size_t index = 0; index < count; ++index) {
				const size_t length = reader.ReadUInt32();
				if (length > Maximum || length > reader.Remaining()) {
					reader.Fail();
					return;
				}
				const auto bytes = reader.ReadRawView(length);
				procedural::Diagnostic diagnostic;
				if (!Decode(
						std::span(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()),
						values[index],
						diagnostic
					)) {
					reader.Fail();
					return;
				}
			}
		}
	}

	void RegisterProceduralComponents() {
		using namespace procedural_detail;
		ecs::Components::Register<ProceduralAuthority>(
			"script.ProceduralAuthority",
			WriteResource<ProceduralAuthority, WriteAuthority>,
			ReadResource<ProceduralAuthority, ReadAuthority, ProceduralHostLimits::MaximumAuthorityBytes>,
			ProceduralHostLimits::MaximumAuthorityBytes + 4
		);
		ecs::Components::Register<ProceduralPublished>(
			"script.ProceduralPublished",
			WriteResource<ProceduralPublished, WritePublished>,
			ReadResource<
				ProceduralPublished,
				ReadPublished,
				ProceduralHostLimits::MaximumPublishedBytes + 8192>,
			ProceduralHostLimits::MaximumPublishedBytes + 8196
		);
	}
}
