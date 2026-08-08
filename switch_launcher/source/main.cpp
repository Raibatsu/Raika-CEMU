#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <map>
#include <unordered_map>
#include <iterator>
#include <array>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include "griddb.h"
#include "cemu_container_title.h"
#include "cemu_titles.h"
#include "cemu_settings.h"
#include "gfxpacks.h"
#include "install.h"
#include "forwarder.h"
#include "launcher_update.h"
#include "SwitchStorage.h"
#include "ui_audio.h"

// SDL uses Xbox button names.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A
#define BTN_SETTINGS SDL_CONTROLLER_BUTTON_Y

static const char *DATA_DIR    = "sdmc:/switch/cemu";
static const char *EMU_HOST_DIR= "sdmc:/switch/cemu/.emu";
static const char *LAUNCHER_INI= "sdmc:/switch/cemu/launcher.ini";
static const char *COVERS_DIR  = "sdmc:/switch/cemu/covers";
static const char *GAMECFG_DIR = "sdmc:/switch/cemu/gamecfg";
static const char *DEF_GAMEDIR = "sdmc:/switch/cemu/games";
static const char *SETTINGS_XML= "sdmc:/switch/cemu/settings.xml";
static const char *GAMEPROFILES_DIR = "sdmc:/switch/cemu/gameProfiles";
static const char *GRAPHICPACKS_DIR = "sdmc:/switch/cemu/graphicPacks";
static const char *LSFG_DIR = "sdmc:/switch/cemu/lsfg";
static const char *LSFG_DLL_FILE = "sdmc:/switch/cemu/lsfg/Lossless.dll";
static const char *LAUNCHER_NRO = "sdmc:/switch/cemu/cemu.nro";
static std::string g_launcherNroPath = LAUNCHER_NRO;
static const char *EMU_NRO_SRC = "romfs:/emu/cemu_vk.nro";
static const char *EMU_HASH_SRC = "romfs:/emu/cemu_vk.sha256";
static const char *EMU_NRO_DST = "sdmc:/switch/cemu/.emu/cemu_vk.nro";
static const char *LAUNCH_HANDOFF = "sdmc:/switch/cemu/switch.ini";

struct KV { std::string k, v; };
struct Store { std::vector<KV> kv; };

static Store g_global;
static Store g_game;
static Store g_titles;
static Store g_containerTitles;
static Store *g_active = &g_global;
static int g_systemCemuLanguage = 1;
static const char *g_systemLanguageName = "English";
static const char *TITLES_INI = "sdmc:/switch/cemu/titles.ini";
static const char *CONTAINER_TITLES_INI = "sdmc:/switch/cemu/container_titles.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static void setLauncherPathFromArg(const char *path) {
  if(!path||!path[0]) return;
  std::string candidate=path;
  std::replace(candidate.begin(),candidate.end(),'\\','/');
  if(candidate.rfind("sdmc:/",0)!=0||candidate.find("/../")!=std::string::npos||
     candidate.find("/./")!=std::string::npos) return;
  std::string lowercase=candidate;
  std::transform(lowercase.begin(),lowercase.end(),lowercase.begin(),
    [](unsigned char value){ return (char)std::tolower(value); });
  if(lowercase.size()<4||lowercase.compare(lowercase.size()-4,4,".nro")!=0) return;
  g_launcherNroPath=std::move(candidate);
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  for (auto &e : s.kv) if (e.k == key) return e.v.c_str();
  return def;
}
static void storeSet(Store &s, const char *key, const char *val) {
  for (auto &e : s.kv) if (e.k == key) { e.v = val; return; }
  s.kv.push_back({ key, val });
}
static void storeRemove(Store &s, const char *key) {
  for (size_t i = 0; i < s.kv.size(); i++) if (s.kv[i].k == key) { s.kv.erase(s.kv.begin()+i); return; }
}
static void storeRemovePrefix(Store &s, const char *prefix) {
  const size_t length = strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(), s.kv.end(), [&](const KV &entry) {
    return entry.k.compare(0, length, prefix) == 0;
  }), s.kv.end());
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool regularFileExists(const std::string &path) {
  bool exists = false;
  return queryRegularFile(path, exists) && exists;
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;

  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file) return false;
  bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# Cemu launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

enum OType { OT_CHOICE, OT_RANGE, OT_ACTION };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d }
#define O_ACTION(l)            { l, nullptr, OT_ACTION, nullptr,0, 0,0,0, nullptr }

static const Choice C_cpumode[]  = { {"Multi-core recompiler","3"}, {"Single-core recompiler","1"},
                                     {"Interpreter (slow)","0"} };
// ActiveSettings stores the CPU timer as a shift value.
static const Choice C_timer[]    = { {"1x (normal)","3"}, {"2x faster","2"}, {"4x faster","1"}, {"8x faster","0"},
                                     {"0.5x slower","4"}, {"0.25x slower","5"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_bool01[]   = { {"Off","0"}, {"On","1"} };
static const Choice C_barriers[] = { {"Disabled","false"}, {"Enabled","true"} };
static const Choice C_filter[]   = { {"Bilinear","0"}, {"Bicubic","1"}, {"Hermite","2"}, {"Nearest","3"} };
static const Choice C_scaling[]  = { {"Keep aspect ratio","0"}, {"Stretch","1"} };
static const Choice C_ovpos[]    = { {"Disabled","0"}, {"Top-left","1"}, {"Top-center","2"}, {"Top-right","3"},
                                     {"Bottom-left","4"}, {"Bottom-center","5"}, {"Bottom-right","6"} };
static const Choice C_lang[]     = { {"Auto","-1"}, {"Japanese","0"}, {"English","1"}, {"French","2"}, {"German","3"},
                                     {"Italian","4"}, {"Spanish","5"}, {"Chinese","6"}, {"Korean","7"},
                                     {"Dutch","8"}, {"Portuguese","9"}, {"Russian","10"}, {"Taiwanese","11"} };
static const Choice C_ctype[]    = { {"Wii U GamePad","GamePad"}, {"Pro Controller","Pro"}, {"Classic Controller","Classic"} };
static const Choice C_gamepadLayout[] = { {"TV only","off"}, {"PAD only","pad"}, {"PAD on right","right"}, {"PAD on left","left"},
                                           {"PAD below TV","below"}, {"PAD above TV","above"} };
static const Choice C_lsfgFlowScale[] = { {"Quarter resolution","0.25"}, {"Half resolution","0.5"} };
static const Choice C_players[]  = { {"1","1"}, {"2","2"}, {"3","3"}, {"4","4"},
                                     {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
struct InputMapping { const char *label, *emu, *key, *def; };
static const InputMapping C_inputMappings[] = {
  {"A", "A", "in_A", "A"}, {"B", "B", "in_B", "B"},
  {"X", "X", "in_X", "X"}, {"Y", "Y", "in_Y", "Y"},
  {"L", "L", "in_L", "L"}, {"R", "R", "in_R", "R"},
  {"ZL", "ZL", "in_ZL", "ZL"}, {"ZR", "ZR", "in_ZR", "ZR"},
  {"Plus", "Plus", "in_Plus", "PLUS"}, {"Minus", "Minus", "in_Minus", "MINUS"},
  {"D-Pad Up", "Up", "in_Up", "DUP"}, {"D-Pad Down", "Down", "in_Down", "DDOWN"},
  {"D-Pad Left", "Left", "in_Left", "DLEFT"}, {"D-Pad Right", "Right", "in_Right", "DRIGHT"},
  {"L-Stick click", "StickL", "in_StickL", "LCLICK"},
  {"R-Stick click", "StickR", "in_StickR", "RCLICK"},
};
static const Choice C_launcherTheme[] = { {"XMB (PS3)","xmb"}, {"Bubbles","homebrew"}, {"Glow","animated"},
                                          {"Classic","classic"}, {"OLED black","oled"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };

enum { SCR_CPU, SCR_GRAPHICS, SCR_FRAMEGEN, SCR_AUDIO, SCR_OVERLAY, SCR_INPUT, SCR_ACCESSORIES, SCR_COUNT };

static const Opt S_cpu[] = {
  O_CHOICE("CPU mode",         "cpuMode",       C_cpumode, "3"),
  O_CHOICE("CPU timer speed",  "TimerShiftFactor", C_timer, "3"),
  O_CHOICE("Hardware video decoding",  "H264HardwareDecode", C_bool,    "true"),
};
static const Opt S_graphics[] = {
  O_CHOICE("VSync",                 "VSync",             C_bool01,  "0"),
  O_CHOICE("Triple buffering",      "TripleBuffer",      C_bool01,  "1"),
  O_CHOICE("Async shader compile",  "AsyncCompile",      C_bool,    "true"),
  O_CHOICE("Accurate barriers (Vulkan)", "vkAccurateBarriers", C_barriers, "true"),
  O_CHOICE("Upscale filter",        "UpscaleFilter",     C_filter,  "1"),
  O_CHOICE("Downscale filter",      "DownscaleFilter",   C_filter,  "0"),
  O_CHOICE("Fullscreen scaling",    "FullscreenScaling", C_scaling, "0"),
  O_CHOICE("GamePad screen",        "GamePadLayout",     C_gamepadLayout, "off"),
};
static const Opt S_audio[] = {
  O_RANGE ("TV volume",        "TVVolume",    0, 100, 5, "50"),
  O_CHOICE("GamePad audio",    "PadAudio",    C_bool,    "true"),
  O_RANGE ("GamePad volume",   "PadVolume",   0, 100, 5, "50"),
  O_RANGE ("Audio latency",    "AudioDelay",  0, 23,  1, "2"),
};
static const Opt S_framegen[] = {
  O_CHOICE("LSFG 2x (Vulkan)", "Wrapper/LSFGEnabled", C_bool, "false"),
  O_CHOICE("Flow resolution", "Wrapper/LSFGFlowScale", C_lsfgFlowScale, "0.25"),
  O_CHOICE("Performance mode", "Wrapper/LSFGPerformance", C_bool, "true"),
};
static const Opt S_overlay[] = {
  O_CHOICE("FPS counter",      "OverlayFPS",         C_bool,  "false"),
  O_CHOICE("Overlay position", "OverlayPosition",    C_ovpos, "1"),
  O_CHOICE("Shader-compile notice","NotifShaderCompile",C_bool,"true"),
};
static const Opt O_console_language = O_CHOICE("Console language", "console_language", C_lang, "-1");
static const Opt S_input[] = {
  O_CHOICE("Controller type",  "in_type",   C_ctype, "GamePad"),
  O_CHOICE("Active players",   "in_players", C_players, "1"),
  O_CHOICE("Rumble",           "in_rumble", C_bool,  "true"),
  O_RANGE ("Stick dead zone",  "in_deadzone", 0, 50, 1, "15"),
  O_ACTION("Control mapping"),
};
static const Opt S_accessories[] = {
  O_CHOICE("Skylanders Portal",      "UsbSkylanders", C_bool, "false"),
  O_CHOICE("Disney Infinity Base",   "UsbInfinity",   C_bool, "false"),
  O_CHOICE("LEGO Dimensions Toypad", "UsbDimensions", C_bool, "false"),
};
static const Opt S_launcher[] = {
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "homebrew"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "5"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", C_bool,   "true"),
};
struct Screen { const char *title; const Opt *opts; int n; };
static const Screen g_screens[SCR_COUNT] = {
  { "CPU / Emulation",   S_cpu,      (int)(sizeof(S_cpu)/sizeof(Opt)) },
  { "Graphics",          S_graphics, (int)(sizeof(S_graphics)/sizeof(Opt)) },
  { "Frame Generation",  S_framegen, (int)(sizeof(S_framegen)/sizeof(Opt)) },
  { "Audio",             S_audio,    (int)(sizeof(S_audio)/sizeof(Opt)) },
  { "Overlay",           S_overlay,  (int)(sizeof(S_overlay)/sizeof(Opt)) },
  { "Controller / Input",S_input,    (int)(sizeof(S_input)/sizeof(Opt)) },
  { "USB Accessories",   S_accessories, (int)(sizeof(S_accessories)/sizeof(Opt)) },
};

static void detectSystemLanguage() {
  if(R_FAILED(setInitialize())) return;
  u64 code=0;
  SetLanguage language=SetLanguage_ENUS;
  Result result=setGetSystemLanguage(&code);
  if(R_SUCCEEDED(result)) result=setMakeLanguage(code,&language);
  setExit();
  if(R_FAILED(result)) return;

  switch(language){
    case SetLanguage_JA: g_systemCemuLanguage=0; g_systemLanguageName="Japanese"; break;
    case SetLanguage_FR: case SetLanguage_FRCA: g_systemCemuLanguage=2; g_systemLanguageName="French"; break;
    case SetLanguage_DE: g_systemCemuLanguage=3; g_systemLanguageName="German"; break;
    case SetLanguage_IT: g_systemCemuLanguage=4; g_systemLanguageName="Italian"; break;
    case SetLanguage_ES: case SetLanguage_ES419: g_systemCemuLanguage=5; g_systemLanguageName="Spanish"; break;
    case SetLanguage_ZHCN: case SetLanguage_ZHHANS: g_systemCemuLanguage=6; g_systemLanguageName="Chinese"; break;
    case SetLanguage_KO: g_systemCemuLanguage=7; g_systemLanguageName="Korean"; break;
    case SetLanguage_NL: g_systemCemuLanguage=8; g_systemLanguageName="Dutch"; break;
    case SetLanguage_PT: case SetLanguage_PTBR: g_systemCemuLanguage=9; g_systemLanguageName="Portuguese"; break;
    case SetLanguage_RU: g_systemCemuLanguage=10; g_systemLanguageName="Russian"; break;
    case SetLanguage_ZHTW: case SetLanguage_ZHHANT: g_systemCemuLanguage=11; g_systemLanguageName="Taiwanese"; break;
    default: g_systemCemuLanguage=1; g_systemLanguageName="English"; break;
  }
}

static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (!o.key) continue;
      std::string v = iniGet(o.key, o.def);
      iniSet(o.key, v.c_str());
    }
}

static std::vector<CemuKV> buildEffectiveSettings(const std::string &gameKey) {
  Store perGame;
  if (!gameKey.empty()) {
    const std::string path = std::string(GAMECFG_DIR) + "/" + gameKey + ".ini";
    if (regularFileExists(path))
      storeLoad(perGame, path.c_str());
  }
  std::vector<CemuKV> out;
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (!o.key) continue;
      const char *v = nullptr;
      for (auto &e : perGame.kv)  if (e.k == o.key) { v = e.v.c_str(); break; }
      if (!v) for (auto &e : g_global.kv) if (e.k == o.key) { v = e.v.c_str(); break; }
      out.push_back({ o.key, v ? v : (o.def ? o.def : "") });
    }
  for (const auto &mapping : C_inputMappings) {
    const char *value = nullptr;
    for (auto &entry : perGame.kv) if (entry.k == mapping.key) { value = entry.v.c_str(); break; }
    if (!value) for (auto &entry : g_global.kv) if (entry.k == mapping.key) { value = entry.v.c_str(); break; }
    out.push_back({mapping.key, value ? value : mapping.def});
  }
  { const char *v = nullptr;
    for (auto &e : g_global.kv) if (e.k == "console_language") { v = e.v.c_str(); break; }
    std::string language=v?v:"-1";
    if(language=="-1") language=std::to_string(g_systemCemuLanguage);
    out.push_back({ "console_language", std::move(language) }); }
  return out;
}

static bool writeInputIni(const std::vector<CemuKV> &eff) {
  std::string text = "[controller]\n";
  text += "type = " + std::string(cemuKVGet(eff, "in_type", "GamePad")) + "\n";
  text += "players = " + std::string(cemuKVGet(eff, "in_players", "1")) + "\n";
  text += "rumble = ";
  text += (strcmp(cemuKVGet(eff, "in_rumble", "true"), "true") == 0 ? "on\n" : "off\n");
  text += "deadzone = " + std::string(cemuKVGet(eff, "in_deadzone", "15")) + "\n";
  text += "[map]\n";
  for (const auto &mapping : C_inputMappings)
    text += std::string(mapping.emu) + " = " + cemuKVGet(eff, mapping.key, mapping.def) + "\n";
  return writeAtomicText("sdmc:/switch/cemu/input.ini", text);
}

static bool appendHandoffValue(std::string &text, const char *key, const std::string &value) {
  if (value.find_first_of("\r\n") != std::string::npos)
    return false;
  text += key;
  text += '=';
  text += value;
  text += '\n';
  return true;
}

static const char *ENABLED_PACKS_FILE = "sdmc:/switch/cemu/enabled_packs.txt";
static bool readEnabledPacks(std::vector<CemuGraphicPack> &out) {
  out.clear();
  if (!recoverAtomicFile(ENABLED_PACKS_FILE)) return false;
  FILE *f = fopen(ENABLED_PACKS_FILE, "r");
  if (!f) return errno == ENOENT;
  char line[4096];
  bool ok = true;
  while (fgets(line, sizeof(line), f)) {
    if (!strchr(line, '\n') && !feof(f)) { ok = false; break; }
    std::string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty()) continue;
    CemuGraphicPack pk;
    size_t start = 0; bool first = true;
    for (;;) {
      size_t tab = s.find('\t', start);
      std::string field = s.substr(start, tab == std::string::npos ? std::string::npos : tab - start);
      if (first) {
        pk.rulesRel = (field.rfind("sdmc:", 0) == 0) ? field : (std::string(DATA_DIR) + "/" + field);
        first = false; }
      else if (!field.empty()) { size_t eq = field.find('='); if (eq != std::string::npos) pk.presets.push_back({field.substr(0, eq), field.substr(eq + 1)}); }
      if (tab == std::string::npos) break;
      start = tab + 1;
    }
    if (!pk.rulesRel.empty()) out.push_back(std::move(pk));
  }
  if (ferror(f) || fclose(f) != 0) ok = false;
  if (!ok) out.clear();
  return ok;
}

static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr;
static SDL_Texture  *g_logo = nullptr;
static int SW = 1280, SH = 720;
static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_griddbReady = false;
static bool g_storageSocketReady = false;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil = 0;

enum class LauncherTheme { Xmb, Bubbles, Glow, Classic, Oled };
static LauncherTheme g_launcherTheme = LauncherTheme::Bubbles;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static int g_gridColumns = 5;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 184 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 210 };

static void fillRect(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  int width;
  int height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "homebrew");
  g_launcherTheme = !strcmp(theme, "classic") ? LauncherTheme::Classic :
                    !strcmp(theme, "oled") ? LauncherTheme::Oled :
                    !strcmp(theme, "animated") ? LauncherTheme::Glow :
                    !strcmp(theme, "xmb") ? LauncherTheme::Xmb : LauncherTheme::Bubbles;
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "5"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));

  if (g_launcherTheme == LauncherTheme::Xmb) {
    COL_BG={2,35,92,255}; COL_TXT={246,250,255,255}; COL_DIM={176,207,233,255};
    COL_HI={151,229,255,255}; COL_VAL={255,255,255,255}; COL_SEL={116,218,255,255};
    COL_PANEL={4,28,73,164}; COL_CARD={5,36,86,196}; COL_FOCUS={20,91,148,214};
  } else if (g_launcherTheme == LauncherTheme::Classic) {
    COL_BG={22,24,30,255}; COL_TXT={228,230,235,255}; COL_DIM={150,155,165,255};
    COL_HI={96,200,255,255}; COL_VAL={255,210,100,255}; COL_SEL={255,170,0,255};
    COL_PANEL={28,31,40,255}; COL_CARD={24,26,34,255}; COL_FOCUS={66,56,30,235};
  } else if (g_launcherTheme == LauncherTheme::Oled) {
    COL_BG={0,0,0,255}; COL_TXT={245,247,249,255}; COL_DIM={145,151,158,255};
    COL_HI={105,220,255,255}; COL_VAL={255,255,255,255}; COL_SEL={0,210,190,255};
    COL_PANEL={4,4,5,248}; COL_CARD={8,8,10,250}; COL_FOCUS={0,58,53,245};
  } else if (g_launcherTheme == LauncherTheme::Bubbles) {
    COL_BG={0,8,16,255}; COL_TXT={235,248,255,255}; COL_DIM={143,192,216,255};
    COL_HI={118,222,255,255}; COL_VAL={194,239,255,255}; COL_SEL={61,183,235,255};
    COL_PANEL={4,31,50,190}; COL_CARD={5,35,56,218}; COL_FOCUS={12,76,108,220};
  } else {
    COL_BG={8,12,24,255}; COL_TXT={235,239,247,255}; COL_DIM={151,163,184,255};
    COL_HI={100,211,255,255}; COL_VAL={255,215,120,255}; COL_SEL={116,200,255,255};
    COL_PANEL={16,23,39,184}; COL_CARD={22,30,49,214}; COL_FOCUS={28,69,92,208};
  }
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() {
  return g_launcherTheme==LauncherTheme::Xmb||g_launcherTheme==LauncherTheme::Bubbles||g_launcherTheme==LauncherTheme::Glow;
}

static void drawGlow(float x,float y,float radius,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
  int diameter=(int)(SH*radius);
  SDL_Rect destination={(int)(SW*x)-diameter/2,(int)(SH*y)-diameter/2,diameter,diameter};
  SDL_SetTextureColorMod(g_glowTexture,red,green,blue);
  SDL_SetTextureAlphaMod(g_glowTexture,alpha);
  SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&destination);
}

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

static Uint8 blendChannel(Uint8 first,Uint8 second,float amount) {
  return (Uint8)(first+(second-first)*std::clamp(amount,0.f,1.f));
}

static float xmbWaveY(float x,float time,float center,float amplitude,float frequency,float slope,float phase) {
  const float primary=sinf(x*6.2831853f*frequency+phase+time*0.115f);
  const float detail=sinf(x*6.2831853f*(frequency*2.07f)+phase*0.61f-time*0.072f);
  return center+slope*(x-0.5f)+amplitude*(primary+detail*0.24f);
}

static void drawXmbRibbon(float time,float center,float amplitude,float frequency,float slope,float phase,
                          int halfWidth,SDL_Color color) {
  constexpr int pointCount=121;
  std::array<SDL_Point,pointCount> points{};
  for(int offset=-halfWidth;offset<=halfWidth;offset++){
    float distance=halfWidth?fabsf((float)offset/halfWidth):0.f;
    Uint8 alpha=(Uint8)(color.a*powf(std::max(0.f,1.f-distance),1.45f));
    if(alpha<2) continue;
    for(int point=0;point<pointCount;point++){
      float x=(float)point/(pointCount-1);
      points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)+offset};
    }
    SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,alpha);
    SDL_RenderDrawLines(g_ren,points.data(),pointCount);
  }
}

static void drawXmbFilament(float time,float center,float amplitude,float frequency,float slope,float phase,
                            SDL_Color color) {
  constexpr int pointCount=161;
  std::array<SDL_Point,pointCount> points{};
  for(int point=0;point<pointCount;point++){
    float x=(float)point/(pointCount-1);
    points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)};
  }
  SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,color.a);
  SDL_RenderDrawLines(g_ren,points.data(),pointCount);
}

static void drawXmbSparkles(float time) {
  for(int index=0;index<42;index++){
    float x=fmodf(index*0.618034f+time*(0.0022f+(index%5)*0.00045f),1.08f)-0.04f;
    float y=xmbWaveY(x,time,0.585f,0.095f,0.91f,0.075f,0.4f)+
            (fmodf(index*0.413f,1.f)-0.5f)*0.31f;
    float pulse=0.5f+0.5f*sinf(time*(0.55f+(index%7)*0.08f)+index*1.731f);
    Uint8 alpha=(Uint8)(28.f+pulse*(index%9==0?142.f:82.f));
    int px=(int)(x*SW),py=(int)(y*SH);
    fillRect(px,py,index%9==0?3:2,index%9==0?3:2,(SDL_Color){220,246,255,alpha});
    if(index%9==0&&pulse>0.55f){
      SDL_SetRenderDrawColor(g_ren,235,251,255,(Uint8)(alpha*0.62f));
      SDL_RenderDrawLine(g_ren,px-5,py+1,px+7,py+1);
      SDL_RenderDrawLine(g_ren,px+1,py-5,px+1,py+7);
    }
  }
}

static void drawXmbBackground(float time) {
  const SDL_Color top={3,37,102,255},middle={8,93,184,255},bottom={0,20,68,255};
  constexpr int bands=72;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.52f){
      float amount=y/0.52f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.52f)/0.48f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }
  if(g_glowTexture){
    drawGlow(0.10f,0.43f,1.18f,55,157,255,54);
    drawGlow(0.84f,0.38f,0.92f,41,112,228,42);
  }
  drawXmbRibbon(time,0.655f,0.082f,0.78f,-0.105f,2.15f,std::max(12,SH/18),(SDL_Color){63,166,255,31});
  drawXmbRibbon(time,0.575f,0.074f,0.96f,0.080f,0.35f,std::max(10,SH/25),(SDL_Color){189,235,255,48});
  drawXmbRibbon(time,0.605f,0.049f,1.28f,-0.025f,3.82f,std::max(5,SH/54),(SDL_Color){230,250,255,72});
  for(int trace=0;trace<9;trace++){
    float offset=(trace-4)*0.009f;
    drawXmbFilament(time,0.588f+offset,0.083f+trace*0.0017f,0.91f,0.052f,
                    0.62f+trace*0.19f,(SDL_Color){202,241,255,(Uint8)(18+trace%3*8)});
  }
  drawXmbFilament(time,0.578f,0.073f,0.96f,0.080f,0.35f,(SDL_Color){243,253,255,136});
  drawXmbSparkles(time);
}

static void drawBubble(int centerX,int centerY,int radius,Uint8 alpha) {
  if(radius<3||alpha==0) return;
  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,90,205,255);
    SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(alpha/5));
    SDL_Rect glow={centerX-radius*2,centerY-radius*2,radius*4,radius*4};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&glow);
  }
  const int segments=24;
  SDL_SetRenderDrawColor(g_ren,124,220,255,alpha);
  std::array<SDL_Point,segments+1> outer{},inner{};
  for(int segment=0;segment<=segments;segment++){
    float angle=segment*6.2831853f/segments;
    float x=cosf(angle),y=sinf(angle);
    outer[segment]={centerX+(int)(x*radius),centerY+(int)(y*radius)};
    inner[segment]={centerX+(int)(x*(radius-1)),centerY+(int)(y*(radius-1))};
  }
  SDL_RenderDrawLines(g_ren,outer.data(),(int)outer.size());
  SDL_RenderDrawLines(g_ren,inner.data(),(int)inner.size());
  SDL_SetRenderDrawColor(g_ren,235,252,255,(Uint8)std::min(255,(int)alpha+55));
  std::array<SDL_Point,6> highlight{};
  for(int segment=0;segment<(int)highlight.size();segment++){
    float angle=3.55f+segment*0.13f;
    highlight[segment]={centerX+(int)(cosf(angle)*radius),centerY+(int)(sinf(angle)*radius)};
  }
  SDL_RenderDrawLines(g_ren,highlight.data(),(int)highlight.size());
}

