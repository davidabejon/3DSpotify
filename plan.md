# Plan: vistas de detalle de canción, álbum y artista

Ruta propuesta en el repo: `docs/plan_vistas_detalle.md`.

Este plan parte del código de la instantánea actual (`source/main.c` con `run_tabs()`,
`source/ui/view_results.c`, `source/ui/view_player.c`, `source/platform3ds/spclient.c`) y de
`docs/STYLES.md`. Nada de lo que depende del hardware (render, input, red, audio) se puede
verificar fuera de la consola: cada fase indica qué se comprueba en host y qué queda para la
3DS.

---

## 1. Objetivo

| Desde                                   | Acción                               | Abre                    |
|-----------------------------------------|--------------------------------------|-------------------------|
| Pestaña «Búsqueda», lista de resultados | X sobre la fila en foco              | Detalle de canción      |
| Detalle de canción                      | Botón «Artista»                      | Detalle de artista      |
| Detalle de canción                      | Botón «Álbum»                        | Detalle de álbum        |
| Detalle de álbum / artista, lista       | X sobre la fila en foco              | Detalle de canción      |
| Pestaña «Reproducción», canción activa  | Tocar el título                      | Detalle de canción      |
| Pestaña «Reproducción», canción activa  | Tocar el artista                     | Detalle de artista      |
| Pestaña «Reproducción», canción activa  | Tocar el álbum                       | Detalle de álbum        |

«Canción activa» = fase `LOADING`, `PLAYING` o `FINISHED` (las fases en las que
`view_player_draw_bottom()` ya pinta el texto de la pista). En `IDLE` y `FAILED` no hay
enlaces.

Lo que **no** cambia: A (o tocar la fila en foco) en resultados sigue reproduciendo sin
pantalla intermedia (§7.4). El detalle es un camino adicional por X.

---

## 2. Qué falta hoy

1. **Identificadores.** `sp_track` solo guarda `artist` y `album` como texto. Para abrir un
   álbum o un artista hacen falta sus URIs. El protobuf ya los trae: `Track.album (3) →
   Album.gid (1)` y `Track.artist (4) → Artist.gid (1)`, 16 bytes cada uno, que hay que
   pasar a base62 (`spotify:album:<22>` / `spotify:artist:<22>`). No existe ningún helper
   base62 en el repo.
2. **Metadatos de álbum y artista.** `spclient.c` solo pide `EXT_KIND_TRACK_V4`. Faltan las
   peticiones de álbum (nombre, artista, fecha, sello, portada, discos con los gid de sus
   pistas) y de artista (nombre, retrato, `top_track` por país).
3. **Parser duplicado.** La lectura de un `Track` está copiada en `sp_track_metadata()` y en
   `metadata_batch_request()`. Añadir campos en dos sitios es buscar un desajuste.
4. **Navegación.** `run_tabs()` enruta por `active_tab` a una vista fija por pestaña. No hay
   concepto de sub-pantalla ni de pila para volver con B.
5. **Lista reutilizable.** Toda la lógica de lista (foco, arrastre §6.9, desplazamiento,
   miniaturas, filas §5.3) vive dentro de `view_results.c`. Álbum y artista necesitan lo
   mismo.
6. **Pestaña «Reproducción».** Solo pinta título y artista (no el álbum), en líneas de 18 y
   15 px, por debajo del mínimo táctil de 32 px (§4.4). `player_hit_test()` solo conoce los
   tres botones.
7. **Pantalla superior.** `top_logic.h` solo conoce `TOP_LOGO`, `TOP_NOW_CARD` y
   `TOP_ART_BIG`, y todo usa `cover_art`, que **no se puede reutilizar**: `player_pump()`
   espera a `artwork_pending(&cover_art)` antes de preparar la pista, así que pedir ahí la
   portada de un álbum retrasaría la reproducción.
8. **País de la cuenta.** `ap_info.country` se rellena en el login pero `spotify_session` no
   lo guarda; hace falta para elegir el grupo de `top_track` del artista.

---

## 3. Decisiones de diseño

### 3.1 Pila de detalle por pestaña

- Cada pestaña que puede abrir detalles («Búsqueda» y «Reproducción») tiene su propia pila.
  Mientras la pila de la pestaña activa no está vacía, la pantalla inferior la dibuja el
  detalle de la cima en lugar de la vista raíz de la pestaña. La barra de pestañas sigue
  arriba con la misma pestaña activa.
- B desapila un nivel; con la pila vacía, B hace lo de siempre (resultados → «Inicio»,
  reproductor → «Búsqueda»).
- L/R cambian de pestaña sin tocar las pilas: al volver, el usuario está donde lo dejó.
- Profundidad máxima `DETAIL_DEPTH = 6`. Al apilar con la pila llena se descarta el nivel más
  antiguo (el grafo canción → álbum → canción → artista → … no tiene fin).
- Se vacía la pila de «Búsqueda» al lanzar una búsqueda nueva (`search_start()`), y ambas en
  `stop_tabs()`.
- Lógica pura en `source/ui/detail_nav.h` (push/pop/top/clear/descartar el más antiguo),
  testeable en host.

### 3.2 Documento de detalle y pool estático

Cada nivel de la pila apunta a un `detail_doc` de un pool **global** (nunca en el stack de un
hilo):

