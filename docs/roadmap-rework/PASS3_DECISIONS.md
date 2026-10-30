# Pass 3 decisions

- CPU scheduling: 40 serial and parallel scheduler pairs produced matching
  outputs. Pinned dispatch preserved worker hierarchy with no dropped scopes.
  Timings remain unqualified on the active host; GPU overlap remains unverified.
- Procedural publication: one CPU run of the rebuilt benchmark passed 128
  trials in each of four serial and threaded scenarios; canonical pipeline
  hashes and completed-job work matched. Submit-to-ready, scene publication,
  and cancellation-to-observed-join were measured separately. Canceled work
  differed as expected. Keep scheduling unchanged: this shared-host diagnostic
  is not an optimization comparison. Queue wait and worker execution are not
  separately attributed.
