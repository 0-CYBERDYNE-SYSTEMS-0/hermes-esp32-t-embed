# Hermes portable core provenance

The files under `src/` and `include/hg/` are copied from the portable core at
`firmware/core/` in [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk),
commit `edecb81c22f1c9308338735b0613be3d341e94ac`.

The imported source closure is `app.cpp`, `canvas.cpp`, `crypto.cpp`,
`font5x7.cpp`, `json.cpp`, `mascot_data.cpp`, `protocol.cpp`, `touch.cpp`,
`ui.cpp`, and `vad.cpp`, with the required headers from `include/hg/`. The
upstream MIT license is preserved in `LICENSE`; `mascot_data.cpp` also contains
Hermes Agent mascot artwork, whose attribution is preserved in `NOTICE`.