```c
typedef enum { DETAIL_TRACK, DETAIL_ALBUM, DETAIL_ARTIST } detail_kind;
typedef enum { DETAIL_LOADING, DETAIL_READY, DETAIL_FAILED } detail_state;

typedef struct {
	detail_kind  kind;
	detail_state state;
	char uri[64];
	/* Cabecera (pantalla superior) */
	char title[128];        /* canción, álbum o artista */
	char subtitle[128];     /* artista del álbum */
	char art[80];           /* portada o retrato, art_large */
	char label[64];
	int  year, album_type, total_tracks;
	/* Lista. DETAIL_TRACK: copia de la lista de origen; la canción
	 * mostrada es tracks[selected] y «Reproducir» usa la lista entera
	 * como cola. DETAIL_ALBUM/ARTIST: sus pistas. */
	sp_track tracks[DETAIL_MAX_TRACKS];
	int  count;
	/* Estado de la vista, para volver exactamente igual. */
	int  selected, first;
	char error[192];
} detail_doc;
```

- `DETAIL_MAX_TRACKS = 24`, igual que `SEARCH_LIMIT`, `MAX_TRACKS` y `THUMBS_MAX`.
- Pool: 2 pestañas × `DETAIL_DEPTH` = 12 documentos. Con el `sp_track` ampliado (~0,9 KB)
  son ~22 KB por documento, ~270 KB en BSS. Se acepta a cambio de no repetir peticiones al
  volver con B (cada conexión nueva paga un handshake TLS de segundos).
- Guardar la lista de origen dentro del `DETAIL_TRACK` evita punteros a listas que pueden
  cambiar (resultados, `playback_queue`).

### 3.3 Carga en segundo plano

- Un único trabajo `detail_job` global en `main.c`, calcado de `search_job`: copia de host y
  `library_token`, `cancel_requested`, `done`, hilo con `background_priority()` y 16–32 KB de
  stack. `secure_clear()` de las copias del token al terminar, como `search_join()`.
- Una sola `http_session` keep-alive por carga (`http_session_open()` +
  `sp_track_metadata_batch_s()`), porque álbum y artista necesitan dos peticiones: metadatos
  de la entidad y lote de pistas.
- `DETAIL_TRACK` no necesita red: todo está ya en el `sp_track` (con los campos nuevos).
- Abrir otro detalle mientras uno carga cancela el anterior sin esperarlo (mismo patrón que
  `player_release()` con `prep.abandoned`), para no bloquear el hilo principal y con él el
  decoder.
- `stop_tabs()`: cancelar y esperar el trabajo **antes** de `thumbs_wait()` y con
  `tls_set_abort(true)` activo, como el resto de workers.

### 3.4 Entradas en cada pantalla

| Pantalla             | A                          | X                    | B              | Cruceta                |
|----------------------|----------------------------|----------------------|----------------|------------------------|
| Resultados           | Reproducir (sin cambios)   | **Detalle canción**  | Inicio         | Foco (sin cambios)     |
| Detalle canción      | Activar botón en foco      | —                    | Desapilar      | Foco entre botones     |
| Detalle álbum        | Reproducir álbum desde ahí | **Detalle canción**  | Desapilar      | Foco en la lista       |
| Detalle artista      | Reproducir populares       | **Detalle canción**  | Desapilar      | Foco en la lista       |
| Reproducción         | Pausa (sin cambios)        | Detalle canción (*)  | Búsqueda       | Saltar / volumen       |

(*) Opcional, ver §8. La petición es solo táctil.

- X respeta §6.6: si un arrastre dejó la fila en foco cortada o fuera de la vista, la primera
  pulsación solo devuelve la lista a ella y muestra el glifo X pulsado.
- Tocar la fila en foco en álbum/artista reproduce (como en resultados). El detalle por toque
  queda para X: no hay gesto táctil distinto de «tocar» en §6.
- El glifo X usa `COL_BTN_X`, que ya existe en `theme.h`. Barra de botones en listas:
  `[A] Reprod. [X] Detalle [Y] Buscar [B] Volver` en resultados (4 acciones, el máximo de
  §5.6); `[A] Reprod. [X] Detalle [B] Volver` en álbum y artista.

### 3.5 Pantalla superior

- Nuevo `artwork_state detail_art` en `main.c`, independiente de `cover_art`. Coste: otra
  textura 256×256 RGBA8 (256 KB de memoria lineal) mientras haya imagen.
- `top_logic.h`: nuevo modo `TOP_DETAIL`, elegido cuando la pestaña activa tiene un detalle
  en pantalla. El spinner de descarga sigue encima en cualquier modo.
- Maquetación igual que la cara superior de §7.5 (imagen 160×160 a 12 px, sombra 3 px,
  textos a la derecha), con otra fuente de datos. Se extrae el dibujo de `top_now.c` a una
  función que reciba los textos y el `artwork_state`, en vez de duplicarlo.
- Al desapilar, se vuelve a pedir la imagen del nivel que queda arriba. Son unos KB; la
  caché de imágenes queda fuera de este plan.

---

## 4. Diseño de cada pantalla (borrador para `STYLES.md`)

### 4.1 Detalle de canción

```
 SUPERIOR                                  INFERIOR
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │ ┌──────────┐                   │        │ [⌂] [▓⌕▓] [▮▮▮] [⚙]      │ 32 px
 │ │          │  Título canción   │        │                          │
 │ │ carátula │  Artista          │        │ [▶  Reproducir         ] │ 48..80   primario
 │ │ 160×160  │  Álbum            │        │ [   Artista: Nombre    ] │ 96..128  secundario
 │ │          │  3:42 · Pista 2   │        │ [   Album: Nombre      ] │ 144..176 secundario
 │ └──────────┘  2004             │        │──────────────────────────│
 └────────────────────────────────┘        │ [A] Elegir  [B] Volver   │
                                           └──────────────────────────┘
```

- Misma geometría que Ajustes (§7.8): tres botones de 32 px a todo el ancho útil, 16 px de
  separación, foco con la cruceta (§6.2) y toque (§6.4). El foco empieza en «Reproducir».
