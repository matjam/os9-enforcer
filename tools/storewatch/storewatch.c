/*
 * storewatch: a QEMU TCG plugin that counts every store the Mac OS 9 guest
 * makes into low memory and the system heap, records which code made each
 * one, and models what protecting the system heap page by page would cost.
 *
 * Usage (run.sh passes PLUGIN to QEMU's -plugin option):
 *   PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/sw ./run.sh
 *
 * Options:
 *   out=PREFIX       where the files go (default "storewatch");
 *   mode=insns       only count guest instructions, to measure how fast the
 *                    guest runs; no store callbacks, so almost no slowdown;
 *   epoch=N[:N...]   model page protection of the system heap with a
 *                    protection interval of N guest instructions, or of one
 *                    tick of the guest's clock (Ticks, 1/60 s) for "ticks";
 *                    up to 6 intervals (see "Page model").
 *
 * It watches stores made with data translation on (MSR DR set) into:
 *   - low memory: below 0x2800, where SysZone starts on 9.2.2;
 *   - the system heap: from there to SysZone's bkLim, read from the guest.
 * Stores with translation off (Open Firmware, parts of the NanoKernel) are
 * only counted.
 *
 * Each store is counted against its site:
 *   - native code: the PC of the store instruction;
 *   - 68k code: a store made by the 68k emulator (code at 0x68000000 to
 *     0x68FFE000) is counted against the 68k instruction after it (r24 - 2;
 *     see common/h68k.s).
 * A store is "app" when that code lies inside the current application's
 * partition, which the plugin reads from the running engine's 68k hook data
 * (cur_lo and cur_hi, 0x68FFFBF0). That is the same test as Enforcer's deny
 * rule. Without a running engine (XLog, Guard or Enforcer), no store is app.
 * One exception: the emulator's own stores into a 68k-to-native call frame
 * (EMU_FRAME_LO to EMU_FRAME_HI) are system stores, although r24 then
 * points at the application's trap. The guest values are read again every
 * 64 watched stores and once a second.
 *
 * Page model: Enforcer protects a page, lets the first store by system code
 * open it (one trap), and protects it again at its next 16 ms tick. For each
 * interval, the plugin replays that on every page of the system heap from
 * page 2 (0x2000) up: at the start of each interval every page is
 * protected; the first system store to a page in an interval is a trap and
 * opens the page until the interval ends. An app store to a protected page
 * is "caught" (the page stays protected); to an open page, "missed". It also
 * adds up how long each page stays open, in instructions, for coverage.
 *
 * The guest runs slower with the plugin, so it does less work per tick of
 * its clock than without; for work that keeps the CPU busy, the "ticks"
 * model then counts more traps than there would be. An interval of
 * instructions does not depend on that: at speed S million instructions a
 * second, 16 ms is S * 16000 instructions.
 *
 * Files:
 *   PREFIX.rate   a line a second: guest instructions and stores by kind;
 *   PREFIX.sites  stores by site, most first, with the instructions run and
 *                 seconds since the last reset (at exit or on request);
 *   PREFIX.words  stores per low memory word (at exit or on request);
 *   PREFIX.pages  the page model (with epoch=; at exit or on request).
 * To clear the counts, create PREFIX.reset; to write the tables now, create
 * PREFIX.dump. The plugin looks for them about once a second and deletes
 * them.
 */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LOW_END   0x2800u     /* end of low memory: SysZone on 9.2.2 */
#define SYS_ZONE  0x2A6u
#define APPL_ZONE 0x2AAu
#define TICKS     0x16Au      /* 60ths of a second since startup */
#define H68K_DATA 0x68FFFB80u /* common/h68k.h kH68kData */
#define D_CUR_LO  0x70u
#define EMU_LO    0x68000000u /* the 68k emulator's code */
#define EMU_HI    0x68FFE000u
/* Emulator code that fills a 68k-to-native call frame (9.2.2). */
#define EMU_FRAME_LO 0x6806E300u
#define EMU_FRAME_HI 0x6806E370u
#define MSR_DR    0x10u
#define REFRESH   64
#define NSITES    (1u << 18)  /* a power of 2 */
#define USED      (1ull << 40)
#define PAGE_SHIFT 12
#define MAX_PAGES 0x4000u     /* system heap up to 64 MB */
#define FIRST_PAGE 2u         /* pages 0 and 1 are all low memory */
#define MAX_MODELS 6
#define TICK_INSNS 32000000u  /* the periodic callback, in instructions */

