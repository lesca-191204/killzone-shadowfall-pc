// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "gow3_settings.h"
#include "gow3_orbs.h"
#include "gow3_actors.h"
#include "gow3_graphics.h"
#include "gow3_perf_stats.h"
#include "../../src/actor_hook.h"

extern Gow3ActorSlot gow3_actor_slots[2];
extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderdoc.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    // Killzone: Noto Sans (SIL OFL, as shadPS4) stands in for the PS4 system font sets
    // (src/runtime_font.c); with DejaVu Sans letters went missing from the game's text.
    ".balign 16\n"
    ".global gow3_system_font_ttf\n"
    "gow3_system_font_ttf:\n"
    ".incbin \"" GOW3_SYSTEM_FONT_PATH "\"\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden gow3_font_ttf\n"
    ".global gow3_font_ttf\n"
    "gow3_font_ttf:\n"
    ".incbin \"" GOW3_FONT_PATH "\"\n"
    ".hidden gow3_font_ttf_end\n"
    ".global gow3_font_ttf_end\n"
    "gow3_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char gow3_font_ttf[];
extern "C" const unsigned char gow3_font_ttf_end[];

namespace Gow3Overlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool r3_down = false, l2_down = false;
// Controller shortcut of the menu (menu_combo, issue #63): 0 R3+L2, 1 L3+R3, 2 Back+Start,
// 3 none (Insert only). The other buttons are tracked for combos 1 and 2.
bool l3_down = false, back_down = false, start_down = false;
// Quick menu (hold Select/Back/Share, runtime_pad.c): Resume / Settings / Exit game.
std::atomic<bool> quick_open{false};
// The FOV hook's float (src/probe.c), rewritten when the FOV changes; null without the hook.
std::atomic<float*> fov_cell{nullptr};
// The LOD distance hook's float (src/probe.c), the setting / 100; null without the hook.
std::atomic<float*> lod_cell{nullptr};
// The camera shake hook's int (src/probe.c), kept equal to the setting; null without the hook.
std::atomic<int*> shake_cell{nullptr};
// The upscaler quality modes' scene size (src/scene_res_hook.h Gow3SceneSize: requested width and
// height, then the applied ones); null when the game started with the native scene size.
std::atomic<uint32_t*> scene_size_cell{nullptr};
// Startup reminder of the quick menu: shown for a few seconds once the game runs.
std::chrono::steady_clock::time_point hint_start{};
std::atomic<bool> hint_done{false};
std::atomic<bool> loading{false};
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<const char*> loading_stage{nullptr};
std::atomic<u32> background_done{0}, background_total{0};
// A short message in a corner (saved screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<Gow3Png::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (value) {
        requested_tab = Gow3Settings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        Gow3Settings::Save();
    }
}

void SetQuickOpen(bool value) {
    if (quick_open.exchange(value) == value) {
        return;
    }
    if (!value && dirty && !menu_open) {
        dirty = false;
        Gow3Settings::Save();
    }
}

// The LOD distance hook's factor: the game divides the LOD switch distances by its multiplier
// (1.2 brings detail changes closer), so the setting (percent of the distance) is its inverse.
float LodScale() {
    return 100.0f / float(std::max(Gow3Settings::Get().lod_distance.load(), 1));
}

// pi/180 times the FOV scale: the hook multiplies the game's FOV (degrees) by it.
void ApplyFov() {
    if (float* cell = fov_cell.load()) {
        *cell = 0.017453292f * float(Gow3Settings::Get().fov.load()) / 100.0f;
    }
}

