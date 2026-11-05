# Spotify 3DS

Cliente no oficial de Spotify para Nintendo 3DS. El cliente nativo se conecta
directamente a Spotify por HTTPS; el antiguo modo con proxy se conserva como
referencia histórica.

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Resumen
Este repositorio contiene:
- El cliente nativo para 3DS en [client/source/native](client/source/native), que empareja la cuenta desde la consola, busca canciones y reproduce audio local con carátula.
- El proxy FastAPI y el cliente antiguo en modo proxy, conservados como componentes heredados: [server/server.py](server/server.py) y [client/Makefile](client/Makefile).

## Requisitos
- Una Nintendo 3DS con Homebrew Launcher y conexión Wi-Fi.
- Para compilar desde código: devkitPro con devkitARM, libctru, citro2d, citro3d y 3dsxtool, además de Python 3.12 o posterior.
- No hace falta configurar ni ejecutar el proxy para usar el cliente nativo.

## Compilar el cliente nativo

Instala devkitPro con devkitARM, libctru, citro2d, citro3d y 3dsxtool, además
de Python 3.12 o posterior. Desde la raíz del repositorio:

```powershell
python client\source\build.py --fetch-deps
```

El target predeterminado es `spotify3ds`, separado de la aplicación antigua del
proxy, que conserva su `client\Makefile`. La descarga fija y verifica mbedTLS
3.6.7 y `stb_vorbis`; también genera la cabecera de Vorbis desde esa misma
fuente verificada. Tras descargar las dependencias, se puede compilar sin
`--fetch-deps`.

Las pruebas de build e integración se ejecutan desde la raíz con:

```powershell
python -m unittest discover -s client\tests
```

La prueba C de codificación, cola y cancelación se ejecuta si hay un compilador
C nativo disponible; en caso contrario, unittest la marca como omitida.

El artefacto queda en
`client\source\dist\3ds\spotify3ds\spotify3ds.3dsx`. Cópialo a
`SD:/3ds/spotify3ds/` y ejecútalo desde Homebrew Launcher, o envíalo desde el
PC con:

```powershell
C:\devkitPro\tools\bin\3dslink.exe client\source\dist\3ds\spotify3ds\spotify3ds.3dsx -a <IP_DE_LA_CONSOLA>
```

El target inicia la sesión directa: renueva la credencial guardada, permite
emparejar si hace falta y comprueba la conexión al access point y a la sesión
de biblioteca. Si no hay una credencial guardada, sigue el código de
emparejamiento que aparece en la consola desde otro dispositivo y autoriza la
cuenta en Spotify. El refresh token se guarda en la SD para los siguientes
arranques. En la pantalla de sesión, A abre la búsqueda, X vuelve a conectar,
Y permite emparejar otra cuenta y START sale. En resultados, arriba/abajo
desplaza la lista, A selecciona, B vuelve y Y inicia otra consulta. B cancela
una búsqueda pendiente cuando termine la petición de red activa.

A en la pantalla de selección inicia la reproducción local. Durante la
reproducción, la pantalla superior muestra la carátula (o un fondo neutro si
no está disponible); la inferior muestra la pista y el progreso. A
pausa/reanuda, izquierda/derecha cambia de pista, arriba/abajo ajusta el
volumen local, B vuelve a resultados y START detiene y sale. La cola sigue el
orden de los resultados y no envuelve en sus extremos. No hay control remoto
ni dependencia del servidor proxy. La reproducción completa, carátulas y
controles aún deben validarse en una 3DS real. Los probes siguen disponibles
como targets independientes; el target nativo no compila `fetch.c` ni
`parse.c` del flujo antiguo.

## Compilar el probe TLS nativo

Instala devkitPro con devkitARM, libctru, citro2d, citro3d y 3dsxtool, junto
con Python 3.12 o posterior. No necesita el toolchain de Melee, Clang ni CMake.
Desde la carpeta del repositorio:

```powershell
python client\source\build.py --app probe_tls --fetch-deps
```

La primera compilación descarga mbedTLS 3.6.7 y verifica su checksum SHA-256.
Después puedes omitir `--fetch-deps` y compilar sin conexión. La biblioteca
se compila con la misma configuración que la aplicación seleccionada.
Usa `--devkitpro "C:\devkitPro"` para indicar el SDK; por defecto comprueba
`DEVKITPRO` y después la ubicación de instalación habitual.
`--debug` genera una compilación sin optimizaciones.

Copia `client/source/dist/3ds/probe_tls/` a `SD:/3ds/probe_tls/` y abre
`probe_tls.3dsx` desde Homebrew Launcher. Los objetos y el ELF quedan en
`client/source/build/probe_tls/`. Los demás targets siguen necesitando sus
fuentes y las dependencias stb correspondientes.