enum { REGION_LOW, REGION_SYS };
enum { KIND_NATIVE, KIND_68K };

typedef struct {
    uint64_t key; /* USED | region << 33 | kind << 32 | pc; 0 = empty */
    uint64_t total, app;
    uint32_t first, last, zone;
    uint32_t epc; /* 68k sites: the emulator's store instruction, last seen */
} Site;

typedef struct {
    uint64_t stores;    /* every guest store */
    uint64_t real_mode; /* below sys_end with translation off */
    uint64_t low, low_app, sys, sys_app;
} Totals;

/* Per vCPU, in the scoreboard. */
typedef struct {
    uint64_t insns; /* instructions run */
    uint64_t since; /* instructions since the periodic callback */
} Counts;

typedef struct {
    uint64_t interval;            /* N instructions, or 0: the guest's Ticks */
    uint64_t epoch;               /* the current interval's number + 1 */
    uint64_t mark[MAX_PAGES];     /* the epoch in which the page opened */
    uint64_t traps[MAX_PAGES];
    uint64_t open[MAX_PAGES];     /* instructions the page spent open */
    uint64_t opened_at[MAX_PAGES];
    uint32_t opened[MAX_PAGES];   /* pages opened in the current epoch */
    uint32_t n_opened;
    uint64_t epochs;              /* intervals that saw a store */
    uint64_t caught, missed;      /* app stores to protected / open pages */
} Model;

static Site *sites;
static uint64_t n_sites, sites_dropped;
static uint64_t word_total[LOW_END / 4], word_app[LOW_END / 4];
static Totals tot, tot_last;
static uint64_t insns_at_reset;
static double t_reset;

static Model *models[MAX_MODELS];
static int n_models;
static int insns_only;

static const char *prefix = "storewatch";
static FILE *rate;
static struct qemu_plugin_register *reg_r24, *reg_msr;
static GByteArray *regbuf, *membuf;
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 insns, since;
static uint64_t insns_last;
static double t_start, t_last;

/* From the guest, refreshed every REFRESH watched stores. */
static uint32_t sys_end = LOW_END, cur_lo, cur_hi, appl_zone, guest_ticks;
static unsigned since_refresh = REFRESH;

