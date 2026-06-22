#include "PCH.h"
#include "D3DOverlay.h"
#include "ProgressTracker.h"
#include "config/Settings.h"

#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <cmath>
#include <thread>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <MinHook.h>
#ifdef max
#  undef max
#endif
#ifdef min
#  undef min
#endif

static constexpr float kPI = 3.14159265358979323846f;

// Global plugin state

static bool          g_initialized   = false;   // true after ImGui is set up
static bool          g_presentHooked = false;   // true after we hook the swap chain
static ImGuiContext* g_ctx           = nullptr; // our own ImGui context (separate from game's)
static HWND          g_hwnd          = nullptr; // game window handle, used for mouse input
static UINT          g_width         = 1280;
static UINT          g_height        = 720;

static bool          g_cfgOpen          = false; // is the settings menu open?
static bool          g_prevMenuKey      = false; // previous state of the toggle key
static bool          g_capturingMenuKey = false; // waiting for user to press a new key

// Function pointer types for the two D3D functions we hook.
// We save the originals so we can call them after our code runs.
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
static PresentFn orig_Present = nullptr;

using CreateDevFn = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT,
    const DXGI_SWAP_CHAIN_DESC*,
    IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static CreateDevFn orig_CreateDev = nullptr;

// Animation state - all of these reset when a new load starts

static float s_display  = 0.0f;  // the percentage shown on screen (0-100)
static int   s_tick     = 0;     // frame counter, used to drive time-based animation
static bool  s_loading  = false; // true while a loading screen is active
static bool  s_draining = false; // true when load is done but bar hasn't reached 100% yet
static LoadPhase s_prevPhase = LoadPhase::Idle; // tracks previous load phase to detect transitions
static std::chrono::steady_clock::time_point s_start;

static int       s_loadIndex    = 0;     // counts total loads this session
static int       s_activeStyle  = 0;     // animation style for the current load (random mode can change this)
static int       s_constPattern = 0;     // which star connection pattern to use this load
static int       s_wallSeed     = 0;     // controls which rune glyphs appear on the word wall this load
static int       s_crackSeed    = 0;     // controls crack positions on the standing stone this load
static int       s_mountainSeed = 0;   // random mountain profile per load

//
// DRAWING HELPERS
//

static constexpr float DEG = kPI / 180.0f;

// g_animAlpha multiplies all animation colours so the user can set opacity
static float g_animAlpha = 1.0f;

static ImU32 Col(uint32_t rgb, float a = 1.0f) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                    static_cast<int>(a * g_animAlpha * 255));
}

// Returns the locale-aware display name for a Windows virtual key code.
static std::string VKName(int vk) {
    UINT sc = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (sc == 0) return "?";
    wchar_t buf[64]{};
    GetKeyNameTextW(static_cast<LONG>(sc << 16), buf, 64);
    if (buf[0] == 0) return "?";
    std::string out;
    for (wchar_t c : buf) { if (!c) break; out += static_cast<char>(c); }
    return out;
}

static void FilledArc(ImDrawList* dl, ImVec2 c, float r,
                      float startDeg, float endDeg, ImU32 col) {
    if (endDeg <= startDeg) return;
    int segs = (std::max)(6, static_cast<int>((endDeg - startDeg) / 3.0f));
    dl->PathLineTo(c);
    dl->PathArcTo(c, r, startDeg * DEG, endDeg * DEG, segs);
    dl->PathFillConvex(col);
}

static void Arc(ImDrawList* dl, ImVec2 c, float r,
                float startDeg, float endDeg, ImU32 col, float thick = 2.0f) {
    if (endDeg <= startDeg) return;
    int segs = (std::max)(6, static_cast<int>((endDeg - startDeg) / 3.0f));
    dl->PathArcTo(c, r, startDeg * DEG, endDeg * DEG, segs);
    dl->PathStroke(col, 0, thick);
}

static void RoundRect(ImDrawList* dl, ImVec2 tl, ImVec2 br,
                      ImU32 fill, ImU32 stroke, float rounding, float thick = 1.0f) {
    dl->AddRectFilled(tl, br, fill, rounding);
    dl->AddRect(tl, br, stroke, rounding, 0, thick);
}

static float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Gear helper — used by Anim_DwemerCogs
static void DrawGear(ImDrawList* dl, ImVec2 pos, float r, float rot,
                     int teeth, uint32_t col, float alpha) {
    float toothH  = r * 0.24f;
    float halfAng = kPI / float(teeth) * 0.38f;
    for (int i = 0; i < teeth; i++) {
        float a  = rot + (i / float(teeth)) * 2.0f * kPI;
        float a1 = a - halfAng, a2 = a + halfAng;
        ImVec2 tp[4] = {
            { pos.x + cosf(a1)*r,          pos.y + sinf(a1)*r          },
            { pos.x + cosf(a1)*(r+toothH), pos.y + sinf(a1)*(r+toothH) },
            { pos.x + cosf(a2)*(r+toothH), pos.y + sinf(a2)*(r+toothH) },
            { pos.x + cosf(a2)*r,          pos.y + sinf(a2)*r          }
        };
        dl->AddConvexPolyFilled(tp, 4, Col(col, alpha * 0.55f));
        dl->AddPolyline(tp, 4, Col(col, alpha * 0.90f), ImDrawFlags_Closed, 1.2f);
    }
    dl->AddCircleFilled(pos, r * 0.68f, Col(col, alpha * 0.25f), 32);
    dl->AddCircle(pos,      r * 0.68f, Col(col, alpha * 0.80f), 32, 1.5f);
    for (int i = 0; i < 3; i++) {
        float sa = rot + (i / 3.0f) * 2.0f * kPI;
        ImVec2 sp = { pos.x + cosf(sa)*r*0.40f, pos.y + sinf(sa)*r*0.40f };
        dl->AddCircle(sp, r * 0.09f, Col(col, alpha * 0.55f), 6, 1.0f);
    }
    dl->AddCircleFilled(pos, r * 0.12f, Col(col, alpha), 8);
}

// Hexagon helper — used by Anim_DragonScales
static void DrawHexAt(ImDrawList* dl, ImVec2 center, float size,
                      uint32_t col, float alpha) {
    ImVec2 pts[6];
    for (int i = 0; i < 6; i++) {
        float a  = (i / 6.0f) * 2.0f * kPI + kPI / 6.0f;
        pts[i]   = { center.x + cosf(a)*size, center.y + sinf(a)*size };
    }
    dl->AddConvexPolyFilled(pts, 6, Col(col, alpha * 0.50f));
    dl->AddPolyline(pts, 6, Col(col, alpha), ImDrawFlags_Closed, 1.5f);
}

//
// ANIMATION 0 — CIRCLE FILL
//
static void Anim_CircleFill(ImDrawList* dl, ImVec2 c, float sc,
                             float p, float t, uint32_t col) {
    float glow   = 0.5f + 0.5f * sinf(t * 0.08f);
    float filled = p / 100.0f;

    dl->AddCircle(c, 92*sc, Col(col, 0.35f), 72, 1.0f);
    for (int i = 0; i < 12; i++) {
        float a = i / 12.0f * 360.0f * DEG;
        dl->AddLine({ c.x + cosf(a)*86*sc, c.y + sinf(a)*86*sc },
                    { c.x + cosf(a)*92*sc, c.y + sinf(a)*92*sc },
                    Col(col, 0.35f), 1.5f);
    }

    if (filled > 0.001f) {
        FilledArc(dl, c, 74*sc, -90.0f, -90.0f + 360.0f * filled, Col(col, 0.18f));
        Arc(dl, c, 74*sc, -90.0f, -90.0f + 360.0f * filled,
            Col(col, 0.8f + glow * 0.2f), 4.0f);
    }
    if (filled < 0.999f)
        Arc(dl, c, 74*sc, -90.0f + 360.0f * filled, 270.0f, Col(0x333355, 0.55f), 2.0f);

    float ir = (18.0f + 3.0f * glow) * sc;
    dl->AddCircleFilled(c, ir, Col(col, 0.30f + 0.20f * glow), 32);

    if (filled > 0.001f) {
        float ta = (-90.0f + 360.0f * filled) * DEG;
        dl->AddLine({ c.x + cosf(ta)*65*sc, c.y + sinf(ta)*65*sc },
                    { c.x + cosf(ta)*85*sc, c.y + sinf(ta)*85*sc },
                    IM_COL32(255,255,255,210), 3.0f);
    }
}

//
// ANIMATION 1 — DRAGON EYE
//
static void Anim_DragonEye(ImDrawList* dl, ImVec2 c, float sc,
                            float p, float t, uint32_t col) {
    float f   = p / 100.0f;
    float eye = f * 80.0f * sc;
    float ww  = 100.0f * sc;

    dl->PathLineTo({ c.x - ww, c.y });
    for (int i = 0; i <= 32; i++) {
        float frac = i / 32.0f;
        dl->PathLineTo({ c.x + (frac*2.0f-1.0f)*ww, c.y - eye*sinf(frac*kPI) });
    }
    dl->PathLineTo({ c.x + ww, c.y });
    dl->PathStroke(Col(col, 0.90f), 0, 3.0f);

    dl->PathLineTo({ c.x - ww, c.y });
    for (int i = 0; i <= 32; i++) {
        float frac = i / 32.0f;
        dl->PathLineTo({ c.x + (frac*2.0f-1.0f)*ww, c.y + eye*sinf(frac*kPI) });
    }
    dl->PathLineTo({ c.x + ww, c.y });
    dl->PathStroke(Col(col, 0.90f), 0, 3.0f);

    if (eye > 2.0f) {
        float pw = 12.0f * sc;
        dl->AddEllipseFilled(c, ImVec2{ pw, eye * 0.85f }, Col(col, 0.70f), 0.0f, 32);
        dl->AddEllipse(c, ImVec2{ pw, eye * 0.85f }, Col(col, 1.0f), 0.0f, 32, 1.5f);
    }

    int   wc    = static_cast<int>(f * 6);
    float pulse = 0.10f + 0.06f * sinf(t * 0.05f);
    for (int i = 0; i < wc; i++)
        dl->AddCircle(c, (i+1)*16.0f*sc, Col(col, pulse), 48, 1.0f);
}

//
// ANIMATION 2 — NORDIC RUNES
//
static void Anim_NordicRunes(ImDrawList* dl, ImVec2 c, float sc,
                              float p, float t, uint32_t col) {
    struct Ring { int n; float r; float spd; float sz; };
    Ring rings[] = { {8,40,0.6f,5},{12,65,-0.35f,4},{16,88,0.2f,3} };
    int litCount = static_cast<int>(p / 100.0f * 36);
    int idx      = 0;
    for (auto& ring : rings) {
        float off = t * ring.spd * DEG;
        for (int i = 0; i < ring.n; i++) {
            float a  = (i / static_cast<float>(ring.n)) * 360.0f * DEG + off;
            ImVec2 dp { c.x + cosf(a)*ring.r*sc, c.y + sinf(a)*ring.r*sc };
            bool lit = (idx < litCount);
            dl->AddCircleFilled(dp, ring.sz*sc, Col(col, lit ? 0.90f : 0.18f), 12);
            idx++;
        }
    }
    for (int i = 0; i < 4; i++) {
        float a   = i * 90.0f * DEG;
        float cx2 = cosf(a)*100*sc, cy2 = sinf(a)*100*sc;
        float px2 = cosf(a+kPI/2)*6*sc, py2 = sinf(a+kPI/2)*6*sc;
        dl->AddLine({c.x+cx2-px2,c.y+cy2-py2},{c.x+cx2+px2,c.y+cy2+py2},Col(col,0.30f),1.5f);
        dl->AddLine({c.x+cx2,c.y+cy2-10*sc},{c.x+cx2,c.y+cy2+10*sc},Col(col,0.30f),1.5f);
    }
    dl->AddCircle(c, 14*sc, Col(col, 0.50f), 24, 2.0f);
    dl->AddCircleFilled(c, 10*sc, Col(col, 0.40f), 20);
}

