/* Idioma da interface: português (o texto escrito no código) ou inglês.
   tr() recebe o texto em português e devolve a versão no idioma atual;
   o que não estiver na tabela (nomes de presets, aparelhos...) volta igual. */
#ifndef I18N_H
#define I18N_H

enum { LANG_PT = 0, LANG_EN = 1 };

typedef struct { const char *pt, *en; } I18nPair;
extern const I18nPair I18N_EN[];
extern const int I18N_EN_COUNT;

void i18n_set(int lang);
int  i18n_get(void);
/* idioma do sistema: português se o sistema estiver em português, senão inglês */
int  i18n_system_default(void);
/* "pt" / "en" <-> LANG_*; texto desconhecido devolve -1 */
int  i18n_parse(const char *code);
const char *i18n_code(int lang);
/* arquivo com uma linha "pt" ou "en" (usado pelo plugin) */
int  i18n_load_file(const char *path);   /* -1 se não existir */
void i18n_save_file(const char *path, int lang);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format_arg(1)))
#endif
const char *tr(const char *pt);

#endif
