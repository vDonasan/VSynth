/*
 * rlsoft: desenho em software do subconjunto do raylib usado pela interface.
 * Ver raylib.h nesta pasta. Formas com antisserrilhado por distância com
 * sinal (cobertura = 0,5 - distância), texto com stb_truetype.
 */
#include "raylib.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#ifdef _WIN32
#include <direct.h>
/* sem as partes gráficas do Windows: elas têm Rectangle, DrawTextEx... */
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif

/* ------------------------------------------------------------------ */
/* Contexto                                                            */
/* ------------------------------------------------------------------ */
#define MAX_TEX 8

typedef struct { unsigned char *px; int w, h; } Surface;

struct RlContext {
    int   win_w, win_h;
    float ratio;
    /* entrada (vinda da plataforma) */
    float mx, my;
    bool  mdown[3], fdown[3], mpress_l[3], mrel_l[3], mpressed[3], mreleased[3];
    float wheel_acc, wheel;
    bool  kdown[RL_MAX_KEYS], kprev[RL_MAX_KEYS], kpress_l[RL_MAX_KEYS], krel_l[RL_MAX_KEYS], krep_l[RL_MAX_KEYS];
    bool  kpressed[RL_MAX_KEYS], kreleased[RL_MAX_KEYS], krepeat[RL_MAX_KEYS];
    unsigned chars_in[32], chars[32];
    int   nchars_in, nchars, char_pos;
    float moff_x, moff_y, msc_x, msc_y;
    int   cursor;
    double last_input; /* para desenhar menos quando ninguém mexe */
    unsigned ticks;
    /* desenho */
    Surface fb;              /* janela, em pixels de verdade */
    Surface tex[MAX_TEX];    /* render textures (id = índice + 1) */
    Surface *target;
    float   zoom;            /* BeginMode2D */
    int     in_drawing;
};

static RlContext *C = NULL;

RlContext *rls_create(void)
{
    RlContext *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->ratio = 1.0f;
    c->msc_x = c->msc_y = 1.0f;
    c->zoom = 1.0f;
    return c;
}

void rls_destroy(RlContext *c)
{
    if (!c) return;
    free(c->fb.px);
    for (int i = 0; i < MAX_TEX; i++) free(c->tex[i].px);
    if (C == c) C = NULL;
    free(c);
}

RlContext *rls_bind(RlContext *c)
{
    RlContext *o = C;
    C = c;
    return o;
}

void rls_set_window(RlContext *c, int w, int h, float ratio)
{
    c->win_w = w > 1 ? w : 1;
    c->win_h = h > 1 ? h : 1;
    c->ratio = ratio > 0.25f ? ratio : 1.0f;
}

void rls_mouse_move(RlContext *c, float x, float y)
{
    if (x != c->mx || y != c->my) c->last_input = GetTime();
    c->mx = x;
    c->my = y;
}
void rls_mouse_button(RlContext *c, int b, bool down)
{
    if (b < 0 || b > 2) return;
    c->last_input = GetTime();
    if (down && !c->mdown[b]) c->mpress_l[b] = true;
    if (!down && c->mdown[b]) c->mrel_l[b] = true;
    c->mdown[b] = down;
}
void rls_mouse_wheel(RlContext *c, float dy) { c->wheel_acc += dy; c->last_input = GetTime(); }
void rls_key(RlContext *c, int k, bool down)
{
    if (k <= 0 || k >= RL_MAX_KEYS) return;
    c->last_input = GetTime();
    if (down) { if (c->kdown[k]) c->krep_l[k] = true; else c->kpress_l[k] = true; }
    else if (c->kdown[k]) c->krel_l[k] = true;
    c->kdown[k] = down;
}
void rls_char(RlContext *c, unsigned cp)
{
    if (c->nchars_in < 32) c->chars_in[c->nchars_in++] = cp;
}