static void drawBubblesBackground(float time) {
  const SDL_Color top={20,126,169,255},middle={4,54,82,255},bottom={0,5,11,255};
  constexpr int bands=56;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.58f){
      float amount=y/0.58f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.58f)/0.42f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }

  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,118,225,255);
    SDL_SetTextureAlphaMod(g_glowTexture,105);
    SDL_Rect surface={-SW/6,-SH/3,SW*4/3,SH*2/3};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&surface);
    for(int ray=0;ray<7;ray++){
      float sway=sinf(time*(0.10f+ray*0.013f)+ray*1.31f);
      int width=SW*(11+(ray%3)*3)/100;
      int x=SW*(8+ray*14)/100+(int)(sway*SW*0.025f)-width/2;
      SDL_Rect shaft={x,-SH/3,width,SH*4/3};
      SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(23+(ray%3)*7));
      SDL_RenderCopyEx(g_ren,g_glowTexture,nullptr,&shaft,-9.0+ray*2.7+sway*2.0,nullptr,SDL_FLIP_NONE);
    }
  }

  for(int index=0;index<18;index++){
    float progress=fmodf(index*0.173f+time*(0.038f+(index%5)*0.007f),1.18f);
    float y=1.08f-progress;
    float x=0.05f+fmodf(index*0.283f,0.90f)+0.032f*sinf(time*(0.31f+(index%4)*0.04f)+index);
    float fade=std::min(std::clamp((1.10f-y)*5.f,0.f,1.f),std::clamp((y+0.12f)*6.f,0.f,1.f));
    int radius=(int)(SH*(0.009f+(index%6)*0.0042f));
    if(index%11==0) radius=radius*3/2;
    drawBubble((int)(x*SW),(int)(y*SH),radius,(Uint8)(fade*(85+(index%4)*24)));
  }
  drawBackgroundParticles(time,(SDL_Color){164,228,255,62},24,0.008f);
}

static void clearUiBackground() {
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(!hasAnimatedBackground()) return;
  ensureGlowTexture();
  float time=g_uiAnimations?SDL_GetTicks()/1000.f:0.f;
  if(g_launcherTheme==LauncherTheme::Xmb){
    drawXmbBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(g_launcherTheme==LauncherTheme::Bubbles){
    drawBubblesBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(!g_glowTexture) return;
  drawGlow(0.10f+0.13f*sinf(time*0.43f),0.20f+0.11f*cosf(time*0.37f),0.90f,45,140,255,128);
  drawGlow(0.84f+0.12f*cosf(time*0.34f),0.34f+0.10f*sinf(time*0.41f),0.78f,154,75,255,112);
  drawGlow(0.54f+0.10f*sinf(time*0.29f),0.91f+0.06f*cosf(time*0.33f),0.94f,0,210,190,94);
  drawGlow(0.42f+0.08f*cosf(time*0.25f),0.48f+0.09f*sinf(time*0.31f),0.58f,64,125,255,67);
  drawBackgroundParticles(time,(SDL_Color){182,224,255,88},28,0.011f);
  SDL_SetTextureColorMod(g_glowTexture,255,255,255);
  SDL_SetTextureAlphaMod(g_glowTexture,255);
}

static void glassPanel(int x,int y,int width,int height) {
  fillRect(x,y,width,height,COL_PANEL);
  border(x,y,width,height,1,(SDL_Color){255,255,255,(Uint8)(hasAnimatedBackground()?28:16)});
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_Rect d={x,y,found->second.width,found->second.height};
    SDL_RenderCopy(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  int w=sf->w,h=sf->h; SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,w);
  const size_t bytes=(size_t)w*(size_t)h*4;
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void toast(const char *msg);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static void gfxPackScreen(uint64_t filterTitleId);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static void runUpdateScreen();
static std::string installedReleaseTag();
static void pollUpdateNotification();
static void drawUpdateNotification();
static int dropdown(const char *title, const char *const *labels, int n, int cur);
static void ensureDefaultGameSource();
static void beginScreenFx();
static void drawFadeIn();
static int topBarH();
static void drawHeader(const char *title,const char *ctx);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height);
static bool g_rescanAfterSettings = false;

static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr };
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gL=nullptr,*g_gR=nullptr;
// Supersampling keeps the downscaled glyphs crisp.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;
  const int S=GLYPH_SS, base=TTF_FontHeight(g_font_sm)+6;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  SDL_Color edge={14,16,22,255}, hi={92,99,114,255}, face={52,57,68,255}, ink={246,248,252,255};
  if(pill){
    int r=H/2;
    fillCircle(r,r,r,edge);     fillCircle(W-r,r,r,edge);     fillRect(r,0,W-2*r,H,edge);
    fillCircle(r,r,r-S,hi);     fillCircle(W-r,r,r-S,hi);     fillRect(r,S,W-2*r,H-2*S,hi);
    fillCircle(r,r,r-S*2,face); fillCircle(W-r,r,r-S*2,face); fillRect(r,S*2,W-2*r,H-S*4,face);
  } else {
    int R=H/2;
    fillCircle(W/2,H/2,R,edge);
    fillCircle(W/2,H/2,R-S,hi);
    fillCircle(W/2,H/2,R-S*2,face);
  }
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,nullptr);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gL=makeGlyph("L",true); g_gR=makeGlyph("R",true);
}

enum FootAct { FA_NONE, FA_LAUNCH, FA_SORT, FA_OPTIONS, FA_SETTINGS, FA_PAGEL, FA_PAGER, FA_QUIT };
struct FootItem { SDL_Texture *glyph; const char *label; int act; };
static SDL_Rect g_footHit[10]; static int g_footAct[10]; static int g_footN=0;
static void drawFooterHints(const FootItem *it,int n,int cy){
  const int gap=8, pairGap=26, glyphGap=16, fh=TTF_FontHeight(g_font_sm);
  int total=0;
  for(int i=0;i<n;i++){ int gw=0; if(it[i].glyph) SDL_QueryTexture(it[i].glyph,0,0,&gw,0); gw/=GLYPH_SS;
    total+=gw; bool L=it[i].label&&it[i].label[0];
    if(L) total+=gap+textW(g_font_sm,it[i].label);
    if(i<n-1) total+=(L?pairGap:glyphGap); }
  int x=(SW-total)/2; g_footN=0;
  for(int i=0;i<n;i++){ int gw=0,gh=0; if(it[i].glyph) SDL_QueryTexture(it[i].glyph,0,0,&gw,&gh); gw/=GLYPH_SS; gh/=GLYPH_SS;
    int x0=x;
    if(it[i].glyph){ SDL_Rect d={x,cy-gh/2,gw,gh}; SDL_RenderCopy(g_ren,it[i].glyph,nullptr,&d); }
    x+=gw; bool L=it[i].label&&it[i].label[0];
    if(L){ x+=gap; drawText(g_font_sm,x,cy-fh/2,it[i].label,COL_DIM); x+=textW(g_font_sm,it[i].label); }
    if(g_footN<10){ g_footHit[g_footN]={x0-6,cy-gh/2-8,(x-x0)+12,gh+16}; g_footAct[g_footN]=it[i].act; g_footN++; }
    if(i<n-1) x+=(L?pairGap:glyphGap);
  }
}
static int footTapAct(int px,int py){
  for(int i=0;i<g_footN;i++){ SDL_Rect &r=g_footHit[i];
    if(px>=r.x && px<r.x+r.w && py>=r.y && py<r.y+r.h) return g_footAct[i]; }
  return FA_NONE;
}

static SDL_Texture *glyphForButton(const char *button){
  if(!button) return nullptr;
  if(strcmp(button,"A")==0) return g_gA;
  if(strcmp(button,"B")==0) return g_gB;
  if(strcmp(button,"X")==0) return g_gX;
  if(strcmp(button,"Y")==0) return g_gY;
  if(strcmp(button,"+")==0) return g_gPlus;
  if(strcmp(button,"L")==0) return g_gL;
  if(strcmp(button,"R")==0) return g_gR;
  return nullptr;
}

static void buttonHintSize(const char *button,int &width,int &height){
  SDL_Texture *glyph=glyphForButton(button);
  width=height=0;
  if(glyph){
    SDL_QueryTexture(glyph,nullptr,nullptr,&width,&height);
    width/=GLYPH_SS; height/=GLYPH_SS;
  } else {
    width=textW(g_font_sm,button?button:"")+14;
    height=TTF_FontHeight(g_font_sm)+6;
  }
}

static int drawButtonHint(int x,int cy,const char *button,const char *label){
  int width=0,height=0; buttonHintSize(button,width,height);
  SDL_Texture *glyph=glyphForButton(button);
  if(glyph){
    SDL_Rect destination={x,cy-height/2,width,height};
    SDL_RenderCopy(g_ren,glyph,nullptr,&destination);
  } else {
    border(x,cy-height/2,width,height,1,COL_DIM);
    drawTextC(g_font_sm,x+width/2,cy-TTF_FontHeight(g_font_sm)/2,button?button:"",COL_TXT);
  }
  if(label&&label[0])
    drawText(g_font_sm,x+width+8,cy-TTF_FontHeight(g_font_sm)/2,label,COL_DIM);
  return width+((label&&label[0])?8+textW(g_font_sm,label):0);
}

static void drawSettingsFooter(const char *text,int centerY=-1){
  if(!text||!text[0]) return;
  std::vector<std::string> tokens;
  const size_t length=strlen(text);
  size_t cursor=0;
  while(cursor<length){
    while(cursor<length&&text[cursor]==' ') cursor++;
    if(cursor>=length) break;
    const size_t start=cursor;
    while(cursor<length){
      if(cursor+1<length&&text[cursor]==' '&&text[cursor+1]==' ') break;
      cursor++;
    }
    tokens.emplace_back(trim(std::string(text+start,cursor-start)));
    while(cursor<length&&text[cursor]==' ') cursor++;
  }
  const int cy=centerY>=0?centerY:SH-26;
  if(tokens.size()<2||(tokens.size()&1)){
    drawTextC(g_font_sm,SW/2,cy-TTF_FontHeight(g_font_sm)/2,text,COL_DIM);
    return;
  }
  const int pairGap=26;
  int total=0;
  for(size_t i=0;i<tokens.size();i+=2){
    int width=0,height=0; buttonHintSize(tokens[i].c_str(),width,height);
    total+=width+8+textW(g_font_sm,tokens[i+1].c_str());
    if(i+2<tokens.size()) total+=pairGap;
  }
  int x=(SW-total)/2;
  for(size_t i=0;i<tokens.size();i+=2){
    x+=drawButtonHint(x,cy,tokens[i].c_str(),tokens[i+1].c_str());
    if(i+2<tokens.size()) x+=pairGap;
  }
}

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    g_touch.x0=e.tfinger.x*SW; g_touch.y0=e.tfinger.y*SH; g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=e.tfinger.x*SW, uy=e.tfinger.y*SH, dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS) return TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  return true;
}

static bool pollUiEvent(SDL_Event &event) {
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    return true;
  }
  return false;
}

static void navRepeat(){
  if(!g_pad) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

struct Game {
  std::string path;
  std::string file;
  std::string title;
  std::string key;
  uint64_t titleId = 0;
  std::string titleIdError;
  std::string iconPath;
  SDL_Texture *cover = nullptr;
  Uint32 coverAt = 0;
  Uint64 coverUse = 0;
  bool triedCover = false;
  bool hasCfg = false;
  int region = 0;
  long long added = 0;
  long long played = 0;
};
static std::vector<Game> g_games;
static Uint64 g_coverUseSerial = 0;
static constexpr size_t COVER_CACHE_LIMIT = 28;

enum { SORT_ALPHA, SORT_RECENT, SORT_ADDED, SORT_COUNT };
static const char *SORT_NAME[SORT_COUNT] = { "A-Z", "Recently played", "Recently added" };
static int g_sort = SORT_ALPHA;
static Store g_recent;
static const char *RECENT_INI = "sdmc:/switch/cemu/recent.ini";

static int detectRegion(const std::string &file) {
  std::string tags; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("(j)")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("(u)")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("(uk")||has("france")||
      has("germany")||has("spain")||has("ital")||has("(e)")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void applySort() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    if (g_sort == SORT_RECENT && a.played != b.played) return a.played > b.played;
    if (g_sort == SORT_ADDED  && a.added  != b.added)  return a.added  > b.added;
    return cmpTitle(a, b);
  });
}
static void recordPlayed(const Game &game){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b);
  storeSet(g_recent,game.key.c_str(),b);
}

static bool hasGameExt(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".wua",".wud",".wux",".wuhb",".rpx",".elf",".iso" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }
static std::string foldedKey(std::string key);

static std::string normalizeLocationPath(const std::string &input) {
  std::string path=trim(input);
  if(path.empty()) return {};
  std::string output;
  output.reserve(path.size()+1);
  bool slash=false;
  for(char c:path){
    if(c=='\\') c='/';
    if(c=='/'){
      if(slash) continue;
      slash=true;
    } else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos && colon+1==output.size()) output+='/';
  size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum && output.back()=='/') output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &input) {
  return foldedKey(normalizeLocationPath(input));
}

static std::vector<std::string> loadGameSources() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","1"))));
  for(int i=0;i<count;i++){
    std::string key="Wrapper/GamePath"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),i==0?DEF_GAMEDIR:""));
    if(!path.empty()) paths.push_back(std::move(path));
  }
  std::unordered_set<std::string> seen;
  paths.erase(std::remove_if(paths.begin(),paths.end(),[&](const std::string &path){
    return !seen.insert(pathIdentity(path)).second;
  }),paths.end());
  return paths;
}

static void saveGameSources(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty() && seen.insert(pathIdentity(path)).second && paths.size()<16) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Wrapper/GamePath");
  storeSet(g_global,"Wrapper/GamePathCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Wrapper/GamePath"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
}

static std::vector<std::string> loadFavoriteFolders() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  std::unordered_set<std::string> seen;
  for(int i=0;i<count;i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty() && seen.insert(pathIdentity(path)).second) paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

static std::vector<SwitchStorage::SmbShare> loadSmbSharesFromStore() {
  std::vector<SwitchStorage::SmbShare> shares;
  std::unordered_set<std::string> ids;
  int count=std::max(0,std::min(8,atoi(storeGet(g_global,"Storage/SmbCount","0"))));
  for(int i=0;i<count;i++){
    std::string prefix="Storage/Smb"+std::to_string(i);
    SwitchStorage::SmbShare share;
    share.id=storeGet(g_global,(prefix+"Id").c_str(),"");
    share.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    share.server=storeGet(g_global,(prefix+"Server").c_str(),"");
    share.share=storeGet(g_global,(prefix+"Share").c_str(),"");
    share.path=storeGet(g_global,(prefix+"Path").c_str(),"");
    share.user=storeGet(g_global,(prefix+"User").c_str(),"");
    share.password=storeGet(g_global,(prefix+"Password").c_str(),"");
    share.domain=storeGet(g_global,(prefix+"Domain").c_str(),"");
    const char *automatic=storeGet(g_global,(prefix+"AutoMount").c_str(),"true");
    share.autoMount=!strcmp(automatic,"true")||!strcmp(automatic,"1");
    if(!SwitchStorage::SmbRootPath(share.id).empty()&&!share.server.empty()&&!share.share.empty()&&ids.insert(share.id).second)
      shares.push_back(std::move(share));
  }
  return shares;
}

static void saveSmbShares(const std::vector<SwitchStorage::SmbShare> &shares) {
  storeRemovePrefix(g_global,"Storage/Smb");
  storeSet(g_global,"Storage/SmbCount",std::to_string(shares.size()).c_str());
  for(size_t i=0;i<shares.size();i++){
    const auto &share=shares[i]; std::string prefix="Storage/Smb"+std::to_string(i);
    storeSet(g_global,(prefix+"Id").c_str(),share.id.c_str());
    storeSet(g_global,(prefix+"Name").c_str(),share.name.c_str());
    storeSet(g_global,(prefix+"Server").c_str(),share.server.c_str());
    storeSet(g_global,(prefix+"Share").c_str(),share.share.c_str());
    storeSet(g_global,(prefix+"Path").c_str(),share.path.c_str());
    storeSet(g_global,(prefix+"User").c_str(),share.user.c_str());
    storeSet(g_global,(prefix+"Password").c_str(),share.password.c_str());
    storeSet(g_global,(prefix+"Domain").c_str(),share.domain.c_str());
    storeSet(g_global,(prefix+"AutoMount").c_str(),share.autoMount?"true":"false");
  }
  storeSave(g_global,LAUNCHER_INI);
}

static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","nkit","redump","proper","unl","disc","cd","dvd",
    "iso","chd","cso","zso","enfrespt",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true;
  return false;
}
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c;
  }
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  std::string filtered;
  for(size_t start=0;start<o.size();){
    size_t end=o.find(' ',start);
    std::string token=o.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(foldedKey(token)!="enfrespt"){
      if(!filtered.empty()) filtered+=' ';
      filtered+=token;
    }
    if(end==std::string::npos) break;
    start=end+1;
  }
  o=std::move(filtered);
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}

static std::string foldedKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return key;
}

static std::string makeGameKey(const std::string &file, const std::string &path) {
  std::string base = sanitize(file);
  if (base.empty()) base = "game";
  if (base.size() > 80) base.resize(80);

  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : cemu_normalizeTitlePath(path)) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  return base + suffix;
}

static const char *gameStoreGet(Store &store, const Game &game, const char *def) {
  return storeGet(store, game.key.c_str(), def);
}

static bool gameFileExists(const char *dir, const Game &game, const char *extension) {
  return regularFileExists(std::string(dir) + "/" + game.key + extension);
}

static void scanGames(const std::vector<std::string> &sourcePaths) {
  for (auto &g : g_games) if (g.cover) SDL_DestroyTexture(g.cover);
  g_games.clear();
  g_coverUseSerial = 0;
  auto titleCache = cemu_loadTitleCache(std::string(DATA_DIR) + "/title_list_cache.xml");
  Store refreshedContainerTitles;
  std::unordered_set<std::string> seenPaths;
  for(const auto &source:sourcePaths){
    DIR *d=opendir(source.c_str());
    if(!d) continue;
    struct dirent *e;
    while((e=readdir(d))){
      if(e->d_name[0]=='.') continue;
      std::string full=join(source,e->d_name);
      struct stat sst{};
      bool isDir=stat(full.c_str(),&sst)==0&&S_ISDIR(sst.st_mode);
      bool isGameDir=isDir&&stat((full+"/code").c_str(),&sst)==0;
      if(!isGameDir&&(isDir||!hasGameExt(e->d_name))) continue;
      if(!seenPaths.insert(pathIdentity(full)).second) continue;
      Game g;
      g.file=e->d_name;
      g.path=full;
      g.key=makeGameKey(e->d_name,full);
      bool haveStat=stat(full.c_str(),&sst)==0;
      if(haveStat) g.added=(long long)sst.st_mtime;
      if(!isDir&&haveStat){
        const char *cached=storeGet(g_containerTitles,g.key.c_str(),"");
        long long cachedSize=0,cachedTime=0; unsigned long long cachedId=0; int consumed=0;
        if(sscanf(cached,"%lld,%lld,%llx%n",&cachedSize,&cachedTime,&cachedId,&consumed)==3&&cached[consumed]==0&&
           cachedSize==(long long)sst.st_size&&cachedTime==(long long)sst.st_mtime&&(cachedId>>48)==0x0005)
          g.titleId=(uint64_t)cachedId;
      }
      if(!g.titleId) g.titleId=cemu_resolveBaseTitleId(full,titleCache,&g.titleIdError);
      if(!isDir&&haveStat&&g.titleId){
        char cached[96]; snprintf(cached,sizeof(cached),"%lld,%lld,%016llx",(long long)sst.st_size,(long long)sst.st_mtime,(unsigned long long)g.titleId);
        storeSet(refreshedContainerTitles,g.key.c_str(),cached);
      }
      g_games.push_back(std::move(g));
    }
    closedir(d);
  }

  for (auto &game : g_games) {
    const char *customTitle = gameStoreGet(g_titles, game, "");
    game.title = *customTitle ? customTitle : cleanTitle(game.file);
    game.region = detectRegion(game.file);
    game.played = atoll(gameStoreGet(g_recent, game, "0"));
    game.hasCfg = gameFileExists(GAMECFG_DIR, game, ".ini");
  }

  for (auto &t : cemu_scanMlcTitles(std::string(DATA_DIR) + "/mlc01")) {
    Game g;
    g.titleId = t.titleId;
    g.iconPath = t.iconPath;
    char idkey[24]; snprintf(idkey, sizeof(idkey), "%016llx", (unsigned long long)t.titleId);
    g.key = idkey;
    g.file = idkey;
    const char *ct = storeGet(g_titles, g.key.c_str(), "");
    g.title = ct[0] ? std::string(ct) : (t.name.empty() ? std::string(idkey) : t.name);
    g.region = 0;
    g.played = atoll(storeGet(g_recent, g.key.c_str(), "0"));
    struct stat st;
    g.hasCfg = stat((std::string(GAMEPROFILES_DIR) + "/" + g.key + ".ini").c_str(), &st) == 0;
    g_games.push_back(std::move(g));
  }
  bool cacheChanged=g_containerTitles.kv.size()!=refreshedContainerTitles.kv.size();
  if(!cacheChanged) for(const auto &entry:refreshedContainerTitles.kv)
    if(strcmp(storeGet(g_containerTitles,entry.k.c_str(),""),entry.v.c_str())!=0){ cacheChanged=true; break; }
  g_containerTitles=std::move(refreshedContainerTitles);
  if(cacheChanged) storeSave(g_containerTitles,CONTAINER_TITLES_INI);
  applySort();
}
static std::string coverPath(const Game &g) { return std::string(COVERS_DIR) + "/" + g.key + ".png"; }
static std::string existingCoverPath(const Game &g) {
  return coverPath(g);
}

static Game *findGameByKey(const std::string &key) {
  for (auto &game : g_games)
    if (game.key == key) return &game;
  return nullptr;
}

static constexpr int COVER_DECODE_BUDGET = 3;
static int g_cover_budget = 1 << 30;

// Limit decoded width; source images can be much larger than their grid cells.
static SDL_Texture *loadCoverTexture(const std::string &path) {
  SDL_Surface *s = IMG_Load(path.c_str());
  if (!s) return nullptr;
  const int MAXW = 360, MAXH = 540;
  int width=s->w,height=s->h;
  if(width>MAXW){ height=(int)((long long)height*MAXW/width); width=MAXW; }
  if(height>MAXH){ width=(int)((long long)width*MAXH/height); height=MAXH; }
  if(width<1) width=1;
  if(height<1) height=1;
  if (width != s->w || height != s->h) {
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!d) { SDL_FreeSurface(s); return nullptr; }
    SDL_BlendMode blend=SDL_BLENDMODE_NONE;
    SDL_GetSurfaceBlendMode(s,&blend);
    SDL_SetSurfaceBlendMode(s,SDL_BLENDMODE_NONE);
    const bool scaled=SDL_BlitScaled(s,nullptr,d,nullptr)==0;
    SDL_SetSurfaceBlendMode(s,blend);
    SDL_FreeSurface(s);
    if(!scaled){ SDL_FreeSurface(d); return nullptr; }
    s=d;
  }
  SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, s);
  SDL_FreeSurface(s);
  if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
  return t;
}
static SDL_Texture *loadArt(const Game &g) {
  SDL_Texture *t = loadCoverTexture(existingCoverPath(g));
  if (!t && !g.iconPath.empty()) t = loadCoverTexture(g.iconPath);
  return t;
}

static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static void ensureCover(Game &g) {
  if (g.cover) { touchCover(g); return; }
  if (g.triedCover) return;
  if (g_cover_budget <= 0) return;
  g_cover_budget--;
  g.triedCover = true;
  installCover(g, loadArt(g));
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverUse = 0;
  g.triedCover = true;
  installCover(g, loadArt(g));
}

static bool promptTextMode(const char *header, const char *initial, char *out, size_t outSize,
                           bool password, bool allowEmpty,
                           const char *subText=nullptr, const char *guideText=nullptr) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd); else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (subText) swkbdConfigSetSubText(&kbd, subText);
  if (guideText) swkbdConfigSetGuideText(&kbd, guideText);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextMode(header,initial,out,outSize,false,false);
}

static bool isTitleFile(const char *name);
static bool folderIsExtractedTitle(const std::string &dir);

struct FileClipboard {
  std::string path;
  bool move=false;
};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t colon=normalized.find(':');
  if(colon==std::string::npos) return normalized=="/";
  for(size_t i=colon+1;i<normalized.size();i++) if(normalized[i]!='/') return false;
  return true;
}

static std::string parentFolder(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  if(filesystemRoot(normalized)) return {};
  size_t slash=normalized.find_last_of('/');
  if(slash==std::string::npos) return {};
  size_t colon=normalized.find(':');
  if(colon!=std::string::npos && slash<=colon+1) return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}

static std::string fileNameOf(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}

static std::string deviceOf(const std::string &path) {
  size_t colon=path.find(':');
  return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}

static bool pathAtOrBelow(const std::string &path,const std::string &root) {
  std::string candidate=pathIdentity(path), base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0) return false;
  if(candidate.size()==base.size()) return true;
  return base.back()=='/'||candidate[base.size()]=='/';
}

static std::string gameLocationLabel(const Game &game) {
  const std::string path=normalizeLocationPath(game.path);
  if(path.empty()) return "Installed title";
  for(const auto &share:loadSmbSharesFromStore()){
    const std::string root=normalizeLocationPath(SwitchStorage::SmbRootPath(share.id));
    if(!pathAtOrBelow(path,root)) continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    std::string address="SMB: smb://"+share.server+"/"+share.share;
    if(!relative.empty()) address+="/"+relative;
    return address;
  }
  if(path.rfind("sdmc:",0)==0) return "SD: "+path;
  if(path.rfind("ums",0)==0) return "USB: "+path;
  return path;
}

static void replaceSavedPathPrefix(const std::string &oldPath,const std::string &newPath) {
  const std::string normalizedOld=normalizeLocationPath(oldPath);
  const std::string normalizedNew=normalizeLocationPath(newPath);
  const std::string oldIdentity=pathIdentity(normalizedOld);
  auto replace=[&](std::vector<std::string> &paths){
    for(auto &path:paths){
      const std::string normalizedPath=normalizeLocationPath(path);
      const std::string identity=pathIdentity(normalizedPath);
      if(identity==oldIdentity) path=normalizedNew;
      else if(identity.size()>oldIdentity.size() && identity.compare(0,oldIdentity.size(),oldIdentity)==0 && identity[oldIdentity.size()]=='/')
        path=normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
    }
  };
  auto sources=loadGameSources(); replace(sources); saveGameSources(sources);
  auto favorites=loadFavoriteFolders(); replace(favorites); saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty() && pathAtOrBelow(g_fileClipboard.path,normalizedOld)){
    const std::string clipboardPath=normalizeLocationPath(g_fileClipboard.path);
    g_fileClipboard.path=normalizeLocationPath(normalizedNew+clipboardPath.substr(normalizedOld.size()));
  }
  g_rescanAfterSettings=true;
}

