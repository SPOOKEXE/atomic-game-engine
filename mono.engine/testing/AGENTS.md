# testing - module invariants

L3, shared tier. Depend only on core and parallel. No store, world, renderer,
authority, product or assertion framework belongs in this module.

The adapter owns the actual runtime loop and publishes owned observations only
after its native completion barrier. Payloads must contain copied values and
owned buffers, never borrowed spans, live handles or mutation callbacks. Const
publication cannot repair a payload that hides mutable aliases. This payload
rule is a review convention; the generic C++ type cannot enforce deep ownership.

Each check owns its indexed result. Fork-join dispatch completes before the
owner advances or destroys its runtime. Never start or stop the host job pool.
Check callbacks may not call an assertion framework or mutate shared state.

Every required hook needs positive evidence. Missing, incomplete or lost
observations fail dependent checks. Timeline retention is bounded by both
sample count and owned payload bytes; overflow never silently passes.

Keys describe all runtime and observation choices with stable values. Cache
reuse requires the complete key and matching check/budget declarations. The
bounded cache retains completed reports only, never worlds or image archives.
Cancellation and deadlines are cooperative at owner checkpoints; do not add
detached workers to pretend a blocked native driver can be safely preempted.
