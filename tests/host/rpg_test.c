/*
 * rpg_test: the RPG layer (rpgdb + inflate + VM + dialogue) on the cooked
 * test world's rpg.dpak. Plays "The Weaver's Lost Ring" through the real
 * dialogue code: greetings, topic learning, Choice, Info Refusal, journal,
 * result scripts with speaker locals, OnActivate scripts, message boxes,
 * start scripts and text defines; then fuzzes the interpreter.
 *
 *   rpg_test <test world data dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dialogue.h"
#include "inflate.h"
#include "mem.h"
#include "rpgdb.h"
#include "vm.h"

static int g_fail, g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL: ");                                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static RpgDb g_db;
static Vm g_vm;
static Dialogue g_dlg;

/* ---- mock world: two villagers and a ring in the loaded "zone" ---- */
typedef struct {
    const char *id;
    int32_t inst;
    int16_t vmi;
} MockRef;

static MockRef g_refs[] = { { "tw_villager", 1, -1 }, { "tw_villager_f", 2, -1 }, { "tw_ring", 3, -1 } };
static int g_activations;
static const char *g_cell = "Test Hamlet";
static int g_disabled_calls;

static int mock_find(void *user, uint32_t hash, VmRef *out)
{
    uint32_t i;
    (void)user;
    for (i = 0; i < PW_COUNTOF(g_refs); ++i) {
        if (pw_hash_name(g_refs[i].id) == hash) {
            out->kind = VM_REF_OBJECT;
            out->inst = g_refs[i].inst;
            out->vmi = g_refs[i].vmi;
            out->object = hash;
            out->key = hash;
            return 1;
        }
    }
    return 0;
}

static int mock_call(void *user, Vm *vm, uint16_t func, const VmRef *ref, const VmCtx *ctx,
                     const VmValue *args, int argc, VmValue *ret)
{
    (void)user;
    (void)ctx;
    (void)args;
    (void)argc;
    (void)ret;
    if (func == MWF_ACTIVATE && ref->kind == VM_REF_OBJECT) {
        g_activations++;
        /* default activation of an item: the player takes it */
        vm_item_add(vm, vm->player_hash, ref->object, 1);
        return 1;
    }
    return 0;
}

static const char *mock_cell(void *user)
{
    (void)user;
    return g_cell;
}

static int mock_interior(void *user)
{
    (void)user;
    return 0;
}

static void mock_disabled(void *user, const VmRef *ref, int disabled)
{
    (void)user;
    (void)ref;
    (void)disabled;
    g_disabled_calls++;
}

static VmRef ref_of(const char *id)
{
    return vm_resolve(&g_vm, pw_hash_name(id));
}

static int topic_index(const char *name)
{
    return rpgdb_topic(&g_db, pw_hash_name(name));
}

static const char *last_text(const Dialogue *d)
{
    uint32_t cursor = 0;
    int kind;
    const char *text, *last = "";
    while (dlg_history_next(d, &cursor, &kind, &text))
        if (kind == DLG_ENTRY_TEXT)
            last = text;
    return last;
}

static int listed(const Dialogue *d, const char *topic)
{
    uint32_t i;
    int t = topic_index(topic);
    for (i = 0; i < d->list_count; ++i)
        if ((int)d->list[i] == t)
            return 1;
    return 0;
}

static void test_inflate(void)
{
    /* "hello hello hello" as a fixed-Huffman block with a back reference,
     * produced by zlib (raw, level 9) */
    static const uint8_t packed[] = { 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x90, 0x00 };
    static const uint8_t stored[] = { 0x01, 0x03, 0x00, 0xfc, 0xff, 'a', 'b', 'c' };
    uint8_t out[64];
    uint32_t n = 0;
    int rc = pw_inflate(packed, sizeof(packed), out, sizeof(out), &n);
    CHECK(rc == PW_INFLATE_OK && n == 17 && memcmp(out, "hello hello hello", 17) == 0,
          "inflate fixed block (rc %d, n %u)", rc, (unsigned)n);
    rc = pw_inflate(stored, sizeof(stored), out, sizeof(out), &n);
    CHECK(rc == PW_INFLATE_OK && n == 3 && memcmp(out, "abc", 3) == 0, "inflate stored block");
    rc = pw_inflate(packed, sizeof(packed), out, 10, &n);
    CHECK(rc == PW_INFLATE_ERR_OUTPUT, "inflate reports a short output buffer");
    rc = pw_inflate(packed, 5, out, sizeof(out), &n);
    CHECK(rc == PW_INFLATE_ERR_INPUT || rc == PW_INFLATE_ERR_DATA, "inflate reports truncated input");
}