// Same path as closing the window (logs and cleanup).
void ExitGame() {
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text follow GOW3_GLYPHS (run.py sets xbox or nintendo when
// the glyph feature is on); unset is PlayStation. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("GOW3_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

// A 0.1x to 100x multiplier with a 1x button.
void MultiplierRow(const char* label, std::atomic<float>& value, const char* help) {
    Row(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("1x").x + style.FramePadding.x * 2 + style.ItemSpacing.x));
    float v = value;
    if (ImGui::SliderFloat("##value", &v, 0.1f, 100.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        value = Gow3Orbs::ClampMultiplier(v);
        dirty = true;
    }
    Help(help);
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) {
        value = 1.0f;
        dirty = true;
    }
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case Gow3Settings::UpscalerFsr3: return "FSR 3.1";
    case Gow3Settings::UpscalerFsr4: return "FSR 4";
    case Gow3Settings::UpscalerFsr411: return "FSR 4.1.1";
    case Gow3Settings::UpscalerTaa: return "TAA";
    case Gow3Settings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = Gow3PerfStats;
    const auto& s = Gow3Settings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

constexpr const char* Unsupported = "Unavailable: game version or code signature mismatch.";

void GameTab() {
    auto& s = Gow3Settings::Get();
    // gow3: God of War III's multipliers and enemy bar are hidden in games where none of their code
    // signatures matched (Killzone Shadow Fall, whose cheats have a tab of their own).
    ImGui::SeparatorText("Controller");
    if (BeginRows("##controller")) {
        SliderRow("Vibration", s.vibration, 0, 100, "%d%%",
                  "Strength of the controller's vibration. 0% turns it off.");
        ImGui::EndTable();
    }

    if (!s.red_orbs_supported.load() && !s.orb_pickup_supported.load() && !s.damage_supported.load() &&
        !s.actor_watch_supported.load()) {
        return;
    }
    ImGui::SeparatorText("Multipliers");
    if (ImGui::Button("Reset all to 1x")) {
        for (auto* m : {&s.red_orb_multiplier, &s.green_orb_multiplier, &s.blue_orb_multiplier,
                        &s.gold_orb_multiplier, &s.damage_dealt, &s.damage_taken}) {
            *m = 1.0f;
        }
        dirty = true;
    }
    Help("Every multiplier back to 1x (the game's normal values).");
    if (BeginRows("##multipliers")) {
        const bool max_orbs = false;
        ImGui::BeginDisabled(!s.red_orbs_supported.load() || max_orbs);
        MultiplierRow("Red orbs", s.red_orb_multiplier,
                      !s.red_orbs_supported.load() ? Unsupported
                      : max_orbs ? "Turn off Max red orbs to use the multiplier."
                                 : "Red orbs gained. Orbs you have and prices stay the same.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.orb_pickup_supported.load());
        const char* pickup = s.orb_pickup_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Green orbs (health)", s.green_orb_multiplier,
                      pickup ? pickup : "Health from each green orb. The bar stays within its maximum.");
        MultiplierRow("Blue orbs (magic)", s.blue_orb_multiplier,
                      pickup ? pickup : "Magic from each blue orb. The bar stays within its maximum.");
        MultiplierRow("Gold orbs (Rage of Sparta)", s.gold_orb_multiplier,
                      pickup ? pickup : "Rage of Sparta from each gold orb. The bar stays within its maximum.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.damage_supported.load());
        const char* damage = s.damage_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Damage dealt", s.damage_dealt, damage ? damage : "Damage Kratos does to enemies.");
        MultiplierRow("Damage taken", s.damage_taken, damage ? damage : "Damage Kratos takes.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    ImGui::SeparatorText("HUD");
    if (BeginRows("##hud")) {
        ImGui::BeginDisabled(!s.actor_watch_supported.load());
        CheckRow("Enemy health bar", s.enemy_health_bar,
                 s.actor_watch_supported.load() ? "A thin bar at the top for the last enemy hit." : Unsupported);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
}

void FloatSliderRow(const char* label, std::atomic<float>& value, float lo, float hi, const char* format,
                    const char* help) {
    Row(label);
    float v = value;
    if (ImGui::SliderFloat("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

// Killzone: the game's own hooks of God of War III (cheats, orbs) are absent, so the first tab
// holds the controls the launcher also sets; all apply at once.
bool KillzoneMenu() {
    const auto& s = Gow3Settings::Get();
    return !s.red_orbs_supported.load() && !s.orb_pickup_supported.load() &&
           !s.damage_supported.load() && !s.actor_watch_supported.load();
}

// Killzone's cheats (src/cheat_hook.h): the game's own debug switches, which only change the player,
// and the free camera (src/free_camera_hook.h). All apply at once.
void CheatsTab() {
    auto& s = Gow3Settings::Get();
    const auto row = [&](unsigned cheat, const char* label, const char* help) {
        const bool available = (s.cheats_supported.load() & (1u << cheat)) != 0;
        ImGui::BeginDisabled(!available);
        CheckRow(label, s.cheats[cheat], available ? help : Unsupported);
        ImGui::EndDisabled();
    };
    ImGui::SeparatorText("Cheats");
    if (BeginRows("##cheats")) {
        row(GOW3_CHEAT_GOD, "God mode", "The player takes no damage. Enemies and allies are unchanged.");
        row(GOW3_CHEAT_AMMO, "Infinite ammo", "Your reserve ammo and grenades are never used up.");
        row(GOW3_CHEAT_NO_RELOAD, "No reload", "The magazine never empties, so weapons never reload.");
        row(GOW3_CHEAT_SPRINT, "Unlimited sprint", "Stamina refills while you sprint.");
        row(GOW3_CHEAT_ECHO, "Continuous Tactical Echo",
            "An OWL scan keeps pulsing. Use the Tactical Echo once after each load; cooldowns stay.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Debug");
    if (BeginRows("##debug")) {
        row(GOW3_CHEAT_CHAPTERS, "Unlock all chapters",
            "Every chapter and section in Chapter Select, at any difficulty. Your save's progress is unchanged.");
        row(GOW3_CHEAT_FREE_CAMERA, "Free camera",
            "F9 toggles, F10 returns to the player. WASD move, Q / E down / up, mouse or arrows look, "
            "Shift faster, Ctrl slower. Gamepad: sticks, L1 / R1 down / up, R2 faster, L2 slower. "
            "The player stands still meanwhile.");
        ImGui::EndTable();
    }
}

void ControlsTab() {
    auto& s = Gow3Settings::Get();
    ImGui::SeparatorText("Mouse");
    if (BeginRows("##mouse")) {
        FloatSliderRow("Sensitivity", s.mouse_sens, 0.2f, 4.0f, "%.1f",
                       "How far the camera turns for the mouse's motion (about 1 is recommended).");
        SliderRow("Fast turns", s.mouse_turn_boost, 1, 12, "x%d",
                  "Raises the game's turn speed limit while the mouse moves (x6 by default).");
        CheckRow("Invert Y axis", s.mouse_invert, "Moving the mouse up looks down.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Camera");
    if (BeginRows("##camera")) {
        const bool fov_ready = fov_cell.load() != nullptr;
        ImGui::BeginDisabled(!fov_ready);
        Row("Field of view (FOV)");
        int fov = s.fov;
        if (ImGui::SliderInt("##value", &fov, 80, 200, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            fov = std::clamp((fov + 2) / 5 * 5, 80, 200);
            Store(s.fov, fov, fov != s.fov.load());
            ApplyFov();
        }
        Help(fov_ready ? "Percent of the game's horizontal field of view (100% = as on PS4); aiming zoom "
                         "widens in the same proportion."
                       : "Unavailable: the game's camera code was not found in this eboot.bin.");
        ImGui::PopID();
        ImGui::EndDisabled();
        const bool shake_ready = shake_cell.load() != nullptr;
        ImGui::BeginDisabled(!shake_ready);
        CheckRow("Camera shake", s.camera_shake,
                 shake_ready ? "Shaking from explosions, hits and the like. Weapon recoil stays."
                             : "Unavailable: the game's camera code was not found in this eboot.bin.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Controller");
    if (BeginRows("##controller")) {
        SliderRow("Vibration", s.vibration, 0, 100, "%d%%", "0% turns vibration off.");
        SliderRow("Stick dead zone", s.pad_dead_zone, 0, 30, "%d%%",
                  "Raise it if the camera or character moves on its own (worn stick).");
        SliderRow("Controller speaker", s.pad_speaker, 0, 100, "%d%%",
                  "Sounds the PS4 sends to the controller's speaker; here they play on your speakers.");
        CheckRow(PadText("Hold Share for the quick menu", "Hold View for the quick menu",
                         "Hold - for the quick menu"),
                 s.quick_menu,
                 "Holding it about a second opens Resume / Settings / Exit game; a short press still "
                 "clicks the touchpad.");
        static const char* const combo_labels[] = {"R3 + L2", "L3 + R3", "Share + Options", "None (Insert only)"};
        ChoiceRow("Menu shortcut", s.menu_combo, combo_labels, nullptr, 4,
                  "Controller buttons that open this menu.");
        CheckRow("Tips at startup", s.quick_menu_hint,
                 "A short note when the game starts: the OWL (hold Delete and move the mouse) and the quick menu.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Keyboard and mouse");
    if (BeginRows("##prompts")) {
        static const char* const prompt_labels[] = {"Automatic", "Keyboard and mouse", "PS4", "Xbox"};
        ChoiceRow("Button prompts", s.prompt_style, prompt_labels, nullptr, 4,
                  "Gameplay button icons: the keys bound to them (Controls in the launcher), the PS4 "
                  "buttons or Xbox ones. Automatic follows the last device used: a PlayStation controller "
                  "shows PS4, any other one Xbox. Sticks keep their PS4 icons.");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("OWL drone: hold Delete and move the mouse (a finger on the touchpad).");
    ImGui::TextDisabled("Keys can be changed in the launcher (Controls).");
}

// Upscaler quality modes: the scene size of a mode at the output the launcher started FSR / DLSS
// at (GOW3_OUTPUT_RES), as the launcher's table.
Gow3Settings::SceneSize QualitySceneSize(int quality) {
    static const std::array<int, 2> output = [] {
        int w = 1920, h = 1080;
        if (const char* env = std::getenv("GOW3_OUTPUT_RES")) std::sscanf(env, "%dx%d", &w, &h);
        return std::array<int, 2>{w, h};
    }();
    return Gow3Settings::UpscaleSceneSize(quality, output[0], output[1]);
}

bool ReducedScene(Gow3Settings::SceneSize size) {
    return size.width < 1920 || size.height < 1080;
}

// A reduced scene size switches live once the game started with one (the cell exists); Native
// and a first reduced size need the launcher (GOW3_RENDER_RES), so a restart.
bool QualityNeedsRestart() {
    const auto& s = Gow3Settings::Get();
    const auto size = QualitySceneSize(s.upscale_quality);
    const auto startup = QualitySceneSize(s.startup_upscale_quality);
    const bool same = size.width == startup.width && size.height == startup.height;
    return !same && !(scene_size_cell.load() && ReducedScene(size));
}

void QualityRow() {
    auto& s = Gow3Settings::Get();
    static const std::array<std::string, Gow3Settings::UpscaleQualityCount> names = [] {
        static const char* const base[] = {"Native", "Quality", "Balanced", "Performance", "Ultra performance"};
        std::array<std::string, Gow3Settings::UpscaleQualityCount> out;
        for (int q = 0; q < Gow3Settings::UpscaleQualityCount; ++q) {
            const auto size = QualitySceneSize(q);
            out[q] = std::string(base[q]) + " (" + std::to_string(size.width) + "x" + std::to_string(size.height) + ")";
        }
        return out;
    }();
    static const std::array<const char*, Gow3Settings::UpscaleQualityCount> labels = [] {
        std::array<const char*, Gow3Settings::UpscaleQualityCount> out;
        for (int q = 0; q < Gow3Settings::UpscaleQualityCount; ++q) out[q] = names[q].c_str();
        return out;
    }();
    ChoiceRow("Upscaling quality", s.upscale_quality, labels.data(), nullptr, Gow3Settings::UpscaleQualityCount,
              scene_size_cell.load()
                  ? "The 3D scene drawn smaller, rebuilt by FSR / DLSS: more FPS when the graphics card is "
                    "the limit, a softer image. Between the reduced modes it switches at once; Native "
                    "needs a restart."
                  : "The 3D scene drawn smaller, rebuilt by FSR / DLSS: more FPS when the graphics card is "
                    "the limit, a softer image. Applied on restart (this launch started at Native); "
                    "after that the reduced modes switch at once.");
}

// Killzone: upscaler and frame generation. Inside an upscaler session (the launcher started the
// game through FSR / DLSS) both switch live; frame generation turned on from off needs a restart
// (the swapchain is made for it). The launcher adopts the choice (upscaling_from_menu).
void UpscalingSection() {
    auto& s = Gow3Settings::Get();
    static const bool session = [] {
        const char* output = std::getenv("GOW3_OUTPUT_RES");
        const char* upscaler = std::getenv("GOW3_UPSCALER");
        return output && *output && upscaler && std::strcmp(upscaler, "none") != 0 &&
               std::strcmp(upscaler, "off") != 0;
    }();
    ImGui::SeparatorText("Upscaling and frame generation");
    if (!BeginRows("##upscaling")) {
        return;
    }
    if (session) {
        // FSR 4 (v07 INT8 model) when the card runs it; its shaders come from the launcher download
        // (fsr4_shaders) and a missing file falls back to FSR 3.1 (fsr4_problem says why).
        const bool dlss = s.dlss_supported.load();
        const char* labels[3] = {"FSR 3.1"};
        int values[3] = {Gow3Settings::UpscalerFsr3};
        int count = 1;
        if (s.fsr4_supported.load()) {
            labels[count] = "FSR 4";
            values[count++] = Gow3Settings::UpscalerFsr4;
        }
        if (dlss) {
            labels[count] = "DLSS";
            values[count++] = Gow3Settings::UpscalerDlss;
        }
        const int before = s.upscaler;
        ChoiceRow("Upscaler", s.upscaler, labels, values, count,
                  dlss ? "Switches at once." : "DLSS needs an NVIDIA RTX card and its files.");
        if (s.upscaler != before) {
            s.upscaling_from_menu = true;
        }
        if (const char* problem = s.fsr4_problem.load()) {
            InfoRow("", problem);
        }
        QualityRow();
    } else {
        InfoRow("Upscaler", "Off (1080p): choose FSR or DLSS in the launcher");
    }
    const bool dlssg = s.dlssg_max_multiplier.load() >= 1;
    const char* const fg_labels[] = {"Off", "FSR 3.1", "DLSS"};
    const int fg_values[] = {Gow3Settings::FrameGenOff, Gow3Settings::FrameGenFsr3, Gow3Settings::FrameGenDlss};
    const int before = s.frame_gen;
    ChoiceRow("Frame generation", s.frame_gen, fg_labels, fg_values, dlssg ? 3 : 2,
              s.startup_frame_gen == Gow3Settings::FrameGenOff
                  ? "Turning it on needs a restart. Doubles the frames shown; best above 60 FPS."
                  : "Switches at once. Doubles the frames shown; best above 60 FPS.");
    if (s.frame_gen != before) {
        s.upscaling_from_menu = true;
    }
    if (const char* problem = s.frame_gen_problem.load()) {
        InfoRow("", problem);
    }
    ImGui::EndTable();
}

void ImageTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    ImGui::SeparatorText("Display");
    if (BeginRows("##display")) {
        ChoiceRow("Display mode", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Windowed, borderless window or exclusive fullscreen.");
        CheckRow("VSync", s.vsync, "Waits for the monitor: no tearing, a little more input delay.");
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    ImGui::SeparatorText("Sharpening");
    if (BeginRows("##scaling")) {
        CheckRow("Upscaling (FSR 1)", s.fsr1,
                 "AMD FSR 1 when the window is larger than the game's 1080p image (1440p, 4K): "
                 "sharper than plain stretching. No effect at 1080p.");
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Sharpening (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "No effect: the game's image is larger than the window."
                 : rcas_blocked              ? "No effect: the game's image is smaller than the window."
                                             : "Sharpens the final image.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Sharpening strength", s.rcas_strength, 0, 100, "%d%%", "How strong the sharpening is.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (KillzoneMenu()) {
        UpscalingSection();
        ImGui::SeparatorText("Textures");
        if (BeginRows("##texture_live")) {
            // The launcher's steps (MIP_BIASES), so both show the same value.
            static constexpr float biases[] = {-2.0f, -1.0f, -0.5f, 0.0f, 0.5f, 1.0f, 2.0f};
            static const char* const labels[] = {"Sharpest (-2.0)", "Sharper (-1.0)", "Sharp (-0.5)",
                                                 "Game default (0)", "Soft (+0.5)", "Softer (+1.0)",
                                                 "Softest (+2.0)"};
            int current = 0;
            for (int i = 1; i < 7; ++i) {
                if (std::abs(biases[i] - s.mip_lod_bias) < std::abs(biases[current] - s.mip_lod_bias)) current = i;
            }
            Row("Texture sharpness (LOD bias)");
            if (ImGui::Combo("##value", &current, labels, 7)) {
                Store(s.mip_lod_bias, biases[current], true);
            }
            Help("Sharper: crisper textures at a distance, a little more shimmer. Softer: smoother.");
            ImGui::PopID();
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Level of detail");
        if (BeginRows("##lod_live")) {
            ImGui::BeginDisabled(lod_cell.load() == nullptr);
            Row("Detail distance (LOD)");
            int lod = s.lod_distance;
            if (ImGui::SliderInt("##value", &lod, 50, 300, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
                lod = std::clamp((lod + 5) / 10 * 10, 50, 300);
                Store(s.lod_distance, lod, lod != s.lod_distance.load());
            }
            Help(lod_cell.load() ? "How far models keep their detailed versions (100% = as on PS4). Higher: "
                                   "less pop-in of characters and objects, more GPU and CPU work. Lower: "
                                   "faster, detail drops sooner."
                                 : "Unavailable: the game's LOD code was not found in this eboot.bin.");
            ImGui::PopID();
            ImGui::EndDisabled();
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Effects (applied on restart)");
        if (BeginRows("##effects")) {
            CheckRow("Motion blur", s.motion_blur, "Blur of fast camera and object motion.");
            CheckRow("Depth of field", s.depth_of_field,
                     "Out-of-focus background when aiming and in cutscenes.");
            CheckRow("Bloom", s.bloom, "Glow around bright lights.");
            CheckRow("Film grain", s.film_grain, "The game's grain over the image.");
            ImGui::EndTable();
        }
    }
    ImGui::SeparatorText("Textures (applied on restart)");
    if (BeginRows("##textures")) {
        static const char* const anisotropy_labels[] = {"Game default", "2x", "4x", "8x", "16x"};
        ChoiceRow("Anisotropic filtering", s.anisotropy, anisotropy_labels, G::Anisotropies.data(),
                  int(G::Anisotropies.size()),
                  "Sharper floors and walls seen at an angle. Little cost on most GPUs.");
        ImGui::EndTable();
    }
    ImGui::TextDisabled("F12 saves a screenshot to the screenshots folder.");
    ImGui::SeparatorText("Information");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", iw, ih);
        InfoRow("Game image", text);
        std::snprintf(text, sizeof(text), "%d x %d", ww, wh);
        InfoRow("Window", text);
        InfoRow("Sharpening", s.rcas_applied ? "applied" : "not applied");
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = Gow3Graphics;
    auto& s = Gow3Settings::Get();
    ImGui::SeparatorText("Live");
    if (BeginRows("##live")) {
        static const char* const fps_labels[] = {"30", "60", "120", "144", "240", "Unlimited"};
        ChoiceRow("Frame rate limit", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Caps the frame rate. The engine frame rate (below) is the highest it can go.");
        const int effective = G::EffectiveFps(s.fps_limit, s.applied_engine_fps);
        // gow3: games without frame rate patches (Killzone) have no engine frame rate to show.
        static const bool game_patches = [] {
            const char* env = std::getenv("GOW3_PATCHES_ONLY");
            const char* fps = std::getenv("GOW3_FPS_PATCHES");
            return env && env[0] == '1' && !(fps && fps[0] == '0');
        }();
        if (game_patches && (s.fps_limit == 0 || effective < s.fps_limit)) {
            char text[48];
            std::snprintf(text, sizeof(text), "%d FPS (engine frame rate)", effective);
            InfoRow("Effective limit", text);
        }
        // gow3: a vblank-paced session (launcher 30 / 60) cannot go above 60 live.
        static const bool vblank_paced = [] {
            const char* v = std::getenv("GOW3_VBLANK_HZ");
            return !v || std::atoi(v) > 0;
        }();
        if (!game_patches && vblank_paced && (s.fps_limit == 0 || s.fps_limit > 60)) {
            InfoRow("Effective limit", "60 FPS: choose 144 / 240 / Unlimited in the launcher");
        }
        if (game_patches && s.applied_engine_fps < s.startup_engine_fps) {
            InfoRow("Engine frame rate", "60 FPS: the frame rate patch is not available for this game build");
        }
        static const char* const queued_labels[] = {"1 (lowest latency)", "2", "Unlimited"};
        ChoiceRow("Frames queued", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "More frames queued: steadier FPS, slower response to input.");
        CheckRow("Compile new shaders in the background", s.async_shaders,
                 "Fewer stutters in new areas; an object or effect may appear a moment later.");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Applied on restart");
    if (BeginRows("##restart")) {
        static const bool game_patches = [] {
            const char* env = std::getenv("GOW3_PATCHES_ONLY");
            return env && env[0] == '1';
        }();
        if (game_patches) {
        // Killzone: above 1080p the launcher's new method has the port draw at GOW3_OUTPUT_RES while
        // the game stays at 1080p; the patch's row would wrongly read "Native" and mix both methods.
        static const char* const port_output = [] {
            const char* env = std::getenv("GOW3_OUTPUT_RES");
            return env && *env && std::strcmp(env, "1920x1080") != 0 ? env : nullptr;
        }();
        // Upscaler quality modes: the 3D scene at GOW3_RENDER_RES (src/scene_res_hook.h).
        static const char* const scene_size = [] {
            const char* env = std::getenv("GOW3_RENDER_RES");
            return env && *env && std::strcmp(env, "1920x1080") != 0 ? env : nullptr;
        }();
        if (port_output || scene_size) {
            char text[128];
            int w = 0, h = 0, sw = 0, sh = 0;
            std::sscanf(port_output ? port_output : "1920x1080", "%dx%d", &w, &h);
            if (scene_size && std::sscanf(scene_size, "%dx%d", &sw, &sh) == 2) {
                // The size the upscaler gets now (the quality mode can change it live).
                sw = s.active_render_width.load();
                sh = s.active_render_height.load();
                std::snprintf(text, sizeof(text), "%d x %d scene, %d x %d output (quality mode)", sw, sh, w, h);
            } else {
                std::snprintf(text, sizeof(text), "%d x %d (port; change it in the launcher)", w, h);
            }
            InfoRow("Render resolution", text);
        } else {
        static const char* const resolution_labels[] = {"Native (1080p)", "480p", "720p", "1440p", "1800p", "4K"};
        ChoiceRow("Render resolution", s.render_resolution, resolution_labels, nullptr, int(G::Resolutions.size()),
                  "The resolution the game draws at. Higher is sharper and slower.");
        }
        static const bool fps_patches = [] {
            const char* env = std::getenv("GOW3_FPS_PATCHES");
            return !(env && env[0] == '0');
        }();
        if (fps_patches) {
        static const char* const engine_labels[] = {"60 (original)", "120", "240"};
        ChoiceRow("Engine frame rate", s.engine_fps, engine_labels, G::EngineFps.data(), int(G::EngineFps.size()),
                  "The highest frame rate the game itself runs at.");
        }
        }
        CheckRow("Game reads GPU data without waiting", s.stale_readback,
                 "Big FPS gain. Switch it off if lighting or objects look wrong.");
        CheckRow("Deferred GPU readbacks", s.deferred_readback,
                 "Fewer waits for the GPU. Switch it off if textures or shadows look wrong.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = Gow3PerfStats;
    auto& s = Gow3Settings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Show overlay", s.show_fps, "The performance overlay in a corner of the screen.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText("What to show");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Frametime", "Upscaler", "GPU (name and use)", "CPU (game)",
                                            "RAM (game / total)", "VRAM (used / available)"};
        for (unsigned item = 0; item < 7; ++item) {
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Preview");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? "(nothing selected)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Appearance");
    if (BeginRows("##appearance")) {
        SliderRow("Font size", s.overlay_scale, 50, 300, "%d%%", "Size of the overlay text.");
        static const char* const corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
        ChoiceRow("Corner", s.overlay_corner, corners, nullptr, 4, "Where the overlay sits.");
        SliderRow("Background opacity", s.overlay_opacity, 0, 100, "%d%%", "0% is no background.");
        static const char* const layouts[] = {"One line", "One item per line"};
        ChoiceRow("Layout", s.overlay_layout, layouts, nullptr, 2, "Items side by side or stacked.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("About");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Game", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "loading %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "ready");
        }
        InfoRow("Shader cache", text);
        std::string hooks;
        const auto add = [&](bool on, const char* name) {
            if (on) {
                hooks += hooks.empty() ? name : std::string(", ") + name;
            }
        };
        add(s.cheats_supported.load() != 0, "cheats");
        add(s.red_orbs_supported.load(), "red orbs");
        add(s.orb_pickup_supported.load(), "orb pickups");
        add(s.damage_supported.load(), "damage");
        add(s.actor_watch_supported.load(), "enemy health");
        InfoRow("Game hooks", hooks.empty() ? "none" : hooks.c_str());
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = Gow3Settings::Get();
    return (s.render_resolution != s.startup_render_resolution) + (s.engine_fps != s.startup_engine_fps) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.anisotropy != s.startup_anisotropy) + (s.motion_blur != s.startup_motion_blur) +
           (s.depth_of_field != s.startup_depth_of_field) + (s.bloom != s.startup_bloom) +
           (s.film_grain != s.startup_film_grain) + QualityNeedsRestart() +
           // Frame generation turned on in a launch that started without it (UpscalingSection).
           (s.startup_frame_gen == Gow3Settings::FrameGenOff && s.frame_gen != Gow3Settings::FrameGenOff);
}

// Hold Select/Back/Share (runtime_pad.c): a small menu a controller alone can use.
void QuickMenu() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    bool close = false;
    if (ImGui::Begin("##quick_menu", nullptr, flags)) {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        ImGui::TextUnformatted(title.empty() ? "Game" : std::string(title).c_str());
        ImGui::Separator();
        const ImVec2 size(280.0f * base_scale, 0.0f);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        if (ImGui::Button("Resume", size)) {
            close = true;
        }
        if (ImGui::Button("Settings", size)) {
            SetQuickOpen(false);
            SetOpen(true);
        }
        if (ImGui::Button("Exit game", size)) {
            Gow3Settings::Save();
            ExitGame();
        }
        ImGui::TextDisabled("%s", PadText("Cross: choose    Circle: back", "A: choose    B: back",
                                          "B: choose    A: back"));
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close = true;
        }
    }
    ImGui::End();
    if (close) {
        SetQuickOpen(false);
    }
}

// Startup tips (quick_menu_hint): for a few seconds once the game runs, a small non-blocking note
// at the bottom: the OWL on keyboard and mouse (players keep asking) and, with a controller, the
// quick menu shortcut. Fades out.
constexpr float HintSeconds = 10.0f;

// " / R3+L2" for the heading: the controller shortcut menu_combo sets, in this pad's names.
std::string MenuComboText() {
    switch (Gow3Settings::Get().menu_combo.load()) {
    case 0:
        return PadText(" / R3+L2", " / RS+LT", " / RS+ZL");
    case 1:
        return PadText(" / L3+R3", " / LS+RS", " / LS+RS");
    case 2:
        return PadText(" / Share+Options", " / View+Menu", " / -+(+)");
    default:
        return "";
    }
}

bool HintActive() {
    const auto& s = Gow3Settings::Get();
    return !hint_done.load() && s.quick_menu_hint.load() && !loading.load();
}

void QuickHint() {
    if (!HintActive()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (hint_start == std::chrono::steady_clock::time_point{}) {
        hint_start = now;
    }
    const float t = std::chrono::duration<float>(now - hint_start).count();
    if (t > HintSeconds) {
        hint_done = true;
        return;
    }
    const float alpha = t < HintSeconds - 1.0f ? 1.0f : HintSeconds - t;
    const char* lines[2];
    int count = 0;
    lines[count++] = "OWL drone: hold Delete and move the mouse   |   Insert: settings";
    if (Gow3Settings::Get().quick_menu.load() && SDL_HasGamepad()) {
        lines[count++] = PadText("Hold Share for the game menu", "Hold View for the game menu",
                                 "Hold - for the game menu");
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 10.0f * base_scale, line = ImGui::GetTextLineHeightWithSpacing();
    float width = 0.0f;
    for (int i = 0; i < count; ++i) {
        width = std::max(width, ImGui::CalcTextSize(lines[i]).x);
    }
    const float top = viewport->WorkPos.y + viewport->WorkSize.y * 0.86f;
    const float left = viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(left - pad, top - pad * 0.6f), ImVec2(left + width + pad, top + line * count + pad * 0.3f),
                        IM_COL32(0, 0, 0, int(150 * alpha)), 6.0f * base_scale);
    for (int i = 0; i < count; ++i) {
        const float x = viewport->WorkPos.x + (viewport->WorkSize.x - ImGui::CalcTextSize(lines[i]).x) * 0.5f;
        draw->AddText(ImVec2(x, top + line * i), IM_COL32(255, 255, 255, int(235 * alpha)), lines[i]);
    }
}

void Menu() {
    auto& s = Gow3Settings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // gow3: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "God of War III" : title);
    }();
    const std::string heading_text = heading + "  (Insert" + MenuComboText() + ")###menu";
    if (!ImGui::Begin(heading_text.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    const int TabCount = KillzoneMenu() ? 5 : 4;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        const char* const tabs[] = {KillzoneMenu() ? "Controls" : "Game", "Image", "Performance", "Overlay",
                                    "Cheats"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 0:
                    if (KillzoneMenu()) {
                        ControlsTab();
                    } else {
                        GameTab();
                    }
                    break;
                case 1: ImageTab(); break;
                case 2: PerformanceTab(); break;
                case 3: OverlayTab(); break;
                default: CheatsTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", footer_help ? footer_help : PadText("L1 / R1 or Q / E: switch tabs. Settings are saved automatically.", "LB / RB or Q / E: switch tabs. Settings are saved automatically.", "L / R or Q / E: switch tabs. Settings are saved automatically."));
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
        ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("GOW3_RESTART_COMMAND"));
        if (ImGui::Button("Apply and restart")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            Gow3Settings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
    if (ImGui::Button("Exit game")) {
        Gow3Settings::Save();
        ExitGame();
    }
    ImGui::SameLine();
    const float close_width = ImGui::CalcTextSize("Close").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Close")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// Enemy health bar from the damage hook's records (gow3_actors.cpp).
// Only the copied values are read, never the game's memory: an actor may be gone by now.
void HealthDisplay() {
    struct Seen {
        uint32_t seq = 0;
        std::chrono::steady_clock::time_point at{};
    };
    static Seen seen[2];
    const auto& s = Gow3Settings::Get();
    if (!s.actor_watch_supported.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    Gow3ActorSlot slot[2];
    for (int i = 0; i < 2; ++i) {
        std::memcpy(&slot[i], const_cast<const Gow3ActorSlot*>(&gow3_actor_slots[i]), sizeof(slot[i]));
        if (slot[i].seq != seen[i].seq) {
            seen[i] = {slot[i].seq, now};
        }
    }
    const auto recent = [&](int i, int seconds) {
        return seen[i].seq != 0 && now - seen[i].at < std::chrono::seconds(seconds);
    };
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float scale = base_scale;
    if (s.enemy_health_bar && recent(1, 4) && Gow3Actors::Valid(slot[1].health, slot[1].max_health)) {
        // Minimal: a thin green bar centered on the screen's top edge; it shrinks toward the center
        // so it stays symmetric, with the health in numbers below.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float width = 260.0f * scale, height = 5.0f * scale, center = display.x * 0.5f;
        const float top = 36.0f * scale;
        const float fraction = Gow3Actors::Fraction(slot[1].health, slot[1].max_health);
        draw->AddRectFilled(ImVec2(center - width * 0.5f - scale, top - scale),
                            ImVec2(center + width * 0.5f + scale, top + height + scale),
                            IM_COL32(0, 0, 0, 140), 2 * scale);
        const float half = width * 0.5f * fraction;
        draw->AddRectFilled(ImVec2(center - half, top), ImVec2(center + half, top + height),
                            IM_COL32(70, 190, 80, 220), 2 * scale);
        char text[48];
        std::snprintf(text, sizeof(text), "%.0f / %.0f", std::ceil(slot[1].health), slot[1].max_health);
        const float font = ImGui::GetFontSize() * 0.9f;
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, text);
        const ImVec2 at{center - size.x * 0.5f, top + height + 4.0f * scale};
        draw->AddText(ImGui::GetFont(), font, ImVec2(at.x + scale, at.y + scale), IM_COL32(0, 0, 0, 160), text);
        draw->AddText(ImGui::GetFont(), font, at, IM_COL32(225, 235, 220, 220), text);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Keyboard: type, Backspace to erase, Enter = OK, Esc = cancel");
    ImGui::TextDisabled("%s", PadText("Controller: Cross (A) = OK, Circle (B) = cancel", "Controller: A = OK, B = cancel", "Controller: B = OK, A = cancel"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.85f, 0.72f, 0.45f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.55f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", PadText("Up/Down or D-pad: choose   Enter or Cross (A): OK   Esc or Circle (B): cancel", "Up/Down or D-pad: choose   Enter or A: OK   Esc or B: cancel", "Up/Down or D-pad: choose   Enter or B: OK   Esc or A: cancel"));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(total ? "Loading shader cache..." : "Preparing graphics...");
    ImGui::SetWindowFontScale(1.0f);
    if (const char* stage = loading_stage) {
        ImGui::TextUnformatted(stage);
    }
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled("The first start after an update takes longer.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = Gow3Settings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Loading shader cache %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

void Notice() {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

} // namespace

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "gow3";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;
    // Killzone port: green accents instead of the dark theme's blue (the other ports keep it).
    const auto green = [](float alpha) { return ImVec4(0.30f, 0.78f, 0.38f, alpha); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.30f, 0.16f, 0.54f);
    colors[ImGuiCol_FrameBgHovered] = green(0.40f);
    colors[ImGuiCol_FrameBgActive] = green(0.67f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.28f, 0.14f, 1.0f);
    colors[ImGuiCol_CheckMark] = green(1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.66f, 0.32f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = green(1.0f);
    colors[ImGuiCol_Button] = green(0.40f);
    colors[ImGuiCol_ButtonHovered] = green(1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.62f, 0.24f, 1.0f);
    colors[ImGuiCol_Header] = green(0.31f);
    colors[ImGuiCol_HeaderHovered] = green(0.80f);
    colors[ImGuiCol_HeaderActive] = green(1.0f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.12f, 0.50f, 0.20f, 0.78f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.12f, 0.50f, 0.20f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = green(0.20f);
    colors[ImGuiCol_ResizeGripHovered] = green(0.67f);
    colors[ImGuiCol_ResizeGripActive] = green(0.95f);
    colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.34f, 0.18f, 0.86f);
    colors[ImGuiCol_TabHovered] = green(0.80f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.48f, 0.24f, 1.0f);
    colors[ImGuiCol_TabSelectedOverline] = green(1.0f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.07f, 0.15f, 0.09f, 0.97f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.12f, 0.30f, 0.16f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = green(0.35f);
    colors[ImGuiCol_NavCursor] = green(1.0f);
    colors[ImGuiCol_TextLink] = green(1.0f);

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(gow3_font_ttf),
                                   int(gow3_font_ttf_end - gow3_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const Gow3Png::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open || quick_open;
    // Insert / R3+L2: the settings menu (from the quick menu too); Esc closes either.
    const auto toggle_menu = [] {
        if (quick_open) {
            SetQuickOpen(false);
            SetOpen(true);
        } else {
            SetOpen(!menu_open);
        }
    };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("GOW3_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (event.key.key == SDLK_F12) {
            if (down && !event.key.repeat) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            return true;
        }
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            if (event.key.key == SDLK_INSERT) {
                toggle_menu();
            } else if (quick_open) {
                SetQuickOpen(false);
            } else {
                SetOpen(false);
            }
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 by default (L3+R3 is a game action in God of War III); menu_combo picks another.
        const int combo = Gow3Settings::Get().menu_combo.load();
        bool* held = button == SDL_GAMEPAD_BUTTON_RIGHT_STICK ? &r3_down
                     : button == SDL_GAMEPAD_BUTTON_LEFT_STICK ? &l3_down
                     : button == SDL_GAMEPAD_BUTTON_BACK       ? &back_down
                     : button == SDL_GAMEPAD_BUTTON_START      ? &start_down
                                                               : nullptr;
        if (held) {
            const bool was = *held;
            *held = down;
            const bool pressed = down && !was;
            const bool r3_l2 = combo == 0 && button == SDL_GAMEPAD_BUTTON_RIGHT_STICK && l2_down;
            const bool l3_r3 = combo == 1 && l3_down && r3_down &&
                               (button == SDL_GAMEPAD_BUTTON_LEFT_STICK || button == SDL_GAMEPAD_BUTTON_RIGHT_STICK);
            const bool back_start = combo == 2 && back_down && start_down &&
                                    (button == SDL_GAMEPAD_BUTTON_BACK || button == SDL_GAMEPAD_BUTTON_START);
            if (pressed && (r3_l2 || l3_r3 || back_start)) {
                toggle_menu();
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !l2_down && r3_down && Gow3Settings::Get().menu_combo.load() == 0) {
                l2_down = true;
                toggle_menu();
                return true;
            }
            l2_down = down;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = Gow3Settings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    Gow3Settings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
    if (!active) {
        loading_stage = nullptr;
    }
}

void SetLoadingStage(const char* stage) {
    loading_stage = stage;
}

bool Visible() {
    const auto& s = Gow3Settings::Get();
    return initialized && (loading || background_total || menu_open || quick_open || text_entry_active ||
                           choice_active || choice_retiring || s.show_fps || NoticeActive() || HintActive() ||
                           (s.actor_watch_supported && s.enemy_health_bar));
}

bool CapturesInput() {
    return menu_open || quick_open || text_entry_active || choice_active;
}

void OpenQuickMenu() {
    if (initialized && !menu_open) {
        quick_open = true;
    }
}

void SetFovCell(float* cell) {
    fov_cell = cell;
    ApplyFov();
}

void SetLodCell(float* cell) {
    *cell = LodScale();
    lod_cell = cell;
}

void SetShakeCell(int* cell) {
    *cell = Gow3Settings::Get().camera_shake.load() ? 1 : 0;
    shake_cell = cell;
}

void SetSceneSizeCell(uint32_t* cell) {
    scene_size_cell = cell;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<Gow3Png::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    // Automated tests: GOW3_SCREENSHOT_FILE names a file whose appearance takes an F12
    // screenshot (user\screenshots); it is deleted. Checked twice a second.
    static const char* screenshot_trigger = std::getenv("GOW3_SCREENSHOT_FILE");
    if (screenshot_trigger && *screenshot_trigger) {
        static auto next_check = std::chrono::steady_clock::now();
        if (const auto now = std::chrono::steady_clock::now(); now >= next_check) {
            next_check = now + std::chrono::milliseconds(500);
            if (std::remove(screenshot_trigger) == 0) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
        }
    }
    ConfirmRestart();
    // FOV (menu, launcher or a settings load after the hook was installed).
    static int applied_fov = -1;
    if (fov_cell.load() && Gow3Settings::Get().fov.load() != applied_fov) {
        applied_fov = Gow3Settings::Get().fov.load();
        ApplyFov();
    }
    if (int* cell = shake_cell.load()) {
        *cell = Gow3Settings::Get().camera_shake.load() ? 1 : 0;
    }
    if (float* cell = lod_cell.load()) {
        *cell = LodScale();
    }
    // Quality mode: a reduced size is requested in one store; the game's next frame makes the
    // scene targets again (src/scene_res_hook.h). Native waits for a restart.
    if (uint32_t* cell = scene_size_cell.load()) {
        auto& s = Gow3Settings::Get();
        const auto size = QualitySceneSize(s.upscale_quality.load());
        if (ReducedScene(size)) {
            const uint64_t packed = uint64_t(uint32_t(size.width)) | uint64_t(uint32_t(size.height)) << 32;
            __atomic_store_n(reinterpret_cast<uint64_t*>(cell), packed, __ATOMIC_RELAXED);
        }
        const uint64_t applied = __atomic_load_n(reinterpret_cast<uint64_t*>(cell + 2), __ATOMIC_RELAXED);
        s.scene_applied_width = int(uint32_t(applied));
        s.scene_applied_height = int(uint32_t(applied >> 32));
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (loading) {
        LoadingScreen();
    }
    io.MouseDrawCursor = menu_open || quick_open;
    if (menu_open) {
        Menu();
    } else if (quick_open) {
        QuickMenu();
    }
    if (!menu_open && !quick_open) {
        QuickHint();
    }
    if (Gow3Settings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (NoticeActive()) {
        Notice();
    }
    if (!loading && !menu_open) {
        HealthDisplay();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace Gow3Overlay
