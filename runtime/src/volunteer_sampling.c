/* volunteer_sampling.c -- in-runtime RAM sampling for volunteers (e1n2).
 *
 * Design: analisis/ab/voluntarios/DISENO.md. Summary:
 *  - Reads objetivos_voluntarios.tsv (next to the .exe) at start.
 *  - Uses the runtime's own CD DMA log (dma.c cd_dma_log_*) to know which
 *    overlay image is loaded. Only functions of the loaded image are armed,
 *    plus the EXE ones (always resident). Unknown image => EXE only.
 *  - Arms/disarms the same fnprobe as the TCP fnprobe_* commands through the
 *    debug_server_fnprobe_* accessors, so nothing is duplicated.
 *  - Writes muestras_<fecha>.jsonl in the selected folder, in the bot's format
 *    plus `imagen` and `pantalla`, and compresses it to muestras_<fecha>.zip on
 *    exit.
 *  - Never writes guest RAM: fnprobe only observes.
 *
 * Off unless --muestreo <carpeta> / PSX_MUESTREO=<carpeta>.
 */
#include "volunteer_sampling.h"
#include "debug_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zlib.h>

#ifdef _WIN32
#  include <direct.h>
#  define VS_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  define VS_MKDIR(p) mkdir((p), 0755)
#endif

/* ---- Runtime pieces we only read ---------------------------------------- */
extern uint32_t psx_read_word(uint32_t addr);
extern uint8_t  psx_read_byte(uint32_t addr);
extern uint64_t s_frame_count;
extern uint32_t cd_dma_log_get_total(void);
extern void     cd_dma_log_get_entry(uint32_t idx, int *lba, uint32_t *dest,
                                     uint32_t *size);

/* ---- Budget (see DISENO.md) --------------------------------------------- */
#define VS_CICLO_FRAMES    30      /* 0,5 s a 60 cuadros/s                     */
#define VS_MAX_POR_CICLO    2      /* muestras nuevas por ciclo (~4/s)         */
#define VS_MAX_POR_FN      90      /* tope por funcion en toda la sesion       */
#define VS_COMPLETA_MIN    60      /* con esto y ambos casos sale del reparto  */
#define VS_N_RECORDS     2000      /* tope de registros del fnprobe por ciclo  */
#define VS_MAX_TOTAL    60000      /* tope de muestras por sesion              */
#define VS_MAX_FNS       8192
#define VS_MAX_IMGS        40
#define VS_DISTINTOS        8      /* valores distintos que se recuerdan       */
#define VS_DMA_SCAN      4096      /* transferencias que se revisan por vuelta */

#define VS_ESTADO_ADDR 0x80075780u

/* Palabras de RAM de cada muestra: las mismas que el bot (CFG_DEFECTO). */
static const uint32_t VS_RAM_ABS[4]   = { 0x80075780u, 0x80075700u,
                                          0x1F8000ACu, 0x80075918u };
static const uint32_t VS_RAM_REL[4]   = { 0x0u, 0x326u, 0x32Au, 0x330u };

typedef struct {
    const char *nombre;
    uint32_t base;        /* direccion virtual de carga */
    uint32_t lba;         /* sector inicial en el disco */
    uint32_t sectores;    /* tamano declarado en la tabla de extents */
} VsImg;

typedef struct {
    uint32_t addr;        /* direccion virtual de la funcion */
    uint16_t img;         /* indice en s_imgs (0 = EXE) */
    uint8_t  es_void;
    uint8_t  visto_bot;
    uint32_t llamadores;
    uint32_t muestras;
    uint32_t ciclos_armada;
    uint32_t ciclos_con_llamadas;
    uint32_t llamadas_vistas;
    uint32_t cero;
    uint32_t no_cero;
    int      nv0, na0;
    uint32_t v0[VS_DISTINTOS];
    uint32_t a0[VS_DISTINTOS];
} VsObj;

/* ---- Estado del modulo -------------------------------------------------- */
static int    s_activo   = 0;    /* init llamado                       */
static int    s_grabando = 0;    /* disco ok + objetivos cargados      */
static char   s_carpeta[512];
static char   s_dir_muestras[600];
static char   s_ruta_objetivos[600];
static char   s_ruta_jsonl[700];
static char   s_ruta_estado[700];
static char   s_partido[32];
static FILE  *s_jsonl = NULL;

