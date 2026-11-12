/*
 * storewatch: a QEMU TCG plugin that counts every store the Mac OS 9 guest
 * makes into low memory and the system heap, and records which code made
 * each one.
 *
 * Usage (run.sh passes PLUGIN to QEMU's -plugin option):
 *   PLUGIN=tools/storewatch/build/libstorewatch.dylib,out=run/sw ./run.sh
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
 * The guest values are read again every 64 watched stores.
 *
 * Files, with out=PREFIX (default "storewatch"):
 *   PREFIX.rate   a line a second: guest instructions and stores by kind;
 *   PREFIX.sites  stores by site, most first, with the instructions run
 *                 since the last reset (at exit or on request);
 *   PREFIX.words  stores per low memory word (at exit or on request).
 * To clear the counts, create PREFIX.reset; to write the two tables now,
 * create PREFIX.dump. The plugin looks for them about once a second and
 * deletes them.
 */

#include <inttypes.h>
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
#define H68K_DATA 0x68FFFB80u /* common/h68k.h kH68kData */
#define D_CUR_LO  0x70u
#define EMU_LO    0x68000000u /* the 68k emulator's code */
#define EMU_HI    0x68FFE000u
#define MSR_DR    0x10u
#define REFRESH   64
#define NSITES    (1u << 18)  /* a power of 2 */
#define USED      (1ull << 40)

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

static Site *sites;
static uint64_t n_sites, sites_dropped;
static uint64_t word_total[LOW_END / 4], word_app[LOW_END / 4];
static Totals tot, tot_last;
static uint64_t insns_at_reset;

static const char *prefix = "storewatch";
static FILE *rate;
static struct qemu_plugin_register *reg_r24, *reg_msr;
static GByteArray *regbuf, *membuf;
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 insns;
static uint64_t insns_last;
static double t_start, t_last;

/* From the guest, refreshed every REFRESH watched stores. */
static uint32_t sys_end = LOW_END, cur_lo, cur_hi, appl_zone;
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
    uint32_t zone, lim, magic, lo, hi, az;
    since_refresh = 0;
    if (read_u32(SYS_ZONE, &zone) && zone && zone < 0x10000 &&
        read_u32(zone, &lim) && lim > LOW_END && lim < 0x40000000)
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

static void dump(void)
{
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
        fprintf(f, "# insns %" PRIu64 " stores %" PRIu64 " real_mode %" PRIu64
                " low %" PRIu64 " low_app %" PRIu64 " sys %" PRIu64
                " sys_app %" PRIu64 " sites %" PRIu64 " dropped %" PRIu64
                " sys_end %08X\n",
                qemu_plugin_u64_sum(insns) - insns_at_reset, tot.stores,
                tot.real_mode, tot.low, tot.low_app, tot.sys, tot.sys_app, n,
                sites_dropped, sys_end);
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
}

static void reset(void)
{
    memset(sites, 0, NSITES * sizeof(Site));
    n_sites = sites_dropped = 0;
    memset(word_total, 0, sizeof word_total);
    memset(word_app, 0, sizeof word_app);
    memset(&tot, 0, sizeof tot);
    memset(&tot_last, 0, sizeof tot_last);
    insns_at_reset = qemu_plugin_u64_sum(insns);
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
    int app = cur_hi && code >= cur_lo && code < cur_hi;
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

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) {
            prefix = argv[i] + 4;
        } else {
            fprintf(stderr, "storewatch: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    sites = g_new0(Site, NSITES);
    regbuf = g_byte_array_new();
    membuf = g_byte_array_new();
    score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(score);
    char *p = path("rate");
    rate = fopen(p, "w");
    g_free(p);
    t_start = t_last = wall();

    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
