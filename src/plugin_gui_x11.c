/* Janela do plugin no Linux (X11): janela filha + XPutImage. Os eventos são
   lidos a cada quadro (o timer do host chama plugwin_tick). */
/* o X11 também tem um tipo "Font": renomeia o dele antes do nosso raylib.h */
#define Font XFont
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#undef Font

#include "plugin_gui.h"
#include <stdlib.h>
#include <string.h>

struct PlugWin {
    Display     *dpy;
    Window       win;
    GC           gc;
    XImage      *img;
    unsigned char *bgra;
    int          img_w, img_h;
    int          w, h;
    double       scale;
    Cursor       hand, arrow;
    int          cur;
    bool         has_kb;
    RlContext   *ctx;
    PlugWinFrame frame;
    void        *user;
};

const char *plugwin_api(void) { return "x11"; }

PlugWin *plugwin_create(uintptr_t parent, int w, int h, double scale, RlContext *ctx, PlugWinFrame frame, void *user)
{
    PlugWin *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->dpy = XOpenDisplay(NULL);
    if (!p->dpy) { free(p); return NULL; }
    p->w = w; p->h = h;
    p->scale = scale > 0 ? scale : 1.0;
    p->ctx = ctx; p->frame = frame; p->user = user;
    int scr = DefaultScreen(p->dpy);
    p->win = XCreateSimpleWindow(p->dpy, (Window)parent, 0, 0, (unsigned)w, (unsigned)h, 0, 0, BlackPixel(p->dpy, scr));
    XSelectInput(p->dpy, p->win, ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                                 KeyPressMask | KeyReleaseMask | StructureNotifyMask | EnterWindowMask);
    p->gc = XCreateGC(p->dpy, p->win, 0, NULL);
    p->hand = XCreateFontCursor(p->dpy, XC_hand2);
    p->arrow = XCreateFontCursor(p->dpy, XC_left_ptr);
    XMapWindow(p->dpy, p->win);
    XFlush(p->dpy);
    rls_set_window(ctx, (int)(w / p->scale), (int)(h / p->scale), (float)p->scale);
    return p;
}

void plugwin_set_size(PlugWin *p, int w, int h, double scale)
{
    p->w = w; p->h = h;
    if (scale > 0) p->scale = scale;
    XResizeWindow(p->dpy, p->win, (unsigned)w, (unsigned)h);
    rls_set_window(p->ctx, (int)(w / p->scale), (int)(h / p->scale), (float)p->scale);
}

void plugwin_show(PlugWin *p, bool show)
{
    if (show) XMapWindow(p->dpy, p->win);
    else XUnmapWindow(p->dpy, p->win);
    XFlush(p->dpy);
}

void plugwin_own_timer(PlugWin *p, bool on) { (void)p; (void)on; /* no X11 quem chama é o timer do host */ }

static int map_key(KeySym k)
{
    if (k >= XK_a && k <= XK_z) return KEY_A + (int)(k - XK_a);
    if (k >= XK_A && k <= XK_Z) return KEY_A + (int)(k - XK_A);
    if (k >= XK_0 && k <= XK_9) return KEY_ZERO + (int)(k - XK_0);
    switch (k) {
    case XK_space: return KEY_SPACE;
    case XK_Return: return KEY_ENTER;
    case XK_KP_Enter: return KEY_KP_ENTER;
    case XK_BackSpace: return KEY_BACKSPACE;
    case XK_Delete: return KEY_DELETE;
    case XK_Escape: return KEY_ESCAPE;
    case XK_Tab: return KEY_TAB;
    case XK_Left: return KEY_LEFT;
    case XK_Right: return KEY_RIGHT;
    case XK_Up: return KEY_UP;
    case XK_Down: return KEY_DOWN;
    case XK_Home: return KEY_HOME;
    case XK_End: return KEY_END;
    case XK_Shift_L: return KEY_LEFT_SHIFT;
    case XK_Shift_R: return KEY_RIGHT_SHIFT;
    case XK_Control_L: return KEY_LEFT_CONTROL;
    case XK_Control_R: return KEY_RIGHT_CONTROL;
    case XK_comma: return KEY_COMMA;
    case XK_period: return KEY_PERIOD;
    case XK_slash: return KEY_SLASH;
    case XK_semicolon: return KEY_SEMICOLON;
    default: return 0;
    }
}