static char  *s_buf_tsv = NULL;  /* objetivos_voluntarios.tsv en memoria */
static VsImg  s_imgs[VS_MAX_IMGS];
static int    s_nimgs = 0;
static VsObj *s_objs = NULL;
static int    s_nobjs = 0;
static int   *s_orden = NULL;    /* indices para ordenar el lote */

static uint32_t s_lote[PSX_FNPROBE_MAX_FNS];  /* virtuales, orden de armado */
static int      s_lote_obj[PSX_FNPROBE_MAX_FNS];  /* indice en s_objs de cada uno */
static int      s_nlote = 0;
static int      s_armado = 0;
static int      s_img_armada = -1;
static uint64_t s_seq_desde = 0;
static uint32_t s_fase_ini = 0;
static uint32_t s_frame_ini = 0;
static uint32_t s_ultimo_dma = 0;
static int      s_img_actual = -1;   /* -1 = imagen desconocida */
static uint64_t s_prox_ciclo = 0;
static uint32_t s_ciclos = 0;
static uint32_t s_escritas = 0;
static uint32_t s_total_sesion = 0;

/* ---- Utilidades --------------------------------------------------------- */
static void vs_copia(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    if (!src) src = "";
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int vs_hex(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 16);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int vs_dec(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return 0;
    v = strtoul(s, &end, 10);
    if (end == s) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* El estado se escribe igual que en las muestras del bot: 4 bytes de
 * 0x80075780 en orden de memoria, en hexadecimal minuscula. */
static void vs_estado_hex(char out[9])
{
    static const char *d = "0123456789abcdef";
    uint32_t i;
    for (i = 0; i < 4; i++) {
        uint8_t b = psx_read_byte(VS_ESTADO_ADDR + i);
        out[i * 2]     = d[(b >> 4) & 0xF];
        out[i * 2 + 1] = d[b & 0xF];
    }
    out[8] = '\0';
}

static int vs_casos_ok(const VsObj *o)
{
    if (o->es_void) return o->na0 >= 2;
    return (o->cero > 0 && o->no_cero > 0) || o->nv0 >= 3;
}

static int vs_completa(const VsObj *o)
{
    if (o->muestras >= VS_MAX_POR_FN) return 1;
    return o->muestras >= VS_COMPLETA_MIN && vs_casos_ok(o);
}

static void vs_recuerda(uint32_t *v, int *n, uint32_t x)
{
    int i;
    for (i = 0; i < *n; i++) if (v[i] == x) return;
    if (*n < VS_DISTINTOS) v[(*n)++] = x;
}

/* ---- Lectura de objetivos ---------------------------------------------- */
static int vs_buscar_img(const char *nombre)
{
    int i;
    for (i = 0; i < s_nimgs; i++)
        if (strcmp(s_imgs[i].nombre, nombre) == 0) return i;
    return -1;
}

static int vs_cargar_objetivos(void)
{
    FILE *f;
    long n;
    char *p, *fin;

    f = fopen(s_ruta_objetivos, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    n = ftell(f);
    if (n <= 0 || n > (long)(32 * 1024 * 1024)) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    s_buf_tsv = (char *)malloc((size_t)n + 1);
    if (!s_buf_tsv) { fclose(f); return 0; }
    if (fread(s_buf_tsv, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(s_buf_tsv); s_buf_tsv = NULL; return 0;
    }
    fclose(f);
    s_buf_tsv[n] = '\0';

    s_objs = (VsObj *)calloc(VS_MAX_FNS, sizeof(VsObj));
    if (!s_objs) return 0;

    p = s_buf_tsv;
    fin = s_buf_tsv + n;
    while (p < fin) {
        char *nl = (char *)memchr(p, '\n', (size_t)(fin - p));
        char *campos[12];
        int nc = 1;
        char *q;
        if (nl) *nl = '\0';
        if (p[0] != '\0' && p[strlen(p) - 1] == '\r')
            p[strlen(p) - 1] = '\0';

        /* Tabla de imagenes: "#imagen<TAB>NOMBRE<TAB>base<TAB>lba<TAB>sectores" */
        if (strncmp(p, "#imagen\t", 8) == 0) {
            uint32_t base = 0, lba = 0, sect = 0;
            campos[0] = p + 8;
            q = campos[0];
            while (*q && nc < 5) {
                if (*q == '\t') { *q = '\0'; campos[nc++] = q + 1; }
                q++;
            }
            if (nc >= 4 && s_nimgs < VS_MAX_IMGS &&
                vs_hex(campos[1], &base) && vs_dec(campos[2], &lba) &&
                vs_dec(campos[3], &sect)) {
                s_imgs[s_nimgs].nombre   = campos[0];
                s_imgs[s_nimgs].base     = base;
                s_imgs[s_nimgs].lba      = lba;
                s_imgs[s_nimgs].sectores = sect;
                s_nimgs++;
            }
            p = nl ? nl + 1 : fin;
            continue;
        }
        if (p[0] == '#' || p[0] == '\0') { p = nl ? nl + 1 : fin; continue; }
        if (strncmp(p, "direccion", 9) == 0) { p = nl ? nl + 1 : fin; continue; }

        /* direccion imagen nombre ret firma llamadores visto */
        campos[0] = p;
        q = p;
        while (*q && nc < 10) {
            if (*q == '\t') { *q = '\0'; campos[nc++] = q + 1; }
            q++;
        }
        if (nc >= 5 && s_nobjs < VS_MAX_FNS) {
            uint32_t addr = 0;
            int img;
            if (vs_hex(campos[0], &addr)) {
                img = vs_buscar_img(campos[1]);
                if (img >= 0) {
                    VsObj *o = &s_objs[s_nobjs++];
                    memset(o, 0, sizeof(*o));
                    o->addr      = addr;
                    o->img       = (uint16_t)img;
                    o->es_void   = (nc > 3 && strcmp(campos[3], "void") == 0);
                    o->visto_bot = (nc > 6 && strcmp(campos[6], "si") == 0);
                    if (nc > 5) vs_dec(campos[5], &o->llamadores);
                }
            }
        }
        p = nl ? nl + 1 : fin;
    }
    s_orden = (int *)malloc(sizeof(int) * (size_t)(s_nobjs ? s_nobjs : 1));
    if (!s_orden) return 0;
    return s_nobjs > 0 && s_nimgs > 0;
}

/* ---- Estado entre sesiones (estado_voluntario.tsv) ---------------------- */
static VsObj *vs_buscar(const char *imagen, uint32_t addr)
{
    int img = vs_buscar_img(imagen);
    int i;
    if (img < 0) return NULL;
    for (i = 0; i < s_nobjs; i++)
        if (s_objs[i].addr == addr && s_objs[i].img == (uint16_t)img)
            return &s_objs[i];
    return NULL;
}

static void vs_leer_lista(const char *s, uint32_t *v, int *n)
{
    while (*s) {
        char *end = NULL;
        unsigned long x = strtoul(s, &end, 16);
        if (end == s) break;
        vs_recuerda(v, n, (uint32_t)x);
        s = end;
        if (*s == ',') s++;
    }
}

static void vs_cargar_estado(void)
{
    FILE *f = fopen(s_ruta_estado, "r");
    char linea[1024];
    if (!f) return;
    while (fgets(linea, sizeof(linea), f)) {
        char *c[12];
        int nc = 1;
        char *q = linea;
        VsObj *o;
        uint32_t addr = 0;
        if (linea[0] == '#' || linea[0] == '\n' || linea[0] == '\r') continue;
        linea[strcspn(linea, "\r\n")] = '\0';
        c[0] = linea;
        while (*q && nc < 12) {
            if (*q == '\t') { *q = '\0'; c[nc++] = q + 1; }
            q++;
        }
        if (nc < 10) continue;
        if (!vs_hex(c[1], &addr)) continue;
        o = vs_buscar(c[0], addr);
        if (!o) continue;
        vs_dec(c[2], &o->muestras);
        vs_dec(c[3], &o->ciclos_armada);
        vs_dec(c[4], &o->ciclos_con_llamadas);
        vs_dec(c[5], &o->llamadas_vistas);
        vs_dec(c[6], &o->cero);
        vs_dec(c[7], &o->no_cero);
        vs_leer_lista(c[8], o->v0, &o->nv0);
        vs_leer_lista(c[9], o->a0, &o->na0);
    }
    fclose(f);
}

static void vs_guardar_estado(void)
{
    FILE *f;
    int i;
    if (!s_ruta_estado[0]) return;
    f = fopen(s_ruta_estado, "w");
    if (!f) return;
    fprintf(f, "# estado del muestreo de voluntarios (e1n2). No editar a mano.\n");
    fprintf(f, "# imagen\tdireccion\tmuestras\tciclos_armada\tciclos_con_llamadas"
               "\tllamadas_vistas\tcero\tno_cero\tv0\ta0\n");
    for (i = 0; i < s_nobjs; i++) {
        const VsObj *o = &s_objs[i];
        int k;
        if (o->muestras == 0 && o->ciclos_armada == 0) continue;
        fprintf(f, "%s\t0x%08X\t%u\t%u\t%u\t%u\t%u\t%u\t",
                s_imgs[o->img].nombre, o->addr, o->muestras, o->ciclos_armada,
                o->ciclos_con_llamadas, o->llamadas_vistas, o->cero, o->no_cero);
        for (k = 0; k < o->nv0; k++) fprintf(f, "%s0x%08X", k ? "," : "", o->v0[k]);
        fputc('\t', f);
        for (k = 0; k < o->na0; k++) fprintf(f, "%s0x%08X", k ? "," : "", o->a0[k]);
        fputc('\n', f);
    }
    fclose(f);
}

/* ---- Imagen cargada (log de DMA del CD del propio runtime) -------------- */
static int vs_zona_imagen(const VsImg *im, uint32_t dest, uint32_t size)
{
    uint32_t base_phys = im->base & 0x1FFFFFFFu;
    uint32_t fin_prog  = base_phys + im->sectores * 2048u;
    uint32_t fin_dma   = dest + (size ? size : 1u);
    return dest < fin_prog && base_phys < fin_dma;
}

static void vs_actualizar_imagen(void)
{
    uint32_t total = cd_dma_log_get_total();
    uint32_t desde, i;
    if (total == s_ultimo_dma) return;
    desde = s_ultimo_dma;
    if (total - desde > VS_DMA_SCAN) desde = total - VS_DMA_SCAN;
    for (i = desde; i < total; i++) {
        int lba = -1;
        uint32_t dest = 0, size = 0;
        int k;
        int encontrada = -1;
        cd_dma_log_get_entry(i, &lba, &dest, &size);
        if (lba < 0) continue;
        for (k = 0; k < s_nimgs; k++) {
            const VsImg *im = &s_imgs[k];
            if (dest != (im->base & 0x1FFFFFFFu)) continue;
            if ((uint32_t)lba < im->lba ||
                (uint32_t)lba >= im->lba + im->sectores) continue;
            encontrada = k;
            break;
        }
        if (encontrada >= 0) {
            s_img_actual = encontrada;
            continue;
        }
        /* Una transferencia que pisa el codigo de la imagen actual con algo que
         * no reconocemos (p. ej. un SELECT vacio) la invalida. */
        if (s_img_actual >= 0 && vs_zona_imagen(&s_imgs[s_img_actual], dest, size))
            s_img_actual = -1;
    }
    s_ultimo_dma = total;
}

/* ---- Reparto y vuelco -------------------------------------------------- */
static int vs_clave(const void *pa, const void *pb)
{
    const VsObj *a = &s_objs[*(const int *)pa];
    const VsObj *b = &s_objs[*(const int *)pb];
    long ka = (long)a->ciclos_armada * 5L - (long)a->ciclos_con_llamadas * 4L;
    long kb = (long)b->ciclos_armada * 5L - (long)b->ciclos_con_llamadas * 4L;
    if (ka != kb) return ka < kb ? -1 : 1;
    if (a->visto_bot != b->visto_bot) return a->visto_bot ? 1 : -1;
    if (a->llamadores != b->llamadores) return a->llamadores > b->llamadores ? -1 : 1;
    if (a->addr != b->addr) return a->addr < b->addr ? -1 : 1;
    return 0;
}

static void vs_armar(void)
{
    int n = 0, m = 0, i, k;
    for (i = 0; i < s_nobjs; i++) {
        const VsObj *o = &s_objs[i];
        if (o->img != 0 && (int)o->img != s_img_actual) continue;  /* 0 = EXE */
        if (vs_completa(o)) continue;
        s_orden[n++] = i;
    }
    if (n == 0) { s_nlote = 0; return; }
    qsort(s_orden, (size_t)n, sizeof(int), vs_clave);
    for (i = 0; i < n && m < PSX_FNPROBE_MAX_FNS; i++) {
        const VsObj *o = &s_objs[s_orden[i]];
        int dup = 0;
        for (k = 0; k < m; k++) if (s_lote[k] == o->addr) { dup = 1; break; }
        if (dup) continue;
        s_lote[m] = o->addr;
        s_lote_obj[m] = s_orden[i];
        m++;
    }
    s_nlote = m;
    s_seq_desde = 0;
    s_fase_ini = (uint32_t)psx_read_byte(VS_ESTADO_ADDR + 2);
    s_frame_ini = (uint32_t)s_frame_count;
    if (!debug_server_fnprobe_start(s_lote, s_nlote, VS_N_RECORDS,
                                    VS_RAM_ABS, 4, VS_RAM_REL, 4, 1)) {
        s_nlote = 0;
        return;
    }
    s_armado = 1;
    s_img_armada = s_img_actual;
}

static void vs_linea(char *out, size_t cap, const VsObj *o,
                     const PsxFnProbeSample *r, const char *estado,
                     uint32_t llamadas_ciclo)
{
    size_t p = 0;
    int k;
#define VS_ADD(...) do { \
        int _n = snprintf(out + (p < cap ? p : cap), \
                          p < cap ? cap - p : 0, __VA_ARGS__); \
        if (_n > 0) p += (size_t)_n; \
    } while (0)
    VS_ADD(
        "{\"fn\":\"0x%08X\",\"nombre\":\"%s\",\"imagen\":\"%s\",\"ret\":\"%s\","
        "\"partido\":\"%s\",\"tick\":%u,\"fase_ini\":%u,\"fase_fin\":%u,"
        "\"estado\":\"%s\",\"pantalla\":\"%s\",\"frame\":%u,\"seq\":%llu,"
        "\"ret_pc\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\","
        "\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"v0\":\"0x%08X\",\"ram\":[",
        o->addr, s_imgs[o->img].nombre, s_imgs[o->img].nombre,
        o->es_void ? "void" : "valor", s_partido,
        (uint32_t)(s_frame_count - s_frame_ini), s_fase_ini,
        (uint32_t)psx_read_byte(VS_ESTADO_ADDR + 2), estado, estado,
        r->frame, (unsigned long long)r->seq, r->ret,
        r->a0, r->a1, r->a2, r->a3, r->v0);
    for (k = 0; k < PSX_FNPROBE_MAX_RAM; k++)
        VS_ADD("%s\"0x%08X\"", k ? "," : "", r->ram[k]);
    VS_ADD("],\"snap\":\"");
    for (k = 0; k < PSX_FNPROBE_SNAP_BYTES; k++)
        VS_ADD("%02x", r->snap[k]);
    VS_ADD("\",\"llamadas_ciclo\":%u}", llamadas_ciclo);
#undef VS_ADD
}

static void vs_volcar(void)
{
    static PsxFnProbeSample recs[VS_N_RECORDS];
    static char linea[4096];
    static char estado[9];
    uint32_t cuentas[PSX_FNPROBE_MAX_FNS];
    int nrec, ncuentas, i, k;
    int elegidas[VS_MAX_POR_CICLO];
    int nelegidas = 0;
    uint32_t nuevas = 0;

    ncuentas = debug_server_fnprobe_counts(cuentas, PSX_FNPROBE_MAX_FNS);
    nrec = debug_server_fnprobe_copy_samples(s_seq_desde, recs, VS_N_RECORDS, NULL);
    debug_server_fnprobe_stop();
    s_armado = 0;

    vs_estado_hex(estado);

    /* Llamadas vistas por funcion (cuenta exacta, no muestreo). */
    for (i = 0; i < s_nlote; i++) {
        VsObj *o = &s_objs[s_lote_obj[i]];
        o->ciclos_armada++;
        if (i < ncuentas) {
            o->llamadas_vistas += cuentas[i];
            if (cuentas[i]) o->ciclos_con_llamadas++;
        }
    }

    /* Elegir hasta VS_MAX_POR_CICLO registros: primero uno con v0 nuevo,
     * despues repartidos entre funciones. */
    if (s_total_sesion < VS_MAX_TOTAL) {
        for (k = 0; k < nrec && nelegidas < VS_MAX_POR_CICLO; k++) {
            VsObj *o = NULL;
            int j, nuevo = 1;
            for (j = 0; j < s_nlote; j++) {
                if (s_lote[j] != recs[k].fn) continue;
                o = &s_objs[s_lote_obj[j]];
                break;
            }
            if (!o) continue;
            for (j = 0; j < o->nv0; j++) if (o->v0[j] == recs[k].v0) { nuevo = 0; break; }
            if (!nuevo) continue;
            elegidas[nelegidas++] = k;
        }
        for (k = 0; k < nrec && nelegidas < VS_MAX_POR_CICLO; k++) {
            int ya = 0, j;
            for (j = 0; j < nelegidas; j++) if (elegidas[j] == k) ya = 1;
            if (!ya) elegidas[nelegidas++] = k;
        }
    }

    for (k = 0; k < nelegidas; k++) {
        const PsxFnProbeSample *r = &recs[elegidas[k]];
        int j;
        VsObj *o = NULL;
        uint32_t llamadas = 0;
        for (j = 0; j < s_nlote; j++) {
            if (s_lote[j] != r->fn) continue;
            o = &s_objs[s_lote_obj[j]];
            if (j < ncuentas) llamadas = cuentas[j];
            break;
        }
        if (!o) continue;
        o->muestras++;
        s_total_sesion++;
        if (r->v0 == 0) o->cero++; else o->no_cero++;
        vs_recuerda(o->v0, &o->nv0, r->v0);
        vs_recuerda(o->a0, &o->na0, r->a0);
        vs_linea(linea, sizeof(linea), o, r, estado, llamadas);
        if (s_jsonl) {
            fputs(linea, s_jsonl);
            fputc('\n', s_jsonl);
        }
        nuevas++;
    }
    if (s_jsonl) fflush(s_jsonl);
    s_escritas += nuevas;
    s_ciclos++;
    if ((s_ciclos % 30) == 0) vs_guardar_estado();
}

/* ---- ZIP (deflate) ----------------------------------------------------- */
static void vs_u16(unsigned char *b, size_t *p, uint32_t v)
{
    b[(*p)++] = (unsigned char)(v & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 8) & 0xFF);
}

static void vs_u32(unsigned char *b, size_t *p, uint32_t v)
{
    b[(*p)++] = (unsigned char)(v & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 8) & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 16) & 0xFF);
    b[(*p)++] = (unsigned char)((v >> 24) & 0xFF);
}

