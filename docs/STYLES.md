# Guía de estilo de 3DSpotify — sistema «Bisagra»

Instrucciones visuales para la interfaz con citro2d en ambas pantallas.

---

## 1. Principios

1. La pantalla superior muestra información; la inferior recibe la interacción.
2. Estética limpia y moderna con un toque Frutiger Aero: superficies brillantes, esquinas
   redondeadas, cristal translúcido, degradados verticales suaves y reflejos claros.
3. Toda pulsación, táctil o física, produce una respuesta visual inmediata (§6).
4. El reproductor solo se redibuja en los casos de §9.
5. Usar exclusivamente los tokens de `source/ui/theme.h`. Ningún color, tamaño o radio suelto
   en el código.

---

## 2. Color

### 2.1 Base

| Token          | Hex / RGBA            | Uso                                                       |
|----------------|-----------------------|-----------------------------------------------------------|
| `fondo_sup`    | `#0C2A1B`             | Parte superior del degradado de fondo.                    |
| `fondo_inf`    | `#061309`             | Parte inferior del degradado de fondo.                    |
| `capa`         | `#0E2618`             | Superficies opacas: filas, paneles.                       |
| `capa_alta`    | `#163522`             | Superficie en foco sin acento.                            |
| `cristal`      | `#0E2618` al 75 %     | Paneles translúcidos sobre fondo o carátula.              |
| `clorofila`    | `#14B358`             | Acento principal: botón primario, progreso, selección.    |
| `musgo`        | `#217846`             | Parte inferior de degradados de acento y estado pulsado.  |
| `menta`        | `#60DBA2`             | Parte superior de degradados de acento, texto de acento.  |
| `brisa`        | `#7FE3D0`             | Halo de foco.                                             |
| `tinta`        | `#020803`             | Texto sobre `clorofila` y sobre botones de color.         |

### 2.2 Luz y sombra

| Token          | RGBA                       | Uso                                         |
|----------------|----------------------------|---------------------------------------------|
| `reflejo`      | blanco, 30 % → 5 %         | Brillo superior de botones y paneles.       |
| `filo`         | blanco, 20 %               | Borde superior de 1 px en superficies.      |
| `sombra_1`     | negro, 25 %                | Sombra cercana, desplazada 1 px abajo.      |
| `sombra_2`     | negro, 12 %                | Sombra lejana, desplazada 3 px abajo.       |

### 2.3 Texto

| Token          | Hex       | Uso                                         |
|----------------|-----------|---------------------------------------------|
| `texto`        | `#E6F4EA` | Texto principal.                            |
| `texto_2`      | `#93B8A3` | Artista, álbum, metadatos, ayudas.          |
| `texto_off`    | `#557562` | Deshabilitado.                              |

### 2.4 Estado

| Token          | Hex       | Uso                                         |
|----------------|-----------|---------------------------------------------|
| `coral`        | `#FF6F5E` | Errores, siempre con icono y texto.         |
| `ambar`        | `#F2C14E` | Avisos recuperables.                        |

### 2.5 Botones físicos

| Botón  | Hex       |
|--------|-----------|
| A      | `#E8463C` |
| B      | `#F2C14E` |
| X      | `#3C8CE8` |
| Y      | `#2FB86A` |
| START / SELECT / cruceta | `capa_alta` |

### 2.6 Reglas

- Texto sobre `clorofila` o sobre un glifo de botón de color: siempre `tinta`.
- `texto`, `texto_2` y `clorofila` sobre `fondo_*`, `capa` y `capa_alta`: contraste ≥ 4.5:1.
- `texto_off` solo para elementos deshabilitados.
- Degradados únicamente verticales y entre dos tokens adyacentes de la misma familia
  (`menta`→`clorofila`, `clorofila`→`musgo`, `fondo_sup`→`fondo_inf`).
- No usar `#1DB954` ni `#1ED760`.

---

## 3. Tipografía

### 3.1 Fuente

Fuente del sistema (`C2D_FontLoadSystem` o fuente por defecto de citro2d) para todo.

