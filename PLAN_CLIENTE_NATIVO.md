# Plan: cliente nativo de Spotify para Nintendo 3DS

## Objetivo

Convertir la aplicación antigua de `client/source` en un cliente que se conecte directamente a Spotify, busque una canción y la reproduzca en la propia Nintendo 3DS. La pantalla superior mostrará la carátula de la pista actual. La aplicación no dependerá del servidor proxy ni controlará la reproducción de otro dispositivo.

El plan reutiliza las rutas directas ya probadas en `probe_pair`, `probe_ap`, `probe_meta` y `probe_play`. No propone reimplementar OAuth, el protocolo de acceso, la descarga de audio ni la decodificación si ya existe un módulo que cumple esa función.

## Alcance de esta primera versión

Incluye:

- Emparejamiento y renovación de credenciales en la consola.
- Búsqueda de canciones y selección de un resultado.
- Reproducción local de la canción seleccionada.
- Pausa/reanudación, pista anterior/siguiente y control de volumen.
- Indicador de progreso y duración.
- Carátula de la pista actual en la pantalla superior.
- Manejo visible de errores y cancelación de operaciones.

Queda fuera de alcance: playlists y navegación por biblioteca, recomendaciones, letras, favoritos, gestión de dispositivos remotos, listas persistentes, reproducción aleatoria y repetición. La cola de esta versión se limita a los resultados de la búsqueda actual.

## Estado actual y piezas reutilizables

- `client/source/fetch.c` solicita JSON al proxy por HTTP sin TLS; sus funciones no deben ser la capa de red del cliente nuevo.
- `client/source/parse.c` extrae valores JSON mediante búsquedas de texto. La lógica nueva debe usar las estructuras de dominio que ya devuelven los módulos Spotify.
- `client/source/main.c` concentra el teclado, la pantalla inferior, el estado de reproducción remoto, las peticiones al proxy y la descarga de carátulas.
- `client/source/image_display.c` contiene el dibujo de la imagen en la pantalla superior, pero también inicialización y descarga de red. Se conservará el dibujo y se desacoplará de la red.
- `client/source/platform3ds/pair3ds.c`, `cfg3ds.c`, `ap3ds.c`, `login5.c`, `spclient.c`, `track.c`, `tls3ds.c` y `http3ds.c` contienen los módulos directos que ya usa el flujo de los probes.
- `sp_search_tracks` busca canciones y devuelve URIs; `sp_track_metadata_batch` obtiene nombres, artistas, duración y URLs de carátula.
- `track_stream_begin` obtiene metadatos, la ubicación del audio y la clave; requiere una sesión AP activa para solicitar esa clave. `track_stream_download` descarga y descifra el contenido, admite cancelación y reanudación.
- `probe_play/main.c` demuestra la cadena de reproducción y el uso de NDSP, pero actualmente emplea una pista fija y `track_fetch` con un límite de 1 MiB. No es todavía un reproductor interactivo de canción completa.
- El probe de audio local demuestra la decodificación Vorbis y la salida NDSP. Es una referencia para el llenado de buffers, no una capa de red.

## Fases

### Fase 0: fijar el target y sanear el build

**Propósito:** poder desarrollar y probar el cliente integrado sin compilar accidentalmente la aplicación del proxy ni depender de fuentes de terceros ausentes.

**Trabajo:**

1. Definir un único target de aplicación para el cliente nativo y documentar cómo se construye y se carga en la consola.
2. Revisar la lista de fuentes en `client/source/build.py`. Excluir `fetch.c` y `parse.c` del nuevo target; mantenerlos temporalmente solo si otro target aún los necesita.
3. Comprobar la unidad de implementación de `stb_image`: `main.c` antiguo define `STB_IMAGE_IMPLEMENTATION`, mientras el target también declara `deps/stb_image_impl.c`. Dejar una sola definición y eliminar del build la referencia a la fuente inexistente o crearla de forma intencional.
4. Confirmar que `stb_vorbis.c` se resuelve de manera reproducible con la descarga fijada y verificada añadida al build. No depender de una descarga sin hash ni de una copia local no documentada.
5. Mantener el compilado de mbedTLS y las opciones TLS alineados con el target nativo, no con el código HTTP antiguo.

**Entregable:** el target compila desde una copia limpia del repositorio con `--fetch-deps` y, una vez descargadas las dependencias, también sin esa opción.