static int vs_comprimir(const char *origen, const char *tmp,
                        uint32_t *crc_out, uint32_t *csize_out,
                        uint32_t *usize_out)
{
    static unsigned char in[65536];
    static unsigned char out[65536];
    FILE *fi = fopen(origen, "rb");
    FILE *fo;
    z_stream zs;
    uint32_t crc = (uint32_t)crc32(0L, Z_NULL, 0);
    uint64_t us = 0, cs = 0;
    int flush, ret;

    if (!fi) return 0;
    fo = fopen(tmp, "wb");
    if (!fo) { fclose(fi); return 0; }
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        fclose(fi); fclose(fo); return 0;
    }
    do {
        size_t n = fread(in, 1, sizeof(in), fi);
        if (ferror(fi)) { deflateEnd(&zs); fclose(fi); fclose(fo); return 0; }
        flush = feof(fi) ? Z_FINISH : Z_NO_FLUSH;
        if (n) { crc = (uint32_t)crc32(crc, in, (uInt)n); us += n; }
        zs.next_in = in;
        zs.avail_in = (uInt)n;
        do {
            size_t have;
            zs.next_out = out;
            zs.avail_out = sizeof(out);
            ret = deflate(&zs, flush);
            if (ret == Z_STREAM_ERROR) {
                deflateEnd(&zs); fclose(fi); fclose(fo); return 0;
            }
            have = sizeof(out) - zs.avail_out;
            if (have) {
                if (fwrite(out, 1, have, fo) != have) {
                    deflateEnd(&zs); fclose(fi); fclose(fo); return 0;
                }
                cs += have;
            }
        } while (zs.avail_out == 0);
    } while (flush != Z_FINISH);
    deflateEnd(&zs);
    fclose(fi);
    if (fclose(fo) != 0) return 0;

    *crc_out   = crc;
    *csize_out = (uint32_t)cs;
    *usize_out = (uint32_t)us;
    return 1;
}

