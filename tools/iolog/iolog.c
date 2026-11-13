/*
 * iolog: a QEMU TCG plugin that keeps a timeline of the loads and stores to
 * a range of guest physical addresses, device registers included, with the
 * values, and of the times chosen instructions run.
 *
 * Usage (run.sh passes PLUGIN to QEMU's -plugin option):
 *   PLUGIN=tools/iolog/build/libiolog.dylib,lo=0x800600A0,hi=0x800600C0,mark=0xF12A14,regs=r9:r10,out=run/io.txt ./run.sh
 *
 * Options:
 *   lo=ADDR, hi=ADDR  the physical range [lo, hi) to log (required);
 *   mark=ADDR         an instruction to log each time it runs (a logical
 *                     address; for NanoKernel code, which runs with
 *                     translation off, its physical address); up to 8;
 *   regs=R[:R...]     registers to record at a mark, by their gdb names
 *                     (r0-r31, lr, ctr, msr, ...), up to 4;
 *   n=N               how many of the latest events to keep (default 4096);
 *   out=FILE          the report (default iolog.txt).
 *
 * The report, written when QEMU exits, lists the kept events oldest first:
 * the guest instruction count, the PC, the kind (R or W for an access, M
 * for a mark), then the physical address, size and value of an access, or
 * the registers at a mark. Totals of each kind come first.
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_MARKS 8
#define MAX_REGS 4

typedef struct {
    uint64_t insn;
    uint32_t pc;
    char kind;                  /* 'R', 'W' or 'M' */
    uint8_t size;
    uint32_t addr;              /* an access: its physical address */
    uint32_t v[MAX_REGS];       /* an access: v[0], its value; a mark: regs */
} Event;

static uint64_t lo, hi;
static uint64_t marks[MAX_MARKS];
static int nmarks;
static char *names[MAX_REGS];
static int nregs;
static struct qemu_plugin_register *regs[MAX_REGS];
static GByteArray *regbuf;
static const char *out = "iolog.txt";
static Event *ring;
static uint64_t ring_n = 4096, ring_next;
static uint64_t totals[3];      /* R, W, M */
static GMutex lock;
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 insns;

static uint32_t read_reg(struct qemu_plugin_register *r)
{
    g_byte_array_set_size(regbuf, 0);
    if (!r || !qemu_plugin_read_register(r, regbuf) || regbuf->len < 4)
        return 0;
    const uint8_t *b = regbuf->data + regbuf->len - 4;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

/* The next slot in the ring; call with the lock held. */
static Event *next_event(void)
{
    Event *e = &ring[ring_next++ % ring_n];
    memset(e, 0, sizeof *e);
    e->insn = qemu_plugin_u64_sum(insns);
    return e;
}

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw)
        return;
    uint64_t paddr = qemu_plugin_hwaddr_phys_addr(hw);
    unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    if (paddr + size <= lo || paddr >= hi)
        return;

    qemu_plugin_mem_value val = qemu_plugin_mem_get_value(info);
    uint32_t v;
    switch (val.type) {
    case QEMU_PLUGIN_MEM_VALUE_U8: v = val.data.u8; break;
    case QEMU_PLUGIN_MEM_VALUE_U16: v = val.data.u16; break;
    case QEMU_PLUGIN_MEM_VALUE_U32: v = val.data.u32; break;
    case QEMU_PLUGIN_MEM_VALUE_U64: v = (uint32_t)val.data.u64; break;
    default: v = 0; break;
    }
    int write = qemu_plugin_mem_is_store(info);
    g_mutex_lock(&lock);
    Event *e = next_event();
    e->pc = (uintptr_t)udata;
    e->kind = write ? 'W' : 'R';
    e->size = size;
    e->addr = paddr;
    e->v[0] = v;
    totals[write]++;
    g_mutex_unlock(&lock);
}

