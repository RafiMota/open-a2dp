# Contributing

Thanks for helping! A few rules keep the project legally clean and easy to
review.

## Clean room

Read [docs/CLEAN_ROOM.md](docs/CLEAN_ROOM.md). In short: never disassemble or
unpack proprietary A2DP drivers or their installers, and cite the public
source (spec section, Microsoft doc, open-source file) for protocol or codec
behaviour you implement. PRs without a source for new protocol behaviour will
be asked for one.

## Workflow

1. Open an issue for anything bigger than a bug fix, referencing the SPEC ID
   (e.g. "F4.3").
2. Keep `core/` portable: no Windows headers, no allocation, C99.
3. Every change to `core/` comes with unit tests; run `sh scripts/zig-build.sh`
   (or `ctest`) before pushing.
4. Kernel code: run Code Analysis and Static Driver Verifier; describe how you
   tested (headset model, Windows build, radio).
5. Update the status column in `docs/SPEC.md` when a requirement changes state.

## Style

Match the surrounding code: 4-space indent, `oa2dp_` prefix in C, Windows
naming (`Oa2dp...`) in drivers, `snake_case` in the service. Format with
`clang-format` defaults close to the existing files.

## Testing with headsets

Report results in the "Headset compatibility" discussion: model, codecs
offered (`oa2dpsvc --console` logs them), codec chosen, any dropouts, and the
`oa2dpctl status` output.
