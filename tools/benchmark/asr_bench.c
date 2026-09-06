#define _POSIX_C_SOURCE 200809L

/* Offline benchmark for the recognition path.
 *
 * It replays recorded clips through the same src/asr.c the daemon uses and
 * reports both halves of the question a model change raises: is the text
 * better, and is it still fast enough. Accuracy is a character error rate for
 * Han characters and a word error rate for Latin words, kept apart so a mixed
 * sentence cannot hide which half regressed. Latency is measured with the
 * audio paced at realtime, because feeding a wav file as fast as the CPU
 * allows measures throughput and says nothing about what a speaker waits for.
 */

#include "asr.h"
#include "punctuation.h"
#include "score.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>

#define MAX_TAGS 32
#define TAG_LENGTH 24
#define TEXT_LENGTH 8192

struct latency {
    double onset_ms;
    double first_partial_ms;
    double partial_interval_ms;
    double final_ms;
    double punctuation_ms;
    unsigned partials;
};

struct bucket {
    char tag[TAG_LENGTH];
    size_t clips;
    size_t scored;
    struct vi_score score;
    struct latency total;
};

struct collector {
    char text[TEXT_LENGTH];
    size_t length;
    struct timespec started;
    double first_partial_ms;
    double last_partial_ms;
    double partial_span_ms;
    unsigned partials;
};

static double ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start->tv_sec) * 1000.0 +
           (double)(now.tv_nsec - start->tv_nsec) / 1e6;
}

/* A recording starts before the speaker does. Timing the first partial from
   the head of the file would charge the model for that silence, so find where
   speech actually begins: the first 20 ms window that rises clearly above the
   quietest part of the clip. */
static double speech_onset_ms(const float *samples, int32_t count) {
    const int32_t window = 320;  /* 20 ms at 16 kHz */
    if (count < window) return 0.0;
    const int32_t windows = count / window;
    double quietest = 1.0;
    for (int32_t i = 0; i < windows; ++i) {
        double squares = 0.0;
        for (int32_t j = 0; j < window; ++j) {
            const double sample = samples[i * window + j];
            squares += sample * sample;
        }
        const double rms = squares / window;
        if (rms < quietest) quietest = rms;
    }
    const double threshold = quietest * 16.0 > 1e-5 ? quietest * 16.0 : 1e-5;
    for (int32_t i = 0; i < windows; ++i) {
        double squares = 0.0;
        for (int32_t j = 0; j < window; ++j) {
            const double sample = samples[i * window + j];
            squares += sample * sample;
        }
        if (squares / window > threshold) return (double)i * 20.0;
    }
    return 0.0;
}

static void sleep_until(const struct timespec *start, double target_ms) {
    const double remaining = target_ms - ms_since(start);
    if (remaining <= 0.0) return;
    const struct timespec delay = {
        .tv_sec = (time_t)(remaining / 1000.0),
        .tv_nsec = (long)((remaining - (double)(long)(remaining / 1000.0) * 1000.0) * 1e6),
    };
    nanosleep(&delay, NULL);
}

static void on_transcript(const char *event, const char *text, void *userdata) {
    struct collector *collector = userdata;
    const double now = ms_since(&collector->started);
    if (strcmp(event, "partial") == 0) {
        if (collector->partials == 0) collector->first_partial_ms = now;
        else collector->partial_span_ms = now - collector->first_partial_ms;
        ++collector->partials;
        collector->last_partial_ms = now;
        return;
    }
    if (text == NULL || text[0] == '\0') return;
    const size_t room = sizeof(collector->text) - collector->length;
    const int written = snprintf(collector->text + collector->length, room,
                                 "%s%s", collector->length > 0 ? " " : "", text);
    if (written < 0) return;
    collector->length = (size_t)written >= room
                            ? sizeof(collector->text) - 1
                            : collector->length + (size_t)written;
}

