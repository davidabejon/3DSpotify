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
			issue_text: "Some Spotify Premium accounts get <code>Audio key refused (0x0001)</code> on every song. <strong>This is a Spotify problem, not a 3DSpotify bug:</strong> Spotify’s servers refuse the audio key for those accounts, and other unofficial clients hit the same error (<a href=\"https://github.com/librespot-org/librespot/issues/1649\">librespot #1649</a>). There is no fix yet, but <strong>we are actively working on solving it, or at least on finding an alternative</strong>.",
			hero_lema: "Spotify on your Nintendo 3DS.",
			hero_frase: "Search for songs and play them on the console itself, with no PC in between.",
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
			m_search_songs: "Search songs",
			m_last_search: "Last search",
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
			f1_title: "Straight to Spotify",
			f1_text: "The console connects to Spotify over HTTPS by itself. No PC, proxy or phone app in between.",
			f2_title: "Search songs",
			f2_text: "Type with the system keyboard and browse results four at a time, with cover thumbnails. The search runs in the background.",
			f3_title: "Plays on the console",
			f3_text: "Audio is decoded and played locally, with the rest of the results as the queue, the album art and its own volume.",
			f4_title: "Music never stops",
			f4_text: "Four tabs (Home, Search, Playing and Settings) and the track keeps playing whichever one you open.",
			f5_title: "Sign in without typing passwords",
			f5_text: "Link your account with a short code on spotify.com/pair or by scanning a QR code with your phone.",
			f6_title: "English and Spanish",
			f6_text: "Switch the language at any moment with SELECT, on any screen.",
			req_title: "You will need",
			req_text: "A Spotify Premium account and a Nintendo 3DS with Wi-Fi and the Homebrew Launcher. Installing the .cia also requires custom firmware.",

			controls_title: "Controls",
			col_button: "Button",
			col_action: "What it does",
			c_a: "Confirm: open the keyboard, play the highlighted song, pause or resume.",
			c_b: "Go back, or cancel a pending search or pairing.",
			c_y: "New search from the results; pair again after a sign-in error.",
			dpad: "D-Pad",
			c_dpad: "Move the focus. In results, left/right jump four songs; in the player, left/right change track and up/down set the volume.",
			c_lr: "Previous or next tab (ZL/ZR too on New 3DS).",
			c_select: "Switch between English and Spanish.",
			c_start: "Exit the app.",
			touch: "Touch",
			c_touch: "Tap tabs, buttons and songs; the action runs when you lift the stylus inside. Drag lists to scroll.",

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
			mcia_3: "Open it from the <strong>HOME Menu</strong>. Settings are kept in <code>SD:/3ds/spotibacan/</code>.",
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
			issue_text: "Algunas cuentas de Spotify Premium reciben <code>Audio key refused (0x0001)</code> en todas las canciones. <strong>Es un problema de Spotify, no un fallo de 3DSpotify:</strong> los servidores de Spotify deniegan la clave de audio a esas cuentas, y otros clientes no oficiales sufren el mismo error (<a href=\"https://github.com/librespot-org/librespot/issues/1649\">librespot #1649</a>). Todavía no hay solución, pero <strong>estamos trabajando activamente para solucionarlo o, al menos, encontrar una alternativa</strong>.",
			hero_lema: "Spotify en tu Nintendo 3DS.",
			hero_frase: "Busca canciones y escúchalas en la propia consola, sin un PC de por medio.",
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
			m_search_songs: "Buscar canciones",
			m_last_search: "Última búsqueda",
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
			f1_title: "Directo a Spotify",
			f1_text: "La consola se conecta sola a Spotify por HTTPS. Sin PC, proxy ni app del móvil de por medio.",
			f2_title: "Busca canciones",
			f2_text: "Escribe con el teclado del sistema y recorre los resultados de cuatro en cuatro, con miniaturas de las carátulas. La búsqueda va en segundo plano.",
			f3_title: "Suena en la consola",
			f3_text: "El audio se decodifica y se reproduce en la propia consola, con el resto de resultados como cola, la carátula y su propio volumen.",
			f4_title: "La música no se corta",
			f4_text: "Cuatro pestañas (Inicio, Búsqueda, Reproducción y Ajustes) y la pista sigue sonando abras la que abras.",
			f5_title: "Inicia sesión sin escribir contraseñas",
			f5_text: "Vincula tu cuenta con un código corto en spotify.com/pair o escaneando un código QR con el móvil.",
			f6_title: "Inglés y español",
			f6_text: "Cambia de idioma cuando quieras con SELECT, en cualquier pantalla.",
			req_title: "Necesitarás",
			req_text: "Una cuenta Spotify Premium y una Nintendo 3DS con Wi-Fi y Homebrew Launcher. Para instalar el .cia hace falta además firmware personalizado.",

			controls_title: "Controles",
			col_button: "Botón",
			col_action: "Qué hace",
			c_a: "Confirmar: abre el teclado, reproduce la canción resaltada, pausa o reanuda.",
			c_b: "Volver, o cancelar una búsqueda o un emparejamiento pendiente.",
			c_y: "Nueva búsqueda desde los resultados; emparejar de nuevo tras un error de sesión.",
			dpad: "Cruceta",
			c_dpad: "Mueve el foco. En los resultados, izquierda/derecha saltan cuatro canciones; en el reproductor, izquierda/derecha cambian de pista y arriba/abajo ajustan el volumen.",
			c_lr: "Pestaña anterior o siguiente (también ZL/ZR en New 3DS).",
			c_select: "Cambia entre inglés y español.",
			c_start: "Sale de la app.",
			touch: "Táctil",
			c_touch: "Toca pestañas, botones y canciones; la acción se ejecuta al levantar el lápiz dentro. Arrastra las listas para desplazarlas.",

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
			mcia_3: "Ábrelo desde el <strong>menú HOME</strong>. Los ajustes se guardan en <code>SD:/3ds/spotibacan/</code>.",
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
	var QUERY = "summer";
	var TABS = ["home", "search", "playing", "settings"];
	var TAB_ICON = { home: "home", search: "search", playing: "playing", settings: "settings" };
	var ROW_STEP = 45;      /* fila de 41 px + 4 px de separación */
	var ROWS_VISIBLE = 4;
	var LIST_H = 184;

	var reduceMotion = window.matchMedia &&
		window.matchMedia("(prefers-reduced-motion: reduce)").matches;

	var st = {
		tab: "playing",
		track: 2,
		pos: 84,
		playing: !reduceMotion,
		finished: false,
		focus: 2,
		scroll: 0
	};

	function T(key) { return tr(current, key); }

	function fmt(s, a, b) {
		return s.replace("%d", a).replace("%d", b).replace("%s", a);
	}

	function mmss(sec) {
		var m = Math.floor(sec / 60), s = sec % 60;
		return m + ":" + (s < 10 ? "0" : "") + s;
	}

	function icon(name) {
		return '<svg class="icon" aria-hidden="true"><use href="#i-' + name + '"/></svg>';
	}

	function cover(tr_, cls) {
		var c = tr_.c;
		return '<span class="m-cover ' + cls + '" aria-hidden="true" style="' +
			"--c1:var(--" + c[0] + ");--c2:var(--" + c[1] + ");--c3:var(--" + c[2] + ");--c4:var(--" + c[3] + ')">' +
			"<i></i><i></i><i></i></span>";
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

	function hints(list) {
		return '<div class="m-abs m-barra-botones" aria-hidden="true">' + list.map(function (h) {
			return '<span class="m-accion"><span class="m-glifo m-glifo--' + h[0].toLowerCase() + '">' +
				h[0] + '</span><span class="m-meta">' + h[1] + "</span></span>";
		}).join("") + "</div>";
	}

	function renderTop() {
		var t = TRACKS[st.track];
		if (st.tab === "playing") {
			/* Reproducción: la carátula grande. */
			return cover(t, "m-cover--grande");
		}
		/* Inicio, Búsqueda y Ajustes: la ficha de lo que suena. */
		return cover(t, "m-cover--ficha") +
			'<div class="m-abs m-ficha">' +
			'<span class="m-titulo m-linea">' + t.t + "</span>" +
			'<span class="m-cuerpo m-linea">' + t.a + "</span>" +
			'<span class="m-cuerpo m-linea">' + t.al + "</span>" +
			"</div>" + bar("m-progreso-sup");
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
		return '<button type="button" class="m-abs m-boton m-boton--primario m-ancho m-cuerpo" style="top:calc(48 * var(--u))" data-ctl="home-search">' +
			icon("search") + "<span>" + T("m_search_songs") + "</span></button>" +
			'<span class="m-abs m-meta" style="left:calc(8 * var(--u));top:calc(96 * var(--u))">' + T("m_last_search") + "</span>" +
			'<button type="button" class="m-abs m-boton m-ancho m-cuerpo" style="top:calc(112 * var(--u))" data-ctl="home-last">' +
			"<span>“" + QUERY + "”</span></button>" +
			hints([["A", T("m_search")]]);
	}

	function renderSearch() {
		var n = TRACKS.length;
		var rows = TRACKS.map(function (t, i) {
			var y = 4 + (i - st.scroll) * ROW_STEP;
			var foco = i === st.focus;
			return '<button type="button" class="m-fila-res' + (foco ? " m-fila-res--foco" : "") +
				'" style="top:calc(' + y + ' * var(--u))" data-ctl="row:' + i + '"' +
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
			'<div class="m-abs m-scroll" aria-hidden="true"><i style="top:calc(' + top +
			' * var(--u));height:calc(' + h + ' * var(--u))"></i></div>' +
			hints([["A", T("m_play")], ["Y", T("m_search")], ["B", T("m_back")]]);
	}

	function renderPlaying() {
		var t = TRACKS[st.track];
		var first = st.track === 0, last = st.track === TRACKS.length - 1;
		var showPause = st.playing;
		function skip(ctl, ic, label, off) {
			return '<button type="button" class="m-boton m-sq40" data-ctl="' + ctl + '" aria-label="' + T(label) + '"' +
				(off ? ' aria-disabled="true"' : "") + ">" + icon(ic) + "</button>";
		}
		return '<div class="m-abs m-cabeza-rep">' +
			'<div class="m-fila"><span class="m-cabeza m-linea">' + t.t + "</span>" +
			'<span class="m-meta">' + fmt(T("m_position_fmt"), st.track + 1, TRACKS.length) + "</span></div>" +
			'<span class="m-cuerpo m-linea" style="color:var(--texto-2)">' + t.a + "</span></div>" +
			'<div class="m-abs m-controles">' +
			skip("prev", "prev", "m_prev", first) +
			'<button type="button" class="m-boton m-boton--primario m-sq48" data-ctl="play" aria-label="' +
			T(showPause ? "m_pause" : "m_play") + '">' + icon(showPause ? "pause" : "play") + "</button>" +
			skip("next", "next", "m_next", last) +
			"</div>" +
			bar("m-progreso-inf") +
			'<div class="m-abs m-volumen"><span class="m-meta">' + T("m_volume") + "</span>" +
			'<div class="m-barra m-barra--vol"><span class="m-barra__progreso" style="width:70%"></span></div></div>' +
			hints([["A", T(showPause ? "m_pause" : "m_play")], ["B", T("m_back")]]);
	}

	function renderSettings() {
		var ys = [48, 96, 144];
		var items = [
			["set-lang", icon("language") + "<span>" + fmt(T("m_lang_fmt"), T("lang_name")) + "</span>"],
			["set-reconnect", "<span>" + T("m_reconnect") + "</span>"],
			["set-pair", "<span>" + T("m_pair") + "</span>"]
		];
		return items.map(function (it, i) {
			return '<button type="button" class="m-abs m-boton m-ancho m-cuerpo" style="top:calc(' + ys[i] +
				' * var(--u))" data-ctl="' + it[0] + '">' + it[1] + "</button>";
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
		case "home-search":
		case "home-last": st.tab = "search"; break;
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