### 3.2 Escala

| Token      | Escala C2D | Uso                                          |
|------------|-----------:|----------------------------------------------|
| `t_titulo` | 0.75       | Título de canción en el reproductor          |
| `t_cabeza` | 0.60       | Cabecera de pantalla, nombre en fila         |
| `t_cuerpo` | 0.50       | Texto general, artista en fila               |
| `t_meta`   | 0.45       | Metadatos, barra de botones. Mínimo.         |

### 3.3 Reglas

- No usar escalas por debajo de `t_meta`.
- Una línea por dato. Recortar con `...` midiendo con `C2D_TextGetDimensions`.
- Mayúscula solo al inicio de frase.
- Alinear a la izquierda en listas y metadatos; centrar solo en pantallas de un único mensaje.
- Todo texto visible sale de `tr(STR_...)`, presente en `lang_en.c` y `lang_es.c`.
- Cada `STR_KEYS_*` debe caber en la barra de botones a `t_meta` (≈ 300 px útiles).

---

## 4. Forma y espacio

### 4.1 Pantallas

| Pantalla  | Tamaño   | Rol          |
|-----------|----------|--------------|
| Superior  | 400×240  | Información  |
| Inferior  | 320×240  | Interacción  |

### 4.2 Espaciado (unidad 4 px)

| Token | px | Uso                                       |
|-------|---:|-------------------------------------------|
| `e1`  | 4  | Separación icono-texto.                   |
| `e2`  | 8  | Margen lateral de la pantalla inferior.   |
| `e3`  | 12 | Margen lateral de la pantalla superior.   |
| `e4`  | 16 | Separación entre bloques.                 |
| `e6`  | 24 | Alto de cabecera y barra de botones.      |

### 4.3 Radios

| Token  | px | Uso                                           |
|--------|---:|-----------------------------------------------|
| `r_s`  | 6  | Filas de lista, campos, avisos.               |
| `r_m`  | 10 | Botones, paneles, carátula.                   |
| `r_p`  | alto / 2 | Píldoras: barras de progreso, glifos de botón, burbujas. |

Dibujar los rectángulos redondeados con un único helper (`ui_round_rect` en
`source/ui/draw.c`): rectángulo central + cuatro `C2D_DrawCircleSolid` en las esquinas.

### 4.4 Objetivos táctiles

- Alto mínimo 32 px.
- Separación mínima de 4 px entre objetivos.

---

## 5. Componentes

### 5.1 Superficie Aero (base de todo)

Orden de dibujo de cualquier botón o panel:

1. `sombra_2`, desplazada 3 px abajo, mismo radio.
2. `sombra_1`, desplazada 1 px abajo, mismo radio.
3. Relleno con degradado vertical (`C2D_DrawRectangle` con colores por vértice).
4. `reflejo`: mitad superior del elemento, inset 2 px, degradado de blanco 30 % a 5 %.
5. `filo`: línea de 1 px en el borde superior, inset igual al radio.

Paneles de cristal: relleno `cristal` plano en el paso 3 y `reflejo` al 15 %.

### 5.2 Botón

| Variante   | Relleno (arriba → abajo) | Texto / icono | Otros                       |
|------------|--------------------------|---------------|-----------------------------|
| Primario   | `menta` → `clorofila`    | `tinta`       |                             |
| Secundario | `capa_alta` → `capa`     | `texto`       |                             |
| Peligro    | `capa_alta` → `capa`     | `coral`       | Contorno 1 px `coral`       |

- Alto 32 px (primario del reproductor 48×48, circular), radio `r_m`, padding horizontal 12 px,
  texto `t_cuerpo`.
- Máximo un botón primario por pantalla.

### 5.3 Fila de lista

```
 ╭──────────────────────────────────────────╮ 36 px
 │ Título de la canción...            3:42  │  t_cabeza · texto   /  t_meta · texto_2
 │ Artista                                  │  t_cuerpo · texto_2
 ╰──────────────────────────────────────────╯
```