static void test_database(void)
{
    const DRpgObject *ring = rpgdb_object(&g_db, pw_hash_name("tw_ring"));
    const DRpgObject *weaver = rpgdb_object(&g_db, pw_hash_name("tw_villager_f"));
    const DRpgObject *book = rpgdb_object(&g_db, pw_hash_name("tw_book_history"));
    const DRpgObject *dagger = rpgdb_object(&g_db, pw_hash_name("tw_dagger"));
    const DRpgFaction *fac = rpgdb_faction(&g_db, pw_hash_name("tw_weavers"));
    char text[512];
    CHECK(ring && ring->type == DRPG_OT_MISC && strcmp(rpgdb_name(&g_db, ring->name), "Weaver's Ring") == 0,
          "ring item record");
    CHECK(ring && ring->value == 120 && ring->script != DRPG_NONE16, "ring value and script");
    CHECK(weaver && weaver->sex == 1 && weaver->link[2] == pw_hash_name("tw_weavers"), "weaver faction");
    CHECK(weaver && rpgdb_actor(&g_db, weaver)->disposition == 40, "weaver base disposition");
    CHECK(fac && strcmp(rpgdb_name(&g_db, fac->rank_name[2]), "Weaver") == 0, "faction rank names");
    CHECK(dagger && dagger->type == DRPG_OT_WEAP && dagger->link[0] == pw_hash_name("tw_spark_en"),
          "weapon enchantment link");
    CHECK(rpgdb_spell(&g_db, pw_hash_name("tw_spark")) != NULL, "spell record");
    CHECK(strcmp(rpgdb_gmst_string(&g_db, "sJournalEntry", "?"), "Your journal has been updated.") == 0,
          "GMST string");
    CHECK(rpgdb_gmst_int(&g_db, "iTestValue", 0) == 7, "GMST int");
    CHECK(rpgdb_gmst_float(&g_db, "fTestValue", 0.0f) == 2.5f, "GMST float");
    CHECK(book && rpgdb_text(&g_db, book->link[1], text, sizeof(text)) > 0 &&
          strstr(text, "founded by weavers.\nThey came for the reeds & the quiet.") != NULL,
          "book text inflated and cleaned: '%s'", text);
    CHECK(topic_index("Greeting 0") >= 0 && topic_index("lost ring") >= 0, "topics by hash");
    CHECK(g_db.hdr->starts.count == 1, "start script listed");
}

static void test_start_script(void)
{
    const VmMessage *m;
    int g = vm_global_index(&g_vm, "tw_welcomed");
    vm_tick(&g_vm, 1.0f / 30.0f);
    m = vm_message_front(&g_vm);
    CHECK(m && strcmp(m->text, "Welcome to the Test Hamlet, Outlander.") == 0 && m->nbuttons == 1 &&
          strcmp(m->buttons[0], "Thanks") == 0, "start script message box ('%s')", m ? m->text : "");
    vm_tick(&g_vm, 1.0f / 30.0f);
    CHECK(vm_global_float(&g_vm, g) == 0.0f, "no button pressed yet");
    vm_message_answer(&g_vm, 0);
    vm_tick(&g_vm, 1.0f / 30.0f);
    CHECK(vm_global_float(&g_vm, g) == 1.0f, "GetButtonPressed sees the answer");
    CHECK(vm_message_front(&g_vm) == NULL, "message box queue drained");
}

