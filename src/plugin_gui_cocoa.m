/* Janela do plugin no macOS: uma NSView filha que desenha a imagem pronta
   (CGImage) e passa mouse e teclado. */
#import <Cocoa/Cocoa.h>

#include "plugin_gui.h"

struct PlugWin;

@interface VSynthView : NSView {
@public
    struct PlugWin *pw;
}
@end

struct PlugWin {
    VSynthView  *view;
    NSTimer     *timer;
    int          w, h;
    RlContext   *ctx;
    PlugWinFrame frame;
    void        *user;
};

const char *plugwin_api(void) { return "cocoa"; }

static int map_key(unsigned short code, NSString *chars)
{
    switch (code) {
    case 49: return KEY_SPACE;
    case 36: return KEY_ENTER;
    case 76: return KEY_KP_ENTER;
    case 51: return KEY_BACKSPACE;
    case 117: return KEY_DELETE;
    case 53: return KEY_ESCAPE;
    case 48: return KEY_TAB;
    case 123: return KEY_LEFT;
    case 124: return KEY_RIGHT;
    case 125: return KEY_DOWN;
    case 126: return KEY_UP;
    case 115: return KEY_HOME;
    case 119: return KEY_END;
    default: break;
    }
    if ([chars length] == 1) {
        unichar c = [chars characterAtIndex:0];
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= 'A' && c <= 'Z') return KEY_A + (c - 'A');
        if (c >= '0' && c <= '9') return KEY_ZERO + (c - '0');
        if (c == ',') return KEY_COMMA;
        if (c == '.') return KEY_PERIOD;
        if (c == '/') return KEY_SLASH;
        if (c == ';') return KEY_SEMICOLON;
    }
    return 0;
}

@implementation VSynthView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *t in [self trackingAreas]) [self removeTrackingArea:t];
    NSTrackingArea *t = [[NSTrackingArea alloc]
        initWithRect:[self bounds]
             options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect | NSTrackingCursorUpdate
               owner:self
            userInfo:nil];
    [self addTrackingArea:t];
    [t release]; /* sem ARC: a view guarda a sua referência */
}

- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    if (pw) {
        float ratio = self.window ? (float)self.window.backingScaleFactor : 1.0f;
        rls_set_window(pw->ctx, pw->w, pw->h, ratio);
    }
}

- (void)drawRect:(NSRect)r
{
    (void)r;
    if (!pw) return;
    int fw, fh;
    const unsigned char *fb = rls_framebuffer(pw->ctx, &fw, &fh);
    if (!fb || fw <= 0 || fh <= 0) return;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, fb, (size_t)fw * fh * 4, NULL);
    CGImageRef img = CGImageCreate((size_t)fw, (size_t)fh, 8, 32, (size_t)fw * 4, cs,
                                   kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CGContextRef c = [[NSGraphicsContext currentContext] CGContext];
    CGContextSaveGState(c);
    /* a view é "flipped": desvira para o CGImage sair de pé */
    CGContextTranslateCTM(c, 0, pw->h);
    CGContextScaleCTM(c, 1, -1);
    CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextDrawImage(c, CGRectMake(0, 0, pw->w, pw->h), img);
    CGContextRestoreGState(c);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

- (void)moveTo:(NSEvent *)e
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    rls_mouse_move(pw->ctx, (float)p.x, (float)p.y);
}
- (void)mouseMoved:(NSEvent *)e { [self moveTo:e]; }
- (void)mouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self moveTo:e]; }
- (void)mouseDown:(NSEvent *)e { [self moveTo:e]; rls_mouse_button(pw->ctx, MOUSE_BUTTON_LEFT, true); }
- (void)mouseUp:(NSEvent *)e { [self moveTo:e]; rls_mouse_button(pw->ctx, MOUSE_BUTTON_LEFT, false); }
- (void)rightMouseDown:(NSEvent *)e { [self moveTo:e]; rls_mouse_button(pw->ctx, MOUSE_BUTTON_RIGHT, true); }
- (void)rightMouseUp:(NSEvent *)e { [self moveTo:e]; rls_mouse_button(pw->ctx, MOUSE_BUTTON_RIGHT, false); }
- (void)scrollWheel:(NSEvent *)e
{
    float d = (float)[e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) d /= 10.0f;
    rls_mouse_wheel(pw->ctx, d);
}
- (void)cursorUpdate:(NSEvent *)e
{
    (void)e;
    if (rls_cursor(pw->ctx) == MOUSE_CURSOR_POINTING_HAND) [[NSCursor pointingHandCursor] set];
    else [[NSCursor arrowCursor] set];
}
- (void)keyDown:(NSEvent *)e
{
    int k = map_key([e keyCode], [e charactersIgnoringModifiers]);
    if (k) rls_key(pw->ctx, k, true);
    NSString *s = [e characters];
    for (NSUInteger i = 0; i < [s length]; i++) {
        unichar c = [s characterAtIndex:i];
        if (c >= 32 && c != 127 && (c < 0xF700 || c > 0xF8FF)) rls_char(pw->ctx, c);
    }
}
- (void)keyUp:(NSEvent *)e
{
    int k = map_key([e keyCode], [e charactersIgnoringModifiers]);
    if (k) rls_key(pw->ctx, k, false);
}
- (void)flagsChanged:(NSEvent *)e
{
    rls_key(pw->ctx, KEY_LEFT_SHIFT, ([e modifierFlags] & NSEventModifierFlagShift) != 0);
}
- (void)tick:(NSTimer *)t
{
    (void)t;
    if (pw) plugwin_tick(pw);
}
@end

