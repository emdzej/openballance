/* GPU interface: the gasm:gfx subset (gasm ABI 0, gfx as of gasm 0.4.0), one call per import, with
   WebGPU-style JSON descriptors. platform_gasm.c forwards to the gasm runner; gpu_null.c (tests) validates
   nothing and returns handles. Semantics: gasm spec/ABI.md, "gasm:gfx". */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { GPU_USAGE_INDEX = 0x10, GPU_USAGE_VERTEX = 0x20, GPU_USAGE_UNIFORM = 0x40, GPU_USAGE_STORAGE = 0x80 };
enum { GPU_STAGE_VERTEX = 1, GPU_STAGE_FRAGMENT = 2 };
enum { GPU_INDEX_U16 = 0, GPU_INDEX_U32 = 1 };

uint32_t gpu_width(void);
uint32_t gpu_height(void);
uint32_t gpu_create_shader(const char *wgsl);
uint32_t gpu_create_buffer(uint32_t size, uint32_t usage);
void gpu_write_buffer(uint32_t buf, uint32_t offset, const void *data, uint32_t len);
uint32_t gpu_create_texture(const char *json);
void gpu_write_texture(uint32_t tex, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *rgba);
uint32_t gpu_create_sampler(const char *json);
uint32_t gpu_create_bind_group_layout(const char *json);
uint32_t gpu_create_pipeline(const char *json);
uint32_t gpu_create_bind_group(const char *json);
bool gpu_begin_frame(float r, float g, float b, float a);   /* false: the frame won't be shown */
void gpu_set_pipeline(uint32_t p);
void gpu_set_bind_group(uint32_t index, uint32_t bg);
void gpu_set_bind_group_offsets(uint32_t index, uint32_t bg, const uint32_t *offsets, uint32_t count);
void gpu_set_viewport(float x, float y, float w, float h, float min_depth, float max_depth);
void gpu_set_scissor_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gpu_set_vertex_buffer(uint32_t slot, uint32_t buf, uint32_t offset);
void gpu_set_index_buffer(uint32_t buf, uint32_t format, uint32_t offset);
void gpu_draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance);
void gpu_draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t base_vertex,
                      uint32_t first_instance);
void gpu_end_frame(void);
