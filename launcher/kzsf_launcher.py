#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Killzone Shadow Fall (PC port) launcher for Windows (Tkinter).

One window: the game folder (CUSA00190, version 01.81, from the player's own dump), basic
graphics options (settings.ini, shared with the in-game menu), keyboard and controller mapping
(GOW3_KEY_MAP / GOW3_PAD_MAP), and shortcuts to saves, logs and the cache.

Everything the port writes lives in user/ beside the launcher:
  user/launcher.json   launcher choices (game folder, language, mappings)
  user/settings.ini    game options (GOW3_CONFIG; the in-game menu writes it too)
  user/out/            the prepared game image (GOW3_DATA_DIR/out; rebuilt when missing)
  user/ps4/            the console's user folder: savedata, screenshots, shader cache
  user/logs/<time>/    one folder per game start

Frozen with PyInstaller (packaging/windows/package_kzsf.sh), the same executable also runs
run.py (`--run`) and the preparation scripts (`--script`), so players need no Python.
The artwork (banner, icon) comes from the player's game folder: nothing of the game ships.
"""
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
import zipfile

import kzsf_fsr4
import kzsf_update

WINDOWS = sys.platform == 'win32'
if WINDOWS:
    import ctypes
    # AMD hybrid laptops: this layer hides the discrete GPU from Vulkan and the game stays black.
    os.environ.setdefault('VK_LOADER_LAYERS_DISABLE', 'VK_LAYER_AMD_switchable_graphics')
FROZEN = getattr(sys, 'frozen', False)
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PORT_DIR / 'scripts'))

VERSION = '0.7.0'
TITLE_ID = 'CUSA00190'
# Other releases of the game players run (their languages), accepted untested: the port was built and
# tested on CUSA00190. Every one must be at version 01.81.
OTHER_TITLE_IDS = {'CUSA00191': 'LATAM', 'CUSA00002': 'Europe', 'CUSA00085': 'Japan'}
TITLE_IDS = {TITLE_ID, *OTHER_TITLE_IDS}
GAME_VERSION = '01.81'
USER_DIR = Path(os.environ.get('KZSF_USER_DIR', PORT_DIR / 'user'))
LAUNCHER_FILE = USER_DIR / 'launcher.json'
SETTINGS_FILE = USER_DIR / 'settings.ini'
PS4_DIR = USER_DIR / 'ps4'
LOGS_DIR = USER_DIR / 'logs'
UPDATE_DIR = USER_DIR / 'update'
# FSR 4 shaders, downloaded by the launcher (kzsf_fsr4.py); the game reads GOW3_FSR4_DIR.
FSR4_DIR = PORT_DIR / 'fsr4_shaders'
KEEP_LOGS = 10
NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

# Killzone Shadow Fall's menus: near black, white text, cyan highlight.
BG = '#0a0e11'
PANEL = '#11181d'
FIELD = '#18222a'
LINE = '#26343d'
TEXT = '#e9eff3'
MUTED = '#8796a0'
ACCENT = '#35c8e4'
ACCENT_DARK = '#1d8ea6'
OK = '#5ad48a'
WARN = '#e8b13a'
ERROR = '#e46060'
MARK = '#2f8bff'  # "Lesca": marks this build as ours
DONATE_BG = '#0070ba'     # PayPal blue
DONATE_HOVER = '#1a8ad6'
DONATE_URL = 'https://www.paypal.com/paypalme/SantiagoLescano577'
DISCORD_BG = '#5865f2'    # Discord blurple
DISCORD_HOVER = '#7983f5'
DISCORD_URL = 'https://discord.gg/UYFcV5rbDw'
UI_FONT = 'Segoe UI'


# ---------------------------------------------------------------------------------------------
# Texts (Spanish and English).

TEXTS = {
    'es': {
        'tab_game': 'Juego', 'tab_graphics': 'Gráficos', 'tab_controls': 'Controles', 'tab_advanced': 'Avanzado',
        'tab_cheats': 'Trucos',
        'cheats': 'Trucos', 'cheats_help': 'Solo afectan al jugador. También se cambian en el juego, al instante, '
        'en el menú (Insert) > Cheats.',
        'cheat_god': 'Modo dios', 'cheat_god_help': 'No recibes daño.',
        'cheat_ammo': 'Munición infinita', 'cheat_ammo_help': 'La munición de reserva y las granadas no se gastan.',
        'cheat_no_reload': 'Sin recarga', 'cheat_no_reload_help': 'El cargador nunca se vacía.',
        'cheat_sprint': 'Sprint ilimitado', 'cheat_sprint_help': 'La resistencia se recarga mientras corres.',
        'cheat_echo': 'Eco táctico continuo', 'cheat_echo_help': 'El escaneo del OWL sigue activo. Úsalo una vez '
        'después de cada carga.',
        'debug': 'Depuración',
        'cheat_chapters': 'Desbloquear todos los capítulos', 'cheat_chapters_help': 'Todos en Selección de capítulo; '
        'tu progreso guardado no cambia.',
        'free_camera': 'Cámara libre', 'free_camera_help': 'En el juego: F9 la activa o desactiva, F10 vuelve al '
        'jugador. WASD mover, Q / E bajar / subir, ratón o flechas mirar, Shift más rápido, Ctrl más lento. Con '
        'mando: sticks, L1 / R1 bajar / subir, R2 más rápido, L2 más lento (actívala desde el menú > Cheats). El '
        'jugador queda quieto mientras tanto.',
        'play': 'JUGAR', 'playing': 'EN JUEGO', 'preparing': 'Iniciando…',
        'game_folder': 'Carpeta del juego',
        'game_folder_help': f'La carpeta de tu dump de PS4 (versión {GAME_VERSION}; {TITLE_ID}, la probada, o '
                            f'{", ".join(sorted(OTHER_TITLE_IDS))}, sin probar): la que contiene eboot.bin.',
        'update_folder': 'Carpeta de la actualización',
        'update_folder_help': f'Tu dump base no es la versión {GAME_VERSION}: elegí la carpeta de la actualización {GAME_VERSION} (CUSA00190-patch).',
        'browse': 'Examinar…', 'choose_game': 'Elegí la carpeta del juego (la que tiene eboot.bin)',
        'choose_update': 'Elegí la carpeta de la actualización',
        'no_folder': 'Todavía no elegiste la carpeta del juego.',
        'no_eboot': 'En esa carpeta no hay eboot.bin.',
        'no_sfo': 'Falta sce_sys/param.sfo: el dump está incompleto.',
        'wrong_game': 'Esa carpeta es de otro juego ({id}). Este port es para Killzone Shadow Fall ({want}).',
        'other_region': 'Versión {id}: no probada (el port se desarrolló y probó en CUSA00190), puede funcionar; contanos cómo te va en GitHub.',
        'other_region_jp': 'Versión {id} (japonesa): no probada (el port se desarrolló y probó en CUSA00190); los textos en japonés pueden verse mal o faltar.',
        'need_update': 'Versión {ver}: falta la actualización {want}.',
        'bad_update': 'La carpeta de actualización no es {want} versión {ver_want} (es {id} {ver}).',
        'update_no_eboot': 'La actualización es la correcta, pero falta eboot.bin en {folder}. '
                           'Volvé a extraer el PKG de la actualización completo.',
        'ready': 'Listo: {title} · {id} · versión {ver}',
        'ready_update': 'Listo: {title} · {id} · versión {ver} (con actualización)',
        'fov': 'Campo de visión (FOV)',
        'fov_help': 'Porcentaje del campo de visión original del juego (100% = como en PS4). '
                    'También agranda el zoom al apuntar en la misma proporción.',
        'tools': 'Carpetas y mantenimiento',
        'open_saves': 'Partidas guardadas', 'open_shots': 'Capturas', 'open_logs': 'Logs',
        'clear_cache': 'Borrar caché',
        'cache_size': 'Caché: {size}. Se vuelve a crear al jugar (el primer inicio y las zonas nuevas tardan más).',
        'clear_confirm': 'Se borra la caché de shaders y la imagen preparada del juego ({size}).\n'
                         'Tus partidas guardadas no se tocan. ¿Continuar?',
        'cleared': 'Caché borrada.',
        'busy_running': 'Cerrá el juego primero.',
        'display_mode': 'Modo de pantalla', 'mode_windowed': 'Ventana', 'mode_borderless': 'Ventana sin bordes',
        'mode_fullscreen': 'Pantalla completa',
        'window_size': 'Tamaño de ventana',
        'window_size_help': 'Solo en modo ventana. La imagen del juego se escala a la ventana.',
        'render_res': 'Resolución de render', 'res_native': '1080p (como en PS4)',
        'ultrawide': 'Ultrawide (beta)', 'ultrawide_off': 'No (16:9)',
        'ultrawide_help': 'Para monitores 21:9 o 32:9: la imagen llena la pantalla y se ve más a los costados '
                          '(Hor+), con el HUD, los menús y los videos en el centro en 16:9. Usa FSR / DLSS '
                          '(Avanzado → Método); reemplaza la resolución de render. La escena se dibuja con '
                          'hasta 1080 líneas y FSR / DLSS la llevan a la salida. Se aplica al iniciar.',
        'render_res_help': 'Más alta = más nítida, más GPU y VRAM. Método en Avanzado. Se aplica al iniciar.',
        'fps_limit': 'Límite de FPS', 'vsync': 'VSync (sincronización vertical)',
        'fps_unlimited': 'Sin límite (experimental)', 'fps_experimental': 'experimental',
        'fps_limit_help': '144 / 240 / sin límite: experimental. Con monitor de 60 Hz, sin VSync.',
        'frame_gen': 'Generación de frames', 'frame_gen_off': 'Desactivada',
        'frame_gen_fsr3': 'AMD FSR 3.1', 'frame_gen_dlss': 'NVIDIA DLSS (RTX 40/50)',
        'frame_gen_help': 'Experimental. Crea un frame entre cada par de frames del juego: casi el doble de '
                          'FPS en pantalla, con un poco más de latencia. Ideal si el juego ya va a 40 FPS o más. '
                          'FSR 3.1 funciona en cualquier placa; DLSS solo en RTX 40/50 (en otras usa FSR 3.1). '
                          'No funciona con el método clásico ni a 720p. Con un monitor de 60 Hz desactivá VSync.',
        'vsync_help': 'Evita el tearing. Desactivado: un poco menos de latencia.',
        'anisotropy': 'Filtrado anisotrópico', 'aniso_game': 'El del juego',
        'mip_bias': 'Nitidez de texturas lejanas', 'mip_game': 'La del juego',
        'mip_sharper': 'más nítida', 'mip_softer': 'más suave',
        'mip_bias_help': 'Más nítida: más detalle lejano, algo de parpadeo. Más suave: lo contrario.',
        'lod_distance': 'Distancia de detalle (LOD)',
        'lod_labels': ['50% (más rendimiento)', '75%', '100% (como en PS4)', '150%', '200%',
                       '300% (menos pop-in)'],
        'lod_distance_help': 'Hasta qué distancia personajes y objetos mantienen su versión detallada. '
                             'Más alto: menos aparición repentina de detalle, más carga de GPU y CPU. '
                             'Se puede cambiar en el juego (Insert).',
        'sharpen': 'Nitidez (RCAS)', 'show_fps': 'Mostrar FPS en pantalla',
        'fsr1': 'Escalado FSR 1',
        'fsr1_help': 'Para pantallas de más de 1080p (1440p, 4K): imagen más nítida que el estirado normal. '
                     'A 1080p no cambia nada.',
        'graphics_note': 'Durante el juego, Insert abre el menú del port (también con el mando: R3 + L2).',
        'effects': 'Efectos', 'effects_help': 'Se aplican al iniciar el juego; el temblor de cámara también cambia al instante desde el menú (Insert). El retroceso del arma se mantiene.',
        'motion_blur': 'Desenfoque de movimiento', 'depth_of_field': 'Profundidad de campo',
        'bloom': 'Bloom (brillo de las luces)', 'film_grain': 'Grano de película',
        'camera_shake': 'Temblor de cámara', 'camera_shake_help': 'Explosiones, impactos. El retroceso del arma se mantiene.',
        'keyboard': 'Teclado', 'controller': 'Mando',
        'press_key': 'Pulsá una tecla…', 'reset_defaults': 'Valores por defecto',
        'controls_help': 'Click en una tecla y apretá otra tecla, un botón del mouse o la rueda (Esc cancela). Movimiento: WASD. Cámara: mouse. Cruceta: flechas.\n'
                         'Touchpad (dron OWL): mantené Suprimir y arrastrá el mouse. Tab / Retroceso: click izquierdo / '
                         'derecho del touchpad.',
        'controller_help': 'Botón del mando que hace de cada botón de PS4. Los gatillos y sticks no se cambian.',
        'vibration': 'Vibración',
        'dead_zone': 'Zona muerta de sticks', 'pad_speaker': 'Altavoz del mando',
        'quick_menu': 'Mantener Select/Share: menú rápido', 'quick_menu_hint': 'Consejos al iniciar',
        'menu_combo': 'Botones que abren el menú del port',
        'menu_combos': ['R3 + L2', 'L3 + R3', 'Share + Options', 'Ninguno (solo Insert)'],
        'quick_menu_help': 'Mantenerlo apretado ~1 segundo abre Continuar / Ajustes / Salir del juego desde el '
                           'mando; un toque corto sigue funcionando como siempre (touchpad). Consejos: al empezar, '
                           'un aviso breve de cómo usar el dron OWL con teclado (Supr + mouse) y del menú rápido.',
        'pad_extra_help': 'Zona muerta: subila si la cámara o el personaje se mueven solos (stick gastado). '
                          'Altavoz del mando: sonidos que la PS4 manda al parlante del mando; acá suenan '
                          'por tus parlantes (0% = apagado).',
        'prompt_style': 'Íconos de botones en el juego',
        'prompt_styles': ['Automático (último dispositivo y tipo de mando)', 'Teclado y mouse', 'PS4', 'Xbox'],
        'prompt_style_help': 'Los avisos del juego muestran la tecla asignada (o el mouse), o los botones de Xbox, '
                             'en vez del botón de PS4. En automático, un mando de PlayStation muestra PS4 y '
                             'cualquier otro, Xbox. Los sticks siguen con el ícono de PS4.',
        'mouse': 'Mouse', 'mouse_look': 'Apuntar con el mouse',
        'mouse_look_help': 'El mouse mueve la cámara 1:1, sin la zona muerta ni el tope de un stick '
                           '(sensibilidad recomendada: alrededor de 1); click izquierdo dispara (R2), derecho '
                           'apunta (L2). Giros rápidos sube el límite de velocidad de giro del juego mientras '
                           'movés el mouse (x6 por defecto). Alt izquierdo libera el cursor.',
        'mouse_sens': 'Sensibilidad', 'mouse_invert': 'Invertir eje Y', 'mouse_turn': 'Giros rápidos',
        'advanced_help': 'Solo si algo se ve mal o el juego no funciona bien. Si tenés dudas, dejalo como está.',
        'async_shaders': 'Compilar shaders nuevos en segundo plano',
        'async_help': 'Menos tirones en zonas nuevas; un objeto puede aparecer un instante después.',
        'stale_readback': 'Lecturas de GPU sin esperar',
        'stale_help': 'Más FPS. Desactivalo si la iluminación u objetos se ven mal.',
        'deferred_readback': 'Lecturas de GPU diferidas',
        'deferred_help': 'Menos esperas. Desactivalo si texturas o sombras se ven mal.',
        'res_method': 'Método de resolución',
        'res_method_upscale': 'AMD FSR 3.1 (recomendado)',
        'res_method_dlss': 'NVIDIA DLSS (solo RTX)',
        'res_method_fsr4': 'AMD FSR 4 (mejor calidad)',
        'fsr4_files': 'Archivos de FSR 4',
        'fsr4_ready': 'Instalados',
        'fsr4_missing': 'No instalados (unos 30 MB)',
        'fsr4_download': 'Descargar',
        'fsr4_downloading': 'Descargando… {done}/{total}',
        'fsr4_failed': 'Falló la descarga: {error}',
        'fsr4_help': 'FSR 4 usa el modelo de IA de AMD (INT8): más nítido y estable que FSR 3.1, un poco más '
                     'pesado. Funciona en placas con Vulkan 1.3 y punto entero de 8 bits (RTX 20 o más nuevas, '
                     'Radeon RX 6000/7000/9000, Arc). Los archivos vienen de FireBurn/Q2RTX en GitHub '
                     '(compilados del código FidelityFX de AMD, licencia MIT). Sin ellos o en otra placa usa FSR 3.1.',
        'res_method_patch': 'Clásico: parche del juego',
        'res_method_help': 'Solo cambia algo por encima de 1080p. FSR 3.1 (recomendado): el juego renderiza en '
                           '1080p y FSR lleva la imagen a la resolución elegida, con HUD, menús y videos nítidos. '
                           'DLSS: lo mismo con NVIDIA DLSS (GeForce RTX; en otras placas usa FSR 3.1). Clásico: '
                           'el juego mismo renderiza a esa resolución; más exigente, y pueden aparecer parpadeos '
                           'de luz, una sombra que sigue a la cámara y videos con zoom.',
        'upscale_quality': 'Calidad de FSR / DLSS',
        'uq_native': 'Nativa (máxima calidad)', 'uq_quality': 'Calidad', 'uq_balanced': 'Equilibrado',
        'uq_performance': 'Rendimiento', 'uq_ultra': 'Ultra rendimiento',
        'upscale_quality_help': 'Con FSR o DLSS (Avanzado → Método de resolución) o con generación de frames: '
                                'la escena 3D se renderiza más chica y FSR / DLSS la reconstruyen; HUD, menús y '
                                'videos siguen nítidos. Da FPS si tu placa de video es el límite (GPU modesta, '
                                '1440p/4K); si el límite es el procesador no suma. Nativa: escena a 1080p.',
        'perf_heading': 'Rendimiento',
        'draw_pipe': 'Dibujo en paralelo (2 hilos de GPU)',
        'nowait_writes': 'Copias del juego sin esperar a la GPU',
        'fast_locks': 'Sincronización rápida entre hilos del juego',
        'perf_help': 'Más FPS y más uso de la GPU. Si ves errores gráficos o el juego se cierra, '
                     'desactivalos de a uno para comparar y contanos en un issue.',
        'smart_pending': 'Saltar dibujos mientras su shader compila',
        'smart_pending_help': 'Casi sin tirones la primera vez que jugás un nivel. Desactivalo si ves objetos, '
                              'luces o sombras que faltan o parpadean al entrar en zonas nuevas: el juego '
                              'espera cada shader (más tirones, imagen siempre completa).',
        'precompile': 'Precompilar shaders al iniciar',
        'precompile_help': 'Antes de jugar prepara todos los shaders de la caché (lo que ya jugaste): '
                           'menos tirones al volver a esas zonas, pero el inicio tarda más y usa más memoria. '
                           'No ayuda en zonas que nunca jugaste.',
        'frames_queued': 'Frames en cola', 'queued_1': '1 (menor latencia)', 'queued_2': '2', 'queued_0': 'Sin límite',
        'reset_all': 'Restaurar todas las opciones',
        'update_available': 'Nueva versión {version} disponible', 'update_now': 'Actualizar ahora',
        'update_notes': 'Ver novedades', 'update_later': 'Más tarde',
        'update_confirm': 'Se va a descargar la versión {version} ({size}) y el launcher se va a reiniciar.\n'
                          'Tus partidas, opciones y caché (carpeta user) no se tocan.\n\n¿Actualizar ahora?',
        'update_downloading': 'Descargando actualización… {percent} %',
        'update_unpacking': 'Preparando actualización…',
        'update_failed': 'No se pudo actualizar: {error}\n\nPodés descargarla a mano desde GitHub.',
        'update_none': 'Ya tenés la última versión ({version}).',
        'update_check_failed': 'No se pudo buscar actualizaciones: {error}',
        'update_close_game': 'Cerrá el juego antes de actualizar.',
        'update_previous_failed': 'La última actualización no se pudo aplicar. Probá de nuevo o descargala desde GitHub.',
        'check_updates': 'Buscar actualizaciones al iniciar', 'check_updates_now': 'Buscar actualizaciones',
        'discord_rpc': 'Mostrar en Discord que estás jugando',
        'reshade': 'ReShade',
        'reshade_help': 'Instalalo con el instalador de ReShade sobre bin\\KillzoneShadowFall.exe (no sobre el '
                        'launcher) y elegí Vulkan. Con el escalado, los efectos que usan profundidad pueden no '
                        'alinearse; si la generación de frames falla con ReShade, desactivala.',
        'reshade_found': 'Estado: instalado en bin.', 'reshade_missing': 'Estado: no instalado.',
        'open_bin': 'Abrir carpeta bin',
        'shortcut': 'Acceso directo en el escritorio',
        'shortcut_help': 'Abre el juego directo, sin el launcher, con las opciones guardadas.',
        'shortcut_create': 'Crear', 'shortcut_done': 'Acceso directo creado en el escritorio.',
        'shortcut_failed': 'No se pudo crear el acceso directo: {error}',
        'game_language': 'Idioma del juego',
        'game_language_auto': 'Automático (el de Windows si el juego lo trae)',
        'game_language_help': 'Textos y voces. Solo aparecen los idiomas que trae tu copia del juego; cada '
                              'región trae los suyos (la americana: inglés y francés).',
        'discord_rpc_help': 'Discord muestra "Killzone Shadow Fall" y el tiempo jugado en tu perfil (Discord '
                            'tiene que estar abierto).',
        'updates': 'Actualizaciones',
        'reset_all_confirm': '¿Volver todas las opciones gráficas, avanzadas y de controles a los valores por defecto?',
        'language': 'Idioma',
        'game_ended': 'El juego se cerró con un error (código {code}).\n\n¿Abrir la carpeta de logs para ver qué pasó?',
        'hint_memory': 'Windows no pudo reservar la memoria del juego. Agrandá el archivo de paginación '
                       '(o dejalo administrado por Windows) y cerrá otros programas pesados.',
        'hint_vram': 'La placa de video se quedó sin memoria (VRAM). Bajá la resolución, desactivá '
                     'la generación de frames y usá un escalador en modo Rendimiento.',
        'hint_device_lost': 'El driver de video reinició la GPU (dispositivo perdido). Actualizá el driver; '
                            'si sigue pasando, desactivá la generación de frames y FSR 4/DLSS.',
        'hint_game_fatal': 'El juego se detuvo con este error: {message}',
        'hint_data_file': 'Falta o está dañado un archivo del juego. Volvé a extraer el juego (y la '
                          'actualización) en una carpeta corta, por ejemplo C:\\Juegos\\KZ: algunos extractores '
                          'saltean los archivos de nombre largo. Si tu copia es de otra región, elegí su idioma en '
                          'Avanzado → Idioma del juego.',
        'hint_gpu_assert': 'Falló una comprobación del renderizador. Si se repite en el mismo lugar, '
                           'reportalo en GitHub con los dos archivos de log.',
        'cant_start': 'No se pudo iniciar el juego:\n{error}',
        'fix_folder': 'Primero elegí una carpeta del juego válida (pestaña Juego).',
        'footer_hint': 'Port nativo para Windows · usa tu propio dump',
        'donate': 'Apoyar con PayPal',
        'discord_join': 'Ports Discord',
    },
    'en': {
        'tab_game': 'Game', 'tab_graphics': 'Graphics', 'tab_controls': 'Controls', 'tab_advanced': 'Advanced',
        'tab_cheats': 'Cheats',
        'cheats': 'Cheats', 'cheats_help': 'They only change the player. They can also be changed in game, at '
        'once, from the menu (Insert) > Cheats.',
        'cheat_god': 'God mode', 'cheat_god_help': 'You take no damage.',
        'cheat_ammo': 'Infinite ammo', 'cheat_ammo_help': 'Reserve ammo and grenades are never used up.',
        'cheat_no_reload': 'No reload', 'cheat_no_reload_help': 'The magazine never empties.',
        'cheat_sprint': 'Unlimited sprint', 'cheat_sprint_help': 'Stamina refills while you sprint.',
        'cheat_echo': 'Continuous Tactical Echo', 'cheat_echo_help': 'The OWL scan keeps pulsing. Use it once '
        'after each load.',
        'debug': 'Debug',
        'cheat_chapters': 'Unlock all chapters', 'cheat_chapters_help': 'All of them in Chapter Select; your saved '
        'progress is unchanged.',
        'free_camera': 'Free camera', 'free_camera_help': 'In game: F9 toggles it, F10 returns to the player. WASD '
        'move, Q / E down / up, mouse or arrows look, Shift faster, Ctrl slower. Gamepad: sticks, L1 / R1 down / '
        'up, R2 faster, L2 slower (turn it on from the menu > Cheats). The player stands still meanwhile.',
        'play': 'PLAY', 'playing': 'IN GAME', 'preparing': 'Starting…',
        'game_folder': 'Game folder',
        'game_folder_help': f'The folder of your PS4 dump (version {GAME_VERSION}; {TITLE_ID}, the tested one, or '
                            f'{", ".join(sorted(OTHER_TITLE_IDS))}, untested): the one holding eboot.bin.',
        'update_folder': 'Update folder',
        'update_folder_help': f'Your base dump is not version {GAME_VERSION}: choose the folder of update {GAME_VERSION} (CUSA00190-patch).',
        'browse': 'Browse…', 'choose_game': 'Choose the game folder (the one with eboot.bin)',
        'choose_update': 'Choose the update folder',
        'no_folder': 'No game folder chosen yet.',
        'no_eboot': 'There is no eboot.bin in that folder.',
        'no_sfo': 'sce_sys/param.sfo is missing: the dump is incomplete.',
        'wrong_game': 'That folder holds another game ({id}). This port is for Killzone Shadow Fall ({want}).',
        'other_region': 'Release {id}: untested (the port was built and tested on CUSA00190), it may work; tell us how it goes on GitHub.',
        'other_region_jp': 'Release {id} (Japanese): untested (the port was built and tested on CUSA00190); Japanese texts may look wrong or be missing.',
        'need_update': 'Version {ver}: update {want} is missing.',
        'bad_update': 'The update folder is not {want} version {ver_want} (it is {id} {ver}).',
        'update_no_eboot': 'The update is the right one, but eboot.bin is missing in {folder}. '
                           'Extract the whole update PKG again.',
        'ready': 'Ready: {title} · {id} · version {ver}',
        'ready_update': 'Ready: {title} · {id} · version {ver} (with update)',
        'fov': 'Field of view (FOV)',
        'fov_help': "Percent of the game's original field of view (100% = as on PS4). "
                    'Aiming zoom widens in the same proportion.',
        'tools': 'Folders and maintenance',
        'open_saves': 'Saved games', 'open_shots': 'Screenshots', 'open_logs': 'Logs',
        'clear_cache': 'Clear cache',
        'cache_size': 'Cache: {size}. It is rebuilt while playing (the first start and new areas take longer).',
        'clear_confirm': 'The shader cache and the prepared game image ({size}) will be deleted.\n'
                         'Your saved games are kept. Continue?',
        'cleared': 'Cache cleared.',
        'busy_running': 'Close the game first.',
        'display_mode': 'Display mode', 'mode_windowed': 'Windowed', 'mode_borderless': 'Borderless window',
        'mode_fullscreen': 'Fullscreen',
        'window_size': 'Window size',
        'window_size_help': 'Windowed mode only. The game image is scaled to the window.',
        'render_res': 'Render resolution', 'res_native': '1080p (as on PS4)',
        'ultrawide': 'Ultrawide (beta)', 'ultrawide_off': 'Off (16:9)',
        'ultrawide_help': 'For 21:9 or 32:9 monitors: the picture fills the screen and shows more at the sides '
                          '(Hor+), with the HUD, menus and videos in the middle at 16:9. Uses FSR / DLSS '
                          '(Advanced → Method); replaces the render resolution. The scene is drawn with up '
                          'to 1080 lines and FSR / DLSS take it to the output. Applied at start.',
        'render_res_help': 'Higher = sharper, more GPU and VRAM. Method under Advanced. Applied at start.',
        'fps_limit': 'Frame rate limit', 'vsync': 'VSync (vertical sync)',
        'fps_unlimited': 'Unlimited (experimental)', 'fps_experimental': 'experimental',
        'fps_limit_help': '144 / 240 / unlimited: experimental. On a 60 Hz monitor, no VSync.',
        'frame_gen': 'Frame generation', 'frame_gen_off': 'Off',
        'frame_gen_fsr3': 'AMD FSR 3.1', 'frame_gen_dlss': 'NVIDIA DLSS (RTX 40/50)',
        'frame_gen_help': 'Experimental. Makes up a frame between each pair of the game\'s frames: almost twice '
                          'the FPS on screen, with a little more latency. Best when the game already runs at 40 '
                          'FPS or more. FSR 3.1 works on any card; DLSS on RTX 40/50 only (others get FSR 3.1). '
                          'Not with the classic method or at 720p. On a 60 Hz monitor turn VSync off.',
        'vsync_help': 'Prevents tearing. Off: slightly lower latency.',
        'anisotropy': 'Anisotropic filtering', 'aniso_game': 'Game default',
        'mip_bias': 'Distant texture sharpness', 'mip_game': 'Game default',
        'mip_sharper': 'sharper', 'mip_softer': 'softer',
        'mip_bias_help': 'Sharper: more distant detail, a bit of shimmer. Softer: the opposite.',
        'lod_distance': 'Detail distance (LOD)',
        'lod_labels': ['50% (faster)', '75%', '100% (as on PS4)', '150%', '200%', '300% (less pop-in)'],
        'lod_distance_help': 'How far characters and objects keep their detailed version. Higher: less '
                             'pop-in, more GPU and CPU load. Can be changed in game (Insert).',
        'sharpen': 'Sharpening (RCAS)', 'show_fps': 'Show FPS on screen',
        'fsr1': 'FSR 1 upscaling',
        'fsr1_help': 'For screens above 1080p (1440p, 4K): sharper than plain stretching. No change at 1080p.',
        'graphics_note': 'In game, Insert opens the port menu (on a controller: R3 + L2).',
        'effects': 'Effects', 'effects_help': 'Applied when the game starts; camera shake also changes at once from the in-game menu (Insert). Weapon recoil stays.',
        'motion_blur': 'Motion blur', 'depth_of_field': 'Depth of field',
        'bloom': 'Bloom (glow around lights)', 'film_grain': 'Film grain',
        'camera_shake': 'Camera shake', 'camera_shake_help': 'Explosions, hits. Weapon recoil stays.',
        'keyboard': 'Keyboard', 'controller': 'Controller',
        'press_key': 'Press a key…', 'reset_defaults': 'Defaults',
        'controls_help': 'Click a key, then press a key, a mouse button or the wheel (Esc cancels). Move: WASD. Camera: mouse. D-pad: arrow keys.\n'
                         'Touchpad (OWL drone): hold Delete and drag the mouse. Tab / Backspace: left / right '
                         'touchpad click.',
        'controller_help': 'The controller button that acts as each PS4 button. Triggers and sticks are fixed.',
        'vibration': 'Vibration',
        'dead_zone': 'Stick dead zone', 'pad_speaker': 'Controller speaker',
        'quick_menu': 'Hold Select/Share: quick menu', 'quick_menu_hint': 'Tips at startup',
        'menu_combo': 'Buttons that open the port menu',
        'menu_combos': ['R3 + L2', 'L3 + R3', 'Share + Options', 'None (Insert only)'],
        'quick_menu_help': 'Holding it about a second opens Resume / Settings / Exit game from the controller; '
                           'a short press still works as usual (touchpad). Tips: a short note at startup on '
                           'using the OWL drone with the keyboard (Delete + mouse) and the quick menu.',
        'pad_extra_help': 'Dead zone: raise it if the camera or character moves on its own (worn stick). '
                          'Controller speaker: sounds the PS4 sends to the controller speaker; here they '
                          'play on your speakers (0% = off).',
        'prompt_style': 'In-game button icons',
        'prompt_styles': ['Automatic (last device and controller type)', 'Keyboard and mouse', 'PS4', 'Xbox'],
        'prompt_style_help': 'Game prompts show the bound key (or the mouse), or Xbox buttons, instead of the '
                             'PS4 button. On automatic, a PlayStation controller shows PS4 and any other one '
                             'Xbox. Sticks keep their PS4 icons.',
        'mouse': 'Mouse', 'mouse_look': 'Aim with the mouse',
        'mouse_look_help': "The mouse moves the camera 1:1, without a stick's dead zone or limit (recommended "
                           'sensitivity: about 1); left click fires (R2), right click aims (L2). Fast turns '
                           "raises the game's turn speed limit while the mouse moves (x6 by default). "
                           'Left Alt frees the cursor.',
        'mouse_sens': 'Sensitivity', 'mouse_invert': 'Invert Y axis', 'mouse_turn': 'Fast turns',
        'advanced_help': 'Only if something looks wrong or the game misbehaves. When in doubt, leave it as it is.',
        'async_shaders': 'Compile new shaders in the background',
        'async_help': 'Fewer stutters in new areas; an object may appear a moment later.',
        'stale_readback': 'GPU readbacks without waiting',
        'stale_help': 'More FPS. Turn it off if lighting or objects look wrong.',
        'deferred_readback': 'Deferred GPU readbacks',
        'deferred_help': 'Fewer waits. Turn it off if textures or shadows look wrong.',
        'upscale_quality': 'FSR / DLSS quality',
        'uq_native': 'Native (best quality)', 'uq_quality': 'Quality', 'uq_balanced': 'Balanced',
        'uq_performance': 'Performance', 'uq_ultra': 'Ultra performance',
        'upscale_quality_help': 'With FSR or DLSS (Advanced → Resolution method) or frame generation: the 3D '
                                'scene renders smaller and FSR / DLSS rebuild it; HUD, menus and videos stay sharp. '
                                'More FPS when your graphics card is the limit (modest GPU, 1440p/4K); no gain '
                                'when the CPU is. Native: the scene at 1080p.',
        'res_method': 'Resolution method',
        'res_method_upscale': 'AMD FSR 3.1 (recommended)',
        'res_method_dlss': 'NVIDIA DLSS (RTX only)',
        'res_method_fsr4': 'AMD FSR 4 (best quality)',
        'fsr4_files': 'FSR 4 files',
        'fsr4_ready': 'Installed',
        'fsr4_missing': 'Not installed (about 30 MB)',
        'fsr4_download': 'Download',
        'fsr4_downloading': 'Downloading… {done}/{total}',
        'fsr4_failed': 'Download failed: {error}',
        'fsr4_help': "FSR 4 uses AMD's AI model (INT8): sharper and steadier than FSR 3.1, a bit heavier. Runs on "
                     'cards with Vulkan 1.3 and 8-bit integer dot product (RTX 20 or newer, Radeon RX '
                     '6000/7000/9000, Arc). The files come from FireBurn/Q2RTX on GitHub (built from AMD’s '
                     'MIT-licensed FidelityFX source). Without them or on other cards it uses FSR 3.1.',
        'res_method_patch': 'Classic: game patch',
        'res_method_help': 'Only matters above 1080p. FSR 3.1 (recommended): the game renders at 1080p and FSR '
                           'takes the image to the chosen resolution, with sharp HUD, menus and videos. DLSS: the '
                           'same with NVIDIA DLSS (GeForce RTX; other cards get FSR 3.1). Classic: the game itself '
                           'renders at that resolution; heavier, and lighting flicker, a shadow following the '
                           'camera and zoomed-in videos may appear.',
        'perf_heading': 'Performance',
        'draw_pipe': 'Parallel drawing (2 GPU threads)',
        'nowait_writes': 'Game copies without waiting for the GPU',
        'fast_locks': 'Fast synchronization between game threads',
        'perf_help': 'More FPS and higher GPU usage. If you see graphics errors or the game closes, '
                     'turn them off one at a time to compare and tell us in an issue.',
        'smart_pending': 'Skip draws while their shader compiles',
        'smart_pending_help': 'Almost no stutter the first time you play a level. Turn it off if objects, lights '
                              'or shadows go missing or flicker when you reach new areas: the game then waits '
                              'for every shader (more stutter, always a complete picture).',
        'precompile': 'Precompile shaders at startup',
        'precompile_help': 'Prepares every shader in the cache (what you already played) before the game: '
                           'less stutter when you return to those areas, but a longer start and more memory. '
                           'It does not help in areas you never played.',
        'frames_queued': 'Frames queued', 'queued_1': '1 (lowest latency)', 'queued_2': '2', 'queued_0': 'Unlimited',
        'reset_all': 'Restore all options',
        'update_available': 'New version {version} available', 'update_now': 'Update now',
        'update_notes': "What's new", 'update_later': 'Later',
        'update_confirm': 'Version {version} ({size}) will be downloaded and the launcher will restart.\n'
                          'Your saves, settings and cache (user folder) are kept.\n\nUpdate now?',
        'update_downloading': 'Downloading update… {percent} %',
        'update_unpacking': 'Preparing update…',
        'update_failed': 'The update failed: {error}\n\nYou can download it by hand from GitHub.',
        'update_none': 'You have the latest version ({version}).',
        'update_check_failed': 'Could not check for updates: {error}',
        'update_close_game': 'Close the game before updating.',
        'update_previous_failed': 'The last update could not be applied. Try again or download it from GitHub.',
        'check_updates': 'Check for updates at startup', 'check_updates_now': 'Check for updates',
        'discord_rpc': 'Show on Discord that you are playing',
        'reshade': 'ReShade',
        'reshade_help': 'Install it with the ReShade installer on bin\\KillzoneShadowFall.exe (not on the '
                        'launcher) and choose Vulkan. With upscaling, effects that use depth may not line up; if '
                        'frame generation fails with ReShade, turn it off.',
        'reshade_found': 'Status: installed in bin.', 'reshade_missing': 'Status: not installed.',
        'open_bin': 'Open bin folder',
        'shortcut': 'Desktop shortcut',
        'shortcut_help': 'Starts the game directly, without the launcher, with the saved options.',
        'shortcut_create': 'Create', 'shortcut_done': 'Shortcut created on the desktop.',
        'shortcut_failed': 'The shortcut could not be created: {error}',
        'game_language': 'Game language',
        'game_language_auto': "Automatic (Windows' language when the game has it)",
        'game_language_help': 'Texts and voices. Only the languages your copy of the game has are listed; '
                              'each region has its own (the US release: English and French).',
        'discord_rpc_help': 'Discord shows "Killzone Shadow Fall" and the time played on your profile (Discord '
                            'must be open).',
        'updates': 'Updates',
        'reset_all_confirm': 'Set every graphics, advanced and control option back to its default?',
        'language': 'Language',
        'game_ended': 'The game closed with an error (code {code}).\n\nOpen the logs folder to see what happened?',
        'hint_memory': 'Windows could not reserve the game memory. Enlarge the page file (or let '
                       'Windows manage it) and close other heavy programs.',
        'hint_vram': 'The graphics card ran out of memory (VRAM). Lower the resolution, turn off frame '
                     'generation and use an upscaler in Performance mode.',
        'hint_device_lost': 'The graphics driver reset the GPU (device lost). Update the driver; if it '
                            'keeps happening, turn off frame generation and FSR 4/DLSS.',
        'hint_game_fatal': 'The game stopped with this error: {message}',
        'hint_data_file': 'A game file is missing or damaged. Extract the game (and its update) again into a '
                          'short folder such as C:\\Games\\KZ: some extractors skip files with long names. If your '
                          'copy is from another region, choose its language in Advanced → Game language.',
        'hint_gpu_assert': 'A renderer check failed. If it happens again at the same place, report it '
                           'on GitHub with both log files.',
        'cant_start': 'The game could not start:\n{error}',
        'fix_folder': 'Choose a valid game folder first (Game tab).',
        'footer_hint': 'Native Windows port · uses your own dump',
        'donate': 'Support with PayPal',
        'discord_join': 'Ports Discord',
    },
}
LANG = 'es'


def crash_hint(run_dir):
    """A likely cause of a crash from the run's errors.log, as a sentence for the player, or ''."""
    try:
        text = (Path(run_dir) / 'errors.log').read_text(encoding='utf-8', errors='replace')[-200000:]
    except OSError:
        return ''
    # The game's own fatal error message (probe.c prints it from Killzone's error handler).
    for line in text.splitlines():
        if line.startswith('Game fatal error: '):
            message = line[len('Game fatal error: '):].strip()[:300]
            hint = tr('hint_game_fatal', message=message)
            if 'cache:lumps' in message or 'data file' in message:
                hint += '\n\n' + tr('hint_data_file')
            return hint
    checks = (
        ('hint_memory', ('of guest memory', 'page file large enough')),
        ('hint_vram', ('ErrorOutOfDeviceMemory', 'OutOfDeviceMemory', 'out of video memory')),
        ('hint_device_lost', ('ErrorDeviceLost', 'DeviceLost', 'Device lost')),
        ('hint_gpu_assert', ('GPU library assertion failed',)),
    )
    for key, needles in checks:
        if any(needle in text for needle in needles):
            return tr(key)
    return ''


def game_ended_text(code, run_dir):
    """The crash message with a hint at its likely cause."""
    hint = crash_hint(run_dir) if run_dir else ''
    return tr('game_ended', code=code) + (f'\n\n{hint}' if hint else '')


def tr(key, **values):
    text = TEXTS.get(LANG, TEXTS['en']).get(key) or TEXTS['en'].get(key, key)
    return text.format(**values) if values else text


def windows_language():
    if WINDOWS:
        try:
            return 'es' if (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff) == 0x0a else 'en'
        except (AttributeError, OSError):
            pass
    return 'es' if os.environ.get('LANG', '').startswith('es') else 'en'


# ---------------------------------------------------------------------------------------------
# Command line roles of the frozen executable.

def attach_stdio():
    """A windowed executable starts without sys.stdout; inherited handles (log files) still exist."""
    if not WINDOWS:
        return
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


def run_role(argv):
    """--run [args]: run.py; --script <file> [args]: a preparation script. Returns an exit code."""
    attach_stdio()
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', line_buffering=True)
        except (AttributeError, ValueError):
            pass
    if argv[0] == '--script':
        path, sys.argv = argv[1], argv[1:]
    else:
        path = str(PORT_DIR / 'run.py')
        sys.argv = [path, *argv[1:]]
    try:
        runpy.run_path(path, run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    return 0


def run_command():
    return [sys.executable, '--run'] if FROZEN else [sys.executable, str(PORT_DIR / 'run.py')]


# ---------------------------------------------------------------------------------------------
# Game folder.

def read_sfo(path):
    from prepare import read_sfo as read
    return read(path)


def update_folder_root(update_dir):
    """The folder of the update that holds sce_sys/param.sfo: the chosen one, or the one folder
    inside it that does (some PKG extractors add a level, issue #70)."""
    root = Path(update_dir)
    if (root / 'sce_sys' / 'param.sfo').is_file():
        return root
    try:
        inner = [d for d in root.iterdir() if d.is_dir() and (d / 'sce_sys' / 'param.sfo').is_file()]
    except OSError:
        inner = []
    return inner[0] if len(inner) == 1 else root


# The game's language is the PS4 system language (runtime GOW3_LANGUAGE, issue #67); each one it has
# is a folder of localcachepink/lumps. A dump only holds the languages of its region (the US release:
# english and french), and asking for a missing one stops the game when the level loads.
GAME_LANGUAGES = (  # (lumps folder, PS4 language id, name in that language, Windows primary language)
    ('english', 1, 'English', 0x09), ('french', 2, 'Français', 0x0c), ('spanish', 3, 'Español', 0x0a),
    ('german', 4, 'Deutsch', 0x07), ('italian', 5, 'Italiano', 0x10), ('dutch', 6, 'Nederlands', 0x13),
    ('portuguese', 7, 'Português', 0x16), ('russian', 8, 'Русский', 0x19), ('korean', 9, '한국어', 0x12),
    ('japanese', 0, '日本語', 0x11), ('finnish', 12, 'Suomi', 0x0b), ('swedish', 13, 'Svenska', 0x1d),
    ('danish', 14, 'Dansk', 0x06), ('norwegian', 15, 'Norsk', 0x14), ('polish', 16, 'Polski', 0x15),
    ('turkish', 19, 'Türkçe', 0x1f), ('arabic', 21, 'العربية', 0x01))


def game_languages(game_dir, update_dir=''):
    """The GAME_LANGUAGES the dump (and its update) has folders for, in that order."""
    found = set()
    for folder in (game_dir, update_dir and update_folder_root(update_dir)):
        try:
            found |= {d.name.casefold() for d in (Path(folder) / 'localcachepink' / 'lumps').iterdir()
                      if d.is_dir()} if folder else set()
        except OSError:
            pass
    return [entry for entry in GAME_LANGUAGES if entry[0] in found]


def game_language_id(settings):
    """The PS4 language id for settings['game_language'] ('auto': Windows' language when the dump has
    it, else English, else the first one it has)."""
    available = game_languages(settings.get('game_dir'), settings.get('update_dir'))
    if not available:
        return 1
    chosen = settings.get('game_language', 'auto')
    for folder, ident, _, _ in available:
        if folder == chosen:
            return ident
    primary = 0
    if WINDOWS:
        try:
            primary = ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3ff
        except (AttributeError, OSError):
            pass
    for folder, ident, _, windows in available:
        if windows == primary:
            return ident
    return next((ident for folder, ident, _, _ in available if folder == 'english'), available[0][1])


def check_game(game_dir, update_dir):
    """(ok, message, needs_update): whether these folders hold Killzone Shadow Fall 01.81."""
    if not game_dir:
        return False, tr('no_folder'), False
    game = Path(game_dir)
    if not (game / 'eboot.bin').is_file():
        return False, tr('no_eboot'), False
    sfo = read_sfo(game / 'sce_sys' / 'param.sfo')
    if not sfo:
        return False, tr('no_sfo'), False
    title_id = str(sfo.get('TITLE_ID', '?'))
    if title_id not in TITLE_IDS:
        return False, tr('wrong_game', id=title_id, want=TITLE_ID), False
    title = clean_title(sfo.get('TITLE', 'Killzone Shadow Fall'))
    version = str(sfo.get('APP_VER', '?'))
    # Another region: playable message plus the warning (untested, Japanese texts).
    other = '' if title_id == TITLE_ID else ' ' + tr('other_region_jp' if title_id == 'CUSA00085' else 'other_region',
                                                     id=title_id)
    if version == GAME_VERSION:
        return True, tr('ready', title=title, id=title_id, ver=version) + other, False
    if not update_dir:
        return False, tr('need_update', ver=version, want=GAME_VERSION), True
    update_root = update_folder_root(update_dir)
    update = read_sfo(update_root / 'sce_sys' / 'param.sfo') or {}
    up_id, up_ver = str(update.get('TITLE_ID', '?')).strip(), str(update.get('APP_VER', '?')).strip()
    if up_id not in TITLE_IDS or up_ver != GAME_VERSION:
        return False, tr('bad_update', want=title_id, ver_want=GAME_VERSION, id=up_id, ver=up_ver), True
    if not (update_root / 'eboot.bin').is_file():
        return False, tr('update_no_eboot', folder=str(update_root)), True
    return True, tr('ready_update', title=title, id=title_id, ver=up_ver) + other, True


def clean_title(title):
    title = ''.join(ch for ch in str(title) if ch.isprintable() and ch not in '™®�')
    return ' '.join(word.capitalize() for word in title.split()) or 'Killzone Shadow Fall'


# ---------------------------------------------------------------------------------------------
# Settings.

GRAPHICS_DEFAULTS = {
    'display_mode': 'windowed', 'render_resolution': 'native', 'fps_limit': '60', 'vsync': '1', 'anisotropy': '16',
    'rcas': '0', 'fsr1': '0', 'mip_lod_bias': '0', 'lod_distance': '100',
    'show_fps': '0', 'vibration': '100', 'async_shaders': '1', 'stale_readback': '1',
    'deferred_readback': '1', 'frames_queued': '1',
    'motion_blur': '1', 'depth_of_field': '1', 'bloom': '1', 'film_grain': '1', 'camera_shake': '1',
    'cheat_god': '0', 'cheat_ammo': '0', 'cheat_no_reload': '0', 'cheat_sprint': '0', 'cheat_echo': '0',
    'cheat_chapters': '0',
}
LAUNCHER_DEFAULTS = {'game_dir': '', 'update_dir': '', 'language': '', 'window_size': '1920x1080',
                     'key_map': {}, 'pad_map': {}, 'mouse_look': True, 'mouse_sens': 1.0,
                     'mouse_invert': False, 'mouse_turn_boost': 6, 'check_updates': True,
                     'smart_pending': True, 'precompile': True,
                     'draw_pipe': True, 'nowait_writes': True, 'fast_locks': True,
                     'res_method': 'upscale', 'discord_rpc': True, 'pad_dead_zone': 0,
                     'pad_speaker': 100, 'fov': 100, 'quick_menu': True, 'quick_menu_hint': True,
                     'menu_combo': 0, 'prompt_style': 0, 'upscale_quality': 'native', 'game_language': 'auto'}
# Discord Rich Presence: the application "Killzone Shadow Fall" (its name is what Discord shows) and the
# key of its large image asset. Empty ID: no presence.
DISCORD_ID = '1562348304153706608'
DISCORD_IMAGE = 'logo'
DISCORD_DETAILS = 'PC Port by Lesca'
RES_METHODS = ('upscale', 'fsr4', 'dlss', 'patch')
# FSR / DLSS quality modes: the game's render size per output (resolution patches of that name in
# patches/Killzone_Shadow_Fall.xml, tools/make_kzsf_res_patches.py). The usual 67 / 58 / 50 / 33 %
# per axis, never above the game's own 1080p (larger patches bring lighting artifacts).
UPSCALE_QUALITIES = ('native', 'quality', 'balanced', 'performance', 'ultra')
UPSCALE_RENDER = {
    '1920x1080': {'quality': '1280x720', 'balanced': '1120x630', 'performance': '960x540', 'ultra': '640x360'},
    '2560x1440': {'quality': '1708x960', 'balanced': '1488x836', 'performance': '1280x720', 'ultra': '852x480'},
    '3200x1800': {'quality': '1920x1080', 'balanced': '1856x1044', 'performance': '1600x900', 'ultra': '1066x600'},
    '3840x2160': {'quality': '1920x1080', 'balanced': '1920x1080', 'performance': '1920x1080', 'ultra': '1280x720'},
}
# Ultrawide outputs (GOW3_OUTPUT_RES wider than 16:9): Hor+ in the loader (src/ultrawide_hook.h),
# the UI centred at 16:9 (gpu ui_composition.h Fit).
ULTRAWIDE = ('off', '2560x1080', '3440x1440', '3840x1600', '5120x1440')


def ultrawide_scene(output, quality):
    """The scene size of an ultrawide output: Gow3Settings::UpscaleSceneSize (gow3_settings.h)."""
    w, h = map(int, output.split('x'))
    height = {'quality': 720, 'balanced': 630, 'performance': 540, 'ultra': 360}.get(quality, 1080)
    height = height * min(h, 1080) // 1080
    return f'{int(height * w / h / 8 + 0.5) * 8}x{height}'


# Frame generation (GOW3_FRAME_GEN): FSR 3.1 on any GPU; DLSS Frame Generation for RTX 40/50
# (the port falls back to FSR 3.1 until it is supported).
FRAME_GENS = ('off', 'fsr3', 'dlss')
RESTART_CODE = 75  # the game's exit code for "Apply and restart" (src/probe.c runtime_restart)
# Render resolutions the port draws itself (GOW3_OUTPUT_RES) with the new method; 720p keeps the patch.
PORT_OUTPUTS = {'1440p': '2560x1440', '1800p': '3200x1800', '4K': '3840x2160'}
WINDOW_SIZES = ('1280x720', '1600x900', '1920x1080', '2560x1440', '3840x2160')
DISPLAY_MODES = ('windowed', 'borderless', 'fullscreen')
FPS_LIMITS = ('30', '60', '144', '240', '0')
# Above 60 the game is not vblank paced: vblank runs at 480 Hz and the present thread caps the rate
# (GOW3_FPS_LIMIT). The engine measures frame time with the guest TSC, so game speed is unchanged.
EXPERIMENTAL_FPS = ('144', '240', '0')
RENDER_RESOLUTIONS = ('720p', 'native', '1440p', '1800p', '4K')  # patches/Killzone_Shadow_Fall.xml
ANISOTROPY = ('0', '2', '4', '8', '16')
MIP_BIASES = ('-2.0', '-1.0', '-0.5', '0', '0.5', '1.0', '2.0')
LOD_DISTANCES = ('50', '75', '100', '150', '200', '300')
FRAMES_QUEUED = ('1', '2', '0')

# PS4 button, label, default key (SDL scancode name) and default controller button (SDL name).
BUTTONS = (
    ('cross', '✕', 'Space', 'south'), ('circle', '○', 'C', 'east'),
    ('square', '□', 'R', 'west'), ('triangle', '△', 'Q', 'north'),
    ('l1', 'L1', '1', 'leftshoulder'), ('r1', 'R1', '3', 'rightshoulder'),
    ('l2', 'L2', 'Mouse Right', ''), ('r2', 'R2', 'Mouse Left', ''),  # mouse look's own (runtime_pad.c)
    ('l3', 'L3', 'Left Shift', 'leftstick'), ('r3', 'R3', 'F', 'rightstick'),
    ('options', 'OPTIONS', 'Return', 'start'),
    ('touchpad_left', 'Touch ←', 'Tab', ''), ('touchpad_right', 'Touch →', 'Backspace', 'back'),
    ('up', '↑', 'Up', 'dpup'), ('down', '↓', 'Down', 'dpdown'), ('left', '←', 'Left', 'dpleft'),
    ('right', '→', 'Right', 'dpright'),
    # The left stick's keys and touchpad swipes (runtime_pad.c move_*, swipe_*; issues #83, #89, #92).
    ('move_forward', 'Move ↑', 'W', ''), ('move_back', 'Move ↓', 'S', ''),
    ('move_left', 'Move ←', 'A', ''), ('move_right', 'Move →', 'D', ''),
    ('swipe_up', 'Swipe ↑', '', ''), ('swipe_down', 'Swipe ↓', '', ''),
    ('swipe_left', 'Swipe ←', '', ''), ('swipe_right', 'Swipe →', '', ''),
)
# Controller buttons a PS4 button can take: SDL name and the label shown (Xbox / PlayStation).
PAD_BUTTONS = (
    ('', '—'), ('south', 'A / ✕'), ('east', 'B / ○'), ('west', 'X / □'), ('north', 'Y / △'),
    ('leftshoulder', 'LB / L1'), ('rightshoulder', 'RB / R1'), ('leftstick', 'LS / L3'),
    ('rightstick', 'RS / R3'), ('start', 'Start / Options'), ('back', 'Back / Share'),
    ('dpup', 'D-pad ↑'), ('dpdown', 'D-pad ↓'), ('dpleft', 'D-pad ←'), ('dpright', 'D-pad →'),
    ('misc1', 'Misc / Mic'), ('right_paddle1', 'Paddle R1'), ('left_paddle1', 'Paddle L1'),
    ('right_paddle2', 'Paddle R2'), ('left_paddle2', 'Paddle L2'),
)
# Keys the game itself or the port already uses: not offered for buttons.
TK_MOUSE = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 4: 'Mouse 4', 5: 'Mouse 5'}
RESERVED_KEYS = {'Insert', 'Delete', 'F9', 'F11', 'F12', 'Escape'}
# Tk keysyms that differ from SDL scancode names.
TK_TO_SDL = {
    'space': 'Space', 'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl',
    'Control_R': 'Right Ctrl', 'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt', 'Return': 'Return', 'Tab': 'Tab',
    'BackSpace': 'Backspace', 'Caps_Lock': 'CapsLock', 'comma': ',', 'period': '.', 'slash': '/',
    'semicolon': ';', 'apostrophe': "'", 'bracketleft': '[', 'bracketright': ']', 'minus': '-',
    'equal': '=', 'grave': '`', 'backslash': '\\', 'Home': 'Home', 'End': 'End', 'Prior': 'PageUp',
    'Next': 'PageDown', 'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Insert': 'Insert',
    'Delete': 'Delete', 'Escape': 'Escape', 'KP_Enter': 'Keypad Enter', 'KP_Add': 'Keypad +',
    'KP_Subtract': 'Keypad -', 'KP_Multiply': 'Keypad *', 'KP_Divide': 'Keypad /', 'KP_Decimal': 'Keypad .',
}
# Tk keysyms on the number pad with Num Lock off.
for _digit, _name in enumerate(('KP_Insert', 'KP_End', 'KP_Down', 'KP_Next', 'KP_Left', 'KP_Begin',
                                'KP_Right', 'KP_Home', 'KP_Up', 'KP_Prior')):
    TK_TO_SDL[_name] = f'Keypad {_digit}'
    TK_TO_SDL[f'KP_{_digit}'] = f'Keypad {_digit}'


FOV_MIN, FOV_MAX = 80, 200


def fov_percent(value):
    """The FOV setting rounded to the patches' 5% steps, within FOV_MIN..FOV_MAX (100 if unset)."""
    try:
        return max(FOV_MIN, min(FOV_MAX, int(round(float(value) / 5) * 5)))
    except (TypeError, ValueError):
        return 100


def turn_boost(value):
    """The fast-turn factor of direct mouse look (1..12, 6 if unset)."""
    try:
        return max(1, min(12, int(round(float(value)))))
    except (TypeError, ValueError):
        return 6


def sdl_key_name(keysym):
    """The SDL scancode name of a Tk keysym, or None when the port cannot read that key."""
    if keysym in TK_TO_SDL:
        return TK_TO_SDL[keysym]
    if len(keysym) == 1 and keysym.isalnum():
        return keysym.upper()
    if keysym.startswith('F') and keysym[1:].isdigit() and 1 <= int(keysym[1:]) <= 12:
        return keysym
    return None


# Controls the in-game menu (Insert, quick menu) also changes: kept in gow3.ini as well, which the
# game reads live. On load gow3.ini wins (it may hold a change made in game); every save writes
# both. key: (type, gow3.ini text of a value).
SHARED_SETTINGS = {
    'mouse_sens': (float, lambda v: f'{float(v):.1f}'),
    'mouse_turn_boost': (int, lambda v: str(int(v))),
    'mouse_invert': (bool, lambda v: '1' if v else '0'),
    'fov': (int, lambda v: str(int(v))),
    'pad_dead_zone': (int, lambda v: str(int(v))),
    'pad_speaker': (int, lambda v: str(int(v))),
    'quick_menu': (bool, lambda v: '1' if v else '0'),
    'quick_menu_hint': (bool, lambda v: '1' if v else '0'),
    'menu_combo': (int, lambda v: str(max(0, min(3, int(v))))),
    'prompt_style': (int, lambda v: str(int(v))),
    # Quality mode: the in-game menu switches it live between reduced modes, or for the next start.
    'upscale_quality': (str, lambda v: v if v in UPSCALE_QUALITIES else 'native'),
}


def write_launcher_file(settings):
    USER_DIR.mkdir(parents=True, exist_ok=True)
    LAUNCHER_FILE.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def create_play_shortcut():
    """A desktop shortcut that starts the game with --play (issues #79, #86). Its path."""
    exe = Path(sys.executable) if getattr(sys, 'frozen', False) else PORT_DIR / 'Killzone Shadow Fall.exe'
    icon = PORT_DIR / 'launcher' / 'kzsf.ico'
    script = (
        "$d=[Environment]::GetFolderPath('Desktop');"
        "$l=Join-Path $d 'Killzone Shadow Fall (play).lnk';"
        "$s=(New-Object -ComObject WScript.Shell).CreateShortcut($l);"
        f"$s.TargetPath='{str(exe).replace(chr(39), chr(39) * 2)}';$s.Arguments='--play';"
        f"$s.WorkingDirectory='{str(PORT_DIR).replace(chr(39), chr(39) * 2)}';"
        + (f"$s.IconLocation='{str(icon).replace(chr(39), chr(39) * 2)}';" if icon.is_file() else '')
        + "$s.Save();$l")
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
                            capture_output=True, text=True, timeout=30,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode != 0 or not result.stdout.strip():
        raise OSError((result.stderr or 'PowerShell failed').strip().splitlines()[-1])
    return result.stdout.strip()


def load_launcher():
    try:
        data = json.loads(LAUNCHER_FILE.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        data = {}
    settings = {**LAUNCHER_DEFAULTS, **(data if isinstance(data, dict) else {})}
    for key in ('key_map', 'pad_map'):
        if not isinstance(settings[key], dict):
            settings[key] = {}
    from graphics_options import read_ini, write_keys
    ini = read_ini(SETTINGS_FILE)
    for key, (kind, _) in SHARED_SETTINGS.items():
        if key in ini:
            try:
                settings[key] = (ini[key] not in ('0', '') if kind is bool else ini[key] if kind is str
                                 else kind(float(ini[key])))
            except ValueError:
                pass
    # Upscaler / frame generation chosen in the in-game menu (gow3_overlay.cpp UpscalingSection).
    if ini.get('upscaling_from_menu') == '1':
        if ini.get('frame_gen') in FRAME_GENS:
            settings['frame_gen'] = ini['frame_gen']
        if ini.get('upscaler') in ('fsr3', 'fsr4', 'dlss') and settings.get('res_method') != 'patch':
            settings['res_method'] = {'fsr3': 'upscale'}.get(ini['upscaler'], ini['upscaler'])
        write_keys(SETTINGS_FILE, {'upscaling_from_menu': '0'})
        write_launcher_file(settings)
    return settings


def save_launcher(settings):
    write_launcher_file(settings)
    from graphics_options import write_keys
    if not SETTINGS_FILE.exists():
        SETTINGS_FILE.write_text('# Killzone Shadow Fall (PC port) settings: launcher and in-game menu (Insert)\n',
                                 encoding='utf-8')
    write_keys(SETTINGS_FILE, {key: text(settings[key]) for key, (_, text) in SHARED_SETTINGS.items()
                               if key in settings})


def load_graphics():
    from graphics_options import read_ini
    ini = read_ini(SETTINGS_FILE)
    values = {key: ini.get(key, value) for key, value in GRAPHICS_DEFAULTS.items()}
    # Values the launcher does not offer (the in-game menu has more) fall back to the closest one.
    if values['display_mode'] not in DISPLAY_MODES:
        values['display_mode'] = 'windowed'
    if values['render_resolution'] not in RENDER_RESOLUTIONS:
        values['render_resolution'] = 'native'
    if values['fps_limit'] not in FPS_LIMITS:
        values['fps_limit'] = '60'
    if values['anisotropy'] not in ANISOTROPY:
        values['anisotropy'] = '16'
    try:
        bias = float(values['mip_lod_bias'])
    except ValueError:
        bias = 0.0
    values['mip_lod_bias'] = min(MIP_BIASES, key=lambda b: abs(float(b) - bias))
    try:
        lod = float(values['lod_distance'])
    except ValueError:
        lod = 100.0
    values['lod_distance'] = min(LOD_DISTANCES, key=lambda d: abs(float(d) - lod))
    if values['frames_queued'] not in FRAMES_QUEUED:
        values['frames_queued'] = '1'
    return values


def save_graphics(values):
    from graphics_options import write_keys
    USER_DIR.mkdir(parents=True, exist_ok=True)
    if not SETTINGS_FILE.exists():
        SETTINGS_FILE.write_text('# Killzone Shadow Fall (PC port) settings: launcher and in-game menu (Insert)\n',
                                 encoding='utf-8')
    write_keys(SETTINGS_FILE, values)


def map_spec(mapping, column):
    """GOW3_KEY_MAP / GOW3_PAD_MAP for the buttons that differ from the defaults."""
    defaults = {name: (key, pad) for name, _, key, pad in BUTTONS}
    parts = []
    for name, value in mapping.items():
        if name in defaults and value != defaults[name][column]:
            parts.append(f'{name}={value}')
    return ','.join(parts)


# ---------------------------------------------------------------------------------------------
# Folders.

def cache_paths():
    return [USER_DIR / 'out', PS4_DIR / 'cache']


def folder_size(path):
    total = 0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


def human_size(size):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if size < 1024 or unit == 'GB':
            return f'{size:.0f} {unit}' if unit in ('B', 'KB') else f'{size:.1f} {unit}'
        size /= 1024
    return f'{size:.1f} GB'


def open_folder(path):
    path = Path(path)
    path.mkdir(parents=True, exist_ok=True)
    if WINDOWS:
        os.startfile(str(path))  # noqa: S606
    else:
        subprocess.Popen(['xdg-open', str(path)])


def adopt_region_saves():
    """Copies saves of another release (CUSA00002/...) into the shared CUSA00190 folder for each user
    that has none there yet (GOW3_SAVEDATA_ID). Nothing is moved or overwritten."""
    for user in (PS4_DIR / 'savedata').glob('*'):
        target = user / TITLE_ID
        if not user.is_dir() or target.exists():
            continue
        source = next((user / i for i in sorted(OTHER_TITLE_IDS) if (user / i).is_dir()), None)
        if source:
            try:
                shutil.copytree(source, target)
            except OSError:
                shutil.rmtree(target, ignore_errors=True)


def prune_logs():
    try:
        runs = sorted((p for p in LOGS_DIR.iterdir() if p.is_dir()), key=lambda p: p.name)
    except OSError:
        return
    for old in runs[:-KEEP_LOGS]:
        shutil.rmtree(old, ignore_errors=True)


# ---------------------------------------------------------------------------------------------
# Starting the game (both windows: this Tk one and kzsf_web.py).

def game_env(settings, graphics, needs_update):
    """The game's environment for these options."""
    env = dict(os.environ)
    env.update({
        'GOW3_GAME_DIR': settings['game_dir'],
        'GOW3_DATA_DIR': str(USER_DIR),
        'GOW3_USER_DIR': str(PS4_DIR),
        'GOW3_CONFIG': str(SETTINGS_FILE),
        'GOW3_WINDOW_SIZE': settings.get('window_size') or '1920x1080',
        'GOW3_KEY_MAP': map_spec(settings['key_map'], 0),
        'GOW3_PAD_MAP': map_spec(settings['pad_map'], 1),
        'PYTHONIOENCODING': 'utf-8',
        # Sensitivity, fast turns, invert Y, FOV, dead zone, controller speaker and the quick
        # menu: the game reads them from gow3.ini (SHARED_SETTINGS), live.
        'GOW3_MOUSE_LOOK': '1' if settings.get('mouse_look', True) else '0',
        # An "Apply and restart" in the game ends it with code 75: game_ended starts it again.
        'GOW3_LAUNCHER_RESTART': '1',
        'GOW3_DISCORD': '1' if DISCORD_ID and settings.get('discord_rpc', True) else '0',
        'GOW3_DISCORD_ID': DISCORD_ID,
        'GOW3_DISCORD_DETAILS': DISCORD_DETAILS,
        'GOW3_DISCORD_IMAGE': DISCORD_IMAGE,
        'GOW3_SMART_PENDING': '1' if settings.get('smart_pending', True) else '0',
        # Precompile: every cached pipeline built before the game, on all but two cores.
        'GOW3_PRECOMPILE': os.environ.get(
            'GOW3_PRECOMPILE', '1' if settings.get('precompile', True) else '0'),
        'GOW3_PARALLEL_WARMUP': os.environ.get('GOW3_PARALLEL_WARMUP', '1'),
        # Performance (2026-10-21 measurements): the two-stage draw pipeline, and libc's copy
        # routine (0x3d99a2) writing next to a compute shader's output without a GPU readback.
        # A variable set before the launcher starts (test .bat files) wins.
        'GOW3_DRAW_PIPE': os.environ.get(
            'GOW3_DRAW_PIPE', '1' if settings.get('draw_pipe', True) else '0'),
        'GOW3_NOWAIT_WRITES': os.environ.get(
            'GOW3_NOWAIT_WRITES', '3d99a2' if settings.get('nowait_writes', True) else ''),
        # PR #96 (clearfeather): rwlock bookkeeping split over 256 locks instead of one.
        'GOW3_RWLOCK_FAST': os.environ.get(
            'GOW3_RWLOCK_FAST', '1' if settings.get('fast_locks', True) else '0'),
        # One save folder for every release of the game: switching region keeps the saves.
        'GOW3_SAVEDATA_ID': TITLE_ID,
        'GOW3_LANGUAGE': os.environ.get('GOW3_LANGUAGE', str(game_language_id(settings))),
    })
    # Upscaling method (any saved value but 'patch'): above 1080p the game stays at 1080p (run.py
    # drops the patch) and FSR 3.1 reaches the output, with the UI and videos drawn at that size.
    # No host scene proxies (their round trips broke foliage lighting). The port jitters the scene
    # (its depth pre-pass included) for FSR / DLSS and skips the game's own temporal anti-aliasing
    # (run.py GOW3_GAME_TAA_PS); GOW3_JITTER=0 in the environment turns the jitter off.
    output = PORT_OUTPUTS.get(graphics['render_resolution'])
    method = settings.get('res_method', 'upscale')
    frame_gen = settings.get('frame_gen') if settings.get('frame_gen') in FRAME_GENS else 'off'
    # Frame generation at 1080p: FSR / DLSS as anti-aliasing at the game's own size (GOW3_NATIVE_AA),
    # the path frame generation takes its depth, motion and picture without UI from.
    # Quality modes (Graphics tab; GOW3_UPSCALE_QUALITY=<mode> overrides it for tests): the 3D
    # scene renders smaller (src/scene_res_hook.h), the UI and videos stay at the output size.
    quality = os.environ.get('GOW3_UPSCALE_QUALITY') or settings.get('upscale_quality', 'native')
    quality = quality if quality in UPSCALE_QUALITIES else 'native'
    native_aa = graphics['render_resolution'] == 'native' and (
        frame_gen != 'off' or (quality != 'native' and method != 'patch'))
    if native_aa:
        output = '1920x1080'
    # Quality modes: the scene renders smaller (GOW3_RENDER_RES, read by the loader) and FSR / DLSS
    # reach the output.
    render = UPSCALE_RENDER.get(output or '', {}).get(quality, '1920x1080')
    # Ultrawide: always through the port (the classic patch cannot widen the view); the scene at
    # the output's shape, Hor+ by the loader.
    ultrawide = settings.get('ultrawide') if settings.get('ultrawide') in ULTRAWIDE else 'off'
    if ultrawide != 'off':
        output, native_aa = ultrawide, False
        method = method if method != 'patch' else 'upscale'
        render = ultrawide_scene(ultrawide, quality)
    if output and (method != 'patch' or native_aa):
        # DLSS falls back to FSR 3.1 by itself when the card or the DLLs cannot run it.
        # FSR 4 falls back to FSR 3.1 the same way (no INT8 support, files missing: fsr4_problem).
        upscaler = {'dlss': 'dlss', 'fsr4': 'fsr4'}.get(method, 'fsr3')
        env.update({'GOW3_OUTPUT_RES': output, 'GOW3_UPSCALER': upscaler,
                    'GOW3_FSR4_DIR': os.environ.get('GOW3_FSR4_DIR', str(FSR4_DIR)),
                    'GOW3_SCENE_NATIVE': '1',
                    'GOW3_JITTER': os.environ.get('GOW3_JITTER', '1'),
                    'GOW3_FRAME_GEN': frame_gen})
        if native_aa:
            env['GOW3_NATIVE_AA'] = '1'
        else:
            env.pop('GOW3_NATIVE_AA', None)
        if render != '1920x1080':
            env['GOW3_RENDER_RES'] = render
        else:
            env.pop('GOW3_RENDER_RES', None)
        # The mode this start applies: the in-game menu compares its choice with it.
        env['GOW3_UPSCALE_QUALITY'] = quality
    else:
        # The classic resolution patch and 720p have no port-side frame path: no frame generation.
        env['GOW3_OUTPUT_RES'] = '1920x1080'
        env['GOW3_FRAME_GEN'] = 'off'
        for key in ('GOW3_UPSCALER', 'GOW3_SCENE_NATIVE', 'GOW3_JITTER', 'GOW3_NATIVE_AA', 'GOW3_RENDER_RES'):
            env.pop(key, None)
    if graphics['fps_limit'] in EXPERIMENTAL_FPS:
        env['GOW3_VBLANK_HZ'] = '0'
        env['GOW3_FPS_LIMIT'] = graphics['fps_limit']
    else:
        env.pop('GOW3_VBLANK_HZ', None)
        env.pop('GOW3_FPS_LIMIT', None)
    if needs_update and settings['update_dir']:
        env['GOW3_PATCH_DIR'] = str(update_folder_root(settings['update_dir']))
    else:
        env.pop('GOW3_PATCH_DIR', None)
    return env


def launch_game(settings, graphics, needs_update):
    """Saves the options and starts the game: (process, game.log, errors.log, run folder); OSError."""
    save_graphics(graphics)
    save_launcher(settings)  # the controls the game reads from gow3.ini (SHARED_SETTINGS)
    run_dir = LOGS_DIR / time.strftime('%Y-%m-%d_%H-%M-%S')
    run_dir.mkdir(parents=True, exist_ok=True)
    prune_logs()
    adopt_region_saves()
    env = game_env(settings, graphics, needs_update)
    out = open(run_dir / 'game.log', 'w', encoding='utf-8')
    err = open(run_dir / 'errors.log', 'w', encoding='utf-8')
    try:
        process = subprocess.Popen(run_command(), cwd=str(PORT_DIR), env=env, stdin=subprocess.DEVNULL,
                                   stdout=out, stderr=err, creationflags=NO_WINDOW)
    except OSError:
        out.close()
        err.close()
        raise
    return process, out, err, run_dir


# ---------------------------------------------------------------------------------------------
# Window.

class Launcher:
    def __init__(self, root, tk, ttk, filedialog, messagebox):
        self.root, self.tk, self.ttk = root, tk, ttk
        self.filedialog, self.messagebox = filedialog, messagebox
        self.settings = load_launcher()
        self.graphics = load_graphics()
        self.process = None
        self.capturing = None
        self.images = {}
        self.scale = max(1.0, root.winfo_fpixels('1i') / 96.0)
        root.title('Killzone Shadow Fall')
        root.configure(bg=BG)
        root.minsize(self.px(900), self.px(780))
        root.geometry(f'{self.px(980)}x{self.px(840)}')
        self.style()
        self.build()
        root.protocol('WM_DELETE_WINDOW', self.close)
        self.update_info = None
        if self.settings.get('check_updates', True):
            self.check_updates(quiet=True)
        if kzsf_update.failed_update(UPDATE_DIR):
            root.after(500, lambda: self.messagebox.showwarning('Killzone Shadow Fall', tr('update_previous_failed'),
                                                                 parent=root))

    def px(self, value):
        return int(value * self.scale)

    def font(self, size, weight='normal', family=UI_FONT):
        return (family, size, weight)

    def style(self):
        s = self.ttk.Style(self.root)
        s.theme_use('clam')
        s.configure('.', background=BG, foreground=TEXT, fieldbackground=FIELD, bordercolor=LINE,
                    lightcolor=LINE, darkcolor=LINE, troughcolor=FIELD, focuscolor=ACCENT, font=self.font(10))
        s.configure('TCombobox', arrowcolor=TEXT, padding=self.px(4), selectbackground=FIELD,
                    selectforeground=TEXT)
        s.map('TCombobox', fieldbackground=[('readonly', FIELD)], foreground=[('readonly', TEXT)],
              bordercolor=[('focus', ACCENT)], arrowcolor=[('active', ACCENT)])
        s.configure('TCheckbutton', background=PANEL, foreground=TEXT, indicatorbackground=FIELD,
                    indicatorforeground=ACCENT, padding=self.px(2))
        s.map('TCheckbutton', background=[('active', PANEL)], indicatorbackground=[('selected', FIELD)],
              indicatorforeground=[('selected', ACCENT)])
        s.configure('Horizontal.TScale', background=PANEL, troughcolor=FIELD, sliderthickness=self.px(14))
        self.root.option_add('*TCombobox*Listbox.background', FIELD)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', ACCENT_DARK)
        self.root.option_add('*TCombobox*Listbox.selectForeground', TEXT)
        self.root.option_add('*TCombobox*Listbox.font', self.font(10))

    # -- layout -----------------------------------------------------------------------------

    def build(self):
        tk = self.tk
        for child in self.root.winfo_children():
            child.destroy()
        self.banner = tk.Canvas(self.root, height=self.px(170), bg=BG, highlightthickness=0, bd=0)
        self.banner.pack(fill='x')
        self.banner.bind('<Configure>', lambda e: self.draw_banner())

        self.tabbar = tk.Frame(self.root, bg=BG)
        self.tabbar.pack(fill='x', padx=self.px(28))
        footer = tk.Frame(self.root, bg=BG)
        footer.pack(side='bottom', fill='x', padx=self.px(28), pady=(0, self.px(18)))
        self.body = tk.Frame(self.root, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        self.body.pack(fill='both', expand=True, padx=self.px(28), pady=(0, self.px(12)))
        tk.Label(footer, text='Lesca', fg=MARK, bg=BG, font=self.font(9, 'bold')).pack(side='left')
        tk.Label(footer, text=f'  v{VERSION}  ·  {tr("footer_hint")}', fg=MUTED, bg=BG,
                 font=self.font(9)).pack(side='left')
        # Optional support link (voluntary; the port is free): a small PayPal badge next to the version,
        # like the README's.
        donate = tk.Label(footer, text=f'♥  {tr("donate")}', fg='white', bg=DONATE_BG,
                          font=self.font(9, 'bold'), padx=self.px(10), pady=self.px(3), cursor='hand2')
        donate.pack(side='left', padx=(self.px(14), 0))
        donate.bind('<Button-1>', lambda e: webbrowser.open(DONATE_URL))
        donate.bind('<Enter>', lambda e: donate.configure(bg=DONATE_HOVER))
        donate.bind('<Leave>', lambda e: donate.configure(bg=DONATE_BG))
        discord = tk.Label(footer, text=tr('discord_join'), fg='white', bg=DISCORD_BG,
                           font=self.font(9, 'bold'), padx=self.px(10), pady=self.px(3), cursor='hand2')
        discord.pack(side='left', padx=(self.px(8), 0))
        discord.bind('<Button-1>', lambda e: webbrowser.open(DISCORD_URL))
        discord.bind('<Enter>', lambda e: discord.configure(bg=DISCORD_HOVER))
        discord.bind('<Leave>', lambda e: discord.configure(bg=DISCORD_BG))
        self.play_button = tk.Label(footer, text=tr('play'), fg=BG, bg=ACCENT, font=self.font(13, 'bold'),
                                    padx=self.px(46), pady=self.px(10), cursor='hand2')
        self.play_button.pack(side='right')
        self.play_button.bind('<Button-1>', lambda e: self.play())
        self.play_button.bind('<Enter>', lambda e: self.play_hover(True))
        self.play_button.bind('<Leave>', lambda e: self.play_hover(False))
        self.status = tk.Label(footer, text='', fg=MUTED, bg=BG, font=self.font(9))
        self.status.pack(side='right', padx=self.px(14))

        self.tabs = [('game', tr('tab_game'), self.game_tab), ('graphics', tr('tab_graphics'), self.graphics_tab),
                     ('controls', tr('tab_controls'), self.controls_tab),
                     ('cheats', tr('tab_cheats'), self.cheats_tab),
                     ('advanced', tr('tab_advanced'), self.advanced_tab)]
        self.tab_labels = {}
        for key, title, _ in self.tabs:
            label = tk.Label(self.tabbar, text=title.upper(), bg=BG, fg=MUTED, font=self.font(10, 'bold'),
                             padx=self.px(14), pady=self.px(9), cursor='hand2')
            label.pack(side='left')
            label.bind('<Button-1>', lambda e, k=key: self.show_tab(k))
            self.tab_labels[key] = label
        lang = tk.Frame(self.tabbar, bg=BG)
        lang.pack(side='right')
        for code in ('en', 'es'):
            label = tk.Label(lang, text=code.upper(), bg=BG, fg=ACCENT if LANG == code else MUTED,
                             font=self.font(9, 'bold'), padx=self.px(6), cursor='hand2')
            label.pack(side='right')
            label.bind('<Button-1>', lambda e, c=code: self.set_language(c))
        tk.Label(lang, text=tr('language') + ':', bg=BG, fg=MUTED, font=self.font(9)).pack(side='right')
        self.update_bar = None
        if getattr(self, 'update_info', None):
            self.show_update_bar()
        self.show_tab(getattr(self, 'current_tab', os.environ.get('KZSF_LAUNCHER_TAB', 'game')))
        self.refresh_game()
        self.update_play_state()

    def play_hover(self, inside):
        if self.process is None and self.game_ok:
            self.play_button.configure(bg=TEXT if inside else ACCENT)

    def show_tab(self, key):
        self.current_tab = key
        for name, label in self.tab_labels.items():
            label.configure(fg=TEXT if name == key else MUTED,
                            bg=PANEL if name == key else BG)
        for child in self.body.winfo_children():
            child.destroy()
        # The tab scrolls when the window is too small for it (bar and mouse wheel only then).
        canvas = self.tk.Canvas(self.body, bg=PANEL, highlightthickness=0, bd=0)
        bar = self.tk.Scrollbar(self.body, orient='vertical', command=canvas.yview)
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side='left', fill='both', expand=True)
        holder = self.tk.Frame(canvas, bg=PANEL)
        window = canvas.create_window(0, 0, window=holder, anchor='nw')
        frame = self.tk.Frame(holder, bg=PANEL)
        frame.pack(fill='both', expand=True, padx=self.px(24), pady=self.px(18))

        def fits():
            return holder.winfo_reqheight() <= canvas.winfo_height()

        def layout(_event=None):
            canvas.itemconfigure(window, width=canvas.winfo_width())
            canvas.configure(scrollregion=(0, 0, canvas.winfo_width(), holder.winfo_reqheight()))
            if fits():
                bar.pack_forget()
                canvas.yview_moveto(0)
            elif not bar.winfo_ismapped():
                bar.pack(side='right', fill='y', before=canvas)

        def wheel(event):
            if not fits():
                canvas.yview_scroll(int(-event.delta / 120) or (-1 if event.delta > 0 else 1), 'units')
        holder.bind('<Configure>', layout)
        canvas.bind('<Configure>', layout)
        canvas.bind('<Enter>', lambda _e: canvas.bind_all('<MouseWheel>', wheel))
        canvas.bind('<Leave>', lambda _e: canvas.unbind_all('<MouseWheel>'))
        dict((k, f) for k, _, f in self.tabs)[key](frame)

    def set_language(self, code):
        global LANG
        if code == LANG:
            return
        LANG = code
        self.settings['language'] = code
        save_launcher(self.settings)
        self.build()

    # -- banner -----------------------------------------------------------------------------

    def load_art(self):
        """The game's own background (sce_sys/pic0.png) for the banner, darkened on the left."""
        game = self.settings.get('game_dir')
        if not game:
            return None
        path = Path(game) / 'sce_sys' / 'pic0.png'
        key = (str(path), self.banner.winfo_width(), self.banner.winfo_height())
        if self.images.get('banner_key') == key:
            return self.images.get('banner')
        try:
            from PIL import Image, ImageTk
            width, height = key[1], key[2]
            with Image.open(path) as source:
                image = source.convert('RGB')
            scale = width / image.width
            image = image.resize((width, max(height, int(image.height * scale))), Image.LANCZOS)
            top = int((image.height - height) * 0.42)
            image = image.crop((0, top, width, top + height))
            shade = Image.new('L', (width, 1))
            shade.putdata([int(235 - 150 * min(1.0, x / (width * 0.75))) for x in range(width)])
            shade = shade.resize((width, height))
            bottom = Image.new('L', (1, height))
            bottom.putdata([int(120 * max(0.0, (y - height * 0.55) / (height * 0.45))) for y in range(height)])
            bottom = bottom.resize((width, height))
            from PIL import ImageChops
            mask = ImageChops.lighter(shade, bottom)
            image = Image.composite(Image.new('RGB', (width, height), BG), image, mask)
            photo = ImageTk.PhotoImage(image)
        except Exception:  # noqa: BLE001  (no Pillow, no picture: the plain banner remains)
            photo = None
        self.images['banner_key'], self.images['banner'] = key, photo
        return photo

    def draw_banner(self):
        c = self.banner
        c.delete('all')
        width, height = c.winfo_width(), c.winfo_height()
        art = self.load_art()
        if art:
            c.create_image(0, 0, image=art, anchor='nw')
        else:
            for i in range(0, height, 2):
                shade = int(10 + 14 * i / height)
                c.create_rectangle(0, i, width, i + 2, fill=f'#{shade // 2:02x}{shade:02x}{shade + 4:02x}', width=0)
        x = self.px(30)
        c.create_text(x, self.px(52), text='KILLZONE', anchor='w', fill=TEXT, font=self.font(36, 'bold', 'Segoe UI Black'))
        c.create_text(x + self.px(3), self.px(96), text='S H A D O W   F A L L', anchor='w', fill=TEXT,
                      font=self.font(19, 'normal', 'Segoe UI Light'))
        c.create_rectangle(x, self.px(120), x + self.px(64), self.px(123), fill=ACCENT, width=0)
        c.create_text(x, self.px(140), text='PC', anchor='w', fill=ACCENT, font=self.font(10, 'bold'))
        c.create_line(0, height - 1, width, height - 1, fill=LINE)

    # -- helpers ----------------------------------------------------------------------------

    def heading(self, parent, text, pady=(0, 6)):
        self.tk.Label(parent, text=text.upper(), bg=PANEL, fg=ACCENT, font=self.font(9, 'bold')).pack(
            anchor='w', pady=(self.px(pady[0]), self.px(pady[1])))

    def note(self, parent, text, color=MUTED, wrap=820):
        label = self.tk.Label(parent, text=text, bg=PANEL, fg=color, font=self.font(9), justify='left',
                              wraplength=self.px(wrap))
        label.pack(anchor='w', pady=(self.px(2), 0))
        return label

    def button(self, parent, text, command, primary=False):
        b = self.tk.Label(parent, text=text, bg=ACCENT if primary else FIELD, fg=BG if primary else TEXT,
                          font=self.font(9, 'bold' if primary else 'normal'), padx=self.px(12), pady=self.px(6),
                          cursor='hand2', highlightthickness=1, highlightbackground=LINE)
        b.bind('<Button-1>', lambda e: command())
        b.bind('<Enter>', lambda e: b.configure(highlightbackground=ACCENT))
        b.bind('<Leave>', lambda e: b.configure(highlightbackground=LINE))
        return b

    def row(self, parent, label):
        frame = self.tk.Frame(parent, bg=PANEL)
        frame.pack(fill='x', pady=self.px(5))
        self.tk.Label(frame, text=label, bg=PANEL, fg=TEXT, font=self.font(10), width=26, anchor='w').pack(side='left')
        return frame

    def choice(self, parent, label, values, labels, current, on_change, help_text=None):
        frame = self.row(parent, label)
        combo = self.ttk.Combobox(frame, values=labels, state='readonly',
                                  width=max(26, max(len(label) for label in labels) + 2))
        combo.current(values.index(current) if current in values else 0)
        combo.pack(side='left')
        combo.bind('<<ComboboxSelected>>', lambda e: on_change(values[combo.current()]))
        if help_text and len(help_text) > 110:
            # A long description goes under the row, the full width: beside the selector it was cut
            # off in the default (smallest) window.
            self.note(parent, '      ' + help_text)
        elif help_text:
            self.tk.Label(frame, text=help_text, bg=PANEL, fg=MUTED, font=self.font(9), justify='left',
                          wraplength=self.px(360)).pack(side='left', padx=self.px(12))
        return combo

    def fsr4_row(self, parent):
        """FSR 4 files: state and a download button (kzsf_fsr4.py, in the background)."""
        frame = self.row(parent, tr('fsr4_files'))
        status = self.tk.Label(frame, bg=PANEL, fg=MUTED, font=self.font(9))
        busy = []

        def refresh():
            ready = not kzsf_fsr4.missing(FSR4_DIR)
            status.configure(text=tr('fsr4_ready') if ready else tr('fsr4_missing'),
                             fg=ACCENT if ready else MUTED)
            if ready:
                button.pack_forget()

        def show(text, color=MUTED):
            if status.winfo_exists():
                status.configure(text=text, fg=color)

        def start():
            if busy:
                return
            busy.append(True)

            def work():
                try:
                    kzsf_fsr4.download(FSR4_DIR, lambda done, total: self.root.after(
                        0, lambda: show(tr('fsr4_downloading', done=done, total=total))))
                    self.root.after(0, lambda: status.winfo_exists() and refresh())
                except OSError as e:
                    message = tr('fsr4_failed', error=e)
                    self.root.after(0, lambda: show(message, ERROR))
                busy.clear()
            threading.Thread(target=work, daemon=True).start()
        button = self.button(frame, tr('fsr4_download'), start)
        button.pack(side='left', padx=(0, self.px(12)))
        status.pack(side='left')
        refresh()
        self.note(parent, '      ' + tr('fsr4_help'))

    def check(self, parent, label, key, help_text=None):
        var = self.tk.BooleanVar(value=self.graphics[key] == '1')

        def changed():
            self.graphics[key] = '1' if var.get() else '0'
            save_graphics({key: self.graphics[key]})
        frame = self.tk.Frame(parent, bg=PANEL)
        frame.pack(fill='x', pady=self.px(4))
        self.ttk.Checkbutton(frame, text=label, variable=var, command=changed).pack(side='left')
        if help_text:
            self.tk.Label(frame, text=help_text, bg=PANEL, fg=MUTED, font=self.font(9)).pack(side='left',
                                                                                         padx=self.px(10))
        self.images.setdefault('vars', []).append(var)
        return var

    def inline_check(self, parent, label, key):
        """A graphics checkbox packed beside the previous one (rows of short options)."""
        var = self.tk.BooleanVar(value=self.graphics[key] == '1')

        def changed():
            self.graphics[key] = '1' if var.get() else '0'
            save_graphics({key: self.graphics[key]})
        self.ttk.Checkbutton(parent, text=label, variable=var, command=changed).pack(side='left',
                                                                                    padx=(0, self.px(22)))
        self.images.setdefault('vars', []).append(var)
        return var

    def set_graphic(self, key, value):
        self.graphics[key] = value
        save_graphics({key: value})

    # -- game tab ---------------------------------------------------------------------------

    def game_tab(self, parent):
        tk = self.tk
        self.heading(parent, tr('game_folder'))
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x')
        self.game_entry = tk.Label(row, text=self.settings['game_dir'] or '—', bg=FIELD, fg=TEXT, anchor='w',
                                   font=self.font(10), padx=self.px(10), pady=self.px(7))
        self.game_entry.pack(side='left', fill='x', expand=True)
        self.button(row, tr('browse'), self.choose_game).pack(side='left', padx=(self.px(8), 0))
        self.note(parent, tr('game_folder_help'))

        self.update_frame = tk.Frame(parent, bg=PANEL)
        self.update_frame.pack(fill='x')
        self.game_status = tk.Label(parent, text='', bg=PANEL, font=self.font(10, 'bold'), anchor='w',
                                    justify='left', wraplength=self.px(820))
        self.game_status.pack(fill='x', pady=(self.px(10), 0))

        self.fov_section(parent)
        self.heading(parent, tr('tools'), pady=(22, 8))
        tools = tk.Frame(parent, bg=PANEL)
        tools.pack(fill='x')
        for text, command in ((tr('open_saves'), lambda: open_folder(PS4_DIR / 'savedata')),
                              (tr('open_shots'), lambda: open_folder(PS4_DIR / 'screenshots')),
                              (tr('open_logs'), lambda: open_folder(LOGS_DIR)),
                              (tr('clear_cache'), self.clear_cache)):
            self.button(tools, text, command).pack(side='left', padx=(0, self.px(8)))
        self.cache_note = self.note(parent, '')
        self.refresh_cache_size()
        self.refresh_game()

    def fov_section(self, parent):
        tk = self.tk
        self.heading(parent, tr('fov'), pady=(22, 8))
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x')
        var = tk.IntVar(value=fov_percent(self.settings.get('fov')))
        value_label = tk.Label(row, text=f'{var.get()}%', bg=PANEL, fg=MUTED, font=self.font(9), width=5)

        def moved(value):
            # The game's patches come in steps of 5% (patches/Killzone_Shadow_Fall.xml "FOV - N%").
            value = fov_percent(float(value))
            var.set(value)
            value_label.configure(text=f'{value}%')
            if value != self.settings.get('fov'):
                self.settings['fov'] = value
                save_launcher(self.settings)
        self.ttk.Scale(row, from_=FOV_MIN, to=FOV_MAX, variable=var, command=moved,
                       length=self.px(260)).pack(side='left')
        value_label.pack(side='left', padx=self.px(10))
        self.button(row, '100%', lambda: moved(100)).pack(side='left')
        self.images['fov_var'] = var
        self.note(parent, tr('fov_help'))

    def refresh_cache_size(self):
        def measure():
            size = sum(folder_size(p) for p in cache_paths())
            self.root.after(0, lambda: self.cache_note.winfo_exists() and
                            self.cache_note.configure(text=tr('cache_size', size=human_size(size))))
        threading.Thread(target=measure, daemon=True).start()

    def refresh_game(self):
        ok, message, needs_update = check_game(self.settings['game_dir'], self.settings['update_dir'])
        self.game_ok = ok
        if getattr(self, 'game_status', None) is not None and self.game_status.winfo_exists():
            # Another region plays, in the warning colour (untested).
            sfo = read_sfo(Path(self.settings['game_dir']) / 'sce_sys' / 'param.sfo') if ok else None
            other = bool(sfo) and str(sfo.get('TITLE_ID')) != TITLE_ID
            self.game_status.configure(text=('✓  ' if ok else '✕  ') + message,
                                       fg=(WARN if other else OK) if ok else
                                       (WARN if needs_update or not self.settings['game_dir'] else ERROR))
            for child in self.update_frame.winfo_children():
                child.destroy()
            if needs_update:
                self.heading(self.update_frame, tr('update_folder'), pady=(16, 6))
                row = self.tk.Frame(self.update_frame, bg=PANEL)
                row.pack(fill='x')
                self.tk.Label(row, text=self.settings['update_dir'] or '—', bg=FIELD, fg=TEXT, anchor='w',
                              font=self.font(10), padx=self.px(10), pady=self.px(7)).pack(side='left', fill='x',
                                                                                         expand=True)
                self.button(row, tr('browse'), self.choose_update).pack(side='left', padx=(self.px(8), 0))
                self.note(self.update_frame, tr('update_folder_help'))
        self.update_play_state()

    def choose_game(self):
        folder = self.filedialog.askdirectory(title=tr('choose_game'), parent=self.root,
                                              initialdir=self.settings['game_dir'] or None)
        if not folder:
            return
        folder = Path(folder)
        # The eboot.bin may be one level down (a folder holding CUSA00190/ or another release).
        if not (folder / 'eboot.bin').is_file():
            folder = next((folder / i for i in sorted(TITLE_IDS) if (folder / i / 'eboot.bin').is_file()), folder)
        self.settings['game_dir'] = str(folder)
        save_launcher(self.settings)
        self.images.pop('banner_key', None)
        self.set_window_icon()
        self.draw_banner()
        self.game_entry.configure(text=str(folder))
        self.refresh_game()

    def choose_update(self):
        folder = self.filedialog.askdirectory(title=tr('choose_update'), parent=self.root,
                                              initialdir=self.settings['update_dir'] or None)
        if folder:
            self.settings['update_dir'] = str(Path(folder))
            save_launcher(self.settings)
            self.refresh_game()

    def clear_cache(self):
        if self.process is not None:
            self.messagebox.showinfo('Killzone Shadow Fall', tr('busy_running'), parent=self.root)
            return
        size = human_size(sum(folder_size(p) for p in cache_paths()))
        if not self.messagebox.askyesno('Killzone Shadow Fall', tr('clear_confirm', size=size), parent=self.root):
            return
        for path in cache_paths():
            shutil.rmtree(path, ignore_errors=True)
        self.refresh_cache_size()
        self.status.configure(text=tr('cleared'), fg=OK)

    # -- graphics tab -----------------------------------------------------------------------

    def graphics_tab(self, parent):
        self.choice(parent, tr('display_mode'), DISPLAY_MODES,
                    [tr('mode_windowed'), tr('mode_borderless'), tr('mode_fullscreen')],
                    self.graphics['display_mode'], lambda v: self.set_graphic('display_mode', v))
        size = self.settings.get('window_size') if self.settings.get('window_size') in WINDOW_SIZES else '1920x1080'
        self.choice(parent, tr('window_size'), WINDOW_SIZES, [s.replace('x', ' × ') for s in WINDOW_SIZES], size,
                    self.set_window_size, tr('window_size_help'))
        self.choice(parent, tr('render_res'), RENDER_RESOLUTIONS,
                    ['720p (1280 × 720)', tr('res_native'), '1440p (2560 × 1440)', '1800p (3200 × 1800)',
                     '4K (3840 × 2160)'], self.graphics['render_resolution'],
                    lambda v: self.set_graphic('render_resolution', v), tr('render_res_help'))
        ultrawide = self.settings.get('ultrawide') if self.settings.get('ultrawide') in ULTRAWIDE else 'off'

        def set_ultrawide(value):
            self.settings['ultrawide'] = value
            save_launcher(self.settings)
        aspects = {'2560x1080': '21:9', '3440x1440': '21:9', '3840x1600': '21:9', '5120x1440': '32:9'}
        self.choice(parent, tr('ultrawide'), ULTRAWIDE,
                    [tr('ultrawide_off')] + [f"{s.replace('x', ' × ')} · {aspects[s]}" for s in ULTRAWIDE[1:]],
                    ultrawide, set_ultrawide, tr('ultrawide_help'))
        self.choice(parent, tr('fps_limit'), FPS_LIMITS,
                    ['30 FPS', '60 FPS', f"144 FPS ({tr('fps_experimental')})",
                     f"240 FPS ({tr('fps_experimental')})", tr('fps_unlimited')],
                    self.graphics['fps_limit'], lambda v: self.set_graphic('fps_limit', v), tr('fps_limit_help'))
        frame_gen = self.settings.get('frame_gen') if self.settings.get('frame_gen') in FRAME_GENS else 'off'

        def set_frame_gen(value):
            self.settings['frame_gen'] = value
            save_launcher(self.settings)
        self.choice(parent, tr('frame_gen'), FRAME_GENS,
                    [tr('frame_gen_off'), tr('frame_gen_fsr3'), tr('frame_gen_dlss')],
                    frame_gen, set_frame_gen, tr('frame_gen_help'))
        quality = self.settings.get('upscale_quality')
        quality = quality if quality in UPSCALE_QUALITIES else 'native'

        def set_quality(value):
            self.settings['upscale_quality'] = value
            save_launcher(self.settings)
        self.choice(parent, tr('upscale_quality'), UPSCALE_QUALITIES,
                    [tr('uq_native'), tr('uq_quality'), tr('uq_balanced'), tr('uq_performance'), tr('uq_ultra')],
                    quality, set_quality, tr('upscale_quality_help'))
        self.choice(parent, tr('anisotropy'), ANISOTROPY, [tr('aniso_game'), '2x', '4x', '8x', '16x'],
                    self.graphics['anisotropy'], lambda v: self.set_graphic('anisotropy', v))
        self.choice(parent, tr('mip_bias'), MIP_BIASES,
                    [f"-2 ({tr('mip_sharper')})", f"-1 ({tr('mip_sharper')})", f"-0.5 ({tr('mip_sharper')})",
                     tr('mip_game'), f"+0.5 ({tr('mip_softer')})", f"+1 ({tr('mip_softer')})",
                     f"+2 ({tr('mip_softer')})"],
                    self.graphics['mip_lod_bias'], lambda v: self.set_graphic('mip_lod_bias', v),
                    tr('mip_bias_help'))
        self.choice(parent, tr('lod_distance'), LOD_DISTANCES, tr('lod_labels'), self.graphics['lod_distance'],
                    lambda v: self.set_graphic('lod_distance', v), tr('lod_distance_help'))
        self.tk.Frame(parent, bg=PANEL, height=self.px(8)).pack()
        self.check(parent, tr('vsync'), 'vsync', tr('vsync_help'))
        self.check(parent, tr('fsr1'), 'fsr1', tr('fsr1_help'))
        self.check(parent, tr('sharpen'), 'rcas')
        self.check(parent, tr('show_fps'), 'show_fps')
        self.heading(parent, tr('effects'), pady=(14, 2))
        for keys in (('motion_blur', 'depth_of_field', 'camera_shake'), ('bloom', 'film_grain')):
            row = self.tk.Frame(parent, bg=PANEL)
            row.pack(fill='x', pady=self.px(2))
            for key in keys:
                self.inline_check(row, tr(key), key)
        self.note(parent, tr('effects_help'))
        self.tk.Frame(parent, bg=LINE, height=1).pack(fill='x', pady=self.px(14))
        self.note(parent, tr('graphics_note'))

    def set_window_size(self, value):
        self.settings['window_size'] = value
        save_launcher(self.settings)

    # -- controls tab -----------------------------------------------------------------------

    def controls_tab(self, parent):
        tk = self.tk
        self.note(parent, tr('controls_help'), wrap=880)
        grid = tk.Frame(parent, bg=PANEL)
        grid.pack(fill='x', pady=(self.px(8), 0))
        self.key_labels = {}
        keys = self.settings['key_map']
        pads = self.settings['pad_map']
        values = [v for v, _ in PAD_BUTTONS]
        rows = (len(BUTTONS) + 1) // 2
        # Two groups side by side, each: PS4 button | keyboard key | controller button.
        for group in range(2):
            col = group * 4
            pad_x = (self.px(36), 0) if group else (0, 0)
            for c, text in enumerate(('PS4', tr('keyboard').upper(), tr('controller').upper())):
                tk.Label(grid, text=text, bg=PANEL, fg=ACCENT, font=self.font(8, 'bold')).grid(
                    row=0, column=col + c, sticky='w', padx=pad_x if c == 0 else (self.px(6), 0))
        for index, (name, label, key, pad) in enumerate(BUTTONS):
            r, col = index % rows + 1, (index // rows) * 4
            pad_x = (self.px(36), 0) if col else (0, 0)
            tk.Label(grid, text=label, bg=PANEL, fg=TEXT, font=self.font(9), width=10, anchor='w').grid(
                row=r, column=col, sticky='w', padx=pad_x)
            k = tk.Label(grid, text=keys.get(name, key), bg=FIELD, fg=TEXT, font=self.font(9), width=12,
                         cursor='hand2', highlightthickness=1, highlightbackground=LINE)
            k.grid(row=r, column=col + 1, sticky='w', padx=(self.px(6), 0), pady=self.px(1))
            k.bind('<Button-1>', lambda e, n=name: self.capture_key(n))
            self.key_labels[name] = k
            if name in ('l2', 'r2'):  # the triggers stay the triggers
                tk.Label(grid, text='LT / L2' if name == 'l2' else 'RT / R2', bg=PANEL, fg=MUTED,
                         font=self.font(9), anchor='w').grid(row=r, column=col + 2, sticky='w', padx=(self.px(10), 0))
                continue
            current = pads.get(name, pad)
            combo = self.ttk.Combobox(grid, values=[t for _, t in PAD_BUTTONS], state='readonly', width=14,
                                      font=self.font(9))
            combo.current(values.index(current) if current in values else 0)
            combo.grid(row=r, column=col + 2, sticky='w', padx=(self.px(6), 0), pady=self.px(1))
            combo.bind('<<ComboboxSelected>>', lambda e, n=name, c=combo: self.set_pad(n, values[c.current()]))
        self.mouse_section(parent)
        self.choice(parent, tr('prompt_style'), [0, 1, 2, 3], tr('prompt_styles'),
                    int(self.settings.get('prompt_style', 0)), self.prompt_style_changed, tr('prompt_style_help'))
        bottom = tk.Frame(parent, bg=PANEL)
        bottom.pack(fill='x', pady=(self.px(8), 0))
        tk.Label(bottom, text=tr('vibration'), bg=PANEL, fg=TEXT, font=self.font(10)).pack(side='left')
        var = tk.IntVar(value=int(self.graphics['vibration']) if self.graphics['vibration'].isdigit() else 100)
        value_label = tk.Label(bottom, text=f'{var.get()}%', bg=PANEL, fg=MUTED, font=self.font(9), width=5)

        def moved(value):
            value = str(int(float(value)))
            value_label.configure(text=f'{value}%')
            if value != self.graphics['vibration']:
                self.set_graphic('vibration', value)
        self.ttk.Scale(bottom, from_=0, to=100, variable=var, command=moved,
                       length=self.px(200)).pack(side='left', padx=self.px(10))
        value_label.pack(side='left')
        self.images['vibration_var'] = var
        self.button(bottom, tr('reset_defaults'), self.reset_controls).pack(side='right')
        self.note(parent, tr('controller_help'))
        self.pad_extra_section(parent)

    def pad_extra_section(self, parent):
        tk = self.tk
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=(self.px(8), 0))
        sliders = []
        # launcher.json key, label, maximum percent, default percent
        for key, label, top, default in (('pad_dead_zone', 'dead_zone', 30, 0),
                                         ('pad_speaker', 'pad_speaker', 100, 100)):
            tk.Label(row, text=tr(label), bg=PANEL, fg=TEXT, font=self.font(10)).pack(
                side='left', padx=(self.px(24) if sliders else 0, 0))
            try:
                current = max(0, min(top, int(self.settings.get(key, default))))
            except (TypeError, ValueError):
                current = default
            var = tk.IntVar(value=current)
            value_label = tk.Label(row, text=f'{current}%', bg=PANEL, fg=MUTED, font=self.font(9), width=5)

            def moved(value, key=key, value_label=value_label):
                value = int(float(value))
                value_label.configure(text=f'{value}%')
                if value != self.settings.get(key):
                    self.settings[key] = value
                    save_launcher(self.settings)
            self.ttk.Scale(row, from_=0, to=top, variable=var, command=moved,
                           length=self.px(150)).pack(side='left', padx=self.px(10))
            value_label.pack(side='left')
            sliders.append(var)
        self.images['pad_extra_vars'] = sliders
        self.note(parent, tr('pad_extra_help'))
        self.quick_menu_section(parent)

    def prompt_style_changed(self, value):
        self.settings['prompt_style'] = value
        save_launcher(self.settings)

    def quick_menu_section(self, parent):
        tk = self.tk
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=(self.px(8), 0))
        quick = tk.BooleanVar(value=bool(self.settings.get('quick_menu', True)))
        hint = tk.BooleanVar(value=bool(self.settings.get('quick_menu_hint', True)))

        def changed():
            self.settings['quick_menu'] = quick.get()
            self.settings['quick_menu_hint'] = hint.get()
            save_launcher(self.settings)
        self.ttk.Checkbutton(row, text=tr('quick_menu'), variable=quick, command=changed).pack(side='left')
        self.ttk.Checkbutton(row, text=tr('quick_menu_hint'), variable=hint, command=changed).pack(
            side='left', padx=(self.px(24), 0))
        self.images['quick_menu_vars'] = (quick, hint)
        self.note(parent, tr('quick_menu_help'))

    def mouse_section(self, parent):
        tk = self.tk
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=(self.px(10), 0))
        look = tk.BooleanVar(value=bool(self.settings.get('mouse_look', True)))
        invert = tk.BooleanVar(value=bool(self.settings.get('mouse_invert', False)))

        def changed():
            self.settings['mouse_look'] = look.get()
            self.settings['mouse_invert'] = invert.get()
            save_launcher(self.settings)
        self.ttk.Checkbutton(row, text=tr('mouse_look'), variable=look, command=changed).pack(side='left')
        tk.Label(row, text=tr('mouse_sens'), bg=PANEL, fg=TEXT, font=self.font(10)).pack(side='left',
                                                                                       padx=(self.px(24), 0))
        sens = tk.DoubleVar(value=float(self.settings.get('mouse_sens', 1.0)))
        sens_label = tk.Label(row, text=f'{sens.get():.1f}', bg=PANEL, fg=MUTED, font=self.font(9), width=4)

        def moved(value):
            value = round(float(value) * 10) / 10
            sens_label.configure(text=f'{value:.1f}')
            if value != self.settings.get('mouse_sens'):
                self.settings['mouse_sens'] = value
                save_launcher(self.settings)
        self.ttk.Scale(row, from_=0.2, to=4.0, variable=sens, command=moved, length=self.px(160)).pack(
            side='left', padx=self.px(8))
        sens_label.pack(side='left')
        self.ttk.Checkbutton(row, text=tr('mouse_invert'), variable=invert, command=changed).pack(
            side='left', padx=(self.px(18), 0))
        self.images['mouse_vars'] = (look, invert, sens)
        self.mouse_turn_section(parent)
        self.note(parent, tr('mouse_look_help'))

    def mouse_turn_section(self, parent):
        tk = self.tk
        row = tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=(self.px(8), 0))
        tk.Label(row, text=tr('mouse_turn'), bg=PANEL, fg=TEXT, font=self.font(10)).pack(side='left')
        boost = tk.IntVar(value=turn_boost(self.settings.get('mouse_turn_boost')))
        boost_label = tk.Label(row, text=f'x{boost.get()}', bg=PANEL, fg=MUTED, font=self.font(9), width=4)

        def moved(value):
            value = turn_boost(value)
            boost.set(value)
            boost_label.configure(text=f'x{value}')
            if value != self.settings.get('mouse_turn_boost'):
                self.settings['mouse_turn_boost'] = value
                save_launcher(self.settings)
        self.ttk.Scale(row, from_=1, to=12, variable=boost, command=moved, length=self.px(160)).pack(
            side='left', padx=self.px(8))
        boost_label.pack(side='left')
        self.images['mouse_turn_var'] = boost

    def capture_key(self, name):
        if self.capturing:
            self.cancel_capture()
        self.capturing = name
        label = self.key_labels[name]
        label.configure(text=tr('press_key'), fg=ACCENT, highlightbackground=ACCENT)
        self.root.bind('<KeyPress>', self.key_pressed)
        # Bound once this click is over, or it would be taken as the new binding.
        self.root.after(150, self.bind_mouse_capture)
        self.root.focus_force()

    def bind_mouse_capture(self):
        if self.capturing:
            self.root.bind('<ButtonPress>', lambda e: self.assign_key(TK_MOUSE.get(e.num)))
            self.root.bind('<MouseWheel>', lambda e: self.assign_key('Wheel Up' if e.delta > 0 else 'Wheel Down'))

    def cancel_capture(self):
        name = self.capturing
        self.capturing = None
        self.root.unbind('<KeyPress>')
        self.root.unbind('<ButtonPress>')
        self.root.unbind('<MouseWheel>')
        if name and name in self.key_labels and self.key_labels[name].winfo_exists():
            default = next(key for n, _, key, _ in BUTTONS if n == name)
            self.key_labels[name].configure(text=self.settings['key_map'].get(name, default), fg=TEXT,
                                            highlightbackground=LINE)

    def key_pressed(self, event):
        name = self.capturing
        if not name:
            return
        if event.keysym == 'Escape':
            self.cancel_capture()
            return
        self.assign_key(sdl_key_name(event.keysym))

    def assign_key(self, key):
        name = self.capturing
        if not name:
            return
        if not key or key in RESERVED_KEYS:
            self.root.bell()
            return
        # A key serves one button: whichever had it gets this button's old key.
        keys = self.settings['key_map']
        defaults = {n: k for n, _, k, _ in BUTTONS}
        old = keys.get(name, defaults[name])
        for other in defaults:
            if other != name and keys.get(other, defaults[other]) == key:
                keys[other] = old
                self.key_labels[other].configure(text=old)
        keys[name] = key
        save_launcher(self.settings)
        self.cancel_capture()

    def set_pad(self, name, value):
        self.settings['pad_map'][name] = value
        save_launcher(self.settings)

    def reset_controls(self):
        self.settings.update(key_map={}, pad_map={}, mouse_look=True, mouse_sens=1.0, mouse_invert=False,
                             mouse_turn_boost=6, pad_dead_zone=0, pad_speaker=100, quick_menu=True,
                             quick_menu_hint=True)
        save_launcher(self.settings)
        self.set_graphic('vibration', '100')
        self.show_tab('controls')

    # -- advanced tab -----------------------------------------------------------------------

    def perf_section(self, parent):
        row = self.tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=(self.px(6), 0))
        self.tk.Label(row, text=tr('perf_heading') + ':', bg=PANEL, fg=ACCENT,
                      font=self.font(9, 'bold')).pack(side='left')
        for key in ('draw_pipe', 'nowait_writes', 'fast_locks'):
            var = self.tk.BooleanVar(value=bool(self.settings.get(key, True)))

            def changed(key=key, var=var):
                self.settings[key] = var.get()
                save_launcher(self.settings)
            self.ttk.Checkbutton(row, text=tr(key), variable=var, command=changed).pack(
                side='left', padx=(self.px(12), 0))
            self.images.setdefault('vars', []).append(var)
        self.note(parent, '      ' + tr('perf_help'))

    # -- cheats tab -------------------------------------------------------------------------

    def cheats_tab(self, parent):
        """The game's own debug switches (gow3.ini cheat_*, which the in-game menu changes live)."""
        self.heading(parent, tr('cheats'))
        for key in ('cheat_god', 'cheat_ammo', 'cheat_no_reload', 'cheat_sprint', 'cheat_echo'):
            self.check(parent, tr(key), key, tr(key + '_help'))
        self.note(parent, tr('cheats_help'))
        self.heading(parent, tr('debug'), pady=(18, 6))
        self.check(parent, tr('cheat_chapters'), 'cheat_chapters', tr('cheat_chapters_help'))
        frame = self.row(parent, tr('free_camera'))
        self.tk.Label(frame, text='F9 / F10', bg=PANEL, fg=ACCENT, font=self.font(10, 'bold')).pack(side='left')
        self.note(parent, tr('free_camera_help'))

    def advanced_tab(self, parent):
        self.note(parent, tr('advanced_help'), color=WARN)
        self.tk.Frame(parent, bg=PANEL, height=self.px(8)).pack()
        self.check(parent, tr('async_shaders'), 'async_shaders')
        self.note(parent, '      ' + tr('async_help'))
        smart = self.tk.BooleanVar(value=bool(self.settings.get('smart_pending', True)))

        def smart_changed():
            self.settings['smart_pending'] = smart.get()
            save_launcher(self.settings)
        self.ttk.Checkbutton(parent, text=tr('smart_pending'), variable=smart,
                             command=smart_changed).pack(anchor='w', pady=self.px(4))
        self.images.setdefault('vars', []).append(smart)
        self.note(parent, '      ' + tr('smart_pending_help'))
        precompile = self.tk.BooleanVar(value=bool(self.settings.get('precompile', True)))

        def precompile_changed():
            self.settings['precompile'] = precompile.get()
            save_launcher(self.settings)
        self.ttk.Checkbutton(parent, text=tr('precompile'), variable=precompile,
                             command=precompile_changed).pack(anchor='w', pady=self.px(4))
        self.images.setdefault('vars', []).append(precompile)
        self.note(parent, '      ' + tr('precompile_help'))
        self.perf_section(parent)
        self.tk.Frame(parent, bg=PANEL, height=self.px(8)).pack()
        method = self.settings.get('res_method') if self.settings.get('res_method') in RES_METHODS else 'upscale'

        def set_method(value):
            self.settings['res_method'] = value
            save_launcher(self.settings)
        self.choice(parent, tr('res_method'), RES_METHODS,
                    [tr('res_method_upscale'), tr('res_method_fsr4'), tr('res_method_dlss'),
                     tr('res_method_patch')],
                    method, set_method)
        self.note(parent, '      ' + tr('res_method_help'))
        self.fsr4_row(parent)
        self.check(parent, tr('stale_readback'), 'stale_readback')
        self.note(parent, '      ' + tr('stale_help'))
        self.check(parent, tr('deferred_readback'), 'deferred_readback')
        self.note(parent, '      ' + tr('deferred_help'))
        self.tk.Frame(parent, bg=PANEL, height=self.px(8)).pack()
        self.choice(parent, tr('frames_queued'), FRAMES_QUEUED, [tr('queued_1'), tr('queued_2'), tr('queued_0')],
                    self.graphics['frames_queued'], lambda v: self.set_graphic('frames_queued', v))
        self.tk.Frame(parent, bg=LINE, height=1).pack(fill='x', pady=self.px(14))
        self.heading(parent, tr('updates'))
        var = self.tk.BooleanVar(value=bool(self.settings.get('check_updates', True)))

        def changed():
            self.settings['check_updates'] = var.get()
            save_launcher(self.settings)
        row = self.tk.Frame(parent, bg=PANEL)
        row.pack(fill='x', pady=self.px(4))
        self.ttk.Checkbutton(row, text=tr('check_updates'), variable=var, command=changed).pack(side='left')
        self.button(row, tr('check_updates_now'), lambda: self.check_updates(quiet=False)).pack(
            side='left', padx=self.px(16))
        self.images.setdefault('vars', []).append(var)
        discord = self.tk.BooleanVar(value=bool(self.settings.get('discord_rpc', True)))

        def discord_changed():
            self.settings['discord_rpc'] = discord.get()
            save_launcher(self.settings)
        self.ttk.Checkbutton(parent, text=tr('discord_rpc'), variable=discord,
                             command=discord_changed).pack(anchor='w', pady=(self.px(8), 0))
        self.note(parent, '      ' + tr('discord_rpc_help'))
        self.images.setdefault('vars', []).append(discord)
        self.tk.Frame(parent, bg=LINE, height=1).pack(fill='x', pady=self.px(14))
        self.button(parent, tr('reset_all'), self.reset_all).pack(anchor='w')

    def reset_all(self):
        if not self.messagebox.askyesno('Killzone Shadow Fall', tr('reset_all_confirm'), parent=self.root):
            return
        self.graphics = dict(GRAPHICS_DEFAULTS)
        save_graphics(self.graphics)
        self.settings.update(window_size='1920x1080', key_map={}, pad_map={}, mouse_look=True, mouse_sens=1.0,
                             mouse_invert=False, mouse_turn_boost=6, smart_pending=True, precompile=True,
                             draw_pipe=True,
                             nowait_writes=True, fast_locks=True,
                             res_method='upscale', frame_gen='off', ultrawide='off',
                             discord_rpc=True, pad_dead_zone=0, pad_speaker=100, fov=100,
                             quick_menu=True, quick_menu_hint=True)
        save_launcher(self.settings)
        self.show_tab('advanced')

    # -- updates ----------------------------------------------------------------------------

    def check_updates(self, quiet):
        """Asks GitHub for a newer release in the background; quiet: no message when none or offline."""
        def work():
            try:
                # KZSF_UPDATE_AS_VERSION: test the update flow from an older version number.
                release, error = kzsf_update.check(os.environ.get('KZSF_UPDATE_AS_VERSION') or VERSION), None
            except (OSError, ValueError) as e:
                release, error = None, e
            self.root.after(0, lambda: self.update_checked(release, error, quiet))
        threading.Thread(target=work, daemon=True).start()

    def update_checked(self, release, error, quiet):
        if release:
            self.update_info = release
            self.show_update_bar()
        elif not quiet:
            text = tr('update_check_failed', error=error) if error else tr('update_none', version=VERSION)
            self.messagebox.showinfo('Killzone Shadow Fall', text, parent=self.root)

    def show_update_bar(self):
        tk, release = self.tk, self.update_info
        if self.update_bar is not None and self.update_bar.winfo_exists():
            self.update_bar.destroy()
        bar = tk.Frame(self.root, bg=FIELD, highlightthickness=1, highlightbackground=ACCENT)
        bar.pack(side='bottom', fill='x', padx=self.px(28), pady=(0, self.px(10)), before=self.body)
        tk.Label(bar, text=tr('update_available', version=release['version']), bg=FIELD, fg=ACCENT,
                 font=self.font(10, 'bold')).pack(side='left', padx=self.px(12), pady=self.px(8))
        later = self.button(bar, tr('update_later'), lambda: bar.destroy())
        later.pack(side='right', padx=(self.px(6), self.px(10)), pady=self.px(6))
        self.button(bar, tr('update_notes'), lambda: webbrowser.open(release['page'])).pack(side='right', padx=self.px(6))
        self.button(bar, tr('update_now'), self.install_update, primary=True).pack(side='right', padx=self.px(6))
        self.update_bar = bar

    def install_update(self):
        release = self.update_info
        if self.process is not None:
            self.messagebox.showwarning('Killzone Shadow Fall', tr('update_close_game'), parent=self.root)
            return
        if not FROZEN:  # running from source: git pulls the code, the page has the zip
            webbrowser.open(release['page'])
            return
        if not self.messagebox.askyesno('Killzone Shadow Fall', tr('update_confirm', version=release['version'],
                                                                     size=human_size(release['size'])),
                                        parent=self.root):
            return
        self.play_button.configure(cursor='arrow')
        self.updating = True  # PLAY waits while the update downloads

        def progress(done, total):
            percent = int(done * 100 / total) if total else 0
            self.root.after(0, lambda: self.status.configure(text=tr('update_downloading', percent=percent),
                                                             fg=ACCENT))

        def work():
            try:
                archive = kzsf_update.download(release, UPDATE_DIR, progress)
                self.root.after(0, lambda: self.status.configure(text=tr('update_unpacking'), fg=ACCENT))
                unpacked = UPDATE_DIR / 'new'
                source = kzsf_update.extract(archive, unpacked, Path(sys.executable).name)
                script = kzsf_update.install_script(source, PORT_DIR, os.getpid(), Path(sys.executable).name,
                                                    UPDATE_DIR, cleanup=(unpacked, archive))
                kzsf_update.run_script(script)
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                self.root.after(0, lambda e=e: self.update_failed(e))
                return
            self.root.after(0, self.root.destroy)
            kzsf_update.exit_for_update()
        threading.Thread(target=work, daemon=True).start()

    def update_failed(self, error):
        self.updating = False
        self.status.configure(text='', fg=MUTED)
        self.update_play_state()
        self.messagebox.showerror('Killzone Shadow Fall', tr('update_failed', error=error), parent=self.root)

    # -- playing ----------------------------------------------------------------------------

    def update_play_state(self):
        if not hasattr(self, 'play_button'):
            return
        if self.process is not None:
            self.play_button.configure(text=tr('playing'), bg=FIELD, fg=MUTED, cursor='arrow')
        elif not getattr(self, 'game_ok', False):
            self.play_button.configure(text=tr('play'), bg=FIELD, fg=MUTED, cursor='arrow')
        else:
            self.play_button.configure(text=tr('play'), bg=ACCENT, fg=BG, cursor='hand2')

    def play(self):
        if self.process is not None or getattr(self, 'updating', False):
            return
        ok, message, needs_update = check_game(self.settings['game_dir'], self.settings['update_dir'])
        if not ok:
            self.messagebox.showwarning('Killzone Shadow Fall', f'{message}\n\n{tr("fix_folder")}', parent=self.root)
            self.show_tab('game')
            return
        try:
            self.process, out, err, run_dir = launch_game(self.settings, self.graphics, needs_update)
        except OSError as error:
            self.process = None
            self.messagebox.showerror('Killzone Shadow Fall', tr('cant_start', error=error), parent=self.root)
            return
        self.status.configure(text=tr('preparing'), fg=MUTED)
        self.update_play_state()
        threading.Thread(target=self.wait_game, args=(self.process, out, err, run_dir), daemon=True).start()

    def wait_game(self, process, out, err, run_dir):
        code = process.wait()
        out.close()
        err.close()
        self.root.after(0, lambda: self.game_ended(code, run_dir))

    def game_ended(self, code, run_dir):
        self.process = None
        self.status.configure(text='', fg=MUTED)
        self.update_play_state()
        if code == RESTART_CODE:
            # "Apply and restart" in the game: the options it changed (gow3.ini) are read again.
            self.settings = load_launcher()
            self.graphics = load_graphics()
            self.build()
            self.play()
            return
        if getattr(self, 'cache_note', None) is not None and self.cache_note.winfo_exists():
            self.refresh_cache_size()
        # 0: closed normally; 1 with an empty error log: the window was closed by the player.
        if code not in (0, None) and self.messagebox.askyesno('Killzone Shadow Fall', game_ended_text(code, run_dir),
                                                               parent=self.root):
            open_folder(run_dir)

    def set_window_icon(self):
        """The game's icon (sce_sys/icon0.png) on the launcher window, when Pillow can read it."""
        game = self.settings.get('game_dir')
        if not game:
            return
        try:
            from PIL import Image, ImageTk
            with Image.open(Path(game) / 'sce_sys' / 'icon0.png') as source:
                icon = ImageTk.PhotoImage(source.convert('RGB').resize((64, 64), Image.LANCZOS))
            self.root.iconphoto(True, icon)
            self.images['icon'] = icon
        except Exception:  # noqa: BLE001
            pass

    def close(self):
        self.root.destroy()


