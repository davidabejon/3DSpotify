# Guía de estilo de 3DSpotify — sistema «Bisagra»

Instrucciones visuales para la interfaz con citro2d en ambas pantallas.

---

## 1. Principios

1. La pantalla superior muestra información; la inferior recibe la interacción.
2. Estética limpia y geométrica: esquinas rectas, colores planos y vivos, relieve con bandas
   sólidas y sombras duras. Sin transparencias, sin brillos, sin cristal.
3. Toda pulsación, táctil o física, produce una respuesta visual inmediata (§6).
4. El reproductor solo se redibuja en los casos de §9.
5. Usar exclusivamente los tokens de `source/ui/theme.h`. Ningún color ni tamaño suelto en el
   código.

---

## 2. Color

Todos los colores son opacos (alfa 255).

### 2.1 Fondo y superficies

| Token          | Hex       | Uso                                                        |
|----------------|-----------|------------------------------------------------------------|
| `fondo_sup`    | `#1A7049` | Parte superior del degradado de fondo.                     |
| `fondo_inf`    | `#136040` | Parte inferior del degradado de fondo.                     |
| `capa`         | `#0F4B30` | Superficies: filas, paneles, pestañas, barra de botones.   |
| `capa_alta`    | `#17603E` | Relleno de botón secundario, pista de las barras.          |
| `capa_luz`     | `#237A52` | Banda superior del relieve de `capa_alta`.                 |
| `sombra`       | `#08301E` | Sombra dura, banda inferior del relieve neutro.            |

### 2.2 Acento

| Token          | Hex       | Uso                                                        |
|----------------|-----------|------------------------------------------------------------|
| `clorofila`    | `#2EE87A` | Acento principal: botón primario, progreso, fila en foco.  |
| `menta`        | `#8CF5B5` | Banda superior del relieve de acento.                      |
| `musgo`        | `#1BB85C` | Banda inferior del relieve de acento, estado pulsado, descarga. |
| `lima`         | `#C6F542` | Borde de foco.                                             |
| `tinta`        | `#04140B` | Contorno de 1 px, texto sobre colores claros.              |

### 2.3 Texto

| Token          | Hex       | Uso                                         |
|----------------|-----------|---------------------------------------------|
| `texto`        | `#FFFFFF` | Texto principal.                            |
| `texto_2`      | `#C4EED6` | Artista, álbum, metadatos, ayudas.          |
| `texto_off`    | `#8ABCA0` | Deshabilitado. Solo sobre `capa`.           |

### 2.4 Estado

| Token          | Hex       | Uso                                                 |
|----------------|-----------|-----------------------------------------------------|
| `coral`        | `#FF7A66` | Relleno de avisos de error, texto en `tinta`.       |
| `coral_osc`    | `#D85443` | Banda inferior del relieve de `coral`, `coral` pulsado. |
| `ambar`        | `#FFD25C` | Relleno de avisos recuperables, texto en `tinta`.   |
| `ambar_osc`    | `#C99A1F` | Banda inferior del relieve de `ambar`.              |

### 2.5 Botones físicos

| Botón  | Hex       |
|--------|-----------|
| A      | `#FF4F45` |
| B      | `#FFD23F` |
| X      | `#3FA2FF` |
| Y      | `#39D97A` |
| START / SELECT / cruceta / L / R | `capa_alta` |

### 2.6 Combinaciones de texto permitidas

| Fondo                                   | Texto permitido                         |
|-----------------------------------------|-----------------------------------------|
| `fondo_sup`, `fondo_inf`                | `texto`, `texto_2`                      |
| `capa`                                  | `texto`, `texto_2`, `texto_off`, `clorofila`, `lima` |
| `capa_alta`                             | `texto`, `texto_2`                      |
| `clorofila`, `musgo`, `lima`, `coral`, `coral_osc`, `ambar`, glifos A/B/X/Y | `tinta` |
| `texto`: solo el código QR (§5.13)      | `tinta`                                 |
| `texto_2`: solo los glifos START y SELECT (§5.6) | `tinta`                        |

Todas las combinaciones de la tabla tienen un contraste ≥ 4.5:1, también en estado pulsado
(`musgo`, `capa` y `coral_osc` con su texto). Cualquier otra combinación está prohibida.

### 2.7 Reglas

- Degradado únicamente en el fondo de pantalla (`fondo_sup` → `fondo_inf`, vertical). Todo
  lo demás, relleno plano.
- No usar transparencias ni mezclas alfa en ningún elemento. Excepciones: el logotipo
  (`romfs/gfx/logo.png`) se dibuja con su propio canal alfa, y el spinner de descarga de la
  pantalla superior va sobre un velo gris `#404040` al ~70 % de opacidad (`SCRIM_*` en
  `theme.h`), 8 px más grande que el spinner por cada lado. En las pantallas de arranque
  (§7.2) el spinner va sin velo, porque no hay nada debajo que tapar. La pantalla superior
  de Ajustes (§7.8) oscurece el fondo y el logotipo con un velo `sombra` al 80 % de opacidad
  (`ABOUT_VEIL_*` en `theme.h`); el resultado queda más oscuro que `capa`, así que el texto
  sobre él sigue las combinaciones de `capa` (§2.6).
- `texto` no se usa como relleno, salvo en el código QR (§5.13): un QR necesita módulos
  oscuros sobre fondo claro para que lo lea una cámara, y es la única superficie blanca de la
  aplicación.
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
- Una línea por dato. Recortar con `...` midiendo con `C2D_TextGetDimensions`. Única
  excepción: la ficha de detalle de la pantalla superior (§7.12), donde el texto que no cabe
  rueda (§5.15) en vez de recortarse.
- Mayúscula solo al inicio de frase.
- Alinear a la izquierda en listas y metadatos; centrar solo en pantallas de un único mensaje.
- Coordenadas de texto en píxeles enteros.
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
| `e6`  | 24 | Alto de la barra de botones.              |

### 4.3 Geometría

- Esquinas rectas en todos los elementos.
- Posiciones y tamaños en píxeles enteros; nada de coordenadas fraccionarias.
- Contorno: 1 px `tinta`.
- Relieve: banda superior de 2 px (clara) y banda inferior de 2 px (oscura).
- Sombra dura: rectángulo `sombra` del mismo tamaño, desplazado 2 px a la derecha y 2 px abajo.

### 4.4 Objetivos táctiles

- Alto mínimo 32 px.
- Separación mínima de 4 px entre objetivos.

---

## 5. Componentes

### 5.1 Superficie en relieve (base de todo)

Orden de dibujo de cualquier botón o panel elevado, todo con `C2D_DrawRectSolid`:

1. Sombra dura (§4.3).
2. Contorno 1 px `tinta` (rectángulo exterior).
3. Relleno plano.
4. Banda superior 2 px, color claro de la variante.
5. Banda inferior 2 px, color oscuro de la variante.

```
  ████████████████████        █ = tinta (contorno)
  █▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀█        ▀ = banda clara
  █     relleno      █▒       ▄ = banda oscura
  █▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄█▒       ▒ = sombra dura
  ████████████████████▒
    ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒
```

Paneles no interactivos (barra de botones, paneles de detalle) y pestañas en reposo: relleno
`capa`, contorno 1 px `tinta`, sin relieve ni sombra.

### 5.2 Botón

| Variante   | Relleno     | Banda clara | Banda oscura | Texto / icono |
|------------|-------------|-------------|--------------|---------------|
| Primario   | `clorofila` | `menta`     | `musgo`      | `tinta`       |
| Secundario | `capa_alta` | `capa_luz`  | `sombra`     | `texto`       |
| Peligro    | `coral`     | `#FFB0A3`   | `coral_osc`  | `tinta`       |

- Alto 32 px, padding horizontal 12 px, texto `t_cuerpo`.
- Botones del reproductor: primario 48×48, anterior y siguiente secundarios 40×40, cuadrados.
- Máximo un botón primario por pantalla.

### 5.3 Fila de lista

```
 ┌──────────────────────────────────────────┐ 41 px
 │ ┌────┐ Título de la canción...     3:42  │  t_cabeza · texto   /  t_meta · texto_2
 │ │ 33 │                                   │
 │ └────┘ Artista                           │  t_cuerpo · texto_2
 └──────────────────────────────────────────┘
   4 px  miniatura 33×33, texto a e2 (8 px) de ella
```

- Reposo: relleno `capa`, contorno 1 px `tinta`, sin relieve, separación 4 px entre filas.
- En foco: relleno `clorofila`, banda inferior 2 px `musgo`; título, artista y duración en
  `tinta`.