static struct bucket *bucket_for(struct bucket *buckets, size_t *count,
                                 const char *tag, size_t length) {
    if (length == 0 || length >= TAG_LENGTH) return NULL;
    for (size_t i = 0; i < *count; ++i) {
        if (strncmp(buckets[i].tag, tag, length) == 0 &&
            buckets[i].tag[length] == '\0') {
            return &buckets[i];
        }
    }
    if (*count >= MAX_TAGS) return NULL;
    struct bucket *bucket = &buckets[(*count)++];
    memcpy(bucket->tag, tag, length);
    bucket->tag[length] = '\0';
    return bucket;
}

static void add_latency(struct latency *total, const struct latency *one) {
    total->onset_ms += one->onset_ms;
    total->first_partial_ms += one->first_partial_ms;
    total->partial_interval_ms += one->partial_interval_ms;
    total->final_ms += one->final_ms;
    total->punctuation_ms += one->punctuation_ms;
    total->partials += one->partials;
}

static void tally(struct bucket *buckets, size_t *count, const char *tags,
                  const struct vi_score *score, const struct latency *latency) {
    /* score is NULL for a clip with no reference: it still times the model. */
    const char *cursor = tags;
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        const size_t length = comma != NULL ? (size_t)(comma - cursor)
                                            : strlen(cursor);
        struct bucket *bucket = bucket_for(buckets, count, cursor, length);
        if (bucket != NULL) {
            ++bucket->clips;
            if (score != NULL) {
                ++bucket->scored;
                vi_score_accumulate(&bucket->score, score);
            }
            add_latency(&bucket->total, latency);
        }
        if (comma == NULL) break;
        cursor = comma + 1;
    }
}

static void print_class(const char *label, const struct vi_score_class *counts) {
    const double rate = vi_score_rate(counts);
    if (rate < 0.0) {
        printf("  %s      -", label);
        return;
    }
    printf("  %s %5.1f%% (S%zu D%zu I%zu / %zu)", label, rate * 100.0,
           counts->substitutions, counts->deletions, counts->insertions,
           counts->reference);
}

static void print_summary(const char *label, size_t clips, size_t scored,
                          const struct vi_score *score,
                          const struct latency *total) {
    printf("%-14s %3zu clips", label, clips);
    if (scored > 0) {
        print_class("CER", &score->han);
        print_class("WER", &score->word);
    } else {
        printf("  not scored     ");
    }
    const double count = clips > 0 ? (double)clips : 1.0;
    printf("  partial %4.0f ms  final %4.0f ms", total->first_partial_ms / count,
           total->final_ms / count);
    if (total->punctuation_ms > 0.0) {
        printf("  punct %3.0f ms", total->punctuation_ms / count);
    }
    putchar('\n');
}

/* Splits "path\ttags\treference" in place. */
static int split_manifest_line(char *line, char **path, char **tags,
                               char **reference) {
    char *end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r')) *--end = '\0';
    if (line[0] == '\0' || line[0] == '#') return -1;
    char *first = strchr(line, '\t');
    if (first == NULL) return -1;
    *first++ = '\0';
    char *second = strchr(first, '\t');
    if (second == NULL) return -1;
    *second++ = '\0';
    *path = line;
    *tags = first;
    *reference = second;
    return **reference == '\0' ? -1 : 0;
}

struct run {
    struct vi_asr *asr;
    struct vi_punctuation *punctuation;
    struct collector *collector;
    bool realtime;
};