//
// ANIMATION 3 — WAVEFORM PULSE
//
static void Anim_Waveform(ImDrawList* dl, ImVec2 c, float sc,
                           float p, float t, uint32_t col) {
    float f  = p / 100.0f;
    int   n  = 24;
    float bw = 8*sc, sp = 13*sc;
    float tw = n * sp;
    float x0 = c.x - tw/2 + sp/2;

    float rr = Lerp(0x40/255.0f, ((col>>16)&0xFF)/255.0f, f);
    float gg = Lerp(0x60/255.0f, ((col>> 8)&0xFF)/255.0f, f);
    float bb = Lerp(0xCC/255.0f, ( col     &0xFF)/255.0f, f);
    ImU32 barCol = IM_COL32(int(rr*255), int(gg*255), int(bb*255), static_cast<int>(200*g_animAlpha));

    for (int i = 0; i < n; i++) {
        float bx  = x0 + i * sp;
        float env = 1.0f - fabsf((i / float(n-1)) * 2.0f - 1.0f);
        float w1  = sinf(i * 0.6f - t * 0.15f) * 0.5f + 0.5f;
        float w2  = sinf(i * 1.1f + t * 0.08f) * 0.25f + 0.25f;
        float bh  = (w1 + w2) * env * 70*sc * f + 4*sc;
        dl->AddRectFilled({bx-bw/2,c.y-bh},{bx+bw/2,c.y+bh}, barCol, 2.0f);
        dl->AddLine({bx-bw/2,c.y-bh},{bx+bw/2,c.y-bh},IM_COL32(255,255,255,80),1.0f);
    }
    dl->AddLine({c.x-tw/2,c.y},{c.x+tw/2,c.y}, Col(col,0.28f), 1.0f);
}

//
// ANIMATION 4 — PIXEL BLOCKS
//
static std::vector<int> s_blockOrder;

static void Anim_PixelBlocks(ImDrawList* dl, ImVec2 c, float sc,
                              float p, float /*t*/, uint32_t col) {
    if (s_blockOrder.empty()) {
        s_blockOrder.resize(64);
        for (int i = 0; i < 64; i++) s_blockOrder[i] = i;
        for (int i = 63; i > 0; i--) {
            int j = rand() % (i+1);
            std::swap(s_blockOrder[i], s_blockOrder[j]);
        }
    }
    int litN = static_cast<int>(p / 100.0f * 64);
    float bs = 18*sc, gap = 3*sc, step = bs + gap;
    float ox = c.x - 4*step + gap/2;
    float oy = c.y - 4*step + gap/2;

    for (int idx = 0; idx < 64; idx++) {
        int cell   = s_blockOrder[idx];
        float bx   = ox + (cell % 8) * step;
        float by   = oy + (cell / 8) * step;
        bool  isLit = (idx < litN);
        dl->AddRectFilled({bx,by},{bx+bs,by+bs}, Col(col,  isLit ? 0.85f : 0.38f), 2.0f);
        dl->AddRect({bx,by},{bx+bs,by+bs}, Col(isLit ? col : 0x334455, isLit ? 0.60f : 0.22f), 2.0f, 0, 1.0f);
        if (isLit && idx == litN-1)
            dl->AddRectFilled({bx+2,by+2},{bx+bs-2,by+bs-2}, IM_COL32(255,255,255,55), 1.0f);
    }
}

//
// ANIMATION 5 — ORBIT DOTS
//
static void Anim_OrbitDots(ImDrawList* dl, ImVec2 c, float sc,
                            float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    int   n    = 12;
    float r    = 70*sc;
    int   litN = static_cast<int>(f * n);
    float curs = t * 2.5f * DEG;

    for (int i = 0; i < n; i++) {
        float a   = (i / float(n)) * 360.0f * DEG - kPI/2;
        bool  act = (i < litN);
        if (act) {
            for (int tr = 1; tr <= 5; tr++) {
                float ta  = (i/float(n))*360.0f*DEG - kPI/2 - tr*4.0f*DEG;
                dl->AddCircleFilled({c.x+cosf(ta)*r, c.y+sinf(ta)*r},
                                    (4.0f - tr*0.5f)*sc, Col(col, (15-tr*2)/100.0f), 10);
            }
        }
        dl->AddCircleFilled({c.x+cosf(a)*r, c.y+sinf(a)*r},
                            (act ? 7.0f : 4.0f)*sc, Col(col, act ? 0.90f : 0.22f), 16);
    }
    dl->AddCircleFilled({c.x+cosf(curs)*r, c.y+sinf(curs)*r},
                        5*sc, IM_COL32(255,255,255,204), 14);
    dl->AddCircle(c, r, Col(col,0.18f), 48, 1.0f);
    dl->AddCircle(c, 14*sc, Col(col,0.40f), 24, 2.0f);
}

//
// ANIMATION 6 — COMPASS ROSE
//
static void Anim_CompassRose(ImDrawList* dl, ImVec2 c, float sc,
                              float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    int   revN = static_cast<int>(f * 8);
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);

    for (int i = 0; i < 8; i++) {
        float ba   = (i / 8.0f) * 360.0f * DEG - kPI/2;
        bool  card = (i % 2 == 0);
        float len  = (card ? 90.0f : 55.0f) * sc;
        float wid  = (card ? 14.0f :  9.0f) * sc;
        bool  rev  = (i < revN);
        ImU32 fc   = Col(col, rev ? 0.80f : 0.28f);

        float tipX = c.x + cosf(ba)*len, tipY = c.y + sinf(ba)*len;
        float sA   = ba + kPI/2, sB = ba - kPI/2;
        float bx   = c.x + cosf(ba)*15*sc, by = c.y + sinf(ba)*15*sc;
        float mx   = c.x + cosf(ba)*(len*0.45f), my = c.y + sinf(ba)*(len*0.45f);

        ImVec2 pts[7] = {
            {bx + cosf(sA)*wid,        by + sinf(sA)*wid},
            {mx + cosf(sA)*wid*0.4f,   my + sinf(sA)*wid*0.4f},
            {tipX, tipY},
            {mx + cosf(sB)*wid*0.4f,   my + sinf(sB)*wid*0.4f},
            {bx + cosf(sB)*wid,        by + sinf(sB)*wid},
            {bx, by},
            {bx + cosf(sA)*wid,        by + sinf(sA)*wid}
        };
        dl->AddConvexPolyFilled(pts, 6, fc);
        dl->AddPolyline(pts, 7, Col(col, rev ? 0.90f : 0.35f), 0, 1.0f);
    }
    dl->AddCircle(c, 16*sc, Col(col, 0.60f + 0.20f*glow), 24, 2.0f);
    dl->AddCircleFilled(c, 12*sc, Col(col, 0.50f), 20);
}

//
// ANIMATION 7 — HELIX SPIRAL
//
static void Anim_Helix(ImDrawList* dl, ImVec2 c, float sc,
                        float p, float t, uint32_t col) {
    float f      = p / 100.0f;
    int   segs   = 48;
    float height = 90.0f * f * sc;
    float twist  = t * 0.05f;

    for (int i = 0; i <= segs; i++) {
        float frac   = i / float(segs);
        float hy     = -height + frac * height * 2.0f;
        float angle1 = frac * 4.0f * kPI + twist;
        float angle2 = angle1 + kPI;
        float amp    = (35.0f + 10.0f * sinf(frac * kPI)) * sc;
        float hx1 = cosf(angle1)*amp, hx2 = cosf(angle2)*amp;
        float d1  = sinf(angle1),     d2  = sinf(angle2);

        dl->AddCircleFilled({c.x+hx1,c.y+hy}, 3*sc, Col(col, 0.30f + 0.55f*((d1+1)/2)), 8);
        dl->AddCircleFilled({c.x+hx2,c.y+hy}, 3*sc, Col(0x5599CC, 0.30f + 0.55f*((d2+1)/2)), 8);
        if (i % 4 == 0) {
            float alpha = 0.20f + 0.20f * fabsf(d1);
            dl->AddLine({c.x+hx1,c.y+hy},{c.x+hx2,c.y+hy},Col(col,alpha),1.0f);
        }
    }
    dl->AddLine({c.x,c.y-height},{c.x,c.y+height},Col(col,0.20f),1.0f);
}

//
// ANIMATION 8 — SNOWFLAKE
//
static void Anim_Snowflake(ImDrawList* dl, ImVec2 c, float sc,
                            float p, float t, uint32_t /*col*/) {
    float f      = p / 100.0f;
    float armLen = 80.0f * f * sc;

    for (int arm = 0; arm < 6; arm++) {
        float ba = (arm / 6.0f) * 360.0f * DEG;
        ImVec2 tip { c.x + cosf(ba)*armLen, c.y + sinf(ba)*armLen };
        dl->AddLine(c, tip, IM_COL32(136,204,255,204), 2.0f);

        for (int br = 1; br <= 3; br++) {
            float bf  = br / 4.0f;
            float bl  = (1.0f - bf) * 28.0f * f * sc;
            float bbx = c.x + cosf(ba)*(armLen*bf);
            float bby = c.y + sinf(ba)*(armLen*bf);
            float a1  = ba + 60*DEG, a2 = ba - 60*DEG;
            dl->AddLine({bbx,bby},{bbx+cosf(a1)*bl,bby+sinf(a1)*bl},IM_COL32(170,221,255,178),1.5f);
            dl->AddLine({bbx,bby},{bbx+cosf(a2)*bl,bby+sinf(a2)*bl},IM_COL32(170,221,255,178),1.5f);
        }
    }
    if (f > 0.85f) {
        for (int sa = 0; sa < 6; sa++) {
            float spa   = (sa/6.0f)*360.0f*DEG;
            float flash = fabsf(sinf(t*0.2f + sa));
            dl->AddCircleFilled({c.x+cosf(spa)*armLen, c.y+sinf(spa)*armLen},
                4*sc, IM_COL32(255,255,255,static_cast<int>(flash*150)), 10);
        }
    }
    dl->AddCircle(c, 8*sc, IM_COL32(136,204,255,178), 16, 2.0f);
    dl->AddCircleFilled(c, 5*sc, IM_COL32(204,238,255,153), 12);
}