static void test_quest(void)
{
    Dialogue *d = &g_dlg;
    VmRef elder = ref_of("tw_villager"), weaver = ref_of("tw_villager_f");
    int lost = topic_index("lost ring"), rumors = topic_index("latest rumors");
    int secret = topic_index("little secret");
    DlgLink links[8];
    int n;

    /* the elder greets and seeds a topic */
    CHECK(dlg_start(d, &elder, "Hamlet Elder") == 1, "elder greets");
    CHECK(strcmp(last_text(d), "Welcome to the hamlet, Outlander. Ask me about the latest rumors.") == 0,
          "%%PCName define ('%s')", last_text(d));
    CHECK(listed(d, "latest rumors"), "rumors learned from the greeting");
    n = dlg_links(d, last_text(d), links, 8);
    CHECK(n == 1 && links[0].topic == (uint16_t)rumors && links[0].start == 51 && links[0].len == 13,
          "hyperlink span (n %d)", n);
    dlg_ask(d, (uint32_t)rumors);
    CHECK(strstr(last_text(d), "Ask her about her lost ring.") != NULL, "rumor response");
    CHECK(!vm_topic_known(&g_vm, (uint32_t)lost), "elder cannot answer 'lost ring': not learned");
    dlg_end(d);

    /* the weaver: first greeting runs a result script on her locals */
    CHECK(dlg_start(d, &weaver, "Hamlet Weaver") == 1, "weaver greets");
    CHECK(strstr(last_text(d), "Have you heard about my lost ring?") != NULL, "first greeting");
    CHECK(vm_topic_known(&g_vm, (uint32_t)lost) && listed(d, "lost ring"), "AddTopic from result script");
    CHECK(listed(d, "latest rumors"), "generic topic answered by the weaver");
    weaver = ref_of("tw_villager_f");
    CHECK(weaver.vmi >= 0, "weaver has a script instance");

    /* Info Refusal: little secret needs disposition 55 */
    vm_add_topic(&g_vm, pw_hash_name("little secret"));
    dlg_ask(d, (uint32_t)secret);
    CHECK(strcmp(last_text(d), "I don't know you well enough to talk about that.") == 0,
          "info refusal at disposition 40 ('%s')", last_text(d));

    /* Choice */
    dlg_ask(d, (uint32_t)lost);
    CHECK(strstr(last_text(d), "Will you find it for me?") != NULL, "quest offer");
    CHECK(d->in_choice && d->choice_count == 2 && strcmp(d->choices[0].text, "I will find it.") == 0,
          "choices offered (%u)", (unsigned)d->choice_count);
    dlg_ask(d, (uint32_t)rumors);
    CHECK(strstr(last_text(d), "Will you find it for me?") != NULL, "topics are locked while choosing");
    dlg_answer(d, 0);
    CHECK(strcmp(last_text(d), "Thank you, Folk. Search the cellar.") == 0, "choice 1 answer ('%s')",
          last_text(d));
    CHECK(!d->in_choice, "choice finished");
    CHECK(vm_journal_index(&g_vm, pw_hash_name("tw_lostring")) == 10 && g_vm.journal_count == 1,
          "journal stage 10");
    CHECK(g_vm.notify_count > 0 && strcmp(g_vm.notify[g_vm.notify_count - 1],
                                         "Your journal has been updated.") == 0, "journal notification");
    dlg_ask(d, (uint32_t)lost);
    CHECK(strcmp(last_text(d), "Have you found my ring yet?") == 0, "reminder before the ring is found");
    dlg_end(d);

    /* the ring's OnActivate script */
    {
        int vmi = vm_bind_reference(&g_vm, pw_hash_name("tw_ring"), pw_hash_name("tw_ring"), 3);
        VmRef ring;
        const VmMessage *m;
        g_refs[2].vmi = (int16_t)vmi;
        ring = ref_of("tw_ring");
        CHECK(vmi >= 0 && vm_activate(&g_vm, &ring) == 1, "ring script handles activation");
        vm_tick(&g_vm, 1.0f / 30.0f);
        m = vm_message_front(&g_vm);
        CHECK(m && strcmp(m->text, "The ring glints in the dust.") == 0, "ring message box");
        if (m)
            vm_message_answer(&g_vm, 0);
        CHECK(g_activations == 1, "script Activate performed the default action");
        CHECK(vm_item_count(&g_vm, g_vm.player_hash, pw_hash_name("tw_ring")) == 1, "ring in inventory");
        CHECK(vm_journal_index(&g_vm, pw_hash_name("tw_lostring")) == 20, "journal stage 20");
        vm_tick(&g_vm, 1.0f / 30.0f);
        CHECK(g_activations == 1, "OnActivate is a one-frame event");
    }

    /* reward */
    CHECK(dlg_start(d, &weaver, "Hamlet Weaver") == 1, "weaver greets again");
    CHECK(strcmp(last_text(d), "Hello again, Outlander. The Weaver of the Weavers' Circle greets you.") == 0,
          "%%Class / %%Faction defines ('%s')", last_text(d));
    int32_t gold0 = vm_item_count(&g_vm, g_vm.player_hash, pw_hash_name("gold_001"));
    dlg_ask(d, (uint32_t)lost);
    CHECK(strcmp(last_text(d), "You found it! Here, take this gold.") == 0, "reward response ('%s')",
          last_text(d));
    CHECK(vm_item_count(&g_vm, g_vm.player_hash, pw_hash_name("tw_ring")) == 0, "ring handed over");
    CHECK(vm_item_count(&g_vm, g_vm.player_hash, pw_hash_name("gold_001")) == gold0 + 50, "50 gold received");
    CHECK(vm_journal_index(&g_vm, pw_hash_name("tw_lostring")) == 100 && g_vm.journal_count == 3,
          "journal finished (%u entries)", (unsigned)g_vm.journal_count);
    {
        int t = topic_index("tw_lostring");
        CHECK(t >= 0 && (g_vm.quest_flags[t] & VMQ_FINISHED), "quest marked finished");
    }
    CHECK(vm_disposition(&g_vm, &weaver) == 60, "ModDisposition 20 (now %d)", vm_disposition(&g_vm, &weaver));
    {
        const DRpgVar *v = rpgdb_script_var(&g_db, g_vm.inst[weaver.vmi].script, pw_hash_name("metpc"));
        CHECK(v && g_vm.locals[g_vm.inst[weaver.vmi].locals + v->slot].i == 2,
              "result script set the speaker's local (dynamic access)");
    }
    dlg_ask(d, (uint32_t)secret);
    CHECK(strstr(last_text(d), "finest thread") != NULL, "secret shared at disposition 60");
    dlg_ask(d, (uint32_t)lost);
    CHECK(strcmp(last_text(d), "Thank you again for finding my ring.") == 0, "after the quest");
    CHECK(d->list_count >= 3 && d->list_exhausted[0] <= 1, "topic list (%u)", (unsigned)d->list_count);
    dlg_end(d);

    /* journal text */
    {
        char text[256];
        const DRpgInfo *in = &g_db.infos[g_vm.journal[2].info];
        rpgdb_text(&g_db, in->text, text, sizeof(text));
        CHECK(strstr(text, "rewarded me with 50 gold") != NULL, "journal entry text");
        vm_date_string(&g_vm, g_vm.journal[0].day, g_vm.journal[0].month, text, sizeof(text));
        CHECK(strcmp(text, "16 Last Seed") == 0, "journal date ('%s')", text);
    }
}

