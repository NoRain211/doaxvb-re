#include "netplay.h"

#include <ctype.h>
#include <string.h>

/* Slurs against race, sexuality and gender identity, and Nazi and fascist
   references. Long ones match anywhere in a name's letters; short ones,
   which hide inside ordinary words, only as a whole word, with or without a
   plural s. A run of one letter here needs at least as many of that letter
   in the name. */
static const char *const anywhere[] = {
    "nigger", "nigga", "faggot", "tranny", "trannie", "shemale", "wetback", "raghead", "towelhead",
    "jigaboo", "porchmonkey", "zipperhead", "beaner", "spearchucker", "chingchong",
    "hitler", "neonazi", "natsoc", "siegheil", "fuhrer", "fuehrer", "thirdreich", "fourthreich",
    "swastika", "sonnenrad", "totenkopf", "zyklon", "holohoax", "gasthejews", "auschwitz",
    "goebbels", "himmler", "mengele", "eichmann", "mussolini", "fascis", "facist", "whitepower",
    "whitepride", "wpww", "rahowa", "bloodandhono", "fourteenwords", "kkk",
};
static const char *const whole[] = {
    "nig", "niga", "fag", "fagot", "chink", "gook", "spic", "spick", "kike", "coon", "dyke", "paki",
    "homo", "troon", "nazi", "heil", "aryan",
};
/* White supremacist number codes, in a name's letters and digits with
   everything else dropped. */
static const char *const codes[] = {
    "1488", "14words", "hh88", "88hh", "6mwe",
};

enum { MAX_NAME = 256 };

/* Digits and symbols that stand for letters. */
static const char leet_from[] = "0134579@$!", leet_to[] = "oieastgasi";

static size_t run_length(const char *s, size_t size, size_t at)
{
    size_t end = at;
    while (end < size && s[end] == s[at]) ++end;
    return end - at;
}

/* True when term's letter runs start at text[at], each run in text at least
   as long; *end is where the match ends. */
static bool runs_at(const char *text, size_t size, size_t at, const char *term, size_t *end)
{
    const size_t term_size = strlen(term);
    for (size_t t = 0u; t < term_size;) {
        if (at >= size || text[at] != term[t]) return false;
        const size_t want = run_length(term, term_size, t), have = run_length(text, size, at);
        if (have < want) return false;
        t += want;
        at += have;
    }
    *end = at;
    return true;
}

/* source is the name at the same offset as word, or null; a short term may
   end where the name had a digit or symbol the leet pass mapped. */
static bool word_banned(const char *word, size_t size, const unsigned char *source)
{
    for (size_t i = 0u; i < sizeof whole / sizeof *whole; ++i) {
        size_t end;
        if (runs_at(word, size, 0u, whole[i], &end) &&
            (end == size || (word[end] == 's' && end + run_length(word, size, end) == size) ||
                (source != NULL && !isalpha(source[end]))))
            return true;
    }
    return false;
}

/* Lower-case letters of name with words split at spaces, or false when it is
   too long to check. With leet, digits and symbols that stand for letters
   become them; otherwise they split words. */
static bool normalize(const char *name, bool leet, char *out, size_t *size)
{
    size_t n = 0u;
    for (const unsigned char *c = (const unsigned char *)name; *c != 0u; ++c) {
        if (n == MAX_NAME) return false;
        const char *mapped = leet ? strchr(leet_from, *c) : NULL;
        out[n++] = *c < 0x80u && isalpha(*c) ? (char)tolower(*c) : mapped != NULL ? leet_to[mapped - leet_from] : ' ';
    }
    *size = n;
    return true;
}

/* True when code is in text, a letter of code also matching a digit that stands for it. */
static bool has_code(const char *text, size_t size, const char *code)
{
    const size_t length = strlen(code);
    for (size_t at = 0u; at + length <= size; ++at) {
        size_t i = 0u;
        for (; i < length; ++i) {
            const char c = text[at + i];
            const char *mapped = isdigit((unsigned char)c) ? strchr(leet_from, c) : NULL;
            if (c != code[i] && (mapped == NULL || leet_to[mapped - leet_from] != code[i])) break;
        }
        if (i == length) return true;
    }
    return false;
}

bool recomp_netplay_name_banned(const char *name)
{
    // Look-alike letters from other scripts would pass every check below.
    for (const unsigned char *c = (const unsigned char *)name; *c != 0u; ++c)
        if (*c >= 0x80u) return true;
    char code[MAX_NAME + 1u];
    size_t code_size = 0u;
    for (const unsigned char *c = (const unsigned char *)name; *c != 0u && code_size < MAX_NAME; ++c)
        if (*c < 0x80u && isalnum(*c)) code[code_size++] = (char)tolower(*c);
    code[code_size] = 0;
    for (size_t i = 0u; i < sizeof codes / sizeof *codes; ++i)
        if (has_code(code, code_size, codes[i])) return true;
    for (int leet = 0; leet < 2; ++leet) {
        char text[MAX_NAME], letters[MAX_NAME];
        size_t size, joined = 0u;
        if (!normalize(name, leet != 0, text, &size)) return true;
        for (size_t at = 0u; at < size;) {
            size_t end = at;
            while (end < size && text[end] != ' ') letters[joined++] = text[end++];
            if (end > at && word_banned(text + at, end - at, (const unsigned char *)name + at)) return true;
            at = end + 1u;
        }
        // Letters spaced apart still spell a word.
        if (word_banned(letters, joined, NULL)) return true;
        for (size_t at = 0u; at < joined; at += run_length(letters, joined, at)) {
            for (size_t i = 0u; i < sizeof anywhere / sizeof *anywhere; ++i) {
                size_t end;
                if (runs_at(letters, joined, at, anywhere[i], &end)) return true;
            }
        }
    }
    return false;
}
