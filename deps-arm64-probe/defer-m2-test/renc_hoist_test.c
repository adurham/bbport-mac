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
#include "kk_descriptor_set.h"
#include "kk_descriptor_set_layout.h"
#include "kk_image.h"
#include "kk_image_view.h"

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
      es->bb_wa_any = kk_renc_wa_excused_inflight(cmd);
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

/* kk_launch_tess's VS+TCS pre dispatches (kk_cmd_draw.c order): flags from
 * the bound shaders' reflection (kk_renc_tess_side_flags: ANY when no
 * vertex shader is bound, as in the main stream below), vertex params read,
 * tess state written; a gated draw records the node before cs_get_compute. */
static uint64_t tess_vp = 0x76000000ull;
static void
tess_side(struct kk_cmd_buffer *cmd, uint64_t state)
{
   const unsigned s = ++seq;
   const uint32_t f = kk_renc_tess_side_flags(cmd);
   const uint64_t rd[1] = {tess_vp};
   const bool first = cmd->renc.hoist_draw;
   if (first)
      kk_renc_side_op(cmd, true, KK_RENC_SIDE_RAW, f, rd, 1u, &state, 1u);
   void *enc = getc_(cmd, true);
   if (!first)
      kk_renc_side_op(cmd, true, KK_RENC_SIDE_RAW, f, rd, 1u, &state, 1u);
   if (cmd->pre_gfx->bb_hoisted && enc == cmd->pre_gfx->encoder)
      fprintf(hl, "%u\n", s);
   M(enc, "barrier_after_encoder_stages %x %x", MTL_STAGE_DISPATCH, MTL_STAGE_DISPATCH);
   dispatch(cmd, enc, 9000u, s);
}

/* kk_draw -> kk_bb_hoist_gate (the draw has pre-gfx work) */
static void
gate(struct kk_cmd_buffer *cmd, bool tess, const uint64_t *reads, uint32_t nr,
     bool heap_first)
{
   kk_renc_hoist_draw(cmd, tess ? kk_renc_tess_side_flags(cmd) : 0u, reads, nr,
                      heap_first);
}

/* kk_draw's end (kk_renc_hoist_draw_end) */
static void
draw_done(struct kk_cmd_buffer *cmd)
{
   kk_renc_hoist_draw_end(cmd);
}

/* kk_CmdBeginRendering's kk_renc_rendering_begin: one color attachment in
 * `layout` */
static void
rendering_begin(struct kk_cmd_buffer *cmd, VkImageLayout layout)
{
   VkRenderingAttachmentInfo ca = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = (VkImageView)(uintptr_t)0x1,
      .imageLayout = layout,
   };
   VkRenderingInfo ri = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachments = &ca,
   };
   kk_renc_rendering_begin(cmd, &ri);
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
   kk_renc_rendering_end(cmd);
}

