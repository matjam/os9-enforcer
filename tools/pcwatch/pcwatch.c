/*
 * pcwatch: a QEMU TCG plugin that counts the register values seen each
 * time the guest runs the instruction at one address.
 *
 * Usage (run.sh passes PLUGIN to QEMU's -plugin option):
 *   PLUGIN=tools/pcwatch/build/libpcwatch.dylib,at=0xF12A14,regs=r9:r10,crmask=0x82208000,out=run/pw.txt ./run.sh
 *
 * Options:
 *   at=ADDR       the instruction's logical address (for NanoKernel code,
 *                 which runs with translation off, its physical address);
 *   regs=R[:R...] registers to record, by their gdb names (r0-r31, lr, ctr,
 *                 msr, ...), up to 8;
 *   crmask=MASK   also record CR & MASK (default 0: CR not recorded);
 *   out=FILE      the report (default pcwatch.txt).
 *
 * Each distinct combination of the recorded values is counted. The report
 * lists them by count, most first, and is written when QEMU exits.
 */

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_REGS 8

typedef struct {
    uint32_t v[MAX_REGS + 1];   /* the registers, then CR & crmask */
    uint64_t count;
} Combo;

static uint64_t at;
static uint32_t crmask;
static const char *out = "pcwatch.txt";
static char *names[MAX_REGS];
static int nregs;
static struct qemu_plugin_register *regs[MAX_REGS], *reg_cr;
static GByteArray *regbuf;
static GHashTable *combos;      /* Combo (as key) -> same Combo */
static GMutex lock;

static guint combo_hash(gconstpointer p)
{
    const Combo *c = p;
    guint h = 5381;
    for (int i = 0; i <= nregs; i++)
        h = h * 33 + c->v[i];
    return h;
}

static gboolean combo_equal(gconstpointer a, gconstpointer b)
{
    return !memcmp(((const Combo *)a)->v, ((const Combo *)b)->v,
                   sizeof(uint32_t) * (nregs + 1));
}

static uint32_t read_reg(struct qemu_plugin_register *r)
{
    g_byte_array_set_size(regbuf, 0);
    if (!r || !qemu_plugin_read_register(r, regbuf) || regbuf->len < 4)
        return 0;
    const uint8_t *b = regbuf->data + regbuf->len - 4;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static void on_insn(unsigned int vcpu, void *udata)
{
    Combo key = {0};
    g_mutex_lock(&lock);
    for (int i = 0; i < nregs; i++)
        key.v[i] = read_reg(regs[i]);
    key.v[nregs] = crmask ? read_reg(reg_cr) & crmask : 0;
    Combo *c = g_hash_table_lookup(combos, &key);
    if (!c) {
        c = g_new0(Combo, 1);
        *c = key;
        g_hash_table_insert(combos, c, c);
    }
    c->count++;
    g_mutex_unlock(&lock);
}

static void tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        if (qemu_plugin_insn_vaddr(insn) == at)
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, on_insn, QEMU_PLUGIN_CB_R_REGS, NULL);
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
        if (!strcmp(d->name, "cr"))
            reg_cr = d->handle;
    }
    for (int j = 0; j < nregs; j++)
        if (!regs[j])
            fprintf(stderr, "pcwatch: no register %s\n", names[j]);
    if (all)
        g_array_free(all, TRUE);
}

static gint by_count(gconstpointer a, gconstpointer b)
{
    const Combo *x = *(Combo *const *)a, *y = *(Combo *const *)b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void on_exit(void *userdata)
{
    FILE *f = fopen(out, "w");
    if (!f) {
        perror(out);
        return;
    }
    GPtrArray *list = g_hash_table_get_values_as_ptr_array(combos);
    g_ptr_array_sort(list, by_count);
    fprintf(f, "# at 0x%" PRIx64 "\n#      count", at);
    for (int i = 0; i < nregs; i++)
        fprintf(f, "  %-8s", names[i]);
    if (crmask)
        fprintf(f, "  cr&%08x", crmask);
    fprintf(f, "\n");
    for (guint i = 0; i < list->len; i++) {
        Combo *c = g_ptr_array_index(list, i);
        fprintf(f, "%12" PRIu64, c->count);
        for (int j = 0; j < nregs; j++)
            fprintf(f, "  %08x", c->v[j]);
        if (crmask)
            fprintf(f, "  %08x", c->v[nregs]);
        fprintf(f, "\n");
    }
    g_ptr_array_free(list, TRUE);
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "at=", 3)) {
            at = g_ascii_strtoull(argv[i] + 3, NULL, 0);
        } else if (!strncmp(argv[i], "regs=", 5)) {
            gchar **parts = g_strsplit(argv[i] + 5, ":", -1);
            for (int j = 0; parts[j]; j++) {
                if (nregs == MAX_REGS) {
                    fprintf(stderr, "pcwatch: at most %d registers\n", MAX_REGS);
                    return -1;
                }
                names[nregs++] = g_strdup(parts[j]);
            }
            g_strfreev(parts);
        } else if (!strncmp(argv[i], "crmask=", 7)) {
            crmask = g_ascii_strtoull(argv[i] + 7, NULL, 0);
        } else if (!strncmp(argv[i], "out=", 4)) {
            out = argv[i] + 4;
        } else {
            fprintf(stderr, "pcwatch: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    if (!at) {
        fprintf(stderr, "pcwatch: needs at=ADDR\n");
        return -1;
    }
    regbuf = g_byte_array_new();
    combos = g_hash_table_new(combo_hash, combo_equal);
    qemu_plugin_register_vcpu_init_cb(id, on_vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, on_exit, NULL);
    return 0;
}