static double wall(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static char *path(const char *ext)
{
    return g_strdup_printf("%s.%s", prefix, ext);
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
           (uint32_t)b[2] << 8 | b[3];
}

/* A register of the current vCPU (PowerPC: big-endian, 4 bytes). */
static uint32_t read_reg(struct qemu_plugin_register *r)
{
    g_byte_array_set_size(regbuf, 0);
    if (!r || !qemu_plugin_read_register(r, regbuf) || regbuf->len < 4)
        return 0;
    return be32(regbuf->data + regbuf->len - 4);
}

static int read_u32(uint32_t addr, uint32_t *out)
{
    g_byte_array_set_size(membuf, 0);
    if (!qemu_plugin_read_memory_vaddr(addr, membuf, 4) || membuf->len < 4)
        return 0;
    *out = be32(membuf->data);
    return 1;
}

static void refresh(void)
{
    uint32_t zone, lim, magic, lo, hi, az, t;
    since_refresh = 0;
    if (read_u32(TICKS, &t))
        guest_ticks = t;
    if (read_u32(SYS_ZONE, &zone) && zone && zone < 0x10000 &&
        read_u32(zone, &lim) && lim > LOW_END &&
        lim < (MAX_PAGES << PAGE_SHIFT))
        sys_end = lim; /* bkLim, the zone header's first field */
    if (read_u32(APPL_ZONE, &az))
        appl_zone = az;
    cur_lo = cur_hi = 0;
    if (read_u32(H68K_DATA, &magic) && (magic >> 8) == 0x483638 && /* 'H68' */
        read_u32(H68K_DATA + D_CUR_LO, &lo) &&
        read_u32(H68K_DATA + D_CUR_LO + 4, &hi) && lo < hi) {
        cur_lo = lo;
        cur_hi = hi;
    }
}

static Site *site_for(uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    uint32_t j = (uint32_t)(h >> 40) & (NSITES - 1);
    for (int i = 0; i < 64; i++, j = (j + 1) & (NSITES - 1)) {
        Site *s = &sites[j];
        if (s->key == key)
            return s;
        if (!s->key) {
            if (n_sites >= NSITES / 4 * 3)
                return NULL;
            s->key = key;
            n_sites++;
            return s;
        }
    }
    return NULL;
}

static int by_total(const void *a, const void *b)
{
    const Site *x = *(const Site *const *)a, *y = *(const Site *const *)b;
    return x->total < y->total ? 1 : x->total > y->total ? -1 : 0;
}

static void close_epoch(Model *md, uint64_t end);

static void dump_pages(uint64_t ran)
{
    char *p = path("pages");
    FILE *f = fopen(p, "w");
    g_free(p);
    if (!f)
        return;
    /* Count the open time of the current epoch so far. */
    uint64_t now = qemu_plugin_u64_sum(insns);
    for (int m = 0; m < n_models; m++)
        close_epoch(models[m], now);
    uint32_t last = (sys_end - 1) >> PAGE_SHIFT;
    fprintf(f, "# insns %" PRIu64 " first_page %u last_page %u models %d\n",
            ran, FIRST_PAGE, last, n_models);
    for (int m = 0; m < n_models; m++)
        fprintf(f, "# model %d interval %" PRIu64 " epochs %" PRIu64
                " caught %" PRIu64 " missed %" PRIu64 "\n", m,
                models[m]->interval, models[m]->epochs, models[m]->caught,
                models[m]->missed);
    fprintf(f, "# page then traps and open instructions for each model\n");
    for (uint32_t pg = FIRST_PAGE; pg <= last && pg < MAX_PAGES; pg++) {
        int any = 0;
        for (int m = 0; m < n_models; m++)
            any |= models[m]->traps[pg] != 0;
        if (!any)
            continue;
        fprintf(f, "%08X", pg << PAGE_SHIFT);
        for (int m = 0; m < n_models; m++)
            fprintf(f, " %" PRIu64 " %" PRIu64, models[m]->traps[pg],
                    models[m]->open[pg]);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void dump(void)
{
    uint64_t ran = qemu_plugin_u64_sum(insns) - insns_at_reset;
    char *p = path("sites");
    FILE *f = fopen(p, "w");
    g_free(p);
    if (f) {
        Site **list = g_new(Site *, n_sites ? n_sites : 1);
        uint64_t n = 0;
        for (uint32_t i = 0; i < NSITES; i++)
            if (sites[i].key)
                list[n++] = &sites[i];
        qsort(list, n, sizeof *list, by_total);
        fprintf(f, "# insns %" PRIu64 " secs %.1f stores %" PRIu64
                " real_mode %" PRIu64 " low %" PRIu64 " low_app %" PRIu64
                " sys %" PRIu64 " sys_app %" PRIu64 " sites %" PRIu64
                " dropped %" PRIu64 " sys_end %08X\n",
                ran, wall() - t_reset, tot.stores, tot.real_mode, tot.low,
                tot.low_app, tot.sys, tot.sys_app, n, sites_dropped, sys_end);
        fprintf(f, "# region kind pc total app first last zone epc\n");
        for (uint64_t i = 0; i < n; i++) {
            Site *s = list[i];
            fprintf(f, "%s %s %08X %" PRIu64 " %" PRIu64 " %08X %08X %08X "
                    "%08X\n",
                    (s->key >> 33 & 1) == REGION_LOW ? "low" : "sys",
                    (s->key >> 32 & 1) == KIND_68K ? "68k" : "native",
                    (uint32_t)s->key, s->total, s->app, s->first, s->last,
                    s->zone, s->epc);
        }
        g_free(list);
        fclose(f);
    }
    p = path("words");
    f = fopen(p, "w");
    g_free(p);
    if (f) {
        fprintf(f, "# addr total app\n");
        for (uint32_t i = 0; i < LOW_END / 4; i++)
            if (word_total[i])
                fprintf(f, "%04X %" PRIu64 " %" PRIu64 "\n", i * 4,
                        word_total[i], word_app[i]);
        fclose(f);
    }
    if (n_models)
        dump_pages(ran);
}

static void reset(void)
{
    memset(sites, 0, NSITES * sizeof(Site));
    n_sites = sites_dropped = 0;
    memset(word_total, 0, sizeof word_total);
    memset(word_app, 0, sizeof word_app);
    memset(&tot, 0, sizeof tot);
    memset(&tot_last, 0, sizeof tot_last);
    for (int m = 0; m < n_models; m++) {
        uint64_t interval = models[m]->interval;
        memset(models[m], 0, sizeof(Model));
        models[m]->interval = interval;
    }
    insns_at_reset = qemu_plugin_u64_sum(insns);
    t_reset = wall();
}

/* Take a control file's request: 1 if it was there. */
static int take(const char *ext)
{
    char *p = path(ext);
    int there = access(p, F_OK) == 0;
    if (there)
        unlink(p);
    g_free(p);
    return there;
}

static void tick(void)
{
    double now = wall();
    if (now - t_last < 1.0)
        return;
    refresh();
    uint64_t n = qemu_plugin_u64_sum(insns);
    if (rate) {
        fprintf(rate, "%.1f insns %" PRIu64 " stores %" PRIu64
                " real_mode %" PRIu64 " low %" PRIu64 " low_app %" PRIu64
                " sys %" PRIu64 " sys_app %" PRIu64 " zone %08X cur %08X-%08X\n",
                now - t_start, n - insns_last, tot.stores - tot_last.stores,
                tot.real_mode - tot_last.real_mode, tot.low - tot_last.low,
                tot.low_app - tot_last.low_app, tot.sys - tot_last.sys,
                tot.sys_app - tot_last.sys_app, appl_zone, cur_lo, cur_hi);
        fflush(rate);
    }
    tot_last = tot;
    insns_last = n;
    t_last = now;
    if (take("reset")) {
        reset();
        if (rate)
            fprintf(rate, "# reset at %.1f\n", now - t_start);
    }
    if (take("dump"))
        dump();
}

static void on_period(unsigned int vcpu, void *userdata)
{
    qemu_plugin_u64_set(since, vcpu, 0);
    tick();
}

/* End a model's epoch at instruction count end: every page opened in it is
   protected again. */
static void close_epoch(Model *md, uint64_t end)
{
    for (uint32_t i = 0; i < md->n_opened; i++) {
        uint32_t pg = md->opened[i];
        if (end > md->opened_at[pg])
            md->open[pg] += end - md->opened_at[pg];
    }
    md->n_opened = 0;
}

/* Replay the page model for one watched store into the system heap. */
static void model_store(unsigned int vcpu, uint32_t vaddr, int app)
{
    uint32_t pg = vaddr >> PAGE_SHIFT;
    if (pg < FIRST_PAGE || pg >= MAX_PAGES)
        return;
    uint64_t now = qemu_plugin_u64_get(insns, vcpu);
    for (int m = 0; m < n_models; m++) {
        Model *md = models[m];
        uint64_t e = md->interval ? now / md->interval + 1
                                  : (uint64_t)guest_ticks + 1;
        if (e != md->epoch) {
            /* An interval of instructions ends at a known count; a tick
               ends somewhere before this store, which is close enough. */
            uint64_t end = now;
            if (md->interval && md->epoch &&
                md->epoch * md->interval < now)
                end = md->epoch * md->interval;
            close_epoch(md, end);
            md->epoch = e;
            md->epochs++;
        }
        if (md->mark[pg] == e) { /* open */
            md->missed += app;
            continue;
        }
        if (app) {               /* denied: the page stays protected */
            md->caught++;
            continue;
        }
        md->mark[pg] = e;        /* a trap opens it until the epoch ends */
        md->traps[pg]++;
        md->opened_at[pg] = now;
        md->opened[md->n_opened++] = pg;
    }
}

static void on_store(unsigned int vcpu, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *udata)
{
    tot.stores++;
    if ((tot.stores & 0xFFFF) == 0)
        tick();
    if (vaddr >= sys_end)
        return;
    if (!(read_reg(reg_msr) & MSR_DR)) {
        tot.real_mode++;
        return;
    }
    if (++since_refresh >= REFRESH)
        refresh();
    if (vaddr >= sys_end)
        return;

    uint32_t pc = (uint32_t)(uintptr_t)udata, code = pc;
    int kind = KIND_NATIVE;
    if (pc >= EMU_LO && pc < EMU_HI) {
        kind = KIND_68K;
        code = read_reg(reg_r24) - 2;
    }
    int app = cur_hi && code >= cur_lo && code < cur_hi &&
              !(pc >= EMU_FRAME_LO && pc < EMU_FRAME_HI);
    int region = vaddr < LOW_END ? REGION_LOW : REGION_SYS;
    if (region == REGION_LOW) {
        tot.low++;
        tot.low_app += app;
        word_total[vaddr >> 2]++;
        word_app[vaddr >> 2] += app;
    } else {
        tot.sys++;
        tot.sys_app += app;
    }
    if (n_models)
        model_store(vcpu, (uint32_t)vaddr, app);
    Site *s = site_for(USED | (uint64_t)region << 33 | (uint64_t)kind << 32 |
                       code);
    if (!s) {
        sites_dropped++;
        return;
    }
    if (!s->total)
        s->first = (uint32_t)vaddr;
    s->total++;
    s->app += app;
    s->last = (uint32_t)vaddr;
    s->zone = appl_zone;
    if (kind == KIND_68K)
        s->epc = pc;
}

static void on_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, insns, n);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, since, n);
    qemu_plugin_register_vcpu_tb_exec_cond_cb(
        tb, on_period, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_COND_GE, since,
        TICK_INSNS, NULL);
    if (insns_only)
        return;
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uintptr_t pc = (uintptr_t)qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_mem_cb(insn, on_store, QEMU_PLUGIN_CB_R_REGS,
                                         QEMU_PLUGIN_MEM_W, (void *)pc);
    }
}

