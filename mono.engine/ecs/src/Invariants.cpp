#include <engine/core/Bytes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Invariants.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <cstring>
#include <map>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::ecs {
	namespace {
		// A block of `count` values' worth of correctly aligned bytes.
		//
		// The descriptor's hooks are the only things that may touch it, because
		// the type they belong to is exactly what this file does not know.
		// Construction and destruction are the caller's to order, so that a
		// case can poison the bytes before constructing into them.
		class Block {
		  public:
			Block(const TypeDescriptor &type, size_t count)
				: Bytes(
					  static_cast<std::byte *>(
						  ::operator new(Span(type, count), std::align_val_t(type.Alignment))
					  )
				  ),
				  Length(Span(type, count)), Alignment(type.Alignment) {}

			~Block() {
				::operator delete(Bytes, std::align_val_t(Alignment));
			}

			Block(const Block &) = delete;
			Block &operator=(const Block &) = delete;

			void *Data() {
				return Bytes;
			}

			const void *Data() const {
				return Bytes;
			}

			void Fill(unsigned char value) {
				std::memset(Bytes, value, Length);
			}

		  private:
			// One byte for a tag, so that `operator new` is never asked for
			// nothing and the pointer stays distinct.
			static size_t Span(const TypeDescriptor &type, size_t count) {
				const size_t size = type.Size == 0 ? 1 : type.Size;
				return size * count;
			}

			std::byte *Bytes = nullptr;
			size_t Length = 0;
			uint32_t Alignment = 1;
		};

		bool PowerOfTwo(uint32_t value) {
			return value != 0 && (value & (value - 1)) == 0;
		}

		// The bytes `type.Write` produces for `count` default-constructed
		// values built over a buffer pre-filled with `poison`.
		//
		// The poison is the point: value-initialisation zeroes padding, so two
		// runs over clean buffers agree whether or not a member was left out.
		// Filling first means an uninitialised member keeps whatever was there,
		// and the two poisons disagree.
		std::vector<std::byte> WrittenFrom(const TypeDescriptor &type, size_t count, unsigned char poison) {
			Block block(type, count);
			block.Fill(poison);
			type.DefaultConstruct(block.Data(), count);

			core::ByteWriter writer;
			type.Write(writer, block.Data(), count);

			const std::span<const std::byte> bytes = writer.Bytes();
			std::vector<std::byte> copy(bytes.begin(), bytes.end());

			type.Destruct(block.Data(), count);
			return copy;
		}

		void CheckShape(const TypeDescriptor &type, std::vector<ComponentComplaint> &complaints) {
			const auto complain = [&](std::string rule) {
				complaints.push_back({type.Name, std::move(rule)});
			};

			if (!PowerOfTwo(type.Alignment)) {
				complain(
					"alignment is " + std::to_string(type.Alignment) +
					", which is not a power of two - a column cannot stride by it"
				);
			}

			// A hand-built descriptor - `Schema` builds one - can get this
			// wrong in a way the compiler would not let `DescribeType`. The
			// column strides by `Size`, so a size that is not a multiple of the
			// alignment misaligns every row after the first.
			if (type.Size != 0 && PowerOfTwo(type.Alignment) && type.Size % type.Alignment != 0) {
				complain(
					"size " + std::to_string(type.Size) + " is not a multiple of alignment " +
					std::to_string(type.Alignment) +
					" - a column strides by the size, so every row after the first is misaligned"
				);
			}

			if ((type.Kind == ComponentKind::Tag) != (type.Size == 0)) {
				complain(
					"Kind and Size disagree about whether this is a tag - a tag is a type with no bytes, and "
					"the two are derived from the same fact"
				);
			}

			if (type.DefaultConstruct == nullptr || type.Destruct == nullptr) {
				complain(
					"has no DefaultConstruct or no Destruct - every registration path installs both, so a "
					"null one means the descriptor was built by hand and left incomplete"
				);
			}

			if (type.Serialisable != (type.Write != nullptr && type.Read != nullptr)) {
				complain(
					"Serialisable disagrees with whether Write and Read are installed - a snapshot trusts "
					"the flag and would either refuse a type it could carry or write bytes nothing can read "
					"back"
				);
			}

			// Deliberately not checked: whether a tag claims to be
			// serialisable. A declared tag has no `Write` at all and says
			// `false`; a described one has the hooks its blob generated and
			// says `true` while writing nothing. Both answers are accurate for
			// what the flag means, and the check above is what keeps the flag
			// honest either way.
		}

		void CheckPadding(const TypeDescriptor &type, std::vector<ComponentComplaint> &complaints) {
			if (!type.Padded || !type.RawSerialisation) {
				return;
			}

			complaints.push_back(
				{type.Name,
				 "has padding and is serialised as its object representation, so every save and every "
				 "delta carries bytes no member wrote - which differ between two runs of one scene and "
				 "make `just determinism` and every world comparison unreliable. Order the members "
				 "widest-first or add an explicit `Reserved` field, as `WorldTime` does, or register it "
				 "with a writer of its own."}
			);
		}

		void CheckDeterminism(const TypeDescriptor &type, std::vector<ComponentComplaint> &complaints) {
			if (!type.Serialisable || type.Kind == ComponentKind::Tag) {
				return;
			}

			const std::vector<std::byte> low = WrittenFrom(type, 3, 0x00);
			const std::vector<std::byte> high = WrittenFrom(type, 3, 0xFF);

			if (low != high) {
				complaints.push_back(
					{type.Name,
					 "writes different bytes for the same default-constructed value depending on what "
					 "was in the memory beforehand, so a member is being serialised without ever being "
					 "initialised. Give every member a default, or serialise the ones that have no "
					 "meaningful default explicitly."}
				);
			}
		}

		void CheckRoundTrip(const TypeDescriptor &type, std::vector<ComponentComplaint> &complaints) {
			if (!type.Serialisable || type.Kind == ComponentKind::Tag) {
				return;
			}

			constexpr size_t COUNT = 3;

			Block source(type, COUNT);
			source.Fill(0x00);
			type.DefaultConstruct(source.Data(), COUNT);

			core::ByteWriter writer;
			type.Write(writer, source.Data(), COUNT);
			type.Destruct(source.Data(), COUNT);

			Block restored(type, COUNT);
			restored.Fill(0x00);
			type.DefaultConstruct(restored.Data(), COUNT);

			core::ByteReader reader(writer.Bytes());
			type.Read(reader, restored.Data(), COUNT);

			const bool failed = reader.Failed();
			const bool short_ = !reader.AtEnd();

			core::ByteWriter again;
			type.Write(again, restored.Data(), COUNT);
			const std::span<const std::byte> first = writer.Bytes();
			const std::span<const std::byte> second = again.Bytes();
			const bool same =
				first.size() == second.size() && std::memcmp(first.data(), second.data(), first.size()) == 0;

			type.Destruct(restored.Data(), COUNT);

			if (failed) {
				complaints.push_back(
					{type.Name, "cannot read back the bytes its own writer produced - the reader failed"}
				);
				return;
			}

			if (short_) {
				complaints.push_back(
					{type.Name,
					 "reads a different number of bytes than it writes, so a snapshot holding this "
					 "beside anything else decodes the next column from the wrong offset"}
				);
			}

			if (!same) {
				complaints.push_back(
					{type.Name,
					 "does not survive a round trip - writing what its own reader produced gives "
					 "different bytes, so a value changes every time a world is saved and restored"}
				);
			}
		}

		void CheckWire(const TypeDescriptor &type, std::vector<ComponentComplaint> &complaints) {
			if (!type.Wire.Present()) {
				return;
			}

			const auto complain = [&](std::string rule) {
				complaints.push_back({type.Name, std::move(rule)});
			};

			constexpr size_t COUNT = 3;

			Block source(type, COUNT);
			source.Fill(0x00);
			type.DefaultConstruct(source.Data(), COUNT);

			core::ByteWriter writer;
			type.Wire.Write(writer, source.Data(), COUNT);
			type.Destruct(source.Data(), COUNT);

			const size_t expected = static_cast<size_t>(type.Wire.Size) * COUNT;
			if (writer.Size() != expected) {
				complain(
					"declares a wire size of " + std::to_string(type.Wire.Size) + " but writes " +
					std::to_string(writer.Size() / COUNT) +
					" bytes per value - a receiver sizes its buffer from the declared number and "
					"decodes the next component from the wrong offset"
				);
			}

			// **Totality, which the `WireFormat::Read` contract asks for in
			// words.** Every bit pattern reaches a decoder from a peer, so
			// three patterns a peer could send are decoded here: what a
			// zeroed datagram holds, what a saturated one holds, and a
			// counter, which is the one that catches a length or an index
			// being trusted. A decode that fails the reader on any of them is
			// a decode a hostile or merely unlucky peer can stall.
			const unsigned char patterns[] = {0x00, 0xFF};
			for (const unsigned char pattern : patterns) {
				std::vector<std::byte> bytes(expected, static_cast<std::byte>(pattern));
				Block target(type, COUNT);
				target.Fill(0x00);
				type.DefaultConstruct(target.Data(), COUNT);

				core::ByteReader reader(bytes);
				type.Wire.Read(reader, target.Data(), COUNT);
				const bool failed = reader.Failed();
				const bool leftOver = !reader.AtEnd();
				type.Destruct(target.Data(), COUNT);

				if (failed || leftOver) {
					complain(
						std::string("does not decode a datagram of 0x") + (pattern == 0 ? "00" : "FF") +
						" bytes cleanly, though it is exactly the size its own encoder produces - "
						"`WireFormat::Read` has to be total over its input because every bit pattern "
						"reaches it from a peer"
					);
					break;
				}
			}

			std::vector<std::byte> counted(expected);
			for (size_t index = 0; index < counted.size(); index++) {
				counted[index] = static_cast<std::byte>(index * 7 + 13);
			}

			Block target(type, COUNT);
			target.Fill(0x00);
			type.DefaultConstruct(target.Data(), COUNT);

			core::ByteReader reader(counted);
			type.Wire.Read(reader, target.Data(), COUNT);
			const bool failed = reader.Failed();
			const bool leftOver = !reader.AtEnd();

			// Re-encoding what a decode produced has to be the same size as
			// anything else this format writes, whatever the decode made of
			// those bytes. A format that widens on a value it decoded itself
			// is one a peer can use to change how much a host sends.
			core::ByteWriter reencoded;
			type.Wire.Write(reencoded, target.Data(), COUNT);
			const size_t reencodedSize = reencoded.Size();

			type.Destruct(target.Data(), COUNT);

			if (failed || leftOver) {
				complain(
					"does not decode arbitrary bytes of its own declared length cleanly - "
					"`WireFormat::Read` has to be total over its input because every bit pattern "
					"reaches it from a peer"
				);
			}

			if (reencodedSize != expected) {
				complain(
					"re-encodes a value it decoded itself into " + std::to_string(reencodedSize) +
					" bytes rather than " + std::to_string(expected) +
					" - a peer's bytes are deciding how much this host sends"
				);
			}
		}
	}

	bool AuditChecksPadding() {
		return PaddingIsDetectable();
	}

	std::vector<ComponentComplaint> AuditComponents() {
		std::vector<ComponentComplaint> complaints;

		for (size_t index = 0; index < Components::Count(); index++) {
			const TypeDescriptor &type = Components::Describe(ComponentId{static_cast<uint32_t>(index)});

			CheckShape(type, complaints);
			CheckPadding(type, complaints);

			// The three below run the descriptor's own hooks, so a descriptor
			// that failed the shape check is skipped rather than called through
			// a null pointer.
			if (type.DefaultConstruct == nullptr || type.Destruct == nullptr) {
				continue;
			}

			CheckDeterminism(type, complaints);
			CheckRoundTrip(type, complaints);
			CheckWire(type, complaints);
		}

		return complaints;
	}

	std::vector<ComponentComplaint> AuditComponents(std::string_view prefix) {
		std::vector<ComponentComplaint> kept;
		for (ComponentComplaint &complaint : AuditComponents()) {
			if (complaint.Component.Text().starts_with(prefix)) {
				kept.push_back(std::move(complaint));
			}
		}
		return kept;
	}

	std::string Describe(const std::vector<ComponentComplaint> &complaints) {
		std::string text;
		for (const ComponentComplaint &complaint : complaints) {
			if (!text.empty()) {
				text += '\n';
			}
			text += complaint.Component.Text();
			text += ": ";
			text += complaint.Rule;
		}
		return text;
	}

	namespace {
		// Whether a property's value can be compared as bytes.
		//
		// A `String` property's value is an owned `std::string`, so its object
		// representation is a pointer and a comparison over it answers about the
		// allocation. `Opaque` has no described shape at all. Everything else in
		// the closed list is a value type and comparing bytes is comparing the
		// value.
		bool ComparableAsBytes(PropertyType type) {
			return type != PropertyType::String && type != PropertyType::Opaque;
		}

		// Whether a property's value can be moved through a raw buffer.
		//
		// The same set minus nothing today, kept separate because the reasons
		// differ: the one above is about comparison and this is about whether
		// handing `Get` uninitialised bytes is safe. A `String` getter assigns
		// through a constructed `std::string`, which a byte buffer is not.
		bool SafeInRawBytes(PropertyType type) {
			return type != PropertyType::String;
		}
	}

	std::vector<PropertyComplaint> AuditProperties() {
		// How many write-backs a value gets to settle in. A conversion that
		// rounds needs one or two; anything still moving after four is moving
		// for good.
		constexpr int SETTLING_WRITES = 4;

		std::vector<PropertyComplaint> complaints;

		// One width per type across the whole table, derived rather than
		// tabulated. Two properties of one `PropertyType` disagreeing about
		// `Size` means one of them will be handed a buffer of the other's
		// length by anything marshalling by type - which is every binding.
		std::map<PropertyType, std::pair<uint32_t, core::Name>> widths;

		Store scratch("ecs.invariants");

		for (size_t index = 0; index < Classes::Count(); index++) {
			const ClassId owner{static_cast<uint32_t>(index)};
			const ClassInfo &info = Classes::Describe(owner);

			const auto complain = [&](core::Name property, std::string rule) {
				complaints.push_back({info.Name, property, std::move(rule)});
			};

			// One instance per class, reused by every property on it. Created
			// before the loop so that a class nothing can instantiate is one
			// complaint rather than one per property.
			const Entity instance = scratch.CreateInstance(owner, "probe");
			if (instance == NULL_ENTITY) {
				complain(
					core::Name{}, "no instance of this class can be created, so nothing here could be checked"
				);
				continue;
			}

			for (const PropertyDescriptor &property : info.Properties) {
				const auto fault = [&](std::string rule) { complain(property.Name, std::move(rule)); };

				if (property.Spelling != property.Name.Text()) {
					fault(
						"Spelling and Name disagree - the bindings match a script's key against the spelling "
						"and would never find this property"
					);
				}

				if (property.Type == PropertyType::Opaque || property.Size == 0) {
					fault("has no type or no width, so nothing can marshal it");
					continue;
				}

				if ((property.Type == PropertyType::Enum) != property.EnumName.IsValid()) {
					fault(
						"names an enum set and is not an Enum, or is an Enum and names no set - a value can "
						"only be checked against a set that was named"
					);
				}

				if (property.Reads == nullptr || property.Writes == nullptr) {
					fault(
						"leaves `Reads` or `Writes` null rather than interning an empty set - a caller "
						"asking what this property touches has to dereference it, and per-instance "
						"`.Changed` fans out over exactly these"
					);
					continue;
				}

				if (property.Get == nullptr) {
					fault("has no getter");
					continue;
				}

				// Deliberately not checked: whether `Reads` is a subset of the
				// class's component set. It usually is not, and that is the
				// design rather than a defect - `Anchored` reads a `RigidBody`
				// an anchored part does not have, and answers from its absence.
				// What matters is the next check: the getter has to *answer*.

				if (property.Writable && property.Set == nullptr) {
					fault(
						"is writable and has no setter, so every write is refused by a check nobody meant to "
						"make"
					);
				}

				if (property.Writable && property.Writes->IsEmpty()) {
					fault(
						"is writable and writes no component, so nothing is marked changed and `replication` "
						"sends none of it"
					);
				}

				const auto held = widths.find(property.Type);
				if (held == widths.end()) {
					widths.emplace(property.Type, std::pair{property.Size, property.Name});
				} else if (held->second.first != property.Size) {
					fault(
						"is " + std::to_string(property.Size) + " bytes where '" +
						std::string(held->second.second.Text()) + "' makes the same type " +
						std::to_string(held->second.first) +
						" - a marshaller sizing a buffer by type would overrun one of them"
					);
				}

				if (!SafeInRawBytes(property.Type)) {
					continue;
				}

				// **A getter has to answer for an ordinary instance of the
				// class that declares it.** `Store::GetProperty` returning
				// false becomes `could not read 'X'` in Luau, so a getter that
				// declines is a script error on a field access that looks
				// exactly like every other one. The convention the engine
				// already follows is `Mass`'s: an anchored part has no
				// `RigidBody` and `Mass` answers zero rather than failing,
				// "the honest number for a thing the world may not move".
				std::vector<std::byte> first(property.Size);
				if (!property.Get(scratch, instance, first.data())) {
					fault(
						"cannot be read from an ordinary instance of the class that declares it, so reading "
						"it from a script raises `could not read` rather than answering. Answer the default "
						"for a missing component, as `Mass` does"
					);
					continue;
				}

				// **A read-only property may keep a setter, and then that setter
				// has to refuse.** `PropertyDescriptor::Writable` calls a null
				// `Set` the second refusal for a caller that reached past the
				// flag; a setter that returns `false` is the same refusal
				// written the other way, and `gui`'s resolved fields use it. A
				// setter that *writes* is the case neither reading permits.
				if (!property.Writable) {
					if (property.Set != nullptr && property.Set(scratch, instance, first.data())) {
						fault(
							"is read-only and its setter accepted a write - the flag is what a panel greys "
							"out and what a script is refused by, and a caller holding the descriptor "
							"reaches past both"
						);
					}
					continue;
				}

				// **A setter may refuse when what it writes into is not
				// there, and then none of the checks below mean anything.**
				// `LinearDamping` is the case: an anchored part has no
				// `RigidBody`, so the getter answers the default a body would
				// have had and the setter has nowhere to put a new one.
				// Refusing is right, and the probe instance is the one shape
				// that shows it - so the write-back checks are skipped rather
				// than reported.
				bool writable = true;
				for (const ComponentId component : property.Writes->Ids()) {
					writable = writable && scratch.HasComponent(instance, component);
				}

				if (!writable) {
					continue;
				}

				for (const ComponentId component : property.Writes->Ids()) {
					scratch.ObserveComponent(component);
				}
				scratch.ClearChanges();

				if (!property.Set(scratch, instance, first.data())) {
					fault("refuses the value its own getter just produced");
					continue;
				}

				std::vector<std::byte> second(property.Size);
				if (!property.Get(scratch, instance, second.data())) {
					fault("cannot be read back after a write its own setter accepted");
					continue;
				}

				// **Convergence rather than exactness, because a conversion is
				// allowed to round.** `FieldOfView` stores radians and shows
				// degrees, so writing back what was read moves the last bit and
				// that is not a defect. What would be is never settling: a value
				// that moves on every assignment walks away over a session, and
				// a script doing `p.X = p.X` inside a loop is ordinary.
				if (ComparableAsBytes(property.Type)) {
					std::vector<std::byte> settled = second;
					bool converged = false;
					for (int attempt = 0; attempt < SETTLING_WRITES && !converged; attempt++) {
						if (!property.Set(scratch, instance, settled.data())) {
							break;
						}

						std::vector<std::byte> next(property.Size);
						if (!property.Get(scratch, instance, next.data())) {
							break;
						}

						converged = next == settled;
						settled = std::move(next);
					}

					if (!converged) {
						fault(
							"never settles - writing back the value its own getter produced changes it "
							"again, every time. A conversion may round once; one that moves on every "
							"assignment walks the value away over a session, and `p.X = p.X` in a loop "
							"is ordinary"
						);
					}
				}

				// Two kinds mark no dirty bit, and both say so in the
				// descriptor rather than leaving it to be inferred. A
				// `Structural` write is an archetype move - the component
				// appearing or disappearing is what replication sees, and
				// writing back the value just read changes neither. A
				// `Resource` write lands outside the entity, so there is no row
				// to mark.
				if (property.Kind == PropertyKind::Structural || property.Kind == PropertyKind::Resource) {
					continue;
				}

				bool marked = false;
				for (const ComponentId component : property.Writes->Ids()) {
					marked = marked || scratch.ChangedComponent(instance, component);
				}

				if (!marked) {
					fault(
						"marks none of the components it says it writes, so a script's assignment "
						"never reaches a delta. Reach the bytes through `Store::GetMutable` rather "
						"than any other way - handing out a mutable pointer is already counted as a "
						"write, which is what `Classes::Property` generates and what a hand-written "
						"`Computed` setter has to do too."
					);
				}
			}
		}

		return complaints;
	}

	std::string Describe(const std::vector<PropertyComplaint> &complaints) {
		std::string text;
		for (const PropertyComplaint &complaint : complaints) {
			if (!text.empty()) {
				text += '\n';
			}
			text += complaint.Class.Text();
			text += '.';
			text += complaint.Property.IsValid() ? complaint.Property.Text() : "<class>";
			text += ": ";
			text += complaint.Rule;
		}
		return text;
	}
}