- Duración alineada a la derecha, nunca recortada.
- Miniatura de la carátula (`art_small`, 64 px) a 33×33, a 4 px de los bordes izquierdo,
  superior e inferior de la fila, con contorno 1 px `tinta`. El alto de la fila (41 px) es
  justo el título (`t_cabeza`) más el artista (`t_cuerpo`) con 4 px arriba y abajo.
- Mientras carga o si la pista no tiene carátula: cuadrado `capa_alta` del mismo tamaño con
  el icono de nota (16 px) centrado en `texto_2`. El layout de la fila no cambia cuando llega
  la imagen.
- La miniatura aparece sin transición.
- Variante con número (detalle de álbum, §7.11): todas las pistas comparten portada, así que
  en lugar de la miniatura el cuadrado 33×33 `capa_alta` lleva la posición en la lista
  (1, 2, 3…, continua entre discos) en `t_meta` · `texto_2`, centrada. También en la fila en
  foco: el cuadrado sigue siendo `capa_alta` y `tinta` sobre él apenas se leería. Sin
  descarga de miniaturas.
- Variante de colección (Descargas, §7.14): un álbum o una playlist entre canciones. Con
  portada, en su esquina inferior izquierda una insignia: el icono de disco o de líneas
  (16 px, `texto` sobre `tinta`); sin portada, la nota de siempre y sin insignia. La segunda
  línea dice «Álbum · Canciones: 12» o «Playlist · 8 de 40», y a la derecha va un chevron
  `>` en lugar de la duración: A la abre, no la reproduce. Se distingue de una canción sin
  depender del color, y los estados de foco no cambian.
- Variante sin descargar (colección descargada, §7.14): título, artista y número en
  `texto_off`, y a la derecha, en lugar de la duración, el icono de su estado (reloj en
  cola, error si falló). No toma el foco ni responde al toque.

### 5.4 Barra de progreso

- Rectángulo de 8 px de alto con contorno 1 px `tinta`; pista `capa_alta`.
- Descarga: relleno `musgo` por delante del progreso.
- Progreso: relleno `clorofila`.
- Tirador: rectángulo 6×14 px `texto` con contorno 1 px `tinta`, centrado en el extremo del
  progreso. Arrastrado con el táctil: relleno `lima` (estado pulsado, §6.3).
- Actualizar como máximo una vez por segundo, salvo mientras se arrastra: entonces sigue al
  dedo.

### 5.5 Barra de volumen

Igual que la barra de progreso, 6 px de alto, sin capa de descarga y sin tirador.

### 5.5.1 Barras táctiles

- En el reproductor, las dos barras se arrastran con el táctil. Su zona táctil es más grande
  que la barra: todo el ancho (la del volumen, desde la barra, sin la etiqueta «Vol.») y
  desde bajo los tiempos hasta la barra de botones, partida a medio camino entre las dos.
- Tocar dentro de la zona agarra la barra hasta levantar el lápiz, aunque se salga de ella;
  fuera de los extremos cuenta como el extremo.
- Volumen: sigue al dedo en el mismo frame (§6.7).
- Progreso: el tirador y el tiempo transcurrido siguen al dedo; la pista salta a ese punto al
  levantar el lápiz, no antes. Solo con la pista entera descargada (la capa de descarga
  llena) y mientras suena o está en pausa (no cargando ni terminada); antes, tocar la barra no
  hace nada. Si cambia la pista durante el arrastre, el arrastre se pierde.

### 5.6 Barra de botones

- Franja de 24 px al pie de la pantalla inferior, panel `capa` (§5.1), línea superior 1 px
  `tinta`.
- Cada acción: glifo + etiqueta `t_meta` · `texto_2`.
- Glifo: rectángulo de 14 px de alto, relleno con el color del botón (§2.5), contorno 1 px
  `tinta`, letra en `tinta`. START y SELECT no tienen color propio: relleno `texto_2`.
- Máximo cuatro acciones. Si no caben en el ancho, se quitan etiquetas desde la última y
  quedan solo sus glifos.

```
 [A] Pausa   [◀▶] Pista   [▲▼] Vol.   [SELECT] Idioma
```

### 5.7 Indicador de carga

- Tres barras de 4×12 px, `clorofila` con contorno 1 px `tinta`, subiendo y bajando en pasos
  de 2 px, 6 frames por paso.
- Solo en pantallas de espera. Nunca en el reproductor con audio sonando.

### 5.8 Aviso

- Superficie en relieve (§5.1) con relleno `coral` (error) o `ambar` (aviso), bandas
  `#FFB0A3`/`coral_osc` o `#FFE79E`/`ambar_osc`.
- Icono 16 px a la izquierda, título `t_cabeza`, explicación `t_cuerpo`, todo en `tinta`.

### 5.9 Burbuja

- Superficie en relieve con relleno `capa_alta`, centrada a 8 px sobre la barra de botones;
  icono 16 px + texto `t_cuerpo` · `texto`.
- Aparece y desaparece sin transición. Visible 60 frames.
- Una nueva del mismo tipo sustituye a la anterior.
- Burbuja de espera: la misma superficie con el indicador de carga (§5.7) en lugar del icono.
  Visible mientras dura la espera (por ejemplo, «Buscando…» en «Búsqueda»), no 60 frames.
- Pueden convivir una burbuja de espera y una normal: la de espera ocupa el sitio de siempre
  y la otra se apila encima, a 8 px de la sombra de la primera.

### 5.10 Iconos

- 16×16 (barras, listas) y 24×24 (controles del reproductor).
- Trazo de 2 px, formas geométricas, extremos rectos, alineados a la rejilla de píxeles, sin
  antialiasing.
- Iconos sobre `clorofila`, `coral`, `ambar`: `tinta`. Sobre `capa`, `capa_alta` y fondo:
  `texto`.
- Conjunto: reproducir, pausa, anterior, siguiente, volumen, buscar, cuenta, Wi-Fi, error,
  aviso, idioma, nota musical, inicio (casa), reproducción (ecualizador), ajustes
  (engranaje), aleatorio (dos flechas cruzadas) y, para las descargas, descargar (flecha a
  una bandeja), descargada (check), en cola (reloj), álbum (disco), playlist (líneas) y
  sin conexión (Wi-Fi tachado).
- Embeberlos como arrays C generados (mismo patrón que `ca_bundle.c`).
- El logotipo no es un icono: se carga en tiempo de ejecución desde `romfs:/gfx/logo.png`
  (directorio `romfs/` del repositorio, empaquetado en el `.3dsx` y el `.cia`). PNG RGBA de
  256×128 px como máximo, dibujado 1:1 sin reescalar; sin nombre ni logotipo de Spotify. Si
  no se puede cargar, se muestra el nombre de la app en `t_titulo` · `texto`.

### 5.11 Indicador de desplazamiento

```
 ┌──────────────────────────┐
 │ [⌂]  [⌕]  [▮▮▮]  [⚙]     │  barra de pestañas
 │┌───────────────────────┐▓│  ▓ = tirador `clorofila`
 ││┌──┐ Canción     3:42  │▓│  ░ = pista `capa`
 │└───────────────────────┘░│
 │ ...                     ░│  4 px, pegado al borde derecho
 │──────────────────────────│
```

- Franja de 4 px (`e1`) pegada al borde derecho de la pantalla inferior, entre la barra de
  pestañas y la barra de botones; queda a 4 px del borde derecho de las filas.
- Pista: relleno plano `capa`. Tirador: relleno plano `clorofila`. Sin contorno, sin relieve.
- Alto del tirador proporcional a la parte visible de la lista, con un mínimo de 16 px (`e4`);
  posición proporcional al desplazamiento.
- Solo cuando la lista no cabe entera en pantalla.
- Se actualiza en el mismo frame que el desplazamiento, también durante el arrastre (§6.9).
- No es un control: no responde al toque.

### 5.12 Barra de pestañas

```
 ┌────────┬────────┬────────┬────────┐  32 px
 │   ⌂    │▓▓▓⌕▓▓▓▓│  ▮▮▮   │   ⚙    │  ▓ = activa `clorofila`
 └────────┴────────┴────────┴────────┘
  Inicio   Búsqueda  Reprod.  Ajustes    (sin etiqueta: solo icono)
```

- Franja de 32 px (`TAB_H`, el mínimo táctil) arriba de la pantalla inferior, a todo el
  ancho. Sustituye a la cabecera de 24 px de las vistas.
- Cuatro pestañas de 80 px, en este orden: Inicio (casa), Búsqueda (lupa), Reproducción
  (ecualizador) y Ajustes (engranaje). Cada una lleva solo su icono de 16 px centrado, sin
  texto.
- Reposo: panel `capa` con contorno 1 px `tinta`, icono `texto`.
- Activa: relleno `clorofila`, banda inferior 2 px `musgo`, contorno 1 px `tinta`, icono
  `tinta`.
