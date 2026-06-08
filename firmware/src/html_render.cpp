// Written by Alun Morris and Claude Code
#include "html_render.h"
#include "display.h"

// GFX fonts already compiled in via TFT_eSPI/gfxfont.h — just declare them.
extern const GFXfont FreeSansBold9pt7b;
extern const GFXfont FreeSansBold12pt7b;

// Font IDs (TFT_eSPI built-in bitmap fonts)
#define FONT_BODY  2

// GFX bold fonts for headings
static const GFXfont *const GFX_H1 = &FreeSansBold12pt7b;  // yAdvance = 29
static const GFXfont *const GFX_H2 = &FreeSansBold9pt7b;   // yAdvance = 22
// H3 uses body font (FONT_BODY) with pseudo-bold — no GFX font needed

// Line heights
#define LH_BODY  16
#define LH_H1    29
#define LH_H2    22
#define LH_H3    LH_BODY

#define PARA_GAP        6   // extra space after a paragraph
#define LEFT_MARGIN     4
#define RIGHT_MARGIN    LEFT_MARGIN
#define LIST_INDENT     14
#define UNDERLINE_OFFSET  3   // pixels above bottom of line where underline is drawn
#define BULLET_CHAR     "\x95"

// ---- entity decoder ----------------------------------------------------------

static String decodeEntities(const String &in) {
    String out;
    out.reserve(in.length());
    int i = 0, len = (int)in.length();
    while (i < len) {
        if (in[i] != '&') { out += in[i++]; continue; }
        int semi = in.indexOf(';', i + 1);
        if (semi < 0 || semi - i > 9) { out += in[i++]; continue; }
        String e = in.substring(i + 1, semi);
        if      (e == "amp")  { out += '&'; }
        else if (e == "lt")   { out += '<'; }
        else if (e == "gt")   { out += '>'; }
        else if (e == "quot") { out += '"'; }
        else if (e == "apos") { out += '\''; }
        else if (e == "nbsp") { out += ' '; }
        else if (e.startsWith("#")) {
            int cp = e.startsWith("#x") || e.startsWith("#X")
                     ? (int)strtol(e.c_str() + 2, nullptr, 16)
                     : e.substring(1).toInt();
            if (cp >= 32 && cp < 128) out += (char)cp;
            else out += ' ';
        } else {
            out += in[i++]; continue;  // unknown entity — emit '&' literally
        }
        i = semi + 1;
    }
    return out;
}

// ---- renderer class ----------------------------------------------------------

class HtmlRenderer {
public:
    int16_t  x0, clip_y, clip_h, col_width;
    int32_t  scroll_offset;
    RenderResult result;

    // Cursor in content-space (not screen-space)
    int32_t cy = 0;
    int16_t cx;

    // Current style
    uint8_t          font     = FONT_BODY;
    const GFXfont   *gfx_font = nullptr;   // non-null when using a GFX bold font
    uint16_t         color    = COL_TEXT;
    int16_t          line_h   = LH_BODY;
    uint32_t         link_id  = 0;
    bool             in_list        = false;
    bool             bold           = false;
    bool             skip_next_gap  = false;

    // Parse state
    String   text_buf;
    String   tag_buf;
    bool     in_tag = false;

    HtmlRenderer(int16_t x0, int16_t cy, int16_t ch, int32_t scroll, int16_t cw)
        : x0(x0), clip_y(cy), clip_h(ch), scroll_offset(scroll), col_width(cw) {
        cx = x0 + LEFT_MARGIN;
    }

    // Convert content-space Y to screen Y
    int16_t screenY(int32_t csy) const {
        return (int16_t)(csy - scroll_offset + clip_y);
    }

    // True if a line at content Y would be visible on screen
    bool visible(int32_t csy) const {
        int32_t sy = csy - scroll_offset;
        return sy + line_h > 0 && sy < clip_h;
    }

    int16_t leftEdge() const { return x0 + LEFT_MARGIN + (in_list ? LIST_INDENT : 0); }
    int16_t rightEdge() const { return x0 + col_width - RIGHT_MARGIN; }

    void newline() {
        cy += line_h;
        cx = leftEdge();
        if (cy + line_h > result.content_height)
            result.content_height = cy + line_h;
    }

    void blockBreak() {
        if (cx > leftEdge()) newline();
        if (skip_next_gap) skip_next_gap = false;
        else cy += PARA_GAP;
        cx = leftEdge();
        if (cy > result.content_height) result.content_height = cy;
    }

