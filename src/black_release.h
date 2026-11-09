/* Guest addresses of the local retail release.
 *
 * Hard-coded guest addresses in recomp_manual.c are written for the NTSC
 * release (the canonical layout). BLACK_VA maps one to the release that
 * scripts/generate.py found in game/, and folds to a constant. See
 * scripts/black_release.py for how the layouts were matched.
 */
#pragma once
#include "black_release_gen.h"

#define BLACK_CANONICAL_TEXT_END 0x217FC0u
#define BLACK_IMAGE_END 0x600000u

#define BLACK_VA(va) ((uint32_t)(va) + \
    ((uint32_t)(va) >= BLACK_CANONICAL_TEXT_END && (uint32_t)(va) < BLACK_IMAGE_END ? BLACK_POST_SHIFT : \
     (uint32_t)(va) >= BLACK_TEXT_CUT_HIGH && (uint32_t)(va) < BLACK_CANONICAL_TEXT_END ? BLACK_TEXT_SHIFT : 0u))