static int vs_escribir_zip(const char *zip, const char *origen, const char *nombre)
{
    char tmp[720];
    unsigned char cab[30 + 256];
    unsigned char cen[46 + 256];
    unsigned char eoc[22];
    unsigned char buf[65536];
    uint32_t crc = 0, csize = 0, usize = 0;
    size_t ncab = 0, ncen = 0, neoc = 0, nlen;
    FILE *f, *ft;
    time_t ahora = time(NULL);
    struct tm *lt = localtime(&ahora);
    uint32_t fecha = 0, hora = 0;

    snprintf(tmp, sizeof(tmp), "%s.tmp", zip);
    if (!vs_comprimir(origen, tmp, &crc, &csize, &usize)) {
        remove(tmp);
        return 0;
    }
    if (lt) {
        fecha = (uint32_t)(((lt->tm_year + 1900 - 1980) << 9) |
                           ((lt->tm_mon + 1) << 5) | lt->tm_mday);
        hora  = (uint32_t)((lt->tm_hour << 11) | (lt->tm_min << 5) |
                           (lt->tm_sec / 2));
    }
    nlen = strlen(nombre);
    if (nlen > 200) nlen = 200;

    vs_u32(cab, &ncab, 0x04034B50u);
    vs_u16(cab, &ncab, 20);              /* version needed   */
    vs_u16(cab, &ncab, 0);               /* flags            */
    vs_u16(cab, &ncab, 8);               /* method = deflate */
    vs_u16(cab, &ncab, hora);
    vs_u16(cab, &ncab, fecha);
    vs_u32(cab, &ncab, crc);
    vs_u32(cab, &ncab, csize);
    vs_u32(cab, &ncab, usize);
    vs_u16(cab, &ncab, (uint32_t)nlen);
    vs_u16(cab, &ncab, 0);
    memcpy(cab + ncab, nombre, nlen);
    ncab += nlen;

    vs_u32(cen, &ncen, 0x02014B50u);
    vs_u16(cen, &ncen, 20);              /* version made by   */
    vs_u16(cen, &ncen, 20);              /* version needed    */
    vs_u16(cen, &ncen, 0);
    vs_u16(cen, &ncen, 8);
    vs_u16(cen, &ncen, hora);
    vs_u16(cen, &ncen, fecha);
    vs_u32(cen, &ncen, crc);
    vs_u32(cen, &ncen, csize);
    vs_u32(cen, &ncen, usize);
    vs_u16(cen, &ncen, (uint32_t)nlen);
    vs_u16(cen, &ncen, 0);               /* extra      */
    vs_u16(cen, &ncen, 0);               /* comment    */
    vs_u16(cen, &ncen, 0);               /* disk       */
    vs_u16(cen, &ncen, 0);               /* int attrs  */
    vs_u32(cen, &ncen, 0);               /* ext attrs  */
    vs_u32(cen, &ncen, 0);               /* local hdr offset (one entry) */
    memcpy(cen + ncen, nombre, nlen);
    ncen += nlen;

    vs_u32(eoc, &neoc, 0x06054B50u);
    vs_u16(eoc, &neoc, 0);
    vs_u16(eoc, &neoc, 0);
    vs_u16(eoc, &neoc, 1);
    vs_u16(eoc, &neoc, 1);
    vs_u32(eoc, &neoc, (uint32_t)ncen);
    vs_u32(eoc, &neoc, (uint32_t)(ncab + csize));
    vs_u16(eoc, &neoc, 0);

    f = fopen(zip, "wb");
    if (!f) { remove(tmp); return 0; }
    if (fwrite(cab, 1, ncab, f) != ncab) goto fallo;
    ft = fopen(tmp, "rb");
    if (!ft) goto fallo;
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), ft);
        if (n && fwrite(buf, 1, n, f) != n) { fclose(ft); goto fallo; }
        if (n < sizeof(buf)) break;
    }
    fclose(ft);
    if (fwrite(cen, 1, ncen, f) != ncen) goto fallo;
    if (fwrite(eoc, 1, neoc, f) != neoc) goto fallo;
    if (fclose(f) != 0) { remove(tmp); return 0; }
    remove(tmp);
    return 1;