- «Artista» / «Álbum» se deshabilitan (§6.1) si la pista no trae ese URI. El nombre se
  recorta con «...» (§3.3); completo en la pantalla superior.
- «Reproducir» reproduce `tracks[selected]` con la lista de origen como cola y pasa a
  «Reproducción». Si se abrió desde «Reproducción» y es la pista que ya suena, solo cambia de
  pestaña (no la reinicia).
- Sin carga: los datos ya están en memoria.

### 4.2 Detalle de álbum

```
 SUPERIOR                                  INFERIOR
 ┌────────────────────────────────┐        ┌──────────────────────────┐
 │ ┌──────────┐                   │        │ [⌂] [▓⌕▓] [▮▮▮] [⚙]      │
 │ │ portada  │  Nombre del álbum │        │┌───────────────────────┐▓│
 │ │ 160×160  │  Artista          │        ││┌──┐ Canción uno  3:42 │▓│
 │ │          │  Album · 2004     │        │││ 1│ Artista           │░│ 4 filas
 │ │          │  11 canciones     │        ││└──┘                   │░│
 │ └──────────┘  Sello            │        │ ...                     ░│
 └────────────────────────────────┘        │ [A] Reprod. [X] Detalle [B]│
                                           └──────────────────────────┘
```

- Lista de §5.3 con una variante: en lugar de la miniatura (todas las pistas comparten
  portada), el cuadrado 33×33 `capa_alta` lleva el número de pista en `t_meta` · `texto_2`
  (`tinta` en la fila en foco). Sin descarga de miniaturas.
- Disco múltiple: numeración continua en la lista; el nº de disco solo en el detalle de
  canción.
- Más de 24 pistas: se muestran las 24 primeras y la superior dice «24 de 31 canciones».
- Cargando: filas esqueleto y burbuja «Cargando...» (§5.9 con §5.7), como la búsqueda. B
  cancela y desapila.
- Error: aviso (§5.8) con el detalle, A reintenta, B desapila.

### 4.3 Detalle de artista

- Igual que el álbum, pero con miniaturas reales (§5.3, vía `thumbs`), porque cada canción
  popular es de un álbum distinto.
- Superior: retrato (`portrait_group`, o `portrait` como respaldo) en el cuadro de 160×160,
  nombre en `t_titulo`, «Canciones populares» en `t_cuerpo` · `texto_2`.
- Lista: `top_track` del país de la cuenta; si no hay grupo para ese país, el primero.
- Discografía, biografía y relacionados quedan fuera (ver §8).

### 4.4 Pestaña «Reproducción»: texto pulsable

Tres zonas táctiles de 32 px (§4.4) en lugar de dos líneas sueltas. Todo lo que hay debajo
baja 8 px:

```
 INFERIOR (antes → después)
   tabs        0..32      0..32
   título     44..62     36..68   zona «canción», título + «3 de 24»
   artista    66..81     72..104  dos zonas de 150 px separadas 4 px:
   (álbum)       —                «artista» (8..158) | «álbum» (162..312)
   controles 100..148   108..156  PLAYER_CONTROLS_TOP 100 → 108
   tiempos   155..169   163..177
   progreso  172..180   180..188
   volumen   193..199   201..207
   botones   216..240   216..240
```

- La línea de álbum es nueva en la inferior (hoy solo está en la superior). Artista y álbum
  se recortan con «...» a 150 px; el texto completo sigue en la carátula grande de arriba.
- Componente nuevo «zona de enlace» (§5.14 propuesto): en reposo, solo el texto, sin panel;
  pulsada (§6.3), panel `musgo` con contorno 1 px `tinta`, texto `tinta`, desplazada
  `PRESS_SHIFT`. Sin estado de foco: la cruceta del reproductor ya salta pistas y cambia el
  volumen.
- Activo al levantar el lápiz dentro (§6.4), como los botones. Zona deshabilitada (sin URI)
  no responde.
- `player_logic.h`: `player_link_rect(link)`, `player_link_hit(x, y)` y
  `player_links_enabled(phase)`, puros y con test.

---

## 5. Fases

Cada fase deja el repo compilando sin warnings nuevos y con los tests en verde (salvo el fallo
conocido de `test_readmes_present_native_client_as_primary_workflow`).

### Fase 0 — Confirmar el formato de los datos (probe)

Objetivo: no adivinar números de campo, que en protobuf fallan en silencio.

- Ampliar `source/probe_meta/main.c`: con `SAMPLE_TRACK`, leer `Track.album.gid` y
  `Track.artist.gid`, pasarlos a base62, pedir el álbum y el artista por extended-metadata y
  volcar al log los campos de §2.2.
- Confirmar los valores de `ExtensionKind` para álbum y artista (en `extension_kind.proto` de
  librespot) antes de definir `EXT_KIND_ALBUM_V4` / `EXT_KIND_ARTIST_V4`. Si extended-metadata
  no responde, probar `GET /metadata/4/album/<gid hex>` y `/metadata/4/artist/<gid hex>`.
- Confirmar: `Album.date (6)` → `Date{year 1, month 2, day 3}`, `Album.label (5)`,
  `Album.type (4)`, `Album.disc (11)` → `Disc{number 1, track 3}`, `Artist.top_track (4)` →
  `TopTracks{country 1, track 2}`, `Artist.portrait_group (17)`, `Track.number (5)`,
  `Track.disc_number (6)`, `Track.explicit (9)`.
- Verificación: solo en consola. Guardar el log como referencia para los fixtures de la
  fase 1.

**Resultado (log de consola, 2026-11-13).** Hecha.