static void on_vcpu_init(unsigned int vcpu, void *userdata)
{
    GArray *regs = qemu_plugin_get_registers();
    for (guint i = 0; regs && i < regs->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!strcmp(d->name, "r24"))
            reg_r24 = d->handle;
        else if (!strcmp(d->name, "msr"))
            reg_msr = d->handle;
    }
    if (!reg_r24 || !reg_msr)
        fprintf(stderr, "storewatch: no r24 or msr register\n");
    if (regs)
        g_array_free(regs, TRUE);
}

static void on_exit(void *userdata)
{
    dump();
    if (rate)
        fclose(rate);
}

static int parse_epochs(const char *list)
{
    gchar **parts = g_strsplit(list, ":", -1);
    int ok = 1;
    for (int i = 0; parts[i]; i++) {
        int ticks = !strcmp(parts[i], "ticks");
        uint64_t n = ticks ? 0 : g_ascii_strtoull(parts[i], NULL, 0);
        if ((!n && !ticks) || n_models == MAX_MODELS) {
            ok = 0;
            break;
        }
        models[n_models] = g_new0(Model, 1);
        models[n_models++]->interval = n;
    }
    g_strfreev(parts);
    return ok;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) {
            prefix = argv[i] + 4;
        } else if (!strcmp(argv[i], "mode=insns")) {
            insns_only = 1;
        } else if (!strncmp(argv[i], "epoch=", 6)) {
            if (!parse_epochs(argv[i] + 6)) {
                fprintf(stderr, "storewatch: bad epoch list %s\n", argv[i]);
                return -1;
            }
        } else {
            fprintf(stderr, "storewatch: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    sites = g_new0(Site, NSITES);
    regbuf = g_byte_array_new();
    membuf = g_byte_array_new();
    score = qemu_plugin_scoreboard_new(sizeof(Counts));
    insns = qemu_plugin_scoreboard_u64_in_struct(score, Counts, insns);
    since = qemu_plugin_scoreboard_u64_in_struct(score, Counts, since);
    char *p = path("rate");
    rate = fopen(p, "w");
    g_free(p);
    t_start = t_last = t_reset = wall();

    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
