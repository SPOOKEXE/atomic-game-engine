# FFT unsigned control profile

`pc.fft` follows the default extension path in Pixel Composer source commit
`b69eca232217360cf1502ef0223523d818606652`. Its Preprocess Function is an EScroll
control. The source writes the resolved value with `buffer_u32`; the extension
reads that unsigned word before selecting None, Hann or Blackman.

The native CPU profile uses the inspected official GameMaker HTML5 runtime at
commit `60e51be51ce7f3d52025ef18106cf172b8e22a00`. Its `buffer_write` forwards the
value to `yyb_write`, which writes U32 through `DataView.setUint32`. Running those
actual functions gives truncation toward zero followed by unsigned modulo
2^32. Source scalar choices clamp to 0..2 first. Array-selected controls retain
the existing source getter clamp bypass, so negative and overflowing finite
values also reach the unsigned conversion. Unknown converted window words use
the extension's default, unwindowed branch.

The conversion applies only inside FFT. It does not change generic choice
rounding. Nonfinite native graph values remain invalid even though the inspected
HTML5 buffer write converts NaN and infinities to zero. Samples, channel count,
output shape, FFT workspace and replacement output remain bounded by the existing
shared evaluation budget. Failure preserves the caller's previous output.

The source contract manifest records exact source hashes and executed unsigned
write observations. Headless tests encode and decode a real stereo PCM WAV,
then compile WAV File Read to Audio Window to FFT with a linked animated numeric
control. They compare spectra with a separate direct DFT across fractional,
negative and repeated seeks, exercise array-selected overflow and negative
controls, and check missing clips, tight byte caps and nonfinite controls.

Licensed desktop runner fractional buffer conversion remains unverified. This
profile does not claim the alternate GML FFT path selected by `__use_ext=false`.

Joined release56 core tests pass, including the real WAV graph and animated
choice-array spectra. Licensed reference comparison remains separate.