fallo:
    fclose(f);
    remove(tmp);
    remove(zip);
    return 0;
}

/* ---- Arranque / cierre -------------------------------------------------- */
static void vs_escribir_aviso(const char *nombre, const char *texto)
{
    char ruta[700];
    FILE *f;
    snprintf(ruta, sizeof(ruta), "%s/%s", s_dir_muestras, nombre);
    f = fopen(ruta, "w");
    if (!f) return;
    fputs(texto, f);
    fclose(f);
}

void volunteer_sampling_init(const char *carpeta, const char *exe_dir,
                             int disco_ok, const char *detalle)
{
    time_t ahora = time(NULL);
    struct tm *lt;
    char exe[512];

    if (s_activo || !carpeta || !carpeta[0]) return;
    s_activo = 1;
    vs_copia(s_carpeta, sizeof(s_carpeta), carpeta);
    vs_copia(s_dir_muestras, sizeof(s_dir_muestras), carpeta);
    vs_copia(exe, sizeof(exe), exe_dir);
    snprintf(s_ruta_objetivos, sizeof(s_ruta_objetivos),
             "%s/objetivos_voluntarios.tsv", exe);

    VS_MKDIR(s_carpeta);

    if (!disco_ok) {
        char t[1024];
        snprintf(t, sizeof(t),
            "Muestreo desactivado: el disco montado no es SLPM-87056.\n"
            "Detalle: %s\n\n"
            "No se grabo ninguna muestra. Use su propia copia japonesa del\n"
            "juego (Winning Eleven 2002, SLPM-87056).\n",
            detalle ? detalle : "(sin detalle)");
        vs_escribir_aviso("NO_GRABADO.txt", t);
        fprintf(stdout, "muestreo: disco distinto de SLPM-87056; no se graba nada\n");
        return;
    }

    if (!vs_cargar_objetivos()) {
        vs_escribir_aviso("FALTA_OBJETIVOS.txt",
            "Muestreo desactivado: no se pudo leer objetivos_voluntarios.tsv\n"
            "junto al .exe, o no tiene funciones. Vuelva a descomprimir el\n"
            "paquete completo (el .tsv debe quedar al lado del .exe).\n");
        fprintf(stdout, "muestreo: falta objetivos_voluntarios.tsv valido\n");
        return;
    }

    lt = localtime(&ahora);
    if (lt)
        snprintf(s_partido, sizeof(s_partido), "%04d%02d%02d_%02d%02d%02d",
                 lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                 lt->tm_hour, lt->tm_min, lt->tm_sec);
    else
        vs_copia(s_partido, sizeof(s_partido), "00000000_000000");

    snprintf(s_ruta_jsonl, sizeof(s_ruta_jsonl), "%s/muestras_%s.jsonl",
             s_dir_muestras, s_partido);
    snprintf(s_ruta_estado, sizeof(s_ruta_estado), "%s/estado_voluntario.tsv",
             s_dir_muestras);
    vs_cargar_estado();
    s_jsonl = fopen(s_ruta_jsonl, "a");
    if (!s_jsonl) {
        vs_escribir_aviso("NO_GRABADO.txt",
            "Muestreo desactivado: no se pudo crear el archivo de muestras.\n"
            "Revise que la carpeta no sea de solo lectura.\n");
        fprintf(stdout, "muestreo: no se pudo abrir el .jsonl\n");
        return;
    }
    /* Repasar las ultimas cargas de CD para saber que imagen esta en RAM ya
     * al arrancar (p. ej. OPENNING.BIN del arranque). */
    {
        uint32_t t = cd_dma_log_get_total();
        s_ultimo_dma = t > VS_DMA_SCAN ? t - VS_DMA_SCAN : 0;
    }
    s_prox_ciclo = s_frame_count;
    s_grabando = 1;
    fprintf(stdout, "muestreo: activo (%d objetivos, %d imagenes) -> %s\n",
            s_nobjs, s_nimgs, s_ruta_jsonl);
}