- Pulsada: relleno `musgo`, icono `tinta`. A diferencia del resto de controles (§6.3), no se
  desplaza: su borde inferior sube 4 px y la pestaña encoge solo hacia arriba. La franja que
  queda debajo se rellena con `fondo_sup`, para que las filas que pasan por debajo de la
  barra sigan ocultas.
- La pestaña activa, sin pulsar, tiene el borde inferior 2 px más arriba mientras está
  seleccionada.
- Táctil (§6.4): la pestaña se pulsa al tocarla y se activa al levantar el lápiz dentro.
- Botones (§6.5): L/R pasan a la pestaña anterior o siguiente, en cualquier pantalla; ZL/ZR
  (solo New 3DS) hacen lo mismo. Ninguna vista usa L/R para otra cosa.
  La pestaña nueva se activa en el mismo frame y muestra el estado pulsado.
- No da la vuelta: en la primera y la última pestaña, el botón hacia fuera no hace nada.
- Se dibuja después de la vista: las filas arrastradas pasan por debajo.

### 5.13 Código QR

- Módulos `tinta` sobre un cuadrado `texto` (excepción de §2.6 y §2.7), con contorno 1 px
  `tinta` y sombra dura (§4.3). Sin logotipo en el centro: no se sacrifica corrección de
  errores por decoración.
- Zona tranquila de 4 módulos por cada lado, dentro del cuadrado blanco.
- Escala entera: cada módulo mide un número exacto de píxeles, nunca se reescala.
  - Caja de 148×148 px (`LOGIN_QR_BOX`): 37 módulos × 4 px, es decir, la versión 3 del código
    (29 módulos) más la zona tranquila.
  - Escala mínima de 3 px por módulo (`LOGIN_QR_MIN_SCALE`). Una URL que no quepa a esa escala
    no se muestra como QR, solo como texto.
- Corrección de errores: la más alta que quepa en la versión más pequeña que admite la URL.
- Se codifica una vez por URL (`qrcodegen`, `source/deps/`) y se pinta por tramos: un
  `C2D_DrawRectSolid` por cada racha horizontal de módulos oscuros.
- Solo lleva la URL de verificación con el código de usuario, nunca el código de dispositivo,
  que es secreto.

### 5.14 Enlace

Texto que abre otra pantalla al tocarlo (título, artista y álbum en «Reproducción», §7.5). Un
enlace no es un botón:

- Zona táctil de 32 px de alto (§4.4) alrededor del texto, que se dibuja en su sitio de
  siempre y con su estilo de siempre.
- Reposo: solo el texto, sin panel, contorno ni relieve.
- Pulsado (§6.3): el texto pasa a `lima` con un subrayado `lima` de 2 px (`BEVEL_W`) bajo
  él. Sin panel, sin desplazamiento. `lima` es el único acento con contraste ≥ 4.5:1 sobre
  los dos tonos del fondo (4.78 y 5.96; `clorofila` da 3.74).
- Sin estado de foco: solo táctil. Se activa al levantar el lápiz dentro (§6.4).
- Deshabilitado (sin destino, por ejemplo una pista sin URI de álbum): no responde y no
  cambia de aspecto.

### 5.15 Texto que rueda

Para un dato que no cabe en su línea y que no se ve entero en ningún otro sitio: solo la
ficha de detalle de la pantalla superior (§7.12).

- Empieza quieto en su sitio durante 1,5 s (`MARQUEE_PAUSE_MS`).
- Después se desplaza a la izquierda a 30 px/s (`MARQUEE_SPEED`), seguido de una copia del
  mismo texto 48 px (`MARQUEE_GAP`, 2·`e6`) por detrás. Cuando la copia llega a donde
  empezaba el original, vuelve a esperar: el salto no se ve.
- Recortado a su columna con el scissor de la GPU: no pisa la carátula ni el margen.
- Cada línea rueda por su cuenta; las que caben no se mueven. Vuelve a empezar al cambiar
  de documento.
- Lógica pura en `marquee_logic.h`.

### 5.16 Tesela

La fila de lista (§5.3) en compacto, de una sola línea, para rejillas de 1, 2 o 3 columnas
(las playlists de «Inicio», §7.7).

```
 ┌─────────────────────────┐ 32 px (TILE_H = TOUCH_MIN)
 │ ┌──┐ Nombre de la play...│ t_cuerpo · texto
 │ └──┘                    │
 └─────────────────────────┘
   4 px  portada 24×24, texto a e2 (8 px) de ella
```

- Alto 32 px: el mínimo táctil (§4.4), así que cada tesela es un objetivo válido.
- Portada 24×24 (`TILE_COVER`) a 4 px de los bordes izquierdo, superior e inferior, con
  contorno 1 px `tinta`; sin imagen, cuadrado `capa_alta` con el icono de nota centrado,
  como la fila (corazón para «Canciones que te gustan», flecha para «Descargas»). El layout
  no cambia al llegar la imagen.
- Nombre en `t_cuerpo` · `texto`, una línea, recortado con `...` (§3.3), a `e2` de la
  portada y con `e2` de margen derecho. Sin nombre: «(sin titulo)».
- Estados idénticos a la fila (§5.3, §6.3): reposo `capa` con contorno; en foco `clorofila`
  con banda inferior `musgo` y texto `tinta`; pulsada, desplazada 2 px y relleno `musgo`.
  Fila y tesela comparten el dibujo (`ui_item_frame` en `draw.c`).
- Rejilla: 304 px de ancho útil (320 − 2·`e2`) y separación `GRID_GAP` = 4 px en ambos ejes.
  El resto de la división va a las primeras columnas:

  | Columnas | Anchos      | x de cada columna | Texto útil por tesela |
  |---------:|-------------|-------------------|-----------------------|
  | 1        | 304         | 8                 | 260 px                |
  | 2        | 150, 150    | 8, 162            | 106 px                |
  | 3        | 99, 99, 98  | 8, 111, 214       | 55 / 54 px            |

- Cinco filas de teselas caben exactas entre las barras (4 + 5·32 + 4·4 + 4 = 184 px), como
  las cuatro filas de resultados. Desplazamiento por filas enteras, arrastre (§6.9) e
  indicador (§5.11) como en las listas.
- Lógica pura en `grid_logic.h`; widget en `tile_grid.[ch]`.

---

## 6. Estados e interacción

### 6.1 Estados de un control

| Estado         | Aspecto                                                                 |
|----------------|-------------------------------------------------------------------------|
| Reposo         | Superficie en relieve completa (§5.1).                                  |
| Foco           | Reposo + borde de foco (§6.2).                                          |
| Pulsado        | Ver §6.3.                                                               |
| Deshabilitado  | Relleno `capa`, contorno `tinta`, sin relieve ni sombra, texto e icono `texto_off`. |

### 6.2 Borde de foco

- Rectángulo de 2 px `lima` por fuera del contorno, separado 1 px de él.
- Aparece en el mismo frame en que el control recibe el foco.

### 6.3 Estado pulsado (obligatorio en todo control)

Al detectar la pulsación, en el mismo frame:

- El control entero (contorno, relleno, bandas y contenido) se desplaza 2 px a la derecha y
  2 px abajo, ocupando el sitio de su sombra.
- La sombra dura no se dibuja.
- Las bandas se intercambian: oscura arriba, clara abajo.
- El relleno pasa a su color oscuro: `clorofila` → `musgo`; `capa_alta` → `capa`;
  `coral` → `coral_osc`.

Duración:

- Se mantiene mientras el botón o el lápiz sigan pulsados.
- Duración mínima visible: 6 frames, aunque se suelte antes.
- Al terminar, vuelve a reposo en un solo frame.

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
  `tinta`, letra en el color del botón, desplazado 1 px abajo.
- X abre el detalle de la canción en foco en las listas (resultados, álbum y artista, §7.4
  y §7.11). No tiene control en pantalla: solo se invierte su glifo en la barra de botones.
- Si la acción no tiene control en pantalla (SELECT idioma, START salir, cambio de volumen
  fuera del reproductor), mostrar una burbuja (§5.9) con el resultado: «English», «Volumen 60 %».
  En Ajustes, SELECT tiene control en pantalla: el botón de idioma muestra el estado pulsado.
- L/R y ZL/ZR cambian de pestaña: la pestaña nueva se activa y muestra el estado pulsado
  (§5.12) con la misma duración. No tienen glifo en la barra de botones.

### 6.6 Cruceta en listas

- El foco se mueve en el mismo frame de la pulsación.
- La nueva fila pasa al estado en foco (§5.3).
- Al intentar pasar del primer o último elemento: la fila en foco se desplaza 3 px en esa
  dirección durante 6 frames y vuelve. La cola no da la vuelta.
