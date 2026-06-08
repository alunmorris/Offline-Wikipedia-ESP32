// Written by Alun Morris and Claude Code
#include "ui.h"
#include "config.h"
#include "display.h"
#include "html_render.h"
#include "touch.h"
#include "keyboard.h"
#include "wiki_db.h"
#include <vector>

// ---- states ------------------------------------------------------------------

enum UIState { STATE_SEARCH, STATE_ARTICLE, STATE_IMAGE };
static UIState g_state = STATE_SEARCH;

// ---- search state ------------------------------------------------------------

static String                    g_query;
static std::vector<SearchResult> g_results;
static int                       g_results_scroll = 0;   // first visible result index
static bool                      g_kb_visible = false;
static bool                      g_cursor_on  = true;
static uint32_t                  g_cursor_ms  = 0;

// Search input bar geometry
#define SEARCH_INPUT_Y       NAV_H           // bar sits directly below nav
#define SEARCH_INPUT_H       28              // height of input bar

// Results list geometry (keyboard visible vs. hidden)
#define SEARCH_RESULTS_Y_KB  (SEARCH_INPUT_Y + SEARCH_INPUT_H)   // below input when KB shown
#define SEARCH_RESULTS_H_KB  (KB_TOP_Y - SEARCH_RESULTS_Y_KB)    // space above keyboard
#define SEARCH_RESULTS_Y     NAV_H           // results fill below nav when KB hidden
#define SEARCH_RESULTS_H     (SCREEN_H - NAV_H)

#define SEARCH_RESULT_H      18   // font2=16px + 1px pad; 212/18=11 results

// Search behaviour
#define SEARCH_MAX_RESULTS     20    // results per category (prefix + contains)
#define SEARCH_INPUT_MAX_CHARS 34    // visible chars in search box (font 2)
#define SEARCH_RESULT_MAX_CHARS 36   // visible chars per result row

// Article navigation
#define PAGE_SCROLL_OVERLAP    20    // px of overlap kept when paging for context
#define HISTORY_MAX            30    // max back-navigation entries

// Cursor blink
#define CURSOR_BLINK_MS       500

// Sentinel article_id marking a results-list divider (not a real article)
static const uint32_t SEARCH_DIVIDER_ID = UINT32_MAX;

// ---- article state -----------------------------------------------------------

static uint32_t                  g_art_id = UINT32_MAX;
static String                    g_art_title;
// Current page HTML: a malloc'd buffer containing one page-worth of HTML plus
// injected Prev/Next navigation links.  Freed when the next page or article loads.
static uint8_t                  *g_art_html_buf = nullptr;
static const char               *g_art_html     = nullptr;
static uint32_t                  g_art_html_len  = 0;
static int32_t                   g_art_scroll = 0;

// Pagination state
static uint32_t                  g_art_page_num  = 0;    // current page / block (0-based)
static bool                      g_art_has_next  = false;
static bool                      g_art_pre_paged = false; // true = WKI2 format (pre-sliced blocks)
// g_art_page_starts[n]: byte offset in full HTML where page n begins (WIKI old format only).
static std::vector<uint32_t>     g_art_page_starts;
static int32_t                   g_art_total_h = 0;
static std::vector<LinkRegion>   g_art_links;
static std::vector<ImageRegion>  g_art_images;

// Navigation back-stack (article IDs)
static std::vector<uint32_t>     g_history;

// Full-screen image state
static uint32_t  g_full_img_id     = UINT32_MAX;
static int16_t   g_full_img_w      = 0;
static int16_t   g_full_img_h      = 0;
static int32_t   g_art_scroll_save = 0;   // scroll position to restore when exiting STATE_IMAGE

#define ART_CONTENT_Y  NAV_H
#define ART_CONTENT_H  (SCREEN_H - NAV_H)   // 212px
#define ART_COL_W      (SCREEN_W - 8)        // leave 8px for scroll bar

// Scroll buttons in nav bar (article view only): 4 × 24px on the right edge.
// Order: page-up | page-down | top | bottom
#define NAV_BTN_W          24
#define NAV_BTN_COUNT       4
#define NAV_SCROLL_BTN_X   (SCREEN_W - NAV_BTN_W * NAV_BTN_COUNT)   // 224

// ---- drawing helpers ---------------------------------------------------------