static int score_clip(const struct run *run, const char *wav_path,
                      double *audio_seconds, double *decode_seconds,
                      struct latency *latency) {
    const SherpaOnnxWave *wave = SherpaOnnxReadWave(wav_path);
    if (wave == NULL || wave->samples == NULL || wave->num_samples <= 0) {
        fprintf(stderr, "cannot read %s\n", wav_path);
        if (wave != NULL) SherpaOnnxFreeWave(wave);
        return -1;
    }
    if (wave->sample_rate != 16000) {
        fprintf(stderr, "%s is %d Hz, expected 16000 Hz\n", wav_path,
                wave->sample_rate);
        SherpaOnnxFreeWave(wave);
        return -1;
    }
    *audio_seconds = (double)wave->num_samples / 16000.0;

    latency->onset_ms = speech_onset_ms(wave->samples, wave->num_samples);
    struct collector *collector = run->collector;
    memset(collector, 0, sizeof(*collector));
    clock_gettime(CLOCK_MONOTONIC, &collector->started);

    const int32_t chunk = 1600;  /* 100 ms, the daemon's own feed size */
    double decode_ms = 0.0;
    int status = 0;
    for (int32_t offset = 0; offset < wave->num_samples; offset += chunk) {
        int32_t count = wave->num_samples - offset;
        if (count > chunk) count = chunk;
        if (run->realtime) {
            sleep_until(&collector->started,
                        (double)(offset + count) / 16000.0 * 1000.0);
        }
        struct timespec before;
        clock_gettime(CLOCK_MONOTONIC, &before);
        if (vi_asr_accept(run->asr, wave->samples + offset, (size_t)count) < 0) {
            status = -1;
            break;
        }
        decode_ms += ms_since(&before);
    }
    struct timespec speech_end;
    clock_gettime(CLOCK_MONOTONIC, &speech_end);
    vi_asr_finish(run->asr);
    latency->final_ms = ms_since(&speech_end);
    decode_ms += latency->final_ms;
    *decode_seconds = decode_ms / 1000.0;

    /* Measured from speech onset, not from the head of the file, so the
       number is what a speaker waits for rather than how much silence the
       recording happened to start with. */
    latency->first_partial_ms = collector->first_partial_ms - latency->onset_ms;
    if (latency->first_partial_ms < 0.0) latency->first_partial_ms = 0.0;
    latency->partials = collector->partials;
    latency->partial_interval_ms =
        collector->partials > 1
            ? collector->partial_span_ms / (double)(collector->partials - 1)
            : 0.0;

    if (run->punctuation != NULL && collector->text[0] != '\0') {
        char punctuated[TEXT_LENGTH];
        struct timespec before;
        clock_gettime(CLOCK_MONOTONIC, &before);
        const int done = vi_punctuation_apply(run->punctuation, collector->text,
                                              punctuated, sizeof(punctuated));
        latency->punctuation_ms = ms_since(&before);
        if (done == 0) {
            memcpy(collector->text, punctuated, sizeof(collector->text));
        }
    }
    SherpaOnnxFreeWave(wave);
    return status;
}

static void usage(void) {
    fprintf(stderr,
            "usage: voice-input-asr-bench [options] MANIFEST\n"
            "\n"
            "  --model DIR         streaming ASR model (or VOICE_INPUT_ASR_MODEL)\n"
            "  --punct-model DIR   punctuation model; off when unset\n"
            "  --decoder NAME      greedy_search or modified_beam_search\n"
            "  --threads N         inference threads, default 2\n"
            "  --hotwords FILE     hotwords, modified_beam_search only\n"
            "  --float             prefer float weights over int8\n"
            "  --fast              feed as fast as possible; latency then\n"
            "                      measures throughput, not what a speaker waits\n"
            "  --verbose           print every transcript, not only the wrong ones\n"
            "\n"
            "MANIFEST holds one clip per line: path<TAB>tags<TAB>reference.\n"
            "Paths are resolved against the manifest's own directory. A\n"
            "reference of \"-\" times the clip without scoring it.\n");
}