- Reposo: `capa`, radio `r_s`, sin sombra, separación 4 px entre filas.
- Seleccionada: superficie Aero con relleno `capa_alta` → `capa` + halo de foco (§6.2).
- Duración alineada a la derecha, nunca recortada.
- Sin miniaturas.

### 5.4 Barra de progreso

- Píldora de 8 px de alto, fondo `capa_alta`.
- Relleno de progreso: `menta` → `clorofila`, con `reflejo` en la mitad superior.
- Descarga: píldora `musgo` al 60 % por detrás del progreso.
- Tirador: círculo de 12 px, `texto`, con `sombra_1`.
- Actualizar como máximo una vez por segundo.

### 5.5 Barra de volumen

Igual que la barra de progreso, 6 px de alto, sin capa de descarga.

### 5.6 Barra de botones

- Franja de 24 px al pie de la pantalla inferior, panel de cristal sin radio, `filo` arriba.
- Cada acción: glifo + etiqueta `t_meta` · `texto_2`.
- Glifo: píldora de 14 px de alto, relleno con el color del botón (§2.5) aclarado arriba,
  `reflejo`, letra en `tinta`.
- Máximo cuatro acciones.

```
 (A) Pausa   (◀▶) Pista   (▲▼) Vol.   (SELECT) Idioma
```

### 5.7 Indicador de carga

- Tres barras de 4×12 px, radio 2 px, `menta` → `clorofila`, subiendo y bajando, 6 frames por
  paso.
- Solo en pantallas de espera. Nunca en el reproductor con audio sonando.

### 5.8 Aviso

- Panel de cristal, radio `r_s`, contorno 1 px `coral` o `ambar`.
- Icono 16 px a la izquierda, título `t_cabeza`, explicación `t_cuerpo`.

### 5.9 Burbuja

- Píldora de cristal centrada a 8 px sobre la barra de botones, icono 16 px + texto `t_cuerpo`.
- Visible 60 frames, aparece en 4 frames (opacidad 0 → 100 %), desaparece en 8.
- Solo una a la vez; una nueva sustituye a la anterior.

### 5.10 Iconos

- 16×16 (barras, listas) y 24×24 (controles del reproductor).
- Formas redondeadas, trazo de 2 px, antialiasing.
- Iconos sobre botones primarios: `tinta`. Sobre superficies oscuras: `texto`.
- Conjunto: reproducir, pausa, anterior, siguiente, volumen, buscar, cuenta, Wi-Fi, error,
  aviso, idioma, nota musical.
- Embeberlos como arrays C generados (mismo patrón que `ca_bundle.c`).

---

## 6. Estados e interacción

### 6.1 Estados de un control

| Estado         | Aspecto                                                                 |
|----------------|-------------------------------------------------------------------------|
| Reposo         | Superficie Aero completa (§5.1).                                        |
| Foco           | Reposo + halo de foco.                                                  |
| Pulsado        | Ver §6.3.                                                               |
| Deshabilitado  | Relleno `capa`, sin `reflejo` ni sombras, texto e icono `texto_off`.   |

### 6.2 Halo de foco

- Rectángulo redondeado 2 px mayor por cada lado, `brisa` al 40 %, radio del control + 2.
- Aparece en 4 frames al recibir el foco.

### 6.3 Estado pulsado (obligatorio en todo control)

Al detectar la pulsación, en el mismo frame:

- Relleno sustituido por su versión hundida: primario `clorofila` → `musgo`; secundario
  `capa` → `fondo_inf`.
- `reflejo` al 10 %.
- Sin `sombra_1` ni `sombra_2`.
- Contenido (texto e icono) desplazado 1 px abajo.
- Escala del control al 96 % respecto a su centro.

Duración:

- Se mantiene mientras el botón o el lápiz sigan pulsados.
- Duración mínima visible: 6 frames, aunque se suelte antes.
- Vuelta a reposo en 4 frames.

### 6.4 Pulsación táctil

