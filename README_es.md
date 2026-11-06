# 3DSpotify

Cliente no oficial de Spotify para Nintendo 3DS. La aplicación se conecta
directamente a Spotify por HTTPS.

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
fija y verifica mbedTLS 3.6.7 y `stb_vorbis`; también genera la cabecera de
Vorbis desde esa misma fuente verificada. Tras descargar las dependencias, se
puede compilar sin `--fetch-deps`.

Las pruebas de build e integración se ejecutan desde la raíz con:

```powershell
python -m unittest discover -s tests
```

La prueba C de codificación, cola y cancelación se ejecuta si hay un compilador
C nativo disponible; en caso contrario, unittest la marca como omitida.

El artefacto queda en
`source\dist\3ds\spotify3ds\spotify3ds.3dsx`. Cópialo a
`SD:/3ds/spotify3ds/` y ejecútalo desde Homebrew Launcher, o envíalo desde el
PC con:

```powershell
3dslink.exe source\dist\3ds\spotify3ds\spotify3ds.3dsx -a <IP_DE_LA_CONSOLA>
```
`3dslink` forma parte de las herramientas de devkitPro.

Para instalarla en el menú HOME, genera también el CIA (requiere `makerom` y
`bannertool`, incluidos en las herramientas de devkitPro):

```powershell
python source\build.py --cia
```

Se genera `source\dist\3ds\spotify3ds\spotify3ds.cia`; cópialo a la SD e
instálalo con FBI o un instalador de CIA similar (requiere custom firmware).
El CIA guarda su configuración en `SD:/3ds/spotibacan/`.

El target inicia la sesión directa: renueva la credencial guardada, permite
emparejar si hace falta y comprueba la conexión al access point y a la sesión
de biblioteca. Si no hay una credencial guardada, sigue el código de
emparejamiento que aparece en la consola desde otro dispositivo y autoriza la
cuenta en Spotify. El refresh token se guarda en la SD para los siguientes
arranques. En la pantalla de inicio, toca «Buscar canciones» o pulsa A para
abrir el teclado. Debajo aparece tu última búsqueda como un botón que la
repite: tócalo, o llega a él con arriba/abajo y pulsa A. Un toque actúa al
levantar el lápiz dentro del botón; si lo deslizas fuera, se cancela. X vuelve
a conectar, Y permite emparejar otra cuenta, SELECT cambia el idioma y START
sale. En resultados, arriba/abajo
desplaza la lista, A selecciona, B vuelve y Y inicia otra consulta. B cancela
una búsqueda pendiente cuando termine la petición de red activa.

A en la pantalla de selección inicia la reproducción local. Durante la
reproducción, la pantalla superior muestra la carátula (o un fondo neutro si
no está disponible); la inferior muestra la pista y el progreso. A
pausa/reanuda, izquierda/derecha cambia de pista, arriba/abajo ajusta el
volumen local, B vuelve a resultados y START detiene y sale. La cola sigue el
orden de los resultados y no envuelve en sus extremos. La reproducción
completa, carátulas y controles aún deben validarse en una 3DS real. Los
probes siguen disponibles como targets independientes.

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