//
// ANIMATION 9 — LINEAR BAR
//
static void Anim_LinearBar(ImDrawList* dl, ImVec2 c, float sc,
                            float p, float t, uint32_t col) {
    float f  = p / 100.0f;
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float bw = std::clamp(std::min(disp.x - c.x, c.x) * 1.6f, 80.0f*sc, 700.0f*sc);
    float bh = 20.0f*sc;
    // Anchor: left-half -> left edge at c.x; right-half -> right edge at c.x; middle -> centred
    float bx;
    float decoPad = 14.0f*sc;  // room for diamond caps beyond bar ends
    if (c.x < disp.x * 0.45f)
        bx = c.x;                   // left-anchored
    else if (c.x > disp.x * 0.55f)
        bx = c.x - bw;              // right-anchored
    else
        bx = c.x - bw * 0.5f;      // centred
    bx = std::clamp(bx, decoPad, disp.x - bw - decoPad);
    float by = c.y - bh*0.5f;

    dl->AddRectFilled({bx-2,by-2},{bx+bw+2,by+bh+2}, IM_COL32(13,13,26,230), 3.0f);
    dl->AddRect({bx-2,by-2},{bx+bw+2,by+bh+2}, Col(0x444455,0.80f), 3.0f, 0, 1.0f);

    if (f > 0.001f) {
        float fw = bw * f;
        dl->AddRectFilled({bx,by},{bx+fw,by+bh}, Col(col,0.85f), 2.0f);
        int stripes = static_cast<int>(fw / 4);
        for (int si = 0; si < stripes; si++)
            dl->AddRectFilled({bx+si*4.0f,by},{bx+si*4.0f+2.0f,by+bh}, IM_COL32(0,0,0,45));
        float flash = 0.35f + 0.25f * sinf(t * 0.15f);
        dl->AddRectFilled({bx+fw-4,by},{bx+fw,by+bh},
                          IM_COL32(255,255,255,static_cast<int>(flash*255)));
    }

    auto Diamond = [&](float cx, float cy, float s) {
        ImVec2 pts[4] = {{cx,cy-s},{cx+s,cy},{cx,cy+s},{cx-s,cy}};
        dl->AddConvexPolyFilled(pts, 4, Col(col,0.25f));
        dl->AddPolyline(pts, 4, Col(col,0.70f), ImDrawFlags_Closed, 1.5f);
    };
    Diamond(bx - 10*sc, c.y, 10*sc);
    Diamond(bx + bw + 10*sc, c.y, 10*sc);
}

//
// ANIMATION 10 — ALCHEMY CAULDRON
//
static void Anim_SoulGem(ImDrawList* dl, ImVec2 c, float sc,
                          float p, float t, uint32_t col) {
    float f       = p / 100.0f;
    float glow    = 0.5f + 0.5f*sinf(t * 0.08f);

    // Cauldron geometry
    float rimW  = 34.0f*sc;
    float belW  = 52.0f*sc;
    float baseW = 20.0f*sc;
    float tY    = c.y - 48.0f*sc;
    float shY   = c.y - 20.0f*sc;
    float midY  = c.y + 10.0f*sc;
    float bY    = c.y + 46.0f*sc;
    float legBot = bY + 14.0f*sc;

    // Flames beneath (multi-frequency flicker, 4 layers per flame, col palette)
    float flameBot = legBot + 2.0f*sc;
    // Heat pool glow
    float poolA = 0.5f + 0.5f*sinf(t*0.19f);
    dl->AddEllipseFilled({c.x, flameBot+sc}, {44.0f*sc, 5.5f*sc},
                         Col(col, 0.10f + 0.06f*poolA), 0.0f, 16);
    for (int fi = 0; fi < 5; fi++) {
        // Three independent sin frequencies -> organic, never-repeating flicker
        float n1 = sinf(t*0.17f + fi*1.31f);
        float n2 = sinf(t*0.37f + fi*0.83f);
        float n3 = sinf(t*0.61f + fi*2.07f);
        float noise = n1*0.45f + n2*0.35f + n3*0.20f;  // range ≈ [-1, 1]
        float hs  = 0.60f + 0.40f*noise;               // height scale

        float baseFX = c.x + (fi-2)*12.0f*sc;
        float sway   = sinf(t*0.29f + fi*1.73f) * 1.8f*sc;  // tip sway
        float fh     = (8.5f + 6.0f*sinf(fi*1.57f))*sc * hs;
        float fw     = (5.0f + (fi%3)*1.9f)*sc;

        // Layer 1 — wide outer glow (very transparent)
        ImVec2 l1[3] = {{baseFX+sway, flameBot-fh*1.10f},
                         {baseFX-fw*1.5f, flameBot}, {baseFX+fw*1.5f, flameBot}};
        dl->AddConvexPolyFilled(l1, 3, Col(col, 0.10f));

        // Layer 2 — main flame body
        ImVec2 l2[3] = {{baseFX+sway*0.7f, flameBot-fh},
                         {baseFX-fw, flameBot}, {baseFX+fw, flameBot}};
        dl->AddConvexPolyFilled(l2, 3, Col(col, 0.38f));

        // Layer 3 — bright core
        ImVec2 l3[3] = {{baseFX+sway*0.4f, flameBot-fh*0.55f},
                         {baseFX-fw*0.42f, flameBot-sc}, {baseFX+fw*0.42f, flameBot-sc}};
        dl->AddConvexPolyFilled(l3, 3, Col(col, 0.68f));

        // Layer 4 — hot tip (brightest, narrowest)
        ImVec2 l4[3] = {{baseFX+sway*0.2f, flameBot-fh*0.22f},
                         {baseFX-fw*0.16f, flameBot-fh*0.08f}, {baseFX+fw*0.16f, flameBot-fh*0.08f}};
        dl->AddConvexPolyFilled(l4, 3, Col(col, 0.92f));
    }

    // Body
    ImVec2 body[8] = {
        {c.x-rimW,tY},{c.x+rimW,tY},
        {c.x+belW,shY},{c.x+belW,midY},
        {c.x+baseW,bY},{c.x-baseW,bY},
        {c.x-belW,midY},{c.x-belW,shY},
    };
    dl->AddConvexPolyFilled(body, 8, IM_COL32(18,21,26,(int)(252*g_animAlpha)));

    // Liquid (rises with progress)
    if (f > 0.01f) {
        float liquidTop = bY - (bY-tY)*f;
        liquidTop = std::max(liquidTop, tY+4.0f*sc);

        // Width at liquid surface — follows cauldron profile, 1sc inset from walls
        float liqW;
        if      (liquidTop >= midY) liqW = baseW+(belW-baseW)*(1.0f-(liquidTop-midY)/(bY-midY))-sc;
        else if (liquidTop >= shY)  liqW = belW - sc;
        else                        liqW = rimW+(belW-rimW)*(liquidTop-tY)/(shY-tY)-sc;
        liqW = std::clamp(liqW, 0.0f, belW);

        // Liquid polygon follows cauldron inner contour exactly (no rectangle)
        ImVec2 lp[10]; int ln = 0;
        lp[ln++] = {c.x+liqW, liquidTop};
        if (liquidTop < shY)  { lp[ln++] = {c.x+belW-sc, shY};  lp[ln++] = {c.x+belW-sc, midY}; }
        else if (liquidTop < midY) lp[ln++] = {c.x+belW-sc, midY};
        lp[ln++] = {c.x+baseW-sc, bY-sc};
        lp[ln++] = {c.x-baseW+sc, bY-sc};
        if (liquidTop < midY) lp[ln++] = {c.x-belW+sc, midY};
        if (liquidTop < shY)  lp[ln++] = {c.x-belW+sc, shY};
        lp[ln++] = {c.x-liqW, liquidTop};
        dl->AddConvexPolyFilled(lp, ln, Col(col, 0.36f+0.14f*glow));

        // Surface shimmer
        dl->AddLine({c.x-liqW,liquidTop},{c.x+liqW,liquidTop},
                    Col(col, 0.52f+0.26f*glow), 1.5f);

        // Boiling over: puffs + drops above rim (f > 0.78)
        if (f > 0.78f) {
            float bf = (f-0.78f)/0.22f;
            // Steam puffs
            for (int si = 0; si < 5; si++) {
                float ph = fmodf(t*0.032f+si*0.22f, 1.0f);
                float sx = c.x+(si-2)*rimW*0.45f+sinf(ph*kPI*2.0f+si)*5.0f*sc;
                float sy = tY-ph*38.0f*sc;
                float sa = (ph<0.20f?ph/0.20f:(1.0f-ph)/0.80f)*0.28f*bf;
                dl->AddCircleFilled({sx,sy},(2.5f+ph*8.0f)*sc, Col(col,sa), 8);
            }
            // Liquid drops flung above rim
            for (int di = 0; di < 4; di++) {
                float ph = fmodf(t*0.048f+di*0.27f, 1.0f);
                float dx = c.x+(di-1.5f)*rimW*0.55f;
                float arc = sinf(ph*kPI);
                float dy  = tY - arc*18.0f*sc;
                float da  = arc*0.75f*bf;
                dl->AddCircleFilled({dx,dy}, (1.2f+bf*0.8f)*sc, Col(col,da), 5);
            }
        }

        // Sparks at very high f (> 0.88)
        if (f > 0.88f) {
            float sf = (f-0.88f)/0.12f;
            for (int si = 0; si < 7; si++) {
                float ph  = fmodf(t*0.060f+si*0.145f, 1.0f);
                float ang = kPI*0.85f+si*kPI*0.26f;
                float sp  = ph*(22.0f+si*5.0f)*sc*sf;
                float sa  = (1.0f-ph)*0.80f*sf;
                dl->AddCircleFilled({c.x+cosf(ang)*sp, tY-fabsf(sinf(ang))*sp*0.6f-ph*10.0f*sc},
                                    1.4f*sc, Col(col,sa), 4);
            }
        }
    }

    // Outline + rim
    dl->AddPolyline(body, 8, Col(col, 0.38f+0.22f*f), ImDrawFlags_Closed, 2.0f);
    dl->AddEllipseFilled({c.x,tY},{rimW,5.5f*sc}, IM_COL32(16,19,23,(int)(248*g_animAlpha)), 0.0f, 16);
    dl->AddEllipse({c.x,tY},{rimW+sc,6.0f*sc}, Col(col, 0.50f+0.28f*f), 0.0f, 16, 2.0f);

    // Handles
    for (int side = -1; side <= 1; side += 2) {
        float hx = c.x+side*(rimW+9.0f*sc);
        dl->AddCircle({hx,tY+3.0f*sc}, 6.0f*sc, Col(col, 0.32f+0.15f*f), 12, 1.6f);
        dl->AddLine({c.x+side*rimW,tY},{hx-side*6.0f*sc,tY+3.0f*sc}, Col(col,0.25f), 1.3f);
    }

    // Legs
    for (int li = -1; li <= 1; li++) {
        float lx = c.x+li*14.0f*sc;
        dl->AddLine({lx,bY},{lx+li*6.0f*sc,legBot}, Col(col,0.38f), 2.5f);
    }
}