void rls_new_frame(RlContext *c)
{
    for (int b = 0; b < 3; b++) {
        c->mpressed[b] = c->mpress_l[b];
        if (c->mpress_l[b] && c->mrel_l[b]) {
            /* apertou e soltou no mesmo quadro (clique rápido): neste quadro
               o botão está apertado; a soltura fica para o próximo */
            c->mreleased[b] = false;
            c->fdown[b] = true;
            c->mpress_l[b] = false;
        } else {
            c->mreleased[b] = c->mrel_l[b];
            c->fdown[b] = c->mdown[b];
            c->mpress_l[b] = c->mrel_l[b] = false;
        }
    }
    for (int k = 0; k < RL_MAX_KEYS; k++) {
        c->kpressed[k] = c->kpress_l[k];
        c->kreleased[k] = c->krel_l[k];
        c->krepeat[k] = c->krep_l[k];
        c->kpress_l[k] = c->krel_l[k] = c->krep_l[k] = false;
    }
    c->wheel = c->wheel_acc;
    c->wheel_acc = 0;
    memcpy(c->chars, c->chars_in, sizeof c->chars);
    c->nchars = c->nchars_in;
    c->nchars_in = 0;
    c->char_pos = 0;
}

bool rls_should_draw(RlContext *c)
{
    c->ticks++;
    if (!c->fb.px || GetTime() - c->last_input < 2.0) return true; /* mexendo: todo quadro */
    return c->ticks % 3 == 0;                                       /* parado: 1 em 3 (~10 por segundo) */
}

const unsigned char *rls_framebuffer(RlContext *c, int *w, int *h)
{
    *w = c->fb.w;
    *h = c->fb.h;
    return c->fb.px;
}
int rls_cursor(RlContext *c) { return c->cursor; }

/* ------------------------------------------------------------------ */
/* Tempo, janela, entrada                                              */
/* ------------------------------------------------------------------ */
double GetTime(void)
{
#ifdef _WIN32
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
#else
    static double t0 = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 < 0) t0 = t;
    return t - t0;
#endif
}

int GetScreenWidth(void) { return C ? C->win_w : 0; }
int GetScreenHeight(void) { return C ? C->win_h : 0; }
int GetRenderWidth(void) { return C ? (int)lroundf(C->win_w * C->ratio) : 0; }
int GetRenderHeight(void) { return C ? (int)lroundf(C->win_h * C->ratio) : 0; }

Vector2 GetMousePosition(void)
{
    return (Vector2){(C->mx + C->moff_x) * C->msc_x, (C->my + C->moff_y) * C->msc_y};
}
bool  IsMouseButtonDown(int b) { return b >= 0 && b < 3 && C->fdown[b]; }
bool  IsMouseButtonPressed(int b) { return b >= 0 && b < 3 && C->mpressed[b]; }
bool  IsMouseButtonReleased(int b) { return b >= 0 && b < 3 && C->mreleased[b]; }
float GetMouseWheelMove(void) { return C->wheel; }
void  SetMouseOffset(int x, int y) { C->moff_x = (float)x; C->moff_y = (float)y; }
void  SetMouseScale(float sx, float sy) { C->msc_x = sx; C->msc_y = sy; }
void  SetMouseCursor(int c) { C->cursor = c; }
bool  IsKeyDown(int k) { return k > 0 && k < RL_MAX_KEYS && C->kdown[k]; }
bool  IsKeyPressed(int k) { return k > 0 && k < RL_MAX_KEYS && C->kpressed[k]; }
bool  IsKeyPressedRepeat(int k) { return k > 0 && k < RL_MAX_KEYS && C->krepeat[k]; }
bool  IsKeyReleased(int k) { return k > 0 && k < RL_MAX_KEYS && C->kreleased[k]; }
int   GetCharPressed(void) { return C->char_pos < C->nchars ? (int)C->chars[C->char_pos++] : 0; }

/* ------------------------------------------------------------------ */
/* Superfícies                                                         */
/* ------------------------------------------------------------------ */
static void surface_size(Surface *s, int w, int h)
{
    if (s->px && s->w == w && s->h == h) return;
    free(s->px);
    s->px = calloc((size_t)w * h, 4);
    s->w = s->px ? w : 0;
    s->h = s->px ? h : 0;
}

/* escala de coordenadas de desenho para pixels do alvo atual */
static float T(void)
{
    float s = C->zoom;
    if (C->target == &C->fb) s *= C->ratio;
    return s;
}

void ClearBackground(Color c)
{
    Surface *s = C->target;
    if (!s || !s->px) return;
    uint32_t v = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | ((uint32_t)255 << 24);
    uint32_t *p = (uint32_t *)s->px;
    size_t n = (size_t)s->w * s->h;
    for (size_t i = 0; i < n; i++) p[i] = v;
}