int volunteer_sampling_activo(void)
{
    return s_grabando;
}

void volunteer_sampling_tick(void)
{
    if (!s_activo || !s_grabando) return;
    if (s_frame_count < s_prox_ciclo) return;
    s_prox_ciclo = s_frame_count + VS_CICLO_FRAMES;

    vs_actualizar_imagen();

    /* La imagen cambio mientras habia un lote armado: volcar lo grabado (era de
     * la imagen anterior) y rearmar con la nueva. */
    if (s_armado && s_img_armada != s_img_actual) {
        vs_volcar();
        vs_armar();
        return;
    }
    if (!s_armado) {
        vs_armar();
        return;
    }
    vs_volcar();
    vs_armar();
}

void volunteer_sampling_shutdown(void)
{
    if (!s_activo) return;
    if (s_armado) {
        debug_server_fnprobe_stop();
        s_armado = 0;
    }
    if (s_jsonl) {
        fflush(s_jsonl);
        fclose(s_jsonl);
        s_jsonl = NULL;
    }
    if (s_grabando) {
        char zip[720], nombre[64];
        vs_guardar_estado();
        snprintf(nombre, sizeof(nombre), "muestras_%s.jsonl", s_partido);
        snprintf(zip, sizeof(zip), "%s/muestras_%s.zip", s_dir_muestras, s_partido);
        if (vs_escribir_zip(zip, s_ruta_jsonl, nombre)) {
            fprintf(stdout, "muestreo: %u muestras -> %s\n", s_escritas, zip);
        } else {
            fprintf(stdout, "muestreo: %u muestras; fallo el .zip (el .jsonl queda: %s)\n",
                    s_escritas, s_ruta_jsonl);
        }
    }
    s_grabando = 0;
    s_activo = 0;
    free(s_objs); s_objs = NULL;
    free(s_orden); s_orden = NULL;
    free(s_buf_tsv); s_buf_tsv = NULL;
}