    // Emit text_buf as word-wrapped, styled words
    void flushText() {
        if (text_buf.length() == 0) return;
        String decoded = utf8ToAscii(decodeEntities(text_buf));
        text_buf = "";

        if (gfx_font) tft.setFreeFont(gfx_font);
        else          tft.setTextFont(font);

        // TFT_eSPI skips xAdvance for the last character of a GFX string, so
        // textWidth(" ") == 0 and textWidth(word+" ") == textWidth(word).
        // Measure space via non-final position: textWidth("x x") - textWidth("xx").
        int16_t space_w = tft.textWidth("x x") - tft.textWidth("xx");

        int i = 0, slen = (int)decoded.length();

        // HTML inline whitespace rule: if this text run starts with whitespace
        // and we're mid-line (e.g. right after </a>), emit exactly one space gap
        // then consume all leading whitespace.  Without this, " after a link"
        // loses the leading space and the first word butts up against the link.
        if (i < slen && (decoded[i] == ' ' || decoded[i] == '\n') && cx > leftEdge()) {
            cx += space_w;
            while (i < slen && (decoded[i] == ' ' || decoded[i] == '\n')) i++;
        }

        while (i < slen) {
            // Skip newlines
            while (i < slen && decoded[i] == '\n') i++;
            if (i >= slen) break;

            // Extract next word (non-space run)
            int wstart = i;
            while (i < slen && decoded[i] != ' ' && decoded[i] != '\n') i++;
            if (i == wstart) { i++; continue; }

            String word = decoded.substring(wstart, i);

            // Consume trailing spaces
            bool had_space = false;
            while (i < slen && decoded[i] == ' ') { had_space = true; i++; }

            int16_t ww   = tft.textWidth(word);
            int16_t wwsp = ww + space_w;

            // Wrap if word doesn't fit (but always draw if we're at left edge)
            if (cx + ww > rightEdge() && cx > leftEdge()) {
                newline();
            }

            // Draw if in viewport.
            // Guard sy >= clip_y: visible() allows lines whose top is above clip_y
            // (partial scroll off the top), but screenY() for those lines returns a
            // value < clip_y — i.e. inside the nav bar. Skip drawing those lines.
            if (visible(cy)) {
                int16_t sx = cx;
                int16_t sy = screenY(cy);
                if (sy >= clip_y) {
                    uint16_t drawColor = link_id ? COL_LINK : color;
                    tft.setTextColor(drawColor, COL_BG);
                    if (gfx_font) {
                        tft.drawString(word, sx, sy);
                    } else {
                        tft.drawString(word, sx, sy, font);
                        if (bold) {
                            tft.setTextColor(drawColor);  // transparent bg — don't erase the first pass
                            tft.drawString(word, sx + 1, sy, font);
                        }
                    }

                    if (link_id) {
                        // Underline spans word + trailing space so gaps between
                        // words within a multi-word link are also underlined.
                        int16_t ul  = sy + line_h - UNDERLINE_OFFSET;
                        int16_t ulw = had_space ? wwsp : ww;
                        if (ul >= clip_y && ul < clip_y + clip_h)
                            tft.drawFastHLine(sx, ul, ulw, COL_LINK);
                        result.links.push_back({sx, sy, ulw, line_h, link_id});
                    }
                }
            }

            cx += had_space ? wwsp : ww;
            if (cy + line_h > result.content_height)
                result.content_height = cy + line_h;
        }
    }

