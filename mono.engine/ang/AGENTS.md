# ang - module invariants

L3, `shared` tier. Links `core`, `parallel` and `graphcore`. No GPU, no UI, no
script VM. Arbitrary node graphs: the model, the compiler and the runner.

## graphcore orders, ang runs

`Compile` lowers every graph to `graphcore::CompileGraph` for order, waves,
cycles and limits. Do not grow a second topological sort here. ang adds what
graphcore leaves to domains: types and conversions, generics, subgraph
flattening, the domain rule and the per-node hash. Modules that only need
ordering link `graphcore`, not `ang`.

## Registries are owned, never global

A host owns its `Registry`. There is no process registry, so a script context
cannot change the node types the engine registered. Tests build their own
fixture registry and never import a consumer's node set.

## Names cross boundaries

Node ids, port names, type ids and definition ids are durable text. Dense
indices in a `Plan` are valid for that plan only. `Value::Hash` and every plan
hash use FNV-1a over explicit field bytes, never addresses, padding, interned
ids or time. A `Blob` hashes by the host's `Identity`; zero means unknown and
makes the node uncacheable.

## The domain rule

`Tick` graphs allow `Sync` and `Parallel` only. `Compile` refuses an `Async`
node in a `Tick` graph and there is no override flag. `Parallel` work joins
inside `Run`; workers only evaluate, and commit, budget and reporting happen on
the calling thread in wave order, so a parallel wave gives the serial bytes.
`Async` work runs on `AsyncPool`, never on `Jobs`, and `Run` never waits for it.

## Hooks and reporting

`PreNode`/`PostNode` fire for every visit, `PreCompute`/`PostCompute` only when
`Evaluate` runs, all on the thread that ran the node. Errors land in
`RunReport` and `NodeStatus`, never only in a log. `StopOnFailure` ends a run
at the first failure, reporting later visits Skipped, for hosts that abandon a
tick on one failed adapter call. Sync nodes open
`ENGINE_PROFILE_DYNAMIC` scopes; Parallel and Async nodes report with
`FrameGraph::ReportNamed` after the join or on the run that collects them.
Measured hook and dispatch costs sit beside `Hooks` and
`RunnerOptions::ParallelMinimum`; re-measure with `just ang-bench` before
changing either.

## Edits and undo

`Graph` changes only through `Graph::Apply`, which returns the inverse. ang
owns no undo stack. A batch applies all or nothing.
