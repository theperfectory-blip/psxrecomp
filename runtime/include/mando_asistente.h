#pragma once
/*
 * mando_asistente — configuración manual de mandos que SDL no reconoce.
 *
 * Muchos mandos genéricos USB (DirectInput) no están en la base de datos de
 * SDL: SDL los ve como "joystick" pero no como "gamepad", y el runtime solo
 * abre gamepads, así que el juego no reacciona a ellos.
 *
 * Solución:
 *   - `WE2002_Recompiled.exe --configurar-mando` abre un asistente (ventana
 *     propia, sin juego) que pide pulsar cada botón de PlayStation y guarda una
 *     línea de mapeo con formato SDL en `mandos.txt`, junto al .exe.
 *   - Al arrancar el juego, psx_mandos_cargar() carga `mandos.txt` (y, si
 *     existe, `gamecontrollerdb.txt`) antes de abrir los mandos. Con eso el
 *     mando pasa a ser un gamepad normal y el resto del runtime no cambia.
 *
 * El GUID que SDL asigna depende de qué controlador de Windows atiende al
 * mando (HIDAPI, RawInput, DirectInput…). Por eso el asistente y el juego
 * deben usar las mismas pistas de SDL: psx_mandos_aplicar_pistas().
 */
#include <filesystem>

/* Pistas de joystick de SDL comunes al juego y al asistente. Llamar antes de
 * SDL_Init. */
void psx_mandos_aplicar_pistas(void);

/* Carga los mapeos de `mandos.txt` y `gamecontrollerdb.txt` (si existen) de
 * la carpeta dada. Llamar después de SDL_Init y antes de abrir los mandos.
 * Escribe en stdout los joysticks conectados que siguen sin mapeo.
 * Devuelve el número de mapeos cargados. */
int psx_mandos_cargar(const std::filesystem::path& dir_exe);

/* Asistente interactivo. Inicializa y cierra SDL por su cuenta.
 * Devuelve el código de salida del proceso (0 = guardado, 1 = cancelado/error). */
int psx_mando_asistente(const std::filesystem::path& dir_exe);
