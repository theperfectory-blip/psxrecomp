/* mando_asistente.cpp — configuración manual de mandos genéricos.
 * Ver include/mando_asistente.h para el porqué.
 *
 * El asistente dibuja con SDL_RenderDebugText (fuente 8x8 de SDL3, solo
 * ASCII): los textos van sin tildes a propósito. */

#include "mando_asistente.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(PSX_SDL3)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif

namespace fs = std::filesystem;

void psx_mandos_aplicar_pistas(void) {
    /* Las mismas que el juego necesita (ver comentarios en main.cpp, init de
     * SDL): HIDAPI preferido (mando virtual de Steam), RawInput apagado y el
     * subcontrolador Xbox de HIDAPI encendido. Si cambian, el GUID del mando
     * cambia y los mapeos guardados en mandos.txt dejan de coincidir. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
#if defined(PSX_SDL3)
    /* main.cpp usa el nombre viejo vía SDL_ENABLE_OLD_NAMES; es la misma pista. */
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1",
                            SDL_HINT_OVERRIDE);
#else
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1",
                            SDL_HINT_OVERRIDE);
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
}

static std::string ruta_utf8(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

#if defined(PSX_SDL3)

int psx_mandos_cargar(const fs::path& dir_exe) {
    int total = 0;
    std::error_code ec;
    /* Base comunitaria (opcional): SDL solo toma sus líneas con platform:Windows. */
    const fs::path db = dir_exe / "gamecontrollerdb.txt";
    if (fs::exists(db, ec)) {
        const int n = SDL_AddGamepadMappingsFromFile(ruta_utf8(db).c_str());
        if (n > 0) total += n;
        std::fprintf(stdout, "psxrecomp: mandos: gamecontrollerdb.txt -> %d mapeos\n", n);
    }
    /* mandos.txt se lee línea a línea con SDL_AddGamepadMapping: así vale
     * también una línea escrita a mano sin campo platform (el cargador de
     * archivos de SDL3 la descartaría en silencio). */
    const fs::path propio = dir_exe / "mandos.txt";
    if (fs::exists(propio, ec)) {
        std::ifstream in(propio, std::ios::binary);
        std::string l;
        int n = 0, malas = 0;
        while (std::getline(in, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            if (SDL_AddGamepadMapping(l.c_str()) >= 0) n++;
            else malas++;
        }
        total += n;
        std::fprintf(stdout, "psxrecomp: mandos: mandos.txt -> %d mapeos (%d con error)\n", n, malas);
    }
    int cuenta = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&cuenta);
    for (int i = 0; ids && i < cuenta; i++) {
        if (SDL_IsGamepad(ids[i])) continue;
        char guid[40] = {0};
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), guid, sizeof(guid));
        const char* nom = SDL_GetJoystickNameForID(ids[i]);
        std::fprintf(stdout,
            "psxrecomp: mandos: '%s' (%s) no esta configurado; "
            "usa CONFIGURAR_MANDO.bat\n", nom ? nom : "?", guid);
    }
    SDL_free(ids);
    return total;
}

/* ── Asistente ─────────────────────────────────────────────────────────── */