//
// ANIMATION 11 — DWEMER COGS
// Interlocking mechanical gears — Dwemer Automaton aesthetic
//
static void Anim_DwemerCogs(ImDrawList* dl, ImVec2 c, float sc,
                             float p, float t, uint32_t col) {
    float f     = p / 100.0f;
    float rot   = t * 0.020f;          // radians (≈1.2 rad/s at 60 fps)
    float alpha = 0.25f + 0.65f * f;

    // Three-gear train with correct meshing distances (centre dist = R1+R2 exactly)
    // Gear 1 (driver, 12 teeth) -> Gear 2 (right, 7 teeth) -> Gear 3 (above 2, 5 teeth)
    const int   N1 = 12, N2 = 7, N3 = 5;
    const float R1 = 42.0f*sc, R2 = 25.0f*sc, R3 = 16.0f*sc;

    ImVec2 g1 = { c.x - 20.0f*sc,  c.y + 5.0f*sc  };
    ImVec2 g2 = { g1.x + R1 + R2,  g1.y            };   // exact mesh distance
    ImVec2 g3 = { g2.x,             g2.y - (R2+R3) };   // exact mesh distance

    // Rotation ratios: omega2 = -omega1*N1/N2, phase-offset = π/N (half tooth)
    float rot1 =  rot;
    float rot2 = -(rot1 * float(N1) / float(N2)) + kPI / float(N2);
    float rot3 = -(rot2 * float(N2) / float(N3)) + kPI / float(N3);

    // Draw back-to-front so each gear body covers the meshing tooth tips
    DrawGear(dl, g3, R3, rot3, N3, col, alpha * 0.65f);
    DrawGear(dl, g2, R2, rot2, N2, col, alpha * 0.82f);
    DrawGear(dl, g1, R1, rot1, N1, col, alpha);

    // Progress arc orbiting the drive gear
    float arcR = R1 + 13.0f*sc;
    if (f > 0.01f)
        Arc(dl, g1, arcR, -90.0f, -90.0f + 360.0f*f, Col(col, 0.82f), 3.0f);
    Arc(dl, g1, arcR, -90.0f + 360.0f*f, 270.0f, Col(col, 0.14f), 1.2f);
}

//
// ANIMATION 12 — AURORA BOREALIS
// Northern lights ripple across the sky as the load fills.
// Bands appear one by one; mountain silhouette and stars ground the scene.
//
static void Anim_Shout(ImDrawList* dl, ImVec2 c, float sc,
                        float p, float t, uint32_t col) {
    float f = p / 100.0f;
    float R = 90.0f * sc;

    // Stars — scattered, pulsing subtly
    static const float starX[] = {-0.82f,-0.45f, 0.15f, 0.62f,-0.22f, 0.42f,-0.68f, 0.80f, 0.00f,-0.55f};
    static const float starY[] = {-0.80f,-0.55f,-0.90f,-0.65f,-0.38f,-0.78f,-0.20f,-0.45f,-0.70f,-0.92f};
    for (int si = 0; si < 10; si++) {
        float puls = 0.35f + 0.40f*sinf(t*0.05f + si*0.88f);
        float alpha = puls * (0.20f + 0.70f*f);
        float sr = (0.9f + si%2) * sc;
        dl->AddCircleFilled({c.x + starX[si]*R, c.y + starY[si]*R}, sr,
                            IM_COL32(235,240,255, int(alpha*255)), 4);
    }

    // Aurora bands — appear progressively, each a flowing sine polyline
    struct Band { float yOff; float amp; float freq; float spd; int r,g,b; float minF; };
    static const Band bands[] = {
        {-0.58f, 0.07f, 0.75f, 0.85f,  48,255,120, 0.15f},
        {-0.34f, 0.09f, 1.05f, 1.18f,  28,220,195, 0.40f},
        {-0.12f, 0.06f, 1.30f, 0.62f,  70,155,255, 0.62f},
        { 0.08f, 0.05f, 0.65f, 1.50f, 155, 75,255, 0.80f},
    };
    float W = R * 2.30f;

    for (auto& b : bands) {
        float bF = std::clamp((f - b.minF) / 0.28f, 0.0f, 1.0f);
        if (bF < 0.01f) continue;
        float shimmer = 0.5f + 0.5f*sinf(t*0.06f + b.yOff*3.0f);
        float alpha   = bF * (0.38f + 0.30f*shimmer);
        float baseY   = c.y + b.yOff * R;

        const int NPTS = 18;
        ImVec2 pts[NPTS];
        for (int pi = 0; pi < NPTS; pi++) {
            float nx  = -1.0f + 2.0f*pi/float(NPTS-1);
            float wave = sinf(b.freq*nx*kPI + t*b.spd*0.05f) * b.amp * R;
            pts[pi]   = {c.x + nx*W*0.5f, baseY + wave};
        }
        dl->AddPolyline(pts, NPTS, IM_COL32(b.r,b.g,b.b, int(alpha*0.28f*255)), 0, 20.0f*sc);
        dl->AddPolyline(pts, NPTS, IM_COL32(b.r,b.g,b.b, int(alpha*0.62f*255)), 0,  7.0f*sc);
        dl->AddPolyline(pts, NPTS, IM_COL32(b.r,b.g,b.b, int(alpha*0.88f*255)), 0,  2.2f*sc);
    }

    // Mountain silhouette — random profile per load
    float horizY = c.y + 0.32f*R;
    dl->AddRectFilled({c.x-W*0.5f, horizY}, {c.x+W*0.5f, horizY+28.0f*sc}, IM_COL32(4,6,10,230));

    // Generate 7 mountains from s_mountainSeed (LCG)
    unsigned mState = static_cast<unsigned>(s_mountainSeed);
    auto mLcg = [](unsigned& s) -> float {
        s = s * 1664525u + 1013904223u;
        return float(s >> 1) / float(0x7FFFFFFFu);
    };
    // Fixed x positions spread evenly; vary height and slight x jitter per load
    const int NM = 7;
    float mxBase[NM] = {-0.80f,-0.54f,-0.28f,-0.02f,0.25f,0.52f,0.78f};
    for (int mi = 0; mi < NM; mi++) {
        float mxj = mxBase[mi] + (mLcg(mState) - 0.5f)*0.14f;
        float mh  = (10.0f + mLcg(mState)*28.0f) * sc;
        float mw  = mh * (0.60f + mLcg(mState)*0.50f);
        float mx2 = c.x + mxj*R;
        dl->AddTriangleFilled({mx2-mw, horizY}, {mx2, horizY-mh}, {mx2+mw, horizY},
                              IM_COL32(5,7,13,238));
        dl->AddTriangle({mx2-mw, horizY}, {mx2, horizY-mh}, {mx2+mw, horizY},
                        Col(col, 0.11f + 0.09f*f), 0.7f);
    }
}

//
// ANIMATION 13 — CONSTELLATION
// Stars appear and connect into a Nordic constellation pattern
//
static void Anim_Constellation(ImDrawList* dl, ImVec2 c, float sc,
                                float p, float t, uint32_t col) {
    float f = p / 100.0f;
    float R = 82.0f * sc;

    struct Star { float x, y; };
    static const Star stars[11] = {
        { 0.00f,-1.00f },  // 0
        {-0.58f,-0.55f },  // 1
        { 0.58f,-0.55f },  // 2
        {-0.95f, 0.10f },  // 3
        { 0.95f, 0.10f },  // 4
        {-0.40f, 0.65f },  // 5
        { 0.40f, 0.65f },  // 6
        { 0.00f, 1.00f },  // 7
        {-0.20f,-0.12f },  // 8
        { 0.20f,-0.12f },  // 9
        { 0.00f, 0.30f },  // 10
    };
    const int NStar = 11, NEdge = 13;

    struct Edge { int a, b; };
    // Six distinct connection patterns — random per load via s_constPattern
    static const Edge patterns[6][13] = {
        {{0,1},{0,2},{1,3},{2,4},{3,5},{4,6},{5,7},{6,7},{1,8},{2,9},{8,9},{8,10},{9,10}},
        {{0,2},{2,4},{4,6},{6,7},{7,5},{5,3},{3,1},{1,0},{1,9},{9,10},{10,8},{8,5},{0,9}},
        {{0,7},{1,6},{2,5},{3,4},{0,10},{10,7},{8,4},{9,3},{8,10},{9,10},{1,2},{0,8},{0,9}},
        {{0,3},{0,4},{3,5},{4,6},{5,6},{1,5},{2,6},{7,10},{8,10},{9,10},{0,8},{0,9},{1,2}},
        {{0,1},{1,5},{5,7},{7,6},{6,2},{2,0},{0,10},{10,7},{3,8},{4,9},{1,8},{2,9},{3,4}},
        {{0,4},{4,2},{2,10},{10,3},{3,1},{1,9},{9,7},{7,8},{8,6},{6,5},{5,0},{0,6},{1,10}},
    };
    const Edge* edges = patterns[s_constPattern % 6];

    auto SP = [&](int i) -> ImVec2 {
        return { c.x + stars[i].x * R, c.y + stars[i].y * R };
    };

    int litStars = static_cast<int>(f * NStar);
    int litEdges = static_cast<int>(f * NEdge);

    for (int i = 0; i < litEdges; i++) {
        if (edges[i].a < litStars && edges[i].b < litStars)
            dl->AddLine(SP(edges[i].a), SP(edges[i].b), Col(col, 0.25f), 1.2f);
    }

    for (int i = 0; i < NStar; i++) {
        ImVec2 sp   = SP(i);
        bool   lit  = (i < litStars);
        float  puls = lit ? (0.6f + 0.4f*sinf(t*0.08f + i*1.37f)) : 0.0f;
        float  sr   = (lit ? (4.2f + 2.0f*puls) : 1.8f) * sc;

        if (lit) {
            dl->AddCircleFilled(sp, sr*2.8f, Col(col, 0.10f*puls), 10);
            for (int ray = 0; ray < 4; ray++) {
                float ra = ray * kPI * 0.5f;
                dl->AddLine(sp, {sp.x+cosf(ra)*sr*2.5f, sp.y+sinf(ra)*sr*2.5f},
                            Col(col, 0.40f*puls), 1.0f);
            }
        }
        dl->AddCircleFilled(sp, sr, Col(col, lit ? 0.90f : 0.15f), 10);
    }
}

