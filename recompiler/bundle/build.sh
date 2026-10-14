#!/bin/bash
# Build this recompiled SWF bundle to a browser WASM (WebGPU graphics) page.
#
# Prerequisites: Emscripten (emsdk) with `emcc` on PATH, or EMSDK pointing at
# an emsdk checkout (this script sources $EMSDK/emsdk_env.sh).
#
# Usage:  ./build.sh            -> out/<name>.js, out/<name>.wasm, out/index.html
#         python3 -m http.server -d out 8080   then open http://localhost:8080/
#
# Mirrors SWFRecomp/scripts/build_test.sh (`wasm --graphics`) for AVM1 SWFs and
# SWFRecomp/scripts/build_wasm_avm2.sh for AS3 SWFs (RecompiledABC/ present).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="$(cat "${HERE}/NAME" 2>/dev/null || echo swf)"
RT="${HERE}/runtime"
OUT="${HERE}/out"
SRC="${OUT}/src"
OBJ="${OUT}/obj"
ABC_OPT="${ABC_OPT:--O1}"     # giant RecompiledABC TUs (AVM2 only)
RT_OPT="${RT_OPT:--O2}"
MOCK_DATE_TIME="${MOCK_DATE_TIME:-981152406000}"
SWF_URL="${SWF_URL:-file:///test.swf}"