- El desplazamiento de la lista se aplica sin transición.
- Si un arrastre dejó filas cortadas (§6.9), la pulsación ajusta antes la lista a filas
  enteras: con botones siempre se ven 4 filas completas.
- En la lista de resultados, izquierda/derecha saltan 4 filas. L/R son de la barra de
  pestañas.
- Si además la fila en foco quedó cortada o fuera de la vista, esa primera pulsación (cruceta,
  A o X) solo devuelve la lista a ella: no mueve el foco, ni reproduce, ni abre el detalle.
  A y X muestran igualmente su glifo pulsado (§6.5).
- En una rejilla de teselas (§5.16), arriba/abajo cambian de fila y conservan la columna (si
  la fila de destino es más corta, al último de esa fila); izquierda/derecha cambian de
  columna dentro de la fila. Con 1 columna, izquierda/derecha saltan una página, como en
  resultados. En los bordes no hay vuelta: la tesela se desplaza 3 px hacia ese lado durante
  6 frames.

### 6.7 Volumen

- Arriba/abajo actualiza la barra de volumen en el mismo frame.
- En el reproductor, el glifo ▲▼ de la barra de botones se invierte (§6.5).

### 6.8 Reproducir / pausa

- El icono del botón central cambia en el mismo frame que el estado de audio.
- El botón central muestra el estado pulsado (§6.3) tanto con A como con toque.

### 6.9 Arrastre en listas

- Un toque sobre la lista pasa a ser un arrastre cuando el lápiz se desplaza 8 px (`e2`) en
  vertical desde donde tocó.
- Al empezar el arrastre, la fila tocada vuelve a reposo en ese frame y no ejecuta su acción
  (§6.4).
- Durante el arrastre la lista sigue al lápiz píxel a píxel, sin pasar del primer ni del
  último elemento. Las filas que entran o salen quedan cortadas por la barra de pestañas y la
  barra de botones; puede asomar una quinta fila.
- Solo empieza un arrastre el toque que cae dentro de la lista; el que cae en la barra de
  pestañas es de la barra.
- Al levantar el lápiz, la lista se queda donde está, aunque alguna fila quede cortada por
  la barra de pestañas o la barra de botones.
- El foco se queda en su fila aunque el arrastre la corte o la saque de la vista. La ficha
  de la pantalla superior sigue mostrando esa canción.
- Tocar una fila, cortada o no, la pone en foco sin mover la lista.
- En cuanto se usa la cruceta o A, la lista vuelve a filas enteras (4 a la vista) con la
  fila en foco visible, en el mismo frame y sin transición (§6.6).
- Sin inercia.

---

## 7. Pantallas

### 7.1 Fondo común

Ambas pantallas: degradado vertical `fondo_sup` → `fondo_inf` a pantalla completa.

### 7.2 Arranque / sesión

Conectando:

```
 SUPERIOR 400×240                          INFERIOR 320×240
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │                                │        │                          │
 │          [logotipo]            │        │  Autenticando con el     │ t_cuerpo · texto
 │          (cy 96)               │        │  access point...         │
 │                                │        │       Paso 3 de 5        │ t_meta · texto_2
 │            ▣ ▣ ▣               │        │                          │
 │            ▣   ▣  (cy 196)     │        │──────────────────────────│
 │            ▣ ▣ ▣               │        │ [SELECT] English         │
 └────────────────────────────────┘        └──────────────────────────┘
```

- Superior: logotipo centrado en y 96 y, debajo, el spinner de anillo (`ui_spinner_ring`)
  centrado en y 196, sin velo (§2.7). Es el mismo spinner que la descarga de una pista, para
  que «cargando» se vea igual en toda la aplicación.
- Inferior: la etapa actual en `t_cuerpo` · `texto` y «Paso n de N» en `t_meta` · `texto_2`,
  centrados (pantalla de un único mensaje, §3.3). Son 5 pasos con la credencial guardada y 6
  si hay que emparejar.
- Solo se redibuja cuando avanza el spinner (cada `LOADER_STEP` frames) o cuando cambia la
  etapa o el idioma.
- La sesión arranca en un hilo de trabajo: la pantalla sigue animándose y respondiendo a
  SELECT durante los handshakes TLS.

Error:

```
 SUPERIOR 400×240                          INFERIOR 320×240
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │                                │        │ ┌──────────────────────┐ │
 │          [logotipo]            │        │ │ ! Categoria          │ │ aviso coral (§5.8)
 │                                │        │ │ Detalle del error,   │ │
 │            ┌─────┐             │        │ │ ajustado.            │ │
 │            │  !  │  coral      │        │ └──────────────────────┘ │
 │            └─────┘             │        │ [ Reintentar ][Emparejar]│ 32 px
 └────────────────────────────────┘        │──────────────────────────│
                                           │ [A] Reintentar [Y] Empar.│
                                           └──────────────────────────┘
```

- Superior: el spinner se sustituye por un cuadrado en relieve `coral` del mismo tamaño
  (44 px) con el icono de error en `tinta`.
- Inferior: un aviso de error (§5.8) a todo el ancho útil, con la categoría como título y el
  detalle como cuerpo ajustado. El aviso crece con el texto hasta el espacio disponible.
- Debajo, dos botones de 32 px en una fila: **Reintentar** (primario) y **Emparejar**
  (secundario), separados por `e4`. La cruceta mueve el foco entre ellos y el táctil los activa
  al levantar el lápiz.
- Barra de botones: `[A]` pulsa el botón con el foco y su etiqueta dice cuál, `[Y]` empareja,
  `[START]` sale y `[SELECT]` cambia de idioma. Si la barra no cabe, se quitan etiquetas
  desde la última y quedan solo los glifos (en español pierden la etiqueta START y SELECT).
- Error fatal (sin red: `socInit`, `psInit` o TLS): sin botones; la barra solo tiene START y
  SELECT.

### 7.3 Emparejamiento

```
 SUPERIOR 400×240
 ┌──────────────────────────────────────┐
 │          Vincula tu cuenta           │ t_cabeza · texto, centrado
 │ ┌──────────────────────────────────┐ │ panel capa, x 12..388
 │ │            Tu codigo             │ │ t_meta · texto_2
 │ │              JDDHV3              │ │ t_titulo × 1.5 · texto
 │ │         spotify.com/pair         │ │ t_cabeza · lima
 │ │  Abrela en el movil o el         │ │ t_cuerpo · texto_2, centrado
 │ │  ordenador, inicia sesion,       │ │
 │ │  escribe el codigo y acepta.     │ │
 │ └──────────────────────────────────┘ │
 └──────────────────────────────────────┘

 INFERIOR 320×240
 ┌──────────────────────────────┐
 │ ┌────────────┐ Tambien puedes│ t_cuerpo · texto, ajustado a 144 px
 │ │ ▀▄█▀▄ ▄▀█▄ │ escanear el   │
 │ │ █▄▀ QR ▄▀█ │ codigo QR para│
 │ │ ▄▀█▄▀▄█▀▄▀ │ iniciar sesion│
 │ └────────────┘ ▮▮▮ Esperando │ indicador §5.7 + t_meta · texto_2
 │──────────────────────────────│
 │ [B] Cancelar [SELECT] English│
 └──────────────────────────────┘
```

- Superior: el código grande es la vía principal. La URL corta sale de `verification_uri`
  quitando el esquema y el `www.`, y va en `lima` dentro del panel `capa` (§2.6).
- Si la credencial guardada caducó, el título pasa a «Sesion caducada: vincula tu cuenta de
  nuevo».
- Inferior: el QR (§5.13) en x 8, y 34, centrado en la altura útil, y a la derecha una frase y
  el estado del sondeo con el indicador de carga. Si un sondeo no llega a Spotify, el estado
  pasa a «Sin conexion, reintentando...».
- B cancela el emparejamiento sin tocar la credencial guardada y lleva a la pantalla de error.

### 7.4 Resultados

```
 SUPERIOR                                  INFERIOR
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │ Resultados: "rock"   1-4 de 24 │        │ [⌂] [▓⌕▓] [▮▮▮] [⚙]      │ 32 px
 │ ┌────────────────────────────┐ │        │┌───────────────────────┐▓│
 │ │ Título de la fila en foco  │ │        ││┌──┐ Canción uno  3:42 │▓│
 │ │ Artista                    │ │        ││└──┘ Artista           │░│ 4 filas
 │ │ Álbum                      │ │        │└───────────────────────┘░│ × 41 px
 │ │ 3:42                       │ │        │ ...                     ░│
 │ └────────────────────────────┘ │        │──────────────────────────│
 └────────────────────────────────┘        │ [A] Reprod. [X] Detalle [Y]│ 24 px
                                           └──────────────────────────┘
```

