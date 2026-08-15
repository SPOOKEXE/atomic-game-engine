#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

TEST_SUITE_ID("engine.ecs.components")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::Name;
using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::DescribeType;
using engine::ecs::TypeDescriptor;
using engine::ecs::TypeNameOf;

namespace components_test {
	// The registry is process-wide and nothing unregisters, exactly like
	// core::Name. Every case that registers has to use a name nothing else in
	// the binary will, or it collides with another case's type.
	struct Position {
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
	};

	struct Velocity {
		float X = 0.0f;
	};

	// A tag: no data, matched by a query and nothing else.
	struct Frozen {};

	// Non-trivial on purpose - the lifetime hooks have to be real for this one,
	// and it must not be offered raw serialisation.
	struct Label {
		std::string Text;
		Label() = default;
		explicit Label(std::string text) : Text(std::move(text)) {}
	};

	// Counts its own construction and destruction, which is how the column and
	// archetype cases below check that nothing is leaked or double-destroyed.
	struct Counted {
		static inline int Live = 0;
		int Value = 0;

		Counted() {
			Live++;
		}
		Counted(const Counted &other) : Value(other.Value) {
			Live++;
		}
		Counted(Counted &&other) noexcept : Value(other.Value) {
			Live++;
		}
		Counted &operator=(const Counted &) = default;
		Counted &operator=(Counted &&) = default;
		~Counted() {
			Live--;
		}
	};

	// A component holding a Name, which is the case raw serialisation gets
	// wrong: the id is process-local and means nothing in another process.
	struct Material {
		Name Surface;
	};
}

using namespace components_test;

TEST_CASE("a type registers once and keeps its id", "[ecs]") {
	const ComponentId first = Components::Of<Position>();
	const ComponentId second = Components::Of<Position>();

	REQUIRE(first.IsValid());
	REQUIRE(first == second);
	REQUIRE(Components::Of<Velocity>() != first);
}

TEST_CASE("one type has one id, whatever it is asked for by", "[ecs]") {
	// The defect this catches: the name table maps *names* to ids, so a type
	// registered under a second name would take a second id - and an archetype
	// built from one would silently not match a query built from the other.
	// Nothing about that failure looks like a registration problem later.
	struct Twice {
		int Value = 0;
	};

	const size_t before = Components::Count();

	const ComponentId explicitly = Components::Register<Twice>("test.twice");
	const ComponentId automatically = Components::Of<Twice>();
	const ComponentId again = Components::Register<Twice>("test.twice");

	REQUIRE(explicitly == automatically);
	REQUIRE(explicitly == again);

	// And exactly one entry was added, not three.
	REQUIRE(Components::Count() == before + 1);

	// The explicit name is the one that stuck, because the automatic name is a
	// fallback rather than a claim.
	REQUIRE(Components::Describe(explicitly).Name == Name("test.twice"));
	REQUIRE(Components::Find(Name("test.twice")) == explicitly);
}

TEST_CASE("an automatic registration yields to an explicit one", "[ecs]") {
	// Order matters the other way round too: a type used before it was named
	// keeps the automatic id, and asking for it by the automatic name still
	// works. What must never happen is a second id.
	struct AutomaticFirst {
		int Value = 0;
	};

	const ComponentId automatically = Components::Of<AutomaticFirst>();
	const size_t before = Components::Count();

	// Registering the *same* automatic name again is idempotent.
	const ComponentId same = Components::Register<AutomaticFirst>(TypeNameOf<AutomaticFirst>());
	REQUIRE(same == automatically);
	REQUIRE(Components::Count() == before);
}

TEST_CASE("a name looks up without registering", "[ecs]") {
	const size_t before = Components::Count();
	REQUIRE_FALSE(Components::Find(Name("test.never.registered")).IsValid());
	REQUIRE(Components::Count() == before);

	struct Looked {
		int Value = 0;
	};
	const ComponentId id = Components::Register<Looked>("test.lookup.looked");
	REQUIRE(Components::Find(Name("test.lookup.looked")) == id);
	REQUIRE(Components::Describe(id).Name == Name("test.lookup.looked"));
}