**Criterio de aceptación:** no hay errores por archivos fuente ausentes ni símbolos de implementación duplicados; la salida se puede cargar mediante `3dslink` o desde Homebrew Launcher.

### Fase 1: arranque, identidad y sesión directa

**Estado:** implementada en `client/source/native`; verificada compilando el
target `spotify3ds`. La sesión puede iniciarse con credenciales guardadas o
mediante emparejamiento, y muestra errores recuperables para volver a intentar.

**Propósito:** reemplazar la configuración de IP del proxy por la identidad Spotify ya guardada en la SD.

**Trabajo:**

1. Inicializar los servicios de consola, red y TLS que necesite el cliente, con limpieza garantizada al salir.
2. Cargar el refresh token desde `cfg3ds`; si falta o ya no es válido, guiar al usuario por el flujo de emparejamiento de `pair3ds`.
3. Renovar el access token. Si Spotify rota el refresh token, guardar de forma segura el nuevo valor mediante la configuración existente.
4. Resolver y autenticar en el access point como en `probe_ap`, y obtener las credenciales reutilizables necesarias para `login5`.
5. Obtener un token de biblioteca con `login5_from_credentials` y resolver el host de `spclient`.
6. Modelar errores distintos para falta de Wi-Fi, fallo TLS, token caducado, rechazo de cuenta y fallo del access point. Mostrar un mensaje recuperable en vez de dejar la consola esperando sin contexto.
7. No imprimir tokens, claves de audio ni secretos en pantalla o logs.

**Entregable:** pantalla inicial que confirma sesión lista o permite emparejar y reintentar.

**Criterio de aceptación:** tras emparejar una vez, la siguiente ejecución renueva las credenciales sin intervención; ninguna operación requiere IP local ni servidor proxy.

### Fase 2: búsqueda y selección de canciones

**Estado:** implementada en `client/source/native`; verificada compilando el
target `spotify3ds` y ejecutando los tests de build. La búsqueda y los
metadatos se cargan en un worker, y la selección prepara una cola de resultados.

**Propósito:** sustituir la canción fija del probe por una búsqueda del usuario.

**Trabajo:**

1. Definir estados de interfaz explícitos: inicio/búsqueda, cargando, resultados, reproducción y error.
2. Invocar el teclado virtual de 3DS para escribir la consulta. Validar que no esté vacía y mantenerla para poder repetir o editar la búsqueda.
3. Ejecutar `sp_search_tracks` fuera del bucle de renderizado para que una petición TLS no congele la interfaz.
4. Para los URIs recibidos, obtener los metadatos con `sp_track_metadata_batch`, evitando una petición independiente por cada fila.
5. Presentar una lista paginada o desplazable con título, artista y duración; mostrar estado vacío, progreso, reintento y errores de red.
6. Al confirmar una fila, conservar el resultado completo como pista seleccionada y preparar la cola con los demás resultados en el orden mostrado.
7. No reutilizar `get()` de `parse.c` para interpretar la respuesta. Mantener los límites de longitud de los campos que ya define `sp_track`.

**Entregable:** búsqueda funcional y lista seleccionable sin reproducción.

**Criterio de aceptación:** se pueden buscar consultas con espacios y caracteres codificados correctamente; seleccionar una pista; manejar cero resultados, errores y listas mayores que la pantalla sin bloquear la navegación.

### Fase 3: motor de reproducción local

**Estado:** implementada en `client/source/native`; el target incluye Vorbis
pushdata y reproducción NDSP con descarga cancelable, watermark y liberación
ordenada. La compilación local no sustituye la validación de reproducción
completa en una 3DS real, que sigue pendiente.

**Controles provisionales:** `A` inicia la pista seleccionada; los controles
de reproduccion y la cola se describen en la fase 4.

**Propósito:** convertir la prueba de reproducción de una sola pista en un componente que pueda reproducir la selección del usuario hasta el final.

**Trabajo:**