- Inferior: barra de pestañas de 32 px (§5.12) con «Búsqueda» activa, 4 filas de 41 px con
  miniatura (§5.3) separadas 4 px, barra de botones. La primera fila queda a 4 px de la barra
  de pestañas y la última a 4 px de la barra de botones: 4 + 4 × 41 + 3 × 4 + 4 = 184 px,
  exactamente el espacio entre las barras.
- Superior: título `t_cabeza` · `texto` («Resultados: "consulta"») a la izquierda y rango
  visible («1-4 de 24») en `t_meta` · `texto_2` a la derecha, en la misma línea.
- Antes de la primera búsqueda: mensaje centrado «Todavía no has buscado nada» y botón
  primario «Buscar canciones».
- Desplazamiento continuo: la lista se mueve lo justo para mantener visible la fila en foco.
  Izquierda/derecha saltan 4 filas. Fijar `RESULTS_PER_PAGE` a 4.
- Táctil: tocar una fila la pone en foco; tocar la fila en foco la reproduce; arrastrar
  desplaza la lista (§6.9).
- Indicador de desplazamiento (§5.11) a la derecha, cuando hay más de 4 resultados.
- A, o tocar la fila en foco, pasa a la pestaña «Reproducción» con esa canción. No hay
  pantalla intermedia de canción seleccionada. Una canción suelta no trae lista: la cola es
  ella y una radio a partir de ella (§7.5).
- X abre el detalle de la canción en foco (§7.10): es un camino adicional, A sigue sin
  pantalla intermedia. Barra de botones: `[A] Reproducir [X] Detalle [Y] Buscar [B] Volver`.
- Lanzar una búsqueda nueva cierra los detalles abiertos en «Búsqueda».
- La búsqueda corre en segundo plano: se puede cambiar de pestaña mientras tanto. B la
  cancela desde su pestaña y vuelve a «Inicio».
- B vuelve a «Inicio».
- Superior: lo mismo que en cualquier pestaña (el logotipo, o la ficha de lo que suena); con un
  detalle abierto, su ficha (§7.12).
- El teclado (`swkbd`) se deja con el aspecto del sistema.

### 7.5 Reproductor

```
 SUPERIOR                                  INFERIOR
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │ ┌──────────┐                   │        │ [⌂] [⌕] [▓▮▮▮▓] [⚙]      │ 0..32
 │ │          │  Título canción   │        │ Título canción   3 de 24 │ 36..68   enlace
 │ │ carátula │  Artista          │        │ Artista   │ Álbum    [↓] │ 72..104  2 enlaces + icono
 │ │ 160×160  │  Álbum            │        │   [◀◀]   [ ▶ ]   [▶▶]    │ 108..156
 │ │          │                   │        │ 1:24               3:42  │ 163..177
 │ └──────────┘                   │        │ ███████▌░░░░░░░░░░░░░░░░ │ 180..188
 │                                │        │ Volumen █████░░░         │ 201..207
 └────────────────────────────────┘        │ [A] Pausa  [B] Volver    │ 216..240
                                           └──────────────────────────┘
```

- Carátula 160×160, a 12 px de los bordes superior e izquierdo, contorno 1 px `tinta`,
  sombra dura de 3 px.
- Mientras carga: cuadrado `capa_alta` del mismo tamaño con el icono de nota en `texto_2`.
  El resto del layout no se mueve al llegar la imagen.
- Metadatos a la derecha de la carátula: título `t_titulo` · `texto`, artista y álbum
  `t_cuerpo` · `texto_2`.
- Superior: esta ficha (carátula 160×160, título, artista, álbum, tiempos y barra de
  progreso) en las demás pestañas; en «Reproducción», solo la carátula grande de 216×216
  centrada, a `e3` arriba y abajo.
- Inferior, bajo la barra de pestañas: título `t_cabeza` · `texto` con la posición en la cola
  («3 de 24») en `t_meta` · `texto_2` a la derecha; debajo, artista y álbum en `t_cuerpo` ·
  `texto_2`, cada uno en su mitad (132 px, separadas 4 px) y recortado a ella, y a su
  derecha, a 4 px, el icono de descarga en un cuadrado de 32 px; botón central
  primario 48×48 y anterior y siguiente secundarios 40×40, centrados a 16 px; tiempo
  transcurrido y duración en `t_meta` · `texto_2`; barras de progreso y volumen debajo. La
  tabla de píxeles está en `player_logic.h`.
- Título, artista y álbum son enlaces (§5.14) de 32 px: abren el detalle de la canción
  (§7.10, con la cola como lista), del artista o del álbum (§7.11) en la pila de
  «Reproducción». Solo mientras la pista carga, suena o ha terminado; sin URI de artista o de
  álbum, ese enlace no responde. «3 de 24» no forma parte del enlace.
- El icono de descarga dice dónde está la pista: flecha (no descargada, o falló), reloj (en
  cola), su porcentaje en `t_meta` (descargándose, tomado con el reloj: una vez por segundo)
  o check en `lima` (en la SD). Es estático: nada se anima en el reproductor (§9). Tocarlo
  con flecha, o pulsar X, la descarga; una burbuja (§5.9) dice lo que pasó. La barra añade
  `[X] Descargar` mientras la flecha se ve. Borrar una descarga no se hace aquí: se hace en
  el detalle de canción (§7.10).
- Los iconos de los controles son de 16 px mientras no exista el juego de 24 px (§5.10).
- Anterior y siguiente se deshabilitan (§6.1) en los extremos de la cola.
- La cola depende de dónde se eligió la canción:
  - Canción suelta (un resultado de búsqueda, o su detalle): ella y 25 recomendaciones de
    canciones parecidas, que llegan en segundo plano cuando la canción ya suena. Cuando
    quedan 2 por sonar se piden 25 más a partir de la última, sin repetir ninguna: la radio
    no se acaba. «1 de 1» crece al llegar cada tanda.
  - Álbum, artista o playlist: la lista, desde la elegida, en su orden o en aleatorio
    (§7.11). De una playlist se cargan en segundo plano las páginas que su detalle aún no
    había cargado, hasta 500 pistas, aunque se cierre el detalle. Al acabar la lista, se
    para.
  - Nada se pide a la red mientras una pista se prepara (§9).
- Mientras carga (carátula, clave de audio, conexión): indicador de carga (§5.7) y el paso
  actual en `t_cuerpo` · `texto` en lugar de los controles; aún no suena nada.
- Al terminar una pista empieza sola la siguiente de la cola. Solo al final de la cola queda
  «Pista terminada»: el botón central muestra «reproducir» y vuelve a empezarla; la barra de
  progreso queda llena y «Pista terminada» sustituye a los tiempos. Una pista con error no
  salta a la siguiente.
- Error: aviso de error (§5.8) con el detalle; A reintenta.
- Sin nada en reproducción: mensaje centrado «Todavía no suena nada».
- El audio sigue sonando en cualquier pestaña. B vuelve a «Búsqueda» sin cortarlo; con un
  detalle abierto, B lo cierra primero.
- Saltar con la barra de progreso (§5.5.1): el audio ya en cola se descarta y la pista sigue
  desde el punto elegido, pausada si lo estaba. La posición en bytes es una estimación
  (Vorbis tiene tasa variable): el tiempo se corrige con la posición real en cuanto el
  decodificador la encuentra, y si cae a más de 2 s del punto pedido se corrige una vez.
- Solo se salta con la pista entera descargada: saltar a una parte aún sin descargar dejaba
  la pista en silencio en la consola. La descarga por bloques (`stream_chunks.h`) sigue
  preparada para ello.
- El teclado (`swkbd`) bloquea el hilo principal: la pista se pausa mientras está abierto y
  sigue al cerrarlo.

### 7.6 Pista terminada / error

- Superior: última carátula o, si no la hay, el logotipo.
- Inferior: dentro de la pestaña «Reproducción» (§7.5), con la barra de pestañas.

### 7.7 Inicio

```
 INFERIOR (rejilla de 6)                   INFERIOR (Mostrar más)
 ┌──────────────────────────┐              ┌──────────────────────────┐
 │ [▓⌂▓] [⌕] [▮▮▮] [⚙]      │ 0..32        │ [▓⌂▓] [⌕] [▮▮▮] [⚙]      │ 0..32
 │ Recientes                │ 36..52       │┌──────────┐┌──────────┐▓│ 36..68
 │┌──────────┐┌──────────┐  │ 56..88       ││▢ Lista 1 ││▢ Lista 2 │▓│
 ││▢ Lista 1 ││▢ Lista 2 │  │              │└──────────┘└──────────┘░│ 5 filas
 │└──────────┘└──────────┘  │              │ ...                     ░│ de 32 px
 │ (3 filas, 2 columnas)    │ ..160        │                         ░│ ..212
 │ [     Mostrar mas      ] │ 176..208     │──────────────────────────│
 │──────────────────────────│              │ [A] Abrir [Y] Buscar [B] │
 │ [A] Abrir [Y] Buscar     │ 216..240     └──────────────────────────┘
 └──────────────────────────┘
```