static void removeSavedPathsBelow(const std::string &root) {
  auto sources=loadGameSources();
  sources.erase(std::remove_if(sources.begin(),sources.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),sources.end());
  saveGameSources(sources);
  auto favorites=loadFavoriteFolders();
  favorites.erase(std::remove_if(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),favorites.end());
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,root)) g_fileClipboard={};
  g_rescanAfterSettings=true;
}

static bool validEntryName(const std::string &name) {
  if(name.empty()||name=="."||name==".."||name.size()>255) return false;
  for(unsigned char c:name) if(c<' '||c=='/'||c=='\\'||c==':') return false;
  return true;
}

static bool removeTreeInternal(const std::string &path) {
  if(filesystemRoot(path)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0) return errno==ENOENT;
  if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) return remove(path.c_str())==0;
  if(!S_ISDIR(st.st_mode)) return false;
  DIR *dir=opendir(path.c_str()); if(!dir) return false;
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=removeTreeInternal(join(path,entry->d_name));
  }
  if(closedir(dir)!=0) ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState {
  uint64_t total=0;
  std::atomic<uint64_t> done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex detailMutex;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  if(!current.empty()) state.current=current;
  if(!error.empty()) state.error=error;
}

static std::string transferError(TransferState &state) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  return state.error;
}

static bool transferFrame(TransferState &state) {
  if(!beginUiFrame()){ state.cancelled.store(true); return false; }
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);
    int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-100) state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) state.cancelled.store(true);
  }
  std::string current;
  { std::lock_guard<std::mutex> lock(state.detailMutex); current=state.current; }
  clearUiBackground();
  drawTextC(g_font_big,SW/2,80,"File transfer",COL_HI);
  drawTextC(g_font_sm,SW/2,150,ellipsizedText(g_font_sm,current,SW-180).c_str(),COL_DIM);
  int bw=SW*2/3,bx=(SW-bw)/2,by=SH/2-24,bh=42;
  border(bx,by,bw,bh,2,COL_SEL);
  uint64_t done=state.done.load(std::memory_order_relaxed);
  uint64_t progress=state.total?std::min(done,state.total):0;
  int fill=state.total?(int)((bw-6)*progress/state.total):0;
  fillRect(bx+3,by+3,fill,bh-6,COL_HI);
  char text[96];
  int percent=state.total?(int)(progress*100/state.total):0;
  snprintf(text,sizeof(text),"%d%%  -  %.1f / %.1f MiB",percent,done/1048576.0,state.total/1048576.0);
  drawTextC(g_font,SW/2,by+66,text,COL_TXT);
  if(state.cancelled.load())
    drawTextC(g_font_sm,SW/2,SH-72,"Cancelling...",COL_VAL);
  else
    drawSettingsFooter("B  Cancel",SH-60);
  SDL_RenderPresent(g_ren);
  return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state) {
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)){ state.total+=(uint64_t)st.st_size; return true; }
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  DIR *dir=opendir(path.c_str()); if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=measureTree(join(path,entry->d_name),state);
  }
  if(closedir(dir)!=0) ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state) {
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".cemu-part", backup=destination+".cemu-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){ setTransferDetail(state,{},"Could not open the source file"); return false; }
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){ fclose(input); setTransferDetail(state,{},"Could not create the destination file"); return false; }
  bool ok=true;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)){
    size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){ setTransferDetail(state,{},"Write failed; check free space and permissions"); ok=false; break; }
      state.done.fetch_add(count,std::memory_order_relaxed);
    }
    if(count<state.buffer.size()){
      if(ferror(input)){ setTransferDetail(state,{},"Read failed"); ok=false; }
      break;
    }
  }
  if(state.cancelled.load()) ok=false;
  if(ok&&fflush(output)!=0){ setTransferDetail(state,{},"Could not flush the destination file"); ok=false; }
  if(ok&&fsync(fileno(output))!=0){ setTransferDetail(state,{},"Could not commit the destination file"); ok=false; }
  if(fclose(input)!=0&&ok){ setTransferDetail(state,{},"Could not close the source file"); ok=false; }
  if(fclose(output)!=0&&ok){ setTransferDetail(state,{},"Could not close the destination file"); ok=false; }
  if(!ok||state.cancelled.load()){ remove(partial.c_str()); return false; }
  struct stat destinationStat{}; bool existed=stat(destination.c_str(),&destinationStat)==0;
  if(existed){
    struct stat backupStat{};
    if(lstat(backup.c_str(),&backupStat)==0){ setTransferDetail(state,{},"A previous backup file blocks this operation"); remove(partial.c_str()); return false; }
    if(rename(destination.c_str(),backup.c_str())!=0){ setTransferDetail(state,{},"Could not preserve the existing destination"); remove(partial.c_str()); return false; }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed) rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file"); remove(partial.c_str()); return false;
  }
  if(existed) remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state) {
  struct stat st{};
  if(lstat(source.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)) return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  if(mkdir(destination.c_str(),0777)!=0){ setTransferDetail(state,{},"Could not create a destination folder"); return false; }
  DIR *dir=opendir(source.c_str());
  if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=copyTree(join(source,entry->d_name),join(destination,entry->d_name),state);
  }
  if(closedir(dir)!=0&&ok){ setTransferDetail(state,{},"Could not close a source folder"); ok=false; }
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,uint64_t bytes) {
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize) return true;
  return bytes<=static_cast<uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder) {
  if(g_fileClipboard.path.empty()) return false;
  struct stat sourceStat{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceStat)!=0){ modalMessage("Paste failed",{"The copied item is no longer available."}); g_fileClipboard={}; return false; }
  const std::string destination=join(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path) ||
     (S_ISDIR(sourceStat.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessage("Paste failed",{"The destination cannot be inside the source."}); return false;
  }
  struct stat destinationStat{}; bool destinationExists=lstat(destination.c_str(),&destinationStat)==0;
  if(destinationExists&&S_ISDIR(sourceStat.st_mode)){
    modalMessage("Folder already exists",{"Choose another destination or rename the folder first.",destination}); return false;
  }
  if(destinationExists&&!S_ISREG(destinationStat.st_mode)){
    modalMessage("Paste failed",{"The destination is not a regular file."}); return false;
  }
  if(destinationExists&&!confirmBox("Replace existing file?",{fileNameOf(destination),"","The existing file will be replaced."})) return false;

  bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".cemu-old";
    bool preserved=false;
    if(destinationExists){
      struct stat backupStat{};
      if(lstat(backup.c_str(),&backupStat)==0||rename(destination.c_str(),backup.c_str())!=0){ modalMessage("Move failed",{"Could not preserve the existing destination."}); return false; }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved) remove(backup.c_str());
      replaceSavedPathPrefix(g_fileClipboard.path,destination);
      g_fileClipboard={}; toast("Move complete"); SDL_Delay(700); return true;
    }
    if(preserved) rename(backup.c_str(),destination.c_str());
  }

  TransferState state;
  if(!measureTree(g_fileClipboard.path,state)){ modalMessage("Paste failed",{transferError(state)}); return false; }
  if(!enoughFreeSpace(folder,state.total)){ modalMessage("Not enough free space",{"The destination does not have enough available space."}); return false; }
  setTransferDetail(state,fileNameOf(g_fileClipboard.path));
  bool ok=false;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&](){
    ok=copyTree(g_fileClipboard.path,destination,state);
    complete.store(true,std::memory_order_release);
  });
  while(!complete.load(std::memory_order_acquire)){
    transferFrame(state);
    SDL_Delay(8);
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  if(!ok&&S_ISDIR(sourceStat.st_mode)) removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path)) replaceSavedPathPrefix(g_fileClipboard.path,destination);
    else { modalMessage("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."}); ok=false; }
  }
  if(ok){ g_rescanAfterSettings=true; if(g_fileClipboard.move) g_fileClipboard={}; toast("Transfer complete"); SDL_Delay(700); }
  else if(state.cancelled.load()){ toast("Transfer cancelled"); SDL_Delay(700); }
  else { std::string error=transferError(state); modalMessage("Transfer failed",{error.empty()?"The file transfer could not be completed.":error}); }
  return ok;
}

static bool editSmbShare(SwitchStorage::SmbShare &share,bool creating) {
  SwitchStorage::SmbShare edited=share;
  constexpr int fieldCount=7,saveRow=7,totalRows=8;
  int sel=0;
  bool done=false,saved=false;
  beginScreenFx();

  auto cleanServer=[&](){
    edited.server=trim(edited.server);
    if(edited.server.rfind("smb://",0)==0) edited.server.erase(0,6);
    while(!edited.server.empty()&&edited.server.back()=='/') edited.server.pop_back();
  };
  auto cleanShare=[&](){
    std::string combined=trim(edited.share);
    if(!edited.path.empty()) combined+="/"+edited.path;
    std::replace(combined.begin(),combined.end(),'\\','/');
    while(!combined.empty()&&combined.front()=='/') combined.erase(combined.begin());
    while(!combined.empty()&&combined.back()=='/') combined.pop_back();
    std::string normalized; bool slash=false;
    for(char value:combined){
      if(value=='/'){ if(slash) continue; slash=true; }
      else slash=false;
      normalized+=value;
    }
    size_t separator=normalized.find('/');
    edited.share=trim(normalized.substr(0,separator));
    edited.path=separator==std::string::npos?std::string{}:trim(normalized.substr(separator+1));
  };
  auto sharedFolder=[&](){ return edited.path.empty()?edited.share:edited.share+"/"+edited.path; };
  auto validate=[&](){
    edited.name=trim(edited.name); cleanServer(); cleanShare();
    if(edited.name.empty()){ modalMessage("Display name required",{"Enter a name used to identify this share in Cemu."}); return false; }
    if(edited.server.empty()||edited.server.find('/')!=std::string::npos||edited.server.find('\\')!=std::string::npos){
      modalMessage("Invalid SMB server",{"Enter only a host name or IP address.","Example: 192.168.1.20"}); return false;
    }
    bool invalidPath=edited.share.empty()||edited.share.find(':')!=std::string::npos;
    size_t start=0;
    while(!invalidPath&&start<=edited.path.size()){
      size_t slash=edited.path.find('/',start);
      std::string component=trim(edited.path.substr(start,slash==std::string::npos?std::string::npos:slash-start));
      if((component.empty()&&!edited.path.empty())||component=="."||component==".."||component.find(':')!=std::string::npos) invalidPath=true;
      if(slash==std::string::npos) break;
      start=slash+1;
    }
    if(invalidPath){
      modalMessage("Invalid SMB share",{"Enter a share name, optionally followed by folders.","Do not include a drive letter or smb:// prefix."}); return false;
    }
    return true;
  };
  auto editField=[&](int index){
    char value[256]; bool accepted=false;
    if(index==0) accepted=promptTextMode("SMB display name",edited.name.c_str(),value,sizeof(value),false,false,
      "Friendly name shown in the Cemu file browser.","Example: Living room NAS");
    else if(index==1) accepted=promptTextMode("Server or IP address",edited.server.c_str(),value,sizeof(value),false,false,
      "Enter the network host only. Do not include smb:// or a folder.","Example: 192.168.1.20 or NAS.local");
    else if(index==2){ std::string folder=sharedFolder(); accepted=promptTextMode("Shared folder",folder.c_str(),value,sizeof(value),false,false,
      "Enter the share and an optional folder path inside it.","Nested folders are supported."); }
    else if(index==3) accepted=promptTextMode("Username",edited.user.c_str(),value,sizeof(value),false,true,
      "Account used by the SMB server. Leave blank for guest access.","Leave blank for guest");
    else if(index==4) accepted=promptTextMode("Password",edited.password.c_str(),value,sizeof(value),true,true,
      "Password for the SMB account. It is stored in launcher.ini.","Leave blank when no password is required");
    else if(index==5) accepted=promptTextMode("Workgroup",edited.domain.c_str(),value,sizeof(value),false,true,
      "Usually optional on a home network.","Example: WORKGROUP, or leave blank");
    if(!accepted) return;
    if(index==0) edited.name=value;
    else if(index==1){ edited.server=value; cleanServer(); }
    else if(index==2){ edited.share=value; edited.path.clear(); cleanShare(); }
    else if(index==3) edited.user=value;
    else if(index==4) edited.password=value;
    else if(index==5) edited.domain=value;
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel<6) editField(sel);
    else if(sel==6) edited.autoMount=!edited.autoMount;
    else if(validate()){
      if(creating){
        std::unordered_set<std::string> ids;
        for(const auto &existing:loadSmbSharesFromStore()) ids.insert(existing.id);
        uint64_t seed=armGetSystemTick();
        do { char id[17]; snprintf(id,sizeof(id),"%08llx",(unsigned long long)(seed&0xffffffffULL)); edited.id=id; seed=seed*6364136223846793005ULL+1; } while(ids.count(edited.id));
      }
      share=std::move(edited); saved=true; done=true;
    }
  };

  while(!done){
    if(!beginUiFrame()) break;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      int scale=SW>=1600?3:2,rowHeight=27*scale,y0=topBarH()+26;
      int margin=SW>=1600?90:56,helpWidth=SW>=1600?570:420,gap=SW>=1600?44:28;
      int formWidth=SW-margin*2-helpWidth-gap;
      if(touch==TOUCH_TAP){
        if(ty>=SH-42){ done=true; continue; }
        for(int index=0;index<fieldCount;index++) if(tx>=margin&&tx<margin+formWidth&&ty>=y0+index*rowHeight&&ty<y0+(index+1)*rowHeight){ sel=index; activate(); break; }
        int buttonY=y0+fieldCount*rowHeight+10;
        if(tx>=margin&&tx<margin+formWidth&&ty>=buttonY&&ty<buttonY+rowHeight){ sel=saveRow; activate(); }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+totalRows-1)%totalRows;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%totalRows;
      else if((event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)&&sel==6) edited.autoMount=!edited.autoMount;
      else if(event.cbutton.button==BTN_CONFIRM) activate();
      else if(event.cbutton.button==BTN_CANCEL) done=true;
    }

    clearUiBackground();
    drawHeader(creating?"Add SMB network share":"Edit SMB network share",edited.name.empty()?nullptr:edited.name.c_str());
    int scale=SW>=1600?3:2,rowHeight=27*scale,y0=topBarH()+26;
    int margin=SW>=1600?90:56,helpWidth=SW>=1600?570:420,gap=SW>=1600?44:28;
    int formWidth=SW-margin*2-helpWidth-gap,helpX=margin+formWidth+gap;
    int panelHeight=fieldCount*rowHeight+rowHeight+30;
    glassPanel(margin,y0-10,formWidth,panelHeight);
    glassPanel(helpX,y0-10,helpWidth,panelHeight);
    const char *labels[fieldCount]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup"};
    std::string password=edited.password.empty()?"Not set":std::string(std::min<size_t>(16,edited.password.size()),'*');
    const std::string values[fieldCount]={
      edited.name.empty()?"Not set":edited.name,
      edited.server.empty()?"Not set":edited.server,
      edited.share.empty()?"Not set":sharedFolder(),
      edited.user.empty()?"Guest":edited.user,
      password,
      edited.domain.empty()?"Optional":edited.domain,
      edited.autoMount?"On":"Off"
    };
    for(int index=0;index<fieldCount;index++){
      int y=y0+index*rowHeight; bool current=sel==index;
      if(current){ fillRect(margin+8,y,formWidth-16,rowHeight-2,COL_FOCUS); fillRect(margin+8,y,5,rowHeight-2,COL_SEL); }
      drawText(g_font_sm,margin+30,y+(rowHeight-TTF_FontHeight(g_font_sm))/2,labels[index],current?COL_VAL:COL_DIM);
      drawScrollTextR(g_font,margin+formWidth-24,y+(rowHeight-TTF_FontHeight(g_font))/2,formWidth/2-30,values[index].c_str(),current?COL_VAL:COL_TXT);
    }
    int buttonY=y0+fieldCount*rowHeight+10; bool buttonSelected=sel==saveRow;
    fillRect(margin+14,buttonY,formWidth-28,rowHeight-4,buttonSelected?COL_FOCUS:COL_CARD);
    if(buttonSelected) border(margin+14,buttonY,formWidth-28,rowHeight-4,2,COL_SEL);
    drawTextC(g_font,margin+formWidth/2,buttonY+(rowHeight-TTF_FontHeight(g_font))/2-2,
              creating?"Connect and save":"Save changes",buttonSelected?COL_VAL:COL_HI);

    static const char *helpTitle[totalRows]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup","Save share"};
    static const char *helpLine1[totalRows]={
      "A friendly name shown only in Cemu.","The host name or IP of your SMB server.","The share name and optional folder path.","Leave blank when the share allows guests.",
      "The password for the selected account.","Usually optional on home networks.","Reconnect this share when the launcher opens.","Validate the fields and connect to the share."
    };
    static const char *helpLine2[totalRows]={
      "Example: Living room NAS","Example: 192.168.1.20 or NAS.local","Nested folders are supported.","Use the account configured on your NAS or PC.",
      "The value is masked on this screen.","Example: WORKGROUP","Turn this off for manually connected shares.","Connection errors will be shown after saving."
    };
    drawText(g_font_big,helpX+28,y0+22,helpTitle[sel],COL_HI);
    int helpLineHeight=TTF_FontHeight(g_font_sm)+4;
    drawWrapped(g_font_sm,helpX+28,y0+92,helpWidth-56,helpLineHeight,2,helpLine1[sel],COL_TXT);
    drawWrapped(g_font_sm,helpX+28,y0+156,helpWidth-56,helpLineHeight,2,helpLine2[sel],COL_DIM);
    std::string address="smb://"+(edited.server.empty()?std::string("server"):edited.server)+"/"+(edited.share.empty()?std::string("share"):sharedFolder());
    drawText(g_font_sm,helpX+28,y0+210,"Connection preview",COL_DIM);
    drawScrollTextL(g_font,helpX+28,y0+244,helpWidth-56,address.c_str(),COL_VAL);
    drawButtonHint(helpX+28,y0+panelHeight-66,"A","Edit / toggle");
    drawButtonHint(helpX+28,y0+panelHeight-32,"B","Cancel");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  return saved;
}

static void networkSharesScreen() {
  int sel=0,top=0;
  for(;;){
    auto shares=loadSmbSharesFromStore(); int n=1+(int)shares.size();
    const int listY=112,rowHeight=60; int vis=std::max(1,(SH-listY-58)/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(shares.size()>=8){ toast("Maximum of 8 SMB shares"); SDL_Delay(900); continue; }
            SwitchStorage::SmbShare share;
            if(editSmbShare(share,true)){
              shares.push_back(share); saveSmbShares(shares);
              std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error});
              sel=(int)shares.size(); rebuild=true;
            }
          } else {
            auto &share=shares[sel-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
            const char *actions[]={mounted?"Disconnect":"Connect","Edit","Toggle connect at startup","Remove"};
            int action=dropdown(share.name.c_str(),actions,4,0);
            if(action==0){
              if(mounted) SwitchStorage::UnmountSmb(share.id);
              else { std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
              rebuild=true;
            } else if(action==1){
              SwitchStorage::SmbShare edited=share;
              if(editSmbShare(edited,false)){
                bool reconnect=mounted||edited.autoMount;
                SwitchStorage::UnmountSmb(share.id); share=std::move(edited); saveSmbShares(shares);
                if(reconnect){ std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
                rebuild=true;
              }
            } else if(action==2){ share.autoMount=!share.autoMount; saveSmbShares(shares); rebuild=true; }
            else if(action==3&&confirmBox("Remove SMB share?",{share.name,"","Saved folders on this share will also be removed."})){
              std::string root=SwitchStorage::SmbRootPath(share.id); SwitchStorage::UnmountSmb(share.id);
              shares.erase(shares.begin()+sel-1); saveSmbShares(shares); removeSavedPathsBelow(root);
              sel=std::max(0,sel-1); rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      std::string summary=std::to_string(shares.size())+(shares.size()==1?" saved share":" saved shares");
      drawHeader("SMB network shares",summary.c_str());
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowHeight; bool current=index==sel;
        if(current){ fillRect(56,y-3,SW-112,rowHeight-4,COL_FOCUS); fillRect(56,y-3,5,rowHeight-4,COL_SEL); }
        if(index==0) drawText(g_font,82,y+(rowHeight-TTF_FontHeight(g_font))/2-2,"[ Add SMB share ]",current?COL_VAL:COL_HI);
        else { const auto &share=shares[index-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
          drawText(g_font,82,y,share.name.c_str(),current?COL_VAL:COL_TXT);
          std::string status=mounted?"Connected":(share.autoMount?"Disconnected - auto":"Disconnected");
          drawTextR(g_font_sm,SW-82,y+4,status.c_str(),mounted?(SDL_Color){120,220,120,255}:COL_DIM);
          std::string address="smb://"+share.server+"/"+share.share+(share.path.empty()?std::string{}:"/"+share.path);
          drawText(g_font_sm,82,y+31,ellipsizedText(g_font_sm,address,SW-340).c_str(),COL_DIM); }
      }
      drawSettingsFooter("A  Select       B  Back");
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

enum class BrowserMode { SelectFolder, SelectTitle, Manage };
enum class BrowserItemKind { Use, Up, Paste, Favorite, Directory, File, Location, Smb, ManageSmb };
struct BrowserItem {
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
};

static bool ensurePathMounted(const std::string &path) {
  for(const auto &share:loadSmbSharesFromStore()){
    std::string root=SwitchStorage::SmbRootPath(share.id);
    if(pathAtOrBelow(path,root)){
      if(SwitchStorage::IsSmbMounted(share.id)) return true;
      std::string error;
      if(SwitchStorage::MountSmb(share,&error)) return true;
      modalMessage("SMB connection failed",{share.name,error}); return false;
    }
  }
  return true;
}

static bool isUsbStoragePath(const std::string &path) {
  size_t colon=path.find(':');
  if(colon<4) return false;
  if(tolower((unsigned char)path[0])!='u'||tolower((unsigned char)path[1])!='m'||tolower((unsigned char)path[2])!='s') return false;
  for(size_t index=3;index<colon;index++) if(!isdigit((unsigned char)path[index])) return false;
  return true;
}

static bool hasConfiguredUsbSource(const std::vector<std::string> &paths) {
  return std::any_of(paths.begin(),paths.end(),[](const std::string &path){ return isUsbStoragePath(path); });
}

static bool refreshConfiguredUsbSources(std::vector<std::string> &paths) {
  if(!hasConfiguredUsbSource(paths)) return false;
  const auto locations=SwitchStorage::ListUsbLocations();
  bool changed=false;
  for(auto &path:paths){
    if(!isUsbStoragePath(path)) continue;
    struct stat source{};
    if(stat(path.c_str(),&source)==0&&S_ISDIR(source.st_mode)) continue;
    size_t colon=path.find(':');
    std::string relative=colon==std::string::npos?std::string{}:path.substr(colon+1);
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    std::vector<std::string> matches;
    for(const auto &location:locations){
      std::string candidate=normalizeLocationPath(location.path+relative);
      struct stat candidateStat{};
      if(stat(candidate.c_str(),&candidateStat)==0&&S_ISDIR(candidateStat.st_mode)) matches.push_back(std::move(candidate));
    }
    if(matches.size()==1&&pathIdentity(path)!=pathIdentity(matches.front())){
      path=std::move(matches.front());
      changed=true;
    }
  }
  if(changed){ saveGameSources(paths); storeSave(g_global,LAUNCHER_INI); }
  return changed;
}

static void renderUsbForwarderWait() {
  clearUiBackground();
  const int panelWidth=720,panelHeight=220;
  const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
  glassPanel(panelX,panelY,panelWidth,panelHeight);
  border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
  drawTextC(g_font_big,SW/2,panelY+42,"Connecting USB storage",COL_SEL);
  drawTextC(g_font,SW/2,panelY+108,"Waiting for the game drive...",COL_TXT);
  drawTextC(g_font_sm,SW/2,panelY+150,"The game will start automatically",COL_DIM);
  drawSettingsFooter("B  Cancel",panelY+190);
  SDL_RenderPresent(g_ren);
}

static void ensureSavedPathMountedAtStartup(const std::string &path) {
  auto shares=loadSmbSharesFromStore();
  bool changed=false;
  for(auto &share:shares){
    if(pathAtOrBelow(path,SwitchStorage::SmbRootPath(share.id))&&!share.autoMount){
      share.autoMount=true;
      changed=true;
    }
  }
  if(changed) saveSmbShares(shares);
}

static std::vector<BrowserItem> browserItems(const std::string &current,BrowserMode mode,bool &opened) {
  std::vector<BrowserItem> items; opened=true;
  if(current.empty()){
    SwitchStorage::InitializeUsb();
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true});
    for(const auto &usb:SwitchStorage::ListUsbLocations()) items.push_back({usb.label,usb.path,BrowserItemKind::Location,true});
    for(const auto &share:loadSmbSharesFromStore()){
      bool mounted=SwitchStorage::IsSmbMounted(share.id);
      std::string label="SMB - "+(share.name.empty()?share.share:share.name)+(mounted?"":" (disconnected)");
      items.push_back({label,SwitchStorage::SmbBrowsePath(share),BrowserItemKind::Smb,true});
    }
    for(const auto &favorite:loadFavoriteFolders()) items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true});
    items.push_back({"Manage SMB shares","",BrowserItemKind::ManageSmb,true});
    return items;
  }
  if(mode==BrowserMode::SelectFolder) items.push_back({"[ Use this folder ]",current,BrowserItemKind::Use,true});
  if(mode==BrowserMode::SelectTitle&&folderIsExtractedTitle(current)) items.push_back({"[ Install extracted title here ]",current,BrowserItemKind::Use,true});
  if(mode==BrowserMode::Manage&&!g_fileClipboard.path.empty()) items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,BrowserItemKind::Paste,true});
  if(mode==BrowserMode::Manage){
    auto favorites=loadFavoriteFolders();
    bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(current); });
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true});
  DIR *dir=opendir(current.c_str());
  if(!dir){ opened=false; return items; }
  std::vector<BrowserItem> entries; struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string path=join(current,entry->d_name); bool directory=entry->d_type==DT_DIR;
    if(entry->d_type==DT_UNKNOWN){ struct stat st{}; if(stat(path.c_str(),&st)!=0) continue; directory=S_ISDIR(st.st_mode); }
    if(!directory&&mode==BrowserMode::SelectFolder) continue;
    if(!directory&&mode==BrowserMode::SelectTitle&&!isTitleFile(entry->d_name)) continue;
    entries.push_back({std::string(entry->d_name)+(directory?"/":""),path,directory?BrowserItemKind::Directory:BrowserItemKind::File,directory});
  }
  closedir(dir);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory) return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path) {
  auto favorites=loadFavoriteFolders(); std::string identity=pathIdentity(path);
  auto iterator=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){ return pathIdentity(entry)==identity; });
  bool pinned=iterator==favorites.end();
  if(pinned){
    if(favorites.size()>=24){ toast("Maximum of 24 pinned folders"); SDL_Delay(900); return false; }
    ensureSavedPathMountedAtStartup(path);
    favorites.push_back(normalizeLocationPath(path));
  }
  else favorites.erase(iterator);
  saveFavoriteFolders(favorites); toast(pinned?"Folder pinned":"Folder unpinned"); SDL_Delay(650); return true;
}