void BeginDrawing(void)
{
    surface_size(&C->fb, GetRenderWidth(), GetRenderHeight());
    C->target = &C->fb;
    C->zoom = 1.0f;
    C->in_drawing = 1;
}
void EndDrawing(void) { C->in_drawing = 0; C->target = NULL; }
void BeginMode2D(Camera2D cam) { C->zoom = cam.zoom > 0 ? cam.zoom : 1.0f; }
void EndMode2D(void) { C->zoom = 1.0f; }

RenderTexture2D LoadRenderTexture(int w, int h)
{
    RenderTexture2D r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < MAX_TEX; i++)
        if (!C->tex[i].px) {
            surface_size(&C->tex[i], w, h);
            r.id = (unsigned)i + 1;
            r.texture.id = r.id;
            r.texture.width = w;
            r.texture.height = h;
            break;
        }
    return r;
}
void UnloadRenderTexture(RenderTexture2D t)
{
    if (t.id < 1 || t.id > MAX_TEX) return;
    Surface *s = &C->tex[t.id - 1];
    if (C->target == s) C->target = NULL;
    free(s->px);
    memset(s, 0, sizeof *s);
}
void BeginTextureMode(RenderTexture2D t)
{
    C->target = (t.id >= 1 && t.id <= MAX_TEX) ? &C->tex[t.id - 1] : NULL;
    C->zoom = 1.0f;
}
void EndTextureMode(void) { C->target = C->in_drawing ? &C->fb : NULL; C->zoom = 1.0f; }
void SetTextureFilter(Texture2D t, int f) { (void)t; (void)f; }

/* cópia (com escala bilinear) de uma textura para o alvo; a altura
   negativa do raylib é a inversão do OpenGL, que aqui não existe */
void DrawTexturePro(Texture2D t, Rectangle src, Rectangle dst, Vector2 origin, float rot, Color tint)
{
    (void)origin; (void)rot; (void)tint;
    if (t.id < 1 || t.id > MAX_TEX || !C->target) return;
    Surface *s = &C->tex[t.id - 1], *d = C->target;
    if (!s->px || !d->px) return;
    float k = T();
    float sx0 = src.x, sy0 = src.y, sw = fabsf(src.width), sh = fabsf(src.height);
    int dx0 = (int)lroundf(dst.x * k), dy0 = (int)lroundf(dst.y * k);
    int dw = (int)lroundf(dst.width * k), dh = (int)lroundf(dst.height * k);
    if (dw <= 0 || dh <= 0) return;
    if (dw == (int)sw && dh == (int)sh) { /* 1:1 */
        for (int y = 0; y < dh; y++) {
            int ty = dy0 + y;
            if (ty < 0 || ty >= d->h) continue;
            int x0 = dx0 < 0 ? -dx0 : 0, x1 = dx0 + dw > d->w ? d->w - dx0 : dw;
            if (x1 <= x0) continue;
            memcpy(d->px + ((size_t)ty * d->w + dx0 + x0) * 4, s->px + ((size_t)(sy0 + y) * s->w + (int)sx0 + x0) * 4, (size_t)(x1 - x0) * 4);
        }
        return;
    }
    for (int y = 0; y < dh; y++) {
        int ty = dy0 + y;
        if (ty < 0 || ty >= d->h) continue;
        float fy = sy0 + (y + 0.5f) * sh / dh - 0.5f;
        int y0 = (int)floorf(fy);
        float wy = fy - y0;
        int ya = y0 < 0 ? 0 : (y0 >= s->h ? s->h - 1 : y0), yb = y0 + 1 >= s->h ? s->h - 1 : (y0 + 1 < 0 ? 0 : y0 + 1);
        for (int x = 0; x < dw; x++) {
            int tx = dx0 + x;
            if (tx < 0 || tx >= d->w) continue;
            float fx = sx0 + (x + 0.5f) * sw / dw - 0.5f;
            int x0 = (int)floorf(fx);
            float wx = fx - x0;
            int xa = x0 < 0 ? 0 : (x0 >= s->w ? s->w - 1 : x0), xb = x0 + 1 >= s->w ? s->w - 1 : (x0 + 1 < 0 ? 0 : x0 + 1);
            const unsigned char *p00 = s->px + ((size_t)ya * s->w + xa) * 4, *p01 = s->px + ((size_t)ya * s->w + xb) * 4;
            const unsigned char *p10 = s->px + ((size_t)yb * s->w + xa) * 4, *p11 = s->px + ((size_t)yb * s->w + xb) * 4;
            unsigned char *o = d->px + ((size_t)ty * d->w + tx) * 4;
            for (int c = 0; c < 3; c++) {
                float a = p00[c] + (p01[c] - p00[c]) * wx, b = p10[c] + (p11[c] - p10[c]) * wx;
                o[c] = (unsigned char)(a + (b - a) * wy + 0.5f);
            }
            o[3] = 255;
        }
    }
}