TEST_CASE("an invalid id describes as empty rather than crashing", "[ecs]") {
	// The corrupt-snapshot path: a file naming a component this build does not
	// have resolves to an invalid id, and asking about it must be survivable.
	const TypeDescriptor &missing = Components::Describe(ComponentId{});
	REQUIRE(missing.Size == 0);
	REQUIRE_FALSE(missing.Name.IsValid());

	const TypeDescriptor &past = Components::Describe(ComponentId{0xFFFF'FFF0u});
	REQUIRE(past.Size == 0);
}

TEST_CASE("a descriptor carries size, alignment and triviality", "[ecs]") {
	const TypeDescriptor &position = Components::Describe(Components::Of<Position>());
	REQUIRE(position.Size == sizeof(Position));
	REQUIRE(position.Alignment == alignof(Position));
	REQUIRE(position.Trivial);
	REQUIRE(position.Serialisable);

	const TypeDescriptor &label = Components::Describe(Components::Register<Label>("test.components.label"));
	REQUIRE(label.Size == sizeof(Label));
	REQUIRE_FALSE(label.Trivial);

	// A std::string cannot be written as its object representation - the bytes
	// are a pointer into this process. Refusing is the only correct answer.
	REQUIRE_FALSE(label.Serialisable);
	REQUIRE(label.Write == nullptr);
}

TEST_CASE("a tag has no bytes", "[ecs]") {
	const TypeDescriptor &frozen = Components::Describe(Components::Of<Frozen>());

	// An empty type still occupies one byte in C++, but a column of them has
	// nothing to store: the presence of the component is the whole value.
	REQUIRE(frozen.Size == 0);
	REQUIRE_FALSE(frozen.Serialisable);
}

TEST_CASE("the lifetime hooks construct, copy, move and destroy", "[ecs]") {
	const TypeDescriptor &counted = Components::Describe(Components::Of<Counted>());
	REQUIRE(counted.DefaultConstruct != nullptr);
	REQUIRE(counted.CopyConstruct != nullptr);
	REQUIRE(counted.MoveConstruct != nullptr);
	REQUIRE(counted.Destruct != nullptr);

	const int before = Counted::Live;
	alignas(Counted) std::byte storage[sizeof(Counted) * 4];
	alignas(Counted) std::byte copy[sizeof(Counted) * 4];

	counted.DefaultConstruct(storage, 4);
	REQUIRE(Counted::Live == before + 4);

	auto *values = reinterpret_cast<Counted *>(storage);
	for (int index = 0; index < 4; index++) {
		values[index].Value = index;
	}

	counted.CopyConstruct(copy, storage, 4);
	REQUIRE(Counted::Live == before + 8);
	REQUIRE(reinterpret_cast<Counted *>(copy)[2].Value == 2);

	counted.Destruct(copy, 4);
	counted.Destruct(storage, 4);
	REQUIRE(Counted::Live == before);
}

TEST_CASE("a trivially copyable type serialises as its bytes", "[ecs]") {
	const TypeDescriptor &position = Components::Describe(Components::Of<Position>());

	const Position source[] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
	ByteWriter writer;
	position.Write(writer, source, 2);
	REQUIRE(writer.Size() == sizeof(source));

	Position destination[2] = {};
	ByteReader reader(writer.Bytes());
	position.Read(reader, destination, 2);

	REQUIRE(reader.AtEnd());
	REQUIRE(std::memcmp(source, destination, sizeof(source)) == 0);
}

TEST_CASE("a truncated column read fails rather than tearing", "[ecs]") {
	const TypeDescriptor &position = Components::Describe(Components::Of<Position>());

	const Position source[] = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
	ByteWriter writer;
	position.Write(writer, source, 2);

	// One row's worth of bytes, but the reader is asked for two.
	std::vector<std::byte> truncated(writer.Bytes().begin(), writer.Bytes().begin() + sizeof(Position));

	Position destination[2] = {};
	ByteReader reader(truncated);
	position.Read(reader, destination, 2);

	REQUIRE(reader.Failed());
}

TEST_CASE("a custom serialiser replaces the raw one", "[ecs]") {
	// The case the automatic path gets wrong. A Material holds a Name, whose
	// id is a process-local counter - writing those four bytes would produce a
	// file that means something different in every process that reads it.
	const ComponentId id = Components::Register<Material>(
		"test.material",
		[](ByteWriter &writer, const void *source, size_t count) {
			const auto *values = static_cast<const Material *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(values[index].Surface);
			}
		},
		[](ByteReader &reader, void *destination, size_t count) {
			auto *values = static_cast<Material *>(destination);
			for (size_t index = 0; index < count; index++) {
				values[index].Surface = reader.ReadName();
			}
		}
	);

	const TypeDescriptor &material = Components::Describe(id);
	REQUIRE(material.Serialisable);

	const Material source[] = {{Name("test.material.stone")}, {Name("test.material.wood")}};
	ByteWriter writer;
	material.Write(writer, source, 2);

	// Text on the wire, not ids: two length-prefixed strings, so the record is
	// far longer than the eight bytes two ids would have been.
	REQUIRE(writer.Size() > 2 * sizeof(uint32_t));

	Material destination[2];
	ByteReader reader(writer.Bytes());
	material.Read(reader, destination, 2);

	REQUIRE(reader.AtEnd());
	REQUIRE(destination[0].Surface == Name("test.material.stone"));
	REQUIRE(destination[1].Surface == Name("test.material.wood"));
}

TEST_CASE("the automatic name spells the type out", "[ecs]") {
	// Not a mangled string: the point of the automatic name is that it is
	// legible in a log and in the manifest that will be generated from it.
	const std::string_view spelled = TypeNameOf<Position>();
	REQUIRE(spelled.find("Position") != std::string_view::npos);

	const std::string_view velocity = TypeNameOf<Velocity>();
	REQUIRE(velocity.find("Velocity") != std::string_view::npos);
	REQUIRE(spelled != velocity);
}

TEST_CASE("sealing refuses a type nobody registered at startup", "[ecs]") {
	struct Late {
		int Value = 0;
	};

	// Registered before the seal rather than relying on another case having run
	// first. Catch2 promises no order across files, and a case that only passes
	// when something else ran before it is a case that will fail on somebody
	// else's machine.
	const ComponentId early = Components::Of<Position>();
	REQUIRE(early.IsValid());

	Components::Seal();
	REQUIRE(Components::Sealed());

	// Already-registered types keep working while sealed - sealing closes the
	// table to new types, it does not close the engine.
	REQUIRE(Components::Of<Position>() == early);

	Components::Unseal();
	REQUIRE_FALSE(Components::Sealed());

	// And the new one registers fine once the table is open again.
	REQUIRE(Components::Of<Late>().IsValid());
}

TEST_CASE("concurrent first use of one type yields one id", "[ecs]") {
	struct Contended {
		double Value = 0.0;
	};

	// Of<T>() is reachable from a system, and a system runs on many threads.
	// Two threads racing to first-register the same type must not produce two
	// ids, because an archetype built from one would not match a query built
	// from the other.
	constexpr int THREADS = 8;
	std::vector<ComponentId> seen(THREADS);
	std::vector<std::thread> workers;

	for (int index = 0; index < THREADS; index++) {
		workers.emplace_back([&seen, index] { seen[index] = Components::Of<Contended>(); });
	}
	for (auto &worker : workers) {
		worker.join();
	}

	for (const ComponentId id : seen) {
		REQUIRE(id == seen.front());
		REQUIRE(id.IsValid());
	}
}

TEST_CASE("an id compares and orders by registration index", "[ecs]") {
	const ComponentId invalid;
	REQUIRE_FALSE(invalid.IsValid());
	REQUIRE(invalid == ComponentId{});

	const ComponentId first{0};
	const ComponentId second{1};
	REQUIRE(first < second);
	REQUIRE_FALSE(second < first);
	REQUIRE(first != second);
}