int main(int argc, char **argv) {
    struct vi_asr_config config;
    vi_asr_config_defaults(&config);
    config.model_directory = getenv("VOICE_INPUT_ASR_MODEL");
    if (config.model_directory == NULL) {
        config.model_directory = getenv("VOICE_INPUT_MODEL_DIR");
    }
    const char *punctuation_directory = getenv("VOICE_INPUT_PUNCT_MODEL");
    if (punctuation_directory == NULL) {
        punctuation_directory = getenv("VOICE_INPUT_PUNCT_MODEL_DIR");
    }
    const char *manifest_path = NULL;
    bool verbose = false;
    bool realtime = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            config.model_directory = argv[++i];
        } else if (strcmp(argv[i], "--punct-model") == 0 && i + 1 < argc) {
            punctuation_directory = argv[++i];
        } else if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) {
            config.decoding_method = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            config.threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hotwords") == 0 && i + 1 < argc) {
            config.hotwords_file = argv[++i];
        } else if (strcmp(argv[i], "--float") == 0) {
            config.prefer_int8 = false;
        } else if (strcmp(argv[i], "--fast") == 0) {
            realtime = false;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (argv[i][0] != '-' && manifest_path == NULL) {
            manifest_path = argv[i];
        } else {
            usage();
            return EXIT_FAILURE;
        }
    }
    if (manifest_path == NULL || config.model_directory == NULL ||
        config.model_directory[0] == '\0') {
        usage();
        return EXIT_FAILURE;
    }

    FILE *manifest = fopen(manifest_path, "r");
    if (manifest == NULL) {
        fprintf(stderr, "cannot open %s\n", manifest_path);
        return EXIT_FAILURE;
    }
    char directory[4096];
    snprintf(directory, sizeof(directory), "%s", manifest_path);
    char *slash = strrchr(directory, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        directory[0] = '.';
        directory[1] = '\0';
    }

    struct collector collector = {0};
    struct timespec loading;
    clock_gettime(CLOCK_MONOTONIC, &loading);
    struct vi_asr *asr = vi_asr_create(&config, on_transcript, &collector);
    if (asr == NULL) {
        fprintf(stderr, "cannot load the model in %s\n", config.model_directory);
        fclose(manifest);
        return EXIT_FAILURE;
    }
    const double asr_load_ms = ms_since(&loading);
    double punctuation_load_ms = 0.0;
    struct vi_punctuation *punctuation = NULL;
    if (punctuation_directory != NULL && punctuation_directory[0] != '\0') {
        clock_gettime(CLOCK_MONOTONIC, &loading);
        punctuation = vi_punctuation_create(punctuation_directory, 1);
        punctuation_load_ms = ms_since(&loading);
        if (punctuation == NULL) {
            fprintf(stderr, "cannot load the punctuation model in %s\n",
                    punctuation_directory);
            vi_asr_destroy(asr);
            fclose(manifest);
            return EXIT_FAILURE;
        }
    }
    const struct run run = { .asr = asr, .punctuation = punctuation,
                             .collector = &collector, .realtime = realtime };

    struct bucket buckets[MAX_TAGS] = {0};
    size_t bucket_count = 0;
    struct vi_score total = {0};
    struct latency total_latency = {0};
    size_t clips = 0;
    size_t scored = 0;
    size_t failures = 0;
    double audio_total = 0.0;
    double decode_total = 0.0;
    char *line = NULL;
    size_t line_size = 0;

    printf("model        %s (%s, %s, %d threads, %s weights)\n",
           vi_asr_model_name(asr), vi_asr_model_kind(asr), vi_asr_decoder(asr),
           vi_asr_threads(asr), config.prefer_int8 ? "int8" : "float");
    printf("punctuation  %s\n",
           punctuation != NULL ? vi_punctuation_model(punctuation) : "off");
    printf("load         %.0f ms ASR", asr_load_ms);
    if (punctuation != NULL) printf(", %.0f ms punctuation", punctuation_load_ms);
    printf("\nfeed         %s\n\n", realtime ? "paced at realtime" : "as fast as possible");

    while (getline(&line, &line_size, manifest) > 0) {
        char *relative = NULL;
        char *tags = NULL;
        char *reference = NULL;
        if (split_manifest_line(line, &relative, &tags, &reference) < 0) continue;

        char wav_path[4096];
        int written = relative[0] == '/'
            ? snprintf(wav_path, sizeof(wav_path), "%s", relative)
            : snprintf(wav_path, sizeof(wav_path), "%s/%s", directory, relative);
        if (written < 0 || (size_t)written >= sizeof(wav_path)) {
            ++failures;
            continue;
        }

        double audio_seconds = 0.0;
        double decode_seconds = 0.0;
        struct latency latency = {0};
        if (score_clip(&run, wav_path, &audio_seconds, &decode_seconds,
                       &latency) < 0) {
            ++failures;
            continue;
        }
        /* A reference of "-" means the clip has never been transcribed. It
           still measures the model's speed, which is worth having for a
           recording nobody has written out. */
        const bool has_reference = strcmp(reference, "-") != 0;
        struct vi_score score;
        if (has_reference && vi_score_compare(reference, collector.text, &score) < 0) {
            fprintf(stderr, "cannot score %s\n", relative);
            ++failures;
            continue;
        }

        ++clips;
        if (has_reference) {
            ++scored;
            vi_score_accumulate(&total, &score);
        }
        audio_total += audio_seconds;
        decode_total += decode_seconds;
        add_latency(&total_latency, &latency);
        tally(buckets, &bucket_count, tags, has_reference ? &score : NULL,
              &latency);

        const size_t errors =
            has_reference
                ? vi_score_errors(&score.han) + vi_score_errors(&score.word)
                : 0;
        printf("%-24s [%s]", relative, tags);
        if (has_reference) {
            print_class("CER", &score.han);
            print_class("WER", &score.word);
        } else {
            printf("  not scored     ");
        }
        printf("  partial %4.0f ms  final %4.0f ms", latency.first_partial_ms,
               latency.final_ms);
        if (punctuation != NULL) printf("  punct %3.0f ms", latency.punctuation_ms);
        printf("  %.2fx\n", audio_seconds > 0.0 ? decode_seconds / audio_seconds : 0.0);
        if (errors > 0 || verbose || !has_reference) {
            if (verbose) printf("    onset %.0f ms\n", latency.onset_ms);
            if (has_reference) printf("    ref %s\n", reference);
            printf("    hyp %s\n",
                   collector.text[0] != '\0' ? collector.text : "(nothing)");
        }
    }
    free(line);
    fclose(manifest);
    vi_asr_destroy(asr);
    vi_punctuation_destroy(punctuation);

    if (clips == 0) {
        fprintf(stderr, "no clip was scored\n");
        return EXIT_FAILURE;
    }

    putchar('\n');
    for (size_t i = 0; i < bucket_count; ++i) {
        print_summary(buckets[i].tag, buckets[i].clips, buckets[i].scored,
                      &buckets[i].score, &buckets[i].total);
    }
    print_summary("TOTAL", clips, scored, &total, &total_latency);

    struct rusage usage_after;
    getrusage(RUSAGE_SELF, &usage_after);
    const double cpu_seconds =
        (double)usage_after.ru_utime.tv_sec +
        (double)usage_after.ru_utime.tv_usec / 1e6 +
        (double)usage_after.ru_stime.tv_sec +
        (double)usage_after.ru_stime.tv_usec / 1e6;
    printf("\n%.1f s of audio, %.1f s decoding (%.2fx realtime), "
           "%.1f s CPU, %ld MB peak RSS\n",
           audio_total, decode_total,
           audio_total > 0.0 ? decode_total / audio_total : 0.0, cpu_seconds,
           usage_after.ru_maxrss / 1024);
    printf("mean partial interval %.0f ms over %u partials\n",
           total_latency.partials > 0
               ? total_latency.partial_interval_ms / (double)clips
               : 0.0,
           total_latency.partials);
    if (failures > 0) {
        fprintf(stderr, "%zu clip(s) could not be scored\n", failures);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