namespace {

enum class Tipo { Boton, Eje };

struct Paso {
    const char* pedido;   /* lo que se le pide al jugador */
    const char* corto;    /* nombre PlayStation para el resumen */
    const char* campo;    /* campo del mapeo SDL */
    Tipo tipo;
};

/* Equivalencias PlayStation -> nombre SDL: las mismas que usa input.ini por
 * defecto (cross = a, circle = b, square = x, triangle = y, select = back). */
const Paso kPasos[] = {
    {"Pulsa ARRIBA en la cruceta",                    "Arriba",    "dpup",          Tipo::Boton},
    {"Pulsa ABAJO en la cruceta",                     "Abajo",     "dpdown",        Tipo::Boton},
    {"Pulsa IZQUIERDA en la cruceta",                 "Izquierda", "dpleft",        Tipo::Boton},
    {"Pulsa DERECHA en la cruceta",                   "Derecha",   "dpright",       Tipo::Boton},
    {"Pulsa el boton que hara de X (equis)",          "X",         "a",             Tipo::Boton},
    {"Pulsa el boton que hara de CIRCULO",            "Circulo",   "b",             Tipo::Boton},
    {"Pulsa el boton que hara de CUADRADO",           "Cuadrado",  "x",             Tipo::Boton},
    {"Pulsa el boton que hara de TRIANGULO",          "Triangulo", "y",             Tipo::Boton},
    {"Pulsa L1 (gatillo de arriba, izquierda)",       "L1",        "leftshoulder",  Tipo::Boton},
    {"Pulsa R1 (gatillo de arriba, derecha)",         "R1",        "rightshoulder", Tipo::Boton},
    {"Pulsa L2 (gatillo de abajo, izquierda)",        "L2",        "lefttrigger",   Tipo::Boton},
    {"Pulsa R2 (gatillo de abajo, derecha)",          "R2",        "righttrigger",  Tipo::Boton},
    {"Pulsa SELECT",                                  "Select",    "back",          Tipo::Boton},
    {"Pulsa START",                                   "Start",     "start",         Tipo::Boton},
    {"Hunde el stick IZQUIERDO (L3)",                 "L3",        "leftstick",     Tipo::Boton},
    {"Hunde el stick DERECHO (R3)",                   "R3",        "rightstick",    Tipo::Boton},
    {"Mueve el stick IZQUIERDO a la DERECHA",         "Stick izq horizontal", "leftx",  Tipo::Eje},
    {"Mueve el stick IZQUIERDO hacia ABAJO",          "Stick izq vertical",   "lefty",  Tipo::Eje},
    {"Mueve el stick DERECHO a la DERECHA",           "Stick der horizontal", "rightx", Tipo::Eje},
    {"Mueve el stick DERECHO hacia ABAJO",            "Stick der vertical",   "righty", Tipo::Eje},
};
constexpr int kNumPasos = (int)(sizeof(kPasos) / sizeof(kPasos[0]));

constexpr int kUmbralEje = 20000;   /* movimiento que cuenta como pulsación */
constexpr int kReposoEje = 8000;    /* margen para "soltado" */

/* Lo que el jugador acaba de tocar. */
struct Entrada {
    enum Clase { Nada, Boton, Hat, Eje } clase = Nada;
    int indice = 0;
    int valor = 0;    /* hat: dirección (1,2,4,8); eje: signo (+1/-1) */
};

struct Pad {
    SDL_Joystick* joy = nullptr;
    SDL_JoystickID id = 0;
    std::string nombre;
    std::string guid;
    std::vector<int> base;   /* ejes en reposo */
};

void tomar_base(Pad& p) {
    const int n = SDL_GetNumJoystickAxes(p.joy);
    p.base.assign(n > 0 ? n : 0, 0);
    for (int i = 0; i < n; i++) p.base[i] = SDL_GetJoystickAxis(p.joy, i);
}

bool en_reposo(const Pad& p) {
    for (int i = 0; i < SDL_GetNumJoystickButtons(p.joy); i++)
        if (SDL_GetJoystickButton(p.joy, i)) return false;
    for (int i = 0; i < SDL_GetNumJoystickHats(p.joy); i++)
        if (SDL_GetJoystickHat(p.joy, i) != SDL_HAT_CENTERED) return false;
    for (int i = 0; i < (int)p.base.size(); i++)
        if (std::abs(SDL_GetJoystickAxis(p.joy, i) - p.base[i]) > kReposoEje)
            return false;
    return true;
}

Entrada leer_entrada(const Pad& p) {
    Entrada e;
    for (int i = 0; i < SDL_GetNumJoystickButtons(p.joy); i++)
        if (SDL_GetJoystickButton(p.joy, i)) { e.clase = Entrada::Boton; e.indice = i; return e; }
    for (int i = 0; i < SDL_GetNumJoystickHats(p.joy); i++) {
        const Uint8 h = SDL_GetJoystickHat(p.joy, i);
        for (int d : {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT})
            if (h == d) { e.clase = Entrada::Hat; e.indice = i; e.valor = d; return e; }
    }
    int mejor = -1, delta_mejor = 0;
    for (int i = 0; i < (int)p.base.size(); i++) {
        const int d = SDL_GetJoystickAxis(p.joy, i) - p.base[i];
        if (std::abs(d) > kUmbralEje && std::abs(d) > std::abs(delta_mejor)) {
            mejor = i; delta_mejor = d;
        }
    }
    if (mejor >= 0) { e.clase = Entrada::Eje; e.indice = mejor; e.valor = delta_mejor > 0 ? 1 : -1; }
    return e;
}

/* Convierte la entrada en el valor del mapeo SDL. Vacío = no sirve para este paso. */
std::string token_para(const Paso& paso, const Entrada& e, const Pad& p) {
    char buf[32];
    if (paso.tipo == Tipo::Eje) {
        if (e.clase != Entrada::Eje) return "";
        std::snprintf(buf, sizeof buf, "a%d%s", e.indice, e.valor > 0 ? "" : "~");
        return buf;
    }
    switch (e.clase) {
    case Entrada::Boton:
        std::snprintf(buf, sizeof buf, "b%d", e.indice);
        return buf;
    case Entrada::Hat:
        std::snprintf(buf, sizeof buf, "h%d.%d", e.indice, e.valor);
        return buf;
    case Entrada::Eje: {
        const int base = p.base[e.indice];
        /* Gatillo analógico que descansa en un extremo: eje completo. */
        if (base < -24000 && e.valor > 0)
            std::snprintf(buf, sizeof buf, "a%d", e.indice);
        else if (base > 24000 && e.valor < 0)
            std::snprintf(buf, sizeof buf, "a%d~", e.indice);
        else
            std::snprintf(buf, sizeof buf, "%ca%d", e.valor > 0 ? '+' : '-', e.indice);
        return buf;
    }
    default:
        return "";
    }
}

std::string sin_comas(std::string s) {
    for (char& c : s) if (c == ',' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string armar_mapeo(const Pad& p, const std::vector<std::string>& tokens) {
    std::string linea = p.guid + "," + sin_comas(p.nombre.empty() ? "Mando" : p.nombre) + ",";
    for (int i = 0; i < kNumPasos; i++)
        if (!tokens[i].empty()) linea += std::string(kPasos[i].campo) + ":" + tokens[i] + ",";
    /* SDL3 descarta, al cargar archivos, las líneas sin plataforma. */
    linea += std::string("platform:") + SDL_GetPlatform() + ",";
    return linea;
}

bool guardar_mapeo(const fs::path& dir_exe, const std::string& guid, const std::string& linea) {
    const fs::path ruta = dir_exe / "mandos.txt";
    std::vector<std::string> lineas;
    {
        std::ifstream in(ruta, std::ios::binary);
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.size() >= guid.size() &&
                SDL_strncasecmp(l.c_str(), guid.c_str(), guid.size()) == 0)
                continue;  /* se reemplaza el mapeo anterior de este mando */
            lineas.push_back(l);
        }
    }
    if (lineas.empty())
        lineas.push_back("# Mandos configurados con CONFIGURAR_MANDO.bat (formato SDL). "
                         "Borra una linea para olvidar ese mando.");
    lineas.push_back(linea);
    std::ofstream out(ruta, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto& l : lineas) out << l << "\r\n";
    return (bool)out;
}

/* ── Dibujo ── */

struct Pantalla {
    SDL_Renderer* r = nullptr;
    int y = 0;
    void limpiar() {
        SDL_SetRenderDrawColor(r, 16, 24, 40, 255);
        SDL_RenderClear(r);
        y = 24;
    }
    void texto(const std::string& s, float escala = 2.0f,
               Uint8 cr = 230, Uint8 cg = 230, Uint8 cb = 230, int x = 24) {
        SDL_SetRenderScale(r, escala, escala);
        SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
        SDL_RenderDebugText(r, x / escala, y / escala, s.c_str());
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        y += (int)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * escala) + 10;
    }
    void espacio(int px = 14) { y += px; }
};

