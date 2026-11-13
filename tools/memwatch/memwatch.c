/*
 * memwatch: a QEMU TCG plugin that reports which code reads or writes a
 * range of guest physical memory.
 *
 * Usage (run.sh passes PLUGIN to QEMU's -plugin option):
 *   PLUGIN=tools/memwatch/build/libmemwatch.dylib,lo=0xF29B40,hi=0xF60000,out=run/mw.txt ./run.sh
 *
 * Options:
 *   lo=ADDR, hi=ADDR  the physical range [lo, hi) to watch (required);
 *   out=FILE          the report (default memwatch.txt).
 *
 * Every load and store that touches the range is counted against its
 * instruction's PC (a logical address) and kind (R or W). The report lists
 * each PC and kind once, in the order first seen, with the count, the
 * guest instruction count at the first access, and the first access's
 * logical and physical address and size. It is written when QEMU exits.
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint64_t pc;
    int write;
    uint64_t count;
    uint64_t first_insn;
    uint64_t vaddr;
    uint64_t paddr;
    unsigned size;
} Access;

static uint64_t lo, hi;
static const char *out = "memwatch.txt";
static GHashTable *seen;        /* (pc << 1 | write) -> Access */
static GPtrArray *order;
static GMutex lock;
static struct qemu_plugin_scoreboard *score;
static qemu_plugin_u64 insns;

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info,
                   uint64_t vaddr, void *udata)
{
    struct qemu_plugin_hwaddr *hw = qemu_plugin_get_hwaddr(info, vaddr);
    if (!hw || qemu_plugin_hwaddr_is_io(hw))
        return;
    uint64_t paddr = qemu_plugin_hwaddr_phys_addr(hw);
    unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    if (paddr + size <= lo || paddr >= hi)
        return;

    uint64_t pc = (uintptr_t)udata;
    int write = qemu_plugin_mem_is_store(info);
    uint64_t key = pc << 1 | write;
    g_mutex_lock(&lock);
    Access *a = g_hash_table_lookup(seen, &key);
    if (!a) {
        a = g_new0(Access, 1);
        a->pc = pc;
        a->write = write;
        a->first_insn = qemu_plugin_u64_sum(insns);
        a->vaddr = vaddr;
        a->paddr = paddr;
        a->size = size;
        uint64_t *k = g_new(uint64_t, 1);
        *k = key;
        g_hash_table_insert(seen, k, a);
        g_ptr_array_add(order, a);
    }
    a->count++;
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
    }
}

static void on_exit(void *userdata)
{
    FILE *f = fopen(out, "w");
    if (!f) {
        perror(out);
        return;
    }
    fprintf(f, "# physical range 0x%" PRIx64 "-0x%" PRIx64 "\n", lo, hi);
    fprintf(f, "# pc        kind  count       first-insn  vaddr     paddr     size\n");
    for (guint i = 0; i < order->len; i++) {
        Access *a = g_ptr_array_index(order, i);
        fprintf(f, "%08" PRIx64 "  %s  %10" PRIu64 "  %10" PRIu64
                "  %08" PRIx64 "  %08" PRIx64 "  %u\n",
                a->pc, a->write ? "W" : "R", a->count, a->first_insn,
                a->vaddr, a->paddr, a->size);
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
        } else if (!strncmp(argv[i], "out=", 4)) {
            out = argv[i] + 4;
        } else {
            fprintf(stderr, "memwatch: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    if (hi <= lo) {
        fprintf(stderr, "memwatch: needs lo=ADDR and hi=ADDR, lo < hi\n");
        return -1;
    }
    seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    order = g_ptr_array_new();
    score = qemu_plugin_scoreboard_new(sizeof(uint64_t));
    insns = qemu_plugin_scoreboard_u64(score);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
