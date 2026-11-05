#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap3ds.h"
#include "cfg3ds.h"
#include "login5.h"
#include "pair3ds.h"
#include "spclient.h"
#include "tls3ds.h"

#define SOC_BUFFER 0x100000

typedef struct {
	const char *category;
	char detail[256];
} startup_error;

static void secure_clear(void *data, size_t len)
{
	volatile unsigned char *p = data;
	while (len--)
		*p++ = 0;
}

static void frame(void)
{
	gfxFlushBuffers();
	gfxSwapBuffers();
	gspWaitForVBlank();
}

static void show_progress(const char *message)
{
	consoleClear();
	printf("Spotify 3DS - iniciando sesion directa\n\n%s\n", message);
	frame();
}

static void show_pair_screen(const pair_request *req, int interval,
                             const char *status)
{
	consoleClear();
	printf("Spotify 3DS - emparejamiento\n\n");
	printf("Abre esta direccion en otro dispositivo:\n%s\n\n",
	       req->verification_uri);
	printf("Codigo: %s\n", req->user_code);
	if (req->verification_uri_complete[0])
		printf("\nEnlace directo:\n%s\n", req->verification_uri_complete);
	printf("\n%s\n", status);
	printf("Comprobando cada %d s. B cancela.\n", interval);
}

static bool pair_account(spotify_tokens *tokens, startup_error *error)
{
	pair_request req;
	char detail[192] = {0};
	show_progress("Solicitando un codigo de emparejamiento...");
	if (pair_begin(&req, detail, sizeof(detail)) != 0) {
		error->category = "Emparejamiento";
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&req, sizeof(req));
		return false;
	}

	int interval = req.interval_s > 0 ? req.interval_s : 5;
	time_t deadline = time(NULL) + req.expires_in_s;
	char status[192] = "Esperando autorizacion...";
	while (aptMainLoop()) {
		show_pair_screen(&req, interval, status);
		for (int i = 0; i < interval * 60; i++) {
			hidScanInput();
			if (hidKeysDown() & KEY_B) {
				error->category = "Emparejamiento cancelado";
				snprintf(error->detail, sizeof(error->detail),
				         "No se modificaron las credenciales guardadas.");
				secure_clear(&req, sizeof(req));
				return false;
			}
			frame();
			if (!aptMainLoop()) {
				secure_clear(&req, sizeof(req));
				return false;
			}
		}
		if (time(NULL) >= deadline) {
			error->category = "Codigo de emparejamiento caducado";
			snprintf(error->detail, sizeof(error->detail),
			         "Vuelve a intentarlo para obtener un codigo nuevo.");
			secure_clear(&req, sizeof(req));
			return false;
		}

		detail[0] = '\0';
		pair_status result = pair_poll(&req, tokens, detail, sizeof(detail));
		if (result == PAIR_PENDING) {
			snprintf(status, sizeof(status), "Aun esperando autorizacion.%s%s",
			         detail[0] ? " " : "", detail);
			continue;
		}
		if (result == PAIR_SLOW_DOWN) {
			interval += 5;
			snprintf(status, sizeof(status),
			         "Spotify pidio esperar mas; nuevo intervalo: %d s.",
			         interval);
			continue;
		}
		if (result == PAIR_OK) {
			if (!tokens->refresh_token[0] ||
			    !cfg_save_refresh_token(tokens->refresh_token, tokens->scope)) {
				error->category = "No se pudieron guardar las credenciales";
				snprintf(error->detail, sizeof(error->detail),
				         "Comprueba el espacio disponible en la tarjeta SD.");
				secure_clear(tokens, sizeof(*tokens));
				secure_clear(&req, sizeof(req));
				return false;
			}
			secure_clear(&req, sizeof(req));
			return true;
		}

		error->category = result == PAIR_DENIED
		                  ? "Emparejamiento rechazado"
		                  : result == PAIR_EXPIRED
		                    ? "Codigo de emparejamiento caducado"
		                    : "Fallo de emparejamiento";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "No se recibio una respuesta valida.");
		secure_clear(tokens, sizeof(*tokens));
		secure_clear(&req, sizeof(req));
		return false;
	}
	secure_clear(&req, sizeof(req));
	error->category = "Emparejamiento interrumpido";
	snprintf(error->detail, sizeof(error->detail),
	         "La aplicacion se esta cerrando.");
	return false;
}

