#pragma once

#include <common.h>

// Test whether a point (stage_x, stage_y) in stage twips hits a triangulated shape.
//
// shape_data:    vertex buffer (4 u32s per vertex: float x, float y, u32 style, u32 id)
// shape_offset:  starting vertex index in shape_data
// shape_size:    number of vertices (must be multiple of 3)
// transform:     pointer to 16 floats (4x4 column-major matrix) — the shape's world transform
// stage_x/y:     mouse position in stage twips
//
// The function inverse-transforms the point into the shape's local space,
// then tests it against every triangle in the shape.
//
// Returns 1 if the point is inside any triangle, 0 otherwise.
int hit_test_shape(const char* shape_data, size_t shape_offset, size_t shape_size,
                   const float* transform, float stage_x, float stage_y);

// Multiply two 4x4 column-major matrices: out = A * B
// Used to compose parent + child transforms for hit testing.
void hit_test_mat4_multiply(float* out, const float* A, const float* B);