static void
pass_begin(struct kk_cmd_buffer *cmd, void *desc, bool entry_barrier)
{
   rendering_begin(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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
   /* ---- pass write-set predicate (SHADER_WRITE / XFB / FEEDBACK) ---- */
   {
      struct kk_shader *vs_ro = calloc(1, sizeof(*vs_ro));
      struct kk_shader *vs_w = calloc(1, sizeof(*vs_w));
      struct kk_shader *vs_x = calloc(1, sizeof(*vs_x));
      vs_w->info.bb_mem = vs_w->info.bb_mem_stage = KK_SHADER_MEM_WRITES;
      vs_x->info.bb_mem = vs_x->info.bb_mem_stage = KK_SHADER_MEM_XFB;
      uint32_t tf;
#define NEWPASS(layout)                                                        \
   do {                                                                        \
      c = new_cmd(true, true);                                                 \
      c->argument_table = fake();                                              \
      c->gfx.cmd_buf = fake();                                                 \
      rendering_begin(c, layout);                                              \
      kk_renc_begin(c, d);                                                     \
   } while (0)
#define DRAW_WITH(vs)                                                          \
   do {                                                                        \
      c->state.shaders[MESA_SHADER_VERTEX] = (vs);                             \
      kk_renc_note_draw(c);                                                    \
      kk_renc_draw_primitives(kk_renc(c), MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0); \
   } while (0)
#define DONE() do { kk_renc_fini(c); free(c); } while (0)
      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      c->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
      tf = kk_renc_tess_side_flags(c);
      printf("  %-52s -> 0x%x %s\n", "tess side-op flags, read-only VS (SHADER, not ANY)",
             tf, tf == KK_RENC_SIDE_SHADER ? "ok" : "MISMATCH");
      bad += tf != KK_RENC_SIDE_SHADER;
      EXPECT(c, "tess op, fresh pass, attachment layout", tf, 0x10, KK_RENC_HOIST_OK);
      DRAW_WITH(vs_ro);
      EXPECT(c, "tess op after a read-only draw (attachment writes only)", tf, 0x10, KK_RENC_HOIST_OK);
      c->state.shaders[MESA_SHADER_VERTEX] = vs_w;
      tf = kk_renc_tess_side_flags(c);
      EXPECT(c, "tess op whose own VS writes memory, after a draw", tf, 0x10, KK_RENC_HOIST_SHADER_WRITE);
      DONE();

      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      DRAW_WITH(vs_w);
      c->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
      EXPECT(c, "tess op after a draw whose pipeline writes memory", KK_RENC_SIDE_SHADER, 0x10, KK_RENC_HOIST_SHADER_WRITE);
      EXPECT(c, "predicate op after a draw whose pipeline writes memory", 0u, 0x10, KK_RENC_HOIST_SHADER_WRITE);
      DONE();

      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      DRAW_WITH(vs_x);
      c->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
      EXPECT(c, "tess op after a draw with transform feedback", KK_RENC_SIDE_SHADER, 0x10, KK_RENC_HOIST_XFB);
      DONE();

      NEWPASS(VK_IMAGE_LAYOUT_GENERAL);
      EXPECT(c, "tess op, attachment in GENERAL", KK_RENC_SIDE_SHADER, 0x10, KK_RENC_HOIST_FEEDBACK);
      EXPECT(c, "predicate op (no descriptor reads), GENERAL", 0u, 0x10, KK_RENC_HOIST_OK);
      DONE();
      NEWPASS(VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
      EXPECT(c, "tess op, attachment FEEDBACK_LOOP_OPTIMAL", KK_RENC_SIDE_SHADER, 0x10, KK_RENC_HOIST_FEEDBACK);
      DONE();
      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      c->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
      c->vk.dynamic_graphics_state.feedback_loops = VK_IMAGE_ASPECT_COLOR_BIT;
      tf = kk_renc_tess_side_flags(c);
      EXPECT(c, "tess op, dynamic feedback-loop state on", tf, 0x10, KK_RENC_HOIST_FEEDBACK);
      DONE();
      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      DRAW_WITH(vs_ro);
      kk_renc_barrier_after_encoder_stages(kk_renc(c), MTL_STAGE_FRAGMENT, MTL_STAGE_VERTEX);
      EXPECT(c, "tess op after draw + in-pass barrier", KK_RENC_SIDE_SHADER, 0x10, KK_RENC_HOIST_BARRIER);
      DONE();
      NEWPASS(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      c->state.shaders[MESA_SHADER_VERTEX] = NULL;
      tf = kk_renc_tess_side_flags(c);
      EXPECT(c, "tess op, no VS reflection: ANY kept", tf, 0x10, KK_RENC_HOIST_ANY);
      kk_renc_side_op(c, true, KK_RENC_SIDE_RAW, KK_RENC_SIDE_SHADER, NULL, 0u, NULL, 0u);
      EXPECT(c, "after a non-hoisted tess op (reads unlisted)", KK_RENC_SIDE_SHADER | KK_RENC_SIDE_SHADER_WRITES, 0x10, KK_RENC_HOIST_SHADER_WRITE);
      DONE();
      free(vs_ro);
      free(vs_w);
      free(vs_x);
#undef NEWPASS
#undef DRAW_WITH
#undef DONE
   }
#undef EXPECT
   printf("predicate table: %s\n", bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}

/* ---- oracle test mode ---------------------------------------------------- */
/* Real driver structures, filled the way the driver fills them: a
 * descriptor set (kk_descriptor_set_layout + the descriptor bytes
 * kk_descriptor_set.c writes: kk_buffer_address for a storage buffer,
 * kk_sampled_image_descriptor with a GPU resource id for a sampled image),
 * image views registered by resource id exactly as kk_image_view.c does
 * (kk_renc_oracle_view_range), and shaders whose bb_* reflection names the
 * (set, binding) they access. The pre op goes through the REAL gate
 * (kk_renc_hoist_draw, then kk_renc_side_op with the tess flags), so a
 * hoisted op is checked by the oracle inside kk_renc_slow_side_op, at hoist
 * time; refused ops are also given to kk_renc_oracle_conflict directly to
 * show that the oracle agrees. */

struct oracle_world {
   struct kk_descriptor_set_layout *layout;
   struct kk_descriptor_set *set;
   uint8_t map[256];
   struct kk_image img_att, img_tex;
   struct kk_image_view view_att, view_tex;
   struct kk_shader vs_reader, vs_writer, vs_liar, vs_samples_att;
};

static void
oracle_view(struct kk_image *img, struct kk_image_view *v, uint64_t addr,
            uint64_t size, uint64_t id)
{
   memset(img, 0, sizeof(*img));
   memset(v, 0, sizeof(*v));
   img->plane_count = 1;
   img->planes[0].addr = addr;
   img->planes[0].layout.size_B = size;
   v->vk.image = &img->vk;
   v->vk.view_type = VK_IMAGE_VIEW_TYPE_2D;
   v->vk.level_count = 1;
   v->vk.layer_count = 1;
   v->plane_count = 1;
   v->planes[0].sampled_gpu_resource_id = id;
   /* kk_image_view_init's registration */
   const struct kk_renc_range rg = kk_renc_oracle_view_range(v, 0);
   kk_renc_oracle_register(id, &rg);
}

static void
oracle_world_init(struct oracle_world *w)
{
   memset(w, 0, sizeof(*w));
   /* set 0: binding 0 = STORAGE_BUFFER [0x40000000, +0x1000),
    *        binding 1 = SAMPLED_IMAGE (the texture),
    *        binding 2 = SAMPLED_IMAGE (the color attachment's image) */
   w->layout = calloc(1, sizeof(*w->layout) + 3 * sizeof(w->layout->binding[0]));
   w->layout->binding_count = 3;
   w->layout->binding[0] = (struct kk_descriptor_set_binding_layout){
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .array_size = 1, .offset = 0,
      .stride = sizeof(struct kk_buffer_address)};
   w->layout->binding[1] = (struct kk_descriptor_set_binding_layout){
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .array_size = 1, .offset = 64,
      .stride = sizeof(struct kk_sampled_image_descriptor)};
   w->layout->binding[2] = (struct kk_descriptor_set_binding_layout){
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .array_size = 1, .offset = 128,
      .stride = sizeof(struct kk_sampled_image_descriptor)};
   w->set = calloc(1, sizeof(*w->set));
   w->set->layout = w->layout;
   w->set->mapped_ptr = w->map;
   w->set->size = sizeof(w->map);
   w->set->addr = 0x3F000000ull;
   oracle_view(&w->img_att, &w->view_att, 0x80000000ull, 0x100000ull, 0xA77ull);
   oracle_view(&w->img_tex, &w->view_tex, 0x90000000ull, 0x100000ull, 0x7E7ull);
   /* write_buffer_desc / get_sampled_image_view_desc */
   const struct kk_buffer_address ba = {.base_addr = 0x40000000ull, .size = 0x1000};
   memcpy(w->map + 0, &ba, sizeof(ba));
   struct kk_sampled_image_descriptor sd = {0};
   sd.image_gpu_resource_id = w->view_tex.planes[0].sampled_gpu_resource_id;
   memcpy(w->map + 64, &sd, sizeof(sd));
   sd.image_gpu_resource_id = w->view_att.planes[0].sampled_gpu_resource_id;
   memcpy(w->map + 128, &sd, sizeof(sd));
   /* reflection (kk_bb_reflect_memory's output) */
   w->vs_reader.info.bb_binding_count = 2; /* reads the SSBO and the texture */
   w->vs_reader.info.bb_bindings[0] = KK_SHADER_BB_BINDING(0, 0, false);
   w->vs_reader.info.bb_bindings[1] = KK_SHADER_BB_BINDING(0, 1, false);
   w->vs_writer.info.bb_mem = w->vs_writer.info.bb_mem_stage = KK_SHADER_MEM_WRITES;
   w->vs_writer.info.bb_binding_count = 1; /* stores to the SSBO */
   w->vs_writer.info.bb_bindings[0] = KK_SHADER_BB_BINDING(0, 0, true);
   /* BROKEN reflection: names the SSBO store but lost the WRITES bit */
   w->vs_liar = w->vs_writer;
   w->vs_liar.info.bb_mem = w->vs_liar.info.bb_mem_stage = 0u;
   /* samples the color attachment's own image (a feedback loop) */
   w->vs_samples_att.info.bb_binding_count = 1;
   w->vs_samples_att.info.bb_bindings[0] = KK_SHADER_BB_BINDING(0, 2, false);
}

static const char *const verdict_name[] = {"clear", "CONFLICT", "undecoded-only"};

/* One scenario: a pass whose recorded draw uses `earlier_vs` (NULL: none),
 * optionally an in-pass barrier, then a gated tess draw whose VS is
 * `tess_vs`. Returns the gate decision; *v = the oracle's own verdict. */
static enum kk_renc_hoist_why
oracle_case(struct oracle_world *w, VkImageLayout layout,
            struct kk_shader *earlier_vs, bool barrier, struct kk_shader *tess_vs,
            enum kk_renc_oracle_verdict *v, int64_t *mismatches)
{
   memset(stats, 0, sizeof(stats));
   struct kk_cmd_buffer *c = new_cmd(true, true);
   c->argument_table = fake();
   c->gfx.cmd_buf = fake();
   c->state.gfx.descriptors.sets[0] = w->set;
   c->state.gfx.descriptors.root.sets[0] = w->set->addr;
   c->state.gfx.render.color_att_count = 1;
   c->state.gfx.render.color_att[0].iview = &w->view_att;
   rendering_begin(c, layout);
   kk_renc_begin(c, fake());
   if (earlier_vs != NULL) {
      c->state.shaders[MESA_SHADER_VERTEX] = earlier_vs;
      kk_renc_note_draw(c); /* kk_dispatch_draw */
      kk_renc_draw_primitives(kk_renc(c), MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0);
   }
   if (barrier)
      kk_renc_barrier_after_encoder_stages(kk_renc(c), MTL_STAGE_FRAGMENT,
                                           MTL_STAGE_VERTEX);
   c->state.shaders[MESA_SHADER_VERTEX] = tess_vs;
   const uint32_t f = kk_renc_tess_side_flags(c);
   const uint64_t rd[1] = {0x76000000ull}, st = 0x77000000ull;
   /* kk_bb_hoist_gate, then kk_launch_tess's side op (hoist first) */
   const bool gated = kk_renc_hoist_draw(c, f, NULL, 0u, false);
   enum kk_renc_hoist_why why = KK_RENC_HOIST_OK;
   if (!gated)
      why = kk_renc_hoist_check(c, f, NULL, 0u);
   /* the oracle's own verdict, before the op's node exists */
   *v = kk_renc_oracle_conflict(c, f, rd, 1u, &st, 1u, false);
   if (gated) {
      kk_renc_side_op(c, true, KK_RENC_SIDE_RAW, f, rd, 1u, &st, 1u);
      if (!c->renc.hoist_next) /* refused late */
         why = KK_RENC_HOIST_WHY_COUNT;
   }
   kk_renc_hoist_draw_end(c);
   *mismatches = stats[KK_BBSTAT_HOIST_ORACLE_MISMATCH];
   kk_renc_rendering_end(c);
   kk_renc_fini(c);
   free(c);
   return why;
}

static int
oracle_mode(void)
{
   kk_renc_log_set_file_for_test(NULL);
   kk_renc_hoist_set_flags_for_test(0, 1); /* BB_KK_HOIST_ORACLE=1 */
   mt = fopen("/dev/null", "w");
   struct oracle_world w;
   oracle_world_init(&w);
   int bad = 0;
   enum kk_renc_oracle_verdict v;
   int64_t mm;
   enum kk_renc_hoist_why why;
#define ROW(name, want_why, want_v, want_mm)                                    \
   do {                                                                        \
      const bool ok = why == (want_why) && v == (want_v) && mm == (want_mm);   \
      printf("  %-58s gate=%-12s oracle=%-8s mismatches=%lld -> %s\n", name,  \
             why == KK_RENC_HOIST_WHY_COUNT ? "late" : kk_renc_hoist_why_name(why), \
             verdict_name[v], (long long)mm, ok ? "ok" : "FAIL");              \
      bad += !ok;                                                              \
   } while (0)
   printf("(i) earlier draw's pipeline stores to the SSBO the tess VS reads:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &w.vs_writer,
                     false, &w.vs_reader, &v, &mm);
   ROW("gate refuses, oracle agrees (decoded overlap)", KK_RENC_HOIST_SHADER_WRITE,
       KK_RENC_ORACLE_CONFLICT, 0);
   printf("(ii) earlier draw writes only its attachments:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &w.vs_reader,
                     false, &w.vs_reader, &v, &mm);
   ROW("gate hoists, oracle checked at hoist time: clear", KK_RENC_HOIST_OK,
       KK_RENC_ORACLE_CLEAR, 0);
   printf("(iii) in-pass barrier after the earlier draw:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &w.vs_reader,
                     true, &w.vs_reader, &v, &mm);
   ROW("gate refuses (barrier)", KK_RENC_HOIST_BARRIER, KK_RENC_ORACLE_CLEAR, 0);
   printf("(iii-b) tess VS samples the attachment, layout GENERAL:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_GENERAL, &w.vs_reader, false,
                     &w.vs_samples_att, &v, &mm);
   ROW("gate refuses (feedback), oracle agrees (attachment overlap)",
       KK_RENC_HOIST_FEEDBACK, KK_RENC_ORACLE_CONFLICT, 0);
   printf("(iv) deliberately broken: reflection lost the WRITES bit:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &w.vs_liar,
                     false, &w.vs_reader, &v, &mm);
   ROW("gate hoists, oracle decodes the SSBO store: MISMATCH caught",
       KK_RENC_HOIST_OK, KK_RENC_ORACLE_CONFLICT, 1);
   printf("(iv-b) deliberately broken: attachment sampled, layout claims ATTACHMENT_OPTIMAL:\n");
   why = oracle_case(&w, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &w.vs_reader,
                     false, &w.vs_samples_att, &v, &mm);
   ROW("gate hoists, oracle decodes the attachment read: MISMATCH caught",
       KK_RENC_HOIST_OK, KK_RENC_ORACLE_CONFLICT, 1);
#undef ROW
   printf("oracle test: %s\n", bad ? "FAIL" : "PASS");
   return bad ? 1 : 0;
}

/* ---- the op stream ------------------------------------------------------- */

int
main(int argc, char **argv)
{
   if (argc >= 2 && !strcmp(argv[1], "predicate"))
      return predicate_table();
   if (argc >= 2 && !strcmp(argv[1], "oracle"))
      return oracle_mode();
   /* BB_KK_HOIST_WHY_ALL / BB_KK_HOIST_ORACLE from the environment for the
    * op-stream modes (the latches read the env on first use) */
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
    * (write_u32_array: (address, value) pairs read from memory, flagged ANY
    * on its decoded arguments): late refusal; the first stays hoisted ---- */
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 4);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 3, 3, 1, 0);
   {  /* D8 */
      uint64_t rd[2] = {0xF2000010ull, 0xF1000010ull};
      gate(cmd, false, rd, 2, false);
      struct libkk_predicate_indirect_gt_draw_id_args pa = {0xF0000010ull, 0xF1000010ull, 0xF2000010ull, 5, 5};
      side(cmd, true, LIBKK_PREDICATE_INDIRECT_GT_DRAW_ID, &pa, sizeof(pa));
      uint64_t wa[1] = {0xAA000000ull};
      side(cmd, true, LIBKK_WRITE_U32_ARRAY, wa, sizeof(wa));
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

   /* ---- pass G: tessellation whose VS/TCS only read, after a draw that
    * writes only its attachments: the whole tess chain (VS+TCS dispatch,
    * tessellator count, prefix sum, tessellator) is hoisted (SHADER, not
    * ANY; the libkk tess programs are DRIVER) ---- */
   struct kk_shader *vs_ro = calloc(1, sizeof(*vs_ro));
   struct kk_shader *vs_w = calloc(1, sizeof(*vs_w));
   vs_w->info.bb_mem = vs_w->info.bb_mem_stage = KK_SHADER_MEM_WRITES;
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 6);
   cmd->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
   kk_renc_note_draw(cmd);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0);
   {  /* D11 */
      gate(cmd, true, NULL, 0, false);
      tess_side(cmd, 0x78000000ull);
      struct libkk_tess_tri_args ta = {0x78000000ull, 0};
      side(cmd, true, LIBKK_TESS_TRI, &ta, sizeof(ta));
      struct libkk_prefix_sum_tess_args pa = {0x78000000ull};
      side(cmd, true, LIBKK_PREFIX_SUM_TESS, &pa, sizeof(pa));
      ta.tess_mode = 1;
      side(cmd, true, LIBKK_TESS_TRI, &ta, sizeof(ta));
      kk_renc_note_draw(cmd);
      kk_renc_draw_indexed_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, MTL_INDEX_TYPE_UINT32,
                                               0x99000100ull, 0x100000, 0x78000100ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);

   /* ---- pass H: the same tess draw after a draw whose pipeline writes
    * memory: SHADER_WRITE, M1 path ---- */
   pass_begin(cmd, desc, true);
   state(cmd, pipe, ds, 7);
   cmd->state.shaders[MESA_SHADER_VERTEX] = vs_w;
   kk_renc_note_draw(cmd);
   kk_renc_draw_primitives(r, MTL_PRIMITIVE_TYPE_TRIANGLE, 0, 3, 1, 0);
   cmd->state.shaders[MESA_SHADER_VERTEX] = vs_ro;
   {  /* D12 */
      gate(cmd, true, NULL, 0, false);
      tess_side(cmd, 0x79000000ull);
      struct libkk_tess_tri_args ta = {0x79000000ull, 0};
      side(cmd, true, LIBKK_TESS_TRI, &ta, sizeof(ta));
      kk_renc_note_draw(cmd);
      kk_renc_draw_indexed_primitives_indirect(r, MTL_PRIMITIVE_TYPE_TRIANGLE, MTL_INDEX_TYPE_UINT32,
                                               0x99000100ull, 0x100000, 0x79000100ull);
      draw_done(cmd);
   }
   store_ops(cmd);
   pass_end(cmd, true, MTL_STAGE_ALL);
   cmd->state.shaders[MESA_SHADER_VERTEX] = NULL;

   kk_renc_fini(cmd);
   fclose(rl);
   fclose(mt);
   fclose(hl);
   free(cmd);
   free(vs_ro);
   free(vs_w);

   if (hoist) {
      printf("hoist counters: draws=%lld side_ops=%lld encoders=%lld refused=%lld "
             "(enc=%lld any=%lld any_earlier=%lld raw=%lld barrier=%lld heap=%lld busy=%lld "
             "shader_write=%lld xfb=%lld feedback=%lld) late=%lld\n",
             (long long)stats[KK_BBSTAT_HOIST_DRAWS], (long long)stats[KK_BBSTAT_HOIST_OPS],
             (long long)stats[KK_BBSTAT_HOIST_ENCODERS], (long long)stats[KK_BBSTAT_HOIST_REFUSED],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_ENC], (long long)stats[KK_BBSTAT_HOIST_REFUSED_ANY],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_ANY_EARLIER], (long long)stats[KK_BBSTAT_HOIST_REFUSED_RAW],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_BARRIER], (long long)stats[KK_BBSTAT_HOIST_REFUSED_HEAP],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_BUSY],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_SHADER_WRITE],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_XFB],
             (long long)stats[KK_BBSTAT_HOIST_REFUSED_FEEDBACK],
             (long long)stats[KK_BBSTAT_HOIST_LATE]);
      if (kk_renc_hoist_why_all()) {
         printf("why-all: evals=%lld late_evals=%lld refused=%lld only_any=%lld any_cascade=%lld "
                "renderings=%lld hoisted=%lld if_any=%lld reasons:",
                (long long)stats[KK_BBSTAT_HOIST_WA_EVALS], (long long)stats[KK_BBSTAT_HOIST_WA_LATE_EVALS],
                (long long)stats[KK_BBSTAT_HOIST_WA_REFUSED], (long long)stats[KK_BBSTAT_HOIST_WA_ONLY_ANY],
                (long long)stats[KK_BBSTAT_HOIST_WA_ANY_CASCADE], (long long)stats[KK_BBSTAT_HOIST_WA_REND],
                (long long)stats[KK_BBSTAT_HOIST_WA_REND_HOISTED], (long long)stats[KK_BBSTAT_HOIST_WA_REND_IF_ANY]);
         for (unsigned w = 1; w < KK_RENC_HOIST_WHY_COUNT; w++)
            printf(" %s=%lld", kk_renc_hoist_why_name((enum kk_renc_hoist_why)w),
                   (long long)stats[KK_BBSTAT_HOIST_WA_REASON + w]);
         printf("\n");
      }
      if (kk_renc_hoist_oracle()) {
         printf("oracle: checked=%lld clear=%lld mismatches=%lld undecoded=%lld\n",
                (long long)stats[KK_BBSTAT_HOIST_ORACLE_CHECKS], (long long)stats[KK_BBSTAT_HOIST_ORACLE_CLEAR],
                (long long)stats[KK_BBSTAT_HOIST_ORACLE_MISMATCH], (long long)stats[KK_BBSTAT_HOIST_ORACLE_UNDECODED]);
         if (stats[KK_BBSTAT_HOIST_ORACLE_MISMATCH] != 0)
            return 4; /* a mismatch fails the test mode */
      }
   }
   return 0;
}
