/*
 * bbport M2 offline test: pre-gfx hoist (BB_KK_DEFER_HOIST) vs the M1
 * schedule, no GPU, no Metal.
 *
 * Same method as defer-m1-test: compiled with the driver's own flags, linked
 * against the driver's REAL kk_render_defer.c.o (the object in the probe
 * dylib), mtl_* bridge stubbed by recording functions. On top of the M1
 * harness this one emulates, line for line, the three driver sites that M2
 * changed outside kk_render_defer.c (kk_cmd_buffer.c / kk_cmd_draw.c, which
 * cannot be linked without the whole driver):
 *   gate()     kk_draw -> kk_bb_hoist_gate -> kk_renc_hoist_draw
 *   side()     kk_dispatch_precomp (hoisted draw: side node BEFORE
 *              cs_get_compute, else the M1 order)
 *   getc_()    cs_get_compute (hoist_next -> the hoist encoder on the pass's
 *              command buffer; else kk_renc_slow_sync_force + M1 encoders)
 *   hclose()   kk_cmd_hoist_close (renc.hoist_close)
 *   pass_end() cs_end: pre_gfx encoder, then the gfx state, then post_gfx
 * Every dispatch line carries seq=N, its record-order id, identical in every
 * mode, so run.sh can match a hoisted dispatch with its M1 position.
 *
 * usage: renc_hoist_test direct|deferred|hoist <out-prefix> [perturb-bind|perturb-order]
 *        renc_hoist_test predicate      (refusal-predicate unit table)
 */

#include "kk_cmd_buffer.h"
#include "kk_render_defer.h"
#include "kk_render_enc.h"
#include "kk_device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- recording Metal stubs ------------------------------------------------ */

static FILE *mt;
static uintptr_t fake_next = 0x1000;
/* object -> stable name: plain objects by creation index (the test creates
 * them in the same order in both runs); encoders by kind + ordinal of their
 * kind ("R1" = first render encoder), since the deferred backend creates the
 * render encoder later in CPU order than the direct one. */
static const void *objs[4096];
static char names[4096][16];
static unsigned nobj, nrender, ncompute, nplain;
static void *named(const char *kind, unsigned k)
{
   fake_next += 0x100;
   objs[nobj] = (void *)fake_next;
   snprintf(names[nobj], sizeof(names[0]), "%s%u", kind, k);
   nobj++;
   return (void *)fake_next;
}
static void *fake(void) { return named("o", ++nplain); }
static const char *oname(const void *p)
{
   if (!p) return "nil";
   for (unsigned i = 0; i < nobj; i++) if (objs[i] == p) return names[i];
   return "UNKNOWN";
}
/* encoders are named by creation index, not address */
static uint64_t table_slots[3];
static void *the_table;

#define M(obj, ...) do { fprintf(mt, "%s ", oname(obj)); fprintf(mt, __VA_ARGS__); fputc('\n', mt); } while (0)
#define DRAWB(obj) M(obj, "  bindings 0x%llx 0x%llx 0x%llx", (unsigned long long)table_slots[0], (unsigned long long)table_slots[1], (unsigned long long)table_slots[2])