- Las playlists vienen del historial de reproducción de la cuenta (las más recientes
  primero), seguidas del resto de la carpeta de playlists en su orden, sin repetir; hasta
  24. Sin historial, solo la carpeta, y la etiqueta dice «Tus playlists» en lugar de
  «Recientes». El DJ de Spotify no aparece: es una sesión en vivo de otro servicio
  (atributos `lexicon_*`), sin lista de pistas que este cliente pueda leer ni reproducir.
- Rejilla de 6: etiqueta en `t_meta` · `texto_2` en 36..52; 3 filas × 2 columnas de teselas
  (§5.16) en 56..160; «Mostrar más» como botón secundario (§5.2) a todo el ancho, a `e4`
  debajo (176..208), solo si hay más de 6.
- «Mostrar más» es la misma pestaña en otro modo, no un nivel de detalle: 5 filas de
  teselas entre las barras, con indicador de desplazamiento. B vuelve a la rejilla de 6 con
  el foco en «Mostrar más».
- La segunda tesela es siempre «Descargas» (sin conexión, la única: §7.15), con el icono
  de descarga donde iría la portada: no viene del feed, así que la primera fila es `[Canciones que te gustan]
  [Descargas]`. Su vista previa dice cuántas canciones y colecciones hay en la SD.
- La primera tesela es siempre «Canciones que te gustan», con el icono del corazón
  (§5.10) donde iría la portada. Se abre como una playlist más, por páginas (§7.11); la
  lista entera llega en una petición (`context-resolve`, de la más reciente a la más
  antigua) y los nombres se piden de 50 en 50.
- La cruceta pasa de la última fila de teselas al botón «Mostrar más» y vuelve.
- Toda playlist se abre con el foco en su primera canción. «Lo ultimo» de la vista previa
  es solo informativo: esa canción a menudo ni siquiera está en la playlist (la
  reproducción automática sigue después del final).
- A, o tocar la tesela en foco, abre el detalle de la playlist en la pila de «Inicio»
  (§7.9). Tocar una tesela fuera de foco la pone en foco.
- Y abre el teclado de búsqueda desde cualquier modo.
- Barra de botones: `[A] Abrir [Y] Buscar`; en «Mostrar más», además `[B] Volver`.
- Cargando: 6 teselas esqueleto y burbuja de espera «Cargando...» (§5.9). Error: aviso
  (§5.8) con el detalle y botón primario «Reintentar». Vacío: mensaje centrado «Todavia no
  tienes playlists» y botón primario «Buscar canciones».
- La burbuja de idioma (SELECT) tapa «Mostrar más» durante 60 frames: es transitoria y no
  es un control (§5.9).
- Superior: con una tesela en foco, su ficha (§7.12) como vista previa: portada, nombre,
  «N canciones» y «Lo ultimo: <canción - artista>» si la hay. La portada se pide cuando
  el foco lleva 18 frames (300 ms) quieto, para no descargar al recorrer la rejilla. Con
  el foco en «Mostrar más» o mientras carga, logotipo o la ficha de lo que suena; con un
  detalle abierto, su ficha.
  Volver a conectar y emparejar están en «Ajustes».

### 7.8 Ajustes

```
 INFERIOR
 ┌──────────────────────────┐
 │ [⌂] [⌕] [▮▮▮] [▓⚙▓]      │ 32 px
 │                          │
 │ [ (o) Idioma: Español  ] │ 44..76
 │ [ (↓) Descargas (3)    ] │ 88..120
 │ [  Volver a conectar   ] │ 132..164
 │ [Emparejar otra cuenta ] │ 176..208
 │──────────────────────────│
 │ [A] Elegir  [B] Volver   │
 └──────────────────────────┘
```

- Botones secundarios a todo el ancho útil, 32 px de alto y `e3` (12 px) de separación, el
  primero a `e3` de la barra de pestañas: caben cuatro con su anillo de foco y su sombra
  separados ≥ 4 px (§4.4) y sin tocar la barra de botones. Foco con la cruceta (§6.2) y
  toque (§6.4).
- Idioma: el botón muestra el idioma actual y lo cambia al pulsarlo, sin burbuja.
- Descargas: abre el gestor (§7.13). Mientras algo espera (canciones en cola o
  descargándose, y álbumes o playlists por preparar) lo cuenta: «Descargas (3 en cola)».
- Volver a conectar y emparejar detienen la reproducción y vuelven a la pantalla de sesión.
- Superior: el logotipo en su sitio, oscurecido junto con el fondo por el velo de §2.7, y
  encima, con márgenes de 16 px (`about_layout.h`):

```
 SUPERIOR
 ┌──────────────────────────────────────┐
 │ ┌──────────────────────────────────┐ │ 16..80
 │ │ [foto] Nombre de la cuenta       │ │ panel `capa`, foto 48×48
 │ │        Premium - ES              │ │
 │ │        Usuario: nombre_usuario   │ │
 │ └──────────────────────────────────┘ │
 │  Desarrollo   David Abejón           │ 96..176, filas de 20 px
 │  Basado en    SpotiBacan (ohquebacan)│
 │  Códigos QR   QR Code generator, ... │
 │  Licencia     Apache License 2.0     │
 │ ──────────────────────────────────── │ 180, regla `capa_luz`
 │ Proyecto no oficial, sin relación    │ 188.., aviso de marca
 │ con Spotify AB...                    │
 └──────────────────────────────────────┘
```

  - Cuenta: panel `capa` (§5.1) con la foto de perfil a 48×48 (el icono de persona mientras
    no llega) y, centradas en su altura, hasta tres líneas: el nombre visible (`t_cabeza` ·
    `texto`; sin él, el nombre de usuario), el plan y el país (`t_cuerpo` · `texto_2`) y
    «Usuario: …» (`t_meta` · `texto_2`) cuando el nombre visible es otro. El nombre y la foto
    vienen del perfil público de spclient (`user-profile-view`); el plan y el país, de
    `/v1/me`, que suele rechazar por límite de peticiones. Se piden una vez por sesión y se
    guardan en la configuración para mostrarse al instante la próxima vez.
  - Créditos: etiqueta en `t_meta` · `clorofila` y valor en `t_cuerpo` · `texto`, recortado
    con «...». El autor y lo que nombra `NOTICE`; los nombres propios no se traducen.
  - Aviso de marca de `NOTICE` en `t_meta` · `texto_2`, en varias líneas.
  - Se muestra en Ajustes siempre, suene música o no; el spinner de descarga va encima.
- B vuelve a «Inicio».

### 7.9 Detalles: navegación común

- «Inicio», «Búsqueda» y «Reproducción» tienen cada una su pila de detalles. Mientras la de
  la pestaña activa no está vacía, la pantalla inferior la dibuja el detalle de arriba en
  lugar de la vista de la pestaña, con la barra de pestañas encima y la misma pestaña
  activa; la superior muestra su ficha (§7.12).
- B cierra un nivel; con la pila vacía, B hace lo de siempre en esa pestaña (en «Inicio»,
  salir de «Mostrar más» o nada).
- L/R cambian de pestaña sin tocar las pilas: al volver, cada una está donde se dejó, con el
  foco y el desplazamiento de su lista.
- Seis niveles como máximo por pestaña; al abrir uno más se descarta el más antiguo.
- Reproducir desde un detalle pasa a «Reproducción» y cierra los detalles de esa pestaña para
  que muestre el reproductor. Si la canción ya está sonando, solo cambia de pestaña.

### 7.10 Detalle de canción

```
 INFERIOR
 ┌──────────────────────────┐
 │ [⌂] [▓⌕▓] [▮▮▮] [⚙]      │ 32 px
 │                          │
 │ [▶  Reproducir         ] │ 44..76   primario
 │ [↓  Descargar          ] │ 88..120  secundario
 │ [   Artista: Nombre    ] │ 132..164 secundario
 │ [   Album: Nombre      ] │ 176..208 secundario
 │──────────────────────────│
 │ [A] Elegir  [B] Volver   │
 └──────────────────────────┘
```

- Misma geometría que Ajustes (§7.8). El foco empieza en «Reproducir».
- El botón de descarga dice dónde está la pista, con su icono (§5.10):

  | Estado | Texto | A |
  |--------|-------|---|
  | no descargada | «Descargar» | la pone en cola |
  | en cola | «En cola» | deshabilitado (§6.1) |
  | descargando | «Descargando 42%» (una vez por segundo) | deshabilitado |
  | falló | «Reintentar descarga» | la vuelve a poner en cola |
  | en la SD | «Borrar descarga» | burbuja «Pulsa A otra vez para borrar»; otra A en 2 s la borra |
  | en la SD por un álbum o playlist | «Descargado» | deshabilitado: se quita con ellos |

  Una burbuja (§5.9) dice lo que pasó («Añadida a descargas», o por qué no). Si el botón se
  deshabilita con el foco encima (al ponerla en cola), el foco vuelve a «Reproducir».
