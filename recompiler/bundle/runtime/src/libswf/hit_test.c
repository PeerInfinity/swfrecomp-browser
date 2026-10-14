#include <hit_test.h>

// ---------------------------------------------------------------------------
// point_in_triangle: barycentric coordinate test
// Returns 1 if (px, py) is inside the triangle (ax,ay)-(bx,by)-(cx,cy).
// ---------------------------------------------------------------------------
static int point_in_triangle(float px, float py,
                             float ax, float ay,
                             float bx, float by,
                             float cx, float cy)
{
	float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
	float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
	float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);

	int has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
	int has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);

	return !(has_neg && has_pos);
}

// ---------------------------------------------------------------------------
// hit_test_shape: test a point against a triangulated shape
//
// The transform is a 4x4 column-major matrix. For 2D affine:
//   col0: [a, c, 0, 0]   col1: [b, d, 0, 0]
//   col2: [0, 0, 1, 0]   col3: [tx, ty, 0, 1]
//
// We compute the 2D inverse and transform the stage point to local space,
// then test against each triangle in the shape.
// ---------------------------------------------------------------------------
int hit_test_shape(const char* shape_data, size_t shape_offset, size_t shape_size,
                   const float* transform, float stage_x, float stage_y)
{
	// Extract 2D affine components from column-major 4x4
	float a  = transform[0];   // col0.x
	float c  = transform[1];   // col0.y
	float b  = transform[4];   // col1.x
	float d  = transform[5];   // col1.y
	float tx = transform[12];  // col3.x
	float ty = transform[13];  // col3.y

	// Compute determinant
	float det = a * d - b * c;
	if (det == 0.0f)
		return 0;  // Degenerate transform, can't invert

	float inv_det = 1.0f / det;

	// Inverse-transform the stage point to local space
	float sx = stage_x - tx;
	float sy = stage_y - ty;
	float local_x = ( d * sx - b * sy) * inv_det;
	float local_y = (-c * sx + a * sy) * inv_det;

	// Shape data layout: 4 u32s per vertex (float x, float y, u32 style, u32 id)
	// stride = 4 * sizeof(u32) = 16 bytes per vertex
	const u32* verts = (const u32*)(shape_data + shape_offset * 4 * sizeof(u32));

	// Iterate triangles (3 vertices each)
	size_t num_tris = shape_size / 3;
	for (size_t t = 0; t < num_tris; t++)
	{
		const u32* v0 = &verts[(t * 3 + 0) * 4];
		const u32* v1 = &verts[(t * 3 + 1) * 4];
		const u32* v2 = &verts[(t * 3 + 2) * 4];

		float ax = *(const float*)&v0[0];
		float ay = *(const float*)&v0[1];
		float bx = *(const float*)&v1[0];
		float by = *(const float*)&v1[1];
		float cx = *(const float*)&v2[0];
		float cy = *(const float*)&v2[1];

		if (point_in_triangle(local_x, local_y, ax, ay, bx, by, cx, cy))
			return 1;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// hit_test_mat4_multiply: C = A * B (column-major 4x4)
// ---------------------------------------------------------------------------
void hit_test_mat4_multiply(float* out, const float* A, const float* B)
{
	for (int col = 0; col < 4; col++)
	{
		for (int row = 0; row < 4; row++)
		{
			out[col * 4 + row] =
				A[0 * 4 + row] * B[col * 4 + 0] +
				A[1 * 4 + row] * B[col * 4 + 1] +
				A[2 * 4 + row] * B[col * 4 + 2] +
				A[3 * 4 + row] * B[col * 4 + 3];
		}
	}
}
