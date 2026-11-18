# Zero v0.26.1 development build turnaround

Plan for the roadmap item "Reduce development build turnaround". Proposed, not
started: rule 4 asks for approval before implementation.

## Measured (dev preset, `.ninja_log` and `ninja -t deps`, 2026-11-18)

| Quantity | Value |
|---|---|
| Compiled objects | 3,008 |
| Full rebuild compile time | 52.7 CPU-minutes |
| Link steps | 308, 83 s total, largest 2.9 s (`test_studio`) |
| Static archives | 100, 42 s total |

Linking is not the bottleneck. Turnaround after a header edit is set by how
many objects include it. Rebuild cost of one edit, as CPU-minutes of the
objects that depend on the header:

| Header | Objects | CPU-min |
|---|---|---|
| `core/Name.hpp` | 1,280 | 28.3 |
| `core/Bytes.hpp` | 1,117 | 26.6 |
| `ecs/Entity.hpp` | 1,001 | 25.1 |
| `ecs/Components.hpp`, `TypeDescriptor.hpp`, `Enums.hpp`, `Instance.hpp` | about 945 | about 24.7 |
| `core/types/Vector3.hpp`, `CFrame.hpp`, `AABB.hpp`, `Color3.hpp` | 830 to 950 | about 23.5 |
| `parallel/Jobs.hpp` | 778 | 20.7 |
| `ecs/Store.hpp` (with `Column`, `ChangeChannel`, `Time`) | 762 | about 20.5 |
| `scene/Components.hpp` | 638 | not measured |

At `-j 4` a `Store.hpp` edit is about 5 minutes of wall time; a `Name.hpp`
edit about 7.

## Options

1. **A forward header for the store.** 40 public headers include
   `ecs/Store.hpp`; many only pass `Store &`. A `StoreFwd.hpp` declaring
   `Store` and `Entity` lets those headers drop the full include, so a
   `Store.hpp` edit rebuilds only the `.cpp` files that use it. Expected gain
   is the largest of the three for the most edited header in this milestone.
   Cost: mechanical, one module at a time, each verified with
   `just header-batching-check` and `just test-pre-commit`.
2. **Move the value types out of `Components.hpp` and `Instance.hpp`.** These
   sit under nearly everything because every component registration includes
   them. Splitting registration (`Components::Register`) from the lookup
   surface would let leaf modules include less. Larger and riskier than 1.
3. **Leave the core headers.** `Name.hpp`, `Bytes.hpp` and the math types are
   small and rarely edited; their fan-out is expected and not worth cutting.

Recommendation: option 1 first, measured with this same method before and
after, then decide on option 2 from the result.