static void on_mark(unsigned int vcpu, void *udata)
{
    g_mutex_lock(&lock);
    Event *e = next_event();
    e->pc = (uintptr_t)udata;
    e->kind = 'M';
    for (int i = 0; i < nregs; i++)
        e->v[i] = read_reg(regs[i]);
    totals[2]++;
    g_mutex_unlock(&lock);
}

static void tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, insns, n);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uintptr_t pc = (uintptr_t)qemu_plugin_insn_vaddr(insn);
        qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_RW, (void *)pc);
        for (int m = 0; m < nmarks; m++)
            if (pc == marks[m])
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, on_mark,
                    nregs ? QEMU_PLUGIN_CB_R_REGS : QEMU_PLUGIN_CB_NO_REGS,
                    (void *)pc);
    }
}

static void on_vcpu_init(unsigned int vcpu, void *userdata)
{
    GArray *all = qemu_plugin_get_registers();
    for (guint i = 0; all && i < all->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(all, qemu_plugin_reg_descriptor, i);
        for (int j = 0; j < nregs; j++)
            if (!strcmp(d->name, names[j]))
                regs[j] = d->handle;
    }
    for (int j = 0; j < nregs; j++)
        if (!regs[j])
            fprintf(stderr, "iolog: no register %s\n", names[j]);
    if (all)
        g_array_free(all, TRUE);
}

static void on_exit(void *userdata)
{
    FILE *f = fopen(out, "w");
    if (!f) {
        perror(out);
        return;
    }
    fprintf(f, "# physical range 0x%" PRIx64 "-0x%" PRIx64 "; marks", lo, hi);
    for (int m = 0; m < nmarks; m++)
        fprintf(f, " 0x%" PRIx64, marks[m]);
    fprintf(f, "\n# totals: R %" PRIu64 ", W %" PRIu64 ", M %" PRIu64 "\n",
            totals[0], totals[1], totals[2]);
    fprintf(f, "#       insn  pc        kind  addr/regs\n");
    uint64_t first = ring_next > ring_n ? ring_next - ring_n : 0;
    for (uint64_t i = first; i < ring_next; i++) {
        Event *e = &ring[i % ring_n];
        fprintf(f, "%12" PRIu64 "  %08x  %c", e->insn, e->pc, e->kind);
        if (e->kind == 'M') {
            for (int j = 0; j < nregs; j++)
                fprintf(f, "  %s=%08x", names[j], e->v[j]);
        } else {
            fprintf(f, "  %08x/%u  %08x", e->addr, e->size, e->v[0]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "lo=", 3)) {
            lo = g_ascii_strtoull(argv[i] + 3, NULL, 0);
        } else if (!strncmp(argv[i], "hi=", 3)) {
            hi = g_ascii_strtoull(argv[i] + 3, NULL, 0);
        } else if (!strncmp(argv[i], "mark=", 5)) {
            if (nmarks == MAX_MARKS) {
                fprintf(stderr, "iolog: at most %d marks\n", MAX_MARKS);
                return -1;
            }
            marks[nmarks++] = g_ascii_strtoull(argv[i] + 5, NULL, 0);
        } else if (!strncmp(argv[i], "regs=", 5)) {
            gchar **parts = g_strsplit(argv[i] + 5, ":", -1);
            for (int j = 0; parts[j]; j++) {
                if (nregs == MAX_REGS) {
                    fprintf(stderr, "iolog: at most %d registers\n", MAX_REGS);
                    return -1;
                }
                names[nregs++] = g_strdup(parts[j]);
            }
            g_strfreev(parts);
        } else if (!strncmp(argv[i], "n=", 2)) {
            ring_n = g_ascii_strtoull(argv[i] + 2, NULL, 0);
        } else if (!strncmp(argv[i], "out=", 4)) {
            out = argv[i] + 4;
        } else {
            fprintf(stderr, "iolog: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    if (hi <= lo || ring_n == 0) {
        fprintf(stderr, "iolog: needs lo=ADDR and hi=ADDR, lo < hi, n > 0\n");
        return -1;
    }
    ring = g_new0(Event, ring_n);
    regbuf = g_byte_array_new();
    score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(score);
    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
