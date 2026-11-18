/* 3DSpotify — web del proyecto.
 * Textos en inglés y español (como las tablas de source/lang/) y el estado
 * pulsado de los botones (docs/STYLES.md §6.3). */
(function () {
	"use strict";

	/* ------------------------------------------------------------ */
	/* Textos. Cada clave existe en los dos idiomas; las *_html solo */
	/* llevan marcado propio (strong, code, a).                      */
	/* ------------------------------------------------------------ */

	var STR = {
		en: {
			page_title: "3DSpotify",
			page_desc: "Unofficial Spotify client for the Nintendo 3DS. Search for songs and play them on the console.",
			lang_name: "English",
			skip: "Skip to content",
			issue_title: "Known issue: some Premium accounts can’t play music",
			issue_text: "Some Spotify Premium accounts get <code>Audio key refused (0x0001)</code> on every song. <strong>This is a Spotify problem, not a 3DSpotify bug:</strong> Spotify’s servers refuse the audio key for those accounts, and other unofficial clients hit the same error (<a href=\"https://github.com/librespot-org/librespot/issues/1649\">librespot #1649</a>). There is no fix yet, but <strong>I am actively working on solving it, or at least on finding an alternative</strong>.",
			hero_lema: "Spotify on your Nintendo 3DS.",
			hero_frase: "Search for songs and play them on the console itself, with no PC in between, or download them to listen offline.",
			download: "Download",
			repo: "Source code",
			release_note: "Latest release on GitHub: .3dsx and .cia.",

			qr_title: "Install with FBI",
			qr_lead: "Scan this code from the console to install the latest .cia straight from the release.",
			qr_alt: "QR code with the download link of 3DSpotify.cia",
			qr_step1: "Open <strong>FBI</strong> on a console with custom firmware.",
			qr_step2: "Choose <strong>Remote Install</strong> → <strong>Scan QR Code</strong>.",
			qr_step3: "Point the camera at the code and confirm the install.",
			qr_link: "The code links to:",

			console_label: "Interactive mock-up of the app",
			console_hint: "Try it: tap the tabs, the songs and the buttons.",
			tab_home: "Home",
			tab_search: "Search",
			tab_playing: "Playing",
			tab_settings: "Settings",
			m_recent: "Recent",
			m_show_more: "Show more",
			m_open: "Open",
			m_liked: "Liked Songs",
			m_downloads: "Downloads",
			m_tracks_fmt: "%d tracks",
			m_last_fmt: "Last played: %s",
			m_dl_summary_fmt: "Songs: %d · Albums and playlists: %d",
			m_detail: "Detail",
			m_type: "Type",
			m_download: "Download",
			m_user_fmt: "User: %s",
			m_developer: "Developed by",
			m_based_on: "Based on",
			m_qr: "QR codes",
			m_license: "License",
			m_disclaimer: "Unofficial project, not affiliated with or endorsed by Spotify AB. Spotify is a trademark of Spotify AB.",
			m_lang_fmt: "Language: %s",
			m_reconnect: "Reconnect",
			m_pair: "Pair another account",
			m_position_fmt: "%d of %d",
			m_prev: "Previous",
			m_next: "Next",
			m_finished: "Track finished",
			m_volume: "Volume",
			m_play: "Play",
			m_pause: "Pause",
			m_back: "Back",
			m_search: "Search",
			m_choose: "Choose",

			features_title: "Features",
			f3_title: "Plays on the console",
			f3_text: "Audio is decoded and played locally. The queue plays on by itself, and a single song carries on into a radio of similar songs. Shuffle any list, drag to seek and to set the volume.",
			f7_title: "Downloads and offline",
			f7_text: "Download songs, albums and playlists in the background and play them with no connection. The app even starts offline.",
			f8_title: "Your playlists",
			f8_text: "Home shows your recently played playlists and your Liked Songs, with a preview of each one on the top screen.",
			f9_title: "Songs, albums and artists",
			f9_text: "Open a page for any song, album or artist. Artists show their popular tracks, discography, monthly listeners and biography (in English only).",
			f5_title: "Sign in without typing passwords",
			f5_text: "Link your account with a short code on spotify.com/pair or by scanning a QR code with your phone.",
			f6_title: "English and Spanish",
			f6_text: "Switch the language at any moment with SELECT, on any screen. Korean and Chinese titles show too.",
			req_title: "You will need",
			req_text: "A Spotify Premium account and a Nintendo 3DS with Wi-Fi and the Homebrew Launcher. Installing the .cia also requires custom firmware.",

			controls_title: "Controls",
			col_button: "Button",
			col_action: "What it does",
			c_a: "Confirm: open the keyboard, play the highlighted song, pause or resume.",
			c_b: "Go back, or cancel a pending search or pairing.",
			c_x: "Open the highlighted song's page; in the player, download the track; in the downloads manager, remove a download.",
			c_y: "New search from Home or the results; shuffle on or off in a list; pause or resume the downloads; pair again after a sign-in error.",
			dpad: "D-Pad",
			c_dpad: "Move the focus; hold up or down to keep moving. In results, left/right switch between songs, artists, albums and playlists; in the player, left/right change track and up/down set the volume.",
			c_lr: "Previous or next tab (ZL/ZR too on New 3DS).",
			c_select: "Switch between English and Spanish.",
			c_start: "Exit the app.",
			touch: "Touch",
			c_touch: "Tap tabs, buttons and songs; the action runs when you lift the stylus inside. Drag lists to scroll, the progress bar to seek and the volume bar to set it.",

			install_title: "Installation",
			install_lead: "Pick one of the two formats from the latest release.",
			m3dsx_tag: "Homebrew Launcher",
			m3dsx_1: "Download <code>3DSpotify.3dsx</code>.",
			m3dsx_2: "Copy it to <code>SD:/3ds/3DSpotify/</code>.",
			m3dsx_3: "Open it from the <strong>Homebrew Launcher</strong>.",
			get_3dsx: "Download .3dsx",
			mcia_tag: "FBI · HOME Menu",
			mcia_1: "Scan the <a href=\"#qr\">QR code</a> from FBI, or download <code>3DSpotify.cia</code> and copy it to the SD card.",
			mcia_2: "Install it with <strong>FBI</strong> (requires custom firmware).",
			mcia_3: "Open it from the <strong>HOME Menu</strong>. Settings are kept in <code>SD:/3ds/3DSpotify/</code>.",
			get_cia: "Download .cia",
			first_title: "First launch",
			first_1: "The console shows a code on the top screen. Open <strong>spotify.com/pair</strong> on your phone or computer, sign in, enter the code and accept, or scan the QR code on the bottom screen.",
			first_2: "The credential is saved to the SD card, so later launches connect on their own.",

			foot_disclaimer: "<strong>3DSpotify is an unofficial, independent project.</strong> It is not affiliated with, endorsed by or associated with Spotify AB. Spotify is a trademark of Spotify AB, named here only to describe the service the app connects to.",
			foot_license: "Released under the <a href=\"https://github.com/davidabejon/3DSpotify/blob/main/LICENSE\">Apache License 2.0</a>. Based on <a href=\"https://github.com/ohquebacan/spotibacan\">SpotiBacan</a> by ohquebacan.",
			foot_author: "Made by David Abejón · <a href=\"https://github.com/davidabejon/3DSpotify\">GitHub</a>"
		},

		es: {
			page_title: "3DSpotify",
			page_desc: "Cliente no oficial de Spotify para Nintendo 3DS. Busca canciones y escúchalas en la consola.",
			lang_name: "Español",
			skip: "Saltar al contenido",
			issue_title: "Problema conocido: algunas cuentas Premium no pueden reproducir música",
			issue_text: "Algunas cuentas de Spotify Premium reciben <code>Audio key refused (0x0001)</code> en todas las canciones. <strong>Es un problema de Spotify, no un fallo de 3DSpotify:</strong> los servidores de Spotify deniegan la clave de audio a esas cuentas, y otros clientes no oficiales sufren el mismo error (<a href=\"https://github.com/librespot-org/librespot/issues/1649\">librespot #1649</a>). Todavía no hay solución, pero <strong>estoy trabajando activamente para solucionarlo o, al menos, encontrar una alternativa</strong>.",
			hero_lema: "Spotify en tu Nintendo 3DS.",
			hero_frase: "Busca canciones y escúchalas en la propia consola, sin un PC de por medio, o descárgalas para escucharlas sin conexión.",
			download: "Descargar",
			repo: "Código fuente",
			release_note: "Última versión en GitHub: .3dsx y .cia.",

			qr_title: "Instalar con FBI",
			qr_lead: "Escanea este código desde la consola para instalar el último .cia directamente desde la versión publicada.",
			qr_alt: "Código QR con el enlace de descarga de 3DSpotify.cia",
			qr_step1: "Abre <strong>FBI</strong> en una consola con firmware personalizado.",
			qr_step2: "Elige <strong>Remote Install</strong> → <strong>Scan QR Code</strong>.",
			qr_step3: "Apunta la cámara al código y confirma la instalación.",
			qr_link: "El código enlaza a:",

			console_label: "Maqueta interactiva de la app",
			console_hint: "Pruébala: toca las pestañas, las canciones y los botones.",
			tab_home: "Inicio",
			tab_search: "Búsqueda",
			tab_playing: "Reproducción",
			tab_settings: "Ajustes",
			m_recent: "Recientes",
			m_show_more: "Mostrar más",
			m_open: "Abrir",
			m_liked: "Canciones que te gustan",
			m_downloads: "Descargas",
			m_tracks_fmt: "%d canciones",
			m_last_fmt: "Lo último: %s",
			m_dl_summary_fmt: "Canciones: %d · Álbumes y playlists: %d",
			m_detail: "Detalle",
			m_type: "Tipo",
			m_download: "Descargar",
			m_user_fmt: "Usuario: %s",
			m_developer: "Desarrollo",
			m_based_on: "Basado en",
			m_qr: "Códigos QR",
			m_license: "Licencia",
			m_disclaimer: "Proyecto no oficial, sin relación con Spotify AB ni respaldado por ella. Spotify es una marca de Spotify AB.",
			m_lang_fmt: "Idioma: %s",
			m_reconnect: "Volver a conectar",
			m_pair: "Emparejar otra cuenta",
			m_position_fmt: "%d de %d",
			m_prev: "Anterior",
			m_next: "Siguiente",
			m_finished: "Pista terminada",
			m_volume: "Volumen",
			m_play: "Reproducir",
			m_pause: "Pausa",
			m_back: "Volver",
			m_search: "Buscar",
			m_choose: "Elegir",

			features_title: "Características",
			f3_title: "Suena en la consola",
			f3_text: "El audio se decodifica y se reproduce en la propia consola. La cola sigue sola, y una canción suelta continúa en una radio de canciones parecidas. Mezcla cualquier lista y arrastra para avanzar y para ajustar el volumen.",
			f7_title: "Descargas y sin conexión",
			f7_text: "Descarga canciones, álbumes y playlists en segundo plano y escúchalos sin conexión. La app incluso arranca sin conexión.",
			f8_title: "Tus playlists",
			f8_text: "Inicio muestra las playlists que has escuchado últimamente y tus Canciones que te gustan, con una vista previa de cada una en la pantalla superior.",
			f9_title: "Canciones, álbumes y artistas",
			f9_text: "Abre la página de cualquier canción, álbum o artista. Los artistas muestran sus canciones populares, su discografía, sus oyentes mensuales y su biografía (solo en inglés).",
			f5_title: "Inicia sesión sin escribir contraseñas",
			f5_text: "Vincula tu cuenta con un código corto en spotify.com/pair o escaneando un código QR con el móvil.",
			f6_title: "Inglés y español",
			f6_text: "Cambia de idioma cuando quieras con SELECT, en cualquier pantalla. Los títulos en coreano y chino también se ven.",
			req_title: "Necesitarás",
			req_text: "Una cuenta Spotify Premium y una Nintendo 3DS con Wi-Fi y Homebrew Launcher. Para instalar el .cia hace falta además firmware personalizado.",

			controls_title: "Controles",
			col_button: "Botón",
			col_action: "Qué hace",
			c_a: "Confirmar: abre el teclado, reproduce la canción resaltada, pausa o reanuda.",
			c_b: "Volver, o cancelar una búsqueda o un emparejamiento pendiente.",
			c_x: "Abre la página de la canción resaltada; en el reproductor, descarga la pista; en el gestor de descargas, borra una descarga.",
			c_y: "Nueva búsqueda desde Inicio o los resultados; activa o desactiva el modo aleatorio en una lista; pausa o reanuda las descargas; emparejar de nuevo tras un error de sesión.",
			dpad: "Cruceta",
			c_dpad: "Mueve el foco; mantén arriba o abajo para seguir avanzando. En los resultados, izquierda/derecha cambian entre canciones, artistas, álbumes y playlists; en el reproductor, izquierda/derecha cambian de pista y arriba/abajo ajustan el volumen.",
			c_lr: "Pestaña anterior o siguiente (también ZL/ZR en New 3DS).",
			c_select: "Cambia entre inglés y español.",
			c_start: "Sale de la app.",
			touch: "Táctil",
			c_touch: "Toca pestañas, botones y canciones; la acción se ejecuta al levantar el lápiz dentro. Arrastra las listas para desplazarlas, la barra de progreso para avanzar y la de volumen para ajustarlo.",

			install_title: "Instalación",
			install_lead: "Elige uno de los dos formatos de la última versión.",
			m3dsx_tag: "Homebrew Launcher",
			m3dsx_1: "Descarga <code>3DSpotify.3dsx</code>.",
			m3dsx_2: "Cópialo en <code>SD:/3ds/3DSpotify/</code>.",
			m3dsx_3: "Ábrelo desde el <strong>Homebrew Launcher</strong>.",
			get_3dsx: "Descargar .3dsx",
			mcia_tag: "FBI · Menú HOME",
			mcia_1: "Escanea el <a href=\"#qr\">código QR</a> desde FBI, o descarga <code>3DSpotify.cia</code> y cópialo a la tarjeta SD.",
			mcia_2: "Instálalo con <strong>FBI</strong> (requiere firmware personalizado).",
			mcia_3: "Ábrelo desde el <strong>menú HOME</strong>. Los ajustes se guardan en <code>SD:/3ds/3DSpotify/</code>.",
			get_cia: "Descargar .cia",
			first_title: "Primer arranque",
			first_1: "La consola muestra un código en la pantalla superior. Abre <strong>spotify.com/pair</strong> en el móvil o el ordenador, inicia sesión, escribe el código y acepta, o escanea el código QR de la pantalla inferior.",
			first_2: "La credencial se guarda en la tarjeta SD, así que los siguientes arranques se conectan solos.",

			foot_disclaimer: "<strong>3DSpotify es un proyecto no oficial e independiente.</strong> No está afiliado, respaldado ni asociado con Spotify AB. Spotify es una marca registrada de Spotify AB y aquí solo se nombra para describir el servicio al que se conecta la app.",
			foot_license: "Publicado bajo la <a href=\"https://github.com/davidabejon/3DSpotify/blob/main/LICENSE\">licencia Apache 2.0</a>. Basado en <a href=\"https://github.com/ohquebacan/spotibacan\">SpotiBacan</a>, de ohquebacan.",
			foot_author: "Hecho por David Abejón · <a href=\"https://github.com/davidabejon/3DSpotify\">GitHub</a>"
		}
	};

	var LANGS = ["en", "es"];
	var STORAGE_KEY = "3dspotify-lang";

	function tr(lang, key) {
		var s = STR[lang][key];
		return s === undefined ? STR.en[key] : s;
	}

	function each(sel, fn) {
		Array.prototype.forEach.call(document.querySelectorAll(sel), fn);
	}

	function apply(lang) {
		document.documentElement.lang = lang;
		document.title = tr(lang, "page_title");
		var desc = document.querySelector('meta[name="description"]');
		if (desc) desc.setAttribute("content", tr(lang, "page_desc"));

		each("[data-i18n]", function (el) {
			el.textContent = tr(lang, el.getAttribute("data-i18n"));
		});
		each("[data-i18n-html]", function (el) {
			el.innerHTML = tr(lang, el.getAttribute("data-i18n-html"));
		});
		each("[data-i18n-alt]", function (el) {
			el.setAttribute("alt", tr(lang, el.getAttribute("data-i18n-alt")));
		});
		each("[data-i18n-aria]", function (el) {
			el.setAttribute("aria-label", tr(lang, el.getAttribute("data-i18n-aria")));
		});

		/* El botón ofrece el otro idioma, escrito en ese idioma. */
		var other = lang === "en" ? "es" : "en";
		var btn = document.getElementById("idioma");
		var name = document.getElementById("idioma-nombre");
		if (btn && name) {
			name.textContent = tr(other, "lang_name");
			btn.setAttribute("lang", other);
		}
	}

	function stored() {
		try {
			var v = window.localStorage.getItem(STORAGE_KEY);
			return LANGS.indexOf(v) >= 0 ? v : null;
		} catch (e) {
			return null;
		}
	}

	function store(lang) {
		try {
			window.localStorage.setItem(STORAGE_KEY, lang);
		} catch (e) {
			/* Sin almacenamiento: el idioma dura lo que la página. */
		}
	}

	function initial() {
		/* Inglés por defecto; solo cambia si el visitante eligió otro idioma. */
		return stored() || "en";
	}

	var current = initial();

	function setLang(lang) {
		current = lang;
		apply(current);
		store(current);
		mock.render();
	}

	function toggleLang() {
		setLang(current === "en" ? "es" : "en");
	}

	/* ------------------------------------------------------------ */
	/* Maqueta interactiva de la app (§7.4, §7.5, §7.7, §7.8).       */
	/* Se dibuja entera en píxeles de consola (--u). Los cambios de   */
	/* estado van en un solo frame, sin transiciones (§8).            */
	/* ------------------------------------------------------------ */

	/* Canciones de ejemplo, inventadas; las carátulas son colores
	 * planos del tema: fondo, franja, bloque oscuro, bloque claro. */
	var TRACKS = [
		{ t: "Midnight Drive", a: "The Velvet Hours", al: "Late Summer", d: 222, c: ["ambar", "coral", "tinta", "clorofila"] },
		{ t: "Golden Fields", a: "Ivy Lane", al: "Open Windows", d: 185, c: ["btn-x", "tinta", "lima", "coral"] },
		{ t: "Paper Boats", a: "North Harbor", al: "Late Summer", d: 251, c: ["coral", "ambar", "tinta", "menta"] },
		{ t: "Neon Rain", a: "Static Bloom", al: "City Lights", d: 178, c: ["lima", "musgo", "tinta", "texto"] },
		{ t: "Slow Motion", a: "Ivy Lane", al: "Open Windows", d: 230, c: ["menta", "btn-x", "tinta", "ambar"] },
		{ t: "Back Home", a: "North Harbor", al: "City Lights", d: 207, c: ["texto-2", "tinta", "coral", "clorofila"] }
	];
	/* Inicio (§7.7): las dos teselas fijas y cuatro playlists recientes. */
	var PLAYLISTS = [
		{ key: "m_liked", ic: "heart", n: 248, last: 0 },
		{ key: "m_downloads", ic: "download" },
		{ name: "Road Trip", c: TRACKS[0].c, n: 42, last: 1 },
		{ name: "Rainy Days", c: TRACKS[3].c, n: 21, last: 5 },
		{ name: "Morning Run", c: TRACKS[1].c, n: 58, last: 2 },
		{ name: "Deep Focus", c: TRACKS[4].c, n: 30, last: 3 }
	];
	/* Lo que hay en la SD en la vista previa de «Descargas». */
	var DL_SONGS = 14, DL_COLLECTIONS = 2;
	/* Franja de tipos (§5.17): icono y recuento; las canciones, activas. */
	var KINDS = [["note", TRACKS.length], ["user", 3], ["album", 4], ["playlist", 5]];
	var KIND_X = [12, 107, 202, 297];
	var TABS = ["home", "search", "playing", "settings"];
	var TAB_ICON = { home: "home", search: "search", playing: "playing", settings: "settings" };
	var ROW_STEP = 45;      /* fila de 41 px + 4 px de separación */
	var ROWS_VISIBLE = 4;
	var LIST_H = 184;
	var DL_STEP = 25;       /* % que avanza una descarga por segundo */

	var reduceMotion = window.matchMedia &&
		window.matchMedia("(prefers-reduced-motion: reduce)").matches;

	var st = {
		tab: "playing",
		track: 2,
		pos: 84,
		playing: !reduceMotion,
		finished: false,
		focus: 2,
		scroll: 0,
		home: 2,
		/* Descargas por pista: porcentaje, 100 = en la SD. */
		dl: { 0: 100, 3: 100 }
	};

	function T(key) { return tr(current, key); }

	function fmt(s, a, b) {
		return s.replace("%d", a).replace("%d", b).replace("%s", a);
	}

	function mmss(sec) {
		var m = Math.floor(sec / 60), s = sec % 60;
		return m + ":" + (s < 10 ? "0" : "") + s;
	}

	function px(v) { return "calc(" + v + " * var(--u))"; }

	function icon(name) {
		return '<svg class="icon" aria-hidden="true"><use href="#i-' + name + '"/></svg>';
	}

	function cover(tr_, cls) {
		var c = tr_.c;
		return '<span class="m-cover ' + cls + '" aria-hidden="true" style="' +
			"--c1:var(--" + c[0] + ");--c2:var(--" + c[1] + ");--c3:var(--" + c[2] + ");--c4:var(--" + c[3] + ')">' +
			"<i></i><i></i><i></i></span>";
	}

	/* Sin portada: cuadrado `capa_alta` con el icono centrado (§5.3). */
	function art(item, cls, ic) {
		if (!item.c) {
			return '<span class="m-cover m-cover--icono ' + cls + '" aria-hidden="true">' +
				icon(ic || item.ic) + "</span>";
		}
		return cover(item, cls);
	}

	function bar(cls) {
		return '<div class="m-abs ' + cls + '">' +
			'<div class="m-tiempos m-meta" data-p="tiempos"></div>' +
			'<div class="m-barra">' +
			'<span class="m-barra__descarga" data-p="dl"></span>' +
			'<span class="m-barra__progreso" data-p="pr"></span>' +
			'<span class="m-barra__tirador" data-p="th"></span>' +
			"</div></div>";
	}

	/* Barra de botones (§5.6). "LR" es el glifo de la cruceta, pegado a
	 * la derecha para no cambiar de sitio. */
	function hints(list) {
		return '<div class="m-abs m-barra-botones" aria-hidden="true">' + list.map(function (h) {
			var lr = h[0] === "LR";
			return '<span class="m-accion' + (lr ? " m-accion--der" : "") + '">' +
				'<span class="m-glifo m-glifo--' + (lr ? "sys" : h[0].toLowerCase()) + '">' +
				(lr ? icon("dpad-lr") : h[0]) + '</span><span class="m-meta">' + h[1] + "</span></span>";
		}).join("") + "</div>";
	}

	/* Ficha de la pantalla superior (§7.12): portada 160×160 y los textos
	 * centrados junto a ella. `low` la baja bajo la franja de tipos. */
	function card(artHtml, title, lines, low) {
		return artHtml +
			'<div class="m-abs m-ficha' + (low ? " m-ficha--baja" : "") + '">' +
			'<span class="m-titulo m-linea">' + title + "</span>" +
			lines.map(function (l) {
				return '<span class="m-cuerpo m-linea">' + l + "</span>";
			}).join("") + "</div>";
	}

	function renderStrip() {
		return KINDS.map(function (k, i) {
			return '<span class="m-abs m-tipo' + (i === 0 ? " m-tipo--activo" : "") +
				'" style="left:' + px(KIND_X[i]) + '">' + icon(k[0]) +
				'<span class="m-meta">' + k[1] + "</span></span>";
		}).join("");
	}

	/* Ajustes (§7.8): la cuenta y los créditos sobre el logotipo oscurecido. */
	function renderAbout() {
		var rows = [
			["m_developer", "David Abejón"],
			["m_based_on", "SpotiBacan (ohquebacan)"],
			["m_qr", "QR Code generator, Project Nayuki (MIT)"],
			["m_license", "Apache License 2.0"]
		];
		return '<img class="m-abs m-logo" src="assets/logo.png" alt="">' +
			'<div class="m-abs m-velo"></div>' +
			'<div class="m-abs m-cuenta">' +
			'<span class="m-cuenta__foto">' + icon("user") + "</span>" +
			'<span class="m-cuenta__texto">' +
			'<span class="m-cabeza m-linea">Alex</span>' +
			'<span class="m-cuerpo m-linea">Premium - ES</span>' +
			'<span class="m-meta m-linea">' + fmt(T("m_user_fmt"), "alex_3ds") + "</span>" +
			"</span></div>" +
			rows.map(function (r, i) {
				return '<div class="m-abs m-credito" style="top:' + px(96 + i * 20) + '">' +
					'<span class="m-meta">' + T(r[0]) + "</span>" +
					'<span class="m-cuerpo m-linea">' + r[1] + "</span></div>";
			}).join("") +
			'<div class="m-abs m-regla"></div>' +
			'<p class="m-abs m-marca m-meta">' + T("m_disclaimer") + "</p>";
	}

	function renderTop() {
		var t;
		switch (st.tab) {
		case "playing":
			/* Reproducción: la carátula grande. */
			return cover(TRACKS[st.track], "m-cover--grande");
		case "search":
			/* Franja de tipos y la ficha del resultado en foco. */
			t = TRACKS[st.focus];
			return renderStrip() +
				card(cover(t, "m-cover--ficha m-cover--baja"), t.t, [t.a, t.al, mmss(t.d)], true);
		case "home":
			/* Vista previa de la tesela en foco. */
			var p = PLAYLISTS[st.home];
			if (p.key === "m_downloads") {
				return card(art(p, "m-cover--ficha"), T(p.key),
					[fmt(T("m_dl_summary_fmt"), DL_SONGS, DL_COLLECTIONS)]);
			}
			var last = TRACKS[p.last];
			return card(art(p, "m-cover--ficha", "note"), p.key ? T(p.key) : p.name,
				[fmt(T("m_tracks_fmt"), p.n), fmt(T("m_last_fmt"), last.t + " - " + last.a)]);
		default:
			return renderAbout();
		}
	}

	function renderTabs() {
		return '<div class="m-abs m-pestanas">' + TABS.map(function (tab) {
			var on = st.tab === tab;
			return '<button type="button" class="m-pestana' + (on ? " m-pestana--activa" : "") +
				'" data-ctl="tab:' + tab + '" aria-label="' + T("tab_" + tab) + '"' +
				(on ? ' aria-current="page"' : "") + ">" + icon(TAB_ICON[tab]) + "</button>";
		}).join("") + "</div>";
	}

	function renderHome() {
		/* Rejilla de 6 (§7.7, §5.16): 2 columnas de 150 px, filas de 32 px a 4 px. */
		var tiles = PLAYLISTS.map(function (p, i) {
			var foco = i === st.home;
			var name = p.key ? T(p.key) : p.name;
			return '<button type="button" class="m-fila-res m-tesela' + (foco ? " m-fila-res--foco" : "") +
				'" style="left:' + px(i % 2 ? 162 : 8) + ";top:" + px(56 + Math.floor(i / 2) * 36) +
				'" data-ctl="tile:' + i + '"' + (foco ? ' aria-current="true"' : "") +
				' aria-label="' + name + '">' + art(p, "m-cover--tesela") +
				'<span class="m-tesela__nombre m-cuerpo m-linea">' + name + "</span></button>";
		}).join("");
		return '<span class="m-abs m-meta" style="left:' + px(8) + ";top:" + px(38) + '">' + T("m_recent") + "</span>" +
			tiles +
			'<button type="button" class="m-abs m-boton m-ancho m-centro m-cuerpo" style="top:' + px(176) +
			'" data-ctl="more"><span>' + T("m_show_more") + "</span></button>" +
			hints([["A", T("m_open")], ["Y", T("m_search")]]);
	}

	function renderSearch() {
		var n = TRACKS.length;
		var rows = TRACKS.map(function (t, i) {
			var y = 4 + (i - st.scroll) * ROW_STEP;
			var foco = i === st.focus;
			return '<button type="button" class="m-fila-res' + (foco ? " m-fila-res--foco" : "") +
				'" style="top:' + px(y) + '" data-ctl="row:' + i + '"' +
				(foco ? ' aria-current="true"' : "") +
				' aria-label="' + t.t + ", " + t.a + '">' +
				cover(t, "m-cover--mini") +
				'<span class="m-fila-res__titulo m-cabeza m-linea">' + t.t + "</span>" +
				'<span class="m-fila-res__artista m-cuerpo m-linea">' + t.a + "</span>" +
				'<span class="m-fila-res__dur m-meta">' + mmss(t.d) + "</span>" +
				"</button>";
		}).join("");
		/* Indicador de desplazamiento (§5.11): alto proporcional, mínimo 16 px. */
		var h = Math.max(16, Math.round(LIST_H * ROWS_VISIBLE / n));
		var top = Math.round((LIST_H - h) * st.scroll / (n - ROWS_VISIBLE));
		return '<div class="m-abs m-lista">' + rows + "</div>" +
			'<div class="m-abs m-scroll" aria-hidden="true"><i style="top:' + px(top) +
			";height:" + px(h) + '"></i></div>' +
			hints([["A", T("m_play")], ["X", T("m_detail")], ["Y", T("m_search")], ["LR", T("m_type")]]);
	}

	/* Icono de descarga del reproductor (§7.5): flecha, porcentaje o check. */
	function renderDownload() {
		var s = st.dl[st.track];
		if (s === undefined) {
			return '<button type="button" class="m-abs m-descarga" data-ctl="dl" aria-label="' +
				T("m_download") + '">' + icon("download") + "</button>";
		}
		if (s >= 100) {
			return '<span class="m-abs m-descarga m-descarga--hecha">' + icon("downloaded") + "</span>";
		}
		return '<span class="m-abs m-descarga m-meta">' + s + "%</span>";
	}

	function renderPlaying() {
		var t = TRACKS[st.track];
		var first = st.track === 0, last = st.track === TRACKS.length - 1;
		var showPause = st.playing;
		var arrow = st.dl[st.track] === undefined;
		function skip(ctl, ic, label, off) {
			return '<button type="button" class="m-boton m-sq40" data-ctl="' + ctl + '" aria-label="' + T(label) + '"' +
				(off ? ' aria-disabled="true"' : "") + ">" + icon(ic) + "</button>";
		}
		var keys = [["A", T(showPause ? "m_pause" : "m_play")]];
		if (arrow) keys.push(["X", T("m_download")]);
		keys.push(["B", T("m_back")]);
		/* Filas de 32 px (§7.5): título y posición; artista, álbum y descarga. */
		return '<div class="m-abs m-rep-titulo">' +
			'<span class="m-cabeza m-linea">' + t.t + "</span>" +
			'<span class="m-meta">' + fmt(T("m_position_fmt"), st.track + 1, TRACKS.length) + "</span></div>" +
			'<span class="m-abs m-rep-enlace m-cuerpo m-linea" style="left:' + px(8) + '">' + t.a + "</span>" +
			'<span class="m-abs m-rep-enlace m-cuerpo m-linea" style="left:' + px(144) + '">' + t.al + "</span>" +
			renderDownload() +
			'<div class="m-abs m-controles">' +
			skip("prev", "prev", "m_prev", first) +
			'<button type="button" class="m-boton m-boton--primario m-sq48" data-ctl="play" aria-label="' +
			T(showPause ? "m_pause" : "m_play") + '">' + icon(showPause ? "pause" : "play") + "</button>" +
			skip("next", "next", "m_next", last) +
			"</div>" +
			bar("m-progreso-inf") +
			'<div class="m-abs m-volumen"><span class="m-meta">' + T("m_volume") + "</span>" +
			'<div class="m-barra m-barra--vol"><span class="m-barra__progreso" style="width:70%"></span></div></div>' +
			hints(keys);
	}

	function renderSettings() {
		/* Cuatro botones de 32 px a 12 px de separación (§7.8). */
		var items = [
			["set-lang", icon("language") + "<span>" + fmt(T("m_lang_fmt"), T("lang_name")) + "</span>"],
			["set-dl", icon("download") + "<span>" + T("m_downloads") + "</span>"],
			["set-reconnect", "<span>" + T("m_reconnect") + "</span>"],
			["set-pair", "<span>" + T("m_pair") + "</span>"]
		];
		return items.map(function (it, i) {
			return '<button type="button" class="m-abs m-boton m-ancho m-cuerpo" style="top:' + px(44 + i * 44) +
				'" data-ctl="' + it[0] + '">' + it[1] + "</button>";
		}).join("") + hints([["A", T("m_choose")], ["B", T("m_back")]]);
	}

	function updateProgress() {
		var t = TRACKS[st.track];
		var p = Math.min(1, st.pos / t.d);
		var dl = Math.min(1, (st.pos + 75) / t.d);
		var pct = function (v) { return (v * 100).toFixed(2) + "%"; };
		each("#consola [data-p]", function (el) {
			switch (el.getAttribute("data-p")) {
			case "tiempos":
				el.innerHTML = st.finished
					? "<span>" + T("m_finished") + "</span><span>" + mmss(t.d) + "</span>"
					: "<span>" + mmss(st.pos) + "</span><span>" + mmss(t.d) + "</span>";
				break;
			case "dl": el.style.width = pct(dl); break;
			case "pr": el.style.width = pct(p); break;
			case "th": el.style.left = pct(p); break;
			}
		});
	}

	/* Pulsado de la maqueta: dura lo que la pulsación y, como mínimo,
	 * 100 ms. Sobrevive al redibujo porque se guarda por control. */
	var pressed = { id: null, start: 0, until: 0 };
	var pressTimer = null;

	function ctlEl(id) {
		return document.querySelector('#consola [data-ctl="' + id + '"]');
	}

	function applyPressed() {
		if (!pressed.id) return;
		var el = ctlEl(pressed.id);
		if (el) el.classList.add("is-pressed");
	}

	function clearPressed() {
		if (!pressed.id) return;
		var el = ctlEl(pressed.id);
		if (el) el.classList.remove("is-pressed");
		pressed.id = null;
	}

	function schedulePressEnd() {
		clearTimeout(pressTimer);
		if (!pressed.id || pressed.until === Infinity) return;
		pressTimer = setTimeout(clearPressed, Math.max(0, pressed.until - Date.now()));
	}

	var sup = document.getElementById("m-sup");
	var inf = document.getElementById("m-inf");
	var cons = document.getElementById("consola");

	var mock = {
		render: function () {
			if (!sup || !inf) return;
			var active = document.activeElement;
			var hadFocus = cons.contains(active);
			var focusId = hadFocus && active.getAttribute ? active.getAttribute("data-ctl") : null;

			sup.innerHTML = renderTop();
			var view = { home: renderHome, search: renderSearch, playing: renderPlaying, settings: renderSettings }[st.tab];
			inf.innerHTML = renderTabs() + view();
			updateProgress();
			applyPressed();

			if (hadFocus) {
				var el = (focusId && ctlEl(focusId)) || ctlEl("tab:" + st.tab);
				if (el) el.focus({ preventScroll: true });
			}
		}
	};

	function keepFocusVisible() {
		/* Lista continua (§7.4): se mueve lo justo para ver la fila en foco. */
		if (st.focus < st.scroll) st.scroll = st.focus;
		if (st.focus > st.scroll + ROWS_VISIBLE - 1) st.scroll = st.focus - ROWS_VISIBLE + 1;
	}

	function playTrack(i) {
		st.track = i;
		st.pos = 0;
		st.playing = true;
		st.finished = false;
		/* Mientras suena la cola, la fila en foco sigue a la pista. */
		st.focus = i;
		keepFocusVisible();
	}

	function act(id) {
		var parts = id.split(":");
		switch (parts[0]) {
		case "tab": st.tab = parts[1]; break;
		case "tile": st.home = +parts[1]; break;
		case "dl":
			/* La descarga empieza y avanza con el reloj. */
			if (st.dl[st.track] === undefined) st.dl[st.track] = 0;
			break;
		case "row":
			var i = +parts[1];
			if (st.focus === i) {
				playTrack(i);
				st.tab = "playing";
			} else {
				st.focus = i;
			}
			break;
		case "prev": if (st.track > 0) playTrack(st.track - 1); break;
		case "next": if (st.track < TRACKS.length - 1) playTrack(st.track + 1); break;
		case "play":
			if (st.finished) {
				st.pos = 0;
				st.finished = false;
				st.playing = true;
			} else {
				st.playing = !st.playing;
			}
			break;
		case "set-lang": toggleLang(); return;
		}
		mock.render();
	}

	function ctlFrom(e) {
		var el = e.target.closest && e.target.closest("[data-ctl]");
		if (!el || !cons.contains(el) || el.getAttribute("aria-disabled") === "true") return null;
		return el;
	}

	if (cons && sup && inf) {
		cons.addEventListener("pointerdown", function (e) {
			if (e.button !== 0) return;
			var el = ctlFrom(e);
			if (!el) return;
			clearPressed();
			clearTimeout(pressTimer);
			pressed = { id: el.getAttribute("data-ctl"), start: Date.now(), until: Infinity };
			el.classList.add("is-pressed");
		});
		document.addEventListener("pointerup", function () {
			if (pressed.id && pressed.until === Infinity) {
				pressed.until = Math.max(pressed.start + PRESS_MIN_MS, Date.now());
				schedulePressEnd();
			}
		});
		cons.addEventListener("pointerout", function (e) {
			/* El lápiz sale del control antes de levantarse: reposo y sin acción (§6.4). */
			var el = ctlFrom(e);
			if (!el || pressed.until !== Infinity || pressed.id !== el.getAttribute("data-ctl")) return;
			if (e.relatedTarget && el.contains(e.relatedTarget)) return;
			clearPressed();
		});
		cons.addEventListener("click", function (e) {
			var el = ctlFrom(e);
			if (!el) return;
			var id = el.getAttribute("data-ctl");
			if (pressed.id !== id) {
				/* Teclado (Intro/Espacio): el mismo pulsado mínimo. */
				clearPressed();
				pressed = { id: id, start: Date.now(), until: Date.now() + PRESS_MIN_MS };
			}
			act(id);
			applyPressed();
			schedulePressEnd();
		});

		/* Reloj: el progreso avanza una vez por segundo (§5.4). */
		setInterval(function () {
			/* Las descargas siguen aunque la música esté en pausa. */
			var moved = false;
			Object.keys(st.dl).forEach(function (k) {
				if (st.dl[k] < 100) {
					st.dl[k] = Math.min(100, st.dl[k] + DL_STEP);
					moved = true;
				}
			});
			if (moved && st.tab === "playing") mock.render();
			if (!st.playing) return;
			var t = TRACKS[st.track];
			st.pos += 1;
			if (st.pos < t.d) {
				updateProgress();
			} else if (st.track < TRACKS.length - 1) {
				playTrack(st.track + 1);
				mock.render();
			} else {
				/* Fin de la cola: la cola no da la vuelta. */
				st.pos = t.d;
				st.playing = false;
				st.finished = true;
				mock.render();
			}
		}, 1000);

		var hint = document.getElementById("consola-pista");
		if (hint) hint.hidden = false;
	}

	apply(current);
	mock.render();

	var langBtn = document.getElementById("idioma");
	if (langBtn) {
		langBtn.addEventListener("click", toggleLang);
	}

	/* ------------------------------------------------------------ */
	/* Estado pulsado (§6.3): dura mientras se pulsa y, como mínimo, */
	/* 100 ms (los 6 frames a 60 Hz de la app).                       */
	/* ------------------------------------------------------------ */

	var PRESS_MIN_MS = 100;

	function press(el) {
		clearTimeout(el._pressTimer);
		el._pressStart = Date.now();
		el.classList.add("is-pressed");
	}

	function release(el) {
		if (!el.classList.contains("is-pressed")) return;
		var left = PRESS_MIN_MS - (Date.now() - (el._pressStart || 0));
		clearTimeout(el._pressTimer);
		el._pressTimer = setTimeout(function () {
			el.classList.remove("is-pressed");
		}, Math.max(0, left));
	}

	function cancel(el) {
		/* El puntero salió del control: vuelve a reposo (§6.4). */
		clearTimeout(el._pressTimer);
		el.classList.remove("is-pressed");
	}

	each(".btn", function (el) {
		el.addEventListener("pointerdown", function (e) {
			if (e.button === 0) press(el);
		});
		el.addEventListener("pointerup", function () { release(el); });
		el.addEventListener("pointerleave", function () { cancel(el); });
		el.addEventListener("pointercancel", function () { cancel(el); });

		/* Teclado: Intro en enlaces y botones, Espacio en botones. */
		el.addEventListener("keydown", function (e) {
			if (e.repeat) return;
			if (e.key === "Enter" || (e.key === " " && el.tagName === "BUTTON")) {
				press(el);
				if (e.key === "Enter") release(el);
			}
		});
		el.addEventListener("keyup", function (e) {
			if (e.key === " ") release(el);
		});
		el.addEventListener("blur", function () { release(el); });
	});
})();
