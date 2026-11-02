# HD Mode 7

The shared renderer reconstructs continuous Mode 7 geometry from an owned
frame of raster state. It resamples the original artwork; sprites and HUD
retain native detail. CPU and GPU consume the same reconstruction.

See [HD_MODE7_REBUILD.md](HD_MODE7_REBUILD.md) for frozen emulator references,
mathematics, acceptance evidence and remaining game qualification.

## Standard PPU hosts

Create a `SnesHdCapture`, set its sampling and bind it to the live PPU with the
native frame height. The ordinary scanout stays available. After scanout,
`SnesHdCaptureFrame` returns the completed immutable frame, or NULL when the
frame has no supported Mode 7 content. Reset discards old published frames.
Unbind or destroy the capture before destroying its PPU.

```c
SnesHdCapture *capture = SnesHdCaptureCreate();
SnesHdCaptureSetSampling(capture, 1); /* 1: off; 2..4 samples per axis */
SnesHdCaptureBind(capture, ppu, 224);
/* Execute ordinary PPU scanout. */
const SnesHdFrame *frame = SnesHdCaptureFrame(capture);
/* Draw with SnesHdGpuRender, or SnesHdFrameRenderRegion. */
SnesHdCaptureDestroy(capture);
```

The desktop host does this automatically. Graphics settings retain enabled,
resolution multiplier (1..64) and supersampling (1..4). The latter is independent
of output resolution; 1 disables it. Its squared cost is elective, default off.

## Custom renderers

Publish native composition, picture-memory versions and raw camera rows to a
`SnesHdFrame` builder. Set sampling before sealing. `kSnesHdReconstructed` rows
supply signed coefficients, centers and offsets, original raster Y and camera
identity. Sealing prepares the geometry; neither consumer reads live guest RAM.
F-Zero uses this path, retaining its course map, HUD and temporal camera policy.

The portable ROI interface bounds allocation and accepts padded output rows.
The GPU supports bounded targets and averages supersamples before returning the
presentation image. Nearest window-sized presentation preserves the full-target
sampling result; filters that inspect adjacent pixels use a full target.
Supersampling scratch storage is included in target admission.

Mosaic retains authored native blocks. Unsupported interlace, pseudo-hires or
host-overlay combinations retain native composition. At 1x with supersampling
off, output is the exact native image. The old scanline surface API has been
removed: it could not reconstruct perspective from a complete frame.

## Checks

`tests/ppu/run.ps1` covers the portable PPU contract. The GPU parity test adds
full/visible targets, supersampling, texel boundaries, large maps, negative
margins, flips, composition and retained GL state. Detached drawing is tested
with `tests/ppu/run_detached_line.py` and cannot publish HD frames.