def direct_play():
    """--play (issue #48, Steam and other front ends): the game with the saved options, no launcher
    window; waits for it (restarts included) and returns its exit code. None when the folders are
    not ready: the launcher opens instead."""
    settings, graphics = load_launcher(), load_graphics()
    ok, _, needs_update = check_game(settings['game_dir'], settings['update_dir'])
    if not ok:
        return None
    while True:
        process, out, err, run_dir = launch_game(settings, graphics, needs_update)
        code = process.wait()
        out.close()
        err.close()
        if code != RESTART_CODE:
            break
        # "Apply and restart" in the game: the options it changed (gow3.ini) are read again.
        settings, graphics = load_launcher(), load_graphics()
    # game_ended asks whether to open the logs: Yes opens the run's folder.
    if code not in (0, None) and WINDOWS and ctypes.windll.user32.MessageBoxW(
            None, game_ended_text(code, run_dir), 'Killzone Shadow Fall', 0x34) == 6:
        os.startfile(run_dir)
    return code


def main():
    global LANG
    args = sys.argv[1:]
    if args and args[0] in ('--run', '--script'):
        sys.exit(run_role(args))
    settings = load_launcher()
    LANG = settings.get('language') or windows_language()
    if args and args[0] in ('--play', '-play', '/play'):
        code = direct_play()
        if code is not None:
            sys.exit(code)
    if WINDOWS:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
        try:  # its own taskbar button and icon, not Python's
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('Lesca.KillzoneShadowFall')
        except (AttributeError, OSError):
            pass
    # The new window (kzsf_web.py: HTML in Edge WebView2, part of Windows 10 and 11). Without
    # WebView2, or with KZSF_LAUNCHER_UI=tk, this Tk window opens as before.
    if WINDOWS and os.environ.get('KZSF_LAUNCHER_UI', '').lower() != 'tk':
        try:
            import kzsf_web
            if kzsf_web.run(sys.modules[__name__]):
                return
        except Exception:  # noqa: BLE001  (any failure of the new window: the Tk one still works)
            import traceback
            traceback.print_exc()
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    root = tk.Tk()
    icon = PORT_DIR / 'launcher' / 'kzsf.ico'
    if icon.is_file():
        try:
            root.iconbitmap(default=str(icon))
        except tk.TclError:
            pass
    launcher = Launcher(root, tk, ttk, filedialog, messagebox)
    launcher.set_window_icon()
    root.mainloop()


if __name__ == '__main__':
    main()
