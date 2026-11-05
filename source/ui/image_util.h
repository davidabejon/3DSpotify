/* Image helpers for GPU textures. Pure C with no 3DS headers, so the host
 * tests in tests/native_logic_test.c can exercise them.
 *
 * GPU_RGBA8 textures store each pixel as the bytes A, B, G, R and arrange
 * them in 8x8 tiles whose pixels follow a Morton (Z-order) curve; tiles go
 * left to right, then down. Row 0 of the source image lands in tile row 0,
 * which citro2d shows at the top with a subtexture whose `top` is 1.0. */
#ifndef IMAGE_UTIL_H
#define IMAGE_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* Largest w x h that fits a box x box square with the same aspect ratio,
 * never below 1 px on either side. */
static inline void image_fit(int w, int h, int box, int *out_w, int *out_h)
{
	if (w <= 0 || h <= 0) {
		*out_w = *out_h = 1;
		return;
	}
	if (w >= h) {
		*out_w = box;
		*out_h = (int)((long long)h * box / w);
	} else {
		*out_h = box;
		*out_w = (int)((long long)w * box / h);
	}
	if (*out_w < 1)
		*out_w = 1;
	if (*out_h < 1)
		*out_h = 1;
}

/* Pixel index of (x, y) in a tiled texture `tex_width` pixels wide (a
 * multiple of 8). */
static inline uint32_t image_morton_offset(uint32_t x, uint32_t y,
                                           uint32_t tex_width)
{
	uint32_t tile = ((y >> 3) * (tex_width >> 3) + (x >> 3)) << 6;
	uint32_t in_tile = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
	                   ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
	return tile + in_tile;
}

/* Box-filters an RGBA image (w x h) down to dw x dh and writes it to the
 * top-left corner of a tex_size x tex_size GPU_RGBA8 texture. Pixels outside
 * dw x dh are left untouched. Upscaling degrades to nearest neighbour. */
static inline void image_to_texture(const uint8_t *src, int w, int h,
                                    uint8_t *tex, int tex_size,
                                    int dw, int dh)
{
	if (!src || !tex || w <= 0 || h <= 0 || dw <= 0 || dh <= 0 ||
	    dw > tex_size || dh > tex_size)
		return;
	for (int dy = 0; dy < dh; dy++) {
		int y0 = (int)((long long)dy * h / dh);
		int y1 = (int)((long long)(dy + 1) * h / dh);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (int dx = 0; dx < dw; dx++) {
			int x0 = (int)((long long)dx * w / dw);
			int x1 = (int)((long long)(dx + 1) * w / dw);
			if (x1 <= x0)
				x1 = x0 + 1;
			uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
			for (int y = y0; y < y1; y++) {
				const uint8_t *p = src + ((size_t)y * w + x0) * 4;
				for (int x = x0; x < x1; x++, p += 4) {
					r += p[0];
					g += p[1];
					b += p[2];
					a += p[3];
					n++;
				}
			}
			uint8_t *out = tex + (size_t)image_morton_offset(
				(uint32_t)dx, (uint32_t)dy, (uint32_t)tex_size) * 4;
			out[0] = (uint8_t)(a / n);
			out[1] = (uint8_t)(b / n);
			out[2] = (uint8_t)(g / n);
			out[3] = (uint8_t)(r / n);
		}
	}
}

#endif