1. Crear un controlador de reproducción con un único propietario del estado de la sesión AP, `track_stream`, decodificador Vorbis, buffers NDSP y recursos de pista.
2. Para cada canción, abrir una sesión AP, ejecutar `track_stream_begin` con el URI, y cerrar la sesión AP cuando la clave ya se haya obtenido. No mantener el socket AP abierto durante toda la reproducción si no es necesario.
3. Ejecutar `track_stream_download` en un worker cancelable y alimentar la decodificación incremental con los bytes ya descifrados. La interfaz y el bucle de NDSP deben seguir respondiendo mientras la red espera.
4. Adaptar el esquema de buffers de `probe_play`: rellenar los `ndspWaveBuf` disponibles, vaciar el último bloque al final de la pista y detectar errores y underruns.
5. Proteger los datos compartidos entre descarga y decodificación: solo leer hasta el watermark publicado por `track_stream`, detener y unir el worker antes de liberar memoria, y descartar resultados de una pista anterior después de un salto.
6. Aplicar límites explícitos a tamaño de pista, memoria y errores de formato. Si una canción no está disponible para la cuenta o no existe un formato Vorbis compatible, informar al usuario y permitir volver a resultados.
7. Mantener el access token y las claves fuera de estructuras de UI y de logs.

**Entregable:** una pista seleccionada se descarga, descifra y reproduce localmente hasta el final con el audio en NDSP.

**Criterio de aceptación:** se reproduce una pista completa en hardware real; la consola sigue respondiendo durante la descarga y no hay lectura fuera del watermark, uso de memoria después de liberar ni audio corrupto al llegar al final.

### Fase 4: controles y cola de reproducción

**Estado:** implementada en `client/source/native`; controles locales de
pausa, volumen y navegación por la cola, con comportamiento explícito en los
extremos. Se ha compilado el target nativo y pasado el conjunto de pruebas de
build; la validación de botones y audio en hardware real sigue pendiente.

**Controles implementados:** `A` pausa/reanuda; izquierda/derecha cambia a la
pista anterior/siguiente de los resultados; arriba/abajo ajusta el volumen
local; `B` detiene y vuelve a resultados; `START` detiene y sale. En los
extremos de la cola no se envuelve: se muestra un aviso.

**Optimización previa a fase 5:** el inicio de cada pista reutiliza los
metadatos ya cargados en la búsqueda, los access points resueltos al iniciar
sesión y el access token OAuth mientras siga vigente. Si falla la lista
guardada de access points, se vuelve a resolver antes de informar el error.
Esto evita solicitudes repetidas; el tiempo de red restante depende de la
conexión y de las respuestas de Spotify.

**Propósito:** conectar los botones a la reproducción local, no a comandos del servidor.

**Mapa inicial propuesto:**

- `A`: pausar o reanudar NDSP.
- Cruceta izquierda/derecha: pista anterior/siguiente de los resultados actuales.
- Cruceta arriba/abajo: subir/bajar volumen local en pasos definidos y limitado a 0-100 %.
- `B`: volver a resultados sin abandonar la sesión ni destruir credenciales.
- `START`: detener la pista, cancelar workers, liberar recursos y salir.

**Trabajo:**

1. Aplicar pausa/reanudación al canal NDSP y mantener coherentes el icono, el estado y el reloj de progreso.
2. Cambiar de pista cancelando primero la descarga activa, detener y limpiar los buffers, cerrar el decoder y solo entonces crear la nueva sesión de pista.
3. Definir explícitamente el comportamiento en los extremos de la cola: no avanzar, envolver o mostrar un aviso. Para la primera versión, se recomienda no envolver silenciosamente.
4. Cambiar el volumen mediante la mezcla de NDSP y reflejarlo visualmente; no hacer peticiones a `volume` del proxy.
5. Mostrar progreso y duración. El seek puede añadirse con `L/R` en pasos de 10 segundos solo tras validar que el reinicio del downloader y el reposicionamiento de Vorbis permanecen sincronizados; si no es fiable, se difiere y no se presenta como control disponible.
6. Debounce o procesar eventos de botón una sola vez por pulsación para evitar cambios repetidos accidentales.

**Entregable:** controles de transporte y volumen integrados con el estado del reproductor.

**Criterio de aceptación:** pausa, reanudación, volumen y cambio de pista funcionan durante una descarga o reproducción sin congelar el render ni dejar workers o sockets vivos. Si se implementa seek, la posición y el audio deben coincidir tras varios saltos.

### Fase 5: carátula en pantalla superior

**Estado:** implementada en `client/source/native`; descarga HTTPS limitada al
host `i.scdn.co`, decodificación stb_image en worker, fondo de reserva y
renderizado desde el hilo principal. La validación en hardware real sigue
pendiente.

**Propósito:** recuperar la carátula sin depender del proxy y sin desactivar la validación TLS.

**Trabajo:**

