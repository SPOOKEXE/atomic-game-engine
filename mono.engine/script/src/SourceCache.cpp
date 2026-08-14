#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace engine::script {

	namespace {
		// Written as text with a length, never as the object representation.
		//
		// The row holds a `core::Name` and a `std::string`, and both would be a
		// pointer on the wire - the same reason `scene` registers explicit
		// writers for everything holding a name. `WriteName` writes the text;
		// `WriteString` writes a length and the bytes.
		void WriteSourceCaches(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *caches = static_cast<const SourceCache *>(source);
			for (size_t index = 0; index < count; index++) {
				const SourceCache &cache = caches[index];
				writer.WriteUInt32(static_cast<uint32_t>(cache.Rows.size()));

				// Insertion order, which is program order. Sorting here would
				// only hide a table that had been built differently, and two
				// loads of one game file writing different bytes is exactly the
				// thing a byte-identical snapshot is supposed to catch.
				for (const SourceRow &row : cache.Rows) {
					writer.WriteName(row.Path);
					writer.WriteString(row.Text);
				}
			}
		}

		void ReadSourceCaches(core::ByteReader &reader, void *destination, size_t count) {
			auto *caches = static_cast<SourceCache *>(destination);
			for (size_t index = 0; index < count; index++) {
				SourceCache &cache = caches[index];
				cache.Rows.clear();

				const uint32_t rows = reader.ReadUInt32();
				for (uint32_t at = 0; at < rows && !reader.Failed(); at++) {
					SourceRow row;
					row.Path = reader.ReadName();
					row.Text = std::string(reader.ReadString());
					cache.Rows.push_back(std::move(row));
				}

				// A truncated buffer leaves the reader failed and this table
				// empty rather than half a game's scripts. Same rule the store
				// holds for a snapshot: partly one thing and partly another
				// looks like it works.
				if (reader.Failed()) {
					cache.Rows.clear();
				}
			}
		}
	}

	void SourceCache::Set(core::Name path, std::string_view text) {
		for (SourceRow &row : Rows) {
			if (row.Path == path) {
				row.Text.assign(text);
				return;
			}
		}
		Rows.push_back(SourceRow{path, std::string(text)});
	}

	const std::string *SourceCache::Find(core::Name path) const {
		for (const SourceRow &row : Rows) {
			if (row.Path == path) {
				return &row.Text;
			}
		}
		return nullptr;
	}

	bool SourceCache::Erase(core::Name path) {
		const auto found =
			std::find_if(Rows.begin(), Rows.end(), [path](const SourceRow &row) { return row.Path == path; });

		if (found == Rows.end()) {
			return false;
		}

		// Erased rather than swapped to the back, because insertion order is
		// the determinism this table's serialisation depends on.
		Rows.erase(found);
		return true;
	}

	void RegisterScriptComponents() {
		// **Three where there was one `script.Source`.** An instance holds a
		// program per language and something choosing between them -
		// `LuaSourceContainer` carries why that is not one field with a flag.
		// A game file written before this reads as a world of scripts with no
		// containers, which is a world of empty scripts rather than a world that
		// refuses to load: save-format breaks are acceptable while this is
		// pre-release, and this one is visible rather than silent.
		ecs::Components::Register<LuaSourceContainer>("script.LuaSourceContainer");
		ecs::Components::Register<JavaScriptSourceContainer>("script.JavaScriptSourceContainer");
		ecs::Components::Register<CodeSourceContainerSelector>("script.CodeSourceContainerSelector");
		ecs::Components::Register<Disabled>("script.Disabled");
		ecs::Components::Register<SourceCache>("script.SourceCache", WriteSourceCaches, ReadSourceCaches);
	}

	bool ReadSource(const ecs::Store &store, core::Name path, std::string &out, std::string &error) {
		out.clear();
		error.clear();

		if (!path.IsValid()) {
			error = "no source path";
			return false;
		}

		// **The cache first, and a hit never touches the disk.** That ordering
		// is what makes an unsaved edit in the studio the thing that runs, and
		// it is why `ReadSource` exists as one function instead of two callers
		// each remembering to check.
		if (const auto *cache = store.Resource<SourceCache>()) {
			if (const std::string *text = cache->Find(path)) {
				out = *text;
				return true;
			}
		}

		// An absolute path is used as it stands. `operator/` already drops the
		// left side for an absolute right side, so this is what the standard
		// does anyway - said out loud because a reader checking whether
		// `--script /tmp/x.luau` works should not have to know that.
		const std::filesystem::path named(path.Text());
		const std::filesystem::path resolved = named.is_absolute() ? named : core::Paths::Assets() / named;

		std::ifstream file(resolved, std::ios::binary);
		if (!file) {
			error = "could not open " + resolved.string();
			return false;
		}

		std::ostringstream contents;
		contents << file.rdbuf();
		out = contents.str();
		return true;
	}
}
