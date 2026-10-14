// Image decode for MovieClipLoader.loadClip("foo.{gif,jpg,png}").
// Uses stb_image (implementation in src/libswf/stb_image_impl.c).
//
// Decoded pixels are converted to premultiplied ARGB32 and attached to the
// target MovieClip via attached_bitmap_pixels — picked up by
// actionIterateAttachedBitmaps and drawn via render_webgpu_draw_bitmap_quad.

#include <action.h>
#include <stdlib.h>
#include <string.h>
#include "stb_image.h"

int decodeAndAttachImageToMC(MovieClip* mc, const unsigned char* data, int data_size)
{
	if (mc == NULL || data == NULL || data_size <= 0) return 0;

	int w = 0, h = 0, comp = 0;
	unsigned char* rgba = stbi_load_from_memory(data, data_size, &w, &h, &comp, 4);
	if (rgba == NULL || w <= 0 || h <= 0) {
		if (rgba) stbi_image_free(rgba);
		return 0;
	}

	if (mc->attached_bitmap_pixels != NULL) {
		free(mc->attached_bitmap_pixels);
		mc->attached_bitmap_pixels = NULL;
	}

	size_t npixels = (size_t)w * (size_t)h;
	uint32_t* argb = (uint32_t*)malloc(npixels * sizeof(uint32_t));
	if (argb == NULL) {
		stbi_image_free(rgba);
		return 0;
	}

	for (size_t i = 0; i < npixels; i++) {
		unsigned char r = rgba[i * 4 + 0];
		unsigned char g = rgba[i * 4 + 1];
		unsigned char b = rgba[i * 4 + 2];
		unsigned char a = rgba[i * 4 + 3];
		uint32_t pr, pg, pb;
		if (a == 255) {
			pr = r; pg = g; pb = b;
		} else if (a == 0) {
			argb[i] = 0;
			continue;
		} else {
			// Floor division (NOT round) to match Ruffle's premultiplied
			// alpha math. Off-by-one differences are visible against
			// solid white backdrops — key test:
			// avm1/movieclip_methods_with_loaded_image, where source
			// (174, 49, 63 alpha) blends to (255, 234, 204) with floor
			// vs (255, 235, 204) with round-half-up.
			pr = (uint32_t)r * a / 255;
			pg = (uint32_t)g * a / 255;
			pb = (uint32_t)b * a / 255;
		}
		argb[i] = ((uint32_t)a << 24) | (pr << 16) | (pg << 8) | pb;
	}
	stbi_image_free(rgba);

	mc->attached_bitmap_pixels = argb;
	mc->attached_bitmap_width = (uint16_t)w;
	mc->attached_bitmap_height = (uint16_t)h;
	mc->loaded_image_width = (uint16_t)w;
	mc->loaded_image_height = (uint16_t)h;
	return 1;
}