El probe TLS activa una traza de sockets en `spotify-tls.log`, junto al ejecutable
(alternativa: `sdmc:/spotify-tls.log`). Las entradas `[socket]` registran la IPv4
elegida, resultados de llamadas, `errno` capturado al fallar, `SO_ERROR` SOC sin
traducir, eventos de poll, bytes solicitados y tiempos de las operaciones.
Tambien indican el inicio y final del handshake y si se ofrecio una sesion.
No incluyen contenido de mensajes, claves ni credenciales. `errno=0` indica que
la operacion no fallo; `recv rc=0` indica EOF. En libctru, `POLLOUT=0x10` y
`POLLHUP=0x04`. La traza introduce sobrecarga y se activa con `tls_set_trace()`;
las demas aplicaciones no la activan por defecto. Copia el log completo tras
la prueba, antes de volver a ejecutar el probe: cada ejecucion lo reemplaza.
Los eventos detallados no se imprimen en la pantalla de la consola.
La espera TCP usa intervalos de hasta 100 ms dentro del plazo compartido con DNS.
Tras cada intervalo se comprueba el peer, incluso sin eventos de poll.
`op=tcp_confirmed poll_rc=0` indica que se confirmo el peer sin notificacion
de disponibilidad; `total_ms` mide el tiempo desde el inicio de la conexion TCP.

## Cliente proxy heredado (histórico)

Esta sección describe el cliente antiguo. No es necesaria para compilar ni
utilizar `spotify3ds`; sus comandos, configuración y credenciales son
independientes del cliente nativo.

### Desplegar el servidor proxy
 - Puedes elegir desplegar el servidor con Docker (recomendado) o ejecutarlo directamente desde Python clonando el repositorio.
### 1.a) Servidor proxy con Docker
1. Descarga la release `spotify3ds-proxy.tar` desde las Releases del proyecto.
2. Carga la imagen Docker:
   ```sh
   docker load -i spotify3ds-proxy.tar
   ```
3. Ejecuta el contenedor:
   ```sh
   docker run -d --name spotify3ds-proxy -p 8000:8000 spotify3ds-proxy
   ```
4. Obtén el ID/estado del contenedor:
   ```sh
   docker ps
   ```
5. Entra al contenedor para editar la configuración:
   ```sh
   docker exec -it spotify3ds-proxy bash
   apt update && apt install nano -y
   nano spotify_config.json
   ```
   Rellena `client_id` y `client_secret` (consigue estos valores desde el dashboard de Spotify) y guarda.
6. Desde el host/terminal del contenedor, la primera vez el servidor intentará abrir la URL de autorización. Si no se abre, la verás en la consola. Ábrela en un navegador. Tras autorizar, el servidor almacenará el `code`/`refresh_token` en `spotify_config.json`.

### 1.b) Servidor proxy directamente en Python

1. Clona el repositorio y entra en la carpeta del servidor:
   ```sh
   git clone https://github.com/davidabejon/spotify-3ds.git
   cd spotify-3ds/server
   ```

2. Crea y activa un entorno virtual (Windows):
   ```ps
   python -m venv .venv
   .venv\Scripts\activate
   ```

3. Instala dependencias:
   ```ps
   pip install -r requirements.txt
   ```

4. Edita `spotify_config.json` y añade `client_id` y `client_secret` obtenidos desde el Dashboard de Spotify.

5. Ejecuta el servidor con Uvicorn (desde la carpeta `server`):
   ```ps
   uvicorn server:app --host 0.0.0.0 --port 8000 --reload --reload-include spotify_config.json
   ```
   - Si el servidor imprime un mensaje advirtiendo de que client_id y client_secret no han sido rellenados abre el archivo `spotify_config.json`

6. Desde el terminal, el servidor intentará abrir la URL de autorización. Si no se abre, la verás en la consola. Ábrela en un navegador. Tras autorizar, el servidor almacenará el `code`/`refresh_token` en `spotify_config.json`.

### Aplicación antigua en modo proxy
- Lo más sencillo: descarga `Spotify3DS.cia` desde las [Releases](https://github.com/davidabejon/spotify-3ds/releases), cópialo a la tarjeta SD e instálalo con FBI u otro instalador de CIA
- Alternativamente, descarga `spotify-3ds.zip` desde las [Releases](https://github.com/davidabejon/spotify-3ds/releases): extráelo en la carpeta `/3ds` de la tarjeta SD y ejecútalo desde el Homebrew Launcher.

Al iniciar la aplicación por primera vez, te pedirá la dirección IP del servidor proxy; tras introducirla intentará conectarse automáticamente.

Para cambiar la IP más tarde, pulsa Y para volver a abrir el teclado en pantalla y escribe la nueva dirección IP.

#### Controles
 - A — Reproducir / Pausar
 - Y — Abrir entrada de IP (teclado en pantalla)
 - Cruceta derecha — Siguiente canción
 - Cruceta izquierda — Anterior canción
 - Cruceta arriba — Subir volumen
 - Cruceta abajo — Bajar volumen
 - Botón START — Salir de la aplicación

## Licencia y créditos
- Licencia: Apache License 2.0 — ver el archivo `LICENSE` en la raíz del repositorio.
- Proyecto original / autor: David Abejón.
- Librerías incluidas: stbi (stb_image) en `client/include/stb_image.h` y demás dependencias listadas en el Docker image / requirements.