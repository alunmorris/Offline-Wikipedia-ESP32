// Written by Alun Morris and Claude Code
#pragma once
#include <Arduino.h>
#include <vector>

struct LinkRegion {
    int16_t  x, y, w, h;   // screen-space coords after applying scroll offset
    uint32_t article_id;
};

struct ImageRegion {
    uint32_t img_id;
    int32_t  doc_y;    // content-space Y of the top of this image placeholder
    int16_t  img_w;    // post-resize JPEG pixel width  (stored in <img w="…">)
    int16_t  img_h;    // post-resize JPEG pixel height (stored in <img h="…">)
    int16_t  thumb_h;  // display height in pixels (≤ 120)
};

struct RenderResult {
    std::vector<LinkRegion>  links;    // tappable link rects (screen space)
    std::vector<ImageRegion> images;   // image placeholder regions (doc space)
    int32_t content_height;            // total pixel height of all rendered content
};

// Render stripped Wikipedia HTML onto the TFT.
//   x0            — left edge of text column (pixels)
//   clip_y        — screen Y of the top of the content area
//   clip_h        — height of the content area in pixels
//   scroll_offset — content-space pixels scrolled off the top
//   col_width     — usable column width (leave 8px right margin for scroll bar)
//
// Only draws content whose screen Y falls within [clip_y, clip_y + clip_h).
// Returns link regions in screen coordinates and the total content height.
RenderResult htmlRender(const char *html, uint32_t html_len,
                        int16_t x0, int16_t clip_y, int16_t clip_h,
                        int32_t scroll_offset, int16_t col_width);