- extended-metadata responde para los tres: `TRACK_V4 = 10`, `ALBUM_V4 = 9`,
  `ARTIST_V4 = 8` (`type_url` `spotify.metadata.Track/Album/Artist`). No hace falta
  `/metadata/4/`.
- `Track`: `number (5)` y `disc_number (6)` son `sint32` (Mr. Brightside: 2 y 1).
  `explicit (9)` no viene cuando es falso: tratar ausente como 0. El `Album` incrustado en
  la pista ya trae `gid (1)`, `type (4)`, `label (5)` y `date (6)`, así que `year` sale de la
  pista sin otra petición.
- `Album`: `type (4)` es enum plano (1 = álbum). `date (6)` → `Date` en `sint32`
  (`2004-06-15`, año crudo 4008). Portada en `cover_group (17)` con tamaños 0, 1 y 2;
  `cover (9)` vacío. `disc (11)` → `Disc{number 1 (sint32), track 3}`, y **cada pista del
  disco trae solo el `gid`** (18 bytes): los nombres requieren el lote de pistas, como
  estaba previsto. Respuesta de unos 1,1 KB.
- `Artist`: retrato en `portrait_group (17)` (tamaños 0, 1 y 2); `portrait (11)` vacío.
  `top_track (4)` trae **un solo grupo, ya filtrado por el país de la cuenta** (`ES`), con
  10 pistas de solo `gid`. La respuesta pesa **27 KB** (biografía de 14 KB en `f12` y 519
  `appears_on_group`); cabe en el límite por defecto de `http_request`.
- `ap_info.country` llegó **vacío** (el paquete `CountryCode` no había llegado al cerrar la
  sesión AP). Como el servicio ya filtra `top_track`, basta con usar el grupo cuyo país
  coincida y, si no, el primero: el país deja de ser imprescindible (ver fase 1.6).
- **Pistas duplicadas en el catálogo**: el `SAMPLE_TRACK` (`3n3Ppam7…`) trae
  `canonical_uri (36)` = `spotify:track:003vvx7Niy0yvhvHt4a68B`, que es la pista que sale en
  los populares del artista. Comparar «¿es la pista que suena?» solo por URI fallará entre
  listas distintas; considerar `canonical_uri` (afecta a §4.1 y §8.6).
- Vector base62 para los tests de la fase 1: `3n3Ppam7vgaVa1iaRUc9Lp` ↔
  `6ecac90c03f74dcba9e21b408e14719f`; álbum `4OHNH3sDzIxnmUADXzv2kT` ↔
  `9e4c9dd5f8d641e0aaff6b2a560b45af`; artista `0C0XlULifJtAgn6ZNCW2eu` ↔
  `142571008feb40c881095ffe591b793a`.

### Fase 1 — Datos (`platform3ds/`)

1. **Helper base62**, `source/platform3ds/spotify_id.[ch]`, sin dependencias de 3DS:
   `sp_id_from_gid(const uint8_t gid[16], char out[23])`,
   `sp_gid_from_id(const char *id, uint8_t gid[16])` y `sp_uri_from_gid(kind, gid, out, len)`.
   Test en host: ida y vuelta, entradas inválidas y el vector real del log de la fase 0.
2. **Un solo parser de `Track`**: extraer `static void read_track(const pbv *t, sp_track *out)`
   y usarlo en `sp_track_metadata()` y `metadata_batch_request()`. Sin cambio de
   comportamiento.
3. **Ampliar `sp_track`** (en `spclient.h`, con comentario del porqué):
   `album_uri[40]`, `artist_uri[40]`, `track_number`, `disc_number`, `explicit`, `year`.
   Revisar los usos por valor: `search.tracks`, `playback_queue`, `prep.track`.
4. **Nuevas funciones** en `spclient.c`, sobre `http_session`:
   - `sp_album_metadata(sess, token, album_uri, sp_album *out, err, errlen)`: cabecera +
     hasta `SP_ALBUM_MAX_TRACKS` URIs de pista en orden de disco.
   - `sp_artist_metadata(sess, token, artist_uri, country, sp_artist *out, err, errlen)`:
     nombre, retrato y URIs de populares del país (o del primer grupo).
   - Recorrer los campos repetidos a mano, como ya hacen los ficheros de audio y las
     imágenes.
5. **Parsers testeables**: llevar `read_track`, `read_album` y `read_artist` a
   `source/platform3ds/meta_parse.[ch]` (solo dependen de `proto3ds`) y testearlos en host con
   protobuf construido con `pb_*`. Añadir los `.c` al compilado de `native_logic_test.c`.
6. **País**: `char country[8]` en `spotify_session`, copiado de `login_ap.country` en
   `resolve_and_login()`. No es secreto. Puede llegar vacío (el paquete `CountryCode` llega
   cuando llega): el código debe tolerarlo. Tras la fase 0 es opcional: `top_track` ya
   llega filtrado por país y el primer grupo sirve de respaldo.

**Estado (2026-11-13).** Hechos 1–5; el 6 se deja para la fase 2 (`sp_artist_metadata()`
acepta `country` NULL o vacío y usa el primer grupo).

- `spotify_id.[ch]`: `sp_id_from_gid()`, `sp_gid_from_id()`, `sp_uri_from_gid()` y
  `SP_URI_MAX = 40`.
- `meta_parse.[ch]`: `sp_parse_entity()`, `sp_parse_track()`, `sp_parse_album()`,
  `sp_parse_artist()` y los `EXT_KIND_*`. `spclient.c` ya no lee protobuf de metadatos por su
  cuenta: el sencillo y el lote usan `sp_parse_track()`.
- `proto3ds`: `pb_next_bytes()` para recorrer campos repetidos.
- `sp_track`: `album_uri`, `artist_uri`, `track_number`, `disc_number`, `is_explicit`
  (`explicit` es palabra reservada en C++) y `year`.