PlugWin *plugwin_create(uintptr_t parent, int w, int h, double scale, RlContext *ctx, PlugWinFrame frame, void *user)
{
    (void)scale;
    PlugWin *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->w = w; p->h = h;
    p->ctx = ctx; p->frame = frame; p->user = user;
    VSynthView *v = [[VSynthView alloc] initWithFrame:NSMakeRect(0, 0, w, h)];
    v->pw = p;
    p->view = v;
    [(NSView *)(void *)parent addSubview:v];
    [v release]; /* sem ARC: quem segura agora é a view do host */
    float ratio = v.window ? (float)v.window.backingScaleFactor : 2.0f;
    rls_set_window(ctx, w, h, ratio);
    return p;
}

void plugwin_set_size(PlugWin *p, int w, int h, double scale)
{
    (void)scale;
    p->w = w; p->h = h;
    [p->view setFrameSize:NSMakeSize(w, h)];
    float ratio = p->view.window ? (float)p->view.window.backingScaleFactor : 2.0f;
    rls_set_window(p->ctx, w, h, ratio);
}

void plugwin_show(PlugWin *p, bool show) { [p->view setHidden:!show]; }

void plugwin_own_timer(PlugWin *p, bool on)
{
    if (on && !p->timer)
        p->timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0 target:p->view selector:@selector(tick:) userInfo:nil repeats:YES];
    if (!on && p->timer) { [p->timer invalidate]; p->timer = nil; }
}

void plugwin_tick(PlugWin *p)
{
    if (p->view.window) {
        float ratio = (float)p->view.window.backingScaleFactor;
        rls_set_window(p->ctx, p->w, p->h, ratio);
    }
    if (!rls_should_draw(p->ctx)) return;
    RlContext *old = rls_bind(p->ctx);
    rls_new_frame(p->ctx);
    p->frame(p->user);
    rls_bind(old);
    [p->view setNeedsDisplay:YES];
}

void plugwin_keyboard(PlugWin *p, bool want)
{
    NSWindow *w = p->view.window;
    if (!w) return;
    bool has = [w firstResponder] == p->view;
    if (want && !has) [w makeFirstResponder:p->view];
    else if (!want && has) [w makeFirstResponder:nil];
}

void plugwin_destroy(PlugWin *p)
{
    if (!p) return;
    plugwin_own_timer(p, false);
    p->view->pw = NULL;
    [p->view removeFromSuperview]; /* libera a view (só a do host a segurava) */
    p->view = nil;
    free(p);
}
