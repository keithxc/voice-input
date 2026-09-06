#ifndef VOICE_INPUT_PUNCTUATION_H
#define VOICE_INPUT_PUNCTUATION_H

#include <stddef.h>

/* Punctuation restoration for a finished utterance. The recogniser emits bare
   text; this turns it into something that can be read. It runs on the final
   path only -- a partial is redrawn several times a second and repunctuating
   each redraw would make the overlay flicker for no gain.

   The engine is created once, when the daemon starts, and never reloaded. */

struct vi_punctuation;

/* model_directory holds a ct-transformer punctuation model. Returns NULL when
   the model is missing or fails to load; the daemon then commits unpunctuated
   text rather than refusing to run. */
struct vi_punctuation *vi_punctuation_create(const char *model_directory,
                                             int threads);
void vi_punctuation_destroy(struct vi_punctuation *punctuation);

/* Writes the punctuated text into buffer. On any failure the buffer receives
   the input text unchanged and -1 is returned, so a caller can commit the
   result either way. */
int vi_punctuation_apply(struct vi_punctuation *punctuation, const char *text,
                         char *buffer, size_t size);

/* Rewrites Chinese punctuation as ASCII when the text holds no Han character,
   because the model punctuates English sentences in Chinese. Applied by
   vi_punctuation_apply(); exposed separately so it can be tested without a
   model. Leaves the text alone when it is not purely Latin. */
void vi_punctuation_localise(char *text, size_t size);

/* File name of the loaded model, for the status reply. */
const char *vi_punctuation_model(const struct vi_punctuation *punctuation);

#endif