//
// ANIMATION 14 — DRAGON SCALES
// Overlapping kite-shaped scales arranged like dragon or reptile hide.
// Narrow attachment at top, widest at mid, tapering to a bottom point.
// Upper rows paint over lower rows (like shingles / real scales).
//
static void Anim_DragonScales(ImDrawList* dl, ImVec2 c, float sc,
                               float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);

    // Each kite scale: pointed bottom, narrow top attachment, widest at ~40% down
    float sh = 36.0f * sc;   // scale height (top to bottom point)
    float tw = sh * 0.32f;   // half-width at top attachment edge
    float mw = sh * 0.52f;   // half-width at widest (40% down)
    float bw = sh * 0.30f;   // half-width near bottom

    // Rows overlap: each row's top is inside the previous row
    float rowStep = sh * 0.68f;   // each row starts 32% below previous top
    float colStep = mw * 2.10f;

    const int NROW = 4;
    const int rowCols[4] = { 5, 4, 5, 4 };
    float y0 = c.y - rowStep * 1.5f;
    int total = 18, litCount = int(f * total);

    // Draw from bottom row upward: upper rows overlap lower ones (like shingles)
    for (int row = NROW - 1; row >= 0; row--) {
        int   nc     = rowCols[row];
        float cy     = y0 + row * rowStep;
        float xStart = c.x - colStep * (nc - 1) * 0.5f;

        int rowStart = 0;
        for (int r2 = 0; r2 < row; r2++) rowStart += rowCols[r2];

        for (int ci = 0; ci < nc; ci++) {
            int   idx   = rowStart + ci;
            bool  lit   = (idx < litCount);
            float cx2   = xStart + ci * colStep;
            float alpha = lit ? (0.52f + 0.30f*glow) : 0.08f;

            // Kite polygon — 7 vertices, convex
            ImVec2 pts[7] = {
                {cx2 - tw, cy},              // top-left  (narrow)
                {cx2 + tw, cy},              // top-right (narrow)
                {cx2 + mw, cy + sh*0.40f},   // mid-right (widest)
                {cx2 + bw, cy + sh*0.74f},   // lower-right
                {cx2,      cy + sh},          // bottom point
                {cx2 - bw, cy + sh*0.74f},   // lower-left
                {cx2 - mw, cy + sh*0.40f},   // mid-left  (widest)
            };

            // Dark metallic fill
            dl->AddConvexPolyFilled(pts, 7,
                lit ? Col(col, alpha * 0.30f) : Col(0x111820, 0.28f));

            // Upper sheen: lighter strip at the top where light would catch
            if (lit) {
                ImVec2 sheen[4] = {pts[0], pts[1], pts[2], pts[6]};
                dl->AddConvexPolyFilled(sheen, 4, Col(col, alpha * 0.20f));
            }

            // Outline
            dl->AddPolyline(pts, 7, Col(col, lit ? alpha : 0.11f),
                            ImDrawFlags_Closed, lit ? 1.6f : 0.9f);

            // Central ridge running from attachment to point
            if (lit)
                dl->AddLine({cx2, cy + sh*0.05f}, {cx2, cy + sh*0.88f},
                            Col(col, alpha * 0.48f), 1.1f);
        }
    }
}

//
// ANIMATION 15 — ARCANE ENCHANTMENT
// Spinning arcane rings with orbiting rune symbols
//
static void Anim_Enchantment(ImDrawList* dl, ImVec2 c, float sc,
                              float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.09f);

    struct Ring { float r; float spd; float thick; float minF; };
    Ring rings[] = {
        { 28.0f,  3.5f, 3.0f, 0.00f },
        { 50.0f, -2.0f, 2.5f, 0.25f },
        { 72.0f,  1.3f, 2.0f, 0.50f },
        { 95.0f, -0.8f, 1.5f, 0.75f },
    };

    for (int ri = 0; ri < 4; ri++) {
        auto& r    = rings[ri];
        float alpha = std::clamp((f - r.minF) / 0.25f, 0.0f, 1.0f)
                      * (0.28f + 0.42f*glow);
        if (alpha < 0.01f) continue;

        float rotOff = t * r.spd;                  // radians
        float arcDeg = std::min(360.0f * (f - r.minF) / (1.0f - r.minF + 0.01f), 360.0f);

        // Three broken-ring segments (rotating)
        const int nBreak = 3;
        float segArc = arcDeg / nBreak * 0.78f;
        for (int s = 0; s < nBreak; s++) {
            float startR = rotOff + (s / float(nBreak)) * 2.0f * kPI;
            float startD = startR / DEG;
            Arc(dl, c, r.r*sc, startD, startD + segArc,
                Col(col, alpha), r.thick*sc);
        }
    }

    // Orbiting rune diamonds
    int runeCount = 1 + static_cast<int>(f * 6);
    for (int i = 0; i < runeCount && i < 7; i++) {
        float angle  = t * (0.025f + i * 0.006f) + i * 0.90f;
        float orbitR = (32.0f + i * 11.0f) * sc;
        ImVec2 rp    = { c.x + cosf(angle)*orbitR, c.y + sinf(angle)*orbitR };
        float rs     = (2.5f + (i % 3)) * sc;
        ImVec2 rpts[4] = {
            { rp.x,    rp.y - rs*1.6f },
            { rp.x+rs, rp.y           },
            { rp.x,    rp.y + rs*1.6f },
            { rp.x-rs, rp.y           }
        };
        dl->AddConvexPolyFilled(rpts, 4, Col(col, 0.55f));
        dl->AddPolyline(rpts, 4, IM_COL32(255,255,255,100), ImDrawFlags_Closed, 1.0f);
    }

    // Energy core
    float coreR = (7.0f + 11.0f*f + 4.0f*glow) * sc;
    dl->AddCircleFilled(c, coreR, Col(col, 0.35f + 0.28f*glow), 24);
    dl->AddCircle(c, coreR, IM_COL32(255,255,255,static_cast<int>((0.45f+0.45f*glow)*255)), 24, 2.0f);
}

//
// ANIMATION 16 — WORD WALL
// Nordic stone wall with glowing Dovahzul inscriptions appearing
//
static void Anim_WordWall(ImDrawList* dl, ImVec2 c, float sc,
                           float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.06f);

    // Stone slab — a single continuous piece of wall
    float wW  = 270.0f * sc, wH = 102.0f * sc;
    float bdr =   7.0f * sc;   // border inset
    float x0  = c.x - wW*0.5f, y0 = c.y - wH*0.5f;
    float x1  = x0 + wW,       y1 = y0 + wH;

    // Stone background
    dl->AddRectFilled({x0, y0}, {x1, y1}, Col(0x18202C, 0.92f), 4.0f);
    // Subtle horizontal stone seams
    for (int gi = 1; gi < 3; gi++) {
        float gy = y0 + wH*gi/3.0f;
        dl->AddLine({x0+bdr, gy}, {x1-bdr, gy}, Col(0x000000, 0.22f), 0.8f);
    }
    // Outer border
    dl->AddRect({x0, y0}, {x1, y1}, Col(col, 0.48f + 0.18f*f), 4.0f, 0, 2.5f);
    // Inner border
    dl->AddRect({x0+bdr, y0+bdr}, {x1-bdr, y1-bdr},
                Col(col, 0.18f + 0.10f*f), 2.0f, 0, 1.0f);
    // Corner chisel marks
    float cm = bdr * 0.85f;
    for (int cx2 = 0; cx2 < 2; cx2++) for (int cy2 = 0; cy2 < 2; cy2++) {
        float bx = cx2 ? x1-bdr : x0+bdr, by = cy2 ? y1-bdr : y0+bdr;
        float dx = cx2 ? -cm : cm,         dy = cy2 ? -cm : cm;
        dl->AddLine({bx,by},{bx+dx,by}, Col(col,0.35f), 1.0f);
        dl->AddLine({bx,by},{bx,by+dy}, Col(col,0.35f), 1.0f);
    }

    // Three rows of continuous inscription
    const int ROWS = 3, COLS = 9;
    float cellW = (wW - bdr*3.0f) / COLS;
    float cellH = (wH - bdr*3.0f) / ROWS;
    float rx0   = x0 + bdr*1.5f;
    float ry0   = y0 + bdr*1.5f;

    int total = ROWS * COLS;
    int litN  = static_cast<int>(f * total);

    for (int row = 0; row < ROWS; row++) {
        for (int ci = 0; ci < COLS; ci++) {
            int   idx  = row * COLS + ci;
            bool  lit  = (idx < litN);
            float mx   = rx0 + (ci + 0.5f) * cellW;
            float my   = ry0 + (row + 0.5f) * cellH;
            float gh   = cellH * 0.33f;
            float gw   = cellW * 0.26f;

            // All runes uniform — random Dovahzul-style glyph per cell per load
            float glA = lit ? (0.52f + 0.22f*glow) : 0.09f;
            float lw  = lit ? 1.5f*sc : 1.1f*sc;

            // Hash cell index with load seed for stable-per-load random glyph type
            int tp = static_cast<int>(
                (static_cast<unsigned>(s_wallSeed) ^ (static_cast<unsigned>(idx) * 2246822519u))
                >> 17) % 8;

            switch (tp) {
            case 0:  // slash-with-cap: main diagonal + horizontal at top
                dl->AddLine({mx-gw,my-gh},{mx+gw*0.35f,my+gh},   Col(col,glA),        lw);
                dl->AddLine({mx-gw,my-gh},{mx+gw*0.60f,my-gh},   Col(col,glA*0.85f),  lw*0.90f);
                break;
            case 1:  // angled cross: two diagonals, one arm longer
                dl->AddLine({mx-gw*0.5f,my-gh},{mx+gw*0.7f,my+gh},  Col(col,glA),       lw);
                dl->AddLine({mx+gw*0.7f,my-gh},{mx-gw*0.5f,my+gh},  Col(col,glA*0.80f), lw*0.88f);
                break;
            case 2:  // flag: vertical spine + angled arm pointing upper-right
                dl->AddLine({mx-gw*0.2f,my-gh},{mx-gw*0.2f,my+gh},  Col(col,glA),       lw);
                dl->AddLine({mx-gw*0.2f,my-gh*0.1f},{mx+gw*0.8f,my-gh*0.6f}, Col(col,glA*0.85f), lw*0.90f);
                break;
            case 3:  // arrow-down: V shape with center descender
                dl->AddLine({mx-gw*0.7f,my-gh},{mx,my+gh*0.10f},    Col(col,glA),       lw);
                dl->AddLine({mx+gw*0.7f,my-gh},{mx,my+gh*0.10f},    Col(col,glA),       lw);
                dl->AddLine({mx,my+gh*0.10f}, {mx,my+gh},            Col(col,glA*0.80f), lw*0.90f);
                break;
            case 4:  // angular hook: diagonal + angled foot
                dl->AddLine({mx+gw*0.5f,my-gh},{mx-gw*0.5f,my+gh*0.1f}, Col(col,glA),       lw);
                dl->AddLine({mx-gw*0.5f,my+gh*0.1f},{mx+gw*0.3f,my+gh}, Col(col,glA*0.85f), lw);
                break;
            case 5:  // trident: vertical spine + two angled arms at upper third
                dl->AddLine({mx,my-gh},{mx,my+gh},                   Col(col,glA),       lw);
                dl->AddLine({mx,my-gh*0.2f},{mx-gw*0.8f,my-gh*0.7f},Col(col,glA*0.85f),lw);
                dl->AddLine({mx,my-gh*0.2f},{mx+gw*0.8f,my-gh*0.7f},Col(col,glA*0.85f),lw);
                break;
            case 6:  // double-comb: horizontal bar with two vertical drops
                dl->AddLine({mx-gw*0.6f,my-gh*0.3f},{mx+gw*0.6f,my-gh*0.3f}, Col(col,glA),       lw);
                dl->AddLine({mx-gw*0.38f,my-gh*0.3f},{mx-gw*0.38f,my+gh},    Col(col,glA*0.85f), lw);
                dl->AddLine({mx+gw*0.38f,my-gh*0.3f},{mx+gw*0.38f,my+gh},    Col(col,glA*0.85f), lw);
                break;
            case 7:  // bracket: top bar + left spine + short bottom jut
                dl->AddLine({mx-gw*0.5f,my-gh},{mx+gw*0.3f,my-gh},   Col(col,glA),       lw);
                dl->AddLine({mx-gw*0.5f,my-gh},{mx-gw*0.5f,my+gh},   Col(col,glA),       lw);
                dl->AddLine({mx-gw*0.5f,my+gh},{mx+gw*0.3f,my+gh},   Col(col,glA*0.85f), lw*0.90f);
                break;
            }
        }
    }

    // Leading-edge glow on the currently activating glyph
    if (litN > 0 && litN < total) {
        int   eidx = litN - 1;
        float emx  = rx0 + (eidx % COLS + 0.5f) * cellW;
        float emy  = ry0 + (eidx / COLS + 0.5f) * cellH;
        dl->AddCircle({emx, emy}, cellH*0.38f, Col(col, 0.42f*glow), 12, 1.5f);
    }
}