void *mtl_retain(void *h) { return h; }
void mtl_release(void *h) { (void)h; }
static unsigned ncopy;
mtl_render_pass_descriptor *mtl_render_pass_descriptor_copy(mtl_render_pass_descriptor *d) { (void)d; return named("Dcopy", ++ncopy); }
void mtl_set_address(mtl_argument_table *t, uint64_t a, uint32_t s) { if (t == the_table && s < 3) table_slots[s] = a; }
mtl_render_encoder *mtl_new_render_command_encoder_with_descriptor(mtl_command_buffer *cb, mtl_render_pass_descriptor *d)
{ void *e = named("R", ++nrender); M(e, "new_render_encoder cb=%s", oname(cb)); (void)d; return e; }
void mtl_end_encoding(void *e) { M(e, "end_encoding"); }
void mtl_barrier_after_stages(void *e, enum mtl_stages a, enum mtl_stages b) { M(e, "barrier_after_stages %x %x", a, b); }
void mtl_barrier_after_encoder_stages(void *e, enum mtl_stages a, enum mtl_stages b) { M(e, "barrier_after_encoder_stages %x %x", a, b); }
void mtl_barrier_after_queue_stages(void *e, enum mtl_stages a, enum mtl_stages b) { M(e, "barrier_after_queue_stages %x %x", a, b); }
void mtl_encoder_set_label(void *e, const char *l) { M(e, "set_label %s", l); }
void mtl_encoder_insert_debug_signpost(void *e, const char *l) { M(e, "signpost %s", l); }
void mtl_encoder_push_debug_group(void *e, const char *l) { M(e, "push_group %s", l); }
void mtl_encoder_pop_debug_group(void *e) { M(e, "pop_group"); }
void mtl_set_viewports(mtl_render_encoder *e, struct mtl_viewport *v, uint32_t n)
{ M(e, "viewports %u %g %g %g %g %g %g", n, v[0].originX, v[0].originY, v[0].width, v[0].height, v[0].znear, v[0].zfar); }
void mtl_set_scissor_rects(mtl_render_encoder *e, struct mtl_scissor_rect *r, uint32_t n)
{ M(e, "scissors %u %zu %zu %zu %zu", n, r[0].x, r[0].y, r[0].width, r[0].height); }
void mtl_render_set_pipeline_state(mtl_render_encoder *e, mtl_render_pipeline_state *p) { M(e, "pipeline %s", oname(p)); }
void mtl_set_depth_stencil_state(mtl_render_encoder *e, mtl_depth_stencil_state *s) { M(e, "ds_state %s", oname(s)); }
void mtl_set_stencil_references(mtl_render_encoder *e, uint32_t f, uint32_t b) { M(e, "stencil_ref %u %u", f, b); }
void mtl_set_front_face_winding(mtl_render_encoder *e, enum mtl_winding w) { M(e, "winding %u", w); }
void mtl_set_cull_mode(mtl_render_encoder *e, enum mtl_cull_mode m) { M(e, "cull %u", m); }
void mtl_set_visibility_result_mode(mtl_render_encoder *e, enum mtl_visibility_result_mode m, size_t o) { M(e, "visibility %u %zu", m, o); }
void mtl_set_depth_bias(mtl_render_encoder *e, float a, float b, float c) { M(e, "depth_bias %a %a %a", a, b, c); }
void mtl_set_depth_clip_mode(mtl_render_encoder *e, enum mtl_depth_clip_mode m) { M(e, "depth_clip %u", m); }
void mtl_set_depth_test_bounds(mtl_render_encoder *e, float a, float b) { M(e, "depth_bounds %a %a", a, b); }
void mtl_set_vertex_amplification_count(mtl_render_encoder *e, uint32_t *ids, uint32_t n)
{ M(e, "amplification %u first=%u last=%u", n, n ? ids[0] : 0, n ? ids[n - 1] : 0); }
void mtl_render_set_argument_table(mtl_render_encoder *e, mtl_argument_table *t, enum mtl_render_stages s) { M(e, "argument_table %s %x", oname(t), s); }
void mtl_draw_primitives(mtl_render_encoder *e, enum mtl_primitive_type t, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ M(e, "draw %u %u %u %u %u", t, a, b, c, d); DRAWB(e); }
void mtl_draw_indexed_primitives(mtl_render_encoder *e, enum mtl_primitive_type t, uint32_t n, enum mtl_index_type it, uint64_t ia, uint64_t il, uint32_t ic, int32_t bv, uint32_t bi)
{ M(e, "draw_indexed %u %u %u %llx %llx %u %d %u", t, n, it, (unsigned long long)ia, (unsigned long long)il, ic, bv, bi); DRAWB(e); }
void mtl_draw_primitives_indirect(mtl_render_encoder *e, enum mtl_primitive_type t, uint64_t a)
{ M(e, "draw_indirect %u %llx", t, (unsigned long long)a); DRAWB(e); }
void mtl_draw_indexed_primitives_indirect(mtl_render_encoder *e, enum mtl_primitive_type t, enum mtl_index_type it, uint64_t ia, uint64_t il, uint64_t a)
{ M(e, "draw_indexed_indirect %u %u %llx %llx %llx", t, it, (unsigned long long)ia, (unsigned long long)il, (unsigned long long)a); DRAWB(e); }
void mtl_render_write_timestamp(mtl_render_encoder *e, enum mtl_render_stages s, mtl_counter_heap *h, uint32_t i) { M(e, "timestamp %x %s %u", s, oname(h), i); }
void mtl_render_set_color_store_action(mtl_render_encoder *e, enum mtl_store_action a, uint32_t i) { M(e, "color_store %u %u", a, i); }
void mtl_render_set_depth_store_action(mtl_render_encoder *e, enum mtl_store_action a) { M(e, "depth_store %u", a); }
void mtl_render_set_stencil_store_action(mtl_render_encoder *e, enum mtl_store_action a) { M(e, "stencil_store %u", a); }