- `sp_album` / `sp_artist` y `sp_album_metadata()` / `sp_artist_metadata()` sobre
  `http_session`; los nombres de las pistas se piden después con
  `sp_track_metadata_batch_s()` en la misma sesión.
- Tests de host en `native_logic_test.c` (base62 con los vectores reales, y los tres
  parsers con protobuf construido con `pb_*`). Ejecutados con MSVC: en verde.
- `probe_meta` paso 14: las mismas llamadas del cliente, para comprobarlas en consola.
  Verificado en consola: álbum (14 de 14 pistas, año, sello, tipo, portada), artista (10
  populares, retrato), URIs, nº de pista/disco y sesión reutilizada.
- **Bug previo encontrado en esa prueba:** el lote de extended-metadata **no responde en el
  orden pedido** (el álbum volvió 14, 5, 12, 3…), y `metadata_batch_request()` lo asumía.
  Afectaba ya a la búsqueda y a las playlists. Arreglado con `sp_parse_track_batch()`, que
  coloca cada entrada por su `entity_uri`, con test de host.

### Fase 2 — Navegación y carga (`main.c`, `ui/detail_nav.h`)

1. `source/ui/detail_nav.h` (puro): pila de índices al pool, `DETAIL_DEPTH`, push con
   descarte del más antiguo, pop, top, clear. Tests en host.
2. `main.c`, sección nueva `/* --- Details --- */`: pool `detail_docs[]`, una pila por
   pestaña, `detail_job`, y
   - `detail_open_track(tab, list, count, index)`
   - `detail_open_album(tab, uri)` / `detail_open_artist(tab, uri)`
   - `detail_back(tab)`, `detail_poll()` (una vez por frame, junto a `search_poll()`),
     `detail_stop()` (para `stop_tabs()`).
3. `run_tabs()`: si `detail_active(active_tab)`, el input va a la vista de detalle; si no, a
   la vista raíz. `frame()`: lo mismo para `*_dirty()` y `*_draw_bottom()`.
4. `search_start()` vacía la pila de «Búsqueda».
5. Reproducir desde una lista de detalle: copiar la lista a `playback_queue`,
   `queue_from_results = false`, `player_play()`, `switch_tab(TAB_PLAYER)`. Igual que
   `play_from_results()`, incluido `view_results_allow_thumbs(false)` durante la preparación.

**Estado (2026-11-13).** Hechos 1, 2 y 4; el 3 y el 5 pasan a la fase 4, que es cuando hay
una vista que enrutar y desde la que reproducir (antes solo serían funciones sin usar).

- `source/ui/detail_nav.h` (puro, con tests): pila con descarte del más antiguo,
  `detail_tab_stack()` y `detail_pool_free()`.
- **Cambio respecto al plan:** el pool, las pilas y el cargador viven en
  `source/ui/details.[ch]` en lugar de en una sección de `main.c` (ya ~2000 líneas), siguiendo
  el patrón de `thumbs.c`. API: `details_active/top/open_track/open_album/open_artist/back/
  retry/clear/poll/stop`. `main.c` solo llama a `details_poll()` tras `search_poll()`,
  `details_clear(TAB_SEARCH)` en `search_start()` y `details_stop()` en `stop_tabs()`.
- El trabajo escribe en sus propios búferes y `details_poll()` lo vuelca al documento solo si
  su `serial` no ha cambiado; así abrir otro detalle cancela sin esperar y el que siga
  pendiente en una pila se relanza después (primero el de la pestaña activa).
- Sin país: `top_track` ya llega filtrado (fase 0).
- BSS: `docs` 274 KB + `job` 26 KB.
- Pendiente para la fase 8: los dos errores propios del cargador («no connection to…»,
  «loader thread did not start») van sin traducir, como los de `spclient`.

### Fase 3 — Lista reutilizable (refactor sin cambios visibles)

- Extraer de `view_results.c` un widget `source/ui/track_list.[ch]`: foco, cruceta (§6.6),
  arrastre (§6.9), indicador (§5.11), filas (§5.3) con dos variantes de cuadrado (miniatura o
  número) y su `look`/`dirty`.
- `view_results.c` pasa a usarlo y conserva solo sus modos (idle, buscando, vacío, error) y
  su barra de botones.
- Verificación: tests de host en verde; en consola, la búsqueda debe comportarse exactamente
  igual que antes. Este paso va en su propio commit para poder bisecar.

**Estado (2026-11-13).** Hecha; falta la comprobación en consola.

- `source/ui/track_list.[ch]`: widget con instancia propia (`track_list`), para que
  resultados conserve su estado bajo un detalle. Lleva contenido, foco, desplazamiento,
  pulsación y toque de fila, arrastre, empujón en los extremos, reajuste del foco fuera de
  vista (`track_list_realign(l, down, confirm_keys)`, donde la fase 4 añadirá `KEY_X`), filas,
  esqueleto e indicador. Su `track_list_look` se compone en el `look` de cada vista.
- Variante `TRACK_LIST_NUMBERS` ya hecha (sin uso hasta la fase 5): cuadrado `capa_alta` con
  la posición 1-based. **Desviación de §4.2:** el número va siempre en `texto_2`, también en
  la fila en foco, porque el cuadrado sigue siendo `capa_alta` y `tinta` sobre él apenas se
  leería. Recoger en `STYLES.md` (fase 8).
- `view_results.c` (790 → 558 líneas) conserva modos, botón, glifos, burbujas y barra; su
  `touch` propio solo atiende al botón de los modos sin lista.
- `tests/test_build.py`: las aserciones de lista apuntan ahora a `track_list.c`, y las reglas
  de estilo (sin colores sueltos ni primitivas) lo incluyen.