//
// ANIMATION 17 — STANDING STONE
// Skyrim's iconic standing stones: rough-hewn slab with an arched top and a
// carved circular sign that awakens with the player's approach.
//
static void Anim_StandingStone(ImDrawList* dl, ImVec2 c, float sc,
                                float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.07f);

    // Stone dimensions — tall slab, pointed top like real Skyrim standing stones
    float stH   = 92.0f * sc;
    float stWb  = 34.0f * sc;   // base half-width
    float stWt  = 24.0f * sc;   // shaft top half-width
    float peakH = 20.0f * sc;   // extra height of the triangular peak

    float topY = c.y - stH * 0.42f;
    float botY = c.y + stH * 0.58f;
    ImVec2 apex = {c.x, topY - peakH};   // pointed tip

    // Ground shadow
    dl->AddEllipseFilled({c.x, botY + 3.5f*sc},
                         ImVec2{stWb*0.85f, 3.5f*sc},
                         Col(0x000000, 0.32f), 0.0f, 14);

    // Stone body: pentagon (shaft + triangular peak, all one shape)
    ImVec2 stone[5] = {
        apex,
        {c.x + stWt, topY},
        {c.x + stWb, botY},
        {c.x - stWb, botY},
        {c.x - stWt, topY},
    };
    dl->AddConvexPolyFilled(stone, 5, Col(0x1C2626, 0.94f));

    // Stone outline
    dl->AddPolyline(stone, 5, Col(col, 0.28f + 0.20f*f), ImDrawFlags_Closed, 2.0f);

    // Horizontal chisel band just below the peak-to-shaft junction
    float bandY = topY + 5.0f*sc;
    dl->AddLine({c.x - stWt*0.80f, bandY}, {c.x + stWt*0.80f, bandY},
                Col(col, 0.18f), 1.0f);

    // Cracks that glow and spread as the stone awakens (random per load)
    float cw = stWt, ch = (botY - topY), cy0 = topY;

    // LCG to generate pseudo-random floats in [-1,1] and [0,1]
    unsigned crState = static_cast<unsigned>(s_crackSeed);
    auto lcgF = [](unsigned& s) -> float {
        s = s * 1664525u + 1013904223u;
        return float(s >> 1) / float(0x7FFFFFFFu);
    };

    struct CrPt { float x, y; };
    auto DrawCrack = [&](float threshold, const CrPt* pts, int n) {
        float cf = std::clamp((f - threshold) / 0.18f, 0.0f, 1.0f);
        if (cf < 0.01f) return;
        float bb  = f > 0.65f ? std::clamp((f - 0.65f) / 0.25f, 0.0f, 1.0f) : 0.0f;
        float crA = cf * (0.30f + 0.45f*bb + 0.18f*glow);
        for (int i = 0; i < n - 1; i++) {
            ImVec2 a = {c.x + pts[i  ].x*cw, cy0 + pts[i  ].y*ch};
            ImVec2 b = {c.x + pts[i+1].x*cw, cy0 + pts[i+1].y*ch};
            dl->AddLine(a, b, Col(col, crA*0.28f), 5.0f);
            dl->AddLine(a, b, Col(col, crA),        1.4f);
        }
    };

    // 12 cracks evenly distributed top-to-bottom; alternate left/right lean for variety
    static constexpr int NC = 12;
    for (int ci = 0; ci < NC; ci++) {
        // Band centre steps uniformly from 0.04 to 0.94
        float bandCtr = 0.04f + float(ci) * (0.90f / float(NC - 1));
        float bandHalf = 0.055f;

        // Lean bias: every third crack leans left, right, or stays neutral
        float xBias = (ci % 3 == 0) ? 0.18f : (ci % 3 == 1) ? -0.18f : 0.0f;

        CrPt pts[5];
        pts[0].x = std::clamp(xBias + (lcgF(crState) - 0.5f)*0.72f, -0.82f, 0.82f);
        pts[0].y = std::clamp(bandCtr - bandHalf + lcgF(crState)*bandHalf*2.0f, 0.01f, 0.95f);
        for (int k = 1; k < 5; k++) {
            pts[k].x = std::clamp(pts[k-1].x + (lcgF(crState)-0.5f)*0.65f, -0.82f, 0.82f);
            pts[k].y = std::min(pts[k-1].y + 0.05f + lcgF(crState)*0.10f, 0.97f);
        }
        // Threshold spread 0.04 -> 0.52 so all cracks are visible well before the beam fires
        DrawCrack(0.04f + float(ci) * (0.48f / float(NC - 1)), pts, 5);
    }

    // Phase 2: rising beam from apex (f 0.65 -> 0.88)
    if (f > 0.65f) {
        float bf    = std::clamp((f - 0.65f) / 0.23f, 0.0f, 1.0f);
        float beamH = bf * 85.0f * sc;
        float beamA = 0.65f + 0.28f*glow;
        ImVec2 beamTop = {c.x, apex.y - beamH};
        dl->AddLine(apex, beamTop, Col(col, beamA*0.10f), 18.0f);
        dl->AddLine(apex, beamTop, Col(col, beamA*0.28f),  8.0f);
        dl->AddLine(apex, beamTop, Col(col, beamA),         2.5f);
    }

    // Phase 3: blast rings + particles from apex (f > 0.86)
    if (f > 0.86f) {
        float bf = std::clamp((f - 0.72f) / 0.28f, 0.0f, 1.0f);
        for (int ri = 0; ri < 3; ri++) {
            float phase = fmodf(t*0.040f + ri*0.333f, 1.0f);
            float rr    = phase * 82.0f * sc;
            float ra    = (1.0f - phase) * bf * (0.55f + 0.22f*glow);
            if (ra > 0.02f) dl->AddCircle(apex, rr, Col(col, ra), 22, 2.0f);
        }
        for (int pi = 0; pi < 9; pi++) {
            float phase = fmodf(t*0.035f + pi*0.111f, 1.0f);
            float ang   = -kPI*0.5f + (pi - 4.0f)*0.22f;
            float dist  = phase * (52.0f + pi*7.0f) * sc;
            float pra   = (1.0f - phase) * bf * 0.78f;
            ImVec2 pp   = {apex.x + cosf(ang)*dist, apex.y + sinf(ang)*dist};
            dl->AddCircleFilled(pp, (1.4f + float(pi%3)*0.7f)*sc, Col(col, pra), 4);
        }
        float ba = bf * (0.28f + 0.18f*glow);
        dl->AddCircleFilled(apex, 7.0f*sc*bf, Col(col, ba*0.60f), 10);
    }
}

//
// ANIMATION 18 — TWIN MOONS
// Masser and Secunda in orbital dance — their lit faces grow with progress
//
static void Anim_TwinMoons(ImDrawList* dl, ImVec2 c, float sc,
                            float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float glow = 0.5f + 0.5f * sinf(t * 0.05f);

    float masserOrbitR  = 52.0f * sc;
    float secundaOrbitR = 82.0f * sc;
    float masserAngle   = t * 0.55f * DEG;
    float secundaAngle  = t * 1.05f * DEG + kPI * 0.42f;

    // Elliptic orbits (flattened to look 3D)
    ImVec2 masserPos  = { c.x + cosf(masserAngle)*masserOrbitR,
                          c.y + sinf(masserAngle)*masserOrbitR * 0.42f };
    ImVec2 secundaPos = { c.x + cosf(secundaAngle)*secundaOrbitR,
                          c.y + sinf(secundaAngle)*secundaOrbitR * 0.42f };

    float masserR  = 25.0f * sc;
    float secundaR = 14.0f * sc;

    // Orbit path guides
    dl->AddCircle(c, masserOrbitR, Col(col, 0.09f), 48, 1.0f);
    dl->AddEllipse(c, ImVec2{ secundaOrbitR, secundaOrbitR*0.42f },
                   Col(col, 0.09f), 0.0f, 48, 1.0f);

    // Masser (warm amber)
    dl->AddCircleFilled(masserPos, masserR, Col(0x887755, 0.80f), 32);
    float masserLit = 0.35f + 0.65f*f;
    Arc(dl, masserPos, masserR, -90.0f, -90.0f + 360.0f*masserLit,
        Col(col, 0.32f + 0.25f*glow), masserR * 0.75f);
    dl->AddCircle(masserPos, masserR, Col(col, 0.48f + 0.28f*f), 32, 2.0f);
    // Craters
    for (int i = 0; i < 3; i++) {
        float ca     = (i / 3.0f) * 2.0f * kPI + 0.6f;
        ImVec2 crPos = { masserPos.x + cosf(ca)*masserR*0.32f,
                         masserPos.y + sinf(ca)*masserR*0.32f };
        dl->AddCircle(crPos, masserR*0.12f, Col(0x665544, 0.60f), 8, 1.0f);
    }

    // Secunda (cool blue-silver)
    dl->AddCircleFilled(secundaPos, secundaR, Col(0x6688AA, 0.75f), 24);
    float secLit = 0.28f + 0.72f*f;
    Arc(dl, secundaPos, secundaR, -90.0f, -90.0f + 360.0f*secLit,
        Col(0x99BBDD, 0.30f + 0.22f*glow), secundaR * 0.70f);
    dl->AddCircle(secundaPos, secundaR, Col(0x99BBDD, 0.58f + 0.28f*f), 24, 1.5f);

    // Starfield background dots
    static float starAngles[8] = {0.3f,1.1f,1.9f,2.7f,3.5f,4.2f,5.0f,5.8f};
    for (int i = 0; i < 8; i++) {
        float sr = (55.0f + (i%4)*14.0f) * sc;
        float puls = 0.3f + 0.4f * sinf(t * 0.04f + i * 0.9f);
        dl->AddCircleFilled({ c.x + cosf(starAngles[i])*sr,
                              c.y + sinf(starAngles[i])*sr*0.5f },
                            1.5f*sc, Col(col, puls * f * 0.8f), 4);
    }
}

