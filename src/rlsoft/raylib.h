/*
 * rlsoft: o pedaço do raylib que a interface (ui.c, gui.c) usa, desenhado em
 * software numa imagem RGBA. Serve para o plugin: a DAW dá uma janela filha
 * (Win32, X11, Cocoa) e a camada de plataforma (plugin_gui_*.c) só copia a
 * imagem pronta para ela. Assim a interface do plugin é a mesma do app.
 *
 * Mesmos nomes e semânticas do raylib 5.5 (contornos por fora do retângulo,
 * anéis em graus a partir da direita em sentido horário, avanço do texto
 * truncado em pixels), para o layout sair idêntico.
 *
 * Cada janela tem o seu RlContext (rls_bind antes de desenhar).
 */
#ifndef RLSOFT_RAYLIB_H
#define RLSOFT_RAYLIB_H

#include <stdbool.h>

#ifndef PI
#define PI 3.14159265358979323846f
#endif
#define DEG2RAD (PI / 180.0f)
#define RAD2DEG (180.0f / PI)

typedef struct Vector2 { float x, y; } Vector2;
typedef struct Rectangle { float x, y, width, height; } Rectangle;
typedef struct Color { unsigned char r, g, b, a; } Color;
typedef struct Image { void *data; int width, height, mipmaps, format; } Image;
typedef struct Texture2D { unsigned int id; int width, height, mipmaps, format; } Texture2D;
typedef struct RenderTexture2D { unsigned int id; Texture2D texture, depth; } RenderTexture2D;
typedef struct Camera2D { Vector2 offset, target; float rotation, zoom; } Camera2D;
typedef struct Font { int baseSize, glyphCount, glyphPadding; Texture2D texture; void *impl; } Font;
typedef struct FilePathList { unsigned int capacity, count; char **paths; } FilePathList;

#define BLACK (Color){0, 0, 0, 255}
#define WHITE (Color){255, 255, 255, 255}
#define BLANK (Color){0, 0, 0, 0}

enum {
    KEY_NULL = 0, KEY_SPACE = 32, KEY_COMMA = 44, KEY_PERIOD = 46, KEY_SLASH = 47,
    KEY_ZERO = 48, KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX, KEY_SEVEN, KEY_EIGHT, KEY_NINE,
    KEY_SEMICOLON = 59,
    KEY_A = 65, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_ESCAPE = 256, KEY_ENTER = 257, KEY_TAB = 258, KEY_BACKSPACE = 259, KEY_DELETE = 261,
    KEY_RIGHT = 262, KEY_LEFT = 263, KEY_DOWN = 264, KEY_UP = 265, KEY_HOME = 268, KEY_END = 269,
    KEY_F1 = 290, KEY_KP_ENTER = 335,
    KEY_LEFT_SHIFT = 340, KEY_LEFT_CONTROL = 341, KEY_LEFT_SUPER = 343, KEY_RIGHT_SHIFT = 344, KEY_RIGHT_CONTROL = 345,
    RL_MAX_KEYS = 512
};
enum { MOUSE_BUTTON_LEFT = 0, MOUSE_BUTTON_RIGHT = 1, MOUSE_BUTTON_MIDDLE = 2 };
enum { MOUSE_CURSOR_DEFAULT = 0, MOUSE_CURSOR_POINTING_HAND = 4 };
enum { TEXTURE_FILTER_POINT = 0, TEXTURE_FILTER_BILINEAR = 1 };

