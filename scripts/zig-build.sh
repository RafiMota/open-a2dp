#!/usr/bin/env sh
# Toolchain-light build of the user-mode parts with `zig cc` (no Visual
# Studio needed). Builds and runs the portable core tests and the encoder
# smoke test. Drivers still need Visual Studio + WDK (see BUILDING.md).
#
#   ZIG=/path/to/zig sh scripts/zig-build.sh
#
# Codec libraries are picked up from third_party/ when present
# (scripts/fetch-deps.sh).
set -eu

ZIG=${ZIG:-zig}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/zig"
TP="$ROOT/third_party"
TARGET=${TARGET:-x86_64-windows-gnu}
CFLAGS="-std=c99 -O2 -Wall -Wextra"
# Links below glob obj/*.o, so start clean: stale service objects from an
# earlier run would otherwise end up in test_encoders.exe.
rm -rf "$OUT/obj" "$OUT/obj-tools"
mkdir -p "$OUT/obj"

echo "== core unit tests (host, with UBSan)"
"$ZIG" cc -std=c99 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -fsanitize=undefined \
    -fno-sanitize-trap=undefined -I"$ROOT/core/include" \
    "$ROOT"/core/src/*.c "$ROOT"/core/tests/*.c -o "$OUT/oa2dp_tests.exe"
"$OUT/oa2dp_tests.exe"

echo "== encoders ($TARGET)"
DEFS="-DOA2DP_WITH_MFAAC"
INCS="-I$ROOT/core/include -I$ROOT/codecs/include"
SRCS=""
LIBS="-lmfplat -lmfuuid -lole32"

if [ -f "$TP/sbc/sbc/sbc.c" ]; then
    # libsbc expects an autotools config.h; an empty one is enough.
    mkdir -p "$OUT/sbc-config" && : > "$OUT/sbc-config/config.h"
    DEFS="$DEFS -DOA2DP_WITH_LIBSBC"
    INCS="$INCS -I$TP/sbc"
    for f in sbc.c sbc_primitives.c sbc_primitives_mmx.c sbc_primitives_sse.c; do
        "$ZIG" cc -target "$TARGET" -O2 -w -include stdint.h -I"$OUT/sbc-config" \
            -I"$TP/sbc/sbc" -c "$TP/sbc/sbc/$f" -o "$OUT/obj/dep_$f.o"
    done
    # libsbc probes SSE/MMX with __builtin_cpu_supports(), whose runtime is
    # missing from zig's windows-gnu compiler-rt. Report "no features" so the
    # portable C primitives are used.
    printf '%s\n' 'struct { unsigned v, t, s, f[1]; } __cpu_model;' \
        'int __cpu_indicator_init(void) { return 0; }' > "$OUT/cpu_shim.c"
    "$ZIG" cc -target "$TARGET" -O2 -c "$OUT/cpu_shim.c" -o "$OUT/obj/dep_cpu_shim.o"
fi
if [ -f "$TP/libopenaptx/openaptx.c" ]; then
    DEFS="$DEFS -DOA2DP_WITH_OPENAPTX"
    INCS="$INCS -I$TP/libopenaptx"
    "$ZIG" cc -target "$TARGET" -O2 -I"$TP/libopenaptx" -c "$TP/libopenaptx/openaptx.c" -o "$OUT/obj/dep_openaptx.o"
fi
if [ -f "$TP/libldac/src/ldacBT.c" ]; then
    DEFS="$DEFS -DOA2DP_WITH_LIBLDAC -DOA2DP_WITH_LDAC_ABR"
    INCS="$INCS -I$TP/libldac/inc -I$TP/libldac/abr/inc"
    for f in src/ldaclib.c src/ldacBT.c abr/src/ldacBT_abr.c; do
        "$ZIG" cc -target "$TARGET" -O2 -w -I"$TP/libldac/inc" -I"$TP/libldac/abr/inc" \
            -c "$TP/libldac/$f" -o "$OUT/obj/dep_$(basename "$f").o"
    done
fi

for f in "$ROOT"/core/src/*.c "$ROOT"/codecs/src/*.c "$ROOT"/codecs/tests/test_encoders.c; do
    # shellcheck disable=SC2086
    "$ZIG" cc -target "$TARGET" $CFLAGS $DEFS $INCS -c "$f" -o "$OUT/obj/$(basename "$f").o"
done
# shellcheck disable=SC2086
"$ZIG" c++ -target "$TARGET" -std=c++17 -O2 -Wall -Wextra $DEFS $INCS \
    -c "$ROOT/codecs/src/enc_aac_mf.cpp" -o "$OUT/obj/enc_aac_mf.o"
# shellcheck disable=SC2086
"$ZIG" c++ -target "$TARGET" "$OUT"/obj/*.o $LIBS -o "$OUT/test_encoders.exe"
"$OUT/test_encoders.exe"

echo "== service and tools ($TARGET)"
SVC_OBJS=""
for f in "$ROOT"/service/src/*.cpp; do
    o="$OUT/obj/svc_$(basename "$f").o"
    # shellcheck disable=SC2086
    "$ZIG" c++ -target "$TARGET" -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE $DEFS $INCS \
        -I"$ROOT/driver/common" -c "$f" -o "$o"
    SVC_OBJS="$SVC_OBJS $o"
done
LIB_OBJS=$(ls "$OUT"/obj/*.o | grep -v -e test_encoders -e '/svc_')
# shellcheck disable=SC2086
"$ZIG" c++ -target "$TARGET" -municode $SVC_OBJS $LIB_OBJS $LIBS -lcfgmgr32 -lbthprops \
    -lshell32 -ladvapi32 -o "$OUT/oa2dpsvc.exe"
# oa2dpctl and oa2dpgui share tools/common (device list, driver switch,
# settings/status registry). Their objects stay out of obj/, which feeds the
# service link above.
TOOL_FLAGS="-target $TARGET -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -I$ROOT/core/include"
mkdir -p "$OUT/obj-tools"
# shellcheck disable=SC2086
"$ZIG" c++ $TOOL_FLAGS -c "$ROOT/tools/common/manage.cpp" -o "$OUT/obj-tools/manage.o"
TOOL_OBJS="$OUT/obj-tools/manage.o $OUT/obj/settings.c.o $OUT/obj/media.c.o"
TOOL_OBJS="$TOOL_OBJS $OUT/obj/a2dp_codecs.c.o"
TOOL_LIBS="-lsetupapi -lbthprops -ladvapi32"
# shellcheck disable=SC2086
"$ZIG" c++ $TOOL_FLAGS -municode "$ROOT/tools/oa2dpctl/oa2dpctl.cpp" $TOOL_OBJS $TOOL_LIBS \
    -o "$OUT/oa2dpctl.exe"
# The .rc embeds the manifest (themed controls, DPI awareness, asInvoker).
# shellcheck disable=SC2086
"$ZIG" c++ $TOOL_FLAGS -municode -Wl,--subsystem,windows \
    "$ROOT/tools/oa2dpgui/oa2dpgui.cpp" "$ROOT/tools/oa2dpgui/oa2dpgui.rc" $TOOL_OBJS \
    $TOOL_LIBS -lcomctl32 -lshell32 -luser32 -lgdi32 -o "$OUT/oa2dpgui.exe"
echo "built: $OUT/oa2dpsvc.exe $OUT/oa2dpctl.exe $OUT/oa2dpgui.exe"
