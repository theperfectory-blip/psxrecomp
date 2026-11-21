/* volunteer_sampling.h -- in-runtime RAM sampling for volunteers (e1n2).
 *
 * Off by default. Enabled with --muestreo <carpeta> or PSX_MUESTREO=<carpeta>.
 * Records the same fnprobe samples the debug bot records, but from inside the
 * runtime (no bot, no Python, no TCP client), adding `imagen` and `pantalla`.
 * Only the runtime is touched: the recompiler and generated/ are untouched.
 *
 * See analisis/ab/voluntarios/DISENO.md and LEEME_VOLUNTARIOS.md.
 */
#ifndef VOLUNTEER_SAMPLING_H
#define VOLUNTEER_SAMPLING_H

#ifdef __cplusplus
extern "C" {
#endif

/* carpeta: output folder (the .jsonl/.zip write straight into it).
 * exe_dir: folder holding objetivos_voluntarios.tsv (next to the .exe).
 * disco_ok: 1 only when the mounted disc is SLPM-87056 (checked by the caller
 *           with the runtime's own disc_identity); 0 writes NO_GRABADO.txt and
 *           records nothing. detalle: human-readable reason, may be NULL. */
void volunteer_sampling_init(const char *carpeta, const char *exe_dir,
                             int disco_ok, const char *detalle);

/* Once per guest frame (emulation thread / guest fiber). Cheap when off. */
void volunteer_sampling_tick(void);

/* Disarms fnprobe, saves estado_voluntario.tsv and writes the .zip. */
void volunteer_sampling_shutdown(void);

/* 1 while sampling is on and recording. */
int volunteer_sampling_activo(void);

#ifdef __cplusplus
}
#endif

#endif /* VOLUNTEER_SAMPLING_H */