std::string a_ascii(const char* s) {
    std::string o;
    for (const unsigned char* c = (const unsigned char*)(s ? s : ""); *c; c++)
        o += (*c >= 32 && *c < 127) ? (char)*c : '?';
    return o;
}

enum class Fase { Elegir, Reposo, Esperar, Soltar, Resumen, Prueba };

}  // namespace

int psx_mando_asistente(const fs::path& dir_exe) {
    psx_mandos_aplicar_pistas();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stdout, "configurar-mando: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* Los mapeos ya guardados también cuentan (para la prueba final). */
    psx_mandos_cargar(dir_exe);

    SDL_Window* win = SDL_CreateWindow("Configurar mando - WE2002", 1000, 680, 0);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (win && !ren) ren = SDL_CreateRenderer(win, "software");
    if (!ren) {
        std::fprintf(stdout, "configurar-mando: sin ventana: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    Pantalla pan;
    pan.r = ren;

    std::vector<SDL_Joystick*> abiertos;   /* todos, para elegir el que se pulse */
    auto abrir_todos = [&]() {
        for (SDL_Joystick* j : abiertos) SDL_CloseJoystick(j);
        abiertos.clear();
        int n = 0;
        SDL_JoystickID* ids = SDL_GetJoysticks(&n);
        for (int i = 0; ids && i < n; i++)
            if (SDL_Joystick* j = SDL_OpenJoystick(ids[i])) abiertos.push_back(j);
        SDL_free(ids);
    };
    abrir_todos();

    Pad pad;
    Fase fase = Fase::Elegir;
    int paso = 0;
    std::vector<std::string> tokens(kNumPasos);
    std::string aviso;
    std::string mapeo;
    bool guardado = false;
    SDL_Gamepad* gp = nullptr;
    Uint64 t_fase = SDL_GetTicks();
    bool salir = false;

    auto ir_a = [&](Fase f) { fase = f; t_fase = SDL_GetTicks(); };
    auto siguiente = [&]() {
        aviso.clear();
        paso++;
        ir_a(paso >= kNumPasos ? Fase::Resumen : Fase::Reposo);
    };

    while (!salir) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) salir = true;
            else if (ev.type == SDL_EVENT_JOYSTICK_ADDED ||
                     ev.type == SDL_EVENT_JOYSTICK_REMOVED) {
                if (fase == Fase::Elegir) abrir_todos();
                else if (ev.type == SDL_EVENT_JOYSTICK_REMOVED &&
                         ev.jdevice.which == pad.id) {
                    aviso = "Se desconecto el mando. Vuelve a conectarlo y empieza otra vez.";
                    pad = Pad();
                    if (gp) { SDL_CloseGamepad(gp); gp = nullptr; }
                    abrir_todos();
                    ir_a(Fase::Elegir);
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                const SDL_Keycode k = ev.key.key;
                if (k == SDLK_ESCAPE) {
                    salir = true;
                } else if ((fase == Fase::Reposo || fase == Fase::Esperar ||
                            fase == Fase::Soltar) && k == SDLK_SPACE) {
                    tokens[paso].clear();   /* omitir: este mando no lo tiene */
                    siguiente();
                } else if ((fase == Fase::Reposo || fase == Fase::Esperar ||
                            fase == Fase::Soltar) && k == SDLK_BACKSPACE && paso > 0) {
                    aviso.clear();
                    paso--;
                    ir_a(Fase::Reposo);
                } else if (fase == Fase::Resumen && k == SDLK_BACKSPACE) {
                    paso = kNumPasos - 1;
                    ir_a(Fase::Reposo);
                } else if (fase == Fase::Resumen && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
                    mapeo = armar_mapeo(pad, tokens);
                    guardado = guardar_mapeo(dir_exe, pad.guid, mapeo);
                    std::fprintf(stdout, "configurar-mando: %s %s\n",
                                 guardado ? "guardado" : "NO se pudo guardar", mapeo.c_str());
                    if (guardado && SDL_AddGamepadMapping(mapeo.c_str()) >= 0)
                        gp = SDL_OpenGamepad(pad.id);
                    ir_a(Fase::Prueba);
                } else if (fase == Fase::Prueba && (k == SDLK_RETURN || k == SDLK_KP_ENTER)) {
                    salir = true;
                }
            }
        }

        /* ── Lógica por fase ── */
        if (fase == Fase::Elegir) {
            for (SDL_Joystick* j : abiertos) {
                Pad tmp;
                tmp.joy = j;
                tmp.base.assign(SDL_GetNumJoystickAxes(j), 0);
                /* Elegir solo con un botón o la cruceta (los ejes pueden no
                 * estar centrados todavía). */
                Entrada e = leer_entrada(tmp);
                if (e.clase == Entrada::Boton || e.clase == Entrada::Hat) {
                    pad.joy = j;
                    pad.id = SDL_GetJoystickID(j);
                    pad.nombre = a_ascii(SDL_GetJoystickName(j));
                    char guid[40] = {0};
                    SDL_GUIDToString(SDL_GetJoystickGUID(j), guid, sizeof(guid));
                    pad.guid = guid;
                    std::fill(tokens.begin(), tokens.end(), std::string());
                    paso = 0;
                    aviso.clear();
                    std::fprintf(stdout, "configurar-mando: elegido '%s' (%s)\n",
                                 pad.nombre.c_str(), pad.guid.c_str());
                    ir_a(Fase::Reposo);
                    break;
                }
            }
        } else if (fase == Fase::Reposo) {
            /* La primera vez se fija la posición de reposo de los ejes
             * (gatillos que descansan en un extremo, sticks algo corridos). */
            if (pad.base.empty() && SDL_GetTicks() - t_fase > 600) tomar_base(pad);
            if (!pad.base.empty() || SDL_GetNumJoystickAxes(pad.joy) == 0) {
                if (en_reposo(pad)) ir_a(Fase::Esperar);
            }
        } else if (fase == Fase::Esperar) {
            const Entrada e = leer_entrada(pad);
            if (e.clase != Entrada::Nada) {
                const Paso& ps = kPasos[paso];
                std::string t = token_para(ps, e, pad);
                if (t.empty()) {
                    aviso = (ps.tipo == Tipo::Eje)
                        ? "Eso fue un boton. Mueve el stick, o pulsa ESPACIO si no tiene."
                        : "No reconoci eso. Prueba otra vez.";
                    ir_a(Fase::Soltar);
                } else {
                    /* Un mismo botón no puede ser dos botones de PlayStation.
                     * Los ejes de stick sí pueden repetirse (cruceta = stick). */
                    int repetido = -1;
                    if (ps.tipo == Tipo::Boton)
                        for (int i = 0; i < kNumPasos; i++)
                            if (i != paso && kPasos[i].tipo == Tipo::Boton && tokens[i] == t)
                                repetido = i;
                    if (repetido >= 0) {
                        aviso = std::string("Ese ya es ") + kPasos[repetido].corto +
                                ". Pulsa otro, o ESPACIO para omitir.";
                    } else {
                        tokens[paso] = t;
                        aviso.clear();
                        paso++;
                        if (paso >= kNumPasos) { ir_a(Fase::Resumen); }
                        else { ir_a(Fase::Soltar); }
                    }
                    if (repetido >= 0) ir_a(Fase::Soltar);
                }
            }
        } else if (fase == Fase::Soltar) {
            if (en_reposo(pad)) ir_a(Fase::Esperar);
        }

        /* ── Dibujo ── */
        pan.limpiar();
        pan.texto("CONFIGURAR MANDO", 3.0f, 255, 210, 80);
        pan.espacio(4);
        if (fase == Fase::Elegir) {
            pan.texto("Mandos conectados:");
            int n = 0;
            SDL_JoystickID* ids = SDL_GetJoysticks(&n);
            if (n == 0) pan.texto("  (ninguno) Conecta tu mando por USB.", 2.0f, 255, 120, 120);
            for (int i = 0; ids && i < n; i++) {
                const bool reconocido = SDL_IsGamepad(ids[i]);
                pan.texto(std::string("  - ") + a_ascii(SDL_GetJoystickNameForID(ids[i])) +
                          (reconocido ? "  [ya reconocido]" : "  [sin configurar]"),
                          2.0f, 200, 220, 255);
            }
            SDL_free(ids);
            pan.espacio();
            pan.texto("Pulsa cualquier BOTON del mando que quieres configurar.");
            pan.espacio();
            pan.texto("ESC = salir sin cambiar nada", 2.0f, 160, 160, 160);
        } else if (fase == Fase::Resumen || fase == Fase::Prueba) {
            pan.texto("Mando: " + pad.nombre, 2.0f, 200, 220, 255);
            if (fase == Fase::Resumen) {
                for (int i = 0; i < kNumPasos; i++) {
                    const std::string v = tokens[i].empty() ? "(sin asignar)" : tokens[i];
                    pan.texto(std::string(kPasos[i].corto) + ": " + v, 1.5f,
                              tokens[i].empty() ? 160 : 230, tokens[i].empty() ? 160 : 230,
                              tokens[i].empty() ? 160 : 230);
                }
                pan.espacio(6);
                pan.texto("ENTER = guardar     RETROCESO = corregir     ESC = salir sin guardar",
                          2.0f, 255, 210, 80);
            } else {
                if (guardado) {
                    pan.texto("Guardado en mandos.txt. El juego lo usara la proxima vez.",
                              2.0f, 120, 230, 120);
                } else {
                    pan.texto("NO se pudo escribir mandos.txt (carpeta protegida?).",
                              2.0f, 255, 120, 120);
                }
                pan.espacio();
                pan.texto("PRUEBA: pulsa botones; aqui se ve que boton de PlayStation es.");
                pan.espacio(6);
                struct { const char* n; SDL_GamepadButton b; } kBot[] = {
                    {"Arriba", SDL_GAMEPAD_BUTTON_DPAD_UP}, {"Abajo", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
                    {"Izquierda", SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"Derecha", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
                    {"X", SDL_GAMEPAD_BUTTON_SOUTH}, {"Circulo", SDL_GAMEPAD_BUTTON_EAST},
                    {"Cuadrado", SDL_GAMEPAD_BUTTON_WEST}, {"Triangulo", SDL_GAMEPAD_BUTTON_NORTH},
                    {"L1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"R1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
                    {"Select", SDL_GAMEPAD_BUTTON_BACK}, {"Start", SDL_GAMEPAD_BUTTON_START},
                    {"L3", SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"R3", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
                };
                std::string activos;
                if (gp) {
                    for (const auto& b : kBot)
                        if (SDL_GetGamepadButton(gp, b.b)) activos += std::string(b.n) + "  ";
                    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) activos += "L2  ";
                    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) activos += "R2  ";
                    char st[96];
                    std::snprintf(st, sizeof st, "Stick izq: %6d %6d   Stick der: %6d %6d",
                                  SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX),
                                  SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY),
                                  SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX),
                                  SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
                    pan.texto(activos.empty() ? "(nada pulsado)" : activos, 3.0f, 120, 230, 255);
                    pan.texto(st, 1.5f, 200, 200, 200);
                } else {
                    pan.texto("(no se pudo abrir el mando para la prueba)", 2.0f, 255, 120, 120);
                }
                pan.espacio();
                pan.texto("ENTER o ESC = terminar", 2.0f, 255, 210, 80);
            }
        } else {
            char cab[64];
            std::snprintf(cab, sizeof cab, "Paso %d de %d", paso + 1, kNumPasos);
            pan.texto("Mando: " + pad.nombre, 2.0f, 200, 220, 255);
            pan.texto(cab, 2.0f, 160, 160, 160);
            pan.espacio();
            pan.texto(kPasos[paso].pedido, 3.0f, 255, 255, 255);
            pan.espacio(4);
            if (fase == Fase::Reposo && pad.base.empty())
                pan.texto("No toques el mando un momento...", 2.0f, 160, 160, 160);
            else if (fase != Fase::Esperar)
                pan.texto("Suelta todo...", 2.0f, 160, 160, 160);
            else
                pan.texto("(esperando)", 2.0f, 120, 230, 120);
            if (!aviso.empty()) pan.texto(aviso, 2.0f, 255, 160, 80);
            pan.espacio(20);
            pan.texto("ESPACIO = mi mando no tiene este boton (omitir)", 2.0f, 160, 160, 160);
            pan.texto("RETROCESO = volver al paso anterior", 2.0f, 160, 160, 160);
            pan.texto("ESC = salir sin guardar", 2.0f, 160, 160, 160);
            pan.espacio(10);
            std::string hechos;
            for (int i = 0; i < paso; i++)
                hechos += std::string(kPasos[i].corto) + "=" +
                          (tokens[i].empty() ? "-" : tokens[i]) + "  ";
            /* Lo ya asignado, en líneas de ~70 caracteres. */
            while (!hechos.empty()) {
                size_t corte = hechos.size() <= 70 ? hechos.size() : hechos.rfind("  ", 70);
                if (corte == std::string::npos || corte == 0) corte = std::min<size_t>(70, hechos.size());
                pan.texto(hechos.substr(0, corte), 1.5f, 140, 170, 140);
                hechos.erase(0, corte);
                while (!hechos.empty() && hechos[0] == ' ') hechos.erase(0, 1);
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(10);
    }

    if (gp) SDL_CloseGamepad(gp);
    for (SDL_Joystick* j : abiertos) SDL_CloseJoystick(j);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return guardado ? 0 : 1;
}

#else  /* SDL2: solo carga de mapeos; el asistente necesita SDL3. */

int psx_mandos_cargar(const fs::path& dir_exe) {
    int total = 0;
    for (const char* nombre : {"gamecontrollerdb.txt", "mandos.txt"}) {
        std::error_code ec;
        const fs::path p = dir_exe / nombre;
        if (!fs::exists(p, ec)) continue;
        const int n = SDL_GameControllerAddMappingsFromFile(ruta_utf8(p).c_str());
        if (n > 0) total += n;
    }
    return total;
}

int psx_mando_asistente(const fs::path&) {
    std::fprintf(stdout, "configurar-mando: este build usa SDL2; el asistente requiere SDL3\n");
    return 1;
}

#endif
