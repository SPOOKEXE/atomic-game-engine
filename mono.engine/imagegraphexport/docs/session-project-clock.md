# Range export project clock

An admitted range owns its project cursor. `SourceCacheProject.ProjectFrame`
is copied from that cursor before replay preparation on every attempt. The
initial preview clock can be signed or fractional and stays unchanged in the
caller. The resolved project endpoint, loading and appending observations are
fixed at admission. A changed or removed observation cancels pending work
before another provider call, removes staging and preserves published files.
The owned fixed-size observation is included in the session reservation.

The headless fixture captures two actual range frames through pending host
completion, checks the project endpoint predicate only at frame1, and decodes
both published PNGs against literal pixels. Refusal and cancellation retain
previous output and avoid repeated successful effects. These fixtures test
cursor versus initial preview clock. They do not claim a verified graph node
that transforms the scoped input clock; that scheduler evidence remains open.
