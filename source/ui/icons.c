#include "icons.h"

#include "image_util.h"
#include "theme.h"

#include <citro3d.h>

#include <string.h>

#define ATLAS_SIZE 64
#define PER_ROW (ATLAS_SIZE / ICON_MASK_SIZE)

_Static_assert(ICON_MASK_SIZE == ICON_S, "icons are 16 px (STYLES.md 5.10)");
_Static_assert(ICON_COUNT <= PER_ROW * PER_ROW, "the icon atlas is full");

static C3D_Tex atlas;
static Tex3DS_SubTexture subtex[ICON_COUNT];
static bool ready;

bool icons_init(void)
{
	if (!C3D_TexInit(&atlas, ATLAS_SIZE, ATLAS_SIZE, GPU_RGBA8))
		return false;
	u8 *texels = atlas.data;
	memset(texels, 0, (size_t)ATLAS_SIZE * ATLAS_SIZE * 4);
	for (int i = 0; i < ICON_COUNT; i++) {
		int ox = (i % PER_ROW) * ICON_MASK_SIZE;
		int oy = (i / PER_ROW) * ICON_MASK_SIZE;
		for (int y = 0; y < ICON_MASK_SIZE; y++) {
			for (int x = 0; x < ICON_MASK_SIZE; x++) {
				size_t at = (size_t)image_morton_offset(
					(uint32_t)(ox + x), (uint32_t)(oy + y), ATLAS_SIZE) * 4;
				/* GPU_RGBA8 bytes are A, B, G, R. */
				texels[at] = icon_masks[i][y * ICON_MASK_SIZE + x];
				texels[at + 1] = texels[at + 2] = texels[at + 3] = 0xFF;
			}
		}
		const float size = ATLAS_SIZE;
		subtex[i].width = ICON_MASK_SIZE;
		subtex[i].height = ICON_MASK_SIZE;
		subtex[i].left = ox / size;
		subtex[i].right = (ox + ICON_MASK_SIZE) / size;
		subtex[i].top = 1.0f - oy / size;
		subtex[i].bottom = 1.0f - (oy + ICON_MASK_SIZE) / size;
	}
	C3D_TexFlush(&atlas);
	/* Drawn 1:1 at whole pixels: crisp, no antialiasing (5.10). */
	C3D_TexSetFilter(&atlas, GPU_NEAREST, GPU_NEAREST);
	ready = true;
	return true;
}

void icons_exit(void)
{
	if (ready)
		C3D_TexDelete(&atlas);
	ready = false;
}

C2D_Image icon_image(icon_id id)
{
	C2D_Image image = { NULL, NULL };
	if (ready && (unsigned)id < ICON_COUNT) {
		image.tex = &atlas;
		image.subtex = &subtex[id];
	}
	return image;
}