static void test_goodbye_branch(void)
{
    Dialogue *d = &g_dlg;
    VmRef weaver;
    vm_new_game(&g_vm);
    g_refs[1].vmi = (int16_t)vm_bind_reference(&g_vm, pw_hash_name("tw_villager_f"),
                                               pw_hash_name("tw_villager_f"), 2);
    weaver = ref_of("tw_villager_f");
    CHECK(dlg_start(d, &weaver, "Hamlet Weaver") == 1, "new game: weaver greets");
    dlg_ask(d, (uint32_t)topic_index("lost ring"));
    dlg_answer(d, 1);
    CHECK(strcmp(last_text(d), "Pity. Leave me be, then.") == 0, "choice 2 answer");
    CHECK(d->goodbye, "Goodbye from the result script");
    CHECK(vm_journal_index(&g_vm, pw_hash_name("tw_lostring")) == 0, "quest not started");
    dlg_end(d);
}

static void test_time_and_props(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    int h = vm_global_index(&g_vm, "gamehour");
    float before = vm_global_float(&g_vm, h);
    vm_tick(&g_vm, 120.0f); /* 2 real minutes x timescale 30 = 1 game hour */
    CHECK(vm_global_float(&g_vm, h) > before + 0.99f && vm_global_float(&g_vm, h) < before + 1.01f,
          "GameHour advances with the timescale (%.2f -> %.2f)", before, vm_global_float(&g_vm, h));
    CHECK(vm_prop(&g_vm, &pc, MWP_ATTR_STRENGTH) == 40.0f, "player attributes from the player record");
    CHECK(vm_prop(&g_vm, &pc, MWP_DYN_HEALTH) == 50.0f, "player health from the player record");
}

