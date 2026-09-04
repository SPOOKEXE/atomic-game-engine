#include <engine/core/Bytes.hpp>
#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/script/Clock.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/script/TeleportRequest.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

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

		// The path and the text, both written out rather than memcpy'd.
		//
		// A `core::Name` is an id in memory and text on a wire - the warning
		// `ecs::DescribeType` carries - and a `std::string` is a pointer either
		// way, so neither half of this row could cross as its object
		// representation.
		void WritePrograms(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *programs = static_cast<const Program *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(programs[index].Path);
				writer.WriteString(programs[index].Text);
			}
		}

		void ReadPrograms(core::ByteReader &reader, void *destination, size_t count) {
			auto *programs = static_cast<Program *>(destination);
			for (size_t index = 0; index < count; index++) {
				programs[index].Path = reader.ReadName();
				programs[index].Text = std::string(reader.ReadString());
			}
		}

		// The path as its text, for the same reason every other row holding a
		// name has a writer of its own.
		//
		// **This was raw serialisation until v0.15 and it was wrong in two
		// places at once.** A `core::Name` is a process-local counter, so the
		// object representation put an *interning index* into a save file and
		// onto a wire: a game file written by one process and read by another
		// resolved a script's path to whatever that process had interned in that
		// slot, and a replicated container would have named a different file on
		// every client. `ecs::Store::SNAPSHOT_VERSION` 4 and
		// `replication::PROTOCOL_VERSION` 9 are what refuse the old encoding
		// rather than misreading it.
		void WriteLuaContainers(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *containers = static_cast<const LuaSourceContainer *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(containers[index].Path);
			}
		}

		void ReadLuaContainers(core::ByteReader &reader, void *destination, size_t count) {
			auto *containers = static_cast<LuaSourceContainer *>(destination);
			for (size_t index = 0; index < count; index++) {
				containers[index].Path = reader.ReadName();
			}
		}

		void WriteJavaScriptContainers(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *containers = static_cast<const JavaScriptSourceContainer *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(containers[index].Path);
			}
		}

		void ReadJavaScriptContainers(core::ByteReader &reader, void *destination, size_t count) {
			auto *containers = static_cast<JavaScriptSourceContainer *>(destination);
			for (size_t index = 0; index < count; index++) {
				containers[index].Path = reader.ReadName();
			}
		}
	}

	void SourceCache::Set(core::Name path, std::string_view text) {
		// Bumped even when the text is the same, because the alternative is a
		// comparison paid on every write to save a mirror pass that costs a name
		// compare per script. Over-reporting costs one walk; under-reporting
		// costs a client the edit.
		Generation++;

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
		Generation++;
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
		ecs::Components::Register<LuaSourceContainer>(
			"script.LuaSourceContainer", WriteLuaContainers, ReadLuaContainers
		);
		ecs::Components::Register<JavaScriptSourceContainer>(
			"script.JavaScriptSourceContainer", WriteJavaScriptContainers, ReadJavaScriptContainers
		);
		ecs::Components::Register<CodeSourceContainerSelector>("script.CodeSourceContainerSelector");
		ecs::Components::Register<Disabled>("script.Disabled");
		ecs::Components::Register<SourceCache>("script.SourceCache", WriteSourceCaches, ReadSourceCaches);

		// After the containers, because the order decides component ids and the
		// rule every registration list in this engine follows is **add at the
		// end**.
		ecs::Components::Register<Program>("script.Program", WritePrograms, ReadPrograms);
		ecs::Components::Register<ScriptClock>("script.ScriptClock");
		RegisterTeleportRequestComponents();
	}

	void MirrorSourcePrograms(ecs::Store &store, SourceMirror &mirror) {
		if (store.AdoptOnly()) {
			return;
		}

		// **Asked by name, and this is not defensive programming.** Every
		// `Components::Of<T>` below - including the one inside
		// `Store::Resource<SourceCache>` - registers `T` under the *compiler's*
		// spelling of it when nothing has named it yet, and the explicit
		// `Register<SourceCache>("script.SourceCache")` that comes later then
		// aborts the process with "a type has one name". This runs at the head
		// of every beat, including in a world whose host never opened a script
		// class, so it is the first thing in the process that could do that.
		//
		// A process with no script components registered holds no script
		// instances either, so there is nothing here to mirror.
		static const core::Name CONTAINER("script.LuaSourceContainer");
		if (!ecs::Components::Find(CONTAINER).IsValid()) {
			return;
		}

		const SourceCache *cache = store.Resource<SourceCache>();
		const uint64_t generation = cache != nullptr ? cache->Generation : 0;
		const bool rebuild = generation != mirror.Generation;

		// **Collected first and written after**, because adding a `Program` to
		// an instance that has none moves its row to another archetype, and a
		// structural change inside `Each` is deferred - which would make the
		// state this function reads back a tick later than the state it wrote.
		std::vector<std::pair<ecs::Entity, core::Name>> pending;

		const ecs::ClassId local = LocalScriptClass();
		const ecs::ClassId module = ModuleScriptClass();

		// The Luau container is in every script class's component set, whichever
		// language the instance is set to run - `ScriptsIn` walks the same way
		// and for the same reason.
		store.Each<const LuaSourceContainer>([&](ecs::Entity instance, const LuaSourceContainer &) {
			const core::Name path = ActiveSourceOf(store, instance);
			if (!path.IsValid()) {
				return;
			}

			// **The cheapest question first, and it is the one that answers on
			// every ordinary tick.** A row that already names this path is
			// finished unless the table moved, so the class lookup below - which
			// is a class id and two `IsA` walks - is only paid for a script that
			// needs looking at. Ordering it the other way measured 45 µs a tick
			// over fifty scripts against 29 µs, at `-O0`, for the same answer.
			const Program *held = store.Get<Program>(instance);
			if (!rebuild && held != nullptr && held->Path == path) {
				return;
			}

			const ecs::ClassId owner = store.ClassOf(instance);
			if (!ecs::Classes::IsA(owner, local) && !ecs::Classes::IsA(owner, module)) {
				return;
			}

			pending.emplace_back(instance, path);
		});

		std::string text;
		std::string error;
		for (const std::pair<ecs::Entity, core::Name> &entry : pending) {
			if (!ReadSource(store, entry.second, text, error)) {
				continue;
			}

			// **Written only when it differs, and that is what bounds the
			// wire.** A rebuild is triggered by any write to the table, so one
			// script saved in an editor would otherwise mark every script in the
			// world changed and send every program again.
			const Program *held = store.Get<Program>(entry.first);
			if (held != nullptr && held->Path == entry.second && held->Text == text) {
				continue;
			}

			store.Set(entry.first, Program{entry.second, text});
		}

		mirror.Generation = generation;
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

	bool ReadProgram(
		const ecs::Store &store, ecs::Entity instance, core::Name &path, std::string &out, std::string &error
	) {
		out.clear();
		error.clear();

		path = ActiveSourceOf(store, instance);
		if (!path.IsValid()) {
			// Named, because the caller that raises this is `require` and "no
			// source path" tells an author nothing about which module.
			error = "'" + std::string(store.InstanceNameOf(instance).Text()) + "' has no source";
			return false;
		}

		if (const SourceCache *cache = store.Resource<SourceCache>()) {
			if (const std::string *text = cache->Find(path)) {
				out = *text;
				return true;
			}
		}

		// **The path is compared rather than trusted.** A row is a mirror of
		// whatever the instance pointed at when it was written, and an author
		// who has since pointed the instance somewhere else must not get the old
		// program back - which is exactly what a replica would do with a
		// container that changed and a row that had not caught up.
		if (const Program *program = store.Get<Program>(instance); program != nullptr) {
			if (program->Path == path) {
				out = program->Text;
				return true;
			}
		}

		return ReadSource(store, path, out, error);
	}
}
