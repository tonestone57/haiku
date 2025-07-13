/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */
#ifndef KABY_LAKE_H
#define KABY_LAKE_H

#include "intel_i915.h"

// --- 2D Acceleration ---
void kaby_lake_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count);
void kaby_lake_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count);
void kaby_lake_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count);
void kaby_lake_fill_span(engine_token *et, uint32 color, uint16 *list, uint32 count);

void kaby_lake_screen_to_screen_transparent_blit(engine_token* et,
	blit_params* list, uint32 count);
void kaby_lake_screen_to_screen_monochrome_blit(engine_token* et,
	blit_params* list, uint32 count);

void kaby_lake_draw_line(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2);

void kaby_lake_fill_polygon(engine_token* et, uint32 color, uint32 count,
	const int16* points);

void kaby_lake_alpha_blend(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2);

void kaby_lake_color_key(engine_token* et, uint32 color, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2);

void kaby_lake_stretch_blit(engine_token* et,
	blit_params* list, uint32 count);

void kaby_lake_set_clip_rect(engine_token* et, uint32 x1, uint32 y1,
	uint32 x2, uint32 y2);

// --- Hardware Cursor ---
void kaby_lake_set_cursor(engine_token* et, uint32 x, uint32 y);
void kaby_lake_set_cursor_bitmap(engine_token* et, const uint8* bitmap);
void kaby_lake_show_cursor(engine_token* et);
void kaby_lake_hide_cursor(engine_token* et);

// --- Video Overlays ---
void kaby_lake_configure_overlay(engine_token* et, const overlay_buffer* buffer,
	const overlay_window* window, const overlay_view* view);
void kaby_lake_release_overlay(engine_token* et);

// --- Memory Management ---
void kaby_lake_init_mem();
void kaby_lake_uninit_mem();
status_t get_cmd_buffer(size_t size, uint32* handle, area_id* area, void** cpu_addr);
void put_cmd_buffer(uint32 handle, area_id area);

// --- Blitting ---
void kaby_lake_emit_blit(uint32* cmd_buf, uint32* cur_dw_idx, const blit_params* blit,
	uint32 cmd_dw0);

#endif /* KABY_LAKE_H */
