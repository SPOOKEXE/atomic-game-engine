# Pass 2 decisions

- Procedural baked scene: reject moving placement names into temporary geometry.
  Three heap comparisons against `6bd15f10` showed negligible savings with
  unchanged output and cache identities. Investigate the larger geometry buffers.
  Latency was not compared.
- Delivery cache: reject unbuffered whole-file reads. Three CPU comparisons
  preserved outputs and reduced allocation churn, but the complex delivery path
  was repeatedly slower. Three pass 1 control pairs matched cold and warm
  cache identities and work. Measurements used a shared host.
- Procedural baked scene: accept borrowed mesh views for shared packing. They
  reduce temporary allocations and peak tracked payload while retaining output,
  admission limits, and rollback behavior. Delivery and live timings were mixed
  on a shared host, with some live medians slower. No latency gain is claimed.
  Three pass 1 control pairs matched all fixtures and canonical hashes. The
  procedural scene suite also passed under ASan and UBSan.
- Cadence animation: accept call-local track-vector reuse. Native heap runs
  reduced allocation operations without retaining the buffer across calls.
  Native and town work matched; timings were mixed on a shared host. All
  populated promotion fixtures matched one pair; complex authored fixtures
  matched three pairs. The timing matrix remains open.
- Studio viewport queries: three CPU control pairs matched all 34 work rows
  and internal oracles. Shared-host timings are not accepted as performance proof.
- Character, portal, mixed-shape and weather ground queries: three CPU control
  pairs matched all work rows. Shared-host timings remain unqualified.
- Engine graph: three CPU control pairs passed all 12 fixture cohorts. Timing
  varied with run order on the shared host and remains unqualified.
- CPU image evaluation: three control pairs passed all five full-pixel cohorts.
  Busy and complex medians were lower; shared-host timings remain unqualified.
