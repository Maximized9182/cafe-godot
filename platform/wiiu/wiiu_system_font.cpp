// The Wii U's own system font (CafeStd, from OSGetSharedData) for the crash
// overlay, drawn with GX2 alone.
//
// The glyphs (ASCII 32-126 plus the Wii U button icons listed in EXTRA,
// which CafeStd has in its private-use area) are rasterised once with FreeType at GX2 start-up
// (wiiu_sysfont_init) into a texture atlas, so drawing after a crash needs
// neither FreeType nor Godot's text server. Builds without the freetype
// module, or a console without the shared font, fall back to the built-in
// 10x20 bitmap font.

#include "wiiu_gx2.h"

#include "modules/modules_enabled.gen.h"

#include <coreinit/memory.h>
#include <stdlib.h>
#include <string.h>
#include <whb/log.h>

#ifdef MODULE_FREETYPE_ENABLED
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace {

constexpr int FIRST = 32;
constexpr int ASCII_COUNT = 95; // ASCII 32-126
// Button icons in CafeStd's private-use area (checked against the font):
// A B X Y, HOME (house in a circle), D-pad right, L, ZL, TV (GamePad style).
constexpr uint32_t EXTRA[] = { 0xE000, 0xE001, 0xE002, 0xE003, 0xE044, 0xE07C, 0xE083, 0xE085, 0xE089 };
constexpr int EXTRA_COUNT = sizeof(EXTRA) / sizeof(EXTRA[0]);
constexpr int COUNT = ASCII_COUNT + EXTRA_COUNT;
constexpr float BAKE_PX = 32.0f; // Pixel size of the atlas glyphs.
constexpr uint32_t ATLAS_W = 1024;
constexpr uint32_t ATLAS_H = 256;

struct Glyph {
	float u0, v0, u1, v1; // Atlas coordinates.
	float w, h; // Bitmap size in pixels at BAKE_PX.
	float left, top; // Bitmap offset from the pen position / baseline.
	float advance;
};

bool baked = false;
WiiUTexture *atlas = nullptr;
Glyph glyphs[COUNT];
float ascender = 0.0f; // At BAKE_PX.
float line_height = 0.0f; // At BAKE_PX.

} // namespace

extern "C" bool wiiu_sysfont_init(void) {
#ifdef MODULE_FREETYPE_ENABLED
	if (baked) {
		return true;
	}
	void *font_data = nullptr;
	uint32_t font_size = 0;
	if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font_data, &font_size) || font_data == nullptr) {
		WHBLogPrint("[font] system font not available; using the bitmap font");
		return false;
	}
	FT_Library library;
	if (FT_Init_FreeType(&library) != 0) {
		return false;
	}
	FT_Face face;
	if (FT_New_Memory_Face(library, (const FT_Byte *)font_data, (FT_Long)font_size, 0, &face) != 0) {
		FT_Done_FreeType(library);
		return false;
	}
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)BAKE_PX);
	ascender = (float)(face->size->metrics.ascender >> 6);
	line_height = (float)(face->size->metrics.height >> 6);

	uint8_t *pixels = (uint8_t *)calloc(ATLAS_W * ATLAS_H, 4);
	if (pixels == nullptr) {
		FT_Done_Face(face);
		FT_Done_FreeType(library);
		return false;
	}
	uint32_t pen_x = 1, pen_y = 1, row_h = 0;
	for (int i = 0; i < COUNT; i++) {
		Glyph &g = glyphs[i];
		memset(&g, 0, sizeof(g));
		const FT_ULong code = i < ASCII_COUNT ? (FT_ULong)(FIRST + i) : (FT_ULong)EXTRA[i - ASCII_COUNT];
		if (FT_Load_Char(face, code, FT_LOAD_RENDER) != 0) {
			continue;
		}
		const FT_GlyphSlot slot = face->glyph;
		const FT_Bitmap &bm = slot->bitmap;
		g.advance = (float)(slot->advance.x >> 6);
		g.left = (float)slot->bitmap_left;
		g.top = (float)slot->bitmap_top;
		g.w = (float)bm.width;
		g.h = (float)bm.rows;
		if (bm.width == 0 || bm.rows == 0) {
			continue;
		}
		if (pen_x + bm.width + 1 > ATLAS_W) {
			pen_x = 1;
			pen_y += row_h + 1;
			row_h = 0;
		}
		if (pen_y + bm.rows + 1 > ATLAS_H) {
			break; // Atlas full (shouldn't happen for ASCII at this size).
		}
		for (uint32_t y = 0; y < bm.rows; y++) {
			for (uint32_t x = 0; x < bm.width; x++) {
				uint8_t *px = pixels + ((pen_y + y) * ATLAS_W + pen_x + x) * 4;
				px[0] = px[1] = px[2] = 255;
				px[3] = bm.buffer[y * bm.pitch + x];
			}
		}
		g.u0 = (float)pen_x / ATLAS_W;
		g.v0 = (float)pen_y / ATLAS_H;
		g.u1 = (float)(pen_x + bm.width) / ATLAS_W;
		g.v1 = (float)(pen_y + bm.rows) / ATLAS_H;
		pen_x += bm.width + 1;
		row_h = bm.rows > row_h ? bm.rows : row_h;
	}
	atlas = wiiu_gx2_texture_create(ATLAS_W, ATLAS_H, pixels);
	free(pixels);
	FT_Done_Face(face);
	FT_Done_FreeType(library);
	baked = atlas != nullptr;
	return baked;