static bool resolve_and_login(spotify_tokens *tokens, startup_error *error,
                              char *sp_host, size_t sp_hostlen)
{
	char detail[192] = {0};
	char device_id[64];
	ap_device_id(device_id, sizeof(device_id));

	ap_candidate candidates[AP_MAX_CANDIDATES];
	show_progress("Buscando los access points de Spotify...");
	int count = ap_resolve_list(candidates, AP_MAX_CANDIDATES,
	                            detail, sizeof(detail));
	if (count < 1) {
		error->category = "No se pudo resolver el access point";
		snprintf(error->detail, sizeof(error->detail),
		         "Comprueba Wi-Fi, DNS y TLS. %s", detail);
		secure_clear(tokens, sizeof(*tokens));
		return false;
	}

	ap_info info;
	memset(&info, 0, sizeof(info));
	int tried = 0;
	show_progress("Autenticando con el access point...");
	ap_session *ap = ap_login_any(candidates, count, tokens->access_token,
	                              device_id, &info, &tried,
	                              detail, sizeof(detail));
	secure_clear(tokens, sizeof(*tokens));
	if (!ap) {
		error->category = info.login_error
		                  ? "La cuenta fue rechazada por el access point"
		                  : "Fallo de conexion al access point";
		if (info.login_error)
			snprintf(error->detail, sizeof(error->detail), "%s (codigo %d).",
			         ap_login_error_text(info.login_error), info.login_error);
		else
			snprintf(error->detail, sizeof(error->detail), "%s",
			         detail[0] ? detail : "No respondio ningun access point.");
		secure_clear(&info, sizeof(info));
		return false;
	}

	login5_token library;
	show_progress("Abriendo la sesion de biblioteca...");
	int rc = login5_from_credentials(info.username, info.reusable,
	                                 info.reusable_len, device_id,
	                                 &library, detail, sizeof(detail));
	ap_close(ap);
	secure_clear(&info, sizeof(info));
	secure_clear(device_id, sizeof(device_id));
	if (rc != 0) {
		error->category = strstr(detail, "login5 refused:")
		                  ? "La cuenta rechazo el inicio de sesion"
		                  : "Fallo TLS o de inicio de sesion Spotify";
		snprintf(error->detail, sizeof(error->detail), "%s", detail);
		secure_clear(&library, sizeof(library));
		return false;
	}

	detail[0] = '\0';
	show_progress("Resolviendo el servidor Spotify...");
	rc = sp_resolve(sp_host, sp_hostlen, detail, sizeof(detail));
	secure_clear(&library, sizeof(library));
	if (rc != 0 || !sp_host[0]) {
		error->category = "No se pudo resolver el servidor Spotify";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "Respuesta de servidor no valida.");
		return false;
	}
	return true;
}

static bool start_session(bool force_pair, startup_error *error,
                          char *sp_host, size_t sp_hostlen)
{
	char stored[512] = {0};
	spotify_tokens tokens;
	memset(&tokens, 0, sizeof(tokens));

	if (force_pair || !cfg_load_refresh_token(stored, sizeof(stored))) {
		secure_clear(stored, sizeof(stored));
		if (!pair_account(&tokens, error))
			return false;
		return resolve_and_login(&tokens, error, sp_host, sp_hostlen);
	}

	char detail[192] = {0};
	show_progress("Renovando la credencial de Spotify...");
	if (token_refresh(stored, &tokens, detail, sizeof(detail)) != 0) {
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		if (strstr(detail, "invalid_grant")) {
			consoleClear();
			printf("La credencial guardada ha caducado.\n");
			printf("Se necesita emparejar de nuevo.\n");
			for (int i = 0; i < 90 && aptMainLoop(); i++)
				frame();
			if (!pair_account(&tokens, error))
				return false;
			return resolve_and_login(&tokens, error, sp_host, sp_hostlen);
		}
		error->category = "Fallo OAuth o TLS al renovar la credencial";
		snprintf(error->detail, sizeof(error->detail), "%s",
		         detail[0] ? detail : "No se recibio una respuesta valida.");
		return false;
	}

	if (tokens.refresh_token[0] &&
	    strcmp(tokens.refresh_token, stored) != 0 &&
	    !cfg_save_refresh_token(tokens.refresh_token, tokens.scope)) {
		error->category = "No se pudo guardar la credencial renovada";
		snprintf(error->detail, sizeof(error->detail),
		         "Comprueba el espacio disponible en la tarjeta SD.");
		secure_clear(stored, sizeof(stored));
		secure_clear(&tokens, sizeof(tokens));
		return false;
	}
	secure_clear(stored, sizeof(stored));
	return resolve_and_login(&tokens, error, sp_host, sp_hostlen);
}

