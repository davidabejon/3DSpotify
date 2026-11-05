#include "lang.h"

#include <string.h>

extern const char *const lang_strings_en[STR_COUNT];
extern const char *const lang_strings_es[STR_COUNT];

static const char *const *const tables[LANG_COUNT] = {
	[LANG_EN] = lang_strings_en,
	[LANG_ES] = lang_strings_es,
};

static const char *const codes[LANG_COUNT] = {
	[LANG_EN] = "en",
	[LANG_ES] = "es",
};

static lang_id current = LANG_EN;

/* Enums may be unsigned (they are on ARM), so range checks go through an
 * unsigned cast instead of comparing against 0. */
static bool valid_lang(lang_id lang)
{
	return (unsigned)lang < LANG_COUNT;
}

lang_id lang_get(void)
{
	return current;
}

void lang_set(lang_id lang)
{
	if (valid_lang(lang))
		current = lang;
}

lang_id lang_next(lang_id lang)
{
	if (!valid_lang(lang))
		return LANG_EN;
	return (lang_id)((lang + 1) % LANG_COUNT);
}

const char *lang_code(lang_id lang)
{
	return valid_lang(lang) ? codes[lang] : codes[LANG_EN];
}

bool lang_from_code(const char *code, lang_id *out)
{
	if (!code)
		return false;
	for (int i = 0; i < LANG_COUNT; i++) {
		if (strcmp(code, codes[i]) == 0) {
			*out = (lang_id)i;
			return true;
		}
	}
	return false;
}

const char *lang_text(lang_id lang, str_id id)
{
	if ((unsigned)id >= STR_COUNT)
		return "";
	if (valid_lang(lang) && tables[lang][id])
		return tables[lang][id];
	return lang_strings_en[id] ? lang_strings_en[id] : "";
}

const char *tr(str_id id)
{
	return lang_text(current, id);
}