VkResult __vk_errorf(const void *obj, VkResult r, const char *f, int l, const char *fmt, ...)
{ (void)obj; (void)fmt; fprintf(stderr, "vk_error %d %s:%d\n", r, f, l); exit(3); }


/* ---- driver emulation ---------------------------------------------------- */

static int64_t stats[KK_BBSTAT_COUNT];
static unsigned nhoist, seq;
static bool perturb_order;
static FILE *hl; /* <out>.hoist: one line per hoisted dispatch */

static void
hclose(struct kk_cmd_buffer *cmd)
{
   for (unsigned i = 0; i < 2; i++) {
      struct kk_encoder_state *es = &cmd->cmp[i];
      if (es->encoder && es->bb_hoisted) {
         M(es->encoder, "barrier_after_stages %x %x", MTL_STAGE_ALL, MTL_STAGE_ALL);
         M(es->encoder, "end_encoding");
         es->encoder = NULL;
         es->bb_hoisted = false;
         cmd->renc.hoist_open = false;
      }
   }
}

static void *
start_compute(struct kk_cmd_buffer *cmd, bool pre, bool hoist)
{
   struct kk_encoder_state *es = pre ? cmd->pre_gfx : cmd->post_gfx;
   if (hoist) {
      es->encoder = named("H", ++nhoist);
      es->bb_hoisted = true;
      cmd->renc.hoist_open = true;
      stats[KK_BBSTAT_HOIST_ENCODERS]++;
      M(es->encoder, "new_compute_encoder hoist cb=%s", oname(cmd->gfx.cmd_buf));
   } else {
      es->encoder = named("C", ++ncompute);
      M(es->encoder, "new_compute_encoder %s", pre ? "pre" : "post");
   }
   return es->encoder;
}

static void *
getc_(struct kk_cmd_buffer *cmd, bool pre)
{
   if (cmd->renc.hoist_next) {
      struct kk_encoder_state *es = cmd->pre_gfx;
      if (pre && kk_renc_is_open(cmd) && !cmd->renc.enc_created &&
          (es->encoder == NULL || es->bb_hoisted)) {
         cmd->renc.hoist_next = false;
         if (es->encoder == NULL)
            start_compute(cmd, true, true);
         return es->encoder;
      }
      kk_renc_hoist_late_refusal(cmd, KK_RENC_HOIST_BUSY);
   }
   if (cmd->renc.slow)
      kk_renc_slow_sync_force(cmd);
   struct kk_encoder_state *es =
      (!kk_renc_is_open(cmd) || pre) ? cmd->pre_gfx : cmd->post_gfx;
   if (!es->encoder)
      start_compute(cmd, es == cmd->pre_gfx, false);
   return es->encoder;
}

static void
dispatch(struct kk_cmd_buffer *cmd, void *enc, uint32_t prog, unsigned s)
{
   if (perturb_order && cmd->renc.hoist_open && enc == cmd->pre_gfx->encoder &&
       cmd->pre_gfx->bb_hoisted)
      kk_renc_slow_sync_force(cmd); /* broken hoist: render encoder first */
   uint64_t saved = table_slots[0];
   kk_renc_set_table_address(cmd, 0u, 0xC0DE0000ull + prog);
   M(enc, "dispatch prog=%u seq=%u", prog, s); DRAWB(enc);
   kk_renc_set_table_address(cmd, 0u, saved);
}