### Fase 4 — Detalle de canción + X en resultados

- `source/ui/view_detail.[ch]`: una vista con tres modos (`DETAIL_TRACK/ALBUM/ARTIST`) que
  recibe el `detail_doc` de la cima. Modo canción: los tres botones de §4.1 (reutilizar la
  geometría de `settings_logic.h`, moviéndola a un header común si hace falta).
- `view_results.h`: `RESULTS_ACTION_DETAIL` con X, glifo X en la barra.
- `run_tabs()`: `RESULTS_ACTION_DETAIL` → `detail_open_track(TAB_SEARCH, search.tracks,
  search.count, view_results_selected())`.

**Estado (2026-11-13).** Hecha, con los puntos 2.3 y 2.5 que venían de la fase 2; falta
probarla en consola.

- `source/ui/detail_logic.h` (puro, con tests): `detail_button_rect()` = geometría de Ajustes,
  `detail_hit_test()` y `detail_focus_move()` saltando botones deshabilitados.
- `source/ui/view_detail.[ch]`: recibe el `detail_doc` de la cima en cada llamada y detecta
  el cambio de documento por puntero + `serial` + `state` (reinicia foco y pulsaciones). Modo
  canción completo; para álbum/artista solo pinta la barra con B (fase 5). SELECT muestra la
  burbuja de idioma.
- `view_results`: `RESULTS_ACTION_DETAIL` con X (solo con lista), glifo X en la barra
  (`[A] Reproducir [X] Detalle [Y] Buscar [B] Volver`), y X también reajusta el foco fuera de
  vista (§6.6).
- `main.c`: `switch_tab()`, `run_tabs()` y `frame()` dan paso a la vista de detalle si la
  pestaña tiene pila; `detail_input()`, `play_from_detail()` y `open_result_detail()`.
  **Decisión:** reproducir desde un detalle vacía la pila de «Reproducción» para que esa
  pestaña muestre el reproductor; la de «Búsqueda» se conserva. Si la pista ya suena
  (`LOADING`/`PLAYING`, comparando URI), solo cambia de pestaña.
- Strings (EN/ES): `STR_HINT_DETAIL`, `STR_DETAIL_PLAY`, `STR_DETAIL_ARTIST_FMT`,
  `STR_DETAIL_ALBUM_FMT`, `STR_UNKNOWN_ALBUM`. El test de idiomas admite «Album: %s» igual en
  los dos.
- `test_build.py`: `test_detail_views_are_wired`.
- Los botones «Artista»/«Álbum» ya devuelven su acción, pero `main.c` no la atiende hasta la
  fase 5. La pantalla superior sigue mostrando el logo o la ficha de la pista que suena
  (fase 7).

### Fase 5 — Detalle de álbum y de artista

- Modos lista de `view_detail.c` sobre `track_list`: carga (esqueleto + burbuja), error
  (aviso + reintento), lista con A/X/B.
- Botones «Artista»/«Álbum» del detalle de canción → `detail_open_artist/album()` en la misma
  pestaña.
- Miniaturas en artista: `thumbs_set_tracks()` con las pistas del artista al entrar y con las
  de resultados al volver. `thumbs` mantiene una sola lista, así que volver a resultados
  vuelve a descargar las miniaturas (ver §8).

**Estado (2026-11-13).** Hecha; falta probarla en consola.

- `view_detail.c` con todos los modos: canción, cargando (esqueleto + burbuja «Cargando...»,
  B cancela y desapila), error (aviso con `doc->error` + botón «Reintentar», A o toque),
  lista (A o tocar la fila en foco reproduce desde ahí, X abre el detalle de canción con la
  lista como cola) y vacío («No hay canciones»). Una sola `track_list` cuyo estilo sigue al
  tipo: números para álbum, miniaturas para artista.
- La vista escribe `selected`/`first` en el documento cada frame; al volver al nivel se
  restaura con `track_list_scroll_to()`. Un mismo nivel que termina de cargar no pierde la
  pulsación en curso (solo se reinicia al cambiar de nivel).
- `main.c`: «Artista»/«Álbum» → `details_open_artist/album()` en la misma pestaña; X →
  `details_open_track()` con la lista del nivel; reintento → `details_retry()`.
- **Miniaturas (cambio respecto a 3.3/§5):** en vez de que resultados las resincronice al
  volver, la propiedad de `thumbs.c` vive en `track_list.c`: la lista en pantalla la reclama
  desde el `update` de su vista (`track_list_thumbs_frame()`), y unos resultados que llegan
  sin estar en pantalla solo la toman si nadie la tiene (`track_list_offer_thumbs()`). El
  permiso durante la preparación de la pista es uno solo (`track_list_allow_thumbs()`). Así
  un avance de pista con el artista en pantalla no le quita las miniaturas. Siguen
  descargándose de nuevo al alternar entre listas (§8.3).
- Strings: `STR_DETAIL_LOADING`, `STR_DETAIL_EMPTY`, `STR_DETAIL_FAILED_TITLE`.

### Fase 6 — Enlaces en «Reproducción»

- `player_logic.h`: nueva maquetación de §4.4 (`PLAYER_CONTROLS_TOP = 108`), rects y hit test
  de las zonas, `player_links_enabled()`. Actualizar el comentario de cabecera con la nueva
  tabla de píxeles.
- `view_player.c`: parsear y dibujar la línea de álbum, estado pulsado de las zonas, nuevas
  acciones `PLAYER_ACTION_OPEN_TRACK/ARTIST/ALBUM` encoladas con `pending`/`press_shown()`
  como el resto.
