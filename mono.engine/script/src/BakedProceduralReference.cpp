#include <engine/assets/ContentForm.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/BakedProceduralReference.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <limits>
#include <new>

namespace engine::script {
	namespace baked_reference_detail {
		constexpr uint32_t ENCODING_VERSION = 2;
		constexpr size_t MAXIMUM_NAME_BYTES = 512;

		bool ValidName(std::string_view name) {
			if (name.empty() || name.size() > MAXIMUM_NAME_BYTES ||
				assets::FormOfName(name) != assets::ContentForm::AProcedural || name.front() == '/' ||
				name.find('\\') != name.npos || name.find(':') != name.npos || name.find('\0') != name.npos)
				return false;
			size_t begin = 0;
			while (begin < name.size()) {
				const size_t end = name.find('/', begin);
				const auto part = name.substr(begin, end == name.npos ? name.npos : end - begin);
				if (part.empty() || part == "." || part == "..") return false;
				if (end == name.npos) break;
				begin = end + 1;
			}
			return name.back() != '/';
		}

		bool Valid(const BakedProceduralReference &reference) {
			return reference.Part != ecs::NULL_ENTITY && reference.Revision != 0 &&
				   reference.Scope.CellsPerSide > 0 && reference.Scope.CellsPerSide <= 64 &&
				   ValidName(reference.Description.Text());
		}

		bool Unconfigured(const BakedProceduralReference &reference) {
			return !reference.Description.IsValid() && reference.Scope.X == 0 && reference.Scope.Y == 0 &&
				   reference.Scope.CellsPerSide == procedural::RegionScope{}.CellsPerSide &&
				   reference.Part == ecs::NULL_ENTITY && reference.Revision == 0;
		}

		void Write(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *rows = static_cast<const BakedProceduralReference *>(source);
			for (size_t i = 0; i < count; ++i) {
				writer.WriteUInt32(ENCODING_VERSION);
				writer.WriteName(rows[i].Description);
				writer.WriteInt32(rows[i].Scope.X);
				writer.WriteInt32(rows[i].Scope.Y);
				writer.WriteUInt16(rows[i].Scope.CellsPerSide);
				writer.WriteUInt64(rows[i].Part.Id);
				writer.WriteUInt64(rows[i].Revision);
			}
		}

		void Read(core::ByteReader &reader, void *destination, size_t count) {
			auto *rows = static_cast<BakedProceduralReference *>(destination);
			for (size_t i = 0; i < count; ++i) {
				if (reader.ReadUInt32() != ENCODING_VERSION) {
					reader.Fail();
					return;
				}
				BakedProceduralReference candidate;
				const auto name = reader.ReadString();
				candidate.Scope.X = reader.ReadInt32();
				candidate.Scope.Y = reader.ReadInt32();
				candidate.Scope.CellsPerSide = reader.ReadUInt16();
				candidate.Part = ecs::Entity(reader.ReadUInt64());
				candidate.Revision = reader.ReadUInt64();
				if (reader.Failed()) return;
				if (name.empty()) {
					// Only the exact default is an inert snapshot value. A partial
					// demand must not become a loadable row.
					if (!Unconfigured(candidate)) {
						reader.Fail();
						return;
					}
					rows[i] = candidate;
					continue;
				}
				if (!ValidName(name)) {
					reader.Fail();
					return;
				}
				try {
					candidate.Description = core::Name(name);
				} catch (const std::bad_alloc &) {
					reader.Fail();
					return;
				}
				if (!Valid(candidate)) {
					reader.Fail();
					return;
				}
				rows[i] = candidate;
			}
		}
	}

	void RegisterBakedProceduralReferenceComponents() {
		ecs::Components::Register<BakedProceduralReference>(
			"script.BakedProceduralReference", baked_reference_detail::Write, baked_reference_detail::Read
		);
	}

	bool SetBakedProceduralReference(
		ecs::Store &store,
		procedural::RegionScope scope,
		ecs::Entity mesh,
		ecs::Entity part,
		std::string_view descriptionName,
		procedural::Diagnostic &diagnostic
	) {
		if (!baked_reference_detail::ValidName(descriptionName) || scope.CellsPerSide == 0 ||
			scope.CellsPerSide > 64 || !ecs::Components::Assigned<BakedProceduralReference>().IsValid()) {
			diagnostic.Code = procedural::Status::InvalidInput;
			diagnostic.Field = "baked.reference";
			return false;
		}
		if (!store.Has<BakedProceduralReference>(mesh)) {
			size_t count = 0;
			store.Each<const BakedProceduralReference>([&](ecs::Entity, const BakedProceduralReference &) {
				++count;
			});
			if (count >= MAXIMUM_BAKED_PROCEDURAL_REFERENCES) {
				diagnostic.Code = procedural::Status::LimitExceeded;
				diagnostic.Field = "baked.references";
				return false;
			}
		}
		const auto *prior = store.Get<BakedProceduralReference>(mesh);
		if (prior && prior->Revision == std::numeric_limits<uint64_t>::max()) {
			diagnostic.Code = procedural::Status::LimitExceeded;
			diagnostic.Field = "baked.revision";
			return false;
		}
		try {
			const core::Name name(descriptionName);
			const BakedProceduralReference candidate{
				name, scope, part, prior ? prior->Revision + 1 : uint64_t{1}
			};
			ProceduralSceneTarget target;
			if (!CaptureProceduralSceneTarget(store, scope, mesh, part, target, diagnostic)) return false;
			store.Set(mesh, candidate);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic.Code = procedural::Status::AllocationFailed;
			diagnostic.Field = "baked.reference_storage";
			return false;
		}
	}
}
