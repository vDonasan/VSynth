/* Janela do plugin no Windows: janela filha + SetDIBitsToDevice. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stdlib.h>
#include <string.h>
/* o Windows também tem Rectangle (função) e DrawTextEx (macro): renomeia os
   dele antes do nosso raylib.h */
#define Rectangle Win32Rectangle
#include <windows.h>
#include <windowsx.h>
#undef Rectangle
#undef DrawTextEx

#include "plugin_gui.h"

struct PlugWin {
    HWND         hwnd;
    int          w, h;
    double       scale;
    unsigned char *bgra;
    int          bw, bh;
    int          own_timer;
    RlContext   *ctx;
    PlugWinFrame frame;
    void        *user;
    WCHAR        hi_surrogate;
};

const char *plugwin_api(void) { return "win32"; }

static const WCHAR *CLASS_NAME = L"VirtualSynthPluginView";
static int g_class_refs = 0;

static int map_vk(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return KEY_A + (int)(vk - 'A');
    if (vk >= '0' && vk <= '9') return KEY_ZERO + (int)(vk - '0');
    switch (vk) {
    case VK_SPACE: return KEY_SPACE;
    case VK_RETURN: return KEY_ENTER;
    case VK_BACK: return KEY_BACKSPACE;
    case VK_DELETE: return KEY_DELETE;
    case VK_ESCAPE: return KEY_ESCAPE;
    case VK_TAB: return KEY_TAB;
    case VK_LEFT: return KEY_LEFT;
    case VK_RIGHT: return KEY_RIGHT;
    case VK_UP: return KEY_UP;
    case VK_DOWN: return KEY_DOWN;
    case VK_HOME: return KEY_HOME;
    case VK_END: return KEY_END;
    case VK_SHIFT: return KEY_LEFT_SHIFT;
    case VK_LSHIFT: return KEY_LEFT_SHIFT;
    case VK_RSHIFT: return KEY_RIGHT_SHIFT;
    case VK_CONTROL: return KEY_LEFT_CONTROL;
    case VK_OEM_COMMA: return KEY_COMMA;
    case VK_OEM_PERIOD: return KEY_PERIOD;
    case VK_OEM_2: return KEY_SLASH;
    case VK_OEM_1: return KEY_SEMICOLON;
    default: return 0;
    }
}

static void present(PlugWin *p, HDC dc)
{
    int fw, fh;
    const unsigned char *fb = rls_framebuffer(p->ctx, &fw, &fh);
    if (!fb || fw <= 0 || fh <= 0) return;
    if (!p->bgra || p->bw != fw || p->bh != fh) {
        free(p->bgra);
        p->bgra = malloc((size_t)fw * fh * 4);
        if (!p->bgra) return;
        p->bw = fw; p->bh = fh;
    }
    const uint32_t *src = (const uint32_t *)fb;
    uint32_t *dst = (uint32_t *)p->bgra;
    size_t n = (size_t)fw * fh;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[i] = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
    }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = fw;
    bi.bmiHeader.biHeight = -fh; /* de cima para baixo */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, (DWORD)fw, (DWORD)fh, 0, 0, 0, (UINT)fh, p->bgra, &bi, DIB_RGB_COLORS);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    PlugWin *p = (PlugWin *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!p) return DefWindowProcW(hwnd, msg, wp, lp);
    float s = (float)p->scale;
    switch (msg) {
    case WM_TIMER:
        plugwin_tick(p);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        present(p, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: rls_mouse_move(p->ctx, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        rls_mouse_move(p->ctx, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        rls_mouse_button(p->ctx, msg == WM_LBUTTONDOWN ? MOUSE_BUTTON_LEFT : msg == WM_RBUTTONDOWN ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_MIDDLE, true);
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ReleaseCapture();
        rls_mouse_move(p->ctx, GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
        rls_mouse_button(p->ctx, msg == WM_LBUTTONUP ? MOUSE_BUTTON_LEFT : msg == WM_RBUTTONUP ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_MIDDLE, false);
        return 0;
    case WM_MOUSEWHEEL:
        rls_mouse_wheel(p->ctx, GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, rls_cursor(p->ctx) == MOUSE_CURSOR_POINTING_HAND ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN: case WM_KEYUP: {
        int k = map_vk(wp);
        if (k) { rls_key(p->ctx, k, msg == WM_KEYDOWN); return 0; }
        break;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { p->hi_surrogate = c; return 0; }
        unsigned cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF && p->hi_surrogate) cp = 0x10000 + ((p->hi_surrogate - 0xD800) << 10) + (c - 0xDC00);
        p->hi_surrogate = 0;
        if (cp >= 32 && cp != 127) rls_char(p->ctx, cp);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

PlugWin *plugwin_create(uintptr_t parent, int w, int h, double scale, RlContext *ctx, PlugWinFrame frame, void *user)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    {
        /* o módulo do plugin, não o do host */
        HMODULE m = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)wndproc, &m);
        if (m) inst = m;
    }
    if (g_class_refs++ == 0) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = CLASS_NAME;
        wc.style = CS_DBLCLKS;
        RegisterClassW(&wc);
    }
    PlugWin *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->w = w; p->h = h;
    p->scale = scale > 0 ? scale : 1.0;
    p->ctx = ctx; p->frame = frame; p->user = user;
    p->hwnd = CreateWindowExW(0, CLASS_NAME, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                              0, 0, w, h, (HWND)parent, NULL, inst, NULL);
    if (!p->hwnd) { free(p); if (--g_class_refs == 0) UnregisterClassW(CLASS_NAME, inst); return NULL; }
    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, (LONG_PTR)p);
    rls_set_window(ctx, (int)(w / p->scale), (int)(h / p->scale), (float)p->scale);
    return p;
}

void plugwin_set_size(PlugWin *p, int w, int h, double scale)
{
    p->w = w; p->h = h;
    if (scale > 0) p->scale = scale;
    SetWindowPos(p->hwnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    rls_set_window(p->ctx, (int)(w / p->scale), (int)(h / p->scale), (float)p->scale);
}

void plugwin_show(PlugWin *p, bool show) { ShowWindow(p->hwnd, show ? SW_SHOW : SW_HIDE); }

void plugwin_own_timer(PlugWin *p, bool on)
{
    if (on && !p->own_timer) SetTimer(p->hwnd, 1, 33, NULL);
    if (!on && p->own_timer) KillTimer(p->hwnd, 1);
    p->own_timer = on;
}

void plugwin_tick(PlugWin *p)
{
    if (!rls_should_draw(p->ctx)) return;
    RlContext *old = rls_bind(p->ctx);
    rls_new_frame(p->ctx);
    p->frame(p->user);
    rls_bind(old);
    HDC dc = GetDC(p->hwnd);
    present(p, dc);
    ReleaseDC(p->hwnd, dc);
}

void plugwin_keyboard(PlugWin *p, bool want)
{
    HWND f = GetFocus();
    if (want && f != p->hwnd) SetFocus(p->hwnd);
    else if (!want && f == p->hwnd) SetFocus(GetParent(p->hwnd));
}

void plugwin_destroy(PlugWin *p)
{
    if (!p) return;
    if (p->own_timer) KillTimer(p->hwnd, 1);
    SetWindowLongPtrW(p->hwnd, GWLP_USERDATA, 0);
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(p->hwnd, GWLP_HINSTANCE);
    DestroyWindow(p->hwnd);
    free(p->bgra);
    free(p);
    if (--g_class_refs == 0) UnregisterClassW(CLASS_NAME, inst);
}