static bool browserActions(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&item.kind!=BrowserItemKind::Use) return false;
  std::vector<std::string> labels;
  if(mode==BrowserMode::Manage){ labels={"Copy","Move","Rename"}; }
  bool canPin=item.directory;
  bool pinned=false;
  if(canPin){
    auto favorites=loadFavoriteFolders();
    pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(item.path); });
    labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  if(labels.empty()) return false;
  std::vector<const char*> choices; for(const auto &label:labels) choices.push_back(label.c_str());
  int action=dropdown("File options",choices.data(),(int)choices.size(),0);
  if(action<0) return false;
  if(mode==BrowserMode::Manage&&action==0){ g_fileClipboard={item.path,false}; toast("Copied to clipboard"); SDL_Delay(600); return false; }
  if(mode==BrowserMode::Manage&&action==1){ g_fileClipboard={item.path,true}; toast("Move queued"); SDL_Delay(600); return false; }
  if(mode==BrowserMode::Manage&&action==2){
    char name[256]; std::string oldName=fileNameOf(item.path);
    if(!promptText("Rename",oldName.c_str(),name,sizeof(name))) return false;
    std::string newName=trim(name);
    if(!validEntryName(newName)){ modalMessage("Invalid name",{"Names cannot contain /, \\, :, or control characters."}); return false; }
    std::string destination=join(parentFolder(item.path),newName); struct stat st{};
    if(lstat(destination.c_str(),&st)==0){ modalMessage("Rename failed",{"An item with that name already exists."}); return false; }
    if(rename(item.path.c_str(),destination.c_str())!=0){ modalMessage("Rename failed",{strerror(errno)}); return false; }
    replaceSavedPathPrefix(item.path,destination); toast("Renamed"); SDL_Delay(600); return true;
  }
  if(canPin) return toggleFavorite(item.path);
  return false;
}

static std::string runFileBrowser(const std::string &start,BrowserMode mode) {
  std::string current=normalizeLocationPath(start);
  if(!current.empty()&&!ensurePathMounted(current)) current.clear();
  int sel=0,top=0;
  for(;;){
    bool opened=false; auto items=browserItems(current,mode,opened);
    if(!opened){ modalMessage("Folder unavailable",{current,"","The device may be disconnected."}); current.clear(); sel=top=0; continue; }
    int n=(int)items.size(),vis=std::max(1,(SH-178)/46); if(n==0){ current.clear(); continue; }
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return {};
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48){ uiAudioPlay(UiSound::Back); return {}; }
          for(int row=0;row<vis&&top+row<n;row++){ int y=112+row*46; if(ty>=y&&ty<y+42){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL){ if(current.empty()) return {}; current=parentFolder(current); sel=top=0; rebuild=true; }
        else if(event.cbutton.button==BTN_SETTINGS){ if(browserActions(items[sel],mode)) rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty()){ executePaste(current); rebuild=true; }
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Use) return item.path;
          if(item.kind==BrowserItemKind::Paste){ executePaste(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Favorite){ toggleFavorite(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Up){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::ManageSmb){ networkSharesScreen(); sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Directory){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::File&&mode==BrowserMode::SelectTitle) return item.path;
          else if(item.kind==BrowserItemKind::Location||item.kind==BrowserItemKind::Smb){
            if(ensurePathMounted(item.path)){ DIR *test=opendir(item.path.c_str()); if(test){ closedir(test); current=item.path; sel=top=0; rebuild=true; } else modalMessage("Location unavailable",{item.path}); }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      const char *title=mode==BrowserMode::Manage?"File manager":mode==BrowserMode::SelectTitle?"Select title":"Select game folder";
      drawText(g_font_big,64,30,title,COL_HI);
      drawTextR(g_font_sm,SW-64,48,current.empty()?"Locations":ellipsizedText(g_font_sm,current,SW/2).c_str(),COL_DIM);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=112+row*46; bool selected=index==sel; const auto &item=items[index];
        if(selected){ fillRect(54,y-3,SW-108,42,COL_FOCUS); fillRect(54,y-3,5,42,COL_SEL); }
        SDL_Color color=item.kind==BrowserItemKind::Use||item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        drawText(g_font,80,y,ellipsizedText(g_font,item.label,SW-180).c_str(),selected?COL_VAL:color);
      }
      std::string footer=mode==BrowserMode::Manage?"A  Open       X  Actions       Y  Paste       B  Back":"A  Open / Select       X  Pin       B  Back";
      drawSettingsFooter(footer.c_str());
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

static std::string browseFolder(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectFolder);
}

static void runFileManager() {
  runFileBrowser({},BrowserMode::Manage);
}

static bool isTitleFile(const char *name) {
  const char *dot = strrchr(name, '.'); if (!dot) return false;
  return !strcasecmp(dot, ".wua") || !strcasecmp(dot, ".wud") || !strcasecmp(dot, ".wux") || !strcasecmp(dot, ".wuhb");
}
static bool folderIsExtractedTitle(const std::string &dir) {
  struct stat st;
  if (stat((dir + "/code/app.xml").c_str(), &st) == 0) return true;
  bool c = stat((dir+"/content").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  bool o = stat((dir+"/code").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  bool m = stat((dir+"/meta").c_str(),&st)==0 && S_ISDIR(st.st_mode);
  return c && o && m;
}
static bool copyFileProgress(const std::string &src, const std::string &dst, long long size, void(*prog)(int)) {
  FILE *in = fopen(src.c_str(), "rb"); if (!in) return false;
  if (!recoverAtomicFile(dst)) { fclose(in); return false; }
  const std::string tmp = dst + ".tmp";
  FILE *out = fopen(tmp.c_str(), "wb"); if (!out) { fclose(in); return false; }
  static unsigned char buf[1 << 18]; size_t nr; bool ok = true; long long written = 0; int last = -1;
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  while ((nr = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, nr, out) != nr) { ok = false; break; }
    written += (long long)nr;
    int pct = size > 0 ? (int)(written * 100 / size) : 100;
    if (pct > 100) pct = 100;
    if (prog && pct != last) { prog(pct); last = pct; }
  }
  if (ferror(in)) ok = false;
  if (written != size) ok = false;
  if (ok && fflush(out) != 0) ok = false;
  if (ok && fsync(fileno(out)) != 0) ok = false;
  if (fclose(in) != 0) ok = false;
  if (fclose(out) != 0) ok = false;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  struct stat copied{};
  if (ok && (stat(tmp.c_str(), &copied) != 0 || !S_ISREG(copied.st_mode) || (long long)copied.st_size != size)) ok = false;
  if (ok && !replaceAtomic(dst, tmp)) ok = false;
  if (!ok) remove(tmp.c_str());
  if (ok && prog && last != 100) prog(100);
  return ok;
}
static std::string browseTitleFile(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectTitle);
}

static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key, o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){
    if(o.key&&!strcmp(o.key,"console_language")&&!strcmp(iniGet(o.key,o.def),"-1"))
      snprintf(out,n,"Auto (%s)",g_systemLanguageName);
    else { int i=choiceIdx(o); snprintf(out,n,"%s", i>=0?o.ch[i].label:iniGet(o.key,o.def)); }
  }
  else if (o.type==OT_RANGE){
    if (o.key && !strcmp(o.key,"AudioDelay")) snprintf(out,n,"%d ms", atoi(iniGet(o.key,o.def))*12);
    else if(o.key&&!strcmp(o.key,"in_deadzone")) snprintf(out,n,"%s%%",iniGet(o.key,o.def));
    else snprintf(out,n,"%s", iniGet(o.key,o.def));
  }
  else if (o.type==OT_ACTION) snprintf(out,n,"Press A");
}
static void optAdjust(const Opt &o, int dir) {
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch; iniSet(o.key,o.ch[i].val); }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
}


static float g_hy = -1;
static Uint32 g_fxT = 0;
static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
static int topBarH(){ return SW >= 1600 ? 112 : 80; }
static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c);
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c);
static void drawHeader(const char *title, const char *ctx){
  int bandH = topBarH() - 4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  drawTextC(g_font_big,SW/2,(bandH-TTF_FontHeight(g_font_big))/2,title,COL_VAL);
  if (ctx&&*ctx){
    int titleRight = SW/2 + textW(g_font_big,title)/2;
    int maxW = (SW-28) - titleRight - 30;
    if(maxW > 40) drawScrollTextR(g_font_sm,SW-28,(bandH-TTF_FontHeight(g_font_sm))/2,maxW,ctx,COL_VAL);
  }
}
static const int ROW_H = 46, LIST_Y0 = 118;
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int w = SW-180; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2; *labelX=*colX+40; *valX=*colX+w-40;
}
static int listVis(){ int v=(SH-LIST_Y0-72)/ROW_H; return v<1?1:v; }

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawHeader(S.title, ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  glassPanel(colX-12,LIST_Y0-10,colW+24,vis*ROW_H+18);
  int fh0=TTF_FontHeight(g_font);
  float ty = (float)(LIST_Y0 + (sel-top)*ROW_H + 1);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  fillRect(colX,(int)g_hy,colW,ROW_H-2,COL_FOCUS);
  fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,y=LIST_Y0+r*ROW_H+(ROW_H-fh0)/2; bool cur=(i==sel);
    SDL_Color lc = cur?COL_VAL:COL_TXT;
    SDL_Color vc = cur?COL_VAL:COL_DIM;
    drawText(g_font,labelX,y,S.opts[i].label,lc);
    char v[96]; optValue(S.opts[i],v,sizeof(v));
    drawTextR(g_font,valX,y,v,vc);
  }
  if(S.n>vis){
    int trH=vis*ROW_H, trX=colX+colW+16, trY=LIST_Y0-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  drawFadeIn();
  SDL_RenderPresent(g_ren);
}

static int dropdown(const char *title, const char *const *labels, int n, int cur) {
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = 52;
  int vis = (SH - 200) / rowH; if (vis < 1) vis = 1; if (vis > n) vis = n;
  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) return cur;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){ int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ return top+r; } }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return cur;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    int pw = SW>760?760:SW-160, ph = 90 + vis*rowH, px=(SW-pw)/2, py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+18, title, COL_VAL);
    int ly = py+70;
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=ly+r*rowH; bool curr=(i==sel);
      if(curr){ fillRect(px+8,y,pw-16,rowH-4,COL_FOCUS); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      drawText(g_font, px+34, y+(rowH-TTF_FontHeight(g_font))/2, labels[i], curr?COL_VAL:COL_TXT);
    }
    if(n>vis){ int trH=vis*rowH,trX=px+pw-12,trY=ly; fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n,dn=(n-vis>0?n-vis:1); fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL); }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32]; int n = o.nch>32?32:o.nch;
  std::string automaticLanguage;
  for(int i=0;i<n;i++){
    if(i==0&&o.key&&!strcmp(o.key,"console_language")&&!strcmp(o.ch[i].val,"-1")){
      automaticLanguage="Auto ("+std::string(g_systemLanguageName)+")";
      labels[i]=automaticLanguage.c_str();
    } else labels[i]=o.ch[i].label;
  }
  int idx = dropdown(o.label, labels, n, choiceIdx(o));
  if(idx>=0 && idx<o.nch) iniSet(o.key, o.ch[idx].val);
}

static const char *mappingTokenForButton(Uint8 button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_B: return "A";
    case SDL_CONTROLLER_BUTTON_A: return "B";
    case SDL_CONTROLLER_BUTTON_Y: return "X";
    case SDL_CONTROLLER_BUTTON_X: return "Y";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
    case SDL_CONTROLLER_BUTTON_START: return "PLUS";
    case SDL_CONTROLLER_BUTTON_BACK: return "MINUS";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "DUP";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "DDOWN";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "DLEFT";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "DRIGHT";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "LCLICK";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "RCLICK";
    default: return nullptr;
  }
}

static const char *mappingTokenLabel(const char *token) {
  if (!strcmp(token,"PLUS")) return "Plus";
  if (!strcmp(token,"MINUS")) return "Minus";
  if (!strcmp(token,"DUP")) return "D-Pad Up";
  if (!strcmp(token,"DDOWN")) return "D-Pad Down";
  if (!strcmp(token,"DLEFT")) return "D-Pad Left";
  if (!strcmp(token,"DRIGHT")) return "D-Pad Right";
  if (!strcmp(token,"LCLICK")) return "L-Stick click";
  if (!strcmp(token,"RCLICK")) return "R-Stick click";
  if (!strcmp(token,"NONE")) return "Unmapped";
  return token;
}

static bool mappingInputHeld() {
  if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) return false;
  SDL_GameControllerUpdate();
  for (int button=0; button<SDL_CONTROLLER_BUTTON_MAX; button++)
    if (SDL_GameControllerGetButton(g_pad,(SDL_GameControllerButton)button)) return true;
  return SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>12000 ||
         SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>12000;
}

static void renderMappingPrompt(const InputMapping &mapping,int index,bool releasing,const char *status) {
  clearUiBackground();
  drawHeader("Control mapping", "Press-to-bind");
  int panelW=std::min(900,SW-180),panelH=330,panelX=(SW-panelW)/2,panelY=(SH-panelH)/2-20;
  glassPanel(panelX,panelY,panelW,panelH);
  border(panelX,panelY,panelW,panelH,3,COL_SEL);
  char step[48]; snprintf(step,sizeof(step),"Control %d of %d",index+1,(int)(sizeof(C_inputMappings)/sizeof(*C_inputMappings)));
  drawTextC(g_font_sm,SW/2,panelY+36,step,COL_DIM);
  drawTextC(g_font_big,SW/2,panelY+94,mapping.label,COL_VAL);
  if(releasing) drawTextC(g_font,SW/2,panelY+180,"Release the current button",COL_TXT);
  else drawTextC(g_font,SW/2,panelY+180,"Press the button or trigger to assign",COL_TXT);
  const char *current=iniGet(mapping.key,mapping.def);
  std::string currentLine=std::string("Current: ")+mappingTokenLabel(current);
  drawTextC(g_font_sm,SW/2,panelY+238,status&&*status?status:currentLine.c_str(),status&&*status?COL_HI:COL_DIM);
  drawTextC(g_font_sm,SW/2,SH-72,"Hold Plus to clear       Hold Minus to cancel",COL_DIM);
  drawTextC(g_font_sm,SW/2,SH-38,"Touch left to clear       Touch right to cancel",COL_DIM);
  SDL_RenderPresent(g_ren);
}

static bool captureMappingInput(const InputMapping &mapping,int index,std::string &token) {
  bool releasing=true;
  while (mappingInputHeld()) {
    if (!beginUiFrame()) return false;
    SDL_Event event; while (pollUiEvent(event)) { int x=0,y=0; if(touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-92) return false; }
    renderMappingPrompt(mapping,index,true,nullptr);
    SDL_Delay(8);
  }
  releasing=false;
  int heldSpecial=-1;
  Uint32 heldSince=0;
  for (;;) {
    if (!beginUiFrame()) return false;
    SDL_Event event;
    while (pollUiEvent(event)) {
      int x=0,y=0;
      if (touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-92) {
        if (x<SW/2) { token="NONE"; return true; }
        return false;
      }
      if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) return false;
      if (event.type==SDL_CONTROLLERBUTTONDOWN) {
        const char *captured=mappingTokenForButton(event.cbutton.button);
        if (!captured) continue;
        if (event.cbutton.button==SDL_CONTROLLER_BUTTON_START || event.cbutton.button==SDL_CONTROLLER_BUTTON_BACK) {
          heldSpecial=event.cbutton.button;
          heldSince=SDL_GetTicks();
        } else {
          token=captured;
          return true;
        }
      } else if (event.type==SDL_CONTROLLERBUTTONUP && event.cbutton.button==heldSpecial) {
        token=mappingTokenForButton((Uint8)heldSpecial);
        return true;
      } else if (event.type==SDL_CONTROLLERAXISMOTION && event.caxis.value>16000) {
        if (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERLEFT) { token="ZL"; return true; }
        if (event.caxis.axis==SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { token="ZR"; return true; }
      }
    }
    const Uint32 heldFor=heldSpecial<0?0:SDL_GetTicks()-heldSince;
    if (heldFor>=800) {
      if (heldSpecial==SDL_CONTROLLER_BUTTON_START) { token="NONE"; return true; }
      return false;
    }
    const char *status=heldSpecial==SDL_CONTROLLER_BUTTON_START?"Keep holding Plus to clear":"";
    if (heldSpecial==SDL_CONTROLLER_BUTTON_BACK) status="Keep holding Minus to cancel";
    renderMappingPrompt(mapping,index,releasing,status);
    SDL_Delay(8);
  }
}

static void runInputMappingScreen() {
  const int count=(int)(sizeof(C_inputMappings)/sizeof(*C_inputMappings));
  static int savedSelection=0;
  int sel=std::max(0,std::min(savedSelection,count-1));
  int top=std::max(0,sel-std::min(listVis(),count)+1);

  auto assignSelected=[&]() {
    if (!g_pad || !SDL_GameControllerGetAttached(g_pad)) {
      modalMessage("Control mapping", {"No controller is connected."});
      beginScreenFx();
      return;
    }
    std::string token;
    if (captureMappingInput(C_inputMappings[sel],sel,token)) {
      iniSet(C_inputMappings[sel].key,token.c_str());
      toast("Control assigned");
    }
    beginScreenFx();
  };

  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) { savedSelection=sel; return; }
    SDL_Event event;
    navRepeat();
    while (pollUiEvent(event)) {
      pumpStick(event);
      int tx=0,ty=0;
      const TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(listVis(),count);
      if (touchScrollList(touch,sel,top,count,visible)) continue;
      if (touch==TOUCH_TAP) {
        if (ty<topBarH() || ty>=SH-40) { savedSelection=sel; return; }
        int colX,colW,labelX,valX;
        listCol(&colX,&colW,&labelX,&valX);
        for (int row=0; row<visible && top+row<count; ++row) {
          const int y=LIST_Y0+row*ROW_H;
          if (tx>=colX && tx<colX+colW && ty>=y && ty<y+ROW_H) {
            sel=top+row;
            assignSelected();
            break;
          }
        }
        continue;
      }
      if (event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if (event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+count-1)%count;
      else if (event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%count;
      else if (event.cbutton.button==BTN_CONFIRM) assignSelected();
      else if (event.cbutton.button==BTN_CANCEL) { savedSelection=sel; return; }

      if (sel<top) top=sel;
      if (sel>=top+visible) top=sel-visible+1;
      if (top<0) top=0;
    }

    clearUiBackground();
    drawHeader("Control mapping","Select a control");
    int colX,colW,labelX,valX;
    listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(listVis(),count);
    glassPanel(colX-12,LIST_Y0-10,colW+24,visible*ROW_H+18);
    const float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,ROW_H-2,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
    const int fontHeight=TTF_FontHeight(g_font);
    for (int row=0; row<visible && top+row<count; ++row) {
      const int index=top+row;
      const int y=LIST_Y0+row*ROW_H+(ROW_H-fontHeight)/2;
      const bool current=index==sel;
      drawText(g_font,labelX,y,C_inputMappings[index].label,current?COL_VAL:COL_TXT);
      drawTextR(g_font,valX,y,mappingTokenLabel(iniGet(C_inputMappings[index].key,C_inputMappings[index].def)),current?COL_VAL:COL_DIM);
    }
    if (count>visible) {
      const int trackHeight=visible*ROW_H;
      const int trackX=colX+colW+16;
      const int trackY=LIST_Y0-2;
      fillRect(trackX,trackY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=trackHeight*visible/count;
      const int denominator=std::max(1,count-visible);
      fillRect(trackX,trackY+(trackHeight-thumbHeight)*top/denominator,4,thumbHeight,COL_SEL);
    }
    drawSettingsFooter("A  Assign       B  Back");
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};
static void runSettings(int scr, const char *ctx) {
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr], top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  auto nav=[&](int dir){ sel=(sel+dir+S.n)%S.n; };
  auto adjust=[&](const Opt &option,int direction){
    optAdjust(option,direction);
    if(scr==SCR_FRAMEGEN && option.key && !strcmp(option.key,"Wrapper/LSFGEnabled") &&
       !strcmp(iniGet(option.key,option.def),"true") && !regularFileExists(LSFG_DLL_FILE)){
      iniSet(option.key,"false");
      modalMessage("LSFG is not ready", {
        "Lossless.dll was not found.",
        "Copy it to sdmc:/switch/cemu/lsfg/Lossless.dll",
        "then enable LSFG again."
      });
      beginScreenFx();
    }
  };
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ adjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ adjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=LIST_Y0+r*ROW_H;
            if(ty>=y && ty<y+ROW_H){ int ni=top+r; sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  adjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: adjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_ACTION && scr==SCR_INPUT){ runInputMappingScreen(); beginScreenFx(); }
          else if(o.type==OT_CHOICE && o.nch>2){ optChoosePopup(o); beginScreenFx(); }
          else adjust(o,1);
          break;
        }
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;
    }
    renderSettings(scr,sel,top,ctx);
    SDL_Delay(8);
  }
}

static std::string launcherUpdateStatusText() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking: return "Checking...";
    case LauncherUpdateState::UpdateAvailable: return snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate: return "Up to date";
    case LauncherUpdateState::Downloading: {
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const uint64_t percent=total?std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      return "Downloading "+std::to_string(percent)+"%";
    }
    case LauncherUpdateState::ReadyToInstall: return "Ready to install";
    case LauncherUpdateState::Installing: return "Installing...";
    case LauncherUpdateState::Installed: return "Ready to exit";
    case LauncherUpdateState::Cancelled: return "Cancelled";
    case LauncherUpdateState::Error: return "Check failed";
    case LauncherUpdateState::Idle: break;
  }
  return std::string("Installed ")+installedReleaseTag();
}

static std::string steamGridDbKey(){
  return trim(storeGet(g_global,"Wrapper/SteamGridDBKey",""));
}

static bool promptAndSaveSteamGridDbKey(const char *header,bool allowEmpty){
  const std::string current=steamGridDbKey();
  char value[256];
  if(!promptTextMode(header,current.c_str(),value,sizeof(value),true,allowEmpty,
                     "Used for cover and shortcut artwork downloads.",
                     allowEmpty?"Leave blank to remove the saved key":"Enter a valid key to continue"))
    return false;
  const std::string normalized=trim(value);
  if(!allowEmpty&&normalized.empty()) return false;
  storeSet(g_global,"Wrapper/SteamGridDBKey",normalized.c_str());
  storeSave(g_global,LAUNCHER_INI);
  return true;
}

