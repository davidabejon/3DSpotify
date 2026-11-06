#include "logo.h"

#include "draw.h"
#include "image_util.h"
#include "lang.h"
#include "theme.h"

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declarations only: the implementation lives in artwork.c. */
#include "../../include/stb_image.h"

/* A 256x128 RGBA PNG is far below this even uncompressed. */
#define LOGO_MAX_BYTES (256 * 1024)
#define FALLBACK_GLYPHS 32

static C3D_Tex tex;
static Tex3DS_SubTexture subtex;
static bool has_texture;
static int logo_w, logo_h;

/* The fallback: the app name, reparsed when the language changes. */
static C2D_TextBuf text_buf;
static C2D_Text name_text;
static lang_id parsed_lang;
static bool parsed;

static unsigned char *read_file(const char *path, int *len)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return NULL;
	unsigned char *data = NULL;
	long size = -1;
	if (fseek(file, 0, SEEK_END) == 0)
		size = ftell(file);
	if (size > 0 && size <= LOGO_MAX_BYTES && fseek(file, 0, SEEK_SET) == 0)
		data = malloc((size_t)size);
	if (data && fread(data, 1, (size_t)size, file) != (size_t)size) {
		free(data);
		data = NULL;
	}
	fclose(file);
	if (data)
		*len = (int)size;
	return data;
}

/* Decodes the PNG into a power-of-two GPU_RGBA8 texture, unfiltered. */
static bool load_texture(void)
{
	int len = 0;
	unsigned char *file = read_file(LOGO_PATH, &len);
	if (!file)
		return false;

	bool ok = false;
	int w = 0, h = 0, channels = 0;
	stbi_uc *pixels = NULL;
	/* Check the size before decoding: a large image is rejected, not
	 * rescaled. */
	if (stbi_info_from_memory(file, len, &w, &h, &channels) &&
	    w > 0 && h > 0 && w <= LOGO_MAX_W && h <= LOGO_MAX_H)
		pixels = stbi_load_from_memory(file, len, &w, &h, &channels, 4);
	free(file);
	if (!pixels)
		return false;

	int tex_w = image_pow2(w);
	int tex_h = image_pow2(h);
	if (tex_w && tex_h && C3D_TexInit(&tex, (u16)tex_w, (u16)tex_h,
	                                  GPU_RGBA8)) {
		/* Transparent outside the image, so no stray texels show. */
		memset(tex.data, 0, (size_t)tex_w * tex_h * 4);
		image_copy_to_texture(pixels, w, h, tex.data, tex_w, tex_h);
		C3D_TexFlush(&tex);
		C3D_TexSetFilter(&tex, GPU_NEAREST, GPU_NEAREST);
		subtex.width = (u16)w;
		subtex.height = (u16)h;
		subtex.left = 0.0f;
		subtex.top = 1.0f;
		subtex.right = (float)w / tex_w;
		subtex.bottom = 1.0f - (float)h / tex_h;
		logo_w = w;
		logo_h = h;
		ok = true;
	}
	stbi_image_free(pixels);
	return ok;
}

bool logo_load(void)
{
	logo_free();
	text_buf = C2D_TextBufNew(FALLBACK_GLYPHS);
	has_texture = load_texture();
	return has_texture;
}

void logo_free(void)
{
	if (has_texture)
		C3D_TexDelete(&tex);
	has_texture = false;
	logo_w = logo_h = 0;
	if (text_buf)
		C2D_TextBufDelete(text_buf);
	text_buf = NULL;
	parsed = false;
}

static void draw_fallback(int cx, int cy)
{
	if (!text_buf)
		return;
	if (!parsed || parsed_lang != lang_get()) {
		C2D_TextBufClear(text_buf);
		C2D_TextParse(&name_text, text_buf, tr(STR_APP_NAME));
		C2D_TextOptimize(&name_text);
		parsed_lang = lang_get();
		parsed = true;
	}
	ui_text_centered(&name_text, cx, cy, TXT_TITULO, COL_TEXTO);
}

void logo_draw_centered(int cx, int cy)
{
	if (!has_texture) {
		draw_fallback(cx, cy);
		return;
	}
	C2D_Image image = { &tex, &subtex };
	C2D_DrawImageAt(image, (float)(cx - logo_w / 2), (float)(cy - logo_h / 2),
	                0.0f, NULL, 1.0f, 1.0f);
}
