# 3DSpotify

Cliente no oficial de Spotify para Nintendo 3DS. La aplicación se conecta
directamente a Spotify por HTTPS.

**Web:** [davidabejon.github.io/3DSpotify](https://davidabejon.github.io/3DSpotify/)
(descarga, código QR para FBI, características y controles). Su código está en
[site/](site/) y se publica en GitHub Pages con
[.github/workflows/pages.yml](.github/workflows/pages.yml) en cada push a
`main` que la modifique.

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Resumen
Este repositorio contiene:
- El cliente nativo para 3DS, con el punto de entrada en `source/main.c` y los módulos de interfaz en [source/ui](source/ui), que empareja la cuenta desde la consola, busca canciones y reproduce audio local con carátula.
- Probes independientes de protocolo y herramientas de build/tests en `source` y `tests`.

## Requisitos
- Cuenta de Spotify **Premium**.
- Una Nintendo 3DS con Homebrew Launcher y conexión Wi-Fi.
- Para compilar desde código: devkitPro con devkitARM, libctru, citro2d, citro3d y 3dsxtool, además de Python 3.12 o posterior.

## Compilar el cliente nativo

Instala devkitPro con devkitARM, libctru, citro2d, citro3d y 3dsxtool, además
de Python 3.12 o posterior. Desde la raíz del repositorio:

```powershell
python source\build.py --fetch-deps
```

El target predeterminado y único cliente completo es `spotify3ds`. La descarga
fija y verifica mbedTLS 3.6.7, `stb_vorbis` y `qrcodegen` 1.8.0 (el código QR
del emparejamiento); también genera la cabecera de
Vorbis desde esa misma fuente verificada. Tras descargar las dependencias, se
puede compilar sin `--fetch-deps`.

Las pruebas de build e integración se ejecutan desde la raíz con:

```powershell
python -m unittest discover -s tests
```

La prueba C de codificación, cola y cancelación se ejecuta si hay un compilador
C nativo disponible; en caso contrario, unittest la marca como omitida.

El artefacto queda en
`source\dist\3ds\3DSpotify\3DSpotify.3dsx`. Cópialo a
`SD:/3ds/3DSpotify/` y ejecútalo desde Homebrew Launcher, o envíalo desde el
PC con:

```powershell
3dslink.exe source\dist\3ds\3DSpotify\3DSpotify.3dsx -a <IP_DE_LA_CONSOLA>
```
`3dslink` forma parte de las herramientas de devkitPro.

Para instalarla en el menú HOME, genera también el CIA (requiere `makerom` y
`bannertool`, incluidos en las herramientas de devkitPro):

```powershell
python source\build.py --cia
```

Se genera `source\dist\3ds\3DSpotify\3DSpotify.cia`; cópialo a la SD e
instálalo con FBI o un instalador de CIA similar (requiere custom firmware).
El CIA guarda su configuración en `SD:/3ds/spotibacan/`.

Tanto el `.3dsx` como el `.cia` empaquetan el directorio `romfs/` del
repositorio, que la app lee como `romfs:/`; si falta, el build se detiene.
Contiene el logo de la pantalla superior, `romfs/gfx/logo.png`:

- un diseño propio, sin el nombre ni el logo de Spotify;
- un PNG RGBA de 8 bits de 256×128 px como máximo, que se dibuja 1:1 sin
  reescalar (una imagen mayor no se carga); conserva su canal alfa, bordes
  suaves incluidos.

Sin un logo válido, la pantalla superior muestra el nombre de la app como
texto.

El target inicia la sesión directa: renueva la credencial guardada, permite
emparejar si hace falta y comprueba la conexión al access point y a la sesión
de biblioteca. El refresh token se guarda en la SD para los siguientes
arranques.

### Inicio de sesión

Mientras conecta, la pantalla superior muestra el logo y un spinner, y la
inferior el paso actual («Paso n de N»). **SELECT** cambia el idioma en todas
las pantallas de inicio de sesión.

Si no hay una credencial guardada, o ha caducado, la consola pide vincular la
cuenta:

- **Escribe el código:** abre `spotify.com/pair` en el móvil o el ordenador,
  inicia sesión, escribe el código de la pantalla superior y acepta.
- **O escanea el código QR** de la pantalla inferior: abre la misma página con
  el código ya escrito.
- **B** cancela el emparejamiento sin tocar la credencial guardada.

Si la sesión no puede iniciarse, la pantalla inferior explica por qué:

- **A** (o el botón **Reintentar**) vuelve a intentarlo con la credencial
  guardada. **A** pulsa el botón que tenga el foco; izquierda/derecha lo mueve.
- **Y** (o el botón **Emparejar**) empareja de nuevo con un código nuevo.
- **START** sale.

Sin red (Wi-Fi apagado, o un fallo al preparar la red o TLS de la consola),
solo quedan **START** y **SELECT**.

### Pestañas

Una vez iniciada la sesión, la parte superior de la pantalla inferior es una barra con cuatro
pestañas, cada una con su icono:

| Icono | Pestaña | Qué contiene |
|---|---|---|
| Casa | **Inicio** | Buscar canciones y repetir la última búsqueda. |
| Lupa | **Búsqueda** | Los últimos resultados, la búsqueda en curso o su error. |
| Ecualizador | **Reproducción** | La canción que suena, con sus controles. |
| Engranaje | **Ajustes** | Idioma, volver a conectar y emparejar otra cuenta. |

- Toca una pestaña para abrirla (se activa al levantar el lápiz dentro de ella).
- **L/R** pasan a la pestaña anterior o siguiente desde cualquier pantalla; **ZL/ZR** (solo
  New 3DS) hacen lo mismo.
- La música sigue sonando en cualquier pestaña, y la búsqueda corre en segundo plano mientras
  te mueves entre ellas.
- **SELECT** cambia el idioma desde cualquier pestaña y **START** sale.

### Pantalla superior

- Sin nada sonando, o tras un error de reproducción, todas las pestañas
  muestran el logo.
- Mientras una pista carga, suena (también en pausa) o acaba de terminar,
  **Inicio**, **Búsqueda** y **Ajustes** muestran la ficha de lo que suena:
  carátula, título, artista, álbum, el tiempo y la barra de progreso con la
  descarga por delante. La pestaña **Reproducción** muestra la carátula en
  grande.
- Mientras hay una búsqueda en curso, la pestaña **Búsqueda** añade encima
  del logo o de la ficha una burbuja flotante «Buscando…» con el indicador de
  carga.

### Inicio

Toca **«Buscar canciones»** o pulsa **A** para abrir el teclado. Debajo aparece tu última búsqueda como un botón secundario; tócalo o llega a él con arriba/abajo y pulsa **A**. Al confirmar, la app pasa a la pestaña **Búsqueda**.

**Controles táctiles:**
- Toca un botón para resaltarlo; el botón muestra un estado pulsado cuando levantas el lápiz dentro de él.
- Si deslizas el lápiz fuera del botón antes de levantarlo, se cancela el toque.

**Indicadores de acción:** Los glifos con fondos de color muestran las acciones disponibles:
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — abre el teclado de búsqueda

Mientras el teclado está abierto, la canción que suena se pausa y sigue al cerrarlo.

### Búsqueda

Los resultados muestran cuatro canciones a la vez. La **pantalla inferior** muestra la lista con miniaturas bajo la barra de pestañas; la superior sigue lo descrito en [Pantalla superior](#pantalla-superior).

**Navegación:**
- Arriba/abajo recorre la lista; la canción resaltada gana un fondo verde más brillante.
- Izquierda/derecha salta de cuatro en cuatro.
- Toca una canción para resaltarla; tócala otra vez (o pulsa **A**) para reproducirla: la app pasa a la pestaña **Reproducción**.
- Arrastra la lista con el lápiz para desplazarla; la barra del borde derecho indica por dónde vas.

**Indicadores de acción:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — reproduce la canción resaltada
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — vuelve a la pestaña Inicio
- <span style="background-color: #39D97A; color: black; padding: 2px 4px; border-radius: 2px;">Y</span> — inicia una nueva búsqueda
- Izquierda/derecha — salta de cuatro en cuatro

**Gestos táctiles:**
- **Tocar una fila:** selecciona la canción; la fila se resalta.
- **Tocar nuevamente:** reproduce la canción al momento con el resto de resultados como cola.
- **Arrastrar verticalmente:** desplaza la lista; las filas pueden quedar cortadas por la barra de pestañas y la barra de botones.
- **Arrastrar más de 8 píxeles desde donde tocaste:** el arrastre se confirma; el toque de la fila se cancela.

**Estado de la búsqueda:**
- Antes de la primera búsqueda, la pestaña ofrece el botón **«Buscar canciones»**.
- Si la búsqueda falla, pulsa **A** (o toca el botón de la pantalla) para reintentar.
- Pulsa **B** para cancelar una búsqueda pendiente cuando termine la petición de red activa.
- Mientras suena la cola de estos resultados, la fila resaltada sigue a la canción que suena.

### Reproducción

La canción elegida se reproduce localmente con el resto de resultados como cola, y sigue sonando aunque cambies de pestaña.
- La **pantalla superior** muestra la carátula en grande (con el marcador de la nota hasta que llega).
- La **pantalla inferior** muestra el título, el artista y la posición en la cola («3 de 24»), los botones anterior, reproducir/pausa y siguiente, el tiempo, la barra de progreso (con lo descargado por delante) y la de volumen.

**Controles de reproducción:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — pausa/reanuda; con la pista terminada, la vuelve a empezar; con un error, reintenta
- Izquierda/derecha, o los botones de la pantalla — cambia de pista
- Arriba/abajo — ajusta el volumen local
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — vuelve a la pestaña Búsqueda sin cortar la música
- **START** — detiene y sale

La cola sigue el orden de los resultados y no envuelve en sus extremos: en ellos, el botón anterior o siguiente aparece deshabilitado. La reproducción completa, carátulas y controles aún deben validarse en una 3DS real. Los probes siguen disponibles como targets independientes.

### Ajustes

- **Idioma** — muestra el idioma actual y lo cambia (también con **SELECT** desde cualquier pestaña).
- **Volver a conectar** — vuelve a abrir la sesión con la credencial guardada.
- **Emparejar otra cuenta** — empieza el emparejamiento con un código nuevo.

Elige con arriba/abajo y **A**, o tocando el botón. Volver a conectar y emparejar detienen la reproducción. **B** vuelve a la pestaña Inicio.

## Probes

Los probes son pequeñas aplicaciones independientes que prueban, cada una por
separado, un paso del cliente. Se usaron para construirlo pieza a pieza y
siguen siendo útiles para diagnosticar problemas en una 3DS real. Ejecútalos
uno después de otro en el orden de la tabla: cada paso se apoya en los
anteriores (por ejemplo, los últimos probes usan el token que guarda
`probe_pair`), así que el primero que falle indica dónde está el problema.

| Target | Qué comprueba |
|---|---|
| `probe_tls` | Handshakes TLS 1.2 y una petición HTTPS contra los servidores de Spotify, con tiempos. Escribe `spotify-tls.log` junto al `.3dsx`. |
| `probe_pair` | El emparejamiento de la consola mediante spotify.com/pair y el guardado del token en la SD. |
| `probe_ap` | El inicio de sesión en el access point de Spotify y el mantenimiento de la sesión. |
| `probe_meta` | La cadena completa de inicio de sesión y la lectura de la biblioteca y los metadatos de una canción. |
| `probe_audio` | La decodificación de Ogg Vorbis y su reproducción por NDSP (lee `test.ogg`; sin red). |
| `probe_play` | Una canción de principio a fin: clave de audio, descarga, descifrado y reproducción. |

Compila uno con `--app`, por ejemplo:

```powershell
python source\build.py --app probe_tls --fetch-deps
```

El resultado queda en `source/dist/3ds/<target>/`; copia esa carpeta a
`SD:/3ds/<target>/` y ábrelo desde Homebrew Launcher.

## Licencia y créditos
- Licencia: Apache License 2.0 — ver el archivo `LICENSE` en la raíz del repositorio.
- Autor: David Abejón.
- Basado en [SpotiBacan](https://github.com/ohquebacan/spotibacan), de
  [ohquebacan](https://github.com/ohquebacan). El directorio
  `source/platform3ds/`, los directorios `source/probe_*/` y `source/build.py`
  proceden de ese proyecto y se han modificado para 3DSpotify. Ver `NOTICE`.
- Librerías incluidas: stbi (stb_image) en `include/stb_image.h` y demás dependencias fijadas por el build nativo.