Image LoadImageFromTexture(Texture2D t)
{
    Image img;
    memset(&img, 0, sizeof img);
    if (t.id < 1 || t.id > MAX_TEX) return img;
    Surface *s = &C->tex[t.id - 1];
    img.data = malloc((size_t)s->w * s->h * 4);
    if (!img.data) return img;
    img.width = s->w;
    img.height = s->h;
    /* o raylib devolve de cabeça para baixo (OpenGL) e o chamador inverte */
    for (int y = 0; y < s->h; y++)
        memcpy((unsigned char *)img.data + (size_t)y * s->w * 4, s->px + (size_t)(s->h - 1 - y) * s->w * 4, (size_t)s->w * 4);
    return img;
}
void ImageFlipVertical(Image *img)
{
    if (!img->data) return;
    size_t row = (size_t)img->width * 4;
    unsigned char *tmp = malloc(row);
    if (!tmp) return;
    for (int y = 0; y < img->height / 2; y++) {
        unsigned char *a = (unsigned char *)img->data + y * row, *b = (unsigned char *)img->data + (img->height - 1 - y) * row;
        memcpy(tmp, a, row); memcpy(a, b, row); memcpy(b, tmp, row);
    }
    free(tmp);
}
bool ExportImage(Image img, const char *path)
{
    return img.data && stbi_write_png(path, img.width, img.height, 4, img.data, img.width * 4) != 0;
}
void UnloadImage(Image img) { free(img.data); }

