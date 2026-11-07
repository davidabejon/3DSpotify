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
  (§7.2) el spinner va sin velo, porque no hay nada debajo que tapar.
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
- Una línea por dato. Recortar con `...` midiendo con `C2D_TextGetDimensions`.
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

### 5.4 Barra de progreso

- Rectángulo de 8 px de alto con contorno 1 px `tinta`; pista `capa_alta`.
- Descarga: relleno `musgo` por delante del progreso.
- Progreso: relleno `clorofila`.
- Tirador: rectángulo 6×14 px `texto` con contorno 1 px `tinta`, centrado en el extremo del
  progreso.
- Actualizar como máximo una vez por segundo.

### 5.5 Barra de volumen

Igual que la barra de progreso, 6 px de alto, sin capa de descarga y sin tirador.

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
  aviso, idioma, nota musical, inicio (casa), reproducción (ecualizador) y ajustes
  (engranaje).
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
- Si además la fila en foco quedó cortada o fuera de la vista, esa primera pulsación (cruceta
  o A) solo devuelve la lista a ella: no mueve el foco ni reproduce. A muestra igualmente
  su glifo pulsado (§6.5).

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
 └────────────────────────────────┘        │ [A] Reprod. [Y] Buscar [B]│ 24 px
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
- A, o tocar la fila en foco, pasa a la pestaña «Reproducción» con esa canción y el resto de
  resultados como cola. No hay pantalla intermedia de canción seleccionada. Mientras suena esa
  cola, la fila en foco sigue a la pista que suena.
- La búsqueda corre en segundo plano: se puede cambiar de pestaña mientras tanto. B la
  cancela desde su pestaña y vuelve a «Inicio».
- B vuelve a «Inicio».
- Superior: detalle de la fila en foco en panel `capa`, una línea por dato (título, artista,
  álbum, duración).
- El teclado (`swkbd`) se deja con el aspecto del sistema.

### 7.5 Reproductor

```
 SUPERIOR                                  INFERIOR
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │ ┌──────────┐                   │        │ [⌂] [⌕] [▓▮▮▮▓] [⚙]      │ 32 px
 │ │          │  Título canción   │        │ Título canción   3 de 24 │
 │ │ carátula │  Artista          │        │ Artista                  │
 │ │ 160×160  │  Álbum            │        │   [◀◀]   [ ▶ ]   [▶▶]    │
 │ │          │                   │        │ 1:24               3:42  │
 │ └──────────┘                   │        │ ███████▌░░░░░░░░░░░░░░░░ │
 │                                │        │ Volumen █████░░░         │
 └────────────────────────────────┘        │ [A] Pausa  [B] Volver    │
                                           └──────────────────────────┘
```

- Carátula 160×160, a 12 px de los bordes superior e izquierdo, contorno 1 px `tinta`,
  sombra dura de 3 px.
- Mientras carga: cuadrado `capa_alta` del mismo tamaño con el icono de nota en `texto_2`.
  El resto del layout no se mueve al llegar la imagen.
- Metadatos a la derecha de la carátula: título `t_titulo` · `texto`, artista y álbum
  `t_cuerpo` · `texto_2`.
- Inferior, bajo la barra de pestañas: título `t_cabeza` · `texto` con la posición en la cola
  («3 de 24») en `t_meta` · `texto_2` a la derecha, artista `t_cuerpo` · `texto_2`; botón
  central primario 48×48 y anterior y siguiente secundarios 40×40, centrados a 16 px; tiempo
  transcurrido y duración en `t_meta` · `texto_2`; barras de progreso y volumen debajo.
- Los iconos de los controles son de 16 px mientras no exista el juego de 24 px (§5.10).
- Anterior y siguiente se deshabilitan (§6.1) en los extremos de la cola.
- Mientras carga (carátula, clave de audio, conexión): indicador de carga (§5.7) y el paso
  actual en `t_cuerpo` · `texto` en lugar de los controles; aún no suena nada.
- Pista terminada: el botón central muestra «reproducir» y vuelve a empezarla; la barra de
  progreso queda llena y «Pista terminada» sustituye a los tiempos.
- Error: aviso de error (§5.8) con el detalle; A reintenta.
- Sin nada en reproducción: mensaje centrado «Todavía no suena nada».
- El audio sigue sonando en cualquier pestaña. B vuelve a «Búsqueda» sin cortarlo.
- El teclado (`swkbd`) bloquea el hilo principal: la pista se pausa mientras está abierto y
  sigue al cerrarlo.

### 7.6 Pista terminada / error

- Superior: última carátula o, si no la hay, el logotipo.
- Inferior: dentro de la pestaña «Reproducción» (§7.5), con la barra de pestañas.

### 7.7 Inicio

- Inferior: barra de pestañas con «Inicio» activa, botón primario «Buscar canciones» a 16 px
  bajo ella y, si la hay, la última búsqueda como botón secundario.
- Barra de botones: [A] Buscar. Volver a conectar y emparejar están en «Ajustes».

### 7.8 Ajustes

```
 INFERIOR
 ┌──────────────────────────┐
 │ [⌂] [⌕] [▮▮▮] [▓⚙▓]      │ 32 px
 │                          │
 │ [ (o) Idioma: Español  ] │ 48..80
 │ [  Volver a conectar   ] │ 96..128
 │ [Emparejar otra cuenta ] │ 144..176
 │──────────────────────────│
 │ [A] Elegir  [B] Volver   │
 └──────────────────────────┘
```

- Tres botones secundarios a todo el ancho útil, 32 px de alto y 16 px de separación, el
  primero a 16 px de la barra de pestañas. Foco con la cruceta (§6.2) y toque (§6.4).
- Idioma: el botón muestra el idioma actual y lo cambia al pulsarlo, sin burbuja.
- Volver a conectar y emparejar detienen la reproducción y vuelven a la pantalla de sesión.
- Superior: título «Ajustes» arriba a la izquierda y el nombre de la app centrado.
- B vuelve a «Inicio».

---

## 8. Movimiento

- Duraciones en frames a 60 Hz. Máximo 8 frames.
- Cambios de estado en un solo frame, sin interpolación ni fundidos.
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
  - progreso, descarga y reloj (≤ 1 vez por segundo),
  - feedback de pulsación (§6) mientras dura,
  - cambio de volumen o pausa,
  - llegada de la carátula,
  - aparición y retirada de la burbuja.
- No dibujar indicadores de carga, marquesinas ni animaciones en bucle en el reproductor.
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