- Pulsado al tocar dentro del control.
- La acción se ejecuta al levantar el lápiz dentro del control.
- Si el lápiz sale del control antes de levantarse: volver a reposo y no ejecutar la acción.
- Tocar fuera de cualquier control: sin respuesta.

### 6.5 Pulsación con botones físicos

- La acción se ejecuta al pulsar (`hidKeysDown`).
- El control en pantalla asociado al botón muestra el estado pulsado (§6.3) con la misma
  duración.
- El glifo de ese botón en la barra de botones se invierte durante el mismo tiempo: relleno
  `texto`, letra en el color del botón, escala 96 %.
- Si la acción no tiene control en pantalla (SELECT idioma, START salir, cambio de volumen
  fuera del reproductor), mostrar una burbuja (§5.9) con el resultado: «English», «Volumen 60 %».

### 6.6 Cruceta en listas

- El foco se mueve en el mismo frame de la pulsación.
- La nueva fila recibe el halo de foco (§6.2).
- Al intentar pasar del primer o último elemento: la fila en foco se desplaza 3 px en esa
  dirección y vuelve en 6 frames. La cola no da la vuelta.
- Al cambiar de página, la lista nueva aparece sin transición.

### 6.7 Volumen

- Arriba/abajo actualiza la barra de volumen en el mismo frame.
- En el reproductor, el glifo ▲▼ de la barra de botones se invierte (§6.5).

### 6.8 Reproducir / pausa

- El icono del botón central cambia en el mismo frame que el estado de audio.
- El botón central muestra el estado pulsado (§6.3) tanto con A como con toque.

---

## 7. Pantallas

### 7.1 Fondo común

Ambas pantallas: degradado vertical `fondo_sup` → `fondo_inf` a pantalla completa.

### 7.2 Arranque / sesión

```
 SUPERIOR 400×240                          INFERIOR 320×240
 ╭────────────────────────────────╮        ╭──────────────────────────╮
 │                                │        │                          │
 │          [logotipo]            │        │  Renovando credenciales  │
 │                                │        │  y conectando...         │
 │          (indicador)           │        │                          │
 │                                │        │──────────────────────────│
 ╰────────────────────────────────╯        │ (A) Reintentar (Y) Empar.│
                                           ╰──────────────────────────╯
```

- Superior: logotipo y, debajo, indicador de carga, centrados.
- Inferior: paso actual en `t_cuerpo`, centrado.
- Al fallar: sustituir el indicador por el icono de error y mostrar un aviso abajo.

### 7.3 Emparejamiento

- Superior: código en `t_titulo` × 1.5 dentro de un panel de cristal centrado; URL debajo en
  `t_cuerpo` · `menta`.
- Inferior: estado del sondeo y barra de botones con B.

### 7.4 Resultados

```
 SUPERIOR                                  INFERIOR
 ╭────────────────────────────────╮        ╭──────────────────────────╮
 │ Resultados: "consulta"         │        │ Pág. 1/4      24 canc.   │ 24 px
 │                                │        │╭────────────────────────╮│
 │  Título de la fila en foco     │        ││Canción uno        3:42 ││
 │  Artista                       │        ││Artista                 ││ 5 filas
 │  Álbum · 3:42                  │        │╰────────────────────────╯│ × 36 px
 │                                │        │ ...                      │
 │                                │        │──────────────────────────│
 ╰────────────────────────────────╯        │ (A) Elegir (Y) Buscar (B)│ 24 px
                                           ╰──────────────────────────╯
```

- Inferior: cabecera 24 px, 5 filas de 36 px, barra de botones.
- Superior: detalle de la fila en foco en panel de cristal.
- Fijar `RESULTS_PER_PAGE` a 5.
- El teclado (`swkbd`) se deja con el aspecto del sistema.

### 7.5 Reproductor