if ! command -v emcc >/dev/null 2>&1; then
    if [ -n "${EMSDK:-}" ] && [ -f "${EMSDK}/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        source "${EMSDK}/emsdk_env.sh" >/dev/null 2>&1 || true
    fi
fi
command -v emcc >/dev/null 2>&1 || {
    echo "ERROR: emcc not found. Install emsdk (https://emscripten.org) and run" >&2
    echo "       source /path/to/emsdk/emsdk_env.sh   (or export EMSDK=/path/to/emsdk)" >&2
    exit 1
}

IS_AVM2=false
[ -d "${HERE}/RecompiledABC" ] && IS_AVM2=true

if [ "${FRESH:-0}" = "1" ]; then rm -rf "${OUT}"; fi
mkdir -p "${SRC}" "${OBJ}"

echo "=== Staging sources (${NAME}, avm2=${IS_AVM2}) ==="
cp -p "${RT}/main.c" "${SRC}/"
RUNTIME_C=(
    actionmodern/action.c actionmodern/math.c actionmodern/date.c
    actionmodern/registered_class.c actionmodern/timer.c actionmodern/variables.c
    actionmodern/avm1_amf.c actionmodern/object.c actionmodern/action_queue.c
    actionmodern/sprite_frame_scripts.c actionmodern/image_decode.c
    actionmodern/video_codec.c
    utils.c amf_packet.c
    libswf/swf.c libswf/tag.c libswf/tag_stubs.c libswf/shape_hit_test.c
    libswf/ng_shared.c libswf/hit_test.c libswf/graphics_stubs.c
    libswf/stb_image_impl.c
    audio/audio.c audio/audio_output_web.c
    rendering/render_webgpu.c
    memory/heap.c
)
for f in "${RUNTIME_C[@]}"; do cp -p "${RT}/src/${f}" "${SRC}/"; done
cp -p "${RT}/src/actionmodern/unicode_case_tables.h" "${SRC}/"
cp -p "${RT}/lib/c-hashmap/map.c" "${RT}/lib/c-hashmap/map.h" "${SRC}/"
cp -p "${RT}/lib/o1heap/o1heap.c" "${RT}/lib/o1heap/o1heap.h" "${SRC}/"
cp -p "${RT}/lib/stb/stb_image.h" "${SRC}/"
cp -p "${RT}/third_party/libtess2"/*.c "${RT}/third_party/libtess2"/*.h "${SRC}/"
mkdir -p "${SRC}/memory"; cp -p "${RT}/include/memory/heap.h" "${SRC}/memory/"

EXTRA_DEFINES=()
EXPORTED_FUNCS='["_main","_runSWF","_audio_fill_buffer","_swf_ei_call_internal","_ng_ime_compose_set","_ng_ime_commit_set"]'
if [ "${IS_AVM2}" = true ]; then
    cp -p "${RT}/src/avm2"/*.c "${SRC}/"
    cp -p "${RT}/third_party/quickjs-libregexp"/*.c "${RT}/third_party/quickjs-libregexp"/*.h "${SRC}/"
    cp -p "${RT}/third_party/lzma"/*.c "${RT}/third_party/lzma"/*.h "${SRC}/"
    cp -p "${HERE}/RecompiledABC"/*.c "${HERE}/RecompiledABC"/*.h "${SRC}/" 2>/dev/null || true
    SWF_FILE_SIZE=0; SWF_ONDISK_SIZE=0
    if [ -f "${HERE}/${NAME}.swf" ]; then
        SWF_ONDISK_SIZE=$(stat -c%s "${HERE}/${NAME}.swf")
        SWF_FILE_SIZE=$(python3 -c "import struct,sys;h=open(sys.argv[1],'rb').read(8);print(struct.unpack('<I',h[4:8])[0])" "${HERE}/${NAME}.swf")
    fi
    EXTRA_DEFINES=(-DSWF_AVM2 "-DMOCK_DATE_TIME=${MOCK_DATE_TIME}LL"
        "-DSWF_FILE_SIZE=${SWF_FILE_SIZE}" "-DSWF_ONDISK_SIZE=${SWF_ONDISK_SIZE}"
        "-DSWF_URL=\"${SWF_URL}\"")
    EXPORTED_FUNCS='["_main","_runSWF","_audio_fill_buffer","_avm2_ei_dispatch","_malloc","_free"]'
else
    # Display-list JSON bridge + framebuffer capture exports (debug tooling),
    # same as build_test.sh's graphics wasm builds.
    cp -p "${RT}/display_bridge.c" "${SRC}/"
    EXTRA_DEFINES=(-DHAS_DISPLAY_BRIDGE)
    EXPORTED_FUNCS='["_main","_runSWF","_audio_fill_buffer","_getDisplayListJSON","_getSpriteChildrenJSON","_setObjectTransform","_dbgCapturePNG","_dbgCaptureReady","_dbgCaptureData","_dbgCaptureWidth","_dbgCaptureHeight","_dbgSetFrameCapMs","_swf_ei_call_internal","_ng_ime_compose_set","_ng_ime_commit_set"]'
fi
cp -p "${HERE}/RecompiledScripts"/*.c "${HERE}/RecompiledScripts"/*.h "${SRC}/" 2>/dev/null || true
cp -p "${HERE}/RecompiledTags"/*.c "${HERE}/RecompiledTags"/*.h "${SRC}/" 2>/dev/null || true

INCLUDES=(-I"${SRC}" -I"${RT}/include" -I"${RT}/include/actionmodern" -I"${RT}/include/libswf"
    -I"${RT}/include/memory" -I"${RT}/include/rendering" -I"${RT}/include/audio"
    -I"${RT}/include/avm2" -I"${RT}/lib/c-hashmap" -I"${RT}/lib/o1heap")
WARN=(-Wno-error=implicit-function-declaration -Wno-implicit-function-declaration
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-incompatible-pointer-types)
SIMD=(); [ "${IS_AVM2}" = true ] && SIMD=(-msimd128)

opt_for() {
    case "$1" in
        # one per DoABC tag; *_methods_<k>.c are the tu_split body chunks
        abc*_methods.c|abc*_methods_*.c|abc*_tables.c|abc_timeline.c) echo "${ABC_OPT}" ;;
        *) echo "${RT_OPT}" ;;
    esac
}

echo "=== Compiling (incremental: a .c is rebuilt only if newer than its .o) ==="
# A changed HEADER also invalidates every .o: the runtime's context structs
# (e.g. render_webgpu.h's WebGPURenderContext) are shared by many TUs, and a
# TU compiled against the old layout silently corrupts the new one. Rebuild
# everything older than the newest header under runtime/include.
NEWEST_HDR="$(find "${RT}/include" -type f -name '*.h' -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-)"
OBJS=(); N=0
for src in "${SRC}"/*.c; do
    base="$(basename "${src}" .c)"
    obj="${OBJ}/${base}.o"
    OBJS+=("${obj}")
    if [ -f "${obj}" ] && [ "${obj}" -nt "${src}" ] \
       && { [ -z "${NEWEST_HDR}" ] || [ "${obj}" -nt "${NEWEST_HDR}" ]; }; then continue; fi
    o="$(opt_for "$(basename "${src}")")"
    printf '  %-28s %s\n' "$(basename "${src}")" "${o}"
    emcc -DUSE_WEBGPU "${EXTRA_DEFINES[@]}" "${SIMD[@]}" --use-port=emdawnwebgpu \
        "${INCLUDES[@]}" "${WARN[@]}" "${o}" -c "${src}" -o "${obj}"
    N=$((N+1))
done
echo "  (${N} TUs compiled)"

echo "=== Linking out/${NAME}.js + out/${NAME}.wasm ==="
LINK_EXTRA=(); [ "${IS_AVM2}" = true ] && LINK_EXTRA=(-sUSE_ZLIB=1 -s MAXIMUM_MEMORY=4GB)
emcc "${OBJS[@]}" --use-port=emdawnwebgpu "${SIMD[@]}" \
    -o "${OUT}/${NAME}.js" \
    -s WASM=1 \
    -s EXPORTED_FUNCTIONS="${EXPORTED_FUNCS}" \
    -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap","HEAPF32","HEAPU8"]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=256MB \
    -s STACK_SIZE=8MB \
    -sASYNCIFY \
    -sASYNCIFY_STACK_SIZE=65536 \
    "${LINK_EXTRA[@]}" \
    "${RT_OPT}"

# Player page: template with the module name and the SWF's stage size filled in.
W=$(sed -n 's/^#define FRAME_WIDTH \([0-9]*\).*/\1/p' "${HERE}/RecompiledTags/constants.h" | head -1)
H=$(sed -n 's/^#define FRAME_HEIGHT \([0-9]*\).*/\1/p' "${HERE}/RecompiledTags/constants.h" | head -1)
sed -e "s/{{TEST_NAME}}/${NAME}/g" \
    -e "s/<canvas id=\"canvas\" width=\"550\" height=\"400\">/<canvas id=\"canvas\" width=\"${W:-550}\" height=\"${H:-400}\">/" \
    "${RT}/index_template_graphics.html" > "${OUT}/index.html"
cp -p "${RT}/swf_bridge.js" "${OUT}/" 2>/dev/null || true

echo
echo "Built:"
ls -lh "${OUT}/${NAME}.js" "${OUT}/${NAME}.wasm" "${OUT}/index.html"
echo
echo "Serve it (WebGPU needs a real browser, Chrome recommended):"
echo "  python3 -m http.server -d \"${OUT}\" 8080    # then open http://localhost:8080/"
