# scripthost - module invariants

L11, `shared`. **Which VM to open.** One switch on `Language`, above the two
adapters because it is the only thing in the engine allowed to name both.

## Two hundred lines of source and every scripting suite

That ratio is the module, not an accident of where things landed.

`MakeRuntime` has to see both implementations, which is precisely why `script`
was 33,000 lines until v0.19: the factory lived in the same library as the two
VMs, so the VM boundary could only ever be a naming convention. Moving one
function up a layer is what let `script` drop its vendor edges entirely and what
deleted `mono_check_script_vm_naming`.

The suites are here because **this is the only place both VMs exist in one
process.** "Two languages, two VMs, one binding surface" is a claim about both,
and a case that could reach one of them could not check it - which is what most
of these suites do: they run the same script in each language and compare the
answers. `engine.scripthost.scriptcall` is the clearest example and
`engine.scripthost.runtime` the largest.

**A case that only needs one VM does not belong here.** It belongs in that
adapter's own suite, where the *other* adapter is not linked and an accidental
edge between the two is a link error. `engine.scriptluau.runtime` and
`engine.scriptjs.runtime` are two cases each and are meant to stay that way.

## `game` and `examples` are above this, and that is the one edge the split cost

Both call `MakeRuntime` to start a world's scripts, so both moved from L10 to L12
at v0.19. Nothing but the program band links either, so the move cost two `layer`
fields in `expected_graph.json` and no edge at all. `docs/CODE_ARCH.md` §4.1
carries it.

**Every program links this rather than either adapter**, which is the shape to
keep: a program that reached for `scriptluau` directly would be a program that
had chosen a language for the game it loads.

## Nothing but the switch goes here

The temptation is to put "scripting things that need both" in this module. There
is nothing of that kind: what both languages share is a *description*, and a
description has no VM in it and belongs in `script` at L9. If something here
grows past the factory, the question to ask is which of the three layers below it
should have owned it.