//
// ANIMATION 19 — BLIZZARD VORTEX
// Skyrim Storm Call shout aesthetic: snowflake particles spiral in a vortex,
// more particles and intensity as progress fills; ice shards fly outward late.
//
static void Anim_DaedricPortal(ImDrawList* dl, ImVec2 c, float sc,
                                float p, float t, uint32_t col) {
    float f    = p / 100.0f;
    float R    = 84.0f * sc;

    // Spiral snowflake particles — each traces a logarithmic-ish outward spiral
    int nFlakes = int(6 + 36.0f*f);
    nFlakes = std::min(nFlakes, 42);
    for (int i = 0; i < nFlakes; i++) {
        float fi     = float(i) / float(nFlakes);
        // Each flake orbits at a different radius and angular offset
        float r      = R * (0.10f + 0.90f*fi);
        float theta  = fi * 2.0f*kPI + t*(0.042f + fi*0.008f) * (f > 0.5f ? 1.35f : 1.0f);
        float puls   = 0.5f + 0.5f*sinf(t*0.10f + fi*2.8f);
        float alpha  = (0.25f + 0.60f*f) * puls;

        ImVec2 pos = {c.x + cosf(theta)*r, c.y + sinf(theta)*r*0.70f};

        // Tiny 6-arm snowflake
        float fs = (1.2f + i%3) * sc;
        for (int arm = 0; arm < 6; arm++) {
            float aa = arm*(kPI/3.0f) + theta*0.15f;
            dl->AddLine(pos,
                        {pos.x + cosf(aa)*fs*2.8f, pos.y + sinf(aa)*fs*2.8f},
                        Col(col, alpha*0.75f), 0.85f);
        }
        dl->AddCircleFilled(pos, fs, Col(col, alpha), 4);
    }

    // Vortex eye — dark calm centre
    float eyeR = R * (0.18f - 0.06f*f);
    dl->AddCircleFilled(c, eyeR, Col(0x020408, 0.90f), 16);
    dl->AddCircle(c, eyeR, Col(col, 0.38f + 0.28f*f), 16, 1.8f);

    // Ice shards fly outward as the storm peaks
    if (f > 0.58f) {
        float sf    = (f - 0.58f) / 0.42f;
        int nShards = int(sf * 14);
        for (int si = 0; si < nShards; si++) {
            float ang  = si*(2.0f*kPI/14.0f) + t*0.022f;
            float ph   = fmodf(t*0.048f + si*0.071f, 1.0f);
            float dist = (R*0.78f + ph*R*0.40f);
            ImVec2 tip  = {c.x + cosf(ang)*dist,         c.y + sinf(ang)*dist*0.70f};
            ImVec2 base = {c.x + cosf(ang)*(dist-13*sc), c.y + sinf(ang)*(dist-13*sc)*0.70f};
            float  sha  = (1.0f - ph) * sf * 0.70f;
            dl->AddLine(base, tip, Col(col, sha), 1.8f*sc);
        }
    }

    // Progress ring around the outer vortex
    float arcR = R + 9.0f*sc;
    if (f > 0.01f)
        Arc(dl, c, arcR, -90.0f, -90.0f + 360.0f*f, Col(col, 0.78f), 2.5f);
    Arc(dl, c, arcR, -90.0f + 360.0f*f, 270.0f, Col(col, 0.12f), 1.0f);
}

//
// CONFIG MENU  (press \ to toggle)
//

static float s_prevTick = 0.0f;