- `run_tabs()`: abrir en la pila de `TAB_PLAYER`. El detalle de canción se abre con
  `playback_queue` como lista de origen y `player.index` como índice (copia, la cola puede
  cambiar después).
- `view_player_dirty()`: las zonas solo marcan `dirty` al pulsarse/soltarse (§9).

**Estado (2026-11-13).** Hecha; falta probarla en consola.

- `player_logic.h`: `PLAYER_CONTROLS_TOP = 108`, `player_link_rect()`, `player_link_hit()`
  (con máscara de habilitados) y `player_links_enabled()`, con la tabla de píxeles nueva en
  la cabecera. `PLAYER_TEXT_TOP` desaparece.
- `view_player.c`: línea de álbum nueva (`STR_UNKNOWN_ALBUM` si falta) y las tres zonas.
  **Cambio respecto a §4.4 tras probarlo:** un enlace no es un botón, así que pulsado no lleva
  panel, contorno ni desplazamiento: el texto pasa a `lima` con un subrayado `lima` de
  `BEVEL_W` bajo él. `lima` es el único acento con 4.5:1 sobre los dos tonos del fondo
  (4.78 y 5.96; `clorofila` da 3.74), comprobado en `test_theme_contrast`. El texto queda en
  su x de siempre (8 y 162). «3 de 24» no forma parte del enlace y no cambia. Artista y
  álbum recortados a su mitad. Las zonas solo marcan `dirty` al pulsarse o soltarse; la
  acción se devuelve tras `press_shown()`. Recoger en `STYLES.md` §5.14 (fase 8).
- `main.c`: `open_playing_detail()` abre el detalle en la pila de `TAB_PLAYER`; el de canción
  con una copia de `playback_queue` y `player.index`.
- Tests: zonas, hit test, fases y que los controles no se solapen con un enlace pulsado.
- X en «Reproducción» (§8.2) sigue sin hacerse.

### Fase 7 — Pantalla superior

- `top_logic.h`: `TOP_DETAIL` y `top_mode_for(tab, phase, detail_on)`. Test en host.
- `top.c` / `top_now.c`: dibujo de la ficha con `detail_art`; `top_dirty()` lee
  `artwork_dirty(&detail_art)` solo mientras se muestra.
- `frame()`: `artwork_poll(&detail_art)` junto al de `cover_art`.
- `ask_search_query()`: `artwork_invalidate(&detail_art)` además de `cover_art`.

**Estado (2026-11-13).** Hecha; falta probarla en consola. Se mantiene la ficha de 160×160 y
no la carátula grande (decidido tras preguntar).

- `top_logic.h`: `TOP_DETAIL` y `top_mode_for(tab, phase, detail_on)`, con test.
- `now_layout.h`: `NOW_LINES = 4` y `now_line_cy()`; test de que las cuatro líneas acaban
  junto a la carátula.
- `top_now.c`: `top_now_draw_header()` (carátula con sombra + título + líneas), compartida por
  la tarjeta de «ahora suena» y la ficha del detalle.
- `source/ui/top_detail.[ch]` (nuevo): textos de la ficha. Canción: artista / álbum /
  «3:42 - pista 2 - disco 2» (disco solo si > 1) / año. Álbum: artista / «Album - 2004» /
  «11 canciones» o «24 de 31 canciones» / sello. Artista: «Canciones populares». Cargando y
  error muestran su estado como título. Separador « - » en vez de «·» porque las tablas son
  ASCII.
- `top.c`: `top_set_detail(doc, art)` y el modo nuevo en `top_dirty()`/`top_draw()`.
- `main.c`: `detail_art` + `detail_art_url`; `top_detail_sync()` pide la imagen solo cuando
  cambia la URL del detalle en pantalla (volver al mismo detalle no descarga nada; uno sin
  imagen todavía la vacía). `artwork_poll`, invalidación tras el teclado y `artwork_close`
  como `cover_art`.
- Strings: `STR_DETAIL_TRACK_NUMBER_FMT`, `STR_DETAIL_DISC_FMT`, `STR_ALBUM_TYPE_*`,
  `STR_ALBUM_TRACK_ONE`, `STR_ALBUM_TRACKS_FMT`, `STR_ALBUM_TRACKS_CUT_FMT`,
  `STR_ARTIST_TOP_TRACKS` (sustituyen a `STR_DETAIL_TRACK_META_FMT` de la fase 8).
- Pendiente de vigilar en consola: la imagen del detalle se descarga aunque una pista se esté
  preparando (las miniaturas sí se pausan). Es una petición pequeña; si se nota, pausarla
  igual.

### Fase 8 — Textos, documentación y tests estructurales

- `source/lang/lang.h`, `lang_en.c`, `lang_es.c` (en **ambos**, español sin tildes ni ñ):
  `STR_HINT_DETAIL`, `STR_DETAIL_PLAY`, `STR_DETAIL_ARTIST_FMT`, `STR_DETAIL_ALBUM_FMT`,
  `STR_DETAIL_LOADING`, `STR_DETAIL_FAILED_TITLE`, `STR_DETAIL_TRACK_META_FMT`,
  `STR_ALBUM_TRACKS_FMT`, `STR_ALBUM_TRACKS_CUT_FMT`, `STR_ARTIST_TOP_TRACKS`,
  `STR_UNKNOWN_ALBUM`, `STR_ALBUM_TYPE_ALBUM/SINGLE/EP/COMPILATION`. Los `*_FMT` con los
  mismos especificadores en el mismo orden (lo comprueba `native_logic_test.c`).
- `docs/STYLES.md`: §2.5 (X), §5.3 (variante con número), §5.14 nuevo (zona de enlace),
  §6.5 (X), §7.4 (X abre el detalle; A sigue sin pantalla intermedia), §7.5 (nueva
  maquetación y enlaces), §7.9–7.11 nuevas (canción, álbum, artista), §9 (las vistas de
  detalle solo marcan `dirty` por pulsación, llegada de datos, imagen y miniaturas).