static void test_fuzz(void)
{
    uint8_t blob[512];
    uint32_t seed = 12345, i, k;
    VmRef none;
    CHECK(g_vm.errors == 0, "no script errors before fuzzing (%u: %s)", (unsigned)g_vm.errors,
          g_vm.last_error);
    memset(&none, 0, sizeof(none));
    for (i = 0; i < 4000; ++i) {
        DRpgSnippet h;
        uint32_t code = 1 + (seed % 200);
        for (k = 0; k < sizeof(blob); ++k) {
            seed = seed * 1103515245u + 12345u;
            blob[k] = (uint8_t)(seed >> 16);
        }
        /* bias towards valid opcodes */
        for (k = sizeof(h); k < sizeof(h) + code; k += 3)
            blob[k] = (uint8_t)(blob[k] % MWBC_OP_COUNT);
        h.flags = 0;
        h.pad = 0;
        h.code_size = code;
        h.strings_size = 64;
        memcpy(blob, &h, sizeof(h));
        blob[sizeof(blob) - 1] = 0;
        vm_run_snippet(&g_vm, blob, sizeof(blob), i & 1 ? &none : &g_dlg.actor);
        while (vm_message_front(&g_vm))
            vm_message_answer(&g_vm, 0);
    }
    CHECK(1, "interpreter survived %u random programs", 4000u);
}

int main(int argc, char **argv)
{
    char err[200], path[512];
    VmHost host;
    if (argc < 2) {
        fprintf(stderr, "usage: rpg_test <data dir>\n");
        return 2;
    }
    mem_init();
    test_inflate();
    snprintf(path, sizeof(path), "%s/rpg.dpak", argv[1]);
    if (rpgdb_open(&g_db, path, err, sizeof(err)) != 0) {
        printf("rpgdb_open failed: %s\n", err);
        return 1;
    }
    test_database();
    memset(&host, 0, sizeof(host));
    host.call = mock_call;
    host.find_ref = mock_find;
    host.cell_name = mock_cell;
    host.interior = mock_interior;
    host.set_disabled = mock_disabled;
    CHECK(vm_init(&g_vm, &g_db, &host) == 0, "vm_init");
    CHECK(dlg_init(&g_dlg, &g_vm, &g_db) == 0, "dlg_init");
    g_refs[1].vmi = (int16_t)vm_bind_reference(&g_vm, pw_hash_name("tw_villager_f"),
                                               pw_hash_name("tw_villager_f"), 2);
    test_start_script();
    test_quest();
    CHECK(g_vm.errors == 0, "quest ran without script errors (%u: %s)", (unsigned)g_vm.errors,
          g_vm.last_error);
    test_goodbye_branch();
    test_time_and_props();
    test_fuzz();
    printf("rpg_test: %d/%d checks passed (vm errors %u, unhandled %u, text loads %u)\n",
           g_checks - g_fail, g_checks, (unsigned)g_vm.errors, (unsigned)g_vm.unhandled,
           (unsigned)g_db.text_loads);
    return g_fail ? 1 : 0;
}