1. Obtener la URL grande de `sp_track.art_large` para la canción seleccionada o en reproducción.
2. Descargarla mediante `http_get_url` u otra ruta directa existente, con límite de bytes, validación del certificado y rechazo de hosts ajenos al CDN esperado (`i.scdn.co`). No reutilizar el `SSLCOPT_DisableVerify` de la descarga antigua.
3. Decodificar con stb_image fuera del bucle de dibujo, validando dimensiones, formato y asignación de memoria.
4. Conservar `drawImageToScreen` y el fondo de reserva de `image_display.c`, separándolos de `initNetwork`, `cleanupNetwork` y `downloadImage` antiguos.
5. Actualizar la carátula solo al cambiar de pista; liberar los píxeles anteriores una vez que ya no se dibujen. Si la imagen falta o falla, mostrar un fondo neutro sin interrumpir el audio.
6. Coordinar el render con el framebuffer desde el hilo principal; los workers de red solo descargan/decodifican y publican un resultado listo.

**Entregable:** carátula de la pista actual en la pantalla superior y controles/metadatos en la inferior.

**Criterio de aceptación:** se muestra una imagen correcta al seleccionar y al cambiar de pista; fallo o ausencia de la imagen no detiene la reproducción; ninguna petición de imagen desactiva TLS.

### Fase 6: integración, robustez y documentación

**Propósito:** retirar el comportamiento de mando remoto y validar el alcance completo en consola.

**Trabajo:**

1. Integrar autenticación, búsqueda, cola, audio, controles y carátula en el target de aplicación. Evitar que operaciones de red bloqueantes se ejecuten en el frame loop.
2. Eliminar del flujo nuevo `server_ip`, `/now-playing`, `/play`, `/pause`, `/next`, `/previous`, `/volume` y las dependencias de `fetch.c` y `parse.c`.
3. Centralizar cancelación y limpieza para salida, fallo de red, nueva búsqueda, siguiente/anterior y fallo de decodificación.
4. Revisar eventos de Wi-Fi perdido, respuesta 401/403/429, rechazo de clave, timeout, falta de memoria, canción no compatible y CDN caído. Presentar acciones recuperables.
5. Añadir pruebas focalizadas para codificación de búsqueda, transiciones de estado, cola y liberación/cancelación de recursos. Mantener las pruebas del build y de protocolos existentes.
6. Probar en hardware: emparejamiento nuevo y renovación, búsqueda, selección, reproducción completa, pausa/reanudación, anterior/siguiente, volumen, carátula y salida limpia. Confirmar también que la app sigue usable durante esperas de red.
7. Actualizar `README_es.md` y `README.md`: describir el cliente nativo, el emparejamiento, los controles y el build; mover las instrucciones del proxy a una sección histórica o eliminarlas del flujo principal una vez que el modo antiguo deje de ser compatible.

**Entregable:** cliente nativo reproducible desde el repositorio y guía de uso/build alineada con el producto.

**Criterio de aceptación final:** desde un arranque limpio se puede emparejar, buscar una canción, escucharla en la propia 3DS, usar los controles acordados y ver su carátula sin instalar, configurar ni ejecutar el servidor proxy.

## Riesgos y mitigaciones

- **Carga de red en el hilo de UI:** ejecutar búsqueda, login, carátula y descarga en workers; publicar resultados y errores al hilo principal mediante eventos/estado protegido.
- **Carreras al saltar de pista:** asociar cada operación a una generación de reproducción; cancelar y unir workers antes de liberar `track_stream` o cambiar el decoder.
- **Memoria limitada:** mantener límites de descarga, evitar copias redundantes de audio e imagen, y probar con pistas largas y tamaños máximos realistas.
- **Cuenta sin permiso para una pista:** tratar `AP_KEY_REFUSED` como fallo recuperable de esa pista; no confundirlo con una caída de TLS o del access point.
- **Cambios de servicios internos de Spotify:** conservar errores detallados sin exponer credenciales y aislar los protocolos en `platform3ds`, para que la UI no dependa de detalles protobuf.
- **Build legado mezclado con el nuevo:** hacer explícita la lista de fuentes del target final y verificarla desde checkout limpio, en vez de incluir indiscriminadamente archivos antiguos.

## Orden recomendado de ejecución

Completar las fases 0 y 1 antes de la UI, validar la fase 2 sin audio, y después integrar el motor de la fase 3. Añadir controles y carátulas como fases separables para depurar fallos de red, audio e imagen de forma independiente. La fase 6 cierra la integración y define la aceptación en hardware.