/* ------------------------------------------------------------------ */
/* Formas                                                              */
/* ------------------------------------------------------------------ */
static inline void blend(Surface *s, int x, int y, Color c, float cov)
{
    if (cov <= 0.0f) return;
    unsigned a = (unsigned)(c.a * (cov > 1.0f ? 1.0f : cov) + 0.5f);
    if (!a) return;
    unsigned char *p = s->px + ((size_t)y * s->w + x) * 4;
    if (a >= 255) { p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255; return; }
    unsigned ia = 255 - a;
    p[0] = (unsigned char)((c.r * a + p[0] * ia + 127) / 255);
    p[1] = (unsigned char)((c.g * a + p[1] * ia + 127) / 255);
    p[2] = (unsigned char)((c.b * a + p[2] * ia + 127) / 255);
    p[3] = 255;
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* caixa de pixels que cobre [x0,x1)x[y0,y1) (já em pixels), cortada no alvo */
static bool bbox(Surface *s, float x0, float y0, float x1, float y1, int *ix0, int *iy0, int *ix1, int *iy1)
{
    *ix0 = (int)floorf(x0); *iy0 = (int)floorf(y0);
    *ix1 = (int)ceilf(x1);  *iy1 = (int)ceilf(y1);
    if (*ix0 < 0) *ix0 = 0;
    if (*iy0 < 0) *iy0 = 0;
    if (*ix1 > s->w) *ix1 = s->w;
    if (*iy1 > s->h) *iy1 = s->h;
    return *ix1 > *ix0 && *iy1 > *iy0;
}

/* distância com sinal até um retângulo de cantos redondos (em pixels) */
static inline float sd_rbox(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static void fill_rbox(float x, float y, float w, float h, float r, Color c)
{
    Surface *s = C->target;
    if (!s || !s->px || c.a == 0 || w <= 0 || h <= 0) return;
    float k = T();
    x *= k; y *= k; w *= k; h *= k; r *= k;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int x0, y0, x1, y1;
    if (!bbox(s, x - 1, y - 1, x + w + 1, y + h + 1, &x0, &y0, &x1, &y1)) return;
    float cx = x + w / 2, cy = y + h / 2, hw = w / 2, hh = h / 2;
    /* miolo (pixels inteiramente dentro, longe dos cantos): cor chapada,
       linha por linha; só as bordas e os cantos pedem a conta da distância */
    int ix0 = (int)ceilf(x), ix1 = (int)floorf(x + w);
    int iy0 = (int)ceilf(y + r), iy1 = (int)floorf(y + h - r);
    if (ix0 < x0) ix0 = x0;
    if (ix1 > x1) ix1 = x1;
    uint32_t solid = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | 0xff000000u;
    unsigned a = c.a, ia = 255 - a;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        bool mid = py >= iy0 && py < iy1 && ix1 > ix0;
        int ex0 = mid ? ix0 : x1, ex1 = mid ? ix1 : x1;
        for (int px = x0; px < ex0; px++)
            blend(s, px, py, c, clamp01(0.5f - sd_rbox(px + 0.5f, fy, cx, cy, hw, hh, r)));
        if (mid) {
            uint32_t *row = (uint32_t *)s->px + (size_t)py * s->w;
            if (a == 255) for (int px = ex0; px < ex1; px++) row[px] = solid;
            else {
                unsigned char *q = (unsigned char *)(row + ex0);
                unsigned cr = c.r * a + 127, cg = c.g * a + 127, cb = c.b * a + 127;
                for (int px = ex0; px < ex1; px++, q += 4) {
                    q[0] = (unsigned char)((cr + q[0] * ia) / 255);
                    q[1] = (unsigned char)((cg + q[1] * ia) / 255);
                    q[2] = (unsigned char)((cb + q[2] * ia) / 255);
                }
            }
        }
        for (int px = ex1; px < x1; px++)
            blend(s, px, py, c, clamp01(0.5f - sd_rbox(px + 0.5f, fy, cx, cy, hw, hh, r)));
    }
}

void DrawRectangle(int x, int y, int w, int h, Color c) { fill_rbox((float)x, (float)y, (float)w, (float)h, 0, c); }
void DrawRectangleRec(Rectangle r, Color c) { fill_rbox(r.x, r.y, r.width, r.height, 0, c); }

static float radius_of(Rectangle r, float roundness)
{
    float m = r.width < r.height ? r.width : r.height;
    return roundness <= 0 ? 0 : roundness * m / 2;
}

void DrawRectangleRounded(Rectangle r, float roundness, int seg, Color c)
{
    (void)seg;
    fill_rbox(r.x, r.y, r.width, r.height, radius_of(r, roundness), c);
}

/* como no raylib: o contorno fica por fora do retângulo */
void DrawRectangleRoundedLinesEx(Rectangle r, float roundness, int seg, float thick, Color c)
{
    (void)seg;
    Surface *s = C->target;
    if (!s || !s->px || c.a == 0 || thick <= 0) return;
    float k = T(), rad = radius_of(r, roundness) * k, t = thick * k;
    float x = r.x * k, y = r.y * k, w = r.width * k, h = r.height * k;
    int x0, y0, x1, y1;
    if (!bbox(s, x - t - 1, y - t - 1, x + w + t + 1, y + h + t + 1, &x0, &y0, &x1, &y1)) return;
    float cx = x + w / 2, cy = y + h / 2;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        bool mid = fy > y + rad + 1 && fy < y + h - rad - 1;
        for (int px = x0; px < x1; px++) {
            float fx = px + 0.5f;
            /* bem dentro: nada a desenhar */
            if (mid && fx > x + 1 && fx < x + w - 1) {
                int skip = (int)(x + w - 2);
                if (skip > px) px = skip; /* só pula para a frente */
                continue;
            }
            float outer = clamp01(0.5f - sd_rbox(fx, fy, cx, cy, w / 2 + t, h / 2 + t, rad > 0 ? rad + t : 0));
            float inner = clamp01(0.5f - sd_rbox(fx, fy, cx, cy, w / 2, h / 2, rad));
            blend(s, px, py, c, outer - inner);
        }
    }
}

