#include "overlay_text.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "overlay text test failed: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    RecompOverlayText text;
    char long_line[400];

    recomp_overlay_clear(&text);
    check(recomp_overlay_empty(&text), "a cleared overlay is empty");

    recomp_overlay_set_title(&text, "MODEL VIEWER");
    check(!recomp_overlay_empty(&text) && strcmp(text.title, "MODEL VIEWER") == 0, "a title is kept");
    check(recomp_overlay_add_line(&text, "Animation %u of %u", 36u, 45u), "a line is added");
    check(strcmp(text.line[0], "Animation 36 of 45") == 0, "lines are formatted");

    for (unsigned i = 1u; i < RECOMP_OVERLAY_LINES; ++i) recomp_overlay_add_line(&text, "line %u", i);
    check(!recomp_overlay_add_line(&text, "one too many"), "lines past the limit are dropped");
    check(text.count == RECOMP_OVERLAY_LINES, "the count stops at the limit");

    memset(long_line, 'x', sizeof long_line - 1u);
    long_line[sizeof long_line - 1u] = '\0';
    recomp_overlay_set_hint(&text, long_line);
    check(strlen(text.hint) == RECOMP_OVERLAY_HINT_SIZE - 1u, "a long hint is cut to fit");

    recomp_overlay_clear(&text);
    recomp_overlay_set_hint(&text, "F1 leave");
    check(!recomp_overlay_empty(&text), "a hint alone is not empty");

    check(recomp_overlay_shown() == recomp_overlay_shown(), "there is one shown overlay");

    if (failures == 0) printf("overlay text test passed\n");
    return failures == 0 ? 0 : 1;
}
