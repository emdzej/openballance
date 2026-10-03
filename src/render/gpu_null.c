/* GPU backend for headless tests and tools: every create returns a new handle, everything else is a no-op.
   Not part of the gasm build (platform_gasm.c provides the real calls). */
#include "gpu.h"

static uint32_t next_handle = 1;
static uint32_t H(void) { return next_handle++; }

uint32_t gpu_width(void) { return 640; }
uint32_t gpu_height(void) { return 480; }
uint32_t gpu_create_shader(const char *wgsl) { (void)wgsl; return H(); }
uint32_t gpu_create_buffer(uint32_t size, uint32_t usage) { (void)size; (void)usage; return H(); }
void gpu_write_buffer(uint32_t buf, uint32_t offset, const void *data, uint32_t len) { (void)buf; (void)offset; (void)data; (void)len; }
uint32_t gpu_create_texture(const char *json) { (void)json; return H(); }
void gpu_write_texture(uint32_t tex, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *rgba)
{
    (void)tex; (void)mip; (void)x; (void)y; (void)w; (void)h; (void)rgba;
}
uint32_t gpu_create_sampler(const char *json) { (void)json; return H(); }
uint32_t gpu_create_bind_group_layout(const char *json) { (void)json; return H(); }
uint32_t gpu_create_pipeline(const char *json) { (void)json; return H(); }
uint32_t gpu_create_bind_group(const char *json) { (void)json; return H(); }
bool gpu_begin_frame(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; return false; }
void gpu_set_pipeline(uint32_t p) { (void)p; }
void gpu_set_bind_group(uint32_t index, uint32_t bg) { (void)index; (void)bg; }
void gpu_set_bind_group_offsets(uint32_t index, uint32_t bg, const uint32_t *o, uint32_t n) { (void)index; (void)bg; (void)o; (void)n; }
void gpu_set_viewport(float x, float y, float w, float h, float a, float b) { (void)x; (void)y; (void)w; (void)h; (void)a; (void)b; }
void gpu_set_scissor_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)x; (void)y; (void)w; (void)h; }
void gpu_set_vertex_buffer(uint32_t slot, uint32_t buf, uint32_t offset) { (void)slot; (void)buf; (void)offset; }
void gpu_set_index_buffer(uint32_t buf, uint32_t format, uint32_t offset) { (void)buf; (void)format; (void)offset; }
void gpu_draw(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)a; (void)b; (void)c; (void)d; }
void gpu_draw_indexed(uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void gpu_end_frame(void) {}
void gpu_destroy(uint32_t handle) { (void)handle; }