```
 SUPERIOR                                  INFERIOR
 ╭────────────────────────────────╮        ╭──────────────────────────╮
 │ ╭──────────╮                   │        │ Pista 3/24 de la cola    │
 │ │          │  Título canción   │        │                          │
 │ │ carátula │  Artista          │        │   (◀◀)   ( ▶ )   (▶▶)    │
 │ │ 160×160  │  Álbum            │        │                          │
 │ │          │                   │        │ ━━━━━━━●──────  1:24/3:42│
 │ ╰──────────╯                   │        │ Vol ━━━━━●───            │
 │                                │        │──────────────────────────│
 ╰────────────────────────────────╯        │ (A) Pausa (◀▶) Pista ... │
                                           ╰──────────────────────────╯
```

- Carátula 160×160, a 12 px de los bordes superior e izquierdo, radio `r_m`, con `sombra_1`,
  `sombra_2` y `reflejo` al 12 % en su mitad superior.
- Mientras carga: hueco `capa` del mismo tamaño y radio con el icono de nota en `texto_off`.
  El resto del layout no se mueve al llegar la imagen.
- Metadatos a la derecha de la carátula: título `t_titulo` · `texto`, artista y álbum
  `t_cuerpo` · `texto_2`.
- Inferior: botón central primario circular 48×48; anterior y siguiente secundarios
  circulares 40×40; barras de progreso y volumen debajo.

### 7.6 Pista terminada / error

- Superior: última carátula o, si no la hay, el logotipo.
- Inferior: aviso centrado y barra de botones.

---

## 8. Movimiento

- Duraciones en frames a 60 Hz. Máximo 8 frames.
- Interpolación lineal o ease-out simple.
- Ninguna animación en bucle fuera de los indicadores de carga.

---

## 9. Rendimiento

- Cada pantalla tiene un flag `dirty`. Llamar a `C3D_FrameBegin` / `C3D_FrameEnd` solo cuando
  `dirty` sea verdadero.
- `C2D_TextParse` + `C2D_TextOptimize` solo al cambiar el contenido, nunca en el bucle de
  dibujo.
- Carátula: textura `C3D_Tex` 256×256 RGBA8 con subtextura al tamaño real; conversión a
  formato tiled en el hilo de descarga.
- En el reproductor solo se marca `dirty` por:
  - progreso y reloj (≤ 1 vez por segundo),
  - feedback de pulsación (§6) mientras dura la animación,
  - cambio de volumen o pausa,
  - llegada de la carátula,
  - burbuja visible.
- No dibujar indicadores de carga, marquesinas ni animaciones en bucle en el reproductor.

---

## 10. Implementación

### 10.1 `source/ui/theme.h`