#else
	return false;
#endif
}

extern "C" void wiiu_sysfont_free(void) {
	if (atlas) {
		wiiu_gx2_texture_free(atlas);
		atlas = nullptr;
	}
	baked = false;
}

static const Glyph *glyph_for(uint32_t p_code) {
	if (p_code >= (uint32_t)FIRST && p_code < (uint32_t)(FIRST + ASCII_COUNT)) {
		return &glyphs[p_code - FIRST];
	}
	for (int i = 0; i < EXTRA_COUNT; i++) {
		if (EXTRA[i] == p_code) {
			return &glyphs[ASCII_COUNT + i];
		}
	}
	return &glyphs['?' - FIRST];
}

// Decodes one UTF-8 character and advances *p_text past it.
static uint32_t next_codepoint(const char **p_text) {
	const unsigned char *s = (const unsigned char *)*p_text;
	uint32_t c = s[0];
	int len = 1;
	if (c >= 0xF0 && s[1] && s[2] && s[3]) {
		c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
		len = 4;
	} else if (c >= 0xE0 && s[1] && s[2]) {
		c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
		len = 3;
	} else if (c >= 0xC0 && s[1]) {
		c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
		len = 2;
	}
	*p_text += len;
	return c;
}

// Width of p_text at p_px pixels.
extern "C" float wiiu_sysfont_measure(const char *p_text, float p_px) {
	if (!baked) {
		return (float)strlen(p_text) * WIIU_FONT_CELL_W;
	}
	const float scale = p_px / BAKE_PX;
	float w = 0.0f;
	for (const char *c = p_text; *c;) {
		w += glyph_for(next_codepoint(&c))->advance * scale;
	}
	return w;
}

// Distance between lines at p_px pixels.
extern "C" float wiiu_sysfont_line_height(float p_px) {
	return baked ? line_height * (p_px / BAKE_PX) : (float)WIIU_FONT_CELL_H;
}

// Draws one line (UTF-8) with its top-left at (p_x, p_y), p_px pixels high.
extern "C" void wiiu_sysfont_draw(float p_x, float p_y, float p_px, const char *p_text, float p_r, float p_g, float p_b, float p_a) {
	if (!baked) {
		wiiu_gx2_draw_text(p_x, p_y, p_text, p_r, p_g, p_b, p_a);
		return;
	}
	const float scale = p_px / BAKE_PX;
	const float baseline = p_y + ascender * scale;
	WiiUVertex v[6 * 96];
	uint32_t n = 0;
	float pen = p_x;
	for (const char *c = p_text; *c && n + 6 <= 6 * 96;) {
		const Glyph *g = glyph_for(next_codepoint(&c));
		if (g->w > 0 && g->h > 0) {
			const float x0 = pen + g->left * scale;
			const float y0 = baseline - g->top * scale;
			const float x1 = x0 + g->w * scale;
			const float y1 = y0 + g->h * scale;
			const float xs[6] = { x0, x1, x1, x0, x1, x0 };
			const float ys[6] = { y0, y0, y1, y0, y1, y1 };
			const float us[6] = { g->u0, g->u1, g->u1, g->u0, g->u1, g->u0 };
			const float vs[6] = { g->v0, g->v0, g->v1, g->v0, g->v1, g->v1 };
			for (int k = 0; k < 6; k++) {
				WiiUVertex &vert = v[n + k];
				vert.x = xs[k];
				vert.y = ys[k];
				vert.z = 0.0f;
				vert.u = us[k];
				vert.v = vs[k];
				vert.r = p_r;
				vert.g = p_g;
				vert.b = p_b;
				vert.a = p_a;
			}
			n += 6;
		}
		pen += g->advance * scale;
	}
	wiiu_gx2_draw(nullptr, WIIU_PRIM_TRIANGLES, atlas, WIIU_FILTER_LINEAR, false, v, n);
}
