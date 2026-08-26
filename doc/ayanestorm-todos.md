# AyaneStorm TODOs

## Make the snapshot capture frame pixel-accurate

The on-screen capture frame can differ from the final snapshot crop by a few
pixels. `LLPipeline::renderSnapshotFrame()` uses ideal floating-point aspect
bounds and expands the apparent edge by the decorative border width, while
`LLViewerWindow::rawSnapshot()` independently truncates crop dimensions and
floors centering offsets.

- Extract the capture rectangle calculation into a small AyaneStorm-owned
  helper returning exact integer raw-window bounds.
- Use the same bounds from narrow ownership-tagged hooks in `llviewerwindow.cpp`
  and `pipeline.cpp`.
- Keep decorative border thickness separate from the indicated capture bounds.
- Verify odd window dimensions, differing aspect ratios, UI and HUD capture,
  display scaling, constrained aspect ratio, and tiled high-resolution capture.