void DrawCircleV(Vector2 c0, float radius, Color col)
{
    Surface *s = C->target;
    if (!s || !s->px || col.a == 0 || radius <= 0) return;
    float k = T(), cx = c0.x * k, cy = c0.y * k, r = radius * k;
    int x0, y0, x1, y1;
    if (!bbox(s, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
            blend(s, px, py, col, clamp01(0.5f - (sqrtf(dx * dx + dy * dy) - r)));
        }
}

void DrawRing(Vector2 c0, float inner, float outer, float start, float end, int seg, Color col)
{
    (void)seg;
    Surface *s = C->target;
    if (!s || !s->px || col.a == 0 || start == end) return;
    if (start > end) { float t = start; start = end; end = t; }
    if (inner > outer) { float t = inner; inner = outer; outer = t; }
    float k = T(), cx = c0.x * k, cy = c0.y * k, ri = inner * k, ro = outer * k;
    float span = end - start;
    bool full = span >= 360.0f;
    int x0, y0, x1, y1;
    if (!bbox(s, cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float dx = px + 0.5f - cx, dy = py + 0.5f - cy, dist = sqrtf(dx * dx + dy * dy);
            float d = fmaxf(ri - dist, dist - ro);
            if (d > 1.0f) continue;
            if (!full) {
                float a = atan2f(dy, dx) * RAD2DEG;
                float rel = fmodf(a - start, 360.0f);
                if (rel < 0) rel += 360.0f;
                float da; /* distância angular até a borda, em graus (negativa = dentro) */
                if (rel <= span) da = -fminf(rel, span - rel);
                else da = fminf(rel - span, 360.0f - rel);
                float dpx = da * DEG2RAD * fmaxf(dist, 0.5f);
                d = fmaxf(d, dpx);
            }
            blend(s, px, py, col, clamp01(0.5f - d));
        }
}

void DrawLineEx(Vector2 a, Vector2 b, float thick, Color col)
{
    Surface *s = C->target;
    if (!s || !s->px || col.a == 0 || thick <= 0) return;
    float k = T(), ax = a.x * k, ay = a.y * k, bx = b.x * k, by = b.y * k, ht = thick * k / 2;
    float ux = bx - ax, uy = by - ay, len = sqrtf(ux * ux + uy * uy);
    if (len < 1e-4f) return;
    ux /= len; uy /= len;
    float mx = (ax + bx) / 2, my = (ay + by) / 2, hl = len / 2;
    int x0, y0, x1, y1;
    if (!bbox(s, fminf(ax, bx) - ht - 1, fminf(ay, by) - ht - 1, fmaxf(ax, bx) + ht + 1, fmaxf(ay, by) + ht + 1, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float rx = px + 0.5f - mx, ry = py + 0.5f - my;
            float u = rx * ux + ry * uy, v = -rx * uy + ry * ux;
            float qx = fabsf(u) - hl, qy = fabsf(v) - ht;
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
            float d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0);
            blend(s, px, py, col, clamp01(0.5f - d));
        }
}

void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color col)
{
    Surface *s = C->target;
    if (!s || !s->px || col.a == 0) return;
    float k = T();
    float P[3][2] = {{a.x * k, a.y * k}, {b.x * k, b.y * k}, {c.x * k, c.y * k}};
    float area = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[1][1] - P[0][1]) * (P[2][0] - P[0][0]);
    if (fabsf(area) < 1e-6f) return;
    float sg = area > 0 ? 1.0f : -1.0f, E[3][3];
    for (int i = 0; i < 3; i++) {
        float *p = P[i], *q = P[(i + 1) % 3];
        float nx = -(q[1] - p[1]) * sg, ny = (q[0] - p[0]) * sg, l = sqrtf(nx * nx + ny * ny);
        /* normal para fora: distância positiva fora */
        E[i][0] = -nx / l; E[i][1] = -ny / l; E[i][2] = -(E[i][0] * p[0] + E[i][1] * p[1]);
    }
    int x0, y0, x1, y1;
    float mnx = fminf(P[0][0], fminf(P[1][0], P[2][0])), mxx = fmaxf(P[0][0], fmaxf(P[1][0], P[2][0]));
    float mny = fminf(P[0][1], fminf(P[1][1], P[2][1])), mxy = fmaxf(P[0][1], fmaxf(P[1][1], P[2][1]));
    if (!bbox(s, mnx - 1, mny - 1, mxx + 1, mxy + 1, &x0, &y0, &x1, &y1)) return;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            float fx = px + 0.5f, fy = py + 0.5f, d = -1e9f;
            for (int i = 0; i < 3; i++) d = fmaxf(d, E[i][0] * fx + E[i][1] * fy + E[i][2]);
            blend(s, px, py, col, clamp01(0.5f - d));
        }
}

Color Fade(Color c, float a)
{
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    c.a = (unsigned char)(255.0f * a);
    return c;
}