static void launcherSettingsScreen() {
  static int savedSelection=0,savedTop=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int apiKeyRow=optionCount,coversRow=optionCount+1,listCount=optionCount+2;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=std::max(0,savedTop);
  auto applyChange=[&](){
    applyLauncherAppearance();
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){ savedSelection=sel; savedTop=top; storeSave(g_global,LAUNCHER_INI); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=LIST_Y0+row*ROW_H;
          if(ty>=y&&ty<y+ROW_H){
            sel=top+row;
            SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
            break;
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); beginScreenFx(); }
        else if(sel==apiKeyRow){
          if(promptAndSaveSteamGridDbKey("SteamGridDB API key",true)){
            toast(steamGridDbKey().empty()?"SteamGridDB API key removed":"SteamGridDB API key updated");
            SDL_Delay(900);
          }
          beginScreenFx();
        }
        else if(sel==coversRow){ downloadAllCovers(); beginScreenFx(); }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){
        if(sel<top) top=sel;
        if(sel>=top+visible) top=sel-visible+1;
      }
    }

    clearUiBackground();
    drawHeader("Launcher",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int visible=std::min(std::max(1,(SH-LIST_Y0-190)/ROW_H),listCount);
    const int fontHeight=TTF_FontHeight(g_font);
    glassPanel(colX-12,LIST_Y0-10,colW+24,visible*ROW_H+18);
    if(sel<listCount){
      float target=(float)(LIST_Y0+(sel-top)*ROW_H+1);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      fillRect(colX,(int)g_hy,colW,ROW_H-2,COL_FOCUS);
      fillRect(colX,(int)g_hy,5,ROW_H-2,COL_SEL);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,y=LIST_Y0+row*ROW_H+(ROW_H-fontHeight)/2; bool current=index==sel;
      if(index==apiKeyRow){
        drawText(g_font,labelX,y,"SteamGridDB API key",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,y+(fontHeight-TTF_FontHeight(g_font_sm))/2,
                  steamGridDbKey().empty()?"Not set":"Configured",current?COL_VAL:COL_DIM);
      } else if(index==coversRow){
        drawText(g_font,labelX,y,"Download all covers",current?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,y+(fontHeight-TTF_FontHeight(g_font_sm))/2,"SteamGridDB",current?COL_VAL:COL_DIM);
      } else {
        drawText(g_font,labelX,y,S_launcher[index].label,current?COL_VAL:COL_TXT);
        char value[96]; optValue(S_launcher[index],value,sizeof(value));
        drawTextR(g_font,valX,y,value,current?COL_VAL:COL_DIM);
      }
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,LIST_Y0+visible*ROW_H+24);
    const bool updateSelected=sel==updateRow;
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected?COL_FOCUS:(SDL_Color){35,40,50,225});
    border(buttonX,buttonY,buttonWidth,buttonHeight,2,updateSelected?COL_SEL:COL_DIM);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-fontHeight)/2,"Check for Updates",updateSelected?COL_VAL:COL_TXT);
    const std::string updateStatus=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,updateStatus.c_str(),updateSelected?COL_VAL:COL_DIM);
    drawSettingsFooter("Left / Right  Change       A  Choose       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}
static void installProgress(int pct) {
  clearUiBackground();
  drawHeader("Installing title...", nullptr);
  int bw=SW*2/3, bx=(SW-bw)/2, by=SH/2-20, bh=40;
  border(bx,by,bw,bh,2,COL_SEL);
  fillRect(bx+3,by+3,(bw-6)*pct/100,bh-6,COL_HI);
  char t[16]; snprintf(t,sizeof(t),"%d%%",pct);
  drawTextC(g_font,SW/2,by+bh+18,t,COL_TXT);
  SDL_RenderPresent(g_ren);
}
static void doInstallFlow() {
  std::string picked = browseTitleFile("sdmc:/");
  if (picked.empty()) return;
  struct stat st;
  if (stat(picked.c_str(), &st) != 0) { toast("Not found"); SDL_Delay(1400); return; }
  if (S_ISDIR(st.st_mode)) {
    std::string msg;
    int r = cemu_installTitle(picked, std::string(DATA_DIR)+"/mlc01", installProgress, msg);
    toast(msg.c_str()); SDL_Delay(1600);
    if (r == 0) { ensureDefaultGameSource(); g_rescanAfterSettings = true; }
  } else {
    std::string fname = picked.substr(picked.find_last_of('/') + 1);
    std::string gamesDir = std::string(DATA_DIR) + "/games";
    std::string dst = gamesDir + "/" + fname;
    if (picked == dst) { toast("Already in the games folder"); SDL_Delay(1600); return; }
    if (mkdir(gamesDir.c_str(), 0777) != 0 && errno != EEXIST) { toast("Could not create games folder"); SDL_Delay(1600); return; }
    struct stat dirStat{};
    if (stat(gamesDir.c_str(), &dirStat) != 0 || !S_ISDIR(dirStat.st_mode)) { toast("Games path is not a folder"); SDL_Delay(1600); return; }
    if (!recoverAtomicFile(dst)) { toast("Could not recover previous install"); SDL_Delay(1600); return; }
    struct stat dstStat{};
    if (stat(dst.c_str(), &dstStat) == 0) {
      if (!S_ISREG(dstStat.st_mode)) { toast("Destination is not a file"); SDL_Delay(1600); return; }
      if (!confirmBox("Replace existing file?", {fname, "", "The existing copy will be replaced."})) return;
    } else if (errno != ENOENT) {
      toast("Could not check destination"); SDL_Delay(1600); return;
    }
    bool ok = copyFileProgress(picked, dst, (long long)st.st_size, installProgress);
    toast(ok ? "Installed to games folder" : "Copy failed (SD space?)"); SDL_Delay(1600);
    if (ok) { ensureDefaultGameSource(); g_rescanAfterSettings = true; }
  }
}

static void ensureDefaultGameSource() {
  auto sources=loadGameSources();
  std::string identity=pathIdentity(DEF_GAMEDIR);
  if(std::none_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
    sources.push_back(DEF_GAMEDIR); saveGameSources(sources); g_rescanAfterSettings=true;
  }
}

static void gameSourcesScreen() {
  int sel=0,top=0;
  for(;;){
    auto sources=loadGameSources(); int n=1+(int)sources.size(); int vis=std::max(1,(SH-176)/50);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=112+row*50; if(ty>=y&&ty<y+46){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(sources.size()>=16){ toast("Maximum of 16 game folders"); SDL_Delay(900); continue; }
            std::string selected=browseFolder({});
            if(!selected.empty()){
              std::string identity=pathIdentity(selected);
              if(std::any_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
                toast("Folder already added"); SDL_Delay(800);
              } else { ensureSavedPathMountedAtStartup(selected); sources.push_back(selected); saveGameSources(sources); g_rescanAfterSettings=true; sel=(int)sources.size(); }
              rebuild=true;
            }
          } else {
            const char *actions[]={"Change folder","Move up","Move down","Remove"};
            int action=dropdown("Game folder",actions,4,0); size_t index=(size_t)(sel-1);
            if(action==0){
              std::string selected=browseFolder(sources[index]);
              if(!selected.empty()){
                std::string identity=pathIdentity(selected); bool duplicate=false;
                for(size_t i=0;i<sources.size();i++) if(i!=index&&pathIdentity(sources[i])==identity) duplicate=true;
                if(duplicate){ toast("Folder already added"); SDL_Delay(800); }
                else { ensureSavedPathMountedAtStartup(selected); sources[index]=selected; saveGameSources(sources); g_rescanAfterSettings=true; }
                rebuild=true;
              }
            } else if(action==1&&index>0){ std::swap(sources[index],sources[index-1]); saveGameSources(sources); sel--; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==2&&index+1<sources.size()){ std::swap(sources[index],sources[index+1]); saveGameSources(sources); sel++; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==3&&confirmBox("Remove game folder?",{sources[index],"","No files will be deleted."})){
              sources.erase(sources.begin()+index); saveGameSources(sources); sel=std::max(0,sel-1); g_rescanAfterSettings=true; rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      drawText(g_font_big,64,34,"Game folders",COL_HI);
      drawTextR(g_font_sm,SW-64,52,"All folders are scanned and passed to Cemu",COL_DIM);
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=112+row*50; bool current=index==sel;
        if(current){ fillRect(56,y-3,SW-112,46,COL_FOCUS); fillRect(56,y-3,5,46,COL_SEL); }
        std::string label=index==0?"[ Add game folder ]":sources[index-1];
        drawText(g_font,82,y,ellipsizedText(g_font,label,SW-170).c_str(),current?COL_VAL:(index==0?COL_HI:COL_TXT));
      }
      drawSettingsFooter("A  Select       B  Back");
      SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

static const char *installedKindName(CemuInstalledKind kind) {
  switch (kind) {
    case CemuInstalledKind::Base: return "Base game";
    case CemuInstalledKind::Update: return "Update";
    case CemuInstalledKind::Dlc: return "DLC";
  }
  return "Content";
}

static std::string installedSizeText(uint64_t bytes) {
  char text[32];
  if (bytes >= 1024ull * 1024 * 1024)
    snprintf(text, sizeof(text), "%.2f GiB", bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ull * 1024)
    snprintf(text, sizeof(text), "%.1f MiB", bytes / (1024.0 * 1024));
  else if (bytes >= 1024)
    snprintf(text, sizeof(text), "%.1f KiB", bytes / 1024.0);
  else
    snprintf(text, sizeof(text), "%llu B", (unsigned long long)bytes);
  return text;
}

static bool installedContentScreen(uint64_t baseTitleFilter = 0) {
  const std::string mlcRoot = std::string(DATA_DIR) + "/mlc01";
  bool baseDeleted = false;
  int sel = 0, top = 0;
  for (;;) {
    auto components = cemu_scanInstalledComponents(mlcRoot, true);
    if (baseTitleFilter)
      components.erase(std::remove_if(components.begin(), components.end(),
        [&](const auto &component) { return component.baseTitleId != baseTitleFilter; }),
        components.end());
    const int count = (int)components.size();
    const int visible = std::max(1, (SH - 174) / 70);
    sel = count ? std::max(0, std::min(sel, count - 1)) : 0;
    if (sel < top) top = sel;
    if (sel >= top + visible) top = sel - visible + 1;
    bool refresh = false;
    while (!refresh) {
      if (!beginUiFrame()) return baseDeleted;
      SDL_Event event; navRepeat();
      while (pollUiEvent(event)) {
        pumpStick(event);
        int tx = 0, ty = 0;
        TouchKind touch = touchFeed(event, &tx, &ty);
        if (count && touchScrollList(touch, sel, top, count, visible))
          continue;
        if (touch == TOUCH_TAP) {
          if (ty >= SH - 44) return baseDeleted;
          for (int row = 0; row < visible && top + row < count; row++) {
            const int y = 108 + row * 70;
            if (ty >= y && ty < y + 66) {
              sel = top + row;
              SDL_Event press{};
              press.type = SDL_CONTROLLERBUTTONDOWN;
              press.cbutton.button = BTN_CONFIRM;
              SDL_PushEvent(&press);
              break;
            }
          }
          continue;
        }
        if (event.type != SDL_CONTROLLERBUTTONDOWN)
          continue;
        if (event.cbutton.button == BTN_CANCEL)
          return baseDeleted;
        if (!count)
          continue;
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
          sel = (sel + count - 1) % count;
        else if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
          sel = (sel + 1) % count;
        else if (event.cbutton.button == BTN_CONFIRM) {
          const auto component = components[sel];
          const char *actions[] = {"View details", "Delete installed component"};
          const int action = dropdown(installedKindName(component.kind), actions, 2, 0);
          char titleId[32];
          snprintf(titleId, sizeof(titleId), "%016llX",
                   (unsigned long long)component.titleId);
          if (action == 0) {
            modalMessage("Installed component", {
              component.name.empty() ? "Unnamed Wii U title" : component.name,
              std::string(installedKindName(component.kind)) + "  -  " + titleId,
              installedSizeText(component.sizeBytes),
              component.path,
              component.metadataValid ? "Metadata verified" : "Metadata missing or does not match the title ID"
            });
            beginScreenFx();
          } else if (action == 1 && confirmBox(
              (std::string("Delete ") + installedKindName(component.kind) + "?").c_str(), {
                component.name.empty() ? titleId : component.name,
                titleId,
                component.path,
                "",
                "Only this installed component will be deleted.",
                "Saved data is not deleted. This cannot be undone."
              })) {
            std::string error;
            if (!cemu_removeInstalledComponent(mlcRoot, component.titleId, error)) {
              modalMessage("Delete failed", {error, "Saved data was not touched."});
              beginScreenFx();
            } else {
              baseDeleted = baseDeleted || component.kind == CemuInstalledKind::Base;
              g_rescanAfterSettings = true;
              toast("Installed component deleted");
              SDL_Delay(900);
              refresh = true;
            }
          }
        }
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
      }
      if (refresh) break;

      clearUiBackground();
      drawHeader(baseTitleFilter ? "Installed game content" : "Installed content",
                 "Base games, updates, and DLC");
      if (!count) {
        drawTextC(g_font, SW / 2, SH / 2 - 20,
                  baseTitleFilter ? "No installed content for this game" : "No installed content found",
                  COL_DIM);
      }
      for (int row = 0; row < visible && top + row < count; row++) {
        const int index = top + row, y = 108 + row * 70;
        const bool current = index == sel;
        if (current) {
          fillRect(54, y - 3, SW - 108, 66, COL_FOCUS);
          fillRect(54, y - 3, 5, 66, COL_SEL);
        }
        const auto &component = components[index];
        std::string label = std::string("[") + installedKindName(component.kind) + "] " +
                            (component.name.empty() ? "Unnamed Wii U title" : component.name);
        drawText(g_font, 78, y + 2,
                 ellipsizedText(g_font, label, SW - 310).c_str(),
                 current ? COL_VAL : COL_TXT);
        char titleId[32];
        snprintf(titleId, sizeof(titleId), "%016llX",
                 (unsigned long long)component.titleId);
        drawText(g_font_sm, 80, y + 36, titleId,
                 component.metadataValid ? COL_DIM : (SDL_Color){230, 150, 110, 255});
        const std::string size = installedSizeText(component.sizeBytes);
        drawTextR(g_font_sm, SW - 74, y + 22, size.c_str(), current ? COL_VAL : COL_DIM);
      }
      drawSettingsFooter(count ? "A  Details / delete       B  Back" : "B  Back");
      drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
    }
  }
}

static void libraryStorageScreen() {
  static int savedSelection=0;
  constexpr int rowCount=5;
  constexpr int rowHeight=64;
  constexpr int startY=126;
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  size_t installedCount = cemu_scanInstalledComponents(std::string(DATA_DIR) + "/mlc01").size();
  auto openRow=[&](){
    if(sel==0) gameSourcesScreen();
    else if(sel==1) runFileManager();
    else if(sel==2) networkSharesScreen();
    else if(sel==3) installedContentScreen();
    else doInstallFlow();
    installedCount = cemu_scanInstalledComponents(std::string(DATA_DIR) + "/mlc01").size();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<rowCount;row++){
          int y=startY+row*rowHeight;
          if(ty>=y&&ty<y+rowHeight){ sel=row; openRow(); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM) openRow();
      else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    clearUiBackground();
    drawHeader("Library & storage",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,startY-10,colW+24,rowCount*rowHeight+18);
    float target=(float)(startY+sel*rowHeight+2);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowHeight-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowHeight-4,COL_SEL);

    auto shares=loadSmbSharesFromStore();
    size_t mounted=0;
    for(const auto &share:shares) if(SwitchStorage::IsSmbMounted(share.id)) mounted++;
    size_t folderCount=loadGameSources().size();
    std::string folderValue=std::to_string(folderCount)+(folderCount==1?" folder":" folders");
    std::string smbValue=std::to_string(mounted)+" / "+std::to_string(shares.size())+" connected";
    std::string installedValue = std::to_string(installedCount) +
                                 (installedCount == 1 ? " component" : " components");
    const char *labels[rowCount]={"Game folders","File manager","SMB network shares","Installed content","Install update / DLC / game"};
    const char *values[rowCount]={folderValue.c_str(),"SD / USB / SMB",smbValue.c_str(),installedValue.c_str(),"select package or folder"};
    int fontHeight=TTF_FontHeight(g_font),smallHeight=TTF_FontHeight(g_font_sm);
    for(int row=0;row<rowCount;row++){
      int slot=startY+row*rowHeight,y=slot+(rowHeight-fontHeight)/2;
      bool current=row==sel;
      drawText(g_font,labelX,y,labels[row],current?COL_VAL:COL_TXT);
      drawTextR(g_font_sm,valX,slot+(rowHeight-smallHeight)/2,values[row],current?COL_VAL:COL_DIM);
    }
    drawSettingsFooter("A  Open       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

struct ImportedOnlineAccount {
  uint32_t persistentId{};
  std::string label;
};

static bool parsePersistentId(const char *name, uint32_t &persistentId) {
  if (!name || strlen(name) != 8)
    return false;
  uint32_t value = 0;
  for (int i = 0; i < 8; i++) {
    const char c = name[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' :
                      c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                      c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0)
      return false;
    value = (value << 4) | (uint32_t)digit;
  }
  if (value < 0x80000001)
    return false;
  persistentId = value;
  return true;
}

static std::string readAccountLabel(const std::string &path) {
  FILE *file = fopen(path.c_str(), "rb");
  if (!file)
    return {};
  char buffer[65537];
  const size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
  const bool complete = !ferror(file) && feof(file);
  fclose(file);
  if (!complete)
    return {};
  buffer[size] = 0;
  for (const char *key : {"AccountId=", "MiiName="}) {
    const char *value = strstr(buffer, key);
    if (!value)
      continue;
    value += strlen(key);
    const char *end = strpbrk(value, "\r\n");
    const size_t length = std::min<size_t>(end ? (size_t)(end - value) : strlen(value), 32);
    std::string result(value, length);
    if (!result.empty() && std::all_of(result.begin(), result.end(),
        [](unsigned char c) { return c >= 0x20 && c < 0x7f; }))
      return result;
  }
  return {};
}

static std::vector<ImportedOnlineAccount> scanOnlineAccounts() {
  const std::string root = std::string(DATA_DIR) + "/mlc01/usr/save/system/act";
  std::vector<ImportedOnlineAccount> accounts;
  DIR *dir = opendir(root.c_str());
  if (!dir)
    return accounts;
  while (dirent *entry = readdir(dir)) {
    uint32_t persistentId = 0;
    if (!parsePersistentId(entry->d_name, persistentId))
      continue;
    const std::string folder = root + "/" + entry->d_name;
    const std::string accountFile = folder + "/account.dat";
    struct stat folderStat{}, fileStat{};
    if (lstat(folder.c_str(), &folderStat) != 0 || !S_ISDIR(folderStat.st_mode) ||
        S_ISLNK(folderStat.st_mode) || lstat(accountFile.c_str(), &fileStat) != 0 ||
        !S_ISREG(fileStat.st_mode) || S_ISLNK(fileStat.st_mode))
      continue;
    accounts.push_back({persistentId, readAccountLabel(accountFile)});
  }
  closedir(dir);
  std::sort(accounts.begin(), accounts.end(), [](const auto &left, const auto &right) {
    return left.persistentId < right.persistentId;
  });
  return accounts;
}

static const char *credentialState(const std::string &path, off_t expectedSize) {
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0)
    return "Missing";
  return S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode) && st.st_size == expectedSize
           ? "Ready" : "Invalid";
}

static bool directoryHasFiles(const std::string &path) {
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return false;
  bool found = false;
  while (dirent *entry = readdir(dir)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    struct stat st{};
    const std::string child = path + "/" + entry->d_name;
    if (lstat(child.c_str(), &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

static void onlineSettingsScreen() {
  static const char *serviceNames[] = {"Offline", "Nintendo", "Pretendo", "Custom"};
  auto accounts = scanOnlineAccounts();
  uint32_t persistentId = 0x80000001;
  int service = 0;
  if (!cemu_readAccountSelection(SETTINGS_XML, persistentId, service)) {
    modalMessage("Online settings unavailable", {"settings.xml could not be read safely."});
    return;
  }
  auto accountIndex = [&]() {
    for (size_t i = 0; i < accounts.size(); i++)
      if (accounts[i].persistentId == persistentId) return (int)i;
    return -1;
  };
  int sel = 0;
  beginScreenFx();
  for (;;) {
    const int selectedAccount = accountIndex();
    if (!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while (pollUiEvent(event)) {
      pumpStick(event);
      int tx = 0, ty = 0;
      const TouchKind touch = touchFeed(event, &tx, &ty);
      if (touch == TOUCH_TAP) {
        if (ty >= SH - 42) return;
        if (ty >= 116 && ty < 180) sel = 0;
        else if (ty >= 184 && ty < 248) sel = 1;
        else continue;
        SDL_Event press{};
        press.type = SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button = BTN_CONFIRM;
        SDL_PushEvent(&press);
        continue;
      }
      if (event.type != SDL_CONTROLLERBUTTONDOWN)
        continue;
      if (event.cbutton.button == BTN_CANCEL)
        return;
      if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP ||
          event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        sel = 1 - sel;
      else if (event.cbutton.button == BTN_CONFIRM) {
        if (sel == 0) {
          if (accounts.empty()) {
            modalMessage("No imported accounts", {
              "Copy an existing Wii U account.dat into:",
              std::string(DATA_DIR) + "/mlc01/usr/save/system/act/<persistent-id>/"
            });
          } else {
            std::vector<std::string> labels;
            std::vector<const char *> pointers;
            for (const auto &account : accounts) {
              char id[16];
              snprintf(id, sizeof(id), "%08X", account.persistentId);
              labels.push_back(account.label.empty() ? id : account.label + "  (" + id + ")");
            }
            for (const auto &label : labels) pointers.push_back(label.c_str());
            const int choice = dropdown("Imported Wii U account", pointers.data(),
                                        (int)pointers.size(), selectedAccount);
            if (choice >= 0) {
              persistentId = accounts[choice].persistentId;
              cemu_readAccountService(SETTINGS_XML, persistentId, service);
              if (!cemu_writeAccountSelection(SETTINGS_XML, persistentId, service))
                modalMessage("Settings not saved", {"settings.xml could not be updated safely."});
            }
          }
          beginScreenFx();
        } else if (selectedAccount < 0) {
          modalMessage("Select an account first", {"Online services require an imported account.dat."});
          beginScreenFx();
        } else {
          const int choice = dropdown("Network service", serviceNames, 4, service);
          if (choice >= 0) {
            service = choice;
            if (!cemu_writeAccountSelection(SETTINGS_XML, persistentId, service)) {
              modalMessage("Settings not saved", {"settings.xml could not be updated safely."});
            } else if (service == 3) {
              struct stat st{};
              if (lstat((std::string(DATA_DIR) + "/network_services.xml").c_str(), &st) != 0 ||
                  !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
                modalMessage("Custom service needs configuration", {
                  "Add network_services.xml to the Cemu directory.",
                  "The service remains offline until that file is present."
                });
            }
          }
          beginScreenFx();
        }
      }
    }

    clearUiBackground();
    drawHeader("Online & accounts", "Offline / Nintendo / Pretendo / Custom");
    const int panelX = 170, panelW = SW - 340, rowH = 64;
    glassPanel(panelX, 106, panelW, rowH * 2 + 16);
    const int focusY = 116 + sel * 68;
    fillRect(panelX + 10, focusY, panelW - 20, 58, COL_FOCUS);
    fillRect(panelX + 10, focusY, 5, 58, COL_SEL);
    drawText(g_font, panelX + 34, 132, "Account", sel == 0 ? COL_VAL : COL_TXT);
    std::string accountValue = "None imported";
    if (selectedAccount >= 0) {
      char id[16]; snprintf(id, sizeof(id), "%08X", persistentId);
      accountValue = accounts[selectedAccount].label.empty() ? id : accounts[selectedAccount].label + "  (" + id + ")";
    } else if (!accounts.empty()) {
      char id[32]; snprintf(id, sizeof(id), "%08X (account.dat missing)", persistentId);
      accountValue = id;
    }
    drawTextR(g_font_sm, panelX + panelW - 28, 137,
              ellipsizedText(g_font_sm, accountValue, panelW / 2).c_str(),
              sel == 0 ? COL_VAL : COL_DIM);
    drawText(g_font, panelX + 34, 200, "Service", sel == 1 ? COL_VAL : COL_TXT);
    drawTextR(g_font_sm, panelX + panelW - 28, 205,
              selectedAccount >= 0 ? serviceNames[service] : "Select an account",
              sel == 1 ? COL_VAL : COL_DIM);

    const std::string certRoot = std::string(DATA_DIR) +
      "/mlc01/sys/title/0005001b/10054000/content";
    const bool certsPresent = directoryHasFiles(certRoot + "/ccerts") &&
                              directoryHasFiles(certRoot + "/scerts");
    const char *otp = credentialState(std::string(DATA_DIR) + "/otp.bin", 1024);
    const char *seeprom = credentialState(std::string(DATA_DIR) + "/seeprom.bin", 512);
    struct stat customConfigStat{};
    const std::string customConfigPath = std::string(DATA_DIR) + "/network_services.xml";
    const bool customConfig = lstat(customConfigPath.c_str(), &customConfigStat) == 0 &&
                              S_ISREG(customConfigStat.st_mode) && !S_ISLNK(customConfigStat.st_mode);
    const int statusX = panelX + 28, statusY = 306, statusStep = 43;
    drawText(g_font, statusX, statusY - 52, "Credential readiness", COL_HI);
    const std::string statusLines[] = {
      std::string("Selected account.dat: ") + (selectedAccount >= 0 ? "Ready" : "Missing"),
      std::string("otp.bin (1024 bytes): ") + otp,
      std::string("seeprom.bin (512 bytes): ") + seeprom,
      std::string("Wii U certificate store: ") + (certsPresent ? "Present" : "Missing / incomplete"),
      std::string("Custom service XML: ") + (customConfig ? "Ready" : "Missing")
    };
    for (int i = 0; i < 5; i++) {
      const bool ready = statusLines[i].find("Ready") != std::string::npos ||
                         statusLines[i].find("Present") != std::string::npos;
      drawText(g_font_sm, statusX, statusY + i * statusStep, statusLines[i].c_str(),
               ready ? (SDL_Color){145, 220, 155, 255} : (SDL_Color){235, 145, 120, 255});
    }
    drawTextC(g_font_sm, SW / 2, SH - 76,
              "Selecting a service does not create credentials or guarantee server access.", COL_DIM);
    drawSettingsFooter("A  Change       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void runSettingsRoot(const char *ctx) {
  static const int order[] = { SCR_FRAMEGEN, SCR_CPU, SCR_GRAPHICS, SCR_AUDIO, SCR_OVERLAY, SCR_INPUT, SCR_ACCESSORIES };
  const int nscr=(int)(sizeof(order)/sizeof(*order));
  bool global = !(ctx && *ctx);
  int launcherRow=0,libraryRow=1,onlineRow=2,screenStart=3,langRow=screenStart+nscr,gfxRow=langRow+1;
  int n=nscr+(global?5:0),sel=0,top=0;
  const int rowH=global?54:58,y0=92,sectionGap=34,vis=std::max(1,(SH-y0-42-sectionGap)/rowH);
  auto rowY=[&](int index){ return y0+(index-top)*rowH+(global&&index>=screenStart?sectionGap:0); };
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          for(int row=0;row<vis&&top+row<n;row++){ int index=top+row,y=rowY(index); if(ty>=y && ty<y+rowH){ sel=index;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CONFIRM:
          if(global && sel==launcherRow){ launcherSettingsScreen(); }
          else if(global && sel==libraryRow){ libraryStorageScreen(); }
          else if(global && sel==onlineRow){ onlineSettingsScreen(); }
          else if(global && sel==langRow){ optChoosePopup(O_console_language); }
          else if(global && sel==gfxRow){ gfxPackScreen(0); }
          else runSettings(order[global?sel-screenStart:sel],ctx);
          beginScreenFx();
          break;
        case BTN_CANCEL: return;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
    }
    clearUiBackground();
    drawHeader(global ? "Settings" : "Game settings", global?nullptr:ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    int shown=std::min(vis,n);
    if(global){
      glassPanel(colX-12,y0-10,colW+24,3*rowH+18);
      glassPanel(colX-12,y0+3*rowH+sectionGap-10,colW+24,(shown-3)*rowH+18);
    } else {
      glassPanel(colX-12,y0-10,colW+24,shown*rowH+18);
    }
    int fh0=TTF_FontHeight(g_font);
    float ty=(float)(rowY(sel)+2);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-4,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowH-4,COL_SEL);
    for(int row=0;row<vis&&top+row<n;row++){ int i=top+row,slot=rowY(i),y=slot+(rowH-fh0)/2; bool cur=i==sel;
      if(global && i==langRow){
        char lv[64]; optValue(O_console_language, lv, sizeof(lv));
        drawText(g_font,labelX,y,"Console language",cur?COL_VAL:COL_TXT);
        drawTextR(g_font,valX,y,lv,cur?COL_VAL:COL_DIM);
      } else if(global && i==gfxRow){
        drawText(g_font,labelX,y,"Graphics packs",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"browse / download",cur?COL_VAL:COL_DIM);
      } else if(global && i==launcherRow){
        const char *theme=storeGet(g_global,"Wrapper/Theme","homebrew");
        const char *value=!strcmp(theme,"xmb")?"XMB (PS3)":(!strcmp(theme,"animated")?"Glow":
                          (!strcmp(theme,"classic")?"Classic":(!strcmp(theme,"oled")?"OLED black":"Bubbles")));
        drawText(g_font,labelX,y,"Launcher",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,value,cur?COL_VAL:COL_DIM);
      } else if(global && i==libraryRow){
        drawText(g_font,labelX,y,"Library & storage",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"games / files / network",cur?COL_VAL:COL_DIM);
      } else if(global && i==onlineRow){
        drawText(g_font,labelX,y,"Online & accounts",cur?COL_VAL:COL_TXT);
        drawTextR(g_font_sm,valX,slot+(rowH-TTF_FontHeight(g_font_sm))/2,"Offline / Nintendo / Pretendo",cur?COL_VAL:COL_DIM);
      } else {
        drawText(g_font,labelX,y,g_screens[order[global?i-screenStart:i]].title,cur?COL_VAL:COL_TXT);
        drawTextR(g_font,valX,y,">",cur?COL_VAL:COL_DIM);
      }
    }
    if(n>vis){ int trackH=vis*rowH,trackX=colX+colW+16; fillRect(trackX,y0,4,trackH,(SDL_Color){40,44,54,255});
      int thumbH=std::max(16,trackH*vis/n),denom=std::max(1,n-vis); fillRect(trackX,y0+(trackH-thumbH)*top/denom,4,thumbH,COL_SEL); }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static void toast(const char *msg) {
  for (int f = 0; f < 2; f++) {
    clearUiBackground();
    int pw=820,ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph); border(px,py,pw,ph,2,COL_HI);
    drawTextC(g_font,SW/2,py+46,msg,COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(10);
  }
}

static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    int pw = SW*3/4, ph = 150 + (int)lines.size()*40, px = (SW-pw)/2, py = (SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+34, title, COL_SEL);
    int y = py+108;
    for (auto &l : lines) { drawTextC(g_font, SW/2, y, l.c_str(), COL_TXT); y += 40; }
    drawTextC(g_font_sm, SW/2, py+ph-42, "Press A to continue", COL_DIM);
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=SW*3/4, ph=200+(int)lines.size()*40, px=(SW-pw)/2, py=(SH-ph)/2;
  int bw=210, bh=56, bby=py+ph-bh-22, yesx=SW/2-bw-18, nox=SW/2+18;
  for(;;){
    if (!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_QUIT) return false;
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,(SDL_Color){210,70,70,255});
    drawTextC(g_font_big,SW/2,py+34,title,(SDL_Color){235,120,120,255});
    int y=py+112; for(auto&l:lines){ drawTextC(g_font,SW/2,y,l.c_str(),COL_TXT); y+=40; }
    int fh=TTF_FontHeight(g_font);
    fillRect(yesx,bby,bw,bh,(SDL_Color){150,50,50,255}); border(yesx,bby,bw,bh,2,(SDL_Color){215,95,95,255});
    drawTextC(g_font,yesx+bw/2,bby+(bh-fh)/2,"Yes  (A)",COL_TXT);
    fillRect(nox,bby,bw,bh,(SDL_Color){48,54,64,255}); border(nox,bby,bw,bh,2,COL_DIM);
    drawTextC(g_font,nox+bw/2,bby+(bh-fh)/2,"No  (B)",COL_TXT);
    SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static std::string installedReleaseTag() {
  const std::string built=LauncherUpdate_BuiltReleaseTag();
#if defined(CEMU_SWITCH_UPDATE_TEST)
  return built;
#else
  const std::string stored=storeGet(g_global,"Wrapper/InstalledReleaseTag","");
  if(stored.empty()) return built;
  return LauncherUpdate_IsNewer(stored,built)?stored:built;
#endif
}

static std::vector<size_t> utf8Boundaries(const std::string &text) {
  std::vector<size_t> boundaries{0};
  for(size_t index=0;index<text.size();){
    const unsigned char lead=(unsigned char)text[index];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(index+length>text.size()) length=1;
    for(size_t part=1;part<length;part++) if(((unsigned char)text[index+part]&0xc0)!=0x80){ length=1; break; }
    index+=length;
    boundaries.push_back(index);
  }
  return boundaries;
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth) {
  std::vector<std::string> lines;
  size_t paragraphStart=0;
  while(paragraphStart<=notes.size()){
    size_t paragraphEnd=notes.find('\n',paragraphStart);
    if(paragraphEnd==std::string::npos) paragraphEnd=notes.size();
    std::string paragraph=notes.substr(paragraphStart,paragraphEnd-paragraphStart);
    if(!paragraph.empty()&&paragraph.back()=='\r') paragraph.pop_back();
    for(char &value:paragraph) if(value=='\t'||(unsigned char)value<0x20) value=' ';
    while(!paragraph.empty()&&paragraph.back()==' ') paragraph.pop_back();
    if(paragraph.empty()) lines.emplace_back();
    else {
      bool continuation=false;
      while(!paragraph.empty()){
        while(!paragraph.empty()&&paragraph.front()==' ') paragraph.erase(paragraph.begin());
        if(paragraph.empty()) break;
        const std::string prefix=continuation&&paragraph.rfind("- ",0)!=0?"  ":"";
        if(textW(g_font_sm,(prefix+paragraph).c_str())<=maxWidth){ lines.push_back(prefix+paragraph); break; }
        const auto boundaries=utf8Boundaries(paragraph);
        size_t low=1,high=boundaries.size()-1;
        while(low<high){
          size_t middle=(low+high+1)/2;
          if(textW(g_font_sm,(prefix+paragraph.substr(0,boundaries[middle])).c_str())<=maxWidth) low=middle;
          else high=middle-1;
        }
        size_t split=boundaries[low];
        size_t space=paragraph.rfind(' ',split);
        if(space!=std::string::npos&&space>0&&space>=split/3) split=space;
        lines.push_back(prefix+paragraph.substr(0,split));
        paragraph.erase(0,split);
        continuation=true;
      }
    }
    if(paragraphEnd==notes.size()) break;
    paragraphStart=paragraphEnd+1;
  }
  if(lines.empty()) lines.emplace_back("No release notes were provided.");
  return lines;
}

static void requestLauncherExitAfterUpdate() {
  g_exitRequested=true;
}

static void runUpdateScreen() {
  if(!g_griddbReady){
    modalMessage("Update check unavailable",{
      "The launcher could not initialize its network connection.",
      "Check the connection and try again."
    });
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle)
    LauncherUpdate_StartCheck(installedReleaseTag());

  int scroll=0;
  bool cancelRequested=false,installedSaved=false;
  std::string wrappedTag,wrappedBody;
  std::vector<std::string> wrappedLines;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){
      LauncherUpdate_Cancel();
      return;
    }
    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    if(snapshot.state==LauncherUpdateState::ReadyToInstall){
      if(g_romfsReady){ romfsExit(); g_romfsReady=false; }
      LauncherUpdate_InstallDownloaded(g_launcherNroPath);
      snapshot=LauncherUpdate_GetSnapshot();
      if(snapshot.state==LauncherUpdateState::Error&&!g_romfsReady&&R_SUCCEEDED(romfsInit()))
        g_romfsReady=true;
    }
    if(snapshot.state==LauncherUpdateState::Installed&&!installedSaved){
      storeSet(g_global,"Wrapper/InstalledReleaseTag",snapshot.release.tag.c_str());
      storeSave(g_global,LAUNCHER_INI);
      g_updateNoticeTag.clear();
      installedSaved=true;
    }

    const int panelWidth=SW*7/8,panelHeight=SH*4/5;
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    const int bodyX=panelX+42,bodyY=panelY+126,bodyWidth=panelWidth-84;
    const int footerHeight=108,bodyBottom=panelY+panelHeight-footerHeight;
    const int lineHeight=TTF_FontHeight(g_font_sm)+8;
    const int visibleLines=std::max(1,(bodyBottom-bodyY)/lineHeight);
    if(snapshot.release.tag!=wrappedTag||snapshot.release.notes!=wrappedBody){
      wrappedTag=snapshot.release.tag;
      wrappedBody=snapshot.release.notes;
      wrappedLines=wrapReleaseNotes(wrappedBody.empty()?"Release notes will appear here.":wrappedBody,bodyWidth-20);
      scroll=0;
    }
    const int maxScroll=std::max(0,(int)wrappedLines.size()-visibleLines);
    scroll=std::max(0,std::min(scroll,maxScroll));

    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SCROLL_UP) scroll=std::min(maxScroll,scroll+std::max(1,g_touchScrollSteps));
      else if(touch==TOUCH_SCROLL_DOWN) scroll=std::max(0,scroll-std::max(1,g_touchScrollSteps));
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) scroll=std::max(0,scroll-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) scroll=std::min(maxScroll,scroll+1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) scroll=std::max(0,scroll-visibleLines);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) scroll=std::min(maxScroll,scroll+visibleLines);
      else if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Downloading){ LauncherUpdate_Cancel(); cancelRequested=true; }
        else if(snapshot.state!=LauncherUpdateState::Installed) return;
      } else if(event.cbutton.button==BTN_CONFIRM){
        if(snapshot.state==LauncherUpdateState::UpdateAvailable){
          if(LauncherUpdate_StartDownload(g_launcherNroPath)) cancelRequested=false;
        } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled){
          cancelRequested=false;
          LauncherUpdate_StartCheck(installedReleaseTag());
        } else if(snapshot.state==LauncherUpdateState::Installed){
          requestLauncherExitAfterUpdate();
          return;
        }
      }
    }

    snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();
    fillRect(0,0,SW,SH,(SDL_Color){0,0,0,105});
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawTextC(g_font_big,SW/2,panelY+24,"Cemu Update",COL_SEL);

    std::string status;
    switch(snapshot.state){
      case LauncherUpdateState::Idle: status="Ready to check for updates"; break;
      case LauncherUpdateState::Checking: status="Checking GitHub for the latest release..."; break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::Downloading:
        status=cancelRequested?"Cancelling download...":"Downloading "+snapshot.release.assetName; break;
      case LauncherUpdateState::ReadyToInstall: status="Preparing installation..."; break;
      case LauncherUpdateState::Installing: status="Installing update..."; break;
      case LauncherUpdateState::Installed: status="Update installed successfully - relaunch Cemu manually"; break;
      case LauncherUpdateState::Cancelled: status="Update cancelled"; break;
      case LauncherUpdateState::Error: status=snapshot.error.empty()?"Update failed":snapshot.error; break;
    }
    drawScrollTextL(g_font_sm,bodyX,panelY+92,bodyWidth,status.c_str(),
      snapshot.state==LauncherUpdateState::Error?(SDL_Color){235,125,115,255}:COL_VAL);

    SDL_Rect clip={bodyX,bodyY,bodyWidth,bodyBottom-bodyY};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int row=0;row<visibleLines&&scroll+row<(int)wrappedLines.size();row++)
      drawText(g_font_sm,bodyX,bodyY+row*lineHeight,wrappedLines[scroll+row].c_str(),COL_TXT);
    SDL_RenderSetClipRect(g_ren,nullptr);
    if((int)wrappedLines.size()>visibleLines){
      const int trackX=panelX+panelWidth-25,trackHeight=bodyBottom-bodyY;
      fillRect(trackX,bodyY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=std::max(18,trackHeight*visibleLines/(int)wrappedLines.size());
      fillRect(trackX,bodyY+(trackHeight-thumbHeight)*scroll/std::max(1,maxScroll),4,thumbHeight,COL_SEL);
    }

    if(snapshot.state==LauncherUpdateState::Downloading){
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const int percent=total?(int)std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      const int barX=bodyX,barY=panelY+panelHeight-82,barWidth=bodyWidth,barHeight=24;
      border(barX,barY,barWidth,barHeight,2,COL_SEL);
      fillRect(barX+3,barY+3,(barWidth-6)*percent/100,barHeight-6,COL_HI);
      char progress[96];
      snprintf(progress,sizeof(progress),"%d%%    %.1f / %.1f MiB",percent,
        snapshot.downloaded/(1024.0*1024.0),total/(1024.0*1024.0));
      drawTextC(g_font_sm,SW/2,barY+30,progress,COL_DIM);
    } else {
      const char *controls="B  Back       Up / Down  Scroll       L / R  Page";
      if(snapshot.state==LauncherUpdateState::UpdateAvailable) controls="A  Download       B  Back       Up / Down  Scroll";
      else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled) controls="A  Retry       B  Back";
      else if(snapshot.state==LauncherUpdateState::Installed) controls="A  Exit Cemu";
      drawSettingsFooter(controls,panelY+panelHeight-38);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static void pollUpdateNotification() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&
     snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification() {
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil)){
    g_updateNoticeTag.clear();
    return;
  }
  const int width=std::min(540,SW-40),height=92,x=SW-width-24,y=SH-height-58;
  glassPanel(x,y,width,height);
  border(x,y,width,height,2,COL_SEL);
  const std::string title="Cemu "+g_updateNoticeTag+" is available";
  drawText(g_font,x+22,y+16,ellipsizedText(g_font,title,width-44).c_str(),COL_VAL);
  drawText(g_font_sm,x+22,y+54,"Open Settings > Launcher > Check for Updates",COL_TXT);
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return "No matching artwork was found.";
  return "SteamGridDB returned an unexpected error.";
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName) {
  if(artworks.empty()) return -1;
  const int listX=56,listWidth=SW/2-78,rowHeight=52,startY=116;
  const int previewX=SW/2+28,previewAreaWidth=SW-previewX-56;
  const int previewHeight=std::min(SH-210,SW>=1600?720:510);
  const int previewWidth=previewHeight*2/3;
  const int visible=std::max(1,(SH-startY-72)/rowHeight);
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;

  auto releasePreview=[&](){
    if(preview) SDL_DestroyTexture(preview);
    preview=nullptr;
    remove(temporary.c_str());
  };
  auto loadPreview=[&](int index){
    releasePreview();
    loaded=index; previewFailed=false;
    clearUiBackground();
    drawHeader("Choose cover artwork",gameName);
    drawTextC(g_font,previewX+previewAreaWidth/2,SH/2-18,"Loading preview...",COL_DIM);
    SDL_RenderPresent(g_ren);
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    if(griddb_download_image(url,temporary)==GRIDDB_OK)
      preview=loadScaledTexture(temporary,previewWidth,previewHeight);
    previewFailed=preview==nullptr;
    remove(temporary.c_str());
    beginScreenFx();
  };

  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      int touchSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){
        if(sel!=touchSelection) loadPreview(sel);
        continue;
      }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth){
          for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
            int y=startY+row*rowHeight;
            if(ty>=y&&ty<y+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
          }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }

    clearUiBackground();
    drawHeader("Choose cover artwork",gameName);
    glassPanel(listX-10,startY-10,listWidth+20,std::min(visible,(int)artworks.size())*rowHeight+18);
    for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
      int index=top+row,y=startY+row*rowHeight,currentY=y+(rowHeight-TTF_FontHeight(g_font))/2;
      bool current=index==sel;
      if(current){ fillRect(listX,y,listWidth,rowHeight-3,COL_FOCUS); fillRect(listX,y,5,rowHeight-3,COL_SEL); }
      std::string label="Artwork "+std::to_string(index+1);
      drawText(g_font,listX+26,currentY,label.c_str(),current?COL_VAL:COL_TXT);
      if(artworks[index].width>0&&artworks[index].height>0){
        std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
        drawTextR(g_font_sm,listX+listWidth-20,currentY+(TTF_FontHeight(g_font)-TTF_FontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
      }
    }
    int imageX=previewX+(previewAreaWidth-previewWidth)/2,imageY=startY;
    fillRect(imageX,imageY,previewWidth,previewHeight,COL_CARD);
    if(loaded==sel&&preview){ SDL_Rect destination={imageX,imageY,previewWidth,previewHeight}; SDL_RenderCopy(g_ren,preview,nullptr,&destination); }
    else if(loaded==sel&&previewFailed) drawTextC(g_font_sm,imageX+previewWidth/2,imageY+previewHeight/2,"Preview unavailable",COL_DIM);
    border(imageX,imageY,previewWidth,previewHeight,2,loaded==sel?COL_SEL:COL_DIM);
    drawSettingsFooter("A  Use artwork       B  Back");
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void downloadCover(Game &g) {
  std::string key=steamGridDbKey();
  if (key.empty()) {
    if(promptAndSaveSteamGridDbKey("Enter your free SteamGridDB API key",false)) key=steamGridDbKey();
    else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  mkdir(COVERS_DIR, 0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    toast("Searching SteamGridDB...");
    std::vector<GridDbGameResult> matches;
    int searchResult=griddb_search_games(key,query,matches);
    if(searchResult==GRIDDB_NO_KEY){
      if(!promptAndSaveSteamGridDbKey("SteamGridDB API key rejected",false)) return;
      key=steamGridDbKey();
      continue;
    }
    if(searchResult!=GRIDDB_OK&&searchResult!=GRIDDB_NOT_FOUND){
      modalMessage("Cover search failed",{gridDbErrorText(searchResult)});
      return;
    }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names;
    names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdown("Choose matching title",names.data(),(int)names.size(),-1);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptText("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom);
      if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1];
    break;
  }

  toast("Loading available artwork...");
  std::vector<GridDbArtwork> artworks;
  int result=griddb_fetch_artworks(key,selectedGame.id,artworks);
  if(result!=GRIDDB_OK){ modalMessage("Artwork search failed",{gridDbErrorText(result)}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;

  toast("Downloading selected cover...");
  result=griddb_download_image(artworks[artworkIndex].url,coverPath(g));
  if(result==GRIDDB_OK){ reloadCover(g); toast("Cover downloaded"); }
  else toast("Cover download failed");
  SDL_Delay(1200);
}

static void downloadAllCovers() {
  std::string key=steamGridDbKey();
  if (key.empty()) {
    if(promptAndSaveSteamGridDbKey("Enter your free SteamGridDB API key",false)) key=steamGridDbKey();
    else { toast("A SteamGridDB API key is required"); SDL_Delay(1200); return; }
  }
  mkdir(COVERS_DIR, 0777);
  std::vector<int> pending;
  for (int i=0;i<(int)g_games.size();i++) if(!regularFileExists(existingCoverPath(g_games[i]))) pending.push_back(i);
  if (pending.empty()) { toast("All covers already downloaded"); SDL_Delay(1200); return; }
  int total=(int)pending.size(), done=0, ok=0, fail=0; bool cancel=false;
  for (int k=0;k<total && !cancel;k++){
    if (!beginUiFrame()) return;
    Game &g = g_games[pending[k]];
    SDL_Event e; while(pollUiEvent(e)){ pumpStick(e);
      if(e.type==SDL_CONTROLLERBUTTONDOWN && e.cbutton.button==BTN_CANCEL) cancel=true;
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=SH-90) cancel=true; }
    }
    if(cancel) break;
    clearUiBackground();
    drawHeader("Download all covers", nullptr);
    drawTextC(g_font, SW/2, SH/2-96, ("Downloading  "+std::to_string(done+1)+" / "+std::to_string(total)).c_str(), COL_VAL);
    drawTitleCell(SW/2, SW-260, SH/2-44, g.title, true, COL_TXT);
    int bw=SW-360, bx=180, by=SH/2+16, bh=26;
    fillRect(bx,by,bw,bh,(SDL_Color){40,44,54,255}); border(bx,by,bw,bh,2,COL_DIM);
    fillRect(bx,by, total?bw*done/total:0, bh, COL_SEL);
    char st[64]; snprintf(st,sizeof(st),"%d downloaded    %d failed",ok,fail);
    drawTextC(g_font_sm, SW/2, by+46, st, COL_DIM);
    SDL_RenderPresent(g_ren);
    int rc=GRIDDB_NO_KEY;
    for(;;){
      rc=griddb_fetch_cover(key,g.title,coverPath(g));
      if(rc!=GRIDDB_NO_KEY) break;
      if(!promptAndSaveSteamGridDbKey("SteamGridDB API key rejected",false)){ cancel=true; break; }
      key=steamGridDbKey();
    }
    if(cancel) break;
    if(rc==GRIDDB_OK){ ok++; reloadCover(g); } else fail++;
    done++;
  }
  char msg[96]; snprintf(msg,sizeof(msg),"Covers: %d downloaded, %d failed%s",ok,fail,cancel?" (cancelled)":"");
  toast(msg); SDL_Delay(1600);
}

static bool writeEnabledPacks(const std::vector<CemuGraphicPack> &enabled) {
  std::string text;
  auto valid = [](const std::string &value) { return value.find_first_of("\t\r\n") == std::string::npos; };
  for (auto &pk : enabled) {
    if (pk.rulesRel.empty() || !valid(pk.rulesRel)) return false;
    text += pk.rulesRel;
    for (auto &pr : pk.presets) {
      if (!valid(pr.category) || pr.category.find('=') != std::string::npos || !valid(pr.preset)) return false;
      text += '\t';
      text += pr.category;
      text += '=';
      text += pr.preset;
    }
    text += '\n';
  }
  return writeAtomicText(ENABLED_PACKS_FILE, text);
}

struct PackSel { bool enabled = false; std::vector<std::pair<std::string, std::string>> choices; };

static std::vector<std::string> gfxSplitPath(const std::string &s) {
  std::vector<std::string> t; size_t a = 0;
  while (a <= s.size()) { size_t sl = s.find('/', a); std::string x = s.substr(a, sl == std::string::npos ? s.size() - a : sl - a);
    if (!x.empty()) t.push_back(x);
    if (sl == std::string::npos) break;
    a = sl + 1;
  }
  return t;
}
static std::string gfxGameOf(const GfxPack &p) { auto t = gfxSplitPath(p.path); return t.empty() ? p.name : t[0]; }
static void gfxCatLeaf(const GfxPack &p, std::string &cat, std::string &leaf) {
  auto t = gfxSplitPath(p.path);
  if (t.size() >= 3) { cat = t[1]; leaf.clear(); for (size_t i = 2; i < t.size(); i++) { if (!leaf.empty()) leaf += " / "; leaf += t[i]; } }
  else if (t.size() == 2) { cat = "General"; leaf = t[1]; }
  else { cat = "General"; leaf = t.empty() ? p.name : t[0]; }
  if (leaf.empty()) leaf = p.name;
}
struct GRow { bool header; std::string label; int packIndex; };
static std::vector<GRow> gfxBuildGameRows(const std::vector<GfxPack> &packs, const std::string &gameName, bool perGame) {
  struct It { std::string cat, leaf; int idx; };
  std::vector<It> items;
  for (int i = 0; i < (int)packs.size(); i++) {
    if (!perGame && gfxGameOf(packs[i]) != gameName) continue;
    std::string cat, leaf; gfxCatLeaf(packs[i], cat, leaf);
    items.push_back({cat, leaf, i});
  }
  std::sort(items.begin(), items.end(), [](const It &a, const It &b) { return a.cat != b.cat ? a.cat < b.cat : a.leaf < b.leaf; });
  std::vector<GRow> rows; std::string curCat = "\x01";
  for (auto &it : items) { if (it.cat != curCat) { curCat = it.cat; rows.push_back({true, curCat, -1}); } rows.push_back({false, it.leaf, it.idx}); }
  return rows;
}
static void drawWrapped(TTF_Font *f, int x, int y, int maxW, int lh, int maxLines, const char *s, SDL_Color c) {
  if (!s || !*s) return;
  std::string text = s, line; int drawn = 0;
  auto emit = [&](const std::string &ln) { if (drawn < maxLines) { drawText(f, x, y + drawn * lh, ln.c_str(), c); drawn++; } };
  size_t i = 0;
  while (i < text.size() && drawn < maxLines) {
    size_t ws = i; while (ws < text.size() && text[ws] != ' ' && text[ws] != '\n') ws++;
    std::string word = text.substr(i, ws - i);
    std::string candidate = line.empty() ? word : line + " " + word;
    if (textW(f, candidate.c_str()) > maxW && !line.empty()) { emit(line); line = word; }
    else line = candidate;
    if (ws < text.size() && text[ws] == '\n') { emit(line); line.clear(); }
    i = ws + 1;
  }
  if (drawn < maxLines && !line.empty()) emit(line);
}
struct GfxCat { std::string category; std::vector<int> presetIdx; };
static std::vector<GfxCat> gfxGroupPresets(const GfxPack &p) {
  std::vector<GfxCat> cats;
  for (int i = 0; i < (int)p.presets.size(); i++) { const std::string &c = p.presets[i].category; int ci = -1;
    for (int k = 0; k < (int)cats.size(); k++) if (cats[k].category == c) { ci = k; break; }
    if (ci < 0) { cats.push_back({c, {}}); ci = (int)cats.size() - 1; } cats[ci].presetIdx.push_back(i); }
  return cats;
}
static std::string gfxCatDefault(const GfxPack &p, const GfxCat &c) {
  for (int i : c.presetIdx) if (p.presets[i].isDefault) return p.presets[i].name;
  return c.presetIdx.empty() ? "" : p.presets[c.presetIdx[0]].name;
}

static int downloadLatestGraphicPacks() {
  toast("Downloading latest graphic packs...");
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  const int result=gfxpacks_downloadLatest(GRAPHICPACKS_DIR);
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  toast(result==GFX_OK?"Packs updated":result==GFX_UPTODATE?"Already up to date":
        result==GFX_NET_FAIL?"Download failed (network)":"Extract failed");
  SDL_Delay(1300);
  return result;
}

static void openPackPanel(const GfxPack &p, PackSel &s) {
  auto cats = gfxGroupPresets(p);
  auto choiceRef = [&](const std::string &cat) -> std::string & {
    for (auto &ch : s.choices) if (ch.first == cat) return ch.second;
    s.choices.push_back({cat, ""}); return s.choices.back().second;
  };
  for (auto &c : cats) { auto &ref = choiceRef(c.category); if (ref.empty()) ref = gfxCatDefault(p, c); }
  int row = 0, top = 0, nrows = 1 + (int)cats.size();
  bool hasDesc = !p.description.empty();
  int footerH = hasDesc ? 120 : 0;
  int vis = (SH - LIST_Y0 - 40 - footerH) / ROW_H; if (vis < 1) vis = 1;
  beginScreenFx();
  for (;;) {
    if (!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touchScrollList(touch,row,top,nrows,vis)) continue;
        if(touch==TOUCH_TAP){
          for(int visibleRow=0;visibleRow<vis&&top+visibleRow<nrows;visibleRow++){
            int y=LIST_Y0+visibleRow*ROW_H;
            if(ty>=y&&ty<y+ROW_H){ row=top+visibleRow; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; }
          }
          continue;
        }
      }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   row = (row + nrows - 1) % nrows; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: row = (row + 1) % nrows; break;
        case BTN_CONFIRM:
          if (row == 0) s.enabled = !s.enabled;
          else if (s.enabled) { const GfxCat &c = cats[row - 1]; auto &cur = choiceRef(c.category);
            std::vector<const char *> labels; int ci = 0;
            for (int k = 0; k < (int)c.presetIdx.size(); k++) { const std::string &nm = p.presets[c.presetIdx[k]].name; labels.push_back(nm.c_str()); if (nm == cur) ci = k; }
            int pick = dropdown(c.category.empty() ? "Options" : c.category.c_str(), labels.data(), (int)labels.size(), ci);
            if (pick >= 0 && pick < (int)c.presetIdx.size()) cur = p.presets[c.presetIdx[pick]].name;
            beginScreenFx();
          }
          break;
        case BTN_CANCEL: return;
      }
      if (row < top) top = row;
      if (row >= top + vis) top = row - vis + 1;
      if (top < 0) top = 0;
    }
    clearUiBackground();
    drawHeader(p.name.c_str(), p.path.c_str());
    int colX, colW, labelX, valX; listCol(&colX, &colW, &labelX, &valX);
    int fh0 = TTF_FontHeight(g_font);
    float ty = (float)(LIST_Y0 + (row - top) * ROW_H + 1);
    g_hy = (!g_uiAnimations || g_hy < 0) ? ty : g_hy + (ty - g_hy) * 0.30f;
    fillRect(colX, (int)g_hy, colW, ROW_H - 2, COL_FOCUS);
    fillRect(colX, (int)g_hy, 5, ROW_H - 2, COL_SEL);
    SDL_Color grey = {92, 98, 110, 255};
    for (int r = 0; r < vis && top + r < nrows; r++) {
      int i = top + r, y = LIST_Y0 + r * ROW_H + (ROW_H - fh0) / 2; bool cur = (i == row);
      if (i == 0) { drawText(g_font, labelX, y, "Enabled", cur ? COL_VAL : COL_TXT);
        SDL_Color oc = s.enabled ? (SDL_Color){120, 220, 120, 255} : COL_DIM; drawTextR(g_font, valX, y, s.enabled ? "On" : "Off", oc); }
      else { const GfxCat &c = cats[i - 1]; bool en = s.enabled;
        drawText(g_font, labelX, y, c.category.empty() ? "Options" : c.category.c_str(), en ? (cur ? COL_VAL : COL_TXT) : grey);
        drawScrollTextR(g_font, valX, y, colW / 2 - 24, choiceRef(c.category).c_str(), en ? (cur ? COL_VAL : COL_DIM) : grey); }
    }
    if (nrows > vis) { int trH = vis * ROW_H, trX = colX + colW + 16, trY = LIST_Y0 - 2; fillRect(trX, trY, 4, trH, (SDL_Color){40, 44, 54, 255});
      int thH = trH * vis / nrows, dn = (nrows - vis > 0 ? nrows - vis : 1); fillRect(trX, trY + (trH - thH) * top / dn, 4, thH, COL_SEL); }
    if (hasDesc) {
      int fy = SH - footerH + 6; fillRect(colX, fy - 12, colW, 2, (SDL_Color){40, 44, 54, 255});
      drawWrapped(g_font_sm, colX + 6, fy, colW - 12, TTF_FontHeight(g_font_sm) + 4, 3, p.description.c_str(), COL_DIM);
    }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static void gfxPackScreen(uint64_t filterTitleId) {
  bool perGame = (filterTitleId != 0);
  auto load = [&](std::vector<GfxPack> &packs) {
    packs = gfxpacks_enumerate(GRAPHICPACKS_DIR);
    if (perGame) { std::vector<GfxPack> f;
      for (auto &p : packs) { bool m = p.universal; for (auto id : p.titleIds) if (id == filterTitleId) m = true; if (m) f.push_back(p); }
      packs.swap(f); }
  };
  std::vector<GfxPack> packs; load(packs);
  std::vector<CemuGraphicPack> enabledBefore;
  const bool enabledLoaded = readEnabledPacks(enabledBefore);
  std::map<std::string, PackSel> selByRel;
  for (auto &pk : enabledBefore) { PackSel s; s.enabled = true; for (auto &pr : pk.presets) s.choices.push_back({pr.category, pr.preset}); selByRel[pk.rulesRel] = s; }
  auto commit = [&]() -> bool {
    if (!enabledLoaded) return false;
    std::vector<CemuGraphicPack> out = perGame ? enabledBefore : std::vector<CemuGraphicPack>{};
    for (auto &p : packs) { auto it = selByRel.find(p.rulesRel);
      if (perGame) out.erase(std::remove_if(out.begin(), out.end(), [&](const CemuGraphicPack &pk) { return pk.rulesRel == p.rulesRel; }), out.end());
      if (it != selByRel.end() && it->second.enabled) { CemuGraphicPack g; g.rulesRel = p.rulesRel;
        for (auto &ch : it->second.choices) g.presets.push_back({ch.first, ch.second});
        out.push_back(std::move(g));
      }
    }
    return writeEnabledPacks(out);
  };
  auto buildGames = [&](std::vector<std::string> &games) { games.clear();
    for (auto &p : packs) { std::string g = gfxGameOf(p); bool f = false; for (auto &x : games) if (x == g) { f = true; break; } if (!f) games.push_back(g); }
    std::sort(games.begin(), games.end()); };
  auto packValue = [&](const GfxPack &p, const PackSel &s) -> std::string {
    if (!s.enabled) return "off";
    if (p.presets.empty()) return "On";
    auto cats = gfxGroupPresets(p);
    if (cats.size() > 2) return "On  >";
    std::string v; for (auto &c : cats) { std::string cur; for (auto &ch : s.choices) if (ch.first == c.category) { cur = ch.second; break; }
      if (cur.empty()) cur = gfxCatDefault(p, c);
      if (!v.empty()) v += ", ";
      v += cur;
    }
    return v.empty() ? "On" : v;
  };

  int mode = perGame ? 1 : 0;
  std::vector<std::string> games; buildGames(games);
  std::vector<GRow> rows; if (perGame) rows = gfxBuildGameRows(packs, "", true);
  int sel = 0, top = 0, gameSel = 0;
  auto packVis = [&]() { int v = (SH - LIST_Y0 - 150) / ROW_H; return v < 1 ? 1 : v; };
  auto rowGap = [&](int index) { return mode==0&&top==0&&index>0?16:0; };
  auto nextSelectable = [&](int from, int dir) { int n = (int)rows.size(); if (n == 0) return 0; int i = from;
    for (int k = 0; k < n; k++) { if (i >= 0 && i < n && !rows[i].header) return i; i = (i + dir + n) % n; } return from; };
  if (perGame) sel = nextSelectable(0, +1);
  beginScreenFx();

  for (;;) {
    if (!beginUiFrame()) return;
    int nrows = (mode == 0) ? (1 + (int)games.size()) : (int)rows.size();
    int vis = (mode == 1) ? packVis() : listVis();
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touchScrollList(touch,sel,top,nrows,vis)){
          if(mode==1&&sel>=0&&sel<nrows&&rows[sel].header) sel=nextSelectable(sel,touch==TOUCH_SCROLL_UP?+1:-1);
          if(sel<top) top=sel;
          if(sel>=top+vis) top=sel-vis+1;
          continue;
        }
        if(touch==TOUCH_TAP){
          if(ty<topBarH()||ty>=SH-40){ SDL_Event back{}; back.type=SDL_CONTROLLERBUTTONDOWN; back.cbutton.button=BTN_CANCEL; SDL_PushEvent(&back); continue; }
          for(int visibleRow=0;visibleRow<vis&&top+visibleRow<nrows;visibleRow++){
            int y=LIST_Y0+visibleRow*ROW_H+rowGap(top+visibleRow);
            if(ty>=y&&ty<y+ROW_H){ int hit=top+visibleRow; if(mode==0||!rows[hit].header){ sel=hit; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); } break; }
          }
          continue;
        }
      }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      int b = e.cbutton.button;
      if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
        int dir = (b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? +1 : -1;
        if (nrows > 0) { sel = (sel + dir + nrows) % nrows;
          if (mode == 1) { int g = 0; while (sel >= 0 && sel < nrows && rows[sel].header && g++ < nrows) sel = (sel + dir + nrows) % nrows; } }
      } else if (b == BTN_CANCEL) {
        if (mode == 1 && !perGame) { mode = 0; sel = gameSel; top = 0; beginScreenFx(); }
        else { if (!commit()) { toast("Could not save graphics packs"); SDL_Delay(1600); } return; }
      } else if (b == BTN_CONFIRM) {
        if (mode == 0) {
          if (sel == 0) {
            downloadLatestGraphicPacks();
            load(packs); buildGames(games); sel = top = 0;
            beginScreenFx();
          } else if (sel - 1 < (int)games.size()) {
            gameSel = sel;
            rows = gfxBuildGameRows(packs, games[sel - 1], false); mode = 1; sel = nextSelectable(0, +1); top = 0; beginScreenFx();
          }
        } else if (sel >= 0 && sel < (int)rows.size() && !rows[sel].header) {
          const GfxPack &p = packs[rows[sel].packIndex]; PackSel &s = selByRel[p.rulesRel];
          if (p.presets.empty()) s.enabled = !s.enabled;
          else { openPackPanel(p, s); beginScreenFx(); }
        }
      }
      if (sel < top) top = sel;
      if (sel >= top + vis) top = sel - vis + 1;
      if (top < 0) top = 0;
      while (mode == 1 && top > 0 && rows[top - 1].header) top--;
    }
    nrows = (mode == 0) ? (1 + (int)games.size()) : (int)rows.size();
    vis = (mode == 1) ? packVis() : listVis();
    clearUiBackground();
    std::string gname; if (mode == 1 && !perGame) { for (auto &gr : rows) if (!gr.header) { gname = gfxGameOf(packs[gr.packIndex]); break; } }
    drawHeader("Graphics Packs", gname.empty() ? nullptr : gname.c_str());
    int colX, colW, labelX, valX; listCol(&colX, &colW, &labelX, &valX);
    int fh0 = TTF_FontHeight(g_font);
    float ty = (float)(LIST_Y0 + (sel - top) * ROW_H + rowGap(sel) + 1);
    g_hy = (!g_uiAnimations || g_hy < 0) ? ty : g_hy + (ty - g_hy) * 0.30f;
    fillRect(colX, (int)g_hy, colW, ROW_H - 2, COL_FOCUS);
    fillRect(colX, (int)g_hy, 5, ROW_H - 2, COL_SEL);
    for (int r = 0; r < vis && top + r < nrows; r++) {
      int i = top + r, y = LIST_Y0 + r * ROW_H + rowGap(i) + (ROW_H - fh0) / 2; bool cur = (i == sel);
      if (mode == 0) {
        if (i == 0) drawText(g_font, labelX, y, "Download latest packs", cur ? COL_VAL : COL_HI);
        else { drawText(g_font, labelX, y, games[i - 1].c_str(), cur ? COL_VAL : COL_TXT); drawTextR(g_font, valX, y, ">", cur ? COL_VAL : COL_DIM); }
      } else {
        const GRow &gr = rows[i];
        if (gr.header) { drawText(g_font, colX + 14, y, gr.label.c_str(), COL_HI); }
        else { const GfxPack &p = packs[gr.packIndex]; PackSel &s = selByRel[p.rulesRel]; bool en = s.enabled;
          drawText(g_font, labelX + 28, y, gr.label.c_str(), cur ? COL_VAL : COL_TXT);
          SDL_Color oc = en ? (SDL_Color){120, 220, 120, 255} : COL_DIM;
          drawScrollTextR(g_font, valX, y, colW / 2 - 24, packValue(p, s).c_str(), cur ? COL_VAL : oc); }
      }
    }
    if (mode == 1 && sel >= 0 && sel < (int)rows.size() && !rows[sel].header) {
      const GfxPack &p = packs[rows[sel].packIndex];
      if (!p.description.empty()) { int fy = SH - 128; fillRect(colX, fy - 10, colW, 2, (SDL_Color){40, 44, 54, 255});
        drawWrapped(g_font_sm, colX + 6, fy, colW - 12, TTF_FontHeight(g_font_sm) + 4, 4, p.description.c_str(), COL_DIM); }
    }
    if ((mode == 0 && games.empty()) || (mode == 1 && rows.empty()))
      drawTextC(g_font_sm, SW / 2, LIST_Y0 + ROW_H * 3, "No packs - use Download latest packs", COL_DIM);
    if (nrows > vis) { int trH = vis * ROW_H, trX = colX + colW + 16, trY = LIST_Y0 - 2; fillRect(trX, trY, 4, trH, (SDL_Color){40, 44, 54, 255});
      int thH = trH * vis / nrows, dn = (nrows - vis > 0 ? nrows - vis : 1); fillRect(trX, trY + (trH - thH) * top / dn, 4, thH, COL_SEL); }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
}

static SDL_Texture *loadScaledTexture(const std::string &path, int width, int height) {
  SDL_Surface *source=IMG_Load(path.c_str());
  if(!source) return nullptr;
  SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!scaled){ SDL_FreeSurface(source); return nullptr; }
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  const bool ok=SDL_BlitScaled(source,nullptr,scaled,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);
  SDL_FreeSurface(source);
  if(!ok){ SDL_FreeSurface(scaled); return nullptr; }
  SDL_Texture *texture=SDL_CreateTextureFromSurface(g_ren,scaled);
  SDL_FreeSurface(scaled);
  if(texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  if(!g.iconPath.empty() && stat(g.iconPath.c_str(),&st)==0) paths.push_back(g.iconPath);
  { std::string cp=existingCoverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key=steamGridDbKey();
  if(!key.empty()){
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Fetching icons from SteamGridDB...", COL_TXT);
    SDL_RenderPresent(g_ren);
    int nf=griddb_fetch_icons(key,g.title,tmp,14);
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toast("No icon found - add a SteamGridDB key or download a cover first"); SDL_Delay(1800); return false; }
  int n=(int)paths.size();
  int cols=n<5?n:5; if(cols<1)cols=1;
  int rows=(n+cols-1)/cols, gap=18, top=150, bot=40;
  int cw=(SW-80-(cols-1)*gap)/cols, ch=(SH-top-bot-(rows-1)*gap)/rows;
  int cell=cw<ch?cw:ch; if(cell>200)cell=200; if(cell<90)cell=90;
  int x0=(SW-(cols*cell+(cols-1)*gap))/2, y0=top;
  std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=loadScaledTexture(paths[i],cell,cell);
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    if (!beginUiFrame()) { done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){ pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touch==TOUCH_SCROLL_UP){ sel=std::min(n-1,sel+cols); continue; }
        if(touch==TOUCH_SCROLL_DOWN){ sel=std::max(0,sel-cols); continue; }
        if(touch==TOUCH_TAP){
          for(int i=0;i<n;i++){ int row=i/cols,column=i%cols,x=x0+column*(cell+gap),y=y0+row*(cell+gap);
            if(tx>=x&&tx<x+cell&&ty>=y&&ty<y+cell){ sel=i; chosen=i; done=true; break; } }
          if(done) continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) fillRect(x-6,y-6,cell+12,cell+12,COL_SEL);
      fillRect(x,y,cell,cell,COL_CARD);
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

static void forwarderWizard(Game &g) {
  const int ix=110, iy=176, isz=280;
  const int rx=ix+isz+70; int rw=SW-rx-90;
  const int nameY=196, authY=290, createY=406, fieldH=64, createH=58;
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char author[128]; snprintf(author,sizeof(author),"%s","naga");
  char icon[300]={0};
  { struct stat st; std::string cp=existingCoverPath(g);
    if(stat(cp.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",cp.c_str());
    else if(!g.iconPath.empty() && stat(g.iconPath.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",g.iconPath.c_str()); }
  SDL_Texture *iconTex = icon[0] ? loadScaledTexture(icon,isz,isz) : nullptr;
  int sel=0; bool done=false; beginScreenFx();

  auto edit=[&](const char *hdr, char *buf, size_t sz){
    char b[256];
    if(promptText(hdr, buf, b, sizeof(b)) && b[0] && sz){
      size_t len=std::min(strlen(b),sz-1);
      memcpy(buf,b,len);
      buf[len]=0;
    }
  };
  auto build=[&](){
    if(!icon[0]){ toast("Pick an icon first"); SDL_Delay(1200); return; }
    clearUiBackground();
    drawHeader("Creating HOME shortcut", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    SDL_RenderPresent(g_ren);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    char err[256]={0}; bool ok=forwarder_create(g.key,name,author,icon,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toast("HOME shortcut installed"); SDL_Delay(1800); done=true; }
    else modalMessage("Shortcut failed", { err[0]?err:"Unknown error" });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=loadScaledTexture(icon,isz,isz); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else if(sel==2) edit("Author", author, sizeof(author));
    else build();
  };

  while(!done){
    if (!beginUiFrame()) { done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(ty>=authY-6&&ty<authY+fieldH){ sel=2; activate(); }
          else if(ty>=createY-6&&ty<createY+createH){ sel=3; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?3:(sel==1?3:sel-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==3?1:sel+1); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Create HOME shortcut", g.title.c_str());
    if(sel==0) fillRect(ix-6,iy-6,isz+12,isz+12,COL_SEL);
    fillRect(ix,iy,isz,isz,COL_CARD);
    if(iconTex){ SDL_Rect d{ix,iy,isz,isz}; SDL_RenderCopy(g_ren,iconTex,nullptr,&d); }
    else drawTextC(g_font_sm,ix+isz/2,iy+isz/2,"(no icon)",COL_DIM);
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20, "Icon", sel==0?COL_VAL:COL_DIM);
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      if(cur){ fillRect(rx-10,y-6,rw+20,fieldH,COL_FOCUS); fillRect(rx-10,y-6,5,fieldH,COL_SEL); }
      drawText(g_font_sm, rx, y, label, cur?COL_VAL:COL_DIM);
      drawScrollTextL(g_font, rx, y+26, rw-8, val, cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    field(2,authY,"Author",author);
    { bool cur=sel==3;
      fillRect(rx-10,createY-6,rw+20,createH, cur?(SDL_Color){44,86,44,240}:(SDL_Color){30,46,32,200});
      if(cur) fillRect(rx-10,createY-6,5,createH,COL_SEL);
      drawTextC(g_font, rx+rw/2, createY+12, "Create shortcut", cur?COL_VAL:(SDL_Color){150,225,150,255}); }
    drawFadeIn(); SDL_RenderPresent(g_ren); SDL_Delay(8);
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

static void showTitleIdUnavailable(const char *title,const Game &game) {
  std::vector<std::string> lines;
  if(!game.titleIdError.empty()) lines.push_back(game.titleIdError);
  else lines.push_back("The title ID could not be read from this game's metadata.");
  std::string extension=game.file.substr(game.file.find_last_of('.')==std::string::npos?game.file.size():game.file.find_last_of('.'));
  std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){ return (char)std::tolower(c); });
  if(extension==".wud"||extension==".wux"||extension==".iso")
    lines.push_back("Add the disc key to Cemu/keys.txt or place a .key beside the game.");
  lines.push_back("");
  lines.push_back("Rescan the game folders after correcting the file or key.");
  modalMessage(title,lines);
}

static void clearShaderCaches(const Game &g) {
  if (!g.titleId) {
    showTitleIdUnavailable("Shader caches unavailable",g);
    return;
  }

  static const char *formats[] = {
    "/cache/shaderCache/driver/vk/%016llx.bin",
    "/cache/shaderCache/precompiled/%016llx_spirv.bin",
    "/cache/shaderCache/precompiled/%016llx_gl.bin",
    "/cache/shaderCache/precompiled/%016llx_air.bin",
    "/cache/shaderCache/transferable/%016llx_shaders.bin",
    "/cache/shaderCache/transferable/%016llx_mtlshaders.bin",
    "/cache/shaderCache/transferable/%016llx_vkpipeline.bin",
    "/cache/shaderCache/transferable/%016llx_mtlpipeline.bin",
  };
  std::vector<std::string> paths;
  for (const char *format : formats) {
    char suffix[256];
    int length = snprintf(suffix, sizeof(suffix), format, (unsigned long long)g.titleId);
    if (length > 0 && (size_t)length < sizeof(suffix)) {
      std::string path = std::string(DATA_DIR) + suffix;
      if (regularFileExists(path)) paths.emplace_back(std::move(path));
    }
  }
  if (paths.empty()) {
    toast("No shader caches found");
    SDL_Delay(900);
    return;
  }
  if (!confirmBox("Clear shader caches?", {
        g.title, "", "Cemu will rebuild them the next time the game runs."
      }))
    return;

  int failed = 0;
  for (const auto &path : paths)
    if (remove(path.c_str()) != 0 && errno != ENOENT)
      failed++;
  if (failed) {
    modalMessage("Cache cleanup failed", {
      std::to_string(failed) + " cache file(s) could not be removed."
    });
  } else {
    toast("Shader caches cleared");
    SDL_Delay(900);
  }
}

static int perGameMenu(Game &g) {
  const char *items[] = { "Launch", "Game settings", "Graphics packs", "Rename game", "Download cover (SteamGridDB)", "Create HOME shortcut", "Manage installed content", "Clear shader caches", "Clear game settings", "Delete game (remove from SD)" };
  constexpr int menuY = 184, menuStep = 52, menuHeight = 46;
  int n=10, sel=0, touchTop=0;
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  const std::string downloadedPacksVersion=std::string(GRAPHICPACKS_DIR)+"/downloadedGraphicPacks/version.txt";
  bool graphicPacksDownloaded=regularFileExists(downloadedPacksVersion);
  storeLoad(g_game, gp.c_str());
  beginScreenFx();
  for(;;){
    if (!beginUiFrame()) return 0;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,touchTop,n,n)) continue;
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return 0; }
          for(int i=0;i<n;i++){ int y=menuY+i*menuStep; if(ty>=y-5 && ty<y-5+menuHeight){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(g.title.c_str());
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            storeSave(g_game, gp.c_str());
            g.hasCfg = !g_game.kv.empty();
            beginScreenFx();
          }
          else if(sel==2){
            if(!graphicPacksDownloaded){
              const int result=downloadLatestGraphicPacks();
              if(result==GFX_OK||result==GFX_UPTODATE)
                graphicPacksDownloaded=regularFileExists(downloadedPacksVersion);
            }
            else if (g.titleId) gfxPackScreen(g.titleId);
            else showTitleIdUnavailable("Graphics packs unavailable",g);
            beginScreenFx();
          }
          else if(sel==3){
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==4){ downloadCover(g); beginScreenFx(); }
          else if(sel==5){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==6){
            if (!g.titleId) showTitleIdUnavailable("Installed content unavailable",g);
            else if (installedContentScreen(g.titleId) && g.path.empty()) return 2;
            beginScreenFx();
          }
          else if(sel==7){ clearShaderCaches(g); beginScreenFx(); }
          else if(sel==8){ g_game.kv.clear(); remove(gp.c_str()); g.hasCfg=false; toast("Game settings cleared"); SDL_Delay(700); beginScreenFx(); }
          else if(sel==9){
            if(g.path.empty()){
              if (installedContentScreen(g.titleId)) return 2;
              beginScreenFx();
            }
            else {
              struct stat gameStat{};
              if (stat(g.path.c_str(), &gameStat) != 0) {
                modalMessage("Delete failed", { "The selected game no longer exists.", "No metadata was removed." });
                beginScreenFx();
              } else if (S_ISDIR(gameStat.st_mode)) {
                modalMessage("Folder deletion disabled", {
                  "Extracted game folders are not deleted automatically.",
                  "Remove this folder manually to avoid deleting unrelated files:",
                  g.path
                });
                beginScreenFx();
              } else if (!S_ISREG(gameStat.st_mode)) {
                modalMessage("Delete failed", { "The selected path is not a regular game file." });
                beginScreenFx();
              } else if(confirmBox("Delete game?", { g.title, "", "This permanently deletes the game file from",
                                                  "the SD card. This cannot be undone." })){
                if (remove(g.path.c_str()) != 0) {
                  modalMessage("Delete failed", { "The game file could not be removed.", "No metadata was removed." });
                  beginScreenFx();
                } else {
                  remove(coverPath(g).c_str());
                  remove(gp.c_str());
                  storeRemove(g_titles, g.key.c_str());
                  storeRemove(g_recent, g.key.c_str());
                  storeSave(g_titles, TITLES_INI);
                  storeSave(g_recent, RECENT_INI);
                  toast("Game deleted"); SDL_Delay(800);
                  return 2;
                }
              }
            }
          }
          break;
      }
    }
    clearUiBackground();
    g_cover_budget = 1;
    ensureCover(g);
    int cw=300,chh=450,cx=90,cy=(SH-chh)/2;
    fillRect(cx+5,cy+7,cw,chh,(SDL_Color){0,0,0,60}); fillRect(cx+2,cy+3,cw,chh,(SDL_Color){0,0,0,75});
    if(g.cover){ SDL_SetTextureAlphaMod(g.cover,255); SDL_SetTextureColorMod(g.cover,255,255,255);
      SDL_Rect d={cx,cy,cw,chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d); border(cx,cy,cw,chh,2,COL_DIM); }
    else { fillRect(cx,cy,cw,chh,(SDL_Color){40,44,54,255}); border(cx,cy,cw,chh,2,COL_DIM); drawTextC(g_font,cx+cw/2,cy+chh/2,"NO COVER",COL_DIM); }
    drawScrollTextL(g_font_big,cx+cw+70,104,SW-(cx+cw+70)-50,g.title.c_str(),COL_TXT);
    char titleIdText[48];
    if(g.titleId) snprintf(titleIdText,sizeof(titleIdText),"Title ID  %016llX",(unsigned long long)g.titleId);
    else snprintf(titleIdText,sizeof(titleIdText),"Title ID unavailable");
    drawText(g_font_sm,cx+cw+70,154,titleIdText,g.titleId?COL_DIM:(SDL_Color){230,130,130,255});
    int mx=cx+cw+64, mw=SW-mx-70;
    float ty=(float)(menuY+sel*menuStep-5);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(mx,(int)g_hy,mw,menuHeight,COL_FOCUS);
    fillRect(mx,(int)g_hy,5,menuHeight,COL_SEL);
    for(int i=0;i<n;i++){ int slot=menuY+i*menuStep-5; int y=slot+(menuHeight-TTF_FontHeight(g_font))/2; bool cur=i==sel;
      SDL_Color rc = (i==n-1) ? (SDL_Color){228,120,120,255} : i==2&&!graphicPacksDownloaded ? COL_HI : COL_TXT;
      const char *label=i==2&&!graphicPacksDownloaded?"Download graphics packs":
                        i==4&&regularFileExists(coverPath(g))?"Change cover (SteamGridDB)":items[i];
      drawText(g_font,cx+cw+94,y,label,cur?COL_VAL:rc);
    }
    drawFadeIn();
    SDL_RenderPresent(g_ren);
    SDL_Delay(8);
  }
}

static void drawSetupProgress(int pct, const char *msg) {
  clearUiBackground();
  if (g_logo) { int s = 140; SDL_Rect ld = {(SW - s) / 2, SH / 2 - 170, s, s}; SDL_RenderCopy(g_ren, g_logo, nullptr, &ld); }
  drawTextC(g_font, SW / 2, SH / 2 - 10, msg, COL_TXT);
  int bw = SW * 2 / 3, bx = (SW - bw) / 2, by = SH / 2 + 40, bh = 36;
  border(bx, by, bw, bh, 2, COL_SEL);
  fillRect(bx + 3, by + 3, (bw - 6) * pct / 100, bh - 6, COL_HI);
  char t[16]; snprintf(t, sizeof(t), "%d%%", pct);
  drawTextC(g_font_sm, SW / 2, by + bh + 14, t, COL_DIM);
  SDL_RenderPresent(g_ren);
}

static bool validNro(const std::string &path, long long expectedSize) {
  struct stat fileStat{};
  if (stat(path.c_str(), &fileStat) != 0 || fileStat.st_size <= 0 ||
      (expectedSize >= 0 && fileStat.st_size != expectedSize)) return false;
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) return false;
  NroStart start{};
  NroHeader header{};
  bool ok = fread(&start, 1, sizeof(start), file) == sizeof(start) &&
            fread(&header, 1, sizeof(header), file) == sizeof(header);
  fclose(file);
  if (!ok || header.magic != NROHEADER_MAGIC ||
      header.size < sizeof(start) + sizeof(header) || header.size > (u64)fileStat.st_size)
    return false;
  for (const auto &segment : header.segments)
    if (segment.file_off > header.size || segment.size > header.size - segment.file_off)
      return false;
  return true;
}

static bool readEmbeddedEmulatorHash(std::array<u8, SHA256_HASH_SIZE> &hash,
                                     std::string &text) {
  FILE *file=fopen(EMU_HASH_SRC,"rb");
  if(!file) return false;
  char buffer[128];
  const size_t size=fread(buffer,1,sizeof(buffer),file);
  const bool ok=!ferror(file) && feof(file);
  fclose(file);
  if(!ok) return false;
  std::string value=trim(std::string(buffer,size));
  if(value.size()!=SHA256_HASH_SIZE*2) return false;
  auto hex=[](char c)->int {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
  };
  static const char digits[]="0123456789abcdef";
  text.clear(); text.reserve(SHA256_HASH_SIZE*2);
  for(size_t i=0;i<SHA256_HASH_SIZE;i++){
    int high=hex(value[i*2]),low=hex(value[i*2+1]);
    if(high<0||low<0) return false;
    hash[i]=(u8)((high<<4)|low);
    text+=digits[high]; text+=digits[low];
  }
  return true;
}

static bool ensureEmu() {
  std::string marker = std::string(DATA_DIR) + "/.emu_build";
  const std::string tmp = std::string(EMU_NRO_DST) + ".tmp";
  const std::string old = std::string(EMU_NRO_DST) + ".old";
  bool destinationExists=false,oldExists=false,tmpExists=false;
  if(!queryRegularFile(EMU_NRO_DST,destinationExists) || !queryRegularFile(old,oldExists) ||
     !queryRegularFile(tmp,tmpExists)) return false;
  if (!destinationExists && oldExists) {
    if (validNro(old,-1)) {
      if(rename(old.c_str(),EMU_NRO_DST)!=0) return false;
      destinationExists=true;
    } else if(remove(old.c_str())!=0) {
      return false;
    }
    oldExists=false;
    fsdevCommitDevice("sdmc");
  } else if (destinationExists && oldExists) {
    if (!validNro(EMU_NRO_DST, -1) && validNro(old, -1)) {
      if (remove(EMU_NRO_DST) != 0 || rename(old.c_str(), EMU_NRO_DST) != 0) return false;
    } else {
      if(remove(old.c_str())!=0) return false;
    }
    fsdevCommitDevice("sdmc");
  }
  if(tmpExists && remove(tmp.c_str())!=0) return false;

  std::array<u8,SHA256_HASH_SIZE> expectedHash{};
  std::string expectedHashText;
  if(!readEmbeddedEmulatorHash(expectedHash,expectedHashText) || !recoverAtomicFile(marker)) return false;

  struct stat sourceStat{};
  if (stat(EMU_NRO_SRC, &sourceStat) != 0 || sourceStat.st_size <= 0 || !S_ISREG(sourceStat.st_mode)) return false;
  char cur[80] = {0};
  if (FILE *mf = fopen(marker.c_str(), "r")) { if (!fgets(cur, sizeof(cur), mf)) cur[0] = 0; fclose(mf); }
  struct stat destinationStat{};
  if (trim(cur) == expectedHashText && stat(EMU_NRO_DST, &destinationStat) == 0 &&
      destinationStat.st_size == sourceStat.st_size && validNro(EMU_NRO_DST, sourceStat.st_size))
    return true;

  FILE *in = fopen(EMU_NRO_SRC, "rb"), *out = fopen(tmp.c_str(), "wb");
  if (!in || !out) {
    if (in) fclose(in);
    if (out) fclose(out);
    remove(tmp.c_str());
    return false;
  }
  static char buf[1 << 16]; size_t n; bool ok = true; long long written = 0; int lastPct = -1;
  Sha256Context hashContext;
  sha256ContextCreate(&hashContext);
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    sha256ContextUpdate(&hashContext,buf,n);
    written += (long long)n;
    int pct = (int)((unsigned __int128)written * 100 / sourceStat.st_size);
    if (pct != lastPct) {
      if(!beginUiFrame()){ ok=false; break; }
      SDL_Event event; while(pollUiEvent(event)) {}
      if(g_exitRequested){ ok=false; break; }
      drawSetupProgress(pct, "Updating..."); lastPct = pct;
    }
  }
  if (ferror(in)) ok = false;
  if (fclose(in) != 0) ok = false;
  if (fflush(out) != 0 || fsync(fileno(out)) != 0) ok = false;
  if (fclose(out) != 0) ok = false;
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  std::array<u8,SHA256_HASH_SIZE> actualHash{};
  sha256ContextGetHash(&hashContext,actualHash.data());
  if (!ok || stat(tmp.c_str(), &destinationStat) != 0 ||
      destinationStat.st_size != sourceStat.st_size || actualHash != expectedHash ||
      !validNro(tmp, sourceStat.st_size)) {
    remove(tmp.c_str());
    return false;
  }
  if (!replaceAtomic(EMU_NRO_DST, tmp)) {
    remove(tmp.c_str());
    return false;
  }
  return writeAtomicText(marker, expectedHashText + "\n");
}

struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  bool big = SW >= 1600;
  g.gapx = big?24:18; g.gapy = big?18:14; g.titleH = g_showGameTitles?(big?30:24):0;
  int topBar = big?112:80, footer = big?54:38;
  g.rows = g_gridRows;
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = big?60:40;
  int autoWidth=maxCoverH*2/3;
  g.cols=g_gridColumns;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static int gridHitTest(int px,int py,int top){
  GLay L=gridLayout(); int n=(int)g_games.size();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  SDL_Rect clip={x0,y-2,cellW,(f?TTF_FontHeight(f):26)+8};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-cellW;
  float t=(SDL_GetTicks()%5000)/5000.0f;
  float pp = t<0.5f ? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,title.c_str(),col);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}
static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void renderGrid(int sel,int top,const char*gamedirLabel){
  clearUiBackground();
  g_cover_budget = COVER_DECODE_BUDGET;
  if(sel>=0 && sel<(int)g_games.size()) ensureCover(g_games[sel]);
  GLay L=gridLayout();
  int n=(int)g_games.size(), per=L.cols*L.rows;
  int pages=n?(n+per-1)/per:1, page=n?(sel/per)+1:1;
  int bandH = L.y0 - 4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  char pinfo[160]; snprintf(pinfo,sizeof(pinfo),"%d / %d    \xc2\xb7    Page %d / %d    \xc2\xb7    Sort: %s",n?sel+1:0,n,page,pages,SORT_NAME[g_sort]);
  drawTextC(g_font,SW/2,(bandH-TTF_FontHeight(g_font))/2,pinfo,COL_VAL);
  int pinfoRight=SW/2+textW(g_font,pinfo)/2;
  int folderMaxW=(SW-34)-(pinfoRight+24);
  drawScrollTextR(g_font_sm,SW-34,(bandH-TTF_FontHeight(g_font_sm))/2,folderMaxW,gamedirLabel,COL_DIM);

  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c;
    if(idx>=n) continue;
    Game&g=g_games[idx];
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    bool cur=(idx==sel);
    ensureCover(g);
    fillRect(x+4,y+6,L.cw,L.chh,(SDL_Color){0,0,0,55});
    fillRect(x+2,y+3,L.cw,L.chh,(SDL_Color){0,0,0,70});
    if(g.cover){
      Uint32 el=SDL_GetTicks()-g.coverAt; Uint8 fa=!g_uiAnimations?255:(el<180?(Uint8)(255*el/180):255);
      SDL_SetTextureAlphaMod(g.cover,fa);
      SDL_SetTextureColorMod(g.cover,cur?255:150,cur?255:150,cur?255:150);
      SDL_Rect d={x,y,L.cw,L.chh}; SDL_RenderCopy(g_ren,g.cover,nullptr,&d);
    }
    else { fillRect(x,y,L.cw,L.chh,COL_CARD); drawTextC(g_font_sm,x+L.cw/2,y+L.chh/2-8,"NO COVER",COL_DIM); }
    border(x,y,L.cw,L.chh,1,(SDL_Color){12,13,18,255});
    fillRect(x,y,L.cw,1,(SDL_Color){255,255,255,26});
    if(cur){ const int G=6;
      for(int i=G;i>=1;i--){ Uint8 a=(Uint8)(150*(G-i+1)/G); border(x-2-i,y-2-i,L.cw+4+2*i,L.chh+4+2*i,1,(SDL_Color){255,170,0,a}); }
      border(x-2,y-2,L.cw+4,L.chh+4,2,COL_SEL);
    }
    if(g.region>0 && g_flag[g.region]){
      int fw=L.cw*26/100; if(fw>30)fw=30; if(fw<16)fw=16; int fh=fw*2/3;
      SDL_Rect fd={x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[g.region],nullptr,&fd);
      border(x+6,y+6,fw,fh,1,(SDL_Color){10,12,18,255});
    }
    if(g.hasCfg){ int ds=L.cw/11<12?12:L.cw/11; fillRect(x+L.cw-ds-8,y+8,ds,ds,COL_SEL); border(x+L.cw-ds-8,y+8,ds,ds,2,(SDL_Color){10,12,18,255}); }
    if(g_showGameTitles) drawTitleCell(x+L.cw/2,L.cw,y+L.chh+6,g.title,cur,cur?COL_VAL:COL_DIM);
  }
  if(n==0) drawTextC(g_font,SW/2,SH/2,"No games found -- press X for Settings > Game folder",COL_DIM);
  drawUpdateNotification();
  FootItem foot[] = {
    { g_gA, "Launch", FA_LAUNCH }, { g_gY, "Sort", FA_SORT },
    { g_gX, "Settings", FA_SETTINGS }, { g_gPlus, "Game Menu", FA_OPTIONS },
    { g_gL, "", FA_PAGEL }, { g_gR, "Page", FA_PAGER }, { g_gB, "Quit", FA_QUIT },
  };
  drawFooterHints(foot, 7, SH-26);
  SDL_RenderPresent(g_ren);
}

// Horizontal edges turn whole grid pages; vertical movement stays within a page.
static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

static bool ensureDirectory(const char *path) {
  if (mkdir(path, 0777) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st{};
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  LauncherUpdate_Shutdown();
  for (auto &game : g_games) {
    if (game.cover) SDL_DestroyTexture(game.cover);
    game.cover = nullptr;
  }
  clearTextCaches();
  for (int i = 1; i < 4; i++) {
    if (g_flag[i]) SDL_DestroyTexture(g_flag[i]);
    g_flag[i] = nullptr;
  }
  SDL_Texture **glyphs[] = { &g_gA, &g_gB, &g_gX, &g_gY, &g_gPlus, &g_gL, &g_gR };
  for (SDL_Texture **glyph : glyphs) {
    if (*glyph) SDL_DestroyTexture(*glyph);
    *glyph = nullptr;
  }
  if (g_logo) SDL_DestroyTexture(g_logo);
  g_logo = nullptr;
  if (g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_glowTexture = nullptr;

  if (g_font) TTF_CloseFont(g_font);
  if (g_font_sm) TTF_CloseFont(g_font_sm);
  if (g_font_big) TTF_CloseFont(g_font_big);
  g_font = g_font_sm = g_font_big = nullptr;
  if (g_plReady) plExit();
  g_plReady = false;

  uiAudioShutdown();
  SwitchStorage::Shutdown();
  closeController();
  if (g_ren) SDL_DestroyRenderer(g_ren);
  if (g_win) SDL_DestroyWindow(g_win);
  g_ren = nullptr;
  g_win = nullptr;
  if (g_imgReady) IMG_Quit();
  if (g_ttfReady) TTF_Quit();
  if (g_sdlReady) SDL_Quit();
  g_imgReady = g_ttfReady = g_sdlReady = false;

  if (g_griddbReady) griddb_global_exit();
  g_griddbReady = false;
  if (g_storageSocketReady) socketExit();
  g_storageSocketReady = false;
  if (g_romfsReady) romfsExit();
  g_romfsReady = false;
}

static int startupFailure(const char *message) {
  if (g_sdlReady)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Cemu Launcher", message, g_win);
  cleanupLauncher();
  return 1;
}

int main(int argc, char **argv){
  if(argc>=1 && argv[0] && argv[0][0]){
    g_forwarderSelfPath=argv[0];
    setLauncherPathFromArg(argv[0]);
  }
  std::string updateRecoveryError;
  const bool updateRecoveryOk=LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  if (R_FAILED(romfsInit())) return 1;
  g_romfsReady = true;
  detectSystemLanguage();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO)!=0) return startupFailure("SDL initialization failed.");
  g_sdlReady = true;
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady = true;
  const int imageFlags = IMG_INIT_PNG | IMG_INIT_JPG;
  if((IMG_Init(imageFlags) & imageFlags) != imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady = true;
  if(appletGetOperationMode()==AppletOperationMode_Console){ SW=1920; SH=1080; }
  g_win=SDL_CreateWindow("Cemu",0,0,SW,SH,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&SW,&SH)!=0) return startupFailure("Could not query the display size.");
  { SDL_Surface *ls=IMG_Load("romfs:/logo.png"); if(ls){ g_logo=SDL_CreateTextureFromSurface(g_ren,ls); SDL_FreeSurface(ls); } }
  makeFlags();

  for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){ openController(i); break; }

  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady = true;
  PlFontData fd{};
  if(R_FAILED(plGetSharedFontByType(&fd,PlSharedFontType_Standard)) || !fd.address || !fd.size || fd.size > INT_MAX)
    return startupFailure("Could not load the system font.");
  int sc = SH>=1080?1:0;
  auto openFont = [&](int size) -> TTF_Font * {
    SDL_RWops *rw = SDL_RWFromConstMem(fd.address,(int)fd.size);
    return rw ? TTF_OpenFontRW(rw,1,size) : nullptr;
  };
  g_font_sm =openFont(sc?26:20);
  g_font    =openFont(sc?32:26);
  g_font_big=openFont(sc?52:40);
  if(!g_font_sm || !g_font || !g_font_big) return startupFailure("Could not open the system font.");
  makeGlyphs();
  g_griddbReady = griddb_global_init();
  if(!g_griddbReady && R_SUCCEEDED(socketInitializeDefault())) g_storageSocketReady=true;

  const char *directories[] = {
    "sdmc:/switch", DATA_DIR, EMU_HOST_DIR, COVERS_DIR, GAMECFG_DIR, DEF_GAMEDIR,
    GAMEPROFILES_DIR, GRAPHICPACKS_DIR, LSFG_DIR, "sdmc:/switch/cemu/cache",
    "sdmc:/switch/cemu/install", "sdmc:/switch/cemu/mlc01"
  };
  for (const char *directory : directories)
    if (!ensureDirectory(directory)) return startupFailure("Could not create the Cemu data directories.");

  if(!updateRecoveryOk)
    modalMessage("Update recovery failed",{updateRecoveryError,"The installed launcher was left unchanged."});

  if(!ensureEmu())
    modalMessage("Emulator setup failed", {
      "Could not install the emulator to the SD card.",
      "The SD card may be full or write-protected.",
      "",
      "Free up space and relaunch." });

  struct stat bst;
  bool firstRun = (stat(LAUNCHER_INI, &bst) != 0);
  storeLoad(g_global, LAUNCHER_INI);
  storeLoad(g_titles, TITLES_INI);
  storeLoad(g_recent, RECENT_INI);
  storeLoad(g_containerTitles, CONTAINER_TITLES_INI);
  { int sm = atoi(storeGet(g_global,"Wrapper/SortMode","0")); if(sm>=0 && sm<SORT_COUNT) g_sort = sm; }
  if (firstRun) {
    g_active = &g_global;
    saveGameSources({DEF_GAMEDIR});
    storeSet(g_global, "Wrapper/SteamGridDBKey", "");
    storeSet(g_global, "Wrapper/UiSounds", "true");
    storeSet(g_global, "Wrapper/Theme", "homebrew");
    storeSet(g_global, "console_language", "-1");
    storeSet(g_global, "Wrapper/GridColumns", "5");
    storeSet(g_global, "Wrapper/GridRows", "2");
    storeSet(g_global, "Wrapper/ShowGameTitles", "true");
    storeSet(g_global, "Wrapper/UiAnimations", "true");
    storeSet(g_global, "Wrapper/CheckUpdatesAtBoot", "true");
    storeSet(g_global, "Wrapper/InstalledReleaseTag", LauncherUpdate_BuiltReleaseTag());
    commitAll();
    storeSave(g_global, LAUNCHER_INI);
  } else {
    bool changed=false;
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","5"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","5"); changed=true; }
    if(changed) storeSave(g_global,LAUNCHER_INI);
  }
  applyLauncherAppearance();
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  std::vector<std::string> gamePaths=loadGameSources();
  bool hasUsbSource=hasConfiguredUsbSource(gamePaths);
  SwitchStorage::InitializeFromConfig(LAUNCHER_INI,hasUsbSource);
  uint64_t usbGeneration=SwitchStorage::UsbStatusGeneration();
  refreshConfiguredUsbSources(gamePaths);
  scanGames(gamePaths);
  Uint32 usbRefreshAt=0;
  const uint64_t startupUsbGeneration=SwitchStorage::UsbStatusGeneration();
  if(startupUsbGeneration!=usbGeneration){ usbGeneration=startupUsbGeneration; usbRefreshAt=SDL_GetTicks()+300; }

  if (!cemu_hasConfiguredDiscKey("sdmc:/switch/cemu/keys.txt"))
    modalMessage("Disc key required", {
      "Cemu/keys.txt does not contain a Wii U disc key.",
      "WUX games cannot be decrypted until a valid key is added.",
      "",
      "Add the key to sdmc:/switch/cemu/keys.txt." });

  int sel=0, top=0, rows=1;
  bool running=true, launch=false;
  std::string launchKey;
  std::string launchPath;
  uint64_t launchTitleId=0;

  auto selectGame = [&](Game &game) {
    recordPlayed(game);
    launchKey = game.key;
    launchPath = game.path;
    launchTitleId = game.titleId;
    launch = true;
    running = false;
  };

  bool forwarderRequested=false,forwarderMatched=false;
  std::string forwarderKey;
  for(int ai=1; ai+1<argc; ai++) if(strcmp(argv[ai],"-g")==0){
    forwarderRequested=true;
    forwarderKey=argv[ai+1];
    if (Game *game = findGameByKey(forwarderKey)){ selectGame(*game); forwarderMatched=true; }
    break;
  }
  if(!forwarderRequested&&g_griddbReady&&
     strcmp(storeGet(g_global,"Wrapper/CheckUpdatesAtBoot","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());
  bool forwarderPending=forwarderRequested&&!forwarderMatched&&hasUsbSource;
  const Uint32 forwarderDeadline=forwarderPending?SDL_GetTicks()+6000:0;
  if(forwarderPending&&!usbRefreshAt) usbRefreshAt=SDL_GetTicks()+300;
  if(forwarderRequested&&!forwarderMatched&&!forwarderPending){
    modalMessage("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});
    running=false;
  }

  while(running && beginUiFrame()){
    if(hasUsbSource){
      const Uint32 now=SDL_GetTicks();
      const uint64_t generation=SwitchStorage::UsbStatusGeneration();
      if(generation!=usbGeneration){ usbGeneration=generation; usbRefreshAt=now+300; }
      if(usbRefreshAt&&SDL_TICKS_PASSED(now,usbRefreshAt)){
        usbRefreshAt=0;
        const std::string selected=!g_games.empty()?g_games[sel].key:std::string{};
        refreshConfiguredUsbSources(gamePaths);
        scanGames(gamePaths);
        sel=0;
        if(!selected.empty()) for(size_t index=0;index<g_games.size();index++) if(g_games[index].key==selected){ sel=(int)index; break; }
        top=0;
        if(forwarderPending) if(Game *game=findGameByKey(forwarderKey)){
          selectGame(*game);
          forwarderPending=false;
        }
      }
      if(forwarderPending&&SDL_TICKS_PASSED(now,forwarderDeadline)){
        forwarderPending=false;
        modalMessage("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});
        running=false;
      }
      if(!running) break;
    }
    if(forwarderPending){
      SDL_Event event;
      while(pollUiEvent(event)){
        pumpStick(event);
        if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL){
          running=false;
          break;
        }
      }
      if(!running) break;
      renderUsbForwarderWait();
      SDL_Delay(8);
      continue;
    }
    GLay L=gridLayout();
    int cols=L.cols; rows=L.rows;

    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0,n=(int)g_games.size(); TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_SWIPE_L||tk==TOUCH_SWIPE_R){ sel=gridPage(sel,tk==TOUCH_SWIPE_L?+1:-1,cols,rows,n); top=n?(sel/(cols*rows))*rows:0; continue; }
        if(tk==TOUCH_TAP){
          int fa=footTapAct(tx,ty);
          if(fa==FA_NONE){ int hit=gridHitTest(tx,ty,top);
            if(hit>=0){
              if(hit==sel && n) selectGame(g_games[sel]);
              else sel=hit;
            }
          } else {
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN;
            switch(fa){
              case FA_LAUNCH:   a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break;
              case FA_SORT:     a.cbutton.button=SDL_CONTROLLER_BUTTON_X; SDL_PushEvent(&a); break;
              case FA_OPTIONS:  a.cbutton.button=SDL_CONTROLLER_BUTTON_START; SDL_PushEvent(&a); break;
              case FA_SETTINGS: a.cbutton.button=BTN_SETTINGS; SDL_PushEvent(&a); break;
              case FA_PAGEL:    sel=gridPage(sel,-1,cols,rows,n); break;
              case FA_PAGER:    sel=gridPage(sel,+1,cols,rows,n); break;
              case FA_QUIT:     running=false; break;
            }
          }
          top=n?(sel/(cols*rows))*rows:0;
          if(!running) break;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int n=(int)g_games.size();
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=gridNav(sel,-1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=gridNav(sel,+1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=gridNav(sel,0,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=gridNav(sel,0,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  sel=gridPage(sel,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel=gridPage(sel,+1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_X:
          if(n){
            std::string keep=g_games[sel].key;
            g_sort=(g_sort+1)%SORT_COUNT;
            char sb[8]; snprintf(sb,sizeof(sb),"%d",g_sort);
            storeSet(g_global,"Wrapper/SortMode",sb); storeSave(g_global,LAUNCHER_INI);
            applySort();
            sel=0; for(int i=0;i<n;i++) if(g_games[i].key==keep){ sel=i; break; }
          }
          break;
        case BTN_CONFIRM:
          if(n) selectGame(g_games[sel]);
          break;
        case SDL_CONTROLLER_BUTTON_START:
          if(n){ int r=perGameMenu(g_games[sel]);
            if(r==1) selectGame(g_games[sel]);
            else if(r==2){ scanGames(gamePaths); sel=0; top=0; } }
          break;
        case BTN_SETTINGS: {
          std::vector<std::string> oldPaths=gamePaths;
          g_active=&g_global; runSettingsRoot(nullptr);
          storeSave(g_global,LAUNCHER_INI);
          { GLay updated=gridLayout(); cols=updated.cols; rows=updated.rows; }
          gamePaths=loadGameSources();
          if(gamePaths!=oldPaths||g_rescanAfterSettings){
            hasUsbSource=hasConfiguredUsbSource(gamePaths);
            if(hasUsbSource) SwitchStorage::InitializeUsb();
            usbGeneration=SwitchStorage::UsbStatusGeneration();
            usbRefreshAt=0;
            refreshConfiguredUsbSources(gamePaths);
            scanGames(gamePaths);
            sel=0;
            top=0;
            g_rescanAfterSettings=false;
          }
          break;
        }
        case BTN_CANCEL: running=false; break;
      }
      top = n ? (sel/(cols*rows))*rows : 0;
    }
    pollUpdateNotification();
    const std::string location=!g_games.empty()?gameLocationLabel(g_games[sel]):"No game selected";
    renderGrid(sel,top,location.c_str());
    SDL_Delay(8);
  }

  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global, LAUNCHER_INI);
  storeSave(g_recent, RECENT_INI);

  bool willChain = false;
  if(launch && envHasNextLoad()){
    struct stat est; bool haveEmu = (stat(EMU_NRO_DST, &est) == 0);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

    std::vector<CemuKV> eff = buildEffectiveSettings(launchKey);
    std::vector<CemuGraphicPack> enabledPacks;
    bool configOk = haveEmu && readEnabledPacks(enabledPacks);
    if (configOk) configOk = cemu_writeSettingsXml(SETTINGS_XML, eff, gamePaths, enabledPacks);
    if (configOk) configOk = writeInputIni(eff);
    if(configOk && launchTitleId){
      const char *gname = "";
      for(auto &g:g_games) if(g.key==launchKey){ gname=g.title.c_str(); break; }
      configOk = cemu_writeGameProfile(GAMEPROFILES_DIR, launchTitleId, gname, eff);
    }
    if (configOk) {
      std::string handoff;
      bool lsfgPrepared = !strcmp(cemuKVGet(eff,"Wrapper/LSFGEnabled","false"),"true");
      if (lsfgPrepared && !regularFileExists(LSFG_DLL_FILE)) {
        lsfgPrepared = false;
        modalMessage("LSFG disabled for this launch", {
          "Lossless.dll was not found at",
          "sdmc:/switch/cemu/lsfg/Lossless.dll"
        });
      }
      configOk = appendHandoffValue(handoff, "timer_shift", cemuKVGet(eff,"TimerShiftFactor","3")) &&
                 appendHandoffValue(handoff, "triple_buffer", cemuKVGet(eff,"TripleBuffer","1")) &&
                 appendHandoffValue(handoff, "cpu_mode", cemuKVGet(eff,"cpuMode","3")) &&
                 appendHandoffValue(handoff, "gamepad_layout", cemuKVGet(eff,"GamePadLayout","off")) &&
                 appendHandoffValue(handoff, "lsfg_enabled", lsfgPrepared ? "true" : "false") &&
                 appendHandoffValue(handoff, "lsfg_flow_scale", cemuKVGet(eff,"Wrapper/LSFGFlowScale","0.25")) &&
                 appendHandoffValue(handoff, "lsfg_performance", cemuKVGet(eff,"Wrapper/LSFGPerformance","true")) &&
                 appendHandoffValue(handoff, "usb_skylanders", cemuKVGet(eff,"UsbSkylanders","false")) &&
                 appendHandoffValue(handoff, "usb_infinity", cemuKVGet(eff,"UsbInfinity","false")) &&
                 appendHandoffValue(handoff, "usb_dimensions", cemuKVGet(eff,"UsbDimensions","false"));
      char gameId[32]{};
      if (configOk && !launchPath.empty()) {
        configOk = appendHandoffValue(handoff, "game", launchPath);
      } else if (configOk && launchTitleId) {
        snprintf(gameId, sizeof(gameId), "id:%016llx", (unsigned long long)launchTitleId);
        configOk = appendHandoffValue(handoff, "game", gameId);
      }
      if (configOk) configOk = writeAtomicText(LAUNCH_HANDOFF, handoff);
    }
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(configOk) willChain=true;
    else {
      remove(LAUNCH_HANDOFF);
      if (!haveEmu) {
        toast("Emulator missing - copy cemu.nro into sdmc:/switch/cemu/"); SDL_Delay(2400);
      } else {
        modalMessage("Launch configuration failed", {
          "Cemu configuration could not be updated safely.",
          "The previous settings were preserved.",
          "Check free SD space and file permissions."
        });
      }
    }
  }

  cleanupLauncher();

  if(willChain) envSetNextLoad(EMU_NRO_DST,EMU_NRO_DST);
  return 0;
}