static void show_error(const startup_error *error)
{
	consoleClear();
	printf("No se pudo iniciar la sesion\n\n");
	printf("%s\n\n", error->category ? error->category : "Error");
	printf("%s\n\n", error->detail);
	printf("A: reintentar   Y: emparejar   START: salir\n");
}

int main(int argc, char **argv)
{
	(void)argc;
	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);
	cfg_set_path((argv && argv[0]) ? argv[0] : NULL);

	u32 *soc_buffer = memalign(0x1000, SOC_BUFFER);
	bool soc_ready = false;
	bool ps_ready = false;
	bool tls_ready = false;
	startup_error error = {0};
	char sp_host[128] = {0};

	if (!soc_buffer || R_FAILED(socInit(soc_buffer, SOC_BUFFER))) {
		error.category = "No hay servicio de red";
		snprintf(error.detail, sizeof(error.detail),
		         "Comprueba que el Wi-Fi de la consola esta conectado.");
		goto failed;
	}
	soc_ready = true;
	if (R_FAILED(psInit())) {
		error.category = "No se pudo iniciar el servicio de seguridad";
		snprintf(error.detail, sizeof(error.detail),
		         "No se pudo habilitar la fuente de aleatoriedad de la consola.");
		goto failed;
	}
	ps_ready = true;

	{
		int rc = tls_global_init();
		if (rc != 0) {
			char reason[128];
			tls_strerror(rc, reason, sizeof(reason));
			error.category = "No se pudo inicializar TLS";
			snprintf(error.detail, sizeof(error.detail), "%s", reason);
			goto failed;
		}
	}
	tls_ready = true;

	bool force_pair = false;
	for (;;) {
		consoleClear();
		printf("Spotify 3DS - iniciando sesion directa\n\n");
		printf("Renovando credenciales y conectando...\n");
		frame();
		if (start_session(force_pair, &error, sp_host, sizeof(sp_host))) {
			consoleClear();
			printf("Sesion Spotify lista\n\n");
			printf("Servidor: %s\n\n", sp_host);
			printf("La sesion directa esta preparada.\n");
			printf("A: volver a comprobar   Y: emparejar\n");
			printf("START: salir\n");
			for (;;) {
				hidScanInput();
				u32 down = hidKeysDown();
				if (down & KEY_START)
					goto done;
				if (down & KEY_A) {
					force_pair = false;
					break;
				}
				if (down & KEY_Y) {
					force_pair = true;
					break;
				}
				frame();
				if (!aptMainLoop())
					goto done;
			}
		} else {
			show_error(&error);
			for (;;) {
				hidScanInput();
				u32 down = hidKeysDown();
				if (down & KEY_START)
					goto done;
				if (down & KEY_A) {
					force_pair = false;
					break;
				}
				if (down & KEY_Y) {
					force_pair = true;
					break;
				}
				frame();
				if (!aptMainLoop())
					goto done;
			}
		}
	}

failed:
	show_error(&error);
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			break;
		frame();
	}

done:
	if (tls_ready)
		tls_global_exit();
	if (ps_ready)
		psExit();
	if (soc_ready)
		socExit();
	free(soc_buffer);
	gfxExit();
	return 0;
}