/* igual ao raylib: >0 clareia na direção do branco, <0 escurece */
Color ColorBrightness(Color c, float f)
{
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    float r = c.r, g = c.g, b = c.b;
    if (f < 0) { f = 1 + f; r *= f; g *= f; b *= f; }
    else { r = (255 - r) * f + r; g = (255 - g) * f + g; b = (255 - b) * f + b; }
    return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, c.a};
}

const char *CodepointToUTF8(int cp, int *size)
{
    static char b[6];
    int n = 0;
    if (cp <= 0x7f) { b[0] = (char)cp; n = 1; }
    else if (cp <= 0x7ff) { b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp <= 0xffff) { b[0] = (char)(0xe0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
    else { b[0] = (char)(0xf0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); b[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
    b[n] = 0;
    if (size) *size = n;
    return b;
}

bool CheckCollisionPointRec(Vector2 p, Rectangle r)
{
    return p.x >= r.x && p.x < r.x + r.width && p.y >= r.y && p.y < r.y + r.height;
}

/* ------------------------------------------------------------------ */
/* Texto                                                               */
/* ------------------------------------------------------------------ */
typedef struct { int cp, w, h, xoff, yoff; float adv; unsigned char *bmp; } Glyph;
#define GCACHE 1024
typedef struct {
    stbtt_fontinfo info;
    float scale;
    int   ascent;
    Glyph g[GCACHE];
} FontImpl;

Font LoadFontFromMemory(const char *type, const unsigned char *data, int size, int px, int *cps, int n)
{
    (void)type; (void)size; (void)cps; (void)n;
    Font f;
    memset(&f, 0, sizeof f);
    FontImpl *fi = calloc(1, sizeof *fi);
    if (!fi || !stbtt_InitFont(&fi->info, data, stbtt_GetFontOffsetForIndex(data, 0))) { free(fi); return f; }
    fi->scale = stbtt_ScaleForPixelHeight(&fi->info, (float)px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&fi->info, &asc, &desc, &gap);
    fi->ascent = (int)(asc * fi->scale);
    for (int i = 0; i < GCACHE; i++) fi->g[i].cp = -1;
    f.baseSize = px;
    f.impl = fi;
    return f;
}

void UnloadFont(Font f)
{
    FontImpl *fi = f.impl;
    if (!fi) return;
    for (int i = 0; i < GCACHE; i++) free(fi->g[i].bmp);
    free(fi);
}

static Glyph *glyph(FontImpl *fi, int cp)
{
    unsigned h = (unsigned)cp * 2654435761u % GCACHE;
    for (int i = 0; i < GCACHE; i++) {
        Glyph *g = &fi->g[(h + i) % GCACHE];
        if (g->cp == cp) return g;
        if (g->cp == -1) {
            int gi = stbtt_FindGlyphIndex(&fi->info, cp);
            if (gi == 0 && cp != '?') return glyph(fi, '?');
            g->cp = cp;
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&fi->info, gi, &adv, &lsb);
            /* largura exata (o raylib trunca em pixels inteiros, o que
               junta as palavras em tamanhos pequenos) */
            g->adv = adv * fi->scale;
            g->bmp = stbtt_GetGlyphBitmap(&fi->info, fi->scale, fi->scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
            return g;
        }
    }
    return &fi->g[0];
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = p[0], n = 1;
    if (c >= 0xF0 && p[1] && p[2] && p[3]) { c = ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); n = 4; }
    else if (c >= 0xE0 && p[1] && p[2]) { c = ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); n = 3; }
    else if (c >= 0xC0 && p[1]) { c = ((c & 31) << 6) | (p[1] & 63); n = 2; }
    *s += n;
    return c;
}

void DrawTextEx(Font f, const char *text, Vector2 pos, float size, float spacing, Color col)
{
    FontImpl *fi = f.impl;
    Surface *s = C->target;
    if (!fi || !s || !s->px || !text || f.baseSize <= 0) return;
    float k = T(), sf = size / f.baseSize, gk = sf * k; /* pixels do alvo por pixel do glifo */
    float pen = 0, line = 0;
    while (*text) {
        int cp = utf8_next(&text);
        if (cp == '\n') { pen = 0; line += size * 1.5f; continue; }
        Glyph *g = glyph(fi, cp);
        if (g->bmp && g->w > 0) {
            float gx = (pos.x + pen) * k + g->xoff * gk, gy = (pos.y + line) * k + (fi->ascent + g->yoff) * gk;
            if (fabsf(gk - 1.0f) < 0.02f) {
                int ox = (int)lroundf(gx), oy = (int)lroundf(gy);
                for (int y = 0; y < g->h; y++) {
                    int ty = oy + y;
                    if (ty < 0 || ty >= s->h) continue;
                    for (int x = 0; x < g->w; x++) {
                        int tx = ox + x;
                        if (tx < 0 || tx >= s->w) continue;
                        unsigned char a = g->bmp[y * g->w + x];
                        if (a) blend(s, tx, ty, col, a / 255.0f);
                    }
                }
            } else { /* escala diferente: amostragem bilinear do glifo */
                int w = (int)ceilf(g->w * gk) + 1, h = (int)ceilf(g->h * gk) + 1;
                int ox = (int)floorf(gx), oy = (int)floorf(gy);
                for (int y = 0; y < h; y++) {
                    int ty = oy + y;
                    if (ty < 0 || ty >= s->h) continue;
                    float sy = (ty + 0.5f - gy) / gk - 0.5f;
                    for (int x = 0; x < w; x++) {
                        int tx = ox + x;
                        if (tx < 0 || tx >= s->w) continue;
                        float sx = (tx + 0.5f - gx) / gk - 0.5f;
                        int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
                        float wx = sx - x0, wy = sy - y0, acc = 0;
                        for (int j = 0; j < 2; j++)
                            for (int i = 0; i < 2; i++) {
                                int xx = x0 + i, yy = y0 + j;
                                if (xx < 0 || yy < 0 || xx >= g->w || yy >= g->h) continue;
                                acc += g->bmp[yy * g->w + xx] * (i ? wx : 1 - wx) * (j ? wy : 1 - wy);
                            }
                        if (acc > 0) blend(s, tx, ty, col, acc / 255.0f);
                    }
                }
            }
        }
        pen += g->adv * sf + spacing;
    }
}

Vector2 MeasureTextEx(Font f, const char *text, float size, float spacing)
{
    FontImpl *fi = f.impl;
    if (!fi || !text || f.baseSize <= 0) return (Vector2){0, 0};
    float sf = size / f.baseSize, w = 0, best = 0;
    int n = 0, lines = 1;
    while (*text) {
        int cp = utf8_next(&text);
        if (cp == '\n') { if (w > best) best = w; w = 0; n = 0; lines++; continue; }
        if (n++) w += spacing;
        w += glyph(fi, cp)->adv * sf;
    }
    if (w > best) best = w;
    return (Vector2){best, size * lines + (lines - 1) * size * 0.5f};
}

/* ------------------------------------------------------------------ */
/* Arquivos (presets)                                                  */
/* ------------------------------------------------------------------ */
bool DirectoryExists(const char *path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool MakeDirectory(const char *path)
{
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static bool has_ext(const char *name, const char *filter)
{
    if (!filter || !*filter) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
#ifdef _WIN32
    return _stricmp(dot, filter) == 0;
#else
    return strcasecmp(dot, filter) == 0;
#endif
}

FilePathList LoadDirectoryFilesEx(const char *path, const char *filter, bool sub)
{
    (void)sub;
    FilePathList l = {0, 0, NULL};
    l.capacity = 256;
    l.paths = calloc(l.capacity, sizeof(char *));
    if (!l.paths) { l.capacity = 0; return l; }
#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!has_ext(fd.cFileName, filter) || l.count >= l.capacity) continue;
            size_t n = strlen(path) + strlen(fd.cFileName) + 2;
            l.paths[l.count] = malloc(n);
            if (l.paths[l.count]) snprintf(l.paths[l.count++], n, "%s/%s", path, fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !has_ext(e->d_name, filter) || l.count >= l.capacity) continue;
            size_t n = strlen(path) + strlen(e->d_name) + 2;
            l.paths[l.count] = malloc(n);
            if (l.paths[l.count]) snprintf(l.paths[l.count++], n, "%s/%s", path, e->d_name);
        }
        closedir(d);
    }
#endif
    return l;
}

void UnloadDirectoryFiles(FilePathList l)
{
    for (unsigned i = 0; i < l.count; i++) free(l.paths[i]);
    free(l.paths);
}

const char *GetFileNameWithoutExt(const char *path)
{
    static char buf[256];
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    snprintf(buf, sizeof buf, "%s", b);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) *dot = 0;
    return buf;
}