- `README.md` y `README_es.md`: controles nuevos (X, toque en título/artista/álbum).
- `CLAUDE.md`: estructura (`detail_nav.h`, `view_detail.*`, `track_list.*`,
  `spotify_id.*`, `meta_parse.*`). La sección actual aún habla de `playback()`, que ya no
  existe; conviene corregirlo de paso.
- `tests/test_build.py`: revisar las aserciones que buscan literales en `main.c`,
  `spclient.c` y `lang_*.c` tras los renombrados de las fases 1–6.

**Estado (2026-11-15).** Hecha.

- Antes, como ajuste de la fase 7: la ficha de detalle va centrada en la altura
  (`now_detail_art_rect()`, `now_detail_text_dy()`) y el texto que no cabe rueda en vez de
  recortarse (`marquee_logic.h`, `ui_text_marquee()` con scissor).
- Strings: casi todas llegaron con sus fases; faltaban los dos errores del cargador, ahora
  `STR_ERR_DETAIL_CONNECT_FMT` y `STR_ERR_DETAIL_THREAD`.
- `STYLES.md`: §3.3 (excepción de la ficha), §5.3 (variante con número), §5.14 (enlace, como
  quedó en la fase 6), §5.15 nueva (texto que rueda), §6.5 y §6.6 (X), §7.4 (X, superior
  actualizada), §7.5 (maquetación y enlaces; superior con la carátula grande), §7.9–7.12
  nuevas (navegación, canción, álbum/artista, ficha superior), §8 y §9.
- READMEs: X en Búsqueda, enlaces en Reproducción, ficha superior y sección «Details» /
  «Detalles».
- `CLAUDE.md` no existe en el repo: punto omitido.
- `test_build.py`: las aserciones existentes seguían en verde; nuevo
  `test_detail_views_follow_style_guide_and_docs`.

### Fase 9 — Verificación en consola

No automatizable aquí. Lista para probar con `3dslink`:

1. Buscar, X en una fila → detalle de canción con datos y carátula; B vuelve a la misma fila.
2. Artista y Álbum desde el detalle; X en sus listas; B desapila nivel a nivel.
3. Encadenar más de 6 niveles: el más antiguo se descarta y no hay cuelgues.
4. Reproducir desde álbum y artista: la cola es esa lista, anterior/siguiente sin vuelta.
5. Con música sonando: abrir y recorrer detalles sin cortes de audio (el cuello de botella
   conocido es el trabajo por frame en el hilo del decoder).
6. Reproducción: tocar título, artista y álbum en `LOADING`, `PLAYING` y `FINISHED`; nada en
   `IDLE` ni `FAILED`; los controles siguen respondiendo en su nueva posición.
7. L/R con un detalle abierto y vuelta: la pila sigue ahí.
8. Cancelar una carga con B; abrir otra mientras la anterior carga.
9. Volver a conectar / emparejar desde Ajustes con detalles abiertos: sin cuelgues
   (`stop_tabs()`).
10. SELECT dentro de un detalle: cambia el idioma y la vista se repinta.
11. Una pista sin álbum o sin artista: botones deshabilitados.

---

## 6. Invariantes a vigilar

- Nada de trabajo por frame en el bucle si no cambia nada: las vistas nuevas siguen el
  patrón `look`/`same_look()`/`dirty` y parsean texto solo al cambiar contenido o idioma (§9).
- `player_pump()` sigue siendo lo primero del bucle; ningún join bloqueante nuevo en el hilo
  principal salvo en `stop_tabs()`.
- `cover_art` sigue siendo solo de la pista en reproducción.
- Estructuras grandes (`detail_docs`, `sp_album`, `sp_artist`) en globales, nunca en el stack
  de un hilo.
- El token nunca se escribe en log ni pantalla; `secure_clear()` de sus copias en
  `detail_job`.
- Nada de cadenas visibles en `main.c`: todo por `tr(STR_...)`.
- La cola no da la vuelta (`playback_queue_step()` sin cambios).

---

## 7. Commits sugeridos

```
add. probe_meta dumps album and artist metadata
mv. share one Track parser between single and batch metadata
add. base62 helpers for Spotify ids with host tests
add. album and artist uris to sp_track
add. album and artist metadata requests
add. per-tab detail stack and background loader
mv. extract track list widget from results view
add. track detail view, opened with X from results
add. album and artist detail views
add. tappable title, artist and album in the Playing tab
add. detail card on the top screen
upd. strings, STYLES.md and READMEs for detail views
```

---

## 8. Puntos abiertos

1. **A en álbum/artista.** El plan propone que reproduzca esa lista desde la fila en foco,
   por coherencia con resultados. Alternativa: que A también abra el detalle.
2. **X en «Reproducción».** Atajo físico al detalle de la canción que suena. No se pidió;
   cuesta poco y da acceso sin pantalla táctil.
3. **Caché de miniaturas por URL.** `thumbs` conserva miniaturas por posición dentro de una
   sola lista; alternar entre resultados y artista las vuelve a descargar. Indexar el pool
   por URL lo evitaría. Mejora independiente, posterior.
4. **Varios artistas.** Se usa el primero de `Track.artist`. Elegir entre varios requeriría
   una lista intermedia.
5. **Discografía del artista.** `album_group`/`single_group` permitirían listar álbumes en el
   detalle de artista; fuera de este plan.
6. **Que la lista siga a la pista.** Como hace resultados con `view_results_focus()`, el
   detalle de álbum podría seguir la pista en reproducción si la cola salió de él.