- «Reproducir» reproduce la canción con la lista de la que salió como cola. Si salió de
  una canción suelta (un resultado de búsqueda, o la radio que suena), empieza una radio a
  partir de ella (§7.5).
- «Artista» y «Álbum» abren su detalle en la misma pestaña; se deshabilitan (§6.1) si la
  pista no trae ese URI. El nombre se recorta con `...`: completo en la ficha de arriba.
- No necesita red: los datos ya están en memoria.

### 7.11 Detalle de álbum, de artista y de playlist

- Lista como resultados (§7.4), con su arrastre (§6.9) e indicador (§5.11). Álbum:
  filas con número (§5.3), en orden de disco; de más de 24 pistas se muestran las 24
  primeras. Artista: sus canciones populares, con miniatura. Playlist: sus pistas con
  miniatura (cada una es de un álbum distinto), por páginas de 50: la siguiente se pide sola
  cuando la lista muestra una fila a 8 o menos de la última cargada, con la burbuja de
  espera «Cargando...» (§5.9) mientras llega; la lista sigue respondiendo. Hasta 500 pistas.
  Los episodios y archivos locales se omiten.
- Artista: 4 filas, como resultados. Álbum y playlist: un botón secundario a todo el ancho
  a `e1` de la barra de pestañas (36..68) que los descarga enteros, y debajo 3 filas
  (76..207); las filas arrastradas pasan por debajo del botón, como por debajo de las barras:

```
 INFERIOR
 ┌──────────────────────────┐
 │ [⌂] [▓⌕▓] [▮▮▮] [⚙]      │ 0..32
 │ [↓  Descargar álbum    ] │ 36..68
 │ ┌──┐ Canción             │ 76..117
 │ ┌──┐ Canción             │ 121..162
 │ ┌──┐ Canción             │ 166..207
 │──────────────────────────│
 │ [A] Reproducir ... [B]   │ 216..240
 └──────────────────────────┘
```

  El botón dice «Descargar álbum» o «Descargar playlist» (también «Canciones que te
  gustan»), «Preparando la descarga...» mientras se resuelve en pistas, «Descargando 8 de
  12» y «Descargado»; solo el primero responde (§6.1). El foco empieza en la primera fila;
  arriba desde ella llega al botón (si responde) y abajo vuelve a la lista, donde estaba.
  Con el foco en el botón ninguna fila lo muestra, A descarga y la barra es
  `[A] Elegir [Y] ... [B] Volver`. Tocar una fila devuelve el foco a la lista sin reproducir.
- A, o tocar la fila en foco, reproduce desde ella con la lista como cola; X abre el detalle
  de canción. Barra: `[A] Reproducir [X] Detalle [Y] Aleatorio [B] Volver`.
- Y alterna el aleatorio. Es un ajuste de la app, el mismo para todas las listas, y se
  recuerda entre sesiones. La etiqueta de Y dice cómo está ahora («Aleatorio» o «En orden»)
  y al cambiarlo sale una burbuja (§5.9) con el icono de aleatorio y «Aleatorio activado» o
  «Aleatorio desactivado».
- Con aleatorio, la pista elegida suena primero y el resto de la lista le sigue en orden
  aleatorio; sin él, la lista suena en su orden desde la elegida. Si lo que suena es una
  lista, cambiarlo la reordena al momento sin cortar la pista.
- Cargando: filas esqueleto, donde irán (3 bajo el sitio del botón en álbum y playlist), y
  burbuja de espera «Cargando...» (§5.9). B cancela y cierra.
- Error: aviso (§5.8) con el detalle y botón primario «Reintentar»; A reintenta.
- Sin pistas: mensaje centrado «No hay canciones».

### 7.12 Ficha de detalle (pantalla superior)

- Carátula, portada o retrato a 160×160 con contorno y sombra dura de 3 px, como la ficha de
  §7.5, pero sin tiempos ni barra: la carátula va centrada en la altura (y 40) y el bloque de
  textos, centrado junto a ella. Sin imagen, o mientras llega, el icono de nota (la flecha
  para «Descargas»).
- Título en `t_titulo` · `texto` y hasta 4 líneas en `t_cuerpo` · `texto_2`:
  - Canción: artista / álbum / «3:42 - pista 2 - disco 2» (disco solo si hay más de uno) /
    año.
  - Álbum: artista / «Album - 2004» / «11 canciones» o «24 de 31 canciones» / sello.
  - Artista: «Canciones populares».
  - Playlist: «Playlist» / «11 canciones» o «100 de 312 canciones» (crece con cada
    página). Sin propietario: el
    servicio solo da un id de usuario opaco.
  - Cargando o con error: su estado como título.
- El texto que no cabe en la columna rueda (§5.15).
- La imagen es la del detalle, nunca la de la pista que suena.

---

### 7.13 Gestor de descargas

Ajustes en otro modo, como «Mostrar más» en Inicio: no es un nivel de detalle. B vuelve a los
botones con el foco en «Descargas».

```
 INFERIOR
 ┌──────────────────────────┐
 │ [⌂] [⌕] [▮▮▮] [▓⚙▓]      │ 0..32
 │ EN CURSO                 │ cabecera, 24 px, t_meta · texto_2
 │ [▢ Canción         42% ] │ fila §5.3; el artista comparte línea
 │ [  Artista  ▬▬▬▬░░░░░  ] │   con una barra fina de 2 px
 │ EN COLA (3)              │
 │ [▢ Álbum             ◷ ] │ «Descargando 8 de 12»
 │ CON ERROR (1)            │
 │ [▢ Canción           ! ] │ el motivo en la segunda línea
 │ DESCARGADAS (48)         │ lo más reciente primero
 │ [▢ Canción           ✓ ] │
 │──────────────────────────│
 │ [A] Cancelar [X] Borrar  │ y [Y] Pausar [B] Volver
 └──────────────────────────┘
```

- Una sola lista que se desplaza por píxeles, con arrastre (§6.9) e indicador (§5.11). Las
  cabeceras no toman el foco; la primera fila de una sección trae su cabecera a la vista.
  Una sección vacía no sale.
- Secciones:
  - En curso: la canción que se descarga, con su porcentaje (leído una vez por segundo) y
    una barra fina como la del volumen (§5.5): 6 px, contorno `tinta`, `capa_alta` y
    relleno `clorofila`, que se ve igual en la fila en foco.
  - En cola: los álbumes y playlists que aún se están preparando («Preparando la
    descarga...»), los que tienen canciones esperando («Descargando 8 de 12»), y después
    las canciones sueltas, en el orden de la cola. Las de un álbum o playlist en cola no
    salen sueltas: las representa su fila.
  - Con error: todas las que fallaron, con el motivo.
  - Descargadas: álbumes y playlists sin nada en cola («Canciones: 12»; también los que se
    quedaron cortos por canciones con error), y las canciones en la SD que ninguno de ellos
    contiene, de la más reciente a la más antigua.
- Miniatura: la portada de la SD o de la red (§5.3); sin ella, la nota (el corazón para
  Canciones que te gustan). A la derecha, el estado: reloj, error o
  check, en el color secundario de la fila.
- A: en curso o en cola, cancelar; con error, reintentar; descargada, abrir su detalle en
  «Inicio» (cambia a esa pestaña). X: borrar cualquier fila (en cola, cancelarla). Cancelar
  y borrar piden una segunda pulsación del mismo botón sobre la misma fila mientras la
  burbuja «Pulsa A otra vez para cancelar» / «Pulsa X otra vez para borrar» se ve (2 s).
  Y: pausar o reanudar toda la cola; la etiqueta dice lo que hará y una burbuja confirma.
- Táctil: tocar una fila la enfoca; tocar la enfocada hace lo de A.
- Vacío: mensaje centrado «Todavía no hay descargas».
- Superior: panel `capa` a `e4` de los bordes con «Descargas» en `t_titulo` y, en
  `t_cuerpo` · `texto`, «Canciones: 48 · Álbumes: 3 · Playlists: 2», «Ocupado: 312 MB»
  y «Libre en la SD: 5,1 GB»; debajo, en `t_cabeza` · `texto_2`, el estado de la cola:
  «Descargando», «Nada pendiente», «En pausa», «La tarjeta SD está llena» o «Sin
  conexión». El espacio libre se lee al abrir y cada 10 s, no en cada frame.
- Solo pide frame al pulsar, al cambiar las descargas y con el porcentaje (§9).

