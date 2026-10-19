#ifndef AVM2_JPEGXR_H
#define AVM2_JPEGXR_H

#include <stddef.h>
#include <stdint.h>

// JPEG-XR decode (jxrlib) for Loader-loaded .jxr content and the JPEG-XR-backed
// Stage3D ATF formats. Every JPEG-XR function is a stub returning 0 unless the
// build defines SWF_HAVE_LIBJXR (verify_output.py graphics mode / CMake
// WITH_LIBJXR); no-graphics and WASM builds never decode JPEG-XR.
//
// All jxrlib headers stay inside avm2_jpegxr.c: they define Bool / U8 / max
// and friends, which would collide with runtime code.

// Ruffle render/src/utils.rs decode_jpegxr: decode through jxrlib's TIFF
// conversion and expand to straight (NOT premultiplied) RGBA8, which is what
// the `image` crate's to_rgba8 yields. Returns 1 and a malloc()ed buffer of
// w*h*4 bytes (caller free()s it), or 0.
int avm2_jxr_decode_rgba(const uint8_t* data, size_t len,
                         uint32_t* out_w, uint32_t* out_h, uint8_t** out_rgba);

// Ruffle core/.../display3D/textures/atf_jpegxr.rs do_compressed_upload for
// the JPEG-XR ATF formats: 0/1 (RGB888/RGBA8888, one JPEG-XR image) and
// 4 (CompressedAlpha: LZMA DXT tables + JPEG-XR endpoint planes, rebuilt into
// DXT5 and decoded here to RGBA8 because the Stage3D texel store is RGBA).
// Only face 0, mip 0 is decoded, as in Ruffle. `atf` starts at the "ATF"
// signature; `w`/`h` are the texture's size (already checked against the
// header by the caller). Returns 1 and a malloc()ed w*h*4 RGBA8 buffer, or 0
// for any other format or a decode failure (the texture is left untouched).
int avm2_atf_jxr_decode(const uint8_t* atf, size_t len, uint32_t w, uint32_t h,
                        uint8_t** out_rgba);

// One BC3 (DXT5) block (16 bytes) -> 4x4 straight RGBA8 at `dst` (row stride
// `dst_stride` bytes), Mesa util_format s3tc rounding. Always built: it needs
// no jxrlib, and ATF format 5 (RawCompressedAlpha) is raw DXT5.
void avm2_bc3_decode_block(const uint8_t* b, uint8_t* dst, uint32_t dst_stride);

#endif