static void drawNavScrollBtns() {
    // 4 buttons right-aligned.
    // Icons: ⬆ ⬇ ⬆⬆ ⬇⬇  (single/double arrow = page / top-bottom)
    int16_t cy = NAV_H / 2;
    for (int i = 0; i < NAV_BTN_COUNT; i++) {
        int16_t bx = NAV_SCROLL_BTN_X + i * NAV_BTN_W;
        int16_t cx = bx + NAV_BTN_W / 2;
        tft.drawFastVLine(bx, 3, NAV_H - 6, 0x39E7);
        switch (i) {
        case 0: // 🠕  page-up: arrowhead + stem
            tft.fillTriangle(cx, cy-7, cx-5, cy, cx+5, cy, COL_NAV_FG);
            tft.fillRect(cx-2, cy, 5, 6, COL_NAV_FG);
            break;
        case 1: // 🠗  page-down: stem + arrowhead
            tft.fillRect(cx-2, cy-6, 5, 6, COL_NAV_FG);
            tft.fillTriangle(cx, cy+7, cx-5, cy, cx+5, cy, COL_NAV_FG);
            break;
        case 2: // ⭱  top: double arrowhead up
            tft.fillTriangle(cx, cy-8, cx-5, cy-2, cx+5, cy-2, COL_NAV_FG);
            tft.fillTriangle(cx, cy-1, cx-5, cy+5, cx+5, cy+5, COL_NAV_FG);
            break;
        case 3: // ⭳  bottom: double arrowhead down
            tft.fillTriangle(cx, cy+8, cx-5, cy+2, cx+5, cy+2, COL_NAV_FG);
            tft.fillTriangle(cx, cy+1, cx-5, cy-5, cx+5, cy-5, COL_NAV_FG);
            break;
        }
    }
}

