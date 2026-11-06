/* Tokens visuales del sistema Bisagra (docs/STYLES.md).
 * Sin dependencias de 3DS para poder testearlos en host.
 * Empaquetado RGBA con R en el byte bajo, igual que C2D_Color32().
 * Todos los colores son opacos. */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

#define THEME_RGB(r, g, b) \
	((uint32_t)(r) | ((uint32_t)(g) << 8) | \
	 ((uint32_t)(b) << 16) | 0xFF000000u)

/* Fondo y superficies */
#define COL_FONDO_SUP  THEME_RGB(0x1A, 0x70, 0x49)
#define COL_FONDO_INF  THEME_RGB(0x13, 0x60, 0x40)
#define COL_CAPA       THEME_RGB(0x0F, 0x4B, 0x30)
#define COL_CAPA_ALTA  THEME_RGB(0x17, 0x60, 0x3E)
#define COL_CAPA_LUZ   THEME_RGB(0x23, 0x7A, 0x52)
#define COL_SOMBRA     THEME_RGB(0x08, 0x30, 0x1E)

/* Acento */
#define COL_CLOROFILA  THEME_RGB(0x2E, 0xE8, 0x7A)
#define COL_MENTA      THEME_RGB(0x8C, 0xF5, 0xB5)
#define COL_MUSGO      THEME_RGB(0x1B, 0xB8, 0x5C)
#define COL_LIMA       THEME_RGB(0xC6, 0xF5, 0x42)
#define COL_TINTA      THEME_RGB(0x04, 0x14, 0x0B)

/* Texto */
#define COL_TEXTO      THEME_RGB(0xFF, 0xFF, 0xFF)
#define COL_TEXTO_2    THEME_RGB(0xC4, 0xEE, 0xD6)
#define COL_TEXTO_OFF  THEME_RGB(0x8A, 0xBC, 0xA0)

/* Estado */
#define COL_CORAL      THEME_RGB(0xFF, 0x7A, 0x66)
#define COL_CORAL_LUZ  THEME_RGB(0xFF, 0xB0, 0xA3)
#define COL_CORAL_OSC  THEME_RGB(0xD8, 0x54, 0x43)
#define COL_AMBAR      THEME_RGB(0xFF, 0xD2, 0x5C)
#define COL_AMBAR_LUZ  THEME_RGB(0xFF, 0xE7, 0x9E)
#define COL_AMBAR_OSC  THEME_RGB(0xC9, 0x9A, 0x1F)

/* Botones físicos */
#define COL_BTN_A      THEME_RGB(0xFF, 0x4F, 0x45)
#define COL_BTN_B      THEME_RGB(0xFF, 0xD2, 0x3F)
#define COL_BTN_X      THEME_RGB(0x3F, 0xA2, 0xFF)
#define COL_BTN_Y      THEME_RGB(0x39, 0xD9, 0x7A)

/* Espaciado y forma (px) */
enum {
	SP_1 = 4, SP_2 = 8, SP_3 = 12, SP_4 = 16, SP_6 = 24,
	OUTLINE_W = 1,
	BEVEL_W = 2,
	SHADOW_OFF = 2,
	ART_SHADOW_OFF = 3,
	PRESS_SHIFT = 2,
	FOCUS_W = 2, FOCUS_GAP = 1,
	TOUCH_MIN = 32,
	BAR_H = 24,
	TAB_H = 32,
	ROW_H = 41,
	THUMB_SIZE = 33,
	ART_SIZE = 160,
	BTN_PLAY = 48, BTN_SKIP = 40,
	GLYPH_H = 14,
	PROGRESS_H = 8, VOLUME_H = 6,
};

/* Movimiento (frames a 60 Hz) */
enum {
	ANIM_PRESS_MIN = 6,
	ANIM_EDGE = 6,
	EDGE_SHIFT = 3,
	LOADER_STEP = 6,
	BUBBLE_SHOW = 60,
};

/* Escalas de texto */
#define TXT_TITULO 0.75f
#define TXT_CABEZA 0.60f
#define TXT_CUERPO 0.50f
#define TXT_META   0.45f

/* Derivados: medidas que la guía fija en el texto (§4, §5, §6) y no en la
 * tabla de §10.1. Aquí para que no haya números sueltos en el dibujo. */
enum {
	SCREEN_H = 240,
	SCREEN_TOP_W = 400,
	SCREEN_BOTTOM_W = 320,
	ICON_S = 16,          /* §5.10 iconos de barras y listas */
	ICON_M = 24,          /* §5.10 iconos del reproductor */
	BUBBLE_H = 28,        /* §5.9 icono de 16 px + 6 px arriba y abajo */
	GLYPH_SHIFT = 1,      /* §6.5 glifo invertido, 1 px abajo */
	LOADER_BAR_W = 4,     /* §5.7 barras de 4×12 px */
	LOADER_BAR_H = 12,
	LOADER_STEP_PX = 2,   /* §5.7 pasos de 2 px */
	SCROLL_W = 4,         /* §5.11 indicador de desplazamiento (e1) */
	SCROLL_MIN_H = 16,    /* §5.11 alto mínimo del tirador (e4) */
	DRAG_START = 8,       /* §6.9 un toque pasa a arrastre (e2) */
	HANDLE_W = 6,         /* §5.4 tirador de 6×14 px */
	HANDLE_H = 14,
	ART_BIG = 216,        /* carátula grande de la pantalla superior: 240 - 2·e3 */
};

#endif
