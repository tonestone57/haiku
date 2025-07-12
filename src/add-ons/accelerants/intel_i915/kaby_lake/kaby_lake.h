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

#endif /* KABY_LAKE_H */