static void drawNavBar(const String &title, bool backBtn, bool scrollBtns = false) {
    tft.fillRect(0, 0, SCREEN_W, NAV_H, COL_NAV_BG);
    tft.setTextColor(COL_NAV_FG, COL_NAV_BG);
    if (backBtn) {
        tft.setTextDatum(ML_DATUM);
        tft.drawString("< BACK", 4, NAV_H / 2, 2);
    }
    // Title: centred in the space left of the scroll buttons (if any)
    int16_t title_right = scrollBtns ? NAV_SCROLL_BTN_X : SCREEN_W;
    int16_t title_cx    = backBtn ? (60 + title_right) / 2 : title_right / 2;
    int16_t max_chars   = (title_right - (backBtn ? 60 : 0)) / FONT2_CHAR_W;
    tft.setTextDatum(MC_DATUM);
    tft.drawString(title.substring(0, max_chars), title_cx, NAV_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    if (scrollBtns) drawNavScrollBtns();
}

static void drawSearchInput(bool blink_tick = false) {
    if (!blink_tick) g_cursor_on = true;   // keypress/redraw resets blink to visible
    tft.fillRect(0, SEARCH_INPUT_Y, SCREEN_W, SEARCH_INPUT_H, COL_INPUT_BG);
    tft.drawRect(0, SEARCH_INPUT_Y, SCREEN_W, SEARCH_INPUT_H, COL_BORDER);
    tft.setTextColor(COL_INPUT_FG, COL_INPUT_BG);
    tft.setTextDatum(ML_DATUM);
    String disp = g_query;
    if (g_cursor_on) disp += '|';
    tft.drawString(disp.substring(0, SEARCH_INPUT_MAX_CHARS), 8, SEARCH_INPUT_Y + SEARCH_INPUT_H / 2, 2);
    tft.setTextDatum(TL_DATUM);
    g_cursor_ms = millis();
}


static void drawResultsList(int16_t y0, int16_t max_h) {
    tft.fillRect(0, y0, SCREEN_W, max_h, COL_BG);
    if (g_results.empty()) {
        if (g_query.length() > 0) {
            tft.setTextColor(COL_HINT, COL_BG);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("No results", SCREEN_W / 2, y0 + max_h / 2, 2);
            tft.setTextDatum(TL_DATUM);
        }
        return;
    }
    int max_vis = max_h / SEARCH_RESULT_H;
    for (int i = 0; i < max_vis; i++) {
        int idx = i + g_results_scroll;
        if (idx >= (int)g_results.size()) break;
        int16_t row_y = y0 + i * SEARCH_RESULT_H;

        // Sentinel divider between prefix and contains results
        if (g_results[idx].article_id == SEARCH_DIVIDER_ID) {
            tft.fillRect(0, row_y, SCREEN_W, SEARCH_RESULT_H - 1, COL_SCROLLBAR);
            tft.setTextColor(COL_BORDER, COL_SCROLLBAR);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("contains:", SCREEN_W / 2, row_y + SEARCH_RESULT_H / 2, 2);
            tft.setTextDatum(TL_DATUM);
            continue;
        }

        uint16_t bg = (i & 1) ? COL_RESULT_ODD : COL_RESULT_EVEN;
        tft.fillRect(0, row_y, SCREEN_W, SEARCH_RESULT_H - 1, bg);
        tft.setTextColor(COL_RESULT_FG, bg);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(utf8ToAscii(g_results[idx].title).substring(0, SEARCH_RESULT_MAX_CHARS),
                       8, row_y + SEARCH_RESULT_H / 2, 2);
        tft.setTextDatum(TL_DATUM);
    }
}

static void drawScrollBar(int32_t scroll, int32_t total_h) {
    int16_t bar_x = SCREEN_W - 6;
    tft.fillRect(bar_x, ART_CONTENT_Y, 6, ART_CONTENT_H, COL_SCROLLBAR);
    if (total_h <= ART_CONTENT_H) return;
    int16_t thumb_h = max(16, (int)(ART_CONTENT_H * ART_CONTENT_H / total_h));
    int16_t thumb_y = ART_CONTENT_Y
        + (int16_t)((int32_t)scroll * (ART_CONTENT_H - thumb_h) / (total_h - ART_CONTENT_H));
    tft.fillRect(bar_x, thumb_y, 6, thumb_h, COL_SCROLLTHUMB);
}

static void showImageFull(uint32_t img_id, int16_t img_w, int16_t img_h) {
    g_full_img_id = img_id;
    g_full_img_w  = img_w;
    g_full_img_h  = img_h;
    g_state       = STATE_IMAGE;

    tft.fillScreen(COL_BG);
    drawNavBar("", true);   // back arrow only, no title

    if (!wikiDbImagesAvailable()) return;

    size_t buf_size;
    uint8_t *buf = wikiDbScratchBuf(&buf_size);

    size_t len = wikiDbLoadImage(img_id, buf, buf_size);
    if (len == 0) return;

    // Full-screen: image is ≤ 320 × 212 from preprocessor; use scale 1 for best quality.
    // Centre within the content area (below nav bar).
    int16_t avail_w = SCREEN_W;
    int16_t avail_h = SCREEN_H - NAV_H;
    int16_t draw_x  = (avail_w > img_w) ? (avail_w - img_w) / 2 : 0;
    int16_t draw_y  = NAV_H + ((avail_h > img_h) ? (avail_h - img_h) / 2 : 0);

    displayDrawImage(draw_x, draw_y, buf, (uint32_t)len, 1, NAV_H, avail_h);
}

// Decode and draw a single image thumbnail at its current scroll position.
// Clips to the article content area (ART_CONTENT_Y … ART_CONTENT_Y + ART_CONTENT_H).
// Only called for images that are at least partially visible.
static void drawImageThumbnail(const ImageRegion &img) {
    if (!wikiDbImagesAvailable()) return;

    // Screen Y of the top of this image's placeholder rect
    int16_t sy = (int16_t)(img.doc_y - g_art_scroll + ART_CONTENT_Y);

    // Skip if entirely off screen
    if (sy + img.thumb_h <= ART_CONTENT_Y) return;
    if (sy >= ART_CONTENT_Y + ART_CONTENT_H) return;

    // Reuse the decompression scratch buffer — it's idle after article load.
    // Avoids heap fragmentation and fits within DRAM limits.
    size_t buf_size;
    uint8_t *buf = wikiDbScratchBuf(&buf_size);

    size_t len = wikiDbLoadImage(img.img_id, buf, buf_size);
    if (len == 0) return;

    // Decode at 0.5×. For QOI, read the actual stored height from the header
    // (SVG equations are not height-capped by the preprocessor and can be tall).
    // Pick qoi_scale so decoded_h = stored_h / qoi_scale <= thumb_h.
    uint8_t qoi_scale = 2;
    bool is_qoi = (len >= 12 && buf[0]=='q' && buf[1]=='o' && buf[2]=='i' && buf[3]=='f');
    if (is_qoi && img.thumb_h > 0) {
        uint32_t stored_h = ((uint32_t)buf[8]<<24)|((uint32_t)buf[9]<<16)|
                            ((uint32_t)buf[10]<<8)|buf[11];
        qoi_scale = 2;
        while (qoi_scale < 8 && stored_h / qoi_scale > (uint32_t)img.thumb_h)
            qoi_scale *= 2;
    }
    uint8_t scale     = is_qoi ? qoi_scale : 2;   // JPEG always scale=2

    // Centre the decoded image horizontally within the article column
    int16_t dec_w = (img.img_w > 0) ? img.img_w / scale : ART_COL_W;
    int16_t draw_x = (ART_COL_W > dec_w) ? (ART_COL_W - dec_w) / 2 : 0;

    // Vertical clip window: intersection of placeholder rect with content area.
    // Draw from sy (top of placeholder) — centering would shift the origin above
    // vp_top and clip the top of the image.
    int16_t vp_top = max((int16_t)ART_CONTENT_Y, sy);
    int16_t vp_bot = min((int16_t)(ART_CONTENT_Y + ART_CONTENT_H),
                         (int16_t)(sy + img.thumb_h));
    if (vp_bot > vp_top) {
        displayDrawImage(draw_x, sy, buf, (uint32_t)len, scale, vp_top, vp_bot - vp_top, ART_COL_W, 2);
    }
}

static void renderArticle() {
    tft.fillRect(0, ART_CONTENT_Y, ART_COL_W, ART_CONTENT_H, COL_BG);
    RenderResult rr = htmlRender(g_art_html, g_art_html_len,
                                  0, ART_CONTENT_Y, ART_CONTENT_H,
                                  g_art_scroll, ART_COL_W);
    g_art_links   = rr.links;
    g_art_images  = rr.images;
    g_art_total_h = rr.content_height;

    // Decode and draw visible image thumbnails (lazy: from SD each scroll redraw).
    for (const auto &img : g_art_images) {
        drawImageThumbnail(img);
    }

    drawScrollBar(g_art_scroll, g_art_total_h);
    // Defence-in-depth: re-assert nav bar in case htmlRender drew above clip_y.
    drawNavBar(g_art_title, true, true);
}

// ---- article loading / pagination --------------------------------------------

// Show a loading spinner.
static void showLoading() {
    drawNavBar("Loading...", true);
    tft.fillRect(0, ART_CONTENT_Y, SCREEN_W, ART_CONTENT_H, COL_BG);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Loading...", SCREEN_W / 2, SCREEN_H / 2, 4);
    tft.setTextDatum(TL_DATUM);
}

// Load g_art_page_num of g_art_id and render it.
// Call after setting g_art_id, g_art_page_num, g_art_pre_paged, g_art_page_starts.
static void loadAndShowPage() {
    // Free the current page buffer BEFORE loading decompression data.
    free(g_art_html_buf);
    g_art_html_buf = nullptr;
    g_art_html     = nullptr;
    g_art_html_len = 0;

    showLoading();

    // For WKI2 (pre-paged): request exactly the block we need.
    // For WIKI (old format): always load block 0 (full article); page slicing done below.
    uint16_t load_block = g_art_pre_paged ? (uint16_t)g_art_page_num : 0;
    ArticleResult ar = wikiDbLoadArticle(g_art_id, load_block);
    if (!ar.found) {
        tft.fillRect(0, ART_CONTENT_Y, SCREEN_W, ART_CONTENT_H, COL_BG);
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.setTextDatum(MC_DATUM);
        tft.drawString("Article not found", SCREEN_W / 2, SCREEN_H / 2, 2);
        tft.setTextDatum(TL_DATUM);
        return;
    }

    // Remember format for subsequent page loads.
    g_art_pre_paged = ar.pre_paged;
    g_art_title     = ar.title;

    // ---- Determine page content range ----------------------------------------
    uint32_t page_start, page_end;
    bool     has_next;

    if (ar.pre_paged) {
        // WKI2: ar.html is already exactly this block's content.
        page_start = 0;
        page_end   = ar.html_len;
        has_next   = ((uint32_t)(g_art_page_num + 1) < ar.num_blocks);
    } else {
        // Old WIKI format: ar.html is the full article; slice at page boundaries.
        uint32_t ps = g_art_page_starts[g_art_page_num];
        if (ps >= ar.html_len) {
            page_end = ar.html_len;
            has_next = false;
        } else if (ps + ART_PAGE_BYTES >= ar.html_len) {
            page_end = ar.html_len;
            has_next = false;
        } else {
            page_end = ps + ART_PAGE_BYTES;
            while (page_end > ps && ar.html[page_end - 1] != '>') page_end--;
            if (page_end <= ps) page_end = ps + ART_PAGE_BYTES;
            has_next = true;
            if ((uint32_t)g_art_page_starts.size() <= g_art_page_num + 1)
                g_art_page_starts.push_back(page_end);
        }
        page_start = ps;
    }
    g_art_has_next = has_next;

    // ---- Build page-HTML buffer with injected nav links --------------------
    // Injected at the very start (page > 0) and very end (has_next) of the HTML.
    char prev_nav[80] = {};
    char next_nav[80] = {};
    size_t prev_len = 0, next_len = 0;

    if (g_art_page_num > 0) {
        snprintf(prev_nav, sizeof(prev_nav),
                 "<a href=\"/wiki/%u\">&lt; Prev page</a><p></p>",
                 ARTICLE_ID_PREV_PAGE);
        prev_len = strlen(prev_nav);
    }
    if (has_next) {
        snprintf(next_nav, sizeof(next_nav),
                 "<p></p><a href=\"/wiki/%u\">Next page &gt;</a>",
                 ARTICLE_ID_NEXT_PAGE);
        next_len = strlen(next_nav);
    }

    size_t content_len = page_end - page_start;
    size_t total       = prev_len + content_len + next_len;

    if (ar.pre_paged) {
        // WKI2: HTML lives in g_decomp_buf[0..content_len-1].
        // g_decomp_buf is ~106 KB; each block is ≤ 60 KB, so there is always
        // room to append/prepend the small nav link strings in-place.
        // This avoids a heap malloc for the page buffer entirely.
        uint8_t *buf = (uint8_t *)ar.html;   // == g_decomp_buf
        // Append next_nav first (before any shift).
        if (next_len) memcpy(buf + content_len, next_nav, next_len);
        // Shift content + next_nav forward, then write prev_nav at the start.
        if (prev_len) {
            memmove(buf + prev_len, buf, content_len + next_len);
            memcpy(buf, prev_nav, prev_len);
        }
        freeArticle(ar);
        free(g_art_html_buf);
        g_art_html_buf = nullptr;            // not a heap allocation — no free needed
        g_art_html     = (char *)buf;
        g_art_html_len = (uint32_t)total;
    } else {
        // Old WIKI format: malloc a buffer for the page slice + nav links.
        uint8_t *page_buf = (uint8_t *)malloc(total);
        if (!page_buf) {
            freeArticle(ar);
            tft.fillRect(0, ART_CONTENT_Y, SCREEN_W, ART_CONTENT_H, COL_BG);
            tft.setTextColor(COL_TEXT, COL_BG);
            tft.setTextDatum(MC_DATUM);
            tft.drawString("Out of memory", SCREEN_W / 2, SCREEN_H / 2, 2);
            tft.setTextDatum(TL_DATUM);
            return;
        }
        // Assemble: [prev nav] + [content slice] + [next nav]
        uint8_t *wp = page_buf;
        if (prev_len)    { memcpy(wp, prev_nav,        prev_len);    wp += prev_len; }
        memcpy(wp, ar.html + page_start, content_len);               wp += content_len;
        if (next_len)    { memcpy(wp, next_nav,        next_len);    wp += next_len; }
        freeArticle(ar);
        free(g_art_html_buf);
        g_art_html_buf = page_buf;
        g_art_html     = (char *)page_buf;
        g_art_html_len = (uint32_t)total;
    }
    g_art_scroll   = 0;
    g_state        = STATE_ARTICLE;

    // Nav bar: show page number only for multi-page articles.
    String nav_title = g_art_title;
    if (g_art_page_num > 0 || has_next) {
        String pg = " p." + String(g_art_page_num + 1);
        nav_title = g_art_title.substring(0, 18) + pg;
    }
    drawNavBar(nav_title, true, true);
    renderArticle();
}

static void goToNextPage() {
    if (!g_art_has_next) return;
    g_art_page_num++;
    loadAndShowPage();
}

static void goToPrevPage() {
    if (g_art_page_num == 0) return;
    g_art_page_num--;
    loadAndShowPage();
}

static void goToArticle(uint32_t article_id, bool push_back) {
    // Intercept page-navigation pseudo-IDs injected into page HTML.
    if (article_id == ARTICLE_ID_NEXT_PAGE) { goToNextPage(); return; }
    if (article_id == ARTICLE_ID_PREV_PAGE) { goToPrevPage(); return; }

    if (push_back && g_art_id != UINT32_MAX) {
        g_history.push_back(g_art_id);
        if (g_history.size() > HISTORY_MAX) g_history.erase(g_history.begin());
    }

    g_art_id        = article_id;
    g_art_page_num  = 0;
    g_art_pre_paged = false;           // reset; format detected on first load
    g_art_page_starts.clear();
    g_art_page_starts.push_back(0);   // page 0 always starts at byte 0 (old format)
    g_art_has_next  = false;

    loadAndShowPage();
}

// ---- search screen -----------------------------------------------------------

static void showSearchScreen() {
    g_state = STATE_SEARCH;
    tft.fillScreen(COL_BG);
    drawNavBar("Wikipedia", false, !g_results.empty());
    if (g_kb_visible) {
        drawSearchInput();
        drawResultsList(SEARCH_RESULTS_Y_KB, SEARCH_RESULTS_H_KB);
        keyboardDraw();
    } else if (g_results.empty()) {
        drawSearchInput();
    } else {
        // Results active — no input bar, results fill the space below nav
        drawResultsList(SEARCH_RESULTS_Y, SEARCH_RESULTS_H);
    }
}

static void handleSearchTap(int16_t tx, int16_t ty) {
    // Tap on scroll buttons in nav bar (only when results are visible)
    if (ty < NAV_H && tx >= NAV_SCROLL_BTN_X && !g_results.empty()) {
        int16_t rh  = g_kb_visible ? SEARCH_RESULTS_H_KB : SEARCH_RESULTS_H;
        int max_vis    = rh / SEARCH_RESULT_H;
        int max_scroll = (int)g_results.size() - max_vis;
        if (max_scroll > 0) {
            int btn = (tx - NAV_SCROLL_BTN_X) / NAV_BTN_W;
            switch (btn) {
            case 0: g_results_scroll -= max_vis; break;   // page up
            case 1: g_results_scroll += max_vis; break;   // page down
            case 2: g_results_scroll = 0;           break; // top
            case 3: g_results_scroll = max_scroll;  break; // bottom
            }
            if (g_results_scroll < 0)           g_results_scroll = 0;
            if (g_results_scroll > max_scroll)  g_results_scroll = max_scroll;
            int16_t ry = g_kb_visible ? SEARCH_RESULTS_Y_KB : SEARCH_RESULTS_Y;
            drawResultsList(ry, rh);
        }
        return;
    }

    // Tap inside keyboard
    if (g_kb_visible && ty >= KB_TOP_Y) {
        // Flash the pressed key before processing (uses pre-tap shift state for correct label).
        // Skip flash for SHIFT/NUM — keyboardDraw() will follow immediately anyway.
        bool wasShift = keyboardGetShift();
        bool wasNum   = keyboardGetNum();
        keyboardFlashKey(tx, ty);
        char ch = keyboardHitTest(tx, ty);
        if (ch == '\r') {
            // GO — hide keyboard + input bar, clear stale results, search, exact-match → navigate, else list
            g_kb_visible = false;
            g_results.clear();
            g_results_scroll = 0;
            // Clear everything below nav bar (keyboard + input + old results)
            tft.fillRect(0, NAV_H, SCREEN_W, SCREEN_H - NAV_H, COL_BG);
            g_results = wikiDbSearch(g_query, SEARCH_MAX_RESULTS);
            // Exact match: navigate straight to the article
            if (!g_results.empty()) {
                String normTitle = g_results[0].title;
                normTitle.trim();
                normTitle.toLowerCase();
                String normQuery = g_query;
                normQuery.trim();
                normQuery.toLowerCase();
                if (normTitle == normQuery) {
                    g_history.clear();
                    goToArticle(g_results[0].article_id, false);
                    return;
                }
            }
            // Append secondary (substring) results below a divider
            {
                auto secondary = wikiDbSearchContains(g_query, SEARCH_MAX_RESULTS);
                if (!secondary.empty()) {
                    SearchResult div; div.article_id = SEARCH_DIVIDER_ID; div.title = "";
                    g_results.push_back(div);
                    for (auto &sr : secondary) g_results.push_back(sr);
                }
            }
            // Results now expand to full height below nav bar (no input bar shown)
            drawNavBar("Wikipedia", false, !g_results.empty());
            drawResultsList(SEARCH_RESULTS_Y, SEARCH_RESULTS_H);
        } else if (ch == '\x01' || ch == '\x02') {
            // SHIFT or NUM toggled — redraw keyboard to show new state
            keyboardDraw();
        } else if (ch == '\x03') {
            // CLR — wipe the entire query
            g_query = "";
            drawSearchInput();
        } else if (ch == '\b') {
            if (g_query.length() > 0) {
                g_query.remove(g_query.length() - 1);
                drawSearchInput();
            }
        } else if (ch != 0) {
            g_query += ch;
            drawSearchInput();
            // Redraw keyboard only if shift or num state changed (e.g. shift auto-cleared)
            if (keyboardGetShift() != wasShift || keyboardGetNum() != wasNum)
                keyboardDraw();
        }
        return;
    }

    // Tap in the input-bar zone: show/restore keyboard.
    // When results are visible (no input bar drawn), the zone is the top of the
    // results area; when idle (no results), the actual input box is drawn there.
    if (ty >= SEARCH_INPUT_Y && ty < SEARCH_INPUT_Y + SEARCH_INPUT_H) {
        if (!g_kb_visible) {
            g_kb_visible = true;
            tft.fillRect(0, NAV_H, SCREEN_W, SCREEN_H - NAV_H, COL_BG);
            drawSearchInput();
            drawResultsList(SEARCH_RESULTS_Y_KB, SEARCH_RESULTS_H_KB);
            keyboardDraw();
        }
        return;
    }

    // Tap on results list
    int16_t ry = g_kb_visible ? SEARCH_RESULTS_Y_KB : SEARCH_RESULTS_Y;
    if (ty >= ry) {
        int idx = g_results_scroll + (ty - ry) / SEARCH_RESULT_H;
        if (idx >= 0 && idx < (int)g_results.size()
            && g_results[idx].article_id != SEARCH_DIVIDER_ID) {
            g_kb_visible = false;
            g_history.clear();
            goToArticle(g_results[idx].article_id, false);
        }
    }
}

static void handleSearchSwipe(int16_t dy) {
    if (g_results.empty()) return;
    int16_t rh = g_kb_visible ? SEARCH_RESULTS_H_KB : SEARCH_RESULTS_H;
    int max_vis    = rh / SEARCH_RESULT_H;
    int max_scroll = (int)g_results.size() - max_vis;
    if (max_scroll <= 0) return;

    // dy < 0 → finger moved up → show later results; dy > 0 → show earlier.
    // Scale by rows but always move at least one row.
    int rows = abs(dy) / SEARCH_RESULT_H;
    if (rows == 0) rows = 1;
    g_results_scroll += (dy < 0) ? rows : -rows;
    if (g_results_scroll < 0)           g_results_scroll = 0;
    if (g_results_scroll > max_scroll)  g_results_scroll = max_scroll;

    int16_t ry = g_kb_visible ? SEARCH_RESULTS_Y_KB : SEARCH_RESULTS_Y;
    drawResultsList(ry, rh);
}

// ---- article screen ----------------------------------------------------------

static void handleArticleTap(int16_t tx, int16_t ty) {
    if (ty < NAV_H) {
        if (tx >= NAV_SCROLL_BTN_X) {
            // Scroll buttons: page-up | page-down | top | bottom
            int btn = (tx - NAV_SCROLL_BTN_X) / NAV_BTN_W;
            int32_t max_scroll = max((int32_t)0, g_art_total_h - ART_CONTENT_H);
            int32_t step = ART_CONTENT_H - PAGE_SCROLL_OVERLAP;
            switch (btn) {
            case 0: g_art_scroll = max((int32_t)0,          g_art_scroll - step); break;
            case 1: g_art_scroll = min(max_scroll, g_art_scroll + step); break;
            case 2: g_art_scroll = 0;           break;
            case 3: g_art_scroll = max_scroll;  break;
            }
            renderArticle();
        } else if (tx < NAV_BACK_BTN_W) {
            // Back button
            if (!g_history.empty()) {
                uint32_t prev = g_history.back();
                g_history.pop_back();
                uint32_t was = g_art_id;
                goToArticle(prev, false);
                g_art_id = was;
                (void)was;
            } else {
                showSearchScreen();
            }
        }
        return;
    }

    // Check image thumbnail taps (before link taps — images are large regions)
    for (const auto &img : g_art_images) {
        int16_t sy = (int16_t)(img.doc_y - g_art_scroll + ART_CONTENT_Y);
        if (ty >= sy && ty < sy + img.thumb_h) {
            g_art_scroll_save = g_art_scroll;
            showImageFull(img.img_id, img.img_w, img.img_h);
            return;
        }
    }

    // Check link taps
    for (const auto &link : g_art_links) {
        if (tx >= link.x && tx < link.x + link.w &&
            ty >= link.y && ty < link.y + link.h) {
            goToArticle(link.article_id, true);
            return;
        }
    }
}

static void handleArticleSwipe(int16_t dy) {
    int32_t max_scroll = g_art_total_h - ART_CONTENT_H;
    if (max_scroll < 0) max_scroll = 0;

    // Natural touch: finger moves UP (dy < 0) → scroll forward (read further).
    // Consistent with handleSearchSwipe which also uses -(dy) for the same reason.
    g_art_scroll -= dy;
    if (g_art_scroll < 0)           g_art_scroll = 0;
    if (g_art_scroll > max_scroll)  g_art_scroll = max_scroll;

    renderArticle();
}

// ---- public API --------------------------------------------------------------

void uiInit() {
    // displayInit() was already called from main.cpp before the boot screen.
    touchInit();

    tft.fillScreen(COL_BG);
    drawNavBar("Wikipedia", false);
    drawSearchInput();

    tft.setTextColor(COL_HINT, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Tap the search box to type", SCREEN_W / 2, SCREEN_H / 2 + 10, 2);
    tft.setTextDatum(TL_DATUM);
}

void uiLoop() {
    // Blink cursor in search input while keyboard is visible.
    if (g_state == STATE_SEARCH && g_kb_visible) {
        uint32_t now = millis();
        if (now - g_cursor_ms >= CURSOR_BLINK_MS) {
            g_cursor_on = !g_cursor_on;
            drawSearchInput(true);
        }
    }

    TouchEvent evt = touchProcess();
    if (evt.type == TouchEvent::NONE) return;

    switch (g_state) {
    case STATE_SEARCH:
        if (evt.type == TouchEvent::TAP)
            handleSearchTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::HOLD) {
            // Auto-repeat DEL only
            if (g_kb_visible && evt.y >= KB_TOP_Y) {
                char ch = keyboardHitTest(evt.x, evt.y);
                if (ch == '\b' && g_query.length() > 0) {
                    g_query.remove(g_query.length() - 1);
                    drawSearchInput();
                }
            }
        } else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleSearchSwipe(evt.dy);
        break;
    case STATE_ARTICLE:
        if (evt.type == TouchEvent::TAP)
            handleArticleTap(evt.x, evt.y);
        else if (evt.type == TouchEvent::SWIPE_UP || evt.type == TouchEvent::SWIPE_DOWN)
            handleArticleSwipe(evt.dy);
        break;
    case STATE_IMAGE:
        // Any tap (including the back arrow in the nav bar) returns to the article.
        if (evt.type == TouchEvent::TAP) {
            g_state      = STATE_ARTICLE;
            g_art_scroll = g_art_scroll_save;
            drawNavBar(g_art_title, true, true);
            renderArticle();
        }
        break;
    }
}
