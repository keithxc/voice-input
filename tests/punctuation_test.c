#include "punctuation.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void localise(char *text) { vi_punctuation_localise(text, 256); }

int main(void) {
    /* An English sentence comes back from the model in Chinese punctuation. */
    char english[256] = "please check the return value，then call free。";
    localise(english);
    assert(strcmp(english, "please check the return value, then call free.") == 0);

    char question[256] = "why is this stream paused？";
    localise(question);
    assert(strcmp(question, "why is this stream paused?") == 0);

    /* One Han character means the sentence is Chinese; leave every mark alone,
       including the ones inside its English words. */
    char mixed[256] = "这个 buffer，需要 free。";
    localise(mixed);
    assert(strcmp(mixed, "这个 buffer，需要 free。") == 0);

    /* Nothing to rewrite, nothing to lose. */
    char plain[256] = "already ascii, thank you.";
    localise(plain);
    assert(strcmp(plain, "already ascii, thank you.") == 0);

    char empty[256] = "";
    localise(empty);
    assert(empty[0] == '\0');

    puts("punctuation tests passed");
    return 0;
}
