#include "i18n.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOUSER
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

/* tabela de dispersão montada uma vez: índice do par + 1 (0 = vazio) */
#define HBITS 11
#define HSIZE (1 << HBITS)
static unsigned short g_hash[HSIZE];
static atomic_int g_ready;
static atomic_int g_lang;

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static void build(void)
{
    if (atomic_load(&g_ready)) return;
    for (int i = 0; i < I18N_EN_COUNT; i++) {
        unsigned h = hash_str(I18N_EN[i].pt) & (HSIZE - 1);
        while (g_hash[h]) h = (h + 1) & (HSIZE - 1);
        g_hash[h] = (unsigned short)(i + 1);
    }
    atomic_store(&g_ready, 1);
}

void i18n_set(int lang)
{
    if (lang != LANG_EN) lang = LANG_PT;
    if (lang == LANG_EN) build(); /* antes de trocar, para tr() nunca ver a tabela pela metade */
    atomic_store(&g_lang, lang);
}

int i18n_get(void) { return atomic_load(&g_lang); }

const char *tr(const char *pt)
{
    if (!pt || !*pt || atomic_load_explicit(&g_lang, memory_order_relaxed) != LANG_EN) return pt;
    unsigned h = hash_str(pt) & (HSIZE - 1);
    for (unsigned short k; (k = g_hash[h]) != 0; h = (h + 1) & (HSIZE - 1))
        if (!strcmp(I18N_EN[k - 1].pt, pt)) return I18N_EN[k - 1].en;
#ifdef I18N_LOG_MISS /* para achar textos sem tradução: compile com -DI18N_LOG_MISS */
    fprintf(stderr, "i18n: falta \"%s\"\n", pt);
#endif
    return pt;
}

int i18n_parse(const char *code)
{
    if (!code) return -1;
    while (*code == ' ') code++;
    if (!strncmp(code, "pt", 2) || !strncmp(code, "PT", 2)) return LANG_PT;
    if (!strncmp(code, "en", 2) || !strncmp(code, "EN", 2)) return LANG_EN;
    return -1;
}

const char *i18n_code(int lang) { return lang == LANG_EN ? "en" : "pt"; }

int i18n_system_default(void)
{
#ifdef _WIN32
    LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_PORTUGUESE ? LANG_PT : LANG_EN;
#else
#ifdef __APPLE__
    int found = -1;
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if (langs) {
        if (CFArrayGetCount(langs) > 0) {
            char b[16] = "";
            CFStringRef s = CFArrayGetValueAtIndex(langs, 0);
            if (CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8))
                found = strncmp(b, "pt", 2) == 0 ? LANG_PT : LANG_EN;
        }
        CFRelease(langs);
    }
    if (found >= 0) return found;
#endif
    const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    for (int i = 0; i < 4; i++) {
        const char *v = getenv(vars[i]);
        if (v && *v && strcmp(v, "C") && strcmp(v, "POSIX") && strncmp(v, "C.", 2))
            return strncmp(v, "pt", 2) == 0 ? LANG_PT : LANG_EN;
    }
    return LANG_EN;
#endif
}

int i18n_load_file(const char *path)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) return -1;
    char b[16] = "";
    int r = fgets(b, sizeof b, f) ? i18n_parse(b) : -1;
    fclose(f);
    return r;
}

void i18n_save_file(const char *path, int lang)
{
    FILE *f = path ? fopen(path, "w") : NULL;
    if (!f) return;
    fprintf(f, "%s\n", i18n_code(lang));
    fclose(f);
}