static void DrawConfigMenu() {
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Once, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660, 360), ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints(ImVec2(520, 280), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,      ImVec4(0.08f,0.08f,0.10f,0.95f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.16f,0.14f,0.08f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,        ImVec4(0.04f,0.04f,0.07f,1.0f));

    auto& cfg = Settings::GetSingleton();
    bool open = true;
    std::string winTitle = "Loading Progress  ( " + VKName(cfg.menuKey) + " closes )";
    if (ImGui::Begin(winTitle.c_str(), &open,
                     ImGuiWindowFlags_NoCollapse)) {

        if (ImGui::BeginTable("##layout", 2, 0)) {
            ImGui::TableSetupColumn("##ctrl",    ImGuiTableColumnFlags_WidthFixed, 350.0f);
            ImGui::TableSetupColumn("##preview", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();

            // Left column: controls
            ImGui::TableNextColumn();

            const char* styles[] = {
                "1  Circle Fill",    "2  Dragon Eye",     "3  Nordic Runes",
                "4  Waveform",       "5  Pixel Blocks",   "6  Orbit Dots",
                "7  Compass Rose",   "8  Helix Spiral",   "9  Snowflake",
                "10 Progress Bar",   "11 Alchemy Cauldron","12 Dwemer Cogs",
                "13 Aurora Borealis","14 Constellation",  "15 Dragon Scales",
                "16 Enchantment",    "17 Word Wall",      "18 Standing Stone",
                "19 Twin Moons",     "20 Blizzard Vortex"
            };
            ImGui::Checkbox("Random animation each load", &cfg.randomStyle);
            ImGui::SetNextItemWidth(270);
            if (cfg.randomStyle) ImGui::BeginDisabled();
            ImGui::Combo("Animation", &cfg.animStyle, styles, 20);
            if (cfg.randomStyle) ImGui::EndDisabled();

            const char* positions[] = {
                "Bottom-Right (default)", "Bottom-Left",
                "Bottom-Center",          "Top-Right", "Top-Left"
            };
            ImGui::SetNextItemWidth(255);
            ImGui::Combo("Position", &cfg.position, positions, 5);

            ImGui::SetNextItemWidth(255);
            ImGui::SliderFloat("Scale", &cfg.scale, 0.2f, 3.0f, "%.2f");

            ImGui::SetNextItemWidth(255);
            ImGui::SliderFloat("Opacity", &cfg.overlayAlpha, 0.1f, 1.0f, "%.2f");

            ImGui::Checkbox("Show Percentage", &cfg.showPercent);

            float rgb[3] = {
                ((cfg.color >> 16) & 0xFF) / 255.0f,
                ((cfg.color >>  8) & 0xFF) / 255.0f,
                ( cfg.color        & 0xFF) / 255.0f
            };
            if (ImGui::ColorEdit3("Colour", rgb)) {
                cfg.color = 0xFF000000u
                    | (static_cast<uint32_t>(rgb[0]*255.0f+0.5f) << 16)
                    | (static_cast<uint32_t>(rgb[1]*255.0f+0.5f) <<  8)
                    |  static_cast<uint32_t>(rgb[2]*255.0f+0.5f);
            }

            ImGui::Separator();

            // Key rebinding
            ImGui::Text("Menu key:");
            ImGui::SameLine();
            if (g_capturingMenuKey) {
                ImGui::Button("Press any key...", ImVec2(200, 0));
                // Escape cancels
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    g_capturingMenuKey = false;
                    g_prevMenuKey = true;
                } else {
                    for (int vk = 0x08; vk <= 0xFE; vk++) {
                        if (vk == VK_ESCAPE  || vk == VK_LBUTTON ||
                            vk == VK_RBUTTON || vk == VK_MBUTTON) continue;
                        if (GetAsyncKeyState(vk) & 0x8000) {
                            cfg.menuKey       = vk;
                            g_capturingMenuKey = false;
                            g_prevMenuKey      = true; // prevent immediate retrigger
                            break;
                        }
                    }
                }
            } else {
                std::string label = VKName(cfg.menuKey) + "  (click to rebind)";
                if (ImGui::Button(label.c_str(), ImVec2(200, 0)))
                    g_capturingMenuKey = true;
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Changes are live and saved automatically.");
            ImGui::Spacing();

            if (ImGui::Button("Close")) { cfg.Save(); g_cfgOpen = false; }

            // Right column: live preview
            ImGui::TableNextColumn();

            s_prevTick += 1.0f;
            float    prevP  = 50.0f + 50.0f * sinf(s_prevTick * 0.015f);
            uint32_t col    = cfg.color & 0x00FFFFFFu;

            ImGui::BeginChild("##canvas", ImGui::GetContentRegionAvail(), true,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ImVec2 cMin = ImGui::GetCursorScreenPos();
            ImVec2 cSz  = ImGui::GetContentRegionAvail();
            ImVec2 ctr  = { cMin.x + cSz.x*0.5f, cMin.y + cSz.y*0.5f };

            ImDrawList* pdl = ImGui::GetWindowDrawList();

            // Scale: linear bar and word wall are very wide — use smaller sc
            float prevSc = (cfg.animStyle == 9)
                ? 0.28f
                : cSz.y / 240.0f;

            // Preview renders at full opacity
            float savedAlpha = g_animAlpha;
            g_animAlpha = 1.0f;

            // Re-roll random seeds when style changes or every ~5 seconds (300 ticks)
            static int   s_previewLastStyle = -1;
            static float s_previewRerollAt  = -1.0f;
            if (cfg.animStyle != s_previewLastStyle || s_prevTick >= s_previewRerollAt) {
                s_previewLastStyle = cfg.animStyle;
                s_previewRerollAt  = s_prevTick + 300.0f;
                s_constPattern = rand();
                s_wallSeed     = rand();
                s_crackSeed    = rand();
                s_mountainSeed = rand();
            }

            switch (cfg.animStyle) {
                case 0:  Anim_CircleFill   (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 1:  Anim_DragonEye    (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 2:  Anim_NordicRunes  (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 3:  Anim_Waveform     (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 4:  Anim_PixelBlocks  (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 5:  Anim_OrbitDots    (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 6:  Anim_CompassRose  (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 7:  Anim_Helix        (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 8:  Anim_Snowflake    (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 9:  Anim_LinearBar    (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 10: Anim_SoulGem      (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 11: Anim_DwemerCogs   (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 12: Anim_Shout        (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 13: Anim_Constellation(pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 14: Anim_DragonScales (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 15: Anim_Enchantment  (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 16: Anim_WordWall     (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 17: Anim_StandingStone(pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                case 18: Anim_TwinMoons    (pdl, ctr, prevSc, prevP, s_prevTick, col); break;
                default: Anim_DaedricPortal(pdl, ctr, prevSc, prevP, s_prevTick, col); break;
            }

            g_animAlpha = savedAlpha;

            // Percentage readout
            char pBuf[8];
            snprintf(pBuf, sizeof(pBuf), "%d%%", static_cast<int>(prevP));
            ImVec2 pts = ImGui::CalcTextSize(pBuf);
            pdl->AddText(nullptr, 13.0f,
                { ctr.x - pts.x*0.5f, cMin.y + cSz.y - 18.0f },
                IM_COL32(255,255,255,140), pBuf);

            ImGui::Dummy(cSz);
            ImGui::EndChild();

            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(3);

    if (!open) { Settings::GetSingleton().Save(); g_cfgOpen = false; }
}

//
// MAIN OVERLAY DRAW  (loading animation)
//

static void DrawOverlay() {
    auto& tracker = ProgressTracker::GetSingleton();
    auto  phase   = tracker.GetPhase();
    auto& cfg     = Settings::GetSingleton();

    // Sync active style when idle (no load in progress) and random mode is off
    if (!s_loading && !s_draining && !cfg.randomStyle)
        s_activeStyle = cfg.animStyle;

    // START
    if (!s_loading && !s_draining && phase == LoadPhase::Tracking) {
        s_loading       = true;
        s_tick          = 0;
        s_display       = 0.0f;
        s_start         = std::chrono::steady_clock::now();
        s_blockOrder.clear();
        s_loadIndex++;
        srand(static_cast<unsigned>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                s_start.time_since_epoch()).count()));
        s_constPattern = rand();
        s_wallSeed     = rand();
        s_crackSeed    = rand();
        s_mountainSeed = rand();
        s_activeStyle  = cfg.randomStyle ? (rand() % 20) : cfg.animStyle;
        logger::info("D3DOverlay: load animation started (#{:d}) style={:d}{}",
                     s_loadIndex, s_activeStyle, cfg.randomStyle ? " (random)" : "");
    }

    // DRAIN TRIGGER
    // kPostLoadGame (Complete) fires only for save-game loads.
    // For all cell transitions the only signal is the menu closing (Tracking→Idle).
    // Either way: switch to drain mode and fill proportionally to 100%,
    // which completes during the loading-screen fade-out.
    if (s_loading && !s_draining) {
        bool byEvent    = (phase == LoadPhase::Complete);
        bool byMenuClose = (s_prevPhase == LoadPhase::Tracking
                            && phase     == LoadPhase::Idle);
        if (byEvent || byMenuClose) {
            if (byMenuClose) {
                // Cell transitions never fire kPostLoadGame, so record bytes here.
                tracker.OnLoadComplete();
            }
            s_draining = true;
            logger::info("D3DOverlay: draining to 100% ({})",
                byEvent ? "kPostLoadGame" : "menu closed");
        }
    }
    s_prevPhase = phase;

    // EARLY EXIT
    if (!s_loading && !s_draining) {
        s_display = 0.0f;
        return;
    }

    s_tick++;
    float elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - s_start).count();

    if (s_draining) {
        float remaining = 100.0f - s_display;
        s_display = std::min(s_display + std::max(0.8f, remaining * 0.05f), 100.0f);
        if (s_display >= 100.0f) {
            s_loading  = false;
            s_draining = false;
            // Persist calibration data on a background thread — SaveCache does
            // disk I/O (ini.LoadFile + ini.SaveFile) which must not block Present.
            LONGLONG actual = tracker.GetLastLoadBytes();
            if (actual > 0) {
                cfg.lastStreamCount = static_cast<uint64_t>(actual);
                std::thread([] { Settings::GetSingleton().SaveCache(); }).detach();
            }
            // Do NOT return — let the draw happen at 100% this frame.
            // Next frame: !s_loading && !s_draining -> early exit + s_display reset.
        }
    } else {
        // Real I/O byte progress from the ReadFile hook.
        // GetProgress() returns 0–99 based on bytes_this_load / bytes_last_load.
        // If no calibration data exists yet (first ever launch), it returns 0
        // and we fall back to a conservative time curve capped at 60%.
        float rawProgress = tracker.GetProgress();

        if (rawProgress > 0.5f) {
            // Calibrated: lerp smoothly toward the real ratio.
            // Never go backwards; cap step to avoid jarring jumps.
            float gap = rawProgress - s_display;
            if (gap > 0.0f)
                s_display += std::min(gap * 0.10f, 2.5f);
        } else {
            // No calibration: gentle time curve, hard cap at 60%.
            // Drain fills the rest once the load-complete signal arrives.
            float tau    = 2.0f + elapsed * 0.15f;
            float target = 60.0f * (1.0f - expf(-elapsed / tau));
            float gap    = target - s_display;
            if (gap > 0.0f)
                s_display += std::min(gap * 0.35f, 1.5f);
        }
    }

    // Position
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    float  sc     = cfg.scale * (screen.y / 720.0f);
    float  margin = 160.0f * sc;

    ImVec2 centre;
    switch (cfg.position) {
        case 1:  centre = { margin,               screen.y - margin }; break;
        case 2:  centre = { screen.x * 0.5f,      screen.y - margin }; break;
        case 3:  centre = { screen.x - margin,    margin            }; break;
        case 4:  centre = { margin,               margin            }; break;
        default: centre = { screen.x - margin,    screen.y - margin }; break;
    }

    ImDrawList* dl  = ImGui::GetBackgroundDrawList();
    uint32_t    col = cfg.color & 0x00FFFFFFu;

    // Apply opacity from settings
    g_animAlpha = cfg.overlayAlpha;

    switch (s_activeStyle) {
        case 0:  Anim_CircleFill   (dl, centre, sc, s_display, float(s_tick), col); break;
        case 1:  Anim_DragonEye    (dl, centre, sc, s_display, float(s_tick), col); break;
        case 2:  Anim_NordicRunes  (dl, centre, sc, s_display, float(s_tick), col); break;
        case 3:  Anim_Waveform     (dl, centre, sc, s_display, float(s_tick), col); break;
        case 4:  Anim_PixelBlocks  (dl, centre, sc, s_display, float(s_tick), col); break;
        case 5:  Anim_OrbitDots    (dl, centre, sc, s_display, float(s_tick), col); break;
        case 6:  Anim_CompassRose  (dl, centre, sc, s_display, float(s_tick), col); break;
        case 7:  Anim_Helix        (dl, centre, sc, s_display, float(s_tick), col); break;
        case 8:  Anim_Snowflake    (dl, centre, sc, s_display, float(s_tick), col); break;
        case 9:  Anim_LinearBar    (dl, centre, sc, s_display, float(s_tick), col); break;
        case 10: Anim_SoulGem      (dl, centre, sc, s_display, float(s_tick), col); break;
        case 11: Anim_DwemerCogs   (dl, centre, sc, s_display, float(s_tick), col); break;
        case 12: Anim_Shout        (dl, centre, sc, s_display, float(s_tick), col); break;
        case 13: Anim_Constellation(dl, centre, sc, s_display, float(s_tick), col); break;
        case 14: Anim_DragonScales (dl, centre, sc, s_display, float(s_tick), col); break;
        case 15: Anim_Enchantment  (dl, centre, sc, s_display, float(s_tick), col); break;
        case 16: Anim_WordWall     (dl, centre, sc, s_display, float(s_tick), col); break;
        case 17: Anim_StandingStone(dl, centre, sc, s_display, float(s_tick), col); break;
        case 18: Anim_TwinMoons    (dl, centre, sc, s_display, float(s_tick), col); break;
        default: Anim_DaedricPortal(dl, centre, sc, s_display, float(s_tick), col); break;
    }

    g_animAlpha = 1.0f;

    // Percentage label
    if (cfg.showPercent) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(s_display));
        float fontSize = (std::max)(12.0f, 18.0f * sc);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        float ty = (std::min)(centre.y + 96*sc + 4.0f, screen.y - fontSize - 4.0f);
        ImVec2 tp { centre.x - ts.x * 0.5f, ty };
        dl->AddText(nullptr, fontSize, tp,
                    IM_COL32(255,255,255,static_cast<int>(220*cfg.overlayAlpha)), buf);
    }
}

//
// PRESENT HOOK
//

static HRESULT WINAPI Hook_Present(IDXGISwapChain* chain, UINT sync, UINT flags) {
    if (!g_initialized) {
        ID3D11Device*        device  = nullptr;
        ID3D11DeviceContext* context = nullptr;
        chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
        if (device) {
            device->GetImmediateContext(&context);

            DXGI_SWAP_CHAIN_DESC desc{};
            chain->GetDesc(&desc);
            g_hwnd   = desc.OutputWindow;
            g_width  = desc.BufferDesc.Width  ? desc.BufferDesc.Width  : 1280;
            g_height = desc.BufferDesc.Height ? desc.BufferDesc.Height : 720;

            g_ctx = ImGui::CreateContext();
            ImGui::SetCurrentContext(g_ctx);

            ImGuiIO& io    = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;

            ImGui_ImplDX11_Init(device, context);
            ImGui::StyleColorsDark();
            ImGui::GetStyle().WindowRounding = 6.0f;
            ImGui::GetStyle().FrameRounding  = 4.0f;

            device ->Release();
            context->Release();

            g_initialized = true;
            logger::info("D3DOverlay: ImGui initialised ({}x{})", g_width, g_height);
        }
    }

    if (!g_initialized) return orig_Present(chain, sync, flags);

    ImGuiContext* prevCtx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ctx);

    {
        DXGI_SWAP_CHAIN_DESC desc{};
        chain->GetDesc(&desc);
        if (desc.BufferDesc.Width)  g_width  = desc.BufferDesc.Width;
        if (desc.BufferDesc.Height) g_height = desc.BufferDesc.Height;
    }

    // Toggle key — configurable, default backslash. Skipped while rebinding.
    auto& cfg = Settings::GetSingleton();
    if (!g_capturingMenuKey) {
        bool curKey = (GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0;
        if (curKey && !g_prevMenuKey) g_cfgOpen = !g_cfgOpen;
        g_prevMenuKey = curKey;
    }

    // Cursor management: clip mouse to window while config menu is open.
    // We do NOT touch ShowCursor — its reference counter is owned by the game.
    // ImGui renders a software cursor via io.MouseDrawCursor instead.
    static bool s_cursorClipped = false;
    if (g_cfgOpen && !s_cursorClipped && g_hwnd) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        POINT p1 = { rc.left,  rc.top    };
        POINT p2 = { rc.right, rc.bottom };
        ClientToScreen(g_hwnd, &p1);
        ClientToScreen(g_hwnd, &p2);
        RECT screenRc = { p1.x, p1.y, p2.x, p2.y };
        ClipCursor(&screenRc);
        s_cursorClipped = true;
    } else if (!g_cfgOpen && s_cursorClipped) {
        ClipCursor(nullptr);
        s_cursorClipped = false;
    }

    // Manual IO update
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_width), static_cast<float>(g_height));
    io.DeltaTime   = 1.0f / 60.0f;

    io.MouseDrawCursor = g_cfgOpen;

    if (g_cfgOpen && g_hwnd) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(g_hwnd, &pt);
        io.MousePos     = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    } else {
        io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    }

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    DrawOverlay();
    if (g_cfgOpen) DrawConfigMenu();

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    ImGui::SetCurrentContext(prevCtx);

    return orig_Present(chain, sync, flags);
}

//
// D3D11CreateDeviceAndSwapChain HOOK
//

static HRESULT WINAPI Hook_CreateDev(
    IDXGIAdapter*              adapter,
    D3D_DRIVER_TYPE            driverType,
    HMODULE                    software,
    UINT                       flags,
    const D3D_FEATURE_LEVEL*   pFeatureLevels,
    UINT                       numFeatureLevels,
    UINT                       sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* pDesc,
    IDXGISwapChain**           ppSwapChain,
    ID3D11Device**             ppDevice,
    D3D_FEATURE_LEVEL*         pFeatureLevel,
    ID3D11DeviceContext**      ppImmediateContext)
{
    HRESULT hr = orig_CreateDev(adapter, driverType, software, flags,
        pFeatureLevels, numFeatureLevels, sdkVersion,
        pDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);

    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain && !g_presentHooked) {
        void* presentFn = (*reinterpret_cast<void***>(*ppSwapChain))[8];
        if (MH_CreateHook(presentFn, &Hook_Present,
                          reinterpret_cast<LPVOID*>(&orig_Present)) == MH_OK &&
            MH_EnableHook(presentFn) == MH_OK) {
            g_presentHooked = true;
            logger::info("D3DOverlay: Present hooked from real swap chain");
        } else {
            logger::error("D3DOverlay: failed to hook Present");
        }
    }

    return hr;
}

//
// INSTALL
//

bool D3DOverlay::Install() {
    LPVOID targetFn = nullptr;
    MH_STATUS st = MH_CreateHookApiEx(
        L"d3d11", "D3D11CreateDeviceAndSwapChain",
        &Hook_CreateDev,
        reinterpret_cast<LPVOID*>(&orig_CreateDev),
        &targetFn);

    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        logger::error("D3DOverlay: MH_CreateHookApiEx(CreateDeviceAndSwapChain) failed: {}",
                      static_cast<int>(st));
        return false;
    }

    MH_EnableHook(MH_ALL_HOOKS);
    logger::info("D3DOverlay: D3D11CreateDeviceAndSwapChain hooked — waiting for game device");
    return true;
}
