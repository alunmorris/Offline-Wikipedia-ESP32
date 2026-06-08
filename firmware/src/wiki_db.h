// Written by Alun Morris and Claude Code
#pragma once
#include <Arduino.h>
#include <vector>

struct ArticleResult {
    bool     found      = false;
    uint32_t article_id = 0;
    String   title;

    // html points into wiki_db's persistent decompression buffer (g_decomp_buf).
    // raw_buf is always nullptr — freeArticle() is a no-op.
    // IMPORTANT: copy html content before the next wikiDbLoadArticle() call,
    // which overwrites g_decomp_buf.
    uint8_t *raw_buf   = nullptr;  // always nullptr (persistent buf, not heap-owned)
    char    *html      = nullptr;  // points into g_decomp_buf
    uint32_t html_len  = 0;

    // Pagination (WKI2 multi-block format only).
    // pre_paged = true  → html is already sliced to exactly this block (block_num).
    //             false → html is the full article; caller must do byte-range slicing.
    uint16_t num_blocks = 1;    // total blocks (1 = single-page article or old WIKI format)
    uint16_t block_num  = 0;    // which block was loaded
    bool     pre_paged  = false;// true when using WKI2 per-block decompression
};

// freeArticle() is a no-op (html points into a persistent buffer owned by wiki_db).
inline void freeArticle(ArticleResult &ar) {
    ar.raw_buf  = nullptr;
    ar.html     = nullptr;
    ar.html_len = 0;
}

struct SearchResult {
    uint32_t article_id;
    String   title;
};

/*
 * Allocate the persistent decompression buffer on the cleanest possible heap.
 * Call this as the very first thing in setup(), before display init, SD init,
 * or any other heap-consuming initialisation.  wikiDbInit() will skip the
 * allocation if this was already called.
 */
void wikiDbPreInit();

/*
 * Open the SD card, read index metadata, and load sparse_index.bin into RAM.
 * Returns true on success, false if the SD card or required files are missing.
 */
bool wikiDbInit();

/* Return the total number of articles reported by index_meta.txt. */
uint32_t wikiDbArticleCount();

/* Return the human-readable database name from index_meta.txt (e.g. "English Simple All Maxi May 2026"). */
const String &wikiDbName();

/*
 * Binary-search index.bin for an exact title_key match.
 * title_key must already be normalised (lowercase, trimmed).
 * Returns article_id, or UINT32_MAX if not found.
 */
uint32_t wikiDbFindByTitle(const String &title_key);

/*
 * Load the HTML for one block (page) of an article.
 *
 * For WKI2 format (new): decompresses exactly one 60 KB block; ar.pre_paged = true.
 * For WIKI format (old): decompresses the full article; ar.pre_paged = false.
 *
 * block_num is 0-indexed.  Pass 0 to load the first (or only) page.
 * Returns ArticleResult with found=false on any error.
 */
ArticleResult wikiDbLoadArticle(uint32_t article_id, uint16_t block_num = 0);

/*
 * Prefix search: finds up to maxResults articles whose title_key starts with
 * prefix (already normalised). Returns matches in index order.
 */
std::vector<SearchResult> wikiDbSearch(const String &prefix, int maxResults = 10);

/*
 * Substring search using the in-RAM sparse index as a guide.
 * Returns up to maxResults articles whose title_key CONTAINS query but does
 * NOT start with it (prefix matches come from wikiDbSearch).
 */
std::vector<SearchResult> wikiDbSearchContains(const String &query, int maxResults = 10);

/*
 * Returns true if image data files were found and opened at init.
 * When false, wikiDbLoadImage() always returns 0.
 */
bool wikiDbImagesAvailable();

/*
 * Read the image bytes for img_id into caller-supplied buf (max buf_size bytes).
 * Returns the number of bytes written, or 0 on any error (not found, buf too small, etc.).
 * The returned bytes are a complete JPEG or PNG image (detect format from magic bytes).
 */
size_t wikiDbLoadImage(uint32_t img_id, uint8_t *buf, size_t buf_size);

/*
 * Return a pointer to the internal image scratch buffer (IMG_BUF_SIZE bytes).
 * Safe to call at any time — it is a dedicated static buffer separate from
 * article decompression.
 */
uint8_t *wikiDbScratchBuf(size_t *out_size);