static void pump(PlugWin *p)
{
    float s = (float)p->scale;
    while (XPending(p->dpy)) {
        XEvent ev;
        XNextEvent(p->dpy, &ev);
        switch (ev.type) {
        case MotionNotify: rls_mouse_move(p->ctx, ev.xmotion.x / s, ev.xmotion.y / s); break;
        case ButtonPress:
        case ButtonRelease: {
            bool down = ev.type == ButtonPress;
            rls_mouse_move(p->ctx, ev.xbutton.x / s, ev.xbutton.y / s);
            if (ev.xbutton.button == 1) rls_mouse_button(p->ctx, MOUSE_BUTTON_LEFT, down);
            else if (ev.xbutton.button == 3) rls_mouse_button(p->ctx, MOUSE_BUTTON_RIGHT, down);
            else if (ev.xbutton.button == 2) rls_mouse_button(p->ctx, MOUSE_BUTTON_MIDDLE, down);
            else if (down && ev.xbutton.button == 4) rls_mouse_wheel(p->ctx, 1);
            else if (down && ev.xbutton.button == 5) rls_mouse_wheel(p->ctx, -1);
            break;
        }
        case KeyPress:
        case KeyRelease: {
            char buf[16];
            KeySym ks;
            int n = XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
            int k = map_key(XLookupKeysym(&ev.xkey, 0));
            if (k) rls_key(p->ctx, k, ev.type == KeyPress);
            if (ev.type == KeyPress && n == 1 && (unsigned char)buf[0] >= 32 && buf[0] != 127) rls_char(p->ctx, (unsigned char)buf[0]);
            else if (ev.type == KeyPress && ks >= 0xa0 && ks <= 0xff) rls_char(p->ctx, (unsigned)ks); /* latin-1 (ç, ã...) */
            break;
        }
        default: break;
        }
    }
}

static void present(PlugWin *p)
{
    int fw, fh;
    const unsigned char *fb = rls_framebuffer(p->ctx, &fw, &fh);
    if (!fb || fw <= 0 || fh <= 0) return;
    if (!p->img || p->img_w != fw || p->img_h != fh) {
        if (p->img) { p->img->data = NULL; XDestroyImage(p->img); }
        free(p->bgra);
        p->bgra = malloc((size_t)fw * fh * 4);
        if (!p->bgra) { p->img = NULL; return; }
        Visual *vis = DefaultVisual(p->dpy, DefaultScreen(p->dpy));
        p->img = XCreateImage(p->dpy, vis, (unsigned)DefaultDepth(p->dpy, DefaultScreen(p->dpy)), ZPixmap, 0,
                              (char *)p->bgra, (unsigned)fw, (unsigned)fh, 32, 0);
        p->img_w = fw; p->img_h = fh;
    }
    /* RGBA -> 0x00RRGGBB (TrueColor de 24/32 bits) */
    const uint32_t *src = (const uint32_t *)fb;
    uint32_t *dst = (uint32_t *)p->bgra;
    size_t n = (size_t)fw * fh;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = src[i];
        dst[i] = ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
    }
    XPutImage(p->dpy, p->win, p->gc, p->img, 0, 0, 0, 0, (unsigned)fw, (unsigned)fh);
    int cur = rls_cursor(p->ctx);
    if (cur != p->cur) {
        XDefineCursor(p->dpy, p->win, cur == MOUSE_CURSOR_POINTING_HAND ? p->hand : p->arrow);
        p->cur = cur;
    }
    XFlush(p->dpy);
}

void plugwin_tick(PlugWin *p)
{
    RlContext *old = rls_bind(p->ctx);
    pump(p);
    if (!rls_should_draw(p->ctx)) { rls_bind(old); return; }
    rls_new_frame(p->ctx);
    p->frame(p->user);
    present(p);
    rls_bind(old);
}

void plugwin_keyboard(PlugWin *p, bool want)
{
    if (want == p->has_kb) return;
    if (want) XSetInputFocus(p->dpy, p->win, RevertToParent, CurrentTime);
    else {
        Window parent = 0, root, *kids = NULL;
        unsigned n = 0;
        if (XQueryTree(p->dpy, p->win, &root, &parent, &kids, &n) && parent) XSetInputFocus(p->dpy, parent, RevertToParent, CurrentTime);
        if (kids) XFree(kids);
    }
    XFlush(p->dpy);
    p->has_kb = want;
}

void plugwin_destroy(PlugWin *p)
{
    if (!p) return;
    if (p->img) { p->img->data = NULL; XDestroyImage(p->img); }
    free(p->bgra);
    XFreeCursor(p->dpy, p->hand);
    XFreeCursor(p->dpy, p->arrow);
    XFreeGC(p->dpy, p->gc);
    XDestroyWindow(p->dpy, p->win);
    XCloseDisplay(p->dpy);
    free(p);
}