    void processTag() {
        String tag = tag_buf;
        tag_buf = "";
        tag.trim();
        if (tag.length() == 0) return;

        bool closing = tag.startsWith("/");
        if (closing) tag = tag.substring(1);

        // Extract tag name (up to first space)
        int sp = tag.indexOf(' ');
        String name = sp < 0 ? tag : tag.substring(0, sp);
        name.toLowerCase();
        String attrs = sp < 0 ? "" : tag.substring(sp + 1);

        if (name == "p") {
            flushText();
            blockBreak();
        }
        else if (name == "br") {
            flushText();
            newline();
        }
        else if (name == "h1") {
            flushText();
            if (!closing) { blockBreak(); gfx_font = GFX_H1; color = COL_H1; line_h = LH_H1; bold = true; }
            else          { if (cx > leftEdge()) newline(); cx = leftEdge();
                            gfx_font = nullptr; font = FONT_BODY; color = COL_TEXT; line_h = LH_BODY; bold = false; }
        }
        else if (name == "h2") {
            flushText();
            if (!closing) { blockBreak(); gfx_font = GFX_H2; color = COL_H2; line_h = LH_H2; bold = true; }
            else          { if (cx > leftEdge()) newline(); cx = leftEdge();
                            gfx_font = nullptr; font = FONT_BODY; color = COL_TEXT; line_h = LH_BODY; bold = false; }
        }
        else if (name == "h3" || name == "h4" || name == "h5" || name == "h6") {
            flushText();
            if (!closing) { blockBreak(); gfx_font = nullptr; font = FONT_BODY; color = COL_H3; line_h = LH_H3; bold = true; }
            else          { if (cx > leftEdge()) newline(); cx = leftEdge();
                            font = FONT_BODY; color = COL_TEXT; line_h = LH_BODY; bold = false;
                            skip_next_gap = true; }
        }
        else if (name == "ul" || name == "ol") {
            flushText();
            blockBreak();
            if (!closing) in_list = true;
            else { in_list = false; blockBreak(); }
        }
        else if (name == "li") {
            flushText();
            blockBreak();
            if (!closing) {
                // Draw bullet point (same clip guard as text)
                if (visible(cy) && screenY(cy) >= clip_y) {
                    tft.setTextColor(color, COL_BG);
                    tft.drawString(BULLET_CHAR, x0 + LEFT_MARGIN, screenY(cy), font);
                }
                cx = x0 + LEFT_MARGIN + LIST_INDENT;
            }
        }
        else if (name == "a" && !closing) {
            flushText();
            // Extract article ID from href="/wiki/NNN"
            int hp = attrs.indexOf("href=\"/wiki/");
            if (hp >= 0) {
                int idstart = hp + 12;
                int idend   = attrs.indexOf('"', idstart);
                if (idend > idstart)
                    link_id = (uint32_t)strtoul(attrs.c_str() + idstart, nullptr, 10);
            }
        }
        else if (name == "a" && closing) {
            flushText();
            link_id = 0;
        }
        else if (name == "img" && !closing) {
            flushText();
            // Parse: src="/img/ID" w="W" h="H"
            int src_pos = attrs.indexOf("src=\"/img/");
            if (src_pos >= 0) {
                int idstart = src_pos + 10;   // skip 'src="/img/'
                int idend   = attrs.indexOf('"', idstart);
                if (idend > idstart) {
                    uint32_t img_id = (uint32_t)attrs.substring(idstart, idend).toInt();

                    // Parse w and h attributes
                    int16_t iw = 0, ih = 0;
                    int wpos = attrs.indexOf("w=\"");
                    if (wpos >= 0) {
                        int wend = attrs.indexOf('"', wpos + 3);
                        if (wend > wpos + 3)
                            iw = (int16_t)attrs.substring(wpos + 3, wend).toInt();
                    }
                    int hpos = attrs.indexOf("h=\"");
                    if (hpos >= 0) {
                        int hend = attrs.indexOf('"', hpos + 3);
                        if (hend > hpos + 3)
                            ih = (int16_t)attrs.substring(hpos + 3, hend).toInt();
                    }

                    // Thumbnail at 0.5×: thumb_h = ih/2 exactly matches the decoded height
                    // (both QOI qoi_scale=2 and JPEG scale=2 produce ih/2 rows).
                    // Preprocessor caps ih≤240, so decoded_h≤120 always.
                    int16_t thumb_h = (ih > 0) ? (int16_t)(ih / 2) : 60;
                    if (thumb_h < 8)   thumb_h = 8;
                    if (thumb_h > 120) thumb_h = 120;

                    // Draw white placeholder rect (only if visible in viewport)
                    if (visible(cy)) {
                        int16_t sy = screenY(cy);
                        if (sy >= clip_y && sy < clip_y + clip_h) {
                            tft.fillRect(x0, sy, col_width, thumb_h, COL_BG);
                        }
                    }

                    // Always record the region so tap detection and lazy decode work
                    // at all scroll positions, not just when first visible.
                    result.images.push_back({img_id, cy, iw, ih, thumb_h});

                    // Advance cursor past image
                    cy += thumb_h + 4;    // 4px gap below image
                    if (cy > result.content_height) result.content_height = cy;
                }
            }
            cx = leftEdge();
        }
        else if (name == "div" || name == "section" || name == "main") {
            // Block containers — ensure we're on a new line on both open and close
            flushText();
            if (cx > leftEdge()) blockBreak();
        }
        // All other tags (span, b, i, em, strong) are ignored — their content flows normally
    }

    RenderResult render(const char *html, uint32_t html_len_arg) {
        result.content_height = 0;
        cx = leftEdge();

        int pos = 0, len = (int)html_len_arg;
        while (pos < len) {
            char c = html[pos++];
            if (c == '<') {
                flushText();
                in_tag = true;
                tag_buf = "";
            } else if (c == '>' && in_tag) {
                processTag();
                in_tag = false;
            } else if (in_tag) {
                tag_buf += c;
            } else {
                text_buf += c;
            }
        }
        flushText();
        if (cx > leftEdge()) cy += line_h;
        if (cy > result.content_height) result.content_height = cy;
        return result;
    }
};

// ---- public entry point ------------------------------------------------------

RenderResult htmlRender(const char *html, uint32_t html_len,
                        int16_t x0, int16_t clip_y, int16_t clip_h,
                        int32_t scroll_offset, int16_t col_width) {
    HtmlRenderer r(x0, clip_y, clip_h, scroll_offset, col_width);
    return r.render(html, html_len);
}