### 7.14 Descargas en Inicio

- La tesela «Descargas» (§7.7) abre un nivel de detalle en la pila de «Inicio» que sale del
  índice de descargas, sin red: lista como la de §7.11 con, primero, los álbumes y
  playlists descargados (completos o en curso, lo más reciente primero) con la variante de
  colección (§5.3), y después las canciones descargadas sueltas, lo más reciente primero.
- A en una canción la reproduce con las canciones sueltas como cola (respeta el aleatorio);
  A en una colección la abre. X en una canción, su detalle (con «Borrar descarga»); en una
  colección no hace nada. Barra: `[A] Reproducir` o `[A] Abrir` según la fila,
  `[X] Detalle`, `[Y] Aleatorio`, `[B] Volver`.
- Colección descargada: como el detalle de álbum o playlist (§7.11, filas con número para
  un álbum y con miniatura para una playlist), con todas sus pistas; las que aún no están en
  la SD van con la variante sin descargar (§5.3) y la cruceta las salta. Reproducir pone en
  la cola solo las que están en la SD. Encima, el botón fijo de §7.11 dice «Borrar descarga»
  y pide una segunda A («Pulsa A otra vez para borrar»); al borrar se vuelve al nivel
  anterior. «Descargado» en el detalle de un álbum o playlist abre esta vista.
- Las dos vistas se actualizan solas cuando cambian las descargas.
- Sin nada descargado: «Todavía no hay nada descargado».
- Superior (§7.12): «Descargas» con «Canciones: N · Álbumes y playlists: M»; una colección,
  su portada (desde la SD), «Álbum - artista» y «Canciones: 12 · 48 MB».

### 7.15 Sin conexión

- Con una credencial guardada, la app entra en las pestañas aunque no llegue a Spotify, y
  funciona con lo descargado:
  - sin Wi-Fi al arrancar, al instante (sin esperar a ningún timeout);
  - con Wi-Fi pero sin respuesta, cuando el arranque falla por la red (no encontrar el
    punto de acceso o el servidor, o no recibir respuesta al renovar la credencial o al
    abrir la biblioteca). Si Spotify contesta que no (credencial revocada, acceso
    rechazado), se va a la pantalla de error o a emparejar, como siempre.
  Sin credencial guardada, se empareja como siempre.
- Indicador fijo: el icono de Wi-Fi tachado junto a la lupa de la pestaña «Búsqueda», en
  el color del icono de la pestaña. No se anima (§9).
- Inicio: solo la tesela «Descargas», bajo la etiqueta «Sin conexión: tus descargas», y en
  el sitio de «Mostrar más» el botón «Reintentar conexión».
- Búsqueda: el aviso de error con «Para buscar hace falta conexión...» y «Reintentar», que
  reintenta la conexión.
- Detalle de canción: «Artista», «Álbum» y «Descargar» deshabilitados (§6.1); «Borrar
  descarga» sigue. Reproductor: los enlaces de artista y álbum no responden y X no
  descarga; el icono de estado sigue.
- Reproducción: solo lo descargado. Al pasar a una pista que no está en la SD, salta a la
  siguiente descargada en la dirección en que iba la cola; si no queda ninguna, el
  reproductor muestra «Sin conexión, y nada de esta cola está descargado.».
- «Reintentar conexión» (Inicio, Búsqueda) y «Volver a conectar» (Ajustes) conectan sin
  salir de las pestañas, y lo que suena sigue sonando:
  - sin Wi-Fi, al instante, la burbuja «Sigue sin conexión» (icono de Wi-Fi tachado);
  - con Wi-Fi, «Conectando...» y el arranque en segundo plano. Si llega, «Conectado de
    nuevo»: el indicador se va, Inicio carga sus playlists y todo vuelve a funcionar, sin
    cortar la canción. Si falla por la red, «Sigue sin conexión». Si Spotify dice que no a
    la cuenta, la sesión arranca de nuevo como al abrir la app (pantalla de error o
    emparejar), y ahí sí se para la música.
  Las burbujas salen en la vista que esté en pantalla (Inicio, Reproducción o un detalle).
- Si el Wi-Fi se va con la app ya conectada (se mira una vez por segundo), las pestañas
  pasan a verse y comportarse como tras un arranque sin conexión, con la burbuja «Se perdió
  la conexión» y sin parar lo que suena desde la SD. La descarga en curso vuelve a la cola.
- Las descargas en cola esperan: el motor no descarga sin conexión.

## 8. Movimiento

- Duraciones en frames a 60 Hz. Máximo 8 frames.
- Cambios de estado en un solo frame, sin interpolación ni fundidos.
- Ninguna animación en bucle fuera de los indicadores de carga y del texto que rueda de la
  ficha de detalle (§5.15).

---

## 9. Rendimiento

- Cada pantalla tiene un flag `dirty`. Llamar a `C3D_FrameBegin` / `C3D_FrameEnd` solo cuando
  `dirty` sea verdadero.
- `C2D_TextParse` + `C2D_TextOptimize` solo al cambiar el contenido, nunca en el bucle de
  dibujo.
- Carátula: textura `C3D_Tex` 256×256 RGBA8 con subtextura al tamaño real; conversión a
  formato tiled en el hilo de descarga.
- En el reproductor solo se marca `dirty` por:
  - progreso, descarga y reloj (≤ 1 vez por segundo),
  - feedback de pulsación (§6) mientras dura,
  - cambio de volumen o pausa,
  - llegada de la carátula,
  - aparición y retirada de la burbuja.
- No dibujar indicadores de carga, marquesinas ni animaciones en bucle en el reproductor.
- Las vistas de detalle solo marcan `dirty` por pulsación, llegada de datos, imagen o
  miniaturas, y cambio de idioma. La ficha de arriba, además, cada vez que un texto que rueda
  avanza un píxel (§5.15); si todo cabe, queda quieta.
- Los detalles de álbum, artista y playlist se cargan en un hilo por debajo del
  decodificador, con una sola sesión keep-alive para la entidad y el lote de pistas. Abrir
  otro detalle cancela la carga en curso sin esperarla. De una playlist se piden páginas
  de 50 (`from`/`length`), cada una con su lote de nombres; las pistas van en un buffer
  propio del documento, hecho al abrirlo y liberado al reutilizarlo. La lista solo prepara
  el texto y las miniaturas de una ventana de 24 filas alrededor de lo visible; al moverse
  la ventana, las miniaturas que ya estaban se recolocan sin volver a descargarse.
- Las playlists de «Inicio» se cargan una vez al entrar en las pestañas, en otro hilo por
  debajo del decodificador y con una sesión keep-alive (historial, carpeta y la cabecera de
  las recientes que no están en ella). La rejilla solo marca `dirty` por pulsación, llegada
  de datos o miniaturas y cambio de idioma; los nombres se preparan al cambiar el contenido
  o el idioma.
- Con el audio sonando en otra pestaña, el reproductor no marca `dirty`: solo cuentan la
  pestaña visible y la barra de pestañas. Cambiar de pestaña redibuja las dos pantallas una
  vez.
- El audio se alimenta en cada vuelta del bucle principal, antes de atender a la vista, sea
  cual sea la pestaña.
- La búsqueda y cualquier otro hilo que pueda coincidir con la reproducción van por debajo
  de la prioridad del decodificador.
- Miniaturas de resultados:
  - Un único hilo de descarga con una sesión keep-alive a `i.scdn.co`; primero las filas
    visibles, luego las 4 siguientes.
  - Pool fijo de texturas 64×64 RGBA8, sin reservas por miniatura. Decodificación y conversión
    a formato tiled en el hilo; subida de la textura en el hilo principal.
  - Marcar `dirty` al llegar cada miniatura visible.
  - Pausarlas mientras se prepara la pista (token, access point, clave de audio): se pide
    parar al hilo sin esperarlo, y la preparación empieza cuando termina su petición en
    curso. Durante la descarga del audio siguen: son unos KB frente a megas de audio. Lo ya
    descargado se conserva.
  - No pararlo al salir de la pestaña «Búsqueda»: esperar a su petición en curso bloquearía
    el hilo principal, y con él el audio. Solo pide las filas visibles y las 4 siguientes.

---

## 10. Implementación

### 10.1 `source/ui/theme.h`

```c
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
	ART_BIG = 216,       /* carátula grande de la pantalla superior: 240 - 2·e3 */
	BTN_PLAY = 48, BTN_SKIP = 40,
	GLYPH_H = 14,
	PROGRESS_H = 8, VOLUME_H = 6,
	/* §5.16 tesela: la fila de resultados en compacto, de una línea. */
	TILE_H = TOUCH_MIN,
	TILE_COVER = TILE_H - 2 * SP_1,
	GRID_GAP = SP_1,
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

#endif
```