```c
/* Tokens visuales del sistema Bisagra (docs/guia-de-estilo.md).
 * Sin dependencias de 3DS para poder testearlos en host.
 * Empaquetado RGBA con R en el byte bajo, igual que C2D_Color32(). */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

#define THEME_RGBA(r, g, b, a) \
	((uint32_t)(r) | ((uint32_t)(g) << 8) | \
	 ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define THEME_RGB(r, g, b) THEME_RGBA(r, g, b, 0xFF)

/* Base */
#define COL_FONDO_SUP  THEME_RGB(0x0C, 0x2A, 0x1B)
#define COL_FONDO_INF  THEME_RGB(0x06, 0x13, 0x09)
#define COL_CAPA       THEME_RGB(0x0E, 0x26, 0x18)
#define COL_CAPA_ALTA  THEME_RGB(0x16, 0x35, 0x22)
#define COL_CRISTAL    THEME_RGBA(0x0E, 0x26, 0x18, 0xBF)
#define COL_CLOROFILA  THEME_RGB(0x14, 0xB3, 0x58)
#define COL_MUSGO      THEME_RGB(0x21, 0x78, 0x46)
#define COL_MENTA      THEME_RGB(0x60, 0xDB, 0xA2)
#define COL_BRISA      THEME_RGB(0x7F, 0xE3, 0xD0)
#define COL_TINTA      THEME_RGB(0x02, 0x08, 0x03)

/* Luz y sombra */
#define COL_REFLEJO_ALTO  THEME_RGBA(0xFF, 0xFF, 0xFF, 0x4D)
#define COL_REFLEJO_BAJO  THEME_RGBA(0xFF, 0xFF, 0xFF, 0x0D)
#define COL_FILO          THEME_RGBA(0xFF, 0xFF, 0xFF, 0x33)
#define COL_SOMBRA_1      THEME_RGBA(0x00, 0x00, 0x00, 0x40)
#define COL_SOMBRA_2      THEME_RGBA(0x00, 0x00, 0x00, 0x1F)
#define COL_HALO          THEME_RGBA(0x7F, 0xE3, 0xD0, 0x66)

/* Texto */
#define COL_TEXTO      THEME_RGB(0xE6, 0xF4, 0xEA)
#define COL_TEXTO_2    THEME_RGB(0x93, 0xB8, 0xA3)
#define COL_TEXTO_OFF  THEME_RGB(0x55, 0x75, 0x62)

/* Estado */
#define COL_CORAL      THEME_RGB(0xFF, 0x6F, 0x5E)
#define COL_AMBAR      THEME_RGB(0xF2, 0xC1, 0x4E)

/* Botones físicos */
#define COL_BTN_A      THEME_RGB(0xE8, 0x46, 0x3C)
#define COL_BTN_B      THEME_RGB(0xF2, 0xC1, 0x4E)
#define COL_BTN_X      THEME_RGB(0x3C, 0x8C, 0xE8)
#define COL_BTN_Y      THEME_RGB(0x2F, 0xB8, 0x6A)

/* Espaciado y forma (px) */
enum {
	SP_1 = 4, SP_2 = 8, SP_3 = 12, SP_4 = 16, SP_6 = 24,
	RAD_S = 6, RAD_M = 10,
	TOUCH_MIN = 32,
	BAR_H = 24,
	ROW_H = 36,
	ART_SIZE = 160,
	BTN_PLAY = 48, BTN_SKIP = 40,
};

/* Movimiento (frames a 60 Hz) */
enum {
	ANIM_PRESS_MIN = 6,
	ANIM_RELEASE = 4,
	ANIM_FOCUS = 4,
	ANIM_EDGE = 6,
	BUBBLE_IN = 4, BUBBLE_SHOW = 60, BUBBLE_OUT = 8,
};
#define PRESS_SCALE 0.96f

/* Escalas de texto */
#define TXT_TITULO 0.75f
#define TXT_CABEZA 0.60f
#define TXT_CUERPO 0.50f
#define TXT_META   0.45f

#endif
```

### 10.2 Tareas asociadas

- Crear `source/ui/draw.c` / `draw.h` con `ui_round_rect`, `ui_surface` (§5.1) y
  `ui_button` (estados de §6).
- Lógica de estados de pulsación (reposo/pulsado/duración mínima/cancelación táctil) en un
  header puro y testeado en host.
- Añadir test en host de contraste ≥ 4.5:1 para los pares de §2.6.
- Actualizar `tests/test_build.py` donde cambien cadenas de `main.c`.
- Reflejar los controles táctiles en `README.md` y `README_es.md`.

---

## 11. Lista de comprobación por pantalla

- [ ] Colores, tamaños, radios y duraciones salen de `theme.h`.
- [ ] Texto sobre `clorofila` y sobre glifos de color en `tinta`.
- [ ] Nada por debajo de `TXT_META`; recortes medidos.
- [ ] Objetivos táctiles ≥ 32 px; todo accesible también con botones.
- [ ] Cada control tiene estados reposo, foco, pulsado y deshabilitado.
- [ ] Toda pulsación táctil y física muestra el estado pulsado ≥ 6 frames.
- [ ] Botones físicos invierten su glifo en la barra de botones.
- [ ] Acciones sin control en pantalla muestran burbuja.
- [ ] Ningún texto visible fuera de `tr(STR_...)`, en ambos idiomas.
- [ ] El reproductor solo redibuja en los casos de §9.
- [ ] Probado en consola real con audio sonando.