/* ---------- contexto de uma janela ---------- */
typedef struct RlContext RlContext;
RlContext *rls_create(void);
void       rls_destroy(RlContext *c);
RlContext *rls_bind(RlContext *c); /* devolve o anterior */
/* tamanho da janela em pontos e pixels de verdade por ponto (Retina = 2) */
void       rls_set_window(RlContext *c, int w, int h, float pixel_ratio);
/* eventos da plataforma (coordenadas em pontos da janela) */
void       rls_mouse_move(RlContext *c, float x, float y);
void       rls_mouse_button(RlContext *c, int button, bool down);
void       rls_mouse_wheel(RlContext *c, float dy);
void       rls_key(RlContext *c, int key, bool down);
void       rls_char(RlContext *c, unsigned codepoint);
/* começo de cada quadro: fecha o estado de entrada do quadro anterior */
void       rls_new_frame(RlContext *c);
/* a cada tick do timer: vale a pena desenhar agora? (todo quadro enquanto
   alguém mexe; ~1 em 3 parado) */
bool       rls_should_draw(RlContext *c);
/* imagem pronta para mostrar (RGBA8, w*h pixels de verdade) e o cursor */
const unsigned char *rls_framebuffer(RlContext *c, int *w, int *h);
int        rls_cursor(RlContext *c);

/* ---------- raylib (subconjunto) ---------- */
double GetTime(void);
int    GetScreenWidth(void);
int    GetScreenHeight(void);
int    GetRenderWidth(void);
int    GetRenderHeight(void);

Vector2 GetMousePosition(void);
bool    IsMouseButtonDown(int b);
bool    IsMouseButtonPressed(int b);
bool    IsMouseButtonReleased(int b);
float   GetMouseWheelMove(void);
void    SetMouseOffset(int x, int y);
void    SetMouseScale(float sx, float sy);
void    SetMouseCursor(int cursor);
bool    IsKeyDown(int k);
bool    IsKeyPressed(int k);
bool    IsKeyPressedRepeat(int k);
bool    IsKeyReleased(int k);
int     GetCharPressed(void);

void ClearBackground(Color c);
void BeginDrawing(void);
void EndDrawing(void);
void BeginMode2D(Camera2D cam);
void EndMode2D(void);
RenderTexture2D LoadRenderTexture(int w, int h);
void UnloadRenderTexture(RenderTexture2D t);
void BeginTextureMode(RenderTexture2D t);
void EndTextureMode(void);
void SetTextureFilter(Texture2D t, int filter);
void DrawTexturePro(Texture2D t, Rectangle src, Rectangle dst, Vector2 origin, float rotation, Color tint);
Image LoadImageFromTexture(Texture2D t);
void ImageFlipVertical(Image *img);
bool ExportImage(Image img, const char *path);
void UnloadImage(Image img);

void DrawRectangle(int x, int y, int w, int h, Color c);
void DrawRectangleRec(Rectangle r, Color c);
void DrawRectangleRounded(Rectangle r, float roundness, int segments, Color c);
void DrawRectangleRoundedLinesEx(Rectangle r, float roundness, int segments, float thick, Color c);
void DrawCircleV(Vector2 c, float radius, Color col);
void DrawRing(Vector2 c, float inner, float outer, float start, float end, int segments, Color col);
void DrawLineEx(Vector2 a, Vector2 b, float thick, Color c);
void DrawTriangle(Vector2 a, Vector2 b, Vector2 c, Color col);

Font    LoadFontFromMemory(const char *type, const unsigned char *data, int size, int px, int *codepoints, int n);
void    UnloadFont(Font f);
void    DrawTextEx(Font f, const char *text, Vector2 pos, float size, float spacing, Color c);
Vector2 MeasureTextEx(Font f, const char *text, float size, float spacing);

Color Fade(Color c, float alpha);
Color ColorBrightness(Color c, float factor);
const char *CodepointToUTF8(int codepoint, int *size);
bool  CheckCollisionPointRec(Vector2 p, Rectangle r);

bool         DirectoryExists(const char *path);
bool         MakeDirectory(const char *path);
FilePathList LoadDirectoryFilesEx(const char *path, const char *filter, bool scan_subdirs);
void         UnloadDirectoryFiles(FilePathList list);
const char  *GetFileNameWithoutExt(const char *path);

#endif
