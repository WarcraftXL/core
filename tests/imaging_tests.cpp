// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The pictures wxl.texture makes, pixel by pixel: the BLP2 writer (header, padding, mips, the
// colour of transparent pixels), the SVG rasterizer (anti-aliasing, a viewBox with a negative
// origin, the groups' colour, stroke width and visibility), the PNG decoder, the resampler and the
// tint. Every expected value is worked out from the geometry, not read off an earlier run.

#include "imaging/Imaging.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace img = wxl::core::imaging;

namespace
{
    int g_failures = 0;
    int g_checks   = 0;

    void Check(bool ok, const char* what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what);
        }
    }

    bool Near(int value, int expected, int tolerance) { return std::abs(value - expected) <= tolerance; }

    struct Px
    {
        int r, g, b, a;
    };

    /// A pixel of a premultiplied picture, straight, 0-255.
    Px At(const img::Picture& p, uint32_t x, uint32_t y)
    {
        const float* s = &p.pixels[(static_cast<size_t>(y) * p.width + x) * 4];
        const float  a = s[3];
        auto         c = [&](float v) { return static_cast<int>(std::lround((a > 0 ? v / a : 0.0f) * 255.0f)); };
        return Px{c(s[0]), c(s[1]), c(s[2]), static_cast<int>(std::lround(a * 255.0f))};
    }

    /// A pixel of a BLP mip, straight, 0-255.
    Px MipAt(const std::vector<uint8_t>& blp, uint32_t level, uint32_t x, uint32_t y, uint32_t* w = nullptr,
             uint32_t* h = nullptr)
    {
        uint32_t       mw = 0, mh = 0;
        const uint8_t* bgra = nullptr;
        if (!img::BlpMip(blp, level, &mw, &mh, &bgra) || x >= mw || y >= mh) return Px{-1, -1, -1, -1};
        if (w) *w = mw;
        if (h) *h = mh;
        const uint8_t* p = bgra + (static_cast<size_t>(y) * mw + x) * 4;
        return Px{p[2], p[1], p[0], p[3]};
    }

    img::Picture Solid(uint32_t w, uint32_t h, float r, float g, float b, float a)
    {
        img::Picture p;
        p.width  = w;
        p.height = h;
        p.pixels.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < p.pixels.size(); i += 4)
        {
            p.pixels[i] = r * a;
            p.pixels[i + 1] = g * a;
            p.pixels[i + 2] = b * a;
            p.pixels[i + 3] = a;
        }
        return p;
    }

    bool Render(const std::string& svg, uint32_t w, uint32_t h, const std::vector<img::GroupStyle>& styles,
                img::Picture& out, std::vector<uint32_t>* matched = nullptr)
    {
        std::string error;
        const bool  ok = img::RenderSvg(reinterpret_cast<const uint8_t*>(svg.data()), svg.size(), w, h, styles.data(),
                                        styles.size(), &out, &error, matched);
        if (!ok) std::printf("  render: %s\n", error.c_str());
        return ok;
    }

    // --- the BLP writer -----------------------------------------------------------------------

    void CheckBlp()
    {
        // 3x2: red, green, blue / white, half-transparent black, transparent.
        img::Picture p = Solid(3, 2, 0, 0, 0, 0);
        const float  px[6][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 0.5f}, {0, 0, 0, 0}};
        for (int i = 0; i < 6; ++i)
            for (int c = 0; c < 4; ++c) p.pixels[i * 4 + c] = c < 3 ? px[i][c] * px[i][3] : px[i][3];

        const std::vector<uint8_t> blp = img::EncodeBlp(p, 8, 8);
        Check(blp.size() == img::kBlpHeaderSize + (64 + 16 + 4 + 1) * 4, "blp: header and four levels 8, 4, 2, 1");
        Check(std::memcmp(blp.data(), "BLP2", 4) == 0, "blp: magic BLP2");
        Check(blp[4] == 1 && blp[5] == 0, "blp: format version 1");
        Check(blp[8] == 3 && blp[9] == 8 && blp[10] == 2 && blp[11] == 1,
              "blp: raw colour, eight bits of alpha, ARGB8888 preferred, mips");
        Check(blp[12] == 8 && blp[16] == 8, "blp: 8 x 8");
        Check(blp[0x14] == static_cast<uint8_t>(img::kBlpHeaderSize & 0xFF) && blp[0x15] == (img::kBlpHeaderSize >> 8),
              "blp: the first mip right after the header");
        Check(blp[0x54] == 0 && blp[0x55] == 1, "blp: the first mip is 256 bytes");
        Check(blp[0x54 + 16] == 0 && blp[0x58 + 16] == 0, "blp: no fifth level");

        uint32_t w = 0, h = 0;
        Px       red = MipAt(blp, 0, 0, 0, &w, &h);
        Check(w == 8 && h == 8, "blp mip 0 is 8 x 8");
        Check(red.r == 255 && red.g == 0 && red.b == 0 && red.a == 255, "blp: red at (0, 0), as BGRA");
        Px green = MipAt(blp, 0, 1, 0);
        Check(green.g == 255 && green.r == 0 && green.a == 255, "blp: green at (1, 0)");
        Px half = MipAt(blp, 0, 1, 1);
        Check(half.a == 128 && half.r == 0, "blp: half-transparent black stays black, alpha 128");
        Px clear = MipAt(blp, 0, 2, 1);
        Check(clear.a == 0, "blp: the transparent pixel is transparent");
        // Its visible neighbours: green (1,0), blue (2,0), black (1,1): their mean colour.
        Check(Near(clear.r, 0, 1) && Near(clear.g, 85, 1) && Near(clear.b, 85, 1),
              "blp: a transparent pixel takes its visible neighbours' colour");
        Px pad = MipAt(blp, 0, 7, 7);
        Check(pad.a == 0, "blp: the padding is transparent");
        Px edge = MipAt(blp, 0, 3, 0);
        Check(edge.a == 0 && Near(edge.b, 255, 1) && edge.r == 0, "blp: padding beside blue takes blue");

        // Mip 1 (4x4), pixel (0,0) = mean of red, green, white, half black, premultiplied:
        // alpha (1+1+1+0.5)/4 = 0.875; red (1+0+1+0)/4 = 0.5 premultiplied -> 0.5/0.875.
        Px m1 = MipAt(blp, 1, 0, 0, &w, &h);
        Check(w == 4 && h == 4, "blp mip 1 is 4 x 4");
        Check(Near(m1.a, 223, 1) && Near(m1.r, 146, 1) && Near(m1.g, 146, 1) && Near(m1.b, 73, 1),
              "blp mip 1: the premultiplied mean of its four pixels");
        Px m3 = MipAt(blp, 3, 0, 0, &w, &h);
        Check(w == 1 && h == 1, "blp mip 3 is 1 x 1");
        // 5.5 opaque-equivalents of alpha over 64 pixels.
        Check(Near(m3.a, static_cast<int>(std::lround(255.0 * 4.5 / 64.0)), 1), "blp mip 3: the mean alpha of the texture");

        // A non-square texture: levels until both edges are 1.
        const std::vector<uint8_t> wide = img::EncodeBlp(Solid(20, 3, 1, 1, 1, 1), 32, 8);
        Check(wide.size() == img::kBlpHeaderSize + (256 + 64 + 16 + 4 + 2 + 1) * 4, "blp: 32 x 8 has six levels");
        MipAt(wide, 5, 0, 0, &w, &h);
        Check(w == 1 && h == 1, "blp: the last level of 32 x 8 is 1 x 1");

        Check(img::TextureEdge(1) == 8 && img::TextureEdge(8) == 8 && img::TextureEdge(9) == 16 &&
                  img::TextureEdge(34) == 64 && img::TextureEdge(2048) == 2048,
              "texture edges: powers of two, at least 8");
    }

    // --- the SVG rasterizer --------------------------------------------------------------------

    void CheckSvg()
    {
        img::Picture p;
        const char*  square = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>"
                              "<rect x='0' y='0' width='8.5' height='16' fill='#ff0000'/></svg>";
        Check(img::Sniff(reinterpret_cast<const uint8_t*>(square), std::strlen(square)) == img::Kind::Svg, "sniff: svg");
        Check(Render(square, 16, 16, {}, p), "svg: renders");
        Px in = At(p, 3, 8), edge = At(p, 8, 8), out = At(p, 12, 8);
        Check(in.r == 255 && in.g == 0 && in.a == 255, "svg: inside the rectangle, opaque red");
        Check(Near(edge.a, 128, 12) && edge.r == 255, "svg: the column the edge halves is half covered, red");
        Check(out.a == 0, "svg: outside, nothing");

        // Twice the size: the same drawing, scaled.
        Check(Render(square, 32, 32, {}, p), "svg: renders at 32");
        Check(At(p, 16, 4).a > 200 && At(p, 17, 4).a < 40, "svg: at 32 px the edge is at 17");

        // A viewBox whose origin is negative: the rectangle from -3 covers the left half.
        const char* shifted = "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10' viewBox='-3 -3 10 10'>"
                              "<rect x='-3' y='-3' width='5' height='10' fill='#00ff00'/></svg>";
        Check(Render(shifted, 10, 10, {}, p), "svg: renders a negative viewBox origin");
        Check(At(p, 0, 0).a == 255 && At(p, 4, 9).g == 255 && At(p, 5, 5).a == 0,
              "svg: a negative viewBox origin is honoured");

        // Kept in proportion: a square drawing in 20 x 10 is 10 x 10 in the middle.
        const char* full = "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'>"
                           "<rect x='0' y='0' width='4' height='4' fill='#0000ff'/></svg>";
        Check(Render(full, 20, 10, {}, p), "svg: renders out of proportion");
        Check(At(p, 4, 5).a == 0 && At(p, 5, 5).a == 255 && At(p, 14, 5).a == 255 && At(p, 15, 5).a == 0,
              "svg: fitted and centred");

        uint32_t    w = 0, h = 0;
        std::string error;
        Check(img::SvgSize(reinterpret_cast<const uint8_t*>(shifted), std::strlen(shifted), &w, &h, &error) && w == 10 &&
                  h == 10,
              "svg: its own size");
        const char* bad = "<svg xmlns='http://www.w3.org/2000/svg'><rect width='1' height='1'</svg>";
        Check(!img::RenderSvg(reinterpret_cast<const uint8_t*>(bad), 3, 4, 4, nullptr, 0, &p, &error) && !error.empty(),
              "svg: text that is no SVG is refused, and says why");
    }

    // A reticle in the owner's shape: an outline group (fill and stroke dark at 0.6, stroke 2)
    // under a fill group (light, no stroke). Circles centred at (10, 10.5) so that row 10's
    // pixel centres sit 3.5, 4.5, 5.5 units from the centre at columns 13, 14, 15.
    const char* kReticle = "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='21' viewBox='0 0 20 21'>"
                           "<g id='outline'><circle cx='10' cy='10.5' r='3' fill='#17120C' stroke='#17120C' "
                           "stroke-width='2' fill-opacity='0.6' stroke-opacity='0.6'/></g>"
                           "<g id='fill'><circle cx='10' cy='10.5' r='3' fill='#F6F6EE'/></g></svg>";

    void CheckGroups()
    {
        img::Picture p;
        Check(Render(kReticle, 20, 21, {}, p), "reticle: renders");
        Px centre = At(p, 10, 10), ring = At(p, 13, 10), beyond = At(p, 14, 10);
        Check(centre.r == 0xF6 && centre.g == 0xF6 && centre.b == 0xEE && centre.a == 255, "reticle: the fill's colour");
        // The pixel spans 3 to 4 across and half a unit up and down, where the circle of 4 bends
        // in: a few hundredths of it are outside the stroke, and the rasterizer samples five rows.
        Check(Near(ring.a, 153, 9) && Near(ring.r, 0x17, 2) && Near(ring.b, 0x0C, 2),
              "reticle: the outline at 3.5 is the stroke's dark at 0.6");
        Check(beyond.a < 20, "reticle: nothing at 4.5 with the authored width 2 (stroke from 2 to 4)");

        std::vector<img::GroupStyle> styles(1);
        styles[0].id       = "outline";
        styles[0].setWidth = true;
        styles[0].width    = 0.0f;
        std::vector<uint32_t> matched;
        Check(Render(kReticle, 20, 21, styles, p, &matched), "reticle: renders with outline width 0");
        Check(matched.size() == 1 && matched[0] == 1, "reticle: the style reached the outline group's one shape");
        Check(At(p, 13, 10).a < 20, "reticle: width 0 takes the outline off at 3.5");
        Check(At(p, 10, 10).a == 255, "reticle: width 0 leaves the fill");

        styles[0].width = 2.0f; // twice the authored width: the stroke from 1 to 5
        Check(Render(kReticle, 20, 21, styles, p), "reticle: renders with the outline twice as wide");
        Check(Near(At(p, 14, 10).a, 153, 9), "reticle: twice the width (4 units) reaches 4.5");
        Check(At(p, 15, 10).a < 20, "reticle: twice the width stops before 5.5");

        // Twice as large a picture: the stroke scales with the drawing.
        Check(Render(kReticle, 40, 42, styles, p), "reticle: renders at twice the size");
        Check(Near(At(p, 28, 21).a, 153, 9) && At(p, 31, 21).a < 20, "reticle: at twice the size, twice the pixels");

        styles[0].setWidth = false;
        styles[0].setColor = true;
        styles[0].color    = 0xCC000000u; // black at 0.8: replaces the colour and the 0.6 opacity
        Check(Render(kReticle, 20, 21, styles, p), "reticle: renders with the outline's colour");
        Px recoloured = At(p, 13, 10);
        Check(Near(recoloured.a, 204, 12) && recoloured.r == 0 && recoloured.g == 0, "reticle: the outline in 000000cc");

        img::GroupStyle fill;
        fill.id       = "fill";
        fill.setColor = true;
        fill.color    = 0xFFFF4040u;
        styles.push_back(fill);
        Check(Render(kReticle, 20, 21, styles, p), "reticle: renders with both groups' colours");
        Px red = At(p, 10, 10);
        Check(red.r == 255 && red.g == 0x40 && red.b == 0x40 && red.a == 255, "reticle: the fill in ff4040");

        styles.resize(1);
        styles[0].setColor   = false;
        styles[0].setVisible = true;
        styles[0].visible    = false;
        Check(Render(kReticle, 20, 21, styles, p), "reticle: renders with the outline hidden");
        Check(At(p, 13, 10).a == 0, "reticle: a hidden group draws nothing");

        // Two strokes of a group drawn 2 and 4 wide: twice as wide each, so 4 and 8.
        const char* lines = "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='20'><g id='g' stroke='#000'>"
                            "<path d='M0 5 H20' stroke-width='2'/><path d='M0 15 H20' stroke-width='4'/></g></svg>";
        std::vector<img::GroupStyle> wider(1);
        wider[0].id       = "g";
        wider[0].setWidth = true;
        wider[0].width    = 2.0f;
        Check(Render(lines, 20, 20, {}, p) && At(p, 10, 3).a == 0 && At(p, 10, 12).a == 0, "lines: as drawn, 4..6 and 13..17");
        Check(Render(lines, 20, 20, wider, p) && At(p, 10, 3).a == 255 && At(p, 10, 12).a == 255 && At(p, 10, 2).a == 0 &&
                  At(p, 10, 10).a == 0,
              "lines: twice as wide each keeps their proportion (3..7 and 11..19)");

        styles[0].id = "nothing-by-that-id";
        Check(Render(kReticle, 20, 21, styles, p, &matched) && matched[0] == 0, "reticle: an id no shape has reaches none");
    }

    // --- PNG, the resampler, the tint ---------------------------------------------------------

    // 2x2 RGBA: red, green / blue, transparent white.
    const uint8_t kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x13, 0x49,
        0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x81, 0x34, 0x08, 0x30, 0x00, 0x00, 0x48,
        0xc9, 0x08, 0xf8, 0x71, 0xc5, 0x31, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

    void CheckPng()
    {
        Check(img::Sniff(kPng, sizeof kPng) == img::Kind::Png, "sniff: png");
        img::Picture p;
        std::string  error;
        Check(img::DecodePng(kPng, sizeof kPng, &p, &error), "png: decodes");
        Check(p.width == 2 && p.height == 2, "png: 2 x 2");
        Px r = At(p, 0, 0), g = At(p, 1, 0), b = At(p, 0, 1), t = At(p, 1, 1);
        Check(r.r == 255 && r.a == 255 && g.g == 255 && b.b == 255 && t.a == 0, "png: its four pixels");
        Check(p.pixels[(1 * 2 + 1) * 4] == 0.0f, "png: a transparent pixel is premultiplied to nothing");

        const uint8_t junk[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
        Check(!img::DecodePng(junk, sizeof junk, &p, &error) && !error.empty(), "png: a broken file is refused, and says why");

        // A checker of black and white shrunk by four: grey, every pixel.
        img::Picture checker = Solid(16, 16, 0, 0, 0, 1);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x)
                if ((x + y) & 1)
                    for (int c = 0; c < 3; ++c) checker.pixels[(y * 16 + x) * 4 + c] = 1.0f;
        img::Picture small = img::Resample(checker, 4, 4);
        bool         grey  = true;
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t x = 0; x < 4; ++x)
            {
                Px q = At(small, x, y);
                grey = grey && Near(q.r, 128, 6) && q.a == 255;
            }
        Check(small.width == 4 && small.height == 4 && grey, "resample: a checker shrunk by four is grey");

        // An opaque square on transparency shrunk: no dark fringe, the colour stays the square's.
        img::Picture island = Solid(16, 16, 0, 0, 0, 0);
        for (uint32_t y = 4; y < 12; ++y)
            for (uint32_t x = 4; x < 12; ++x)
            {
                float* s = &island.pixels[(y * 16 + x) * 4];
                s[0] = 1.0f;
                s[1] = 0.5f;
                s[3] = 1.0f;
            }
        img::Picture shrunk = img::Resample(island, 5, 5);
        bool         clean  = true;
        for (uint32_t y = 0; y < 5; ++y)
            for (uint32_t x = 0; x < 5; ++x)
            {
                Px q = At(shrunk, x, y);
                if (q.a > 8) clean = clean && Near(q.r, 255, 3) && Near(q.g, 128, 10) && q.b <= 3;
            }
        Check(clean, "resample: edges keep the colour (premultiplied), no dark fringe");

        img::Picture same = img::Resample(p, 2, 2);
        Check(same.pixels == p.pixels, "resample: the same size is the same picture");

        img::Picture tinted = Solid(1, 1, 1, 1, 1, 1);
        img::Tint(tinted, 0x80FF8000u);
        Px q = At(tinted, 0, 0);
        Check(q.r == 255 && Near(q.g, 128, 1) && q.b == 0 && Near(q.a, 128, 1), "tint: white times 80ff8000");
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CheckBlp();
    CheckSvg();
    CheckGroups();
    CheckPng();
    std::printf("imaging: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
