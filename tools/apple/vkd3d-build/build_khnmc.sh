#!/bin/sh
# Private build of vkd3d 2.1 without autotools (KHNMC): static libraries from the four source directories.
#   VKHEADERS=<dir with vulkan/vulkan.h> SPIRVHEADERS=<dir with spirv/unified1/spirv.h> [CC=cc] [JOBS=4] [OUT=build] [EXTRA_CFLAGS=...] sh build_khnmc.sh
# Needs the generated files made on a host with flex/bison/perl: libs/vkd3d-shader/{hlsl,preproc}.{tab.c,tab.h,yy.c},
# include/private/spirv_grammar.h, include/private/vkd3d_version.h, and the hand-written config.h next to this script.
set -eu
CC=${CC:-cc}; OUT=${OUT:-build}; JOBS=${JOBS:-4}
: "${VKHEADERS:?set VKHEADERS}" "${SPIRVHEADERS:?set SPIRVHEADERS}"
BASE="-O2 -std=gnu11 -w -fPIC -fno-strict-aliasing -D_GNU_SOURCE -DHAVE_CONFIG_H -I. -Iinclude -Iinclude/private -I$VKHEADERS -I$SPIRVHEADERS ${EXTRA_CFLAGS:-}"
mkdir -p "$OUT"
build() {  # name, extra flags, sources...
    name=$1; flags=$2; shift 2; objs=""
    for f in "$@"; do o="$OUT/$name-$(basename "$f" .c).o"; objs="$objs $o"; echo "$CC $BASE $flags -c $f -o $o"; done > "$OUT/$name.cmds"
    # one command per line, run JOBS at a time (BSD xargs cannot substitute a line this long with -I, so no xargs here)
    n=0
    while IFS= read -r cmd; do
        sh -c "$cmd" || touch "$OUT/$name.failed" &
        n=$((n+1)); if [ $((n % JOBS)) -eq 0 ]; then wait; fi
    done < "$OUT/$name.cmds"
    wait
    if [ -e "$OUT/$name.failed" ]; then echo "error: a compile of $name failed" >&2; rm -f "$OUT/$name.failed"; exit 1; fi
    rm -f "$OUT/lib$name.a"; ar rcs "$OUT/lib$name.a" $objs
    echo "built $OUT/lib$name.a ($(echo $objs | wc -w | tr -d ' ') objects)"
}
build vkd3d-common "" libs/vkd3d-common/blob.c libs/vkd3d-common/debug.c libs/vkd3d-common/error.c libs/vkd3d-common/memory.c libs/vkd3d-common/utf8.c
build vkd3d-shader "-DLIBVKD3D_SHADER_SOURCE -DVKD3D_DEBUG_ENV=VKD3D_SHADER_DEBUG -Ilibs/vkd3d-shader" \
    libs/vkd3d-shader/checksum.c libs/vkd3d-shader/d3d_asm.c libs/vkd3d-shader/d3dbc.c libs/vkd3d-shader/dxbc.c libs/vkd3d-shader/dxil.c \
    libs/vkd3d-shader/fx.c libs/vkd3d-shader/glsl.c libs/vkd3d-shader/hlsl.c libs/vkd3d-shader/hlsl_codegen.c libs/vkd3d-shader/hlsl_constant_ops.c \
    libs/vkd3d-shader/ir.c libs/vkd3d-shader/msl.c libs/vkd3d-shader/spirv.c libs/vkd3d-shader/tpf.c libs/vkd3d-shader/vkd3d_shader_main.c \
    libs/vkd3d-shader/hlsl.tab.c libs/vkd3d-shader/hlsl.yy.c libs/vkd3d-shader/preproc.tab.c libs/vkd3d-shader/preproc.yy.c
build vkd3d "-DLIBVKD3D_SOURCE" libs/vkd3d/cache.c libs/vkd3d/command.c libs/vkd3d/device.c libs/vkd3d/resource.c libs/vkd3d/state.c libs/vkd3d/utils.c libs/vkd3d/vkd3d_main.c
build vkd3d-utils "-DLIBVKD3D_UTILS_SOURCE" libs/vkd3d-utils/reflection.c libs/vkd3d-utils/vkd3d_utils_main.c