/* kk_dispatch_precomp */
static void
side(struct kk_cmd_buffer *cmd, bool pre, uint32_t prog, const void *args, size_t sz)
{
   const unsigned s = ++seq;
   const bool hoist_first = cmd->renc.hoist_draw && pre;
   if (hoist_first)
      kk_renc_precomp_side_op(cmd, pre, prog, args, sz);
   void *enc = getc_(cmd, pre);
   M(enc, "barrier_after_encoder_stages %x %x", MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
   if (!hoist_first)
      kk_renc_precomp_side_op(cmd, pre, prog, args, sz);
   if (cmd->pre_gfx->bb_hoisted && enc == cmd->pre_gfx->encoder)
      fprintf(hl, "%u\n", s);
   dispatch(cmd, enc, prog, s);
}

/* kk_launch_tess's VS+TCS pre dispatches (kk_cmd_draw.c:1985 order) */
static void
tess_side(struct kk_cmd_buffer *cmd, uint64_t state)
{
   const unsigned s = ++seq;
   void *enc = getc_(cmd, true);
   kk_renc_side_op(cmd, true, KK_RENC_SIDE_RAW, KK_RENC_SIDE_ANY, NULL, 0u, &state, 1u);
   M(enc, "barrier_after_encoder_stages %x %x", MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
   dispatch(cmd, enc, 9000u, s);
}

/* kk_draw -> kk_bb_hoist_gate (the draw has pre-gfx work) */
static void
gate(struct kk_cmd_buffer *cmd, bool tess, const uint64_t *reads, uint32_t nr,
     bool heap_first)
{
   kk_renc_hoist_draw(cmd, tess ? KK_RENC_SIDE_ANY : 0u, reads, nr, heap_first);
}

static void
draw_done(struct kk_cmd_buffer *cmd)
{
   cmd->renc.hoist_draw = false;
   cmd->renc.hoist_next = false;
}

static void
end_compute(struct kk_cmd_buffer *cmd, struct kk_encoder_state *es)
{
   if (!es->encoder)
      return;
   if (es->bb_hoisted) {
      hclose(cmd);
      return;
   }
   kk_renc_sync(cmd);
   M(es->encoder, "barrier_after_stages %x %x", MTL_STAGE_ALL, MTL_STAGE_ALL);
   M(es->encoder, "end_encoding");
   es->encoder = NULL;
}

/* cs_end x2: pre_gfx, gfx, then the promoted post_gfx */
static void
pass_end(struct kk_cmd_buffer *cmd, bool barrier, enum mtl_stages before)
{
   end_compute(cmd, cmd->pre_gfx);
   kk_renc_end(cmd, barrier, before);
   end_compute(cmd, cmd->post_gfx);
}

static void
pass_begin(struct kk_cmd_buffer *cmd, void *desc, bool entry_barrier)
{
   kk_renc_begin(cmd, desc);
   uint32_t ids[2] = {0, 1};
   kk_renc_set_vertex_amplification_count(kk_renc(cmd), ids, 2);
   if (entry_barrier)
      kk_renc_barrier_after_queue_stages(kk_renc(cmd), MTL_STAGE_ALL, MTL_STAGE_VERTEX);
   kk_renc_set_argument_table(kk_renc(cmd), cmd->argument_table,
                              MTL_RENDER_STAGE_VERTEX | MTL_RENDER_STAGE_FRAGMENT);
}

static void
state(struct kk_cmd_buffer *cmd, void *pipe, void *ds, int salt)
{
   struct kk_renc *r = kk_renc(cmd);
   struct mtl_viewport vp = {0, 0, 1280 + salt, 720, 0, 1};
   struct mtl_scissor_rect sc = {0, 0, 1280, 720 - salt};
   kk_renc_set_scissor_rects(r, &sc, 1);
   kk_renc_set_viewports(r, &vp, 1);
   kk_renc_set_depth_stencil_state(r, ds);
   kk_renc_set_pipeline_state(r, pipe);
   kk_renc_set_cull_mode(r, MTL_CULL_MODE_BACK);
   kk_renc_set_depth_bias(r, 1.5f, -0.25f, 0.0f);
   kk_renc_set_table_address(cmd, 0u, 0xA0000000ull + salt * 0x100);
   kk_renc_set_table_address(cmd, 2u, 0xB0000000ull + salt * 0x40);
}

static void
store_ops(struct kk_cmd_buffer *cmd)
{
   struct kk_renc *r = kk_renc(cmd);
   kk_renc_set_color_store_action(r, MTL_STORE_ACTION_STORE, 0);
   kk_renc_set_depth_store_action(r, MTL_STORE_ACTION_STORE);
}

static struct kk_cmd_buffer *
new_cmd(bool deferred, bool hoist)
{
   struct kk_cmd_buffer *cmd = calloc(1, sizeof(*cmd));
   cmd->pre_gfx = &cmd->cmp[0];
   cmd->post_gfx = &cmd->cmp[1];
   kk_renc_init(cmd, deferred);
   /* kk_create_cmd_buffer's latch (deferred && BB_KK_DEFER_HOIST &&
    * BB_KK_ONE_CMDBUF) */
   cmd->renc.hoist = deferred && hoist;
   if (cmd->renc.hoist) {
      cmd->renc.hoist_close = hclose;
      cmd->renc.hoist_stats = stats;
   }
   return cmd;
}

/* ---- refusal-predicate unit table ---------------------------------------- */

static int
predicate_table(void)
{
   kk_renc_log_set_file_for_test(NULL);
   int bad = 0;
   uint64_t rd;
#define EXPECT(cmd, what, flags, r0, want)                                      \
   do {                                                                        \
      rd = (r0);                                                               \
      enum kk_renc_hoist_why got = kk_renc_hoist_check(cmd, flags, &rd, 1u);   \
      printf("  %-52s -> %-11s %s\n", what, kk_renc_hoist_why_name(got),       \
             got == (want) ? "ok" : "MISMATCH");                               \
      bad += got != (want);                                                    \
   } while (0)
   struct kk_cmd_buffer *c = new_cmd(true, true);
   void *d = fake();
   c->argument_table = fake();
   c->gfx.cmd_buf = fake();
   EXPECT(c, "no pass recording (pass begins lazily)", 0u, 0x10, KK_RENC_HOIST_OK);
   EXPECT(c, "no pass recording, op flagged ANY", KK_RENC_SIDE_ANY, 0x10, KK_RENC_HOIST_ANY);
   kk_renc_begin(c, d);
   EXPECT(c, "fresh pass", 0u, 0x10, KK_RENC_HOIST_OK);
   EXPECT(c, "fresh pass, op flagged ANY", KK_RENC_SIDE_ANY, 0x10, KK_RENC_HOIST_ANY);
   uint64_t w = 0x1234;
   kk_renc_side_op(c, false, 7u, 0u, NULL, 0u, &w, 1u); /* non-hoisted, writes 0x1234 */
   EXPECT(c, "read 0x1234 after non-hoisted write of 0x1234", 0u, 0x1234, KK_RENC_HOIST_RAW);
   EXPECT(c, "read 0x9999 after non-hoisted write of 0x1234", 0u, 0x9999, KK_RENC_HOIST_OK);
   EXPECT(c, "read 0 (no address) never matches", 0u, 0x0, KK_RENC_HOIST_OK);
   /* a hoisted pre side op writing 0x5555 */
   c->renc.hoist_draw = true;
   struct libkk_write_u32_args wa = {0x5555ull, 1u};
   kk_renc_precomp_side_op(c, true, LIBKK_WRITE_U32, &wa, sizeof(wa));
   printf("  %-52s -> hoist_next=%d pass_hoisted=%u\n", "pre side op of a gated draw",
          c->renc.hoist_next, c->renc.pass_hoisted);
   bad += !(c->renc.hoist_next && c->renc.pass_hoisted == 1u);
   draw_done(c);
   EXPECT(c, "read 0x5555 after HOISTED write of 0x5555", 0u, 0x5555, KK_RENC_HOIST_OK);
   kk_renc_side_op(c, false, 8u, KK_RENC_SIDE_ANY, NULL, 0u, NULL, 0u);
   EXPECT(c, "after a non-hoisted ANY side op", 0u, 0x9999, KK_RENC_HOIST_ANY_EARLIER);
   kk_renc_fini(c);
   free(c);

   c = new_cmd(true, true);
   c->argument_table = fake();
   c->gfx.cmd_buf = fake();
   kk_renc_begin(c, d);
   kk_renc_barrier_after_encoder_stages(kk_renc(c), MTL_STAGE_FRAGMENT, MTL_STAGE_VERTEX);
   EXPECT(c, "in-pass barrier before any draw", 0u, 0x10, KK_RENC_HOIST_OK);
   kk_renc_draw_primitives(kk_renc(c), MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0);
   EXPECT(c, "draw, no barrier after it", 0u, 0x10, KK_RENC_HOIST_OK);
   kk_renc_barrier_after_queue_stages(kk_renc(c), MTL_STAGE_ALL, MTL_STAGE_VERTEX);
   EXPECT(c, "in-pass barrier after a draw", 0u, 0x10, KK_RENC_HOIST_BARRIER);
   c->renc.enc_created = true;
   EXPECT(c, "render encoder already created (beats everything)", KK_RENC_SIDE_ANY, 0x10, KK_RENC_HOIST_ENC);
   kk_renc_fini(c);
   free(c);

   /* kk_renc_hoist_draw's own reasons, on a fresh pass */
   c = new_cmd(true, true);
   c->argument_table = fake();
   c->gfx.cmd_buf = fake();
   kk_renc_begin(c, d);
   memset(stats, 0, sizeof(stats));
   bool h = kk_renc_hoist_draw(c, 0u, NULL, 0u, true);
   printf("  %-52s -> hoist=%d refused_heap=%lld\n", "kk_renc_hoist_draw heap_first (fresh pass)",
          h, (long long)stats[KK_BBSTAT_HOIST_REFUSED_HEAP]);
   bad += h || stats[KK_BBSTAT_HOIST_REFUSED_HEAP] != 1;
   void *busy_enc = fake();
   c->pre_gfx->encoder = busy_enc; /* a non-hoist, non-folded encoder */
   h = kk_renc_hoist_draw(c, 0u, NULL, 0u, false);
   printf("  %-52s -> hoist=%d refused_busy=%lld\n", "kk_renc_hoist_draw, pre_gfx holds another encoder",
          h, (long long)stats[KK_BBSTAT_HOIST_REFUSED_BUSY]);
   bad += h || stats[KK_BBSTAT_HOIST_REFUSED_BUSY] != 1;
   c->pre_gfx->encoder = NULL;
   h = kk_renc_hoist_draw(c, 0u, NULL, 0u, false);
   printf("  %-52s -> hoist=%d draws=%lld\n", "kk_renc_hoist_draw, nothing in the way",
          h, (long long)stats[KK_BBSTAT_HOIST_DRAWS]);
   bad += !h || stats[KK_BBSTAT_HOIST_DRAWS] != 1;
   draw_done(c);
   c->renc.hoist = false; /* flag unset: never hoists, never counts */
   memset(stats, 0, sizeof(stats));
   h = kk_renc_hoist_draw(c, 0u, NULL, 0u, false);
   long long sum = 0;
   for (unsigned i = KK_BBSTAT_HOIST_DRAWS; i <= KK_BBSTAT_HOIST_LATE; i++)
      sum += stats[i];
   printf("  %-52s -> hoist=%d counters touched=%lld\n", "flag unset (renc.hoist=false)", h, sum);
   bad += h || sum != 0;
   kk_renc_fini(c);
   free(c);
#undef EXPECT
   printf("predicate table: %s\n", bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}

/* ---- the op stream ------------------------------------------------------- */

int
main(int argc, char **argv)
{
   if (argc >= 2 && !strcmp(argv[1], "predicate"))
      return predicate_table();
   if (argc < 3) return 2;
   const bool deferred = strcmp(argv[1], "direct") != 0;
   const bool hoist = !strcmp(argv[1], "hoist");
   const bool perturb_bind = argc > 3 && !strcmp(argv[3], "perturb-bind");
   perturb_order = argc > 3 && !strcmp(argv[3], "perturb-order");
   char path[1024];
   snprintf(path, sizeof(path), "%s.renc", argv[2]);
   FILE *rl = fopen(path, "w");
   snprintf(path, sizeof(path), "%s.mtl", argv[2]);
   mt = fopen(path, "w");
   snprintf(path, sizeof(path), "%s.hoist", argv[2]);
   hl = fopen(path, "w");
   if (!rl || !mt || !hl) return 2;
   kk_renc_log_set_file_for_test(rl);

   the_table = fake();
   void *cb1 = fake(), *cb2 = fake(), *cb3 = fake();
   void *desc = fake(), *pipe = fake(), *ds = fake();
   struct kk_cmd_buffer *cmd = new_cmd(deferred, hoist);
   cmd->argument_table = the_table;
   cmd->gfx.cmd_buf = cb1;
   kk_renc_set_table_address(cmd, 1u, 0x5A000000ull);
   struct kk_renc *r = kk_renc(cmd);

   /* ---- pass A: two hoistable draws, then a tess draw (ANY) whose M1 sync
    * creates the render encoder, then a predicate draw (ENC) ---- */
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 0);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0);
   {  /* D1: conditional-rendering predicate on an indirect draw */
      uint64_t rd[2] = {0xF2000000ull, 0xF1000000ull};
      gate(cmd, false, rd, 2, false);
      struct libkk_predicate_indirect_neq_zero_args pa = {0xF0000000ull, 0xF1000000ull, 0xF2000000ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_NEQ_ZERO, &pa, sizeof(pa));
      kk_renc_set_table_address(cmd, 2u, perturb_bind ? 0xB0000999ull : 0xB0000040ull);
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF0000000ull);
      draw_done(cmd);
   }
   {  /* D2: unroll reading D1's (hoisted) output; heap already in use */
      uint64_t rd[2] = {0xF0000000ull, 0xD0000000ull};
      gate(cmd, false, rd, 2, false);
      struct libkk_unroll_geometry_args ua = {0xD0000000ull, 0x99000000ull, 0xF0000000ull, 0xF3000000ull, 5, 0xffff, 36, 2, 4, 0, 4};
      side(cmd, true, LIBKK_UNROLL_GEOMETRY, &ua, sizeof(ua));
      kk_renc_draw_indexed_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, MTL_INDEX_TYPE_UINT32,
                                               0x99000100ull, 0x100000, 0xF3000000ull);
      draw_done(cmd);
   }
   kk_renc_set_visibility_result_mode(r, MTL_VISIBILITY_RESULT_MODE_COUNTING, 8);
   {  /* D3: tessellation (memory through the tess state): ANY */
      gate(cmd, true, NULL, 0, false);
      tess_side(cmd, 0x77000000ull);
      kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 96, 1, 0);
      draw_done(cmd);
   }
   kk_renc_set_visibility_result_mode(r, MTL_VISIBILITY_RESULT_MODE_DISABLED, 8);
   {  /* D4: predicate after the render encoder exists: ENC */
      uint64_t rd[1] = {0xF4000000ull};
      gate(cmd, false, rd, 1, false);
      struct libkk_predicate_indirect_eq_zero_args pa = {0xF5000000ull, 0xF6000000ull, 0xF4000000ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_EQ_ZERO, &pa, sizeof(pa));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF5000000ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   /* ---- pass B: in-pass barrier after a draw, then a predicate: BARRIER ---- */
   pass_begin(cmd, desc, false);
   state(cmd, pipe, ds, 1);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_POINT, 1, 2, 3, 4);
   kk_renc_barrier_after_encoder_stages(r, MTL_STAGE_FRAGMENT, MTL_STAGE_VERTEX);
   {  /* D5 */
      uint64_t rd[1] = {0xF7000000ull};
      gate(cmd, false, rd, 1, false);
      struct libkk_predicate_indirect_neq_zero_args pa = {0xF8000000ull, 0xF9000000ull, 0xF7000000ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_NEQ_ZERO, &pa, sizeof(pa));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF8000000ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, false, MTL_STAGE_ALL & ~MTL_STAGE_VERTEX);

   /* ---- pass C (new shared buffer): first heap user: HEAP ---- */
   cmd->gfx.cmd_buf = cb2;
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 2);
   {  /* D6: kk_unroll_geometry -> kk_heap's reset (in-pass post write_u32),
       * then the unroll */
      uint64_t rd[2] = {0xFA000000ull, 0xD2000000ull};
      gate(cmd, false, rd, 2, true);
      struct libkk_write_u32_args wa = {0x99000000ull, 0u};
      side(cmd, false, LIBKK_WRITE_U32, &wa, sizeof(wa));
      struct libkk_unroll_geometry_args ua = {0xD2000000ull, 0x99000000ull, 0xFA000000ull, 0xFB000000ull, 5, 0xffff, 36, 2, 4, 0, 4};
      side(cmd, true, LIBKK_UNROLL_GEOMETRY, &ua, sizeof(ua));
      kk_renc_draw_indexed_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, MTL_INDEX_TYPE_UINT32,
                                               0x99000100ull, 0x100000, 0xFB000000ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   /* ---- pass D: a non-folded compute encoder is open (pre_gfx busy) ---- */
   cmd->gfx.cmd_buf = NULL;
   {
      struct libkk_write_u32_args wa = {0xE0000020ull, 2u};
      side(cmd, true, LIBKK_WRITE_U32, &wa, sizeof(wa)); /* outside a pass */
   }
   cmd->gfx.cmd_buf = cb3;
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 3);
   {  /* D7: BUSY */
      uint64_t rd[1] = {0xFC000000ull};
      gate(cmd, false, rd, 1, false);
      struct libkk_predicate_indirect_neq_zero_args pa = {0xFD000000ull, 0xFE000000ull, 0xFC000000ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_NEQ_ZERO, &pa, sizeof(pa));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xFD000000ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   /* ---- pass E: a gated draw whose second pre side op cannot be hoisted
    * (flagged ANY on its decoded arguments): late refusal; the first stays
    * hoisted ---- */
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 4);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 3, 3, 1, 0);
   {  /* D8 */
      uint64_t rd[2] = {0xF2000010ull, 0xF1000010ull};
      gate(cmd, false, rd, 2, false);
      struct libkk_predicate_indirect_gt_draw_id_args pa = {0xF0000010ull, 0xF1000010ull, 0xF2000010ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_GT_DRAW_ID, &pa, sizeof(pa));
      uint64_t pfx[2] = {0xAA000000ull, 0xAB000000ull};
      side(cmd, true, LIBKK_PREFIX_SUM_TESS, pfx, sizeof(pfx));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF0000010ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   /* ---- pass F: hoisted draw, then an in-pass post write (its M1 sync
    * closes the hoist encoder and creates the render encoder), then a
    * gated draw: ENC ---- */
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 5);
   {  /* D9: hoisted */
      uint64_t rd[1] = {0xF2000030ull};
      gate(cmd, false, rd, 1, false);
      struct libkk_predicate_indirect_neq_zero_args pa = {0xF0000030ull, 0xF1000030ull, 0xF2000030ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_NEQ_ZERO, &pa, sizeof(pa));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF0000030ull);
      draw_done(cmd);
   }
   {
      struct libkk_write_u32_args wa = {0xE0000030ull, 1u};
      side(cmd, false, LIBKK_WRITE_U32, &wa, sizeof(wa)); /* in-pass post */
   }
   {  /* D10: ENC */
      uint64_t rd[1] = {0xF2000040ull};
      gate(cmd, false, rd, 1, false);
      struct libkk_predicate_indirect_neq_zero_args pa = {0xF0000040ull, 0xF1000040ull, 0xF2000040ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_NEQ_ZERO, &pa, sizeof(pa));
      kk_renc_draw_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0xF0000040ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   kk_renc_fini(cmd);
   fclose(rl);
   fclose(mt);
   fclose(hl);
   free(cmd);

   if (hoist) {
      printf("hoist counters: draws=%lld side_ops=%lld encoders=%lld refused=%lld "
             "(enc=%lld any=%lld any_earlier=%lld raw=%lld barrier=%lld heap=%lld busy=%lld) late=%lld\n",
             (long long)stats[KK_BBSTAT_HOIST_DRAWS], (long long)stats[KK_BBSTAT_HOIST_OPS],
             (long long)stats[KK_BBSTAT_HOIST_ENCODERS], (long long)stats[KK_BBSTAT_HOIST_REFUSED],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_ENC], (long long)stats[KK_BBSTAT_HOIST_REFUSED_ANY],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_ANY_EARLIER], (long long)stats[KK_BBSTAT_HOIST_REFUSED_RAW],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_BARRIER], (long long)stats[KK_BBSTAT_HOIST_REFUSED_HEAP],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_BUSY], (long long)stats[KK_BBSTAT_HOIST_LATE]);
   }
   return 0;
}
