# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher window in HTML (launcher/web/), shown by pywebview in Edge WebView2.

kzsf_launcher.py keeps everything else (texts, options, game folder checks, starting the game,
updates); this module only describes the pages and answers the page's calls (window.pywebview.api).
WebView2 ships with Windows 10 (2021 updates) and 11; without it run() returns False and the Tk
window opens instead.

  python launcher/kzsf_web.py --preview out.html   the page with the current options and a dummy
                                                    api, to look at in a browser
"""
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import webbrowser
import zipfile

WEB_DIR = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parent)) / 'web'
if not WEB_DIR.is_dir():
    WEB_DIR = Path(__file__).resolve().parent / 'web'

# Texts of this window only (section names, buttons); the options' own texts are kzsf_launcher's.
WEB_TEXTS = {
    'es': {
        'nav_home': 'Inicio', 'sec_display': 'Pantalla', 'sec_upscaling': 'Escalado',
        'sec_upscaling_sub': 'FSR, DLSS y modos de calidad', 'sec_image': 'Calidad de imagen',
        'sec_effects': 'Efectos', 'sec_shaders': 'Shaders', 'sec_perf': 'Rendimiento',
        'sec_launcher': 'Launcher', 'sec_maint': 'Mantenimiento', 'sec_player': 'Jugador', 'sec_game': 'Juego',
        'sec_mouse': 'Mouse', 'sec_prompts': 'Interfaz', 'sec_pad': 'Mando', 'sec_pad_extra': 'Ajustes del mando',
        'sub_kbm': 'Teclado y mouse', 'sub_pad': 'Mando',
        'grp_face': 'Botones', 'grp_shoulders': 'Gatillos y sticks', 'grp_menu': 'Menú y touchpad',
        'grp_dpad': 'Cruceta · adrenalina, eco táctico, modo de disparo',
        'grp_move': 'Movimiento (stick izquierdo)', 'grp_swipe': 'Deslizar en el touchpad · órdenes del OWL',
        # Multiplayer (beta) page.
        'tab_online': 'Multijugador', 'online_beta': 'BETA',
        'online_warning': 'Beta de prueba: multijugador privado entre amigos, sin servidores oficiales. Una PC crea la sala y las demás se unen por una red virtual (Radmin VPN). Los errores, por issues de GitHub con la carpeta del log.',
        'online_network': 'Red virtual', 'online_refresh': 'Actualizar',
        'online_net_radmin': 'Radmin VPN conectado · {ip}', 'online_net_tailscale': 'Tailscale conectado · {ip}',
        'online_net_lan': 'Sin Radmin VPN: solo red local · {ip}', 'online_net_none': 'Sin red virtual',
        'online_net_help': 'Instalá Radmin VPN (gratis). El host crea una red (Red → Crear red) y los demás se unen con su nombre y contraseña. Todos tienen que verse en la lista de Radmin. Tailscale y una red de casa también sirven.',
        'online_get_radmin': 'Descargar Radmin VPN',
        'online_nickname': 'Tu apodo', 'online_nickname_help': 'De 3 a 16 letras, números, _ o -. Distinto para cada jugador de la sala.',
        'online_host': 'Crear sala', 'online_host_help': 'Esta PC es el servidor de la sala: tiene que quedar abierta mientras juegan. Si Windows pregunta por el firewall, permitilo en redes privadas.',
        'online_host_button': 'Crear sala',
        'online_join': 'Unirse a una sala', 'online_join_help': 'El host te pasa su archivo de conexión (Host-connection.zip, no tiene nada secreto) y el código de la sala.',
        'online_connection': 'Archivo de conexión', 'online_choose': 'Elegir…', 'online_code': 'Código de sala',
        'online_join_button': 'Unirse',
        'online_room': 'Sala', 'online_you_host': 'Sos el host · {nickname}', 'online_you_join': 'Estás en la sala · {nickname}',
        'online_copy': 'Copiar', 'online_copied': 'Código copiado',
        'online_save_connection': 'Guardar archivo de conexión…', 'online_saved': 'Archivo guardado',
        'online_send': 'Mandales a los demás el código y el archivo de conexión.',
        'online_play': 'Jugar en la sala (multijugador)', 'online_close': 'Cerrar la sala', 'online_leave': 'Salir de la sala',
        'online_working': 'Abriendo la sala…', 'online_closed': 'La sala se cerró.',
        'online_steps': 'En el juego', 'online_steps_help': 'Uno crea la Warzone: Multiplayer → Create your Warzone → elegí al menos un mapa y un nombre. Los demás: Multiplayer → Browse Warzones → esa Warzone → Join. No cambien de equipo en mitad de la partida. Tus partidas del multijugador se guardan aparte de la campaña.',
        'online_missing': 'Falta la carpeta multiplayer del port: volvé a descargar el port.',
        'online_saves': 'Dos botones de jugar, dos partidas guardadas',
        'online_saves_help': 'JUGAR (abajo) abre la campaña con tu partida de siempre. JUGAR EN LA SALA abre el multijugador con una partida guardada aparte: el juego arranca en el menú principal sin tu progreso de campaña. Es normal: tu partida de la campaña no se borró ni se tocó, la vas a ver de nuevo con JUGAR. En multijugador el rango, las armas y los desafíos vienen todos desbloqueados. El perfil nuevo toma el idioma de Avanzado → Juego → Idioma del juego. En multijugador el juego se ve a resolución completa: DLSS / FSR siguen activos como anti-aliasing, pero los modos que bajan la resolución (Calidad, Equilibrado, Rendimiento) son solo para la campaña por ahora.',
        'more': 'Ver más', 'less': 'Ver menos', 'cancel': 'Cancelar', 'ok': 'Aceptar', 'yes': 'Sí',
        'open_logs_q': 'Abrir logs', 'close': 'Cerrar', 'game_status': 'Estado',
        'home_sub': 'Port nativo para Windows', 'quick_links': 'Accesos rápidos',
        'reserved_key': 'Esa tecla la usa el juego o el port.',
        'home_hint': 'Durante el juego, Insert abre el menú del port (con el mando: R3 + L2).',
        'free_camera_keys': 'Teclas',
        'support': 'Apoyá el port', 'theme': 'Tema',
        'cache_label': 'Caché de shaders', 'all_options': 'Todas las opciones',
        'all_options_help': 'Gráficos, avanzado y controles vuelven a los valores por defecto.',
        # What each button does in the game (issue #74), from the game's own controller legend.
        'buttons': {'cross': 'Cruz · saltar / trepar', 'circle': 'Círculo · agacharse / deslizarse',
                    'square': 'Cuadrado · recargar / recoger arma', 'triangle': 'Triángulo · cambiar de arma',
                    'l1': 'L1 · orden al OWL', 'r1': 'R1 · granada', 'l2': 'L2 · apuntar', 'r2': 'R2 · disparar',
                    'l3': 'L3 · correr', 'r3': 'R3 · cuerpo a cuerpo', 'options': 'Options · pausa', 'touchpad_left': 'Touchpad · clic izquierdo',
                    'touchpad_right': 'Touchpad · clic derecho', 'up': 'Arriba', 'down': 'Abajo',
                    'left': 'Izquierda', 'right': 'Derecha',
                    'move_forward': 'Avanzar', 'move_back': 'Retroceder', 'move_left': 'Izquierda',
                    'move_right': 'Derecha', 'swipe_up': 'Deslizar arriba', 'swipe_down': 'Deslizar abajo',
                    'swipe_left': 'Deslizar a la izquierda', 'swipe_right': 'Deslizar a la derecha'},
    },
    'en': {
        'nav_home': 'Home', 'sec_display': 'Display', 'sec_upscaling': 'Upscaling',
        'sec_upscaling_sub': 'FSR, DLSS and quality modes', 'sec_image': 'Image quality',
        'sec_effects': 'Effects', 'sec_shaders': 'Shaders', 'sec_perf': 'Performance',
        'sec_launcher': 'Launcher', 'sec_maint': 'Maintenance', 'sec_player': 'Player', 'sec_game': 'Game',
        'sec_mouse': 'Mouse', 'sec_prompts': 'Interface', 'sec_pad': 'Controller',
        'sec_pad_extra': 'Controller settings',
        'sub_kbm': 'Keyboard and mouse', 'sub_pad': 'Controller',
        'grp_face': 'Buttons', 'grp_shoulders': 'Triggers and sticks', 'grp_menu': 'Menu and touchpad',
        'grp_dpad': 'D-pad · adrenaline, tactical echo, fire mode',
        'grp_move': 'Movement (left stick)', 'grp_swipe': 'Touchpad swipes · OWL commands',
        'tab_online': 'Multiplayer', 'online_beta': 'BETA',
        'online_warning': 'Testing beta: private multiplayer between friends, without the official servers. One PC creates the room and the others join it over a virtual LAN (Radmin VPN). Report bugs in GitHub issues with the log folder.',
        'online_network': 'Virtual LAN', 'online_refresh': 'Refresh',
        'online_net_radmin': 'Radmin VPN connected · {ip}', 'online_net_tailscale': 'Tailscale connected · {ip}',
        'online_net_lan': 'No Radmin VPN: home network only · {ip}', 'online_net_none': 'No virtual LAN',
        'online_net_help': "Install Radmin VPN (free). The host creates a network (Network → Create network) and the others join it with its name and password. Everyone must see each other in Radmin's list. Tailscale and a home network work too.",
        'online_get_radmin': 'Download Radmin VPN',
        'online_nickname': 'Your nickname', 'online_nickname_help': '3 to 16 letters, digits, _ or -. A different one for each player in the room.',
        'online_host': 'Create a room', 'online_host_help': "This PC is the room's server: keep it open while you play. If Windows asks about the firewall, allow private networks.",
        'online_host_button': 'Create room',
        'online_join': 'Join a room', 'online_join_help': 'The host sends you their connection file (Host-connection.zip, it has no secrets) and the room code.',
        'online_connection': 'Connection file', 'online_choose': 'Choose…', 'online_code': 'Room code',
        'online_join_button': 'Join',
        'online_room': 'Room', 'online_you_host': 'You are the host · {nickname}', 'online_you_join': 'You are in the room · {nickname}',
        'online_copy': 'Copy', 'online_copied': 'Code copied',
        'online_save_connection': 'Save connection file…', 'online_saved': 'File saved',
        'online_send': 'Send the others the code and the connection file.',
        'online_play': 'Play in the room (multiplayer)', 'online_close': 'Close the room', 'online_leave': 'Leave the room',
        'online_working': 'Opening the room…', 'online_closed': 'The room closed.',
        'online_steps': 'In the game', 'online_steps_help': 'One player creates the Warzone: Multiplayer → Create your Warzone → pick at least one map and a name. The others: Multiplayer → Browse Warzones → that Warzone → Join. Do not switch teams mid-match. Your multiplayer saves are kept apart from the campaign.',
        'online_missing': "The port's multiplayer folder is missing: download the port again.",
        'online_saves': 'Two play buttons, two saves',
        'online_saves_help': 'PLAY (at the bottom) opens the campaign with your usual save. PLAY IN THE ROOM opens multiplayer with a save of its own: the game starts at the main menu without your campaign progress. That is expected: your campaign save was not deleted or touched, PLAY shows it again. In multiplayer the rank, weapons and challenges are all unlocked. The new profile takes the language from Advanced → Game → Game language. In multiplayer the game renders at full resolution: DLSS / FSR stay on as anti-aliasing, but the modes that lower the resolution (Quality, Balanced, Performance) are campaign-only for now.',
        'more': 'More', 'less': 'Less', 'cancel': 'Cancel', 'ok': 'OK', 'yes': 'Yes',
        'open_logs_q': 'Open logs', 'close': 'Close', 'game_status': 'Status',
        'home_sub': 'Native Windows port', 'quick_links': 'Shortcuts',
        'reserved_key': 'The game or the port uses that key.',
        'home_hint': 'In game, Insert opens the port menu (on a controller: R3 + L2).',
        'free_camera_keys': 'Keys',
        'support': 'Support the port', 'theme': 'Theme',
        'cache_label': 'Shader cache', 'all_options': 'All options',
        'all_options_help': 'Graphics, advanced and controls go back to their defaults.',
        'buttons': {'cross': 'Cross · jump / vault', 'circle': 'Circle · crouch / slide',
                    'square': 'Square · reload / pick up weapon', 'triangle': 'Triangle · switch weapon',
                    'l1': 'L1 · OWL command', 'r1': 'R1 · grenade', 'l2': 'L2 · aim', 'r2': 'R2 · fire',
                    'l3': 'L3 · sprint', 'r3': 'R3 · melee', 'options': 'Options · pause', 'touchpad_left': 'Touchpad · left click',
                    'touchpad_right': 'Touchpad · right click', 'up': 'Up', 'down': 'Down',
                    'left': 'Left', 'right': 'Right',
                    'move_forward': 'Forward', 'move_back': 'Back', 'move_left': 'Left', 'move_right': 'Right',
                    'swipe_up': 'Swipe up', 'swipe_down': 'Swipe down', 'swipe_left': 'Swipe left',
                    'swipe_right': 'Swipe right'},
    },
}
# Places the Tk window's texts name that are elsewhere here.
WEB_REPLACE = {
    'es': (('(Avanzado → Método de resolución)', '(Escalado → Método)'), ('(Avanzado → Método)', '(Escalado → Método)'),
           (' Método en Avanzado.', ''), ('(pestaña Juego)', '(Inicio)')),
    'en': (('(Advanced → Resolution method)', '(Upscaling → Method)'), ('(Advanced → Method)', '(Upscaling → Method)'),
           (' Method under Advanced.', ''), ('(Game tab)', '(Home)')),
}
# The window's colours (style.css [data-theme]) and its background while the page loads.
THEMES = {'vektan': '#0b0f13', 'helghast': '#070606'}
BUTTON_GROUPS = (('grp_face', ('cross', 'circle', 'square', 'triangle')),
                 ('grp_shoulders', ('l1', 'r1', 'l2', 'r2', 'l3', 'r3')),
                 ('grp_menu', ('options', 'touchpad_left', 'touchpad_right')),
                 ('grp_dpad', ('up', 'down', 'left', 'right')),
                 ('grp_move', ('move_forward', 'move_back', 'move_left', 'move_right')),
                 ('grp_swipe', ('swipe_up', 'swipe_down', 'swipe_left', 'swipe_right')))


def available():
    """Whether pywebview can show the page in Edge WebView2 (not the old Internet Explorer engine)."""
    try:
        import webview  # noqa: F401
        from webview.platforms import winforms
    except Exception:  # noqa: BLE001  (no pywebview, no pythonnet / .NET)
        return False
    return getattr(winforms, 'renderer', '') == 'edgechromium'


class Api:
    """The calls of the page (window.pywebview.api.<name>); names starting with _ stay hidden."""

    def __init__(self, core):
        self._core = core
        self._window = None
        self._settings = core.load_launcher()
        self._shown = threading.Event()
        self._graphics = core.load_graphics()
        self._process = None
        self._run_dir = None
        self._update = None
        self._updating = False
        self._fsr4_busy = False
        self._online = None  # multiplayer/kz_online.Online, once the Multiplayer page is used
        self._online_busy = False
        self._connection = ''

    # -- texts and pages ----------------------------------------------------------------------

    def _t(self, key, **values):
        core = self._core
        text = WEB_TEXTS.get(core.LANG, WEB_TEXTS['en']).get(key)
        if text is None:
            text = core.tr(key, **values)
            for old, new in WEB_REPLACE.get(core.LANG, ()) if isinstance(text, str) else ():
                text = text.replace(old, new)
        return text

    def _pages(self):
        c, t = self._core, self._t
        aspects = {'2560x1080': '21:9', '3440x1440': '21:9', '3840x1600': '21:9', '5120x1440': '32:9'}

        def select(key, label, values, labels, help_key=None, **extra):
            return {'type': 'select', 'key': key, 'label': t(label), 'help': t(help_key) if help_key else '',
                    'options': [[v, l] for v, l in zip(values, labels)], **extra}

        def toggle(key, label, help_key=None, **extra):
            return {'type': 'toggle', 'key': key, 'label': t(label), 'help': t(help_key) if help_key else '', **extra}

        def slider(key, label, low, high, step, unit='', help_key=None, default=None, **extra):
            return {'type': 'slider', 'key': key, 'label': t(label), 'help': t(help_key) if help_key else '',
                    'min': low, 'max': high, 'step': step, 'unit': unit, 'default': default, **extra}

        upscaled = {'key': 'res_method', 'not': ['patch']}
        graphics = {'id': 'graphics', 'icon': 'monitor', 'title': t('tab_graphics'), 'note': t('graphics_note'),
                    'sections': [
            {'title': t('sec_display'), 'items': [
                select('display_mode', 'display_mode', c.DISPLAY_MODES,
                       [t('mode_windowed'), t('mode_borderless'), t('mode_fullscreen')]),
                select('window_size', 'window_size', c.WINDOW_SIZES, [s.replace('x', ' × ') for s in c.WINDOW_SIZES],
                       'window_size_help', show_if={'key': 'display_mode', 'in': ['windowed']}),
                select('render_resolution', 'render_res', c.RENDER_RESOLUTIONS,
                       ['720p (1280 × 720)', t('res_native'), '1440p (2560 × 1440)', '1800p (3200 × 1800)',
                        '4K (3840 × 2160)'], 'render_res_help'),
                select('ultrawide', 'ultrawide', c.ULTRAWIDE,
                       [t('ultrawide_off')] + [f"{s.replace('x', ' × ')} · {aspects[s]}" for s in c.ULTRAWIDE[1:]],
                       'ultrawide_help'),
                select('fps_limit', 'fps_limit', c.FPS_LIMITS,
                       ['30 FPS', '60 FPS', f"144 FPS ({t('fps_experimental')})",
                        f"240 FPS ({t('fps_experimental')})", t('fps_unlimited')], 'fps_limit_help'),
                toggle('vsync', 'vsync', 'vsync_help'),
                toggle('show_fps', 'show_fps'),
                slider('fov', 'fov', c.FOV_MIN, c.FOV_MAX, 5, '%', 'fov_help', default=100),
            ]},
            {'title': t('sec_upscaling'), 'subtitle': t('sec_upscaling_sub'), 'items': [
                select('res_method', 'res_method', c.RES_METHODS,
                       [t('res_method_upscale'), t('res_method_fsr4'), t('res_method_dlss'), t('res_method_patch')],
                       'res_method_help'),
                {'type': 'fsr4', 'label': t('fsr4_files'), 'help': t('fsr4_help'),
                 'show_if': {'key': 'res_method', 'in': ['fsr4']}},
                select('upscale_quality', 'upscale_quality', c.UPSCALE_QUALITIES,
                       [t('uq_native'), t('uq_quality'), t('uq_balanced'), t('uq_performance'), t('uq_ultra')],
                       'upscale_quality_help', show_if=upscaled),
                select('frame_gen', 'frame_gen', c.FRAME_GENS,
                       [t('frame_gen_off'), t('frame_gen_fsr3'), t('frame_gen_dlss')], 'frame_gen_help',
                       show_if=upscaled),
                toggle('fsr1', 'fsr1', 'fsr1_help'),
                toggle('rcas', 'sharpen'),
            ]},
            {'title': t('sec_image'), 'items': [
                select('anisotropy', 'anisotropy', c.ANISOTROPY, [t('aniso_game'), '2x', '4x', '8x', '16x']),
                select('mip_lod_bias', 'mip_bias', c.MIP_BIASES,
                       [f"-2 ({t('mip_sharper')})", f"-1 ({t('mip_sharper')})", f"-0.5 ({t('mip_sharper')})",
                        t('mip_game'), f"+0.5 ({t('mip_softer')})", f"+1 ({t('mip_softer')})",
                        f"+2 ({t('mip_softer')})"], 'mip_bias_help'),
                select('lod_distance', 'lod_distance', c.LOD_DISTANCES, t('lod_labels'), 'lod_distance_help'),
            ]},
            {'title': t('sec_effects'), 'help': t('effects_help'), 'grid': True, 'items': [
                toggle(k, k) for k in ('motion_blur', 'depth_of_field', 'bloom', 'film_grain', 'camera_shake')]},
        ]}

        buttons = {name: label for name, label, _, _ in c.BUTTONS}
        defaults = {name: (key, pad) for name, _, key, pad in c.BUTTONS}
        groups = [{'title': t(title), 'buttons': [
            {'name': n, 'label': buttons[n], 'key': defaults[n][0], 'pad': defaults[n][1],
             'fixed_pad': {'l2': 'LT / L2', 'r2': 'RT / R2',
                           **{m: 'L stick' for m in ('move_forward', 'move_back', 'move_left', 'move_right')}}.get(n)}
             for n in names]}
            for title, names in BUTTON_GROUPS]
        controls = {'id': 'controls', 'icon': 'gamepad', 'title': t('tab_controls'), 'tabs': [
            {'id': 'kbm', 'title': t('sub_kbm'), 'sections': [
                {'type': 'keys', 'help': t('controls_help'), 'groups': groups},
                {'title': t('sec_mouse'), 'help': t('mouse_look_help'), 'items': [
                    toggle('mouse_look', 'mouse_look'),
                    slider('mouse_sens', 'mouse_sens', 0.2, 4.0, 0.1, '', default=1.0, decimals=1),
                    slider('mouse_turn_boost', 'mouse_turn', 1, 12, 1, '', default=6, prefix='x'),
                    toggle('mouse_invert', 'mouse_invert'),
                ]},
                {'title': t('sec_prompts'), 'items': [
                    select('prompt_style', 'prompt_style', [0, 1, 2, 3], t('prompt_styles'), 'prompt_style_help')]},
            ]},
            {'id': 'pad', 'title': t('sub_pad'), 'sections': [
                {'type': 'pads', 'help': t('controller_help'), 'groups': groups,
                 'options': [list(p) for p in c.PAD_BUTTONS]},
                {'title': t('sec_pad_extra'), 'items': [
                    slider('vibration', 'vibration', 0, 100, 5, '%', default=100),
                    slider('pad_dead_zone', 'dead_zone', 0, 30, 1, '%', default=0),
                    slider('pad_speaker', 'pad_speaker', 0, 100, 5, '%', default=100),
                    {'type': 'help', 'help': t('pad_extra_help')},
                    toggle('quick_menu', 'quick_menu'),
                    toggle('quick_menu_hint', 'quick_menu_hint'),
                    select('menu_combo', 'menu_combo', [0, 1, 2, 3], t('menu_combos')),
                    {'type': 'help', 'help': t('quick_menu_help')},
                ]},
            ]},
        ], 'actions': [{'id': 'reset_controls', 'label': t('reset_defaults')}]}

        cheats = {'id': 'cheats', 'icon': 'bolt', 'title': t('tab_cheats'), 'note': t('cheats_help'), 'sections': [
            {'title': t('sec_player'), 'items': [
                toggle(k, k, k + '_help') for k in ('cheat_god', 'cheat_ammo', 'cheat_no_reload', 'cheat_sprint',
                                                    'cheat_echo')]},
            {'title': t('debug'), 'items': [
                toggle('cheat_chapters', 'cheat_chapters', 'cheat_chapters_help'),
                {'type': 'keys_info', 'label': t('free_camera'), 'help': t('free_camera_help'), 'keys': ['F9', 'F10']},
            ]},
        ]}

        languages = c.game_languages(self._settings['game_dir'], self._settings['update_dir'])
        game_section = {'title': t('sec_game'), 'items': [
            select('game_language', 'game_language', ['auto'] + [l[0] for l in languages],
                   [t('game_language_auto')] + [l[2] for l in languages], 'game_language_help')]}
        advanced = {'id': 'advanced', 'icon': 'sliders', 'title': t('tab_advanced'), 'warning': t('advanced_help'),
                    'sections': [
            game_section,
            {'title': t('sec_shaders'), 'items': [
                toggle('precompile', 'precompile', 'precompile_help'),
                toggle('async_shaders', 'async_shaders', 'async_help'),
                toggle('smart_pending', 'smart_pending', 'smart_pending_help'),
            ]},
            {'title': t('sec_perf'), 'help': t('perf_help'), 'items': [
                toggle('draw_pipe', 'draw_pipe'),
                toggle('nowait_writes', 'nowait_writes'),
                toggle('fast_locks', 'fast_locks'),
                toggle('stale_readback', 'stale_readback', 'stale_help'),
                toggle('deferred_readback', 'deferred_readback', 'deferred_help'),
                select('frames_queued', 'frames_queued', c.FRAMES_QUEUED,
                       [t('queued_1'), t('queued_2'), t('queued_0')]),
            ]},
            {'title': t('sec_launcher'), 'items': [
                toggle('check_updates', 'check_updates',
                       action={'id': 'check_updates_now', 'label': t('check_updates_now')}),
                toggle('discord_rpc', 'discord_rpc', 'discord_rpc_help'),
                {'type': 'button', 'label': t('reshade'), 'id': 'open_bin', 'button': t('open_bin'), 'icon': 'folder',
                 'help': t('reshade_help') + ' ' + t('reshade_found' if any(
                     (c.PORT_DIR / 'bin' / n).is_file() for n in ('ReShade.ini', 'ReShade64.dll', 'dxgi.dll'))
                     else 'reshade_missing')},
                {'type': 'button', 'label': t('shortcut'), 'help': t('shortcut_help'),
                 'button': t('shortcut_create'), 'id': 'create_shortcut', 'icon': 'home'},
            ]},
            {'title': t('sec_maint'), 'items': [
                {'type': 'cache', 'label': t('cache_label'), 'button': t('clear_cache')},
                {'type': 'button', 'label': t('all_options'), 'help': t('all_options_help'), 'button': t('reset_all'),
                 'id': 'reset_all', 'danger': True},
            ]},
        ]}
        home = {'id': 'home', 'icon': 'home', 'title': t('nav_home'), 'type': 'home'}
        online = {'id': 'online', 'icon': 'users', 'title': t('tab_online'), 'type': 'online', 'badge': t('online_beta')}
        return [home, graphics, controls, cheats, online, advanced]

    def _texts(self):
        t = self._t
        keys = ('play', 'playing', 'preparing', 'game_folder', 'game_folder_help', 'update_folder',
                'update_folder_help', 'browse', 'open_saves', 'open_shots', 'open_logs', 'donate', 'discord_join',
                'footer_hint', 'language', 'update_now', 'update_notes', 'update_later', 'fsr4_ready',
                'fsr4_missing', 'fsr4_download', 'press_key', 'clear_confirm', 'reset_all_confirm',
                'update_previous_failed', 'cleared')
        texts = {k: t(k) for k in keys}
        texts.update(WEB_TEXTS.get(self._core.LANG, WEB_TEXTS['en']))
        return texts

    def _values(self):
        c = self._core
        s = self._settings
        values = dict(self._graphics)
        for key in ('window_size', 'mouse_look', 'mouse_sens', 'mouse_invert', 'mouse_turn_boost', 'smart_pending',
                    'precompile', 'draw_pipe', 'nowait_writes', 'fast_locks', 'res_method', 'discord_rpc', 'pad_dead_zone',
                    'pad_speaker', 'quick_menu', 'quick_menu_hint', 'prompt_style', 'upscale_quality',
                    'check_updates', 'game_language', 'menu_combo'):
            values[key] = s.get(key, c.LAUNCHER_DEFAULTS.get(key))
        values['window_size'] = s.get('window_size') if s.get('window_size') in c.WINDOW_SIZES else '1920x1080'
        values['ultrawide'] = s.get('ultrawide') if s.get('ultrawide') in c.ULTRAWIDE else 'off'
        values['frame_gen'] = s.get('frame_gen') if s.get('frame_gen') in c.FRAME_GENS else 'off'
        values['res_method'] = s.get('res_method') if s.get('res_method') in c.RES_METHODS else 'upscale'
        if values['upscale_quality'] not in c.UPSCALE_QUALITIES:
            values['upscale_quality'] = 'native'
        values['launcher_theme'] = s.get('launcher_theme') if s.get('launcher_theme') in THEMES else 'vektan'
        if values['game_language'] not in [l[0] for l in c.game_languages(s['game_dir'], s['update_dir'])]:
            values['game_language'] = 'auto'
        values['fov'] = c.fov_percent(s.get('fov'))
        values['mouse_turn_boost'] = c.turn_boost(s.get('mouse_turn_boost'))
        return values

    def _game(self):
        c = self._core
        ok, message, needs_update = c.check_game(self._settings['game_dir'], self._settings['update_dir'])
        level = 'ok' if ok else ('warn' if needs_update or not self._settings['game_dir'] else 'error')
        if ok:
            sfo = c.read_sfo(Path(self._settings['game_dir']) / 'sce_sys' / 'param.sfo')
            if sfo and str(sfo.get('TITLE_ID')) != c.TITLE_ID:
                level = 'warn'
        return {'ok': ok, 'message': message, 'level': level, 'needs_update': needs_update,
                'game_dir': self._settings['game_dir'], 'update_dir': self._settings['update_dir']}

    def _art(self, name, width):
        """The game's own picture (sce_sys/pic0.png, icon0.png) as a data URL, at most this wide."""
        game = self._settings.get('game_dir')
        if not game:
            return ''
        path = Path(game) / 'sce_sys' / name
        try:
            from PIL import Image
            import io
            with Image.open(path) as source:
                image = source.convert('RGB')
            if image.width > width:
                image = image.resize((width, image.height * width // image.width), Image.LANCZOS)
            buffer = io.BytesIO()
            image.save(buffer, 'JPEG', quality=88)
            return 'data:image/jpeg;base64,' + base64.b64encode(buffer.getvalue()).decode()
        except ImportError:  # no Pillow: the file as it is
            try:
                return 'data:image/png;base64,' + base64.b64encode(path.read_bytes()).decode()
            except OSError:
                return ''
        except Exception:  # noqa: BLE001  (missing or unreadable picture: none)
            return ''

    def get_state(self):
        c = self._core
        return {'lang': c.LANG, 'version': c.VERSION, 'texts': self._texts(), 'pages': self._pages(),
                'values': self._values(), 'game': self._game(), 'art': self._art('pic0.png', 1600),
                'icon': self._art('icon0.png', 128), 'key_map': self._settings['key_map'],
                'pad_map': self._settings['pad_map'], 'playing': self._process is not None,
                'fsr4_ready': not self._core.kzsf_fsr4.missing(c.FSR4_DIR),
                'update': self._update_view(), 'reserved': sorted(c.RESERVED_KEYS),
                'online': self.online_info()}

    # -- options ------------------------------------------------------------------------------

    def set(self, key, value):
        c = self._core
        if key == 'launcher_theme' and value not in THEMES:
            value = 'vektan'
        if key in c.GRAPHICS_DEFAULTS:
            value = ('1' if value else '0') if isinstance(value, bool) else str(value)
            self._graphics[key] = value
            c.save_graphics({key: value})
            return value
        if key == 'fov':
            value = c.fov_percent(value)
        elif key == 'mouse_turn_boost':
            value = c.turn_boost(value)
        elif key == 'mouse_sens':
            value = max(0.2, min(4.0, round(float(value) * 10) / 10))
        elif key in ('pad_dead_zone', 'pad_speaker', 'prompt_style', 'menu_combo'):
            value = int(value)
        elif not isinstance(value, bool):
            value = str(value)
        self._settings[key] = value
        c.save_launcher(self._settings)
        return value

    def assign_key(self, name, key):
        """The capture of the page: a key serves one button, whichever had it gets this one's old key."""
        c = self._core
        if not key or key in c.RESERVED_KEYS:
            return {'ok': False}
        keys = self._settings['key_map']
        defaults = {n: k for n, _, k, _ in c.BUTTONS}
        old = keys.get(name, defaults[name])
        for other in defaults:
            if other != name and keys.get(other, defaults[other]) == key:
                keys[other] = old
        keys[name] = key
        c.save_launcher(self._settings)
        return {'ok': True, 'key_map': keys}

    def set_pad(self, name, value):
        self._settings['pad_map'][name] = value
        self._core.save_launcher(self._settings)

    def reset_controls(self):
        self._settings.update(key_map={}, pad_map={}, mouse_look=True, mouse_sens=1.0, mouse_invert=False,
                              mouse_turn_boost=6, pad_dead_zone=0, pad_speaker=100, quick_menu=True,
                              quick_menu_hint=True)
        self._core.save_launcher(self._settings)
        self.set('vibration', '100')
        return self.get_state()

    def reset_all(self):
        c = self._core
        self._graphics = dict(c.GRAPHICS_DEFAULTS)
        c.save_graphics(self._graphics)
        self._settings.update(window_size='1920x1080', key_map={}, pad_map={}, mouse_look=True, mouse_sens=1.0,
                              mouse_invert=False, mouse_turn_boost=6, smart_pending=True, precompile=True,
                              draw_pipe=True, nowait_writes=True, fast_locks=True, res_method='upscale', frame_gen='off',
                              ultrawide='off', discord_rpc=True, pad_dead_zone=0, pad_speaker=100, fov=100,
                              quick_menu=True, quick_menu_hint=True)
        c.save_launcher(self._settings)
        return self.get_state()

    def set_language(self, code):
        c = self._core
        if code in c.TEXTS:
            c.LANG = code
            self._settings['language'] = code
            c.save_launcher(self._settings)
        return self.get_state()

    # -- game folder, folders, links ----------------------------------------------------------

    def _ask_folder(self, current):
        import webview
        result = self._window.create_file_dialog(webview.FileDialog.FOLDER, directory=current or '')
        return Path(result[0] if isinstance(result, (list, tuple)) else result) if result else None

    def choose_game(self):
        c = self._core
        folder = self._ask_folder(self._settings['game_dir'])
        if folder:
            # The eboot.bin may be one level down (a folder holding CUSA00190/ or another release).
            if not (folder / 'eboot.bin').is_file():
                folder = next((folder / i for i in sorted(c.TITLE_IDS) if (folder / i / 'eboot.bin').is_file()),
                              folder)
            self._settings['game_dir'] = str(folder)
            c.save_launcher(self._settings)
        return {'game': self._game(), 'art': self._art('pic0.png', 1600), 'icon': self._art('icon0.png', 128)}

    def choose_update(self):
        folder = self._ask_folder(self._settings['update_dir'])
        if folder:
            self._settings['update_dir'] = str(folder)
            self._core.save_launcher(self._settings)
        return {'game': self._game()}

    def open(self, name):
        c = self._core
        path = {'saves': c.PS4_DIR / 'savedata', 'shots': c.PS4_DIR / 'screenshots', 'logs': c.LOGS_DIR,
                'run_logs': self._run_dir or c.LOGS_DIR}.get(name)
        if path:
            c.open_folder(path)

    def open_bin(self):
        folder = self._core.PORT_DIR / 'bin'
        if folder.is_dir():
            os.startfile(folder)

    def create_shortcut(self):
        c = self._core
        try:
            c.create_play_shortcut()
        except (OSError, subprocess.SubprocessError) as error:
            return {'error': c.tr('shortcut_failed', error=error)}
        return {'text': c.tr('shortcut_done')}

    def open_url(self, name):
        c = self._core
        url = {'donate': c.DONATE_URL, 'discord': c.DISCORD_URL, 'radmin': 'https://www.radmin-vpn.com/',
               'notes': (self._update or {}).get('page')}.get(name)
        if url:
            webbrowser.open(url)

    def cache_size(self):
        c = self._core

        def measure():
            size = sum(c.folder_size(p) for p in c.cache_paths())
            self._emit('cache', {'text': c.tr('cache_size', size=c.human_size(size)), 'size': c.human_size(size)})
        threading.Thread(target=measure, daemon=True).start()

    def clear_cache(self):
        c = self._core
        if self._process is not None:
            return {'error': c.tr('busy_running')}
        for path in c.cache_paths():
            import shutil
            shutil.rmtree(path, ignore_errors=True)
        self.cache_size()
        return {'ok': True}

    def cache_confirm_text(self):
        c = self._core
        return c.tr('clear_confirm', size=c.human_size(sum(c.folder_size(p) for p in c.cache_paths())))

    # -- FSR 4 files --------------------------------------------------------------------------

    def fsr4_download(self):
        c = self._core
        if self._fsr4_busy:
            return
        self._fsr4_busy = True

        def work():
            try:
                c.kzsf_fsr4.download(c.FSR4_DIR, lambda done, total: self._emit(
                    'fsr4', {'state': 'busy', 'text': c.tr('fsr4_downloading', done=done, total=total)}))
                self._emit('fsr4', {'state': 'ready', 'text': c.tr('fsr4_ready')})
            except OSError as e:
                self._emit('fsr4', {'state': 'error', 'text': c.tr('fsr4_failed', error=e)})
            self._fsr4_busy = False
        threading.Thread(target=work, daemon=True).start()

    # -- updates ------------------------------------------------------------------------------

    def _update_view(self):
        if not self._update:
            return None
        c = self._core
        return {'text': c.tr('update_available', version=self._update['version']),
                'confirm': c.tr('update_confirm', version=self._update['version'],
                                size=c.human_size(self._update['size']))}

    def check_updates(self, quiet=False):
        c = self._core

        def work():
            try:
                release, error = c.kzsf_update.check(os.environ.get('KZSF_UPDATE_AS_VERSION') or c.VERSION), None
            except (OSError, ValueError) as e:
                release, error = None, e
            if release:
                self._update = release
                self._emit('update', self._update_view())
            elif not quiet:
                text = c.tr('update_check_failed', error=error) if error else c.tr('update_none', version=c.VERSION)
                self._emit('message', {'text': text})
        threading.Thread(target=work, daemon=True).start()

    def install_update(self):
        c = self._core
        release = self._update
        if not release:
            return
        if self._process is not None:
            return {'error': c.tr('update_close_game')}
        if not c.FROZEN:  # running from source: git pulls the code, the page has the zip
            webbrowser.open(release['page'])
            return
        self._updating = True

        def progress(done, total):
            percent = int(done * 100 / total) if total else 0
            self._emit('status', {'text': c.tr('update_downloading', percent=percent), 'busy': True})

        def work():
            try:
                archive = c.kzsf_update.download(release, c.UPDATE_DIR, progress)
                self._emit('status', {'text': c.tr('update_unpacking'), 'busy': True})
                unpacked = c.UPDATE_DIR / 'new'
                source = c.kzsf_update.extract(archive, unpacked, Path(sys.executable).name)
                script = c.kzsf_update.install_script(source, c.PORT_DIR, os.getpid(), Path(sys.executable).name,
                                                      c.UPDATE_DIR, cleanup=(unpacked, archive))
                c.kzsf_update.run_script(script)
            except (OSError, ValueError, zipfile.BadZipFile) as e:
                self._updating = False
                self._emit('status', {'text': ''})
                self._emit('message', {'text': c.tr('update_failed', error=e), 'level': 'error'})
                return
            self._window.destroy()
            # The script copies the files once this process is gone: a thread left running kept it
            # alive with no window, so the update never applied (#87).
            c.kzsf_update.exit_for_update()
        threading.Thread(target=work, daemon=True).start()

    # -- playing ------------------------------------------------------------------------------

    def play(self):
        c = self._core
        if self._process is not None or self._updating:
            return {}
        ok, message, needs_update = c.check_game(self._settings['game_dir'], self._settings['update_dir'])
        if not ok:
            return {'error': f"{message}\n\n{self._t('fix_folder')}", 'page': 'home'}
        try:
            self._process, out, err, self._run_dir = c.launch_game(self._settings, self._graphics, needs_update)
        except OSError as error:
            self._process = None
            return {'error': c.tr('cant_start', error=error)}
        threading.Thread(target=self._wait_game, args=(self._process, out, err), daemon=True).start()
        return {'playing': True}

    def _wait_game(self, process, out, err):
        code = process.wait()
        out.close()
        err.close()
        self._process = None
        c = self._core
        if code == c.RESTART_CODE:
            # "Apply and restart" in the game: the options it changed (gow3.ini) are read again.
            self._settings = c.load_launcher()
            self._graphics = c.load_graphics()
            self._emit('state', self.get_state())
            result = self.play()
            if result.get('error'):
                self._emit('message', {'text': result['error'], 'level': 'error'})
            else:
                self._emit('playing', {'playing': True})
            return
        self._emit('playing', {'playing': False})
        self.cache_size()
        # 0: closed normally; 1 with an empty error log: the window was closed by the player.
        if code not in (0, None):
            self._emit('message', {'text': c.game_ended_text(code, self._run_dir), 'level': 'error', 'logs': True})

    # -- multiplayer (beta) -------------------------------------------------------------------

    def _kz_online(self):
        return self._core.online_module()

    def _room_view(self):
        online = self._online
        if not online or not online.alive():
            return None
        room = online.room
        return {'code': room['code'], 'host': room['host'], 'nickname': room['nickname'], 'slot': room['slot']}

    def online_info(self):
        t = self._t
        module = self._kz_online()
        if module is None:
            return {'missing': t('online_missing')}
        net = module.network()
        text = {'radmin': 'online_net_radmin', 'tailscale': 'online_net_tailscale',
                'lan': 'online_net_lan'}.get(net['kind'], 'online_net_none')
        return {'network': {'kind': net['kind'], 'text': t(text).replace('{ip}', net['ip'])},
                'nickname': self._settings.get('online_nickname', ''), 'connection': self._connection,
                'room': self._room_view(), 'busy': self._online_busy}

    def _online_session(self):
        if self._online is None:
            self._online = self._kz_online().Online(self._core.USER_DIR / 'online')
        return self._online

    def _online_start(self, work):
        """A room opens in the background (up to ~20 s): the page hears 'online' when it is done."""
        if self._online_busy:
            return
        self._online_busy = True

        def run():
            error = None
            try:
                work(self._online_session())
            except (OSError, ValueError, subprocess.SubprocessError) as e:
                error = str(e)
            self._online_busy = False
            info = self.online_info()
            if error:
                info['error'] = error
            self._emit('online', info)
        threading.Thread(target=run, daemon=True).start()

    def online_host(self, nickname):
        self.set('online_nickname', nickname)
        self._online_start(lambda online: online.host(nickname))
        return self.online_info()

    def online_join(self, code, nickname):
        self.set('online_nickname', nickname)
        if not self._connection:
            return {**self.online_info(), 'error': self._t('online_join_help')}
        self._online_start(lambda online: online.join(self._connection, code, nickname))
        return self.online_info()

    def online_choose_connection(self):
        import webview
        result = self._window.create_file_dialog(webview.FileDialog.OPEN, allow_multiple=False,
                                                 file_types=('Host-connection (*.zip)', 'All files (*.*)'))
        if result:
            self._connection = str(result[0] if isinstance(result, (list, tuple)) else result)
        return self.online_info()

    def online_save_connection(self):
        import shutil
        import webview
        online = self._online
        if not online or not online.room or not online.room.get('archive'):
            return {}
        result = self._window.create_file_dialog(webview.FileDialog.SAVE, save_filename='Host-connection.zip')
        if not result:
            return {}
        target = Path(result[0] if isinstance(result, (list, tuple)) else result)
        shutil.copyfile(online.room['archive'], target)
        return {'text': self._t('online_saved')}

    def online_close(self):
        if self._online:
            self._online.close()
        return self.online_info()

    def online_play(self):
        """The game in the room (kzsf_launcher.launch_online_game: saves apart from the campaign)."""
        c = self._core
        if self._process is not None or self._updating:
            return {}
        ok, message, _ = c.check_game(self._settings['game_dir'], self._settings['update_dir'])
        if not ok:
            return {'error': f"{message}\n\n{self._t('fix_folder')}", 'page': 'home'}
        if not self._online:
            return {'error': self._t('online_closed')}
        try:
            self._process, self._run_dir = c.launch_online_game(self._settings, self._graphics, self._online)
        except ValueError:
            self._process = None
            return {'error': self._t('online_closed')}
        except OSError as error:
            self._process = None
            return {'error': c.tr('cant_start', error=error)}

        def wait(process):
            process.wait()
            self._process = None
            self._emit('playing', {'playing': False})
            self._emit('online', self.online_info())
        threading.Thread(target=wait, args=(self._process,), daemon=True).start()
        return {'playing': True}

    # -- page events --------------------------------------------------------------------------

    def _emit(self, kind, data):
        if self._window is not None:
            try:
                self._window.evaluate_js(f'window.kz && window.kz.event({json.dumps(kind)}, {json.dumps(data)})')
            except Exception:  # noqa: BLE001  (window closing)
                pass

    def ready(self):
        """The page has drawn itself: background work that answers with events."""
        c = self._core
        self._shown.set()
        if self._settings.pop('launcher_gpu_off_pending', None):
            # The software-drawn window worked: keep it that way (the GPU one stayed black, #82).
            c.save_launcher(self._settings)
        self.cache_size()
        if self._settings.get('check_updates', True):
            self.check_updates(quiet=True)
        if c.kzsf_update.failed_update(c.UPDATE_DIR):
            self._emit('message', {'text': c.tr('update_previous_failed'), 'level': 'warn'})


def page_html(state=None):
    """index.html with its style and script inline (one page, no local web server). With a state:
    the preview, which answers its own calls."""
    html = (WEB_DIR / 'index.html').read_text(encoding='utf-8')
    style = (WEB_DIR / 'style.css').read_text(encoding='utf-8')
    script = (WEB_DIR / 'app.js').read_text(encoding='utf-8')
    if state is not None:
        script = f'window.KZ_PREVIEW = {json.dumps(state)};\n' + script
    return html.replace('/*STYLE*/', style).replace('/*SCRIPT*/', script)


def dark_title_bar(window):
    """The window's title bar in dark mode (Windows 10 2004 and newer); nothing elsewhere."""
    try:
        import ctypes
        hwnd = window.native.Handle.ToInt32()
        value = ctypes.c_int(1)
        for attribute in (20, 19):  # DWMWA_USE_IMMERSIVE_DARK_MODE, and its number before 20H1
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(value), 4) == 0:
                break
    except Exception:  # noqa: BLE001
        pass


def run(core):
    """Shows the launcher; False when this window cannot be used (the Tk one opens instead)."""
    if not available():
        return False
    import webview
    launcher = core.load_launcher()
    # Without GPU drawing: WebView2 showed only a black window on some PCs (#82).
    if launcher.get('launcher_gpu') is False or os.environ.get('KZSF_WEB_GPU') == '0':
        os.environ.setdefault('WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS', '--disable-gpu')
    api = Api(core)
    scale = 1.0
    try:
        import ctypes
        scale = max(1.0, ctypes.windll.user32.GetDpiForSystem() / 96.0)
    except (AttributeError, OSError):
        pass
    window = webview.create_window('Killzone Shadow Fall', html=page_html(), js_api=api,
                                   width=int(1180 * scale), height=int(780 * scale),
                                   min_size=(int(960 * scale), int(640 * scale)),
                                   background_color=THEMES.get(api._settings.get('launcher_theme'), THEMES['vektan']))
    api._window = window
    window.events.shown += lambda: dark_title_bar(window)
    icon = core.PORT_DIR / 'launcher' / 'kzsf.ico'
    blank, closed = threading.Event(), threading.Event()
    window.events.closed += closed.set

    def watchdog():
        # A page that never drew itself: next time without the GPU, this time the classic window.
        if (not api._shown.wait(25) and not closed.is_set() and
                launcher.get('launcher_gpu') is not False):
            blank.set()
            settings = core.load_launcher()
            settings['launcher_gpu'] = False
            settings['launcher_gpu_off_pending'] = True
            core.save_launcher(settings)
            window.destroy()
    threading.Thread(target=watchdog, daemon=True).start()
    webview.start(gui='edgechromium', debug=bool(os.environ.get('KZSF_WEB_DEBUG')),
                  private_mode=True, icon=str(icon) if icon.is_file() else None)
    if api._online:  # a room this window opened ends with it
        api._online.close()
    return not blank.is_set()


def preview(path):
    """--preview: the page with the current options in a file, for a browser."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import kzsf_launcher as core
    core.LANG = os.environ.get('KZSF_LANG') or core.load_launcher().get('language') or core.windows_language()
    Path(path).write_text(page_html(Api(core).get_state()), encoding='utf-8')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--preview':
        preview(sys.argv[2])
    else:
        print(__doc__)
