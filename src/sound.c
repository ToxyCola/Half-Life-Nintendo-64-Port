// HL64 - sound: GoldSrc-style entity channels, 3D spatialization (snd_dma.c S_Spatialize),
// sentences.txt speech (the engine's VOX system), ambient loops and CD-track music.
// Half-Life's WAVs and MP3s are converted to libdragon wav64 (ULC codec) at build time.
#include <libdragon.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "game.h"

#define NUM_MIX      18          // mixer channels: 0..NUM_DYN-1 for sounds, then music
#define NUM_DYN      16
#define MUSIC_CH     16
#define MAX_SLOTS    56          // logical sounds (loops out of earshot keep a slot, not a mixer channel)
#define MAX_CACHED   24
#define MAX_WORDS    32
#define NOMINAL_CLIP 1000.0f     // sound_nominal_clip_dist
#define OUT_GAIN     0.55f

// ------------------------------------------------------------------ wave cache
typedef struct { char name[56]; wav64_t wav; bool ok; int refs; uint32_t used; } wave_t;
static wave_t cache[MAX_CACHED];
static uint32_t use_clock;
static bool audio_ok;

static void lower_copy(char *d, const char *s, int n)
{
    int i = 0;
    for (; s[i] && i < n - 1; i++) d[i] = (s[i] == '\\') ? '/' : tolower((unsigned char)s[i]);
    d[i] = 0;
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// "doors/doormove1.wav" -> cache entry (NULL if the file isn't on the cartridge)
static wave_t *wave_get(const char *sample)
{
    char nm[56];
    while (*sample == '/' || *sample == '\\') sample++;   // a few maps write "/ambience/..."
    lower_copy(nm, sample, sizeof(nm));
    use_clock++;
    int freeslot = -1;
    for (int i = 0; i < MAX_CACHED; i++) {
        if (cache[i].name[0] && !strcmp(cache[i].name, nm)) {
            cache[i].used = use_clock;
            return cache[i].ok ? &cache[i] : NULL;
        }
        if (!cache[i].name[0] && freeslot < 0) freeslot = i;
    }
    if (freeslot < 0) {
        // evict the least recently used wave nobody is playing
        uint32_t best = 0xffffffff;
        for (int i = 0; i < MAX_CACHED; i++)
            if (cache[i].refs == 0 && cache[i].used < best) { best = cache[i].used; freeslot = i; }
        if (freeslot < 0) return NULL;
        if (cache[freeslot].ok) wav64_close(&cache[freeslot].wav);
        cache[freeslot].name[0] = 0;
    }
    wave_t *w = &cache[freeslot];
    strcpy(w->name, nm);
    w->used = use_clock;
    w->refs = 0;
    char path[96];
    snprintf(path, sizeof(path), "rom:/sound/%s64", nm);   // x.wav -> x.wav64
    w->ok = file_exists(path);
    if (w->ok) wav64_open(&w->wav, path);
    else debugf("snd: missing %s\n", nm);
    return w->ok ? w : NULL;
}

// ------------------------------------------------------------------ sentences.txt
typedef struct { char name[24]; const char *text; } sentence_t;
static sentence_t *sents;
static int num_sents;
static char *sent_buf;

static void sentences_load(void)
{
    FILE *f = fopen("rom:/sound/sentences.txt", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    int len = ftell(f);
    fseek(f, 0, SEEK_SET);
    sent_buf = malloc(len + 1);
    fread(sent_buf, 1, len, f);
    fclose(f);
    sent_buf[len] = 0;
    int lines = 1;
    for (int i = 0; i < len; i++) if (sent_buf[i] == '\n') lines++;
    sents = malloc(sizeof(sentence_t) * lines);
    char *p = sent_buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        char *cr = strchr(p, '\r');
        if (cr) *cr = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && !(p[0] == '/' && p[1] == '/')) {
            char *q = p;
            while (*q && *q != ' ' && *q != '\t') q++;
            int n = q - p;
            if (n > 0 && n < 24) {
                sentence_t *s = &sents[num_sents++];
                memcpy(s->name, p, n);
                s->name[n] = 0;
                while (*q == ' ' || *q == '\t') q++;
                s->text = q;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    debugf("snd: %d sentences\n", num_sents);
}

static int sentence_find(const char *name)
{
    for (int i = 0; i < num_sents; i++) if (!strcasecmp(sents[i].name, name)) return i;
    return -1;
}

// SENTENCEG_PlayRndSz: a random member of a group ("BA_OK" -> BA_OK0..BA_OKn), avoiding
// the one picked last time
static int sentence_group_pick(const char *group)
{
    static char last_group[24];
    static int last_pick = -1;
    int gl = strlen(group);
    int cand[64], nc = 0;
    for (int i = 0; i < num_sents && nc < 64; i++) {
        if (strncasecmp(sents[i].name, group, gl)) continue;
        const char *r = sents[i].name + gl;
        if (!*r) continue;
        bool digits = true;
        for (; *r; r++) if (!isdigit((unsigned char)*r)) digits = false;
        if (digits) cand[nc++] = i;
    }
    if (!nc) return -1;
    int pick = cand[rand() % nc];
    if (nc > 1 && pick == last_pick && !strcasecmp(last_group, group)) pick = cand[(rand() % (nc - 1) + 1 + (pick == cand[0] ? 0 : 0)) % nc];
    strncpy(last_group, group, 23);
    last_pick = pick;
    return pick;
}

int snd_sentence_group_count(const char *group)
{
    int gl = strlen(group), n = 0;
    for (int i = 0; i < num_sents; i++)
        if (!strncasecmp(sents[i].name, group, gl) && isdigit((unsigned char)sents[i].name[gl])) n++;
    return n;
}

typedef struct { char file[48]; uint8_t pitch, vol, start, end; } word_t;

static void parse_params(const char **pp, int *pitch, int *vol, int *start, int *end)
{
    const char *p = *pp + 1;   // past '('
    while (*p && *p != ')') {
        char c = tolower((unsigned char)*p);
        if ((c == 'p' || c == 'v' || c == 's' || c == 'e' || c == 't') && isdigit((unsigned char)p[1])) {
            int v = atoi(p + 1);
            if (c == 'p') *pitch = v;
            else if (c == 'v') *vol = v;
            else if (c == 's') *start = v;
            else if (c == 'e') *end = v;
            p++;
            while (isdigit((unsigned char)*p)) p++;
        } else p++;
    }
    if (*p == ')') p++;
    *pp = p;
}

// VOX_ParseString: words, (params) for the word they follow or, standing alone, for
// the words after them, and ',' / '.' as pauses
static int sentence_words(const char *text, word_t *out, int max)
{
    char dir[24] = "vox/";
    const char *p = text;
    // a directory prefix on the first word sets it for the whole sentence
    const char *sl = NULL;
    for (const char *q = p; *q && *q != ' ' && *q != '('; q++) if (*q == '/') sl = q;
    if (sl) {
        int n = sl - p + 1;
        if (n > 23) n = 23;
        memcpy(dir, p, n);
        dir[n] = 0;
        p = sl + 1;
    }
    int dp = 100, dv = 100, ds = 0, de = 100;
    int n = 0;
    while (*p && n < max) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (*p == '(') { parse_params(&p, &dp, &dv, &ds, &de); continue; }
        if (*p == ',' || *p == '.') {
            word_t *w = &out[n++];
            snprintf(w->file, sizeof(w->file), "%s%s.wav", dir, *p == ',' ? "_comma" : "_period");
            w->pitch = dp; w->vol = dv; w->start = 0; w->end = 100;
            p++;
            continue;
        }
        char word[40];
        int k = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '(' && *p != ',' && *p != '.' && k < 39) word[k++] = *p++;
        word[k] = 0;
        int pi = dp, vo = dv, st = ds, en = de;
        if (*p == '(') parse_params(&p, &pi, &vo, &st, &en);
        word_t *w = &out[n++];
        snprintf(w->file, sizeof(w->file), "%s%s.wav", dir, word);
        w->pitch = pi > 255 ? 255 : pi; w->vol = vo > 255 ? 255 : vo; w->start = st; w->end = en;
    }
    return n;
}

// ------------------------------------------------------------------ channels
typedef struct {
    bool active;
    int ent, chan;
    float vol, attn;
    int pitch;
    float origin[3];
    bool has_origin;    // fixed position (ambient sounds) instead of following the entity
    wave_t *wave;
    int mix;            // mixer channel, or -1 while inaudible (loops only)
    bool loop, started;
    // sentences
    word_t *words;
    int nwords, word;
    float word_end;     // stop the current word at this sample position (e < 100)
    char sname[24];
} slot_t;

static slot_t slots[MAX_SLOTS];
static int mix_owner[NUM_DYN];        // slot using each mixer channel, -1 free
static float lis_org[3], lis_right[3] = {0, -1, 0};
static float master_volume = 0.8f;
static int music_track;
static wave_t music_wave;
static bool music_open;
static double music_resume = -1;   // position to pick the track up from after a level load
static bool music_reopen;

void snd_set_volume(float v) { master_volume = v; }
float snd_master_volume(void) { return master_volume; }

void snd_init(void)
{
#ifdef NOAUDIO
    sentences_load();
    return;
#endif
    audio_init(22050, AUDIO_DEFAULT_LATENCY);
    mixer_init(NUM_MIX);
    wav64_init_compression(2);
    mixer_ch_set_limits(MUSIC_CH, 16, 22050, 0);   // allocate the music channel's buffer now, not mid-game
    for (int i = 0; i < NUM_DYN; i++) {
        mix_owner[i] = -1;
        mixer_ch_set_limits(i, 16, 22050 * 2.0f, 0);    // pitch up to 200% (higher is clamped)
    }
    sentences_load();
    // The mixer allocates each channel's sample buffer the first time something plays on it.
    // Do that for every channel now, so those buffers don't end up between the big per-map
    // allocations later (which fragments the heap until a large map no longer fits).
    if (file_exists("rom:/sound/common/null.wav64")) {
        wav64_t w;
        wav64_open(&w, "rom:/sound/common/null.wav64");
        for (int ch = 0; ch < NUM_MIX; ch++) {
            wav64_play(&w, ch);
            mixer_ch_set_vol(ch, 0, 0);
            mixer_ch_stop(ch);
        }
        wav64_close(&w);
    }
    audio_ok = true;
}

static void slot_release_mix(slot_t *s)
{
    if (s->mix >= 0) {
        mixer_ch_stop(s->mix);
        mix_owner[s->mix] = -1;
        s->mix = -1;
    }
}

static void slot_free(slot_t *s)
{
    slot_release_mix(s);
    if (s->wave) { s->wave->refs--; s->wave = NULL; }
    if (s->words) { free(s->words); s->words = NULL; }
    s->active = false;
}

// left/right gains for a slot (S_Spatialize)
static void spatialize(slot_t *s, float *l, float *r)
{
    float v = s->vol * master_volume * OUT_GAIN;
    if (s->ent == 0 || s->attn <= 0) { *l = *r = v; return; }   // the player's own sounds / ATTN_NONE
    float o[3];
    if (s->has_origin) memcpy(o, s->origin, 12);
    else if (!ents_sound_origin(s->ent, o)) memcpy(o, s->origin, 12);
    else memcpy(s->origin, o, 12);
    float d[3] = {o[0] - lis_org[0], o[1] - lis_org[1], o[2] - lis_org[2]};
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float dot = 0;
    if (len > 0.1f) dot = (d[0] * lis_right[0] + d[1] * lis_right[1] + d[2] * lis_right[2]) / len;
    float dist = len * s->attn / NOMINAL_CLIP;
    float sc = 1.0f - dist;
    if (sc < 0) sc = 0;
    sc *= sc;   // perceptual falloff: distant sounds drop off faster than GoldSrc's linear ramp
    *l = v * sc * (1.0f - dot);
    *r = v * sc * (1.0f + dot);
    if (*l > 1) *l = 1;
    if (*r > 1) *r = 1;
}

static int alloc_mix(void)
{
    for (int i = 0; i < NUM_DYN; i++) if (mix_owner[i] < 0) return i;
    // all busy: steal the quietest plain sound (never speech)
    int best = -1;
    float bv = 1e9f;
    for (int i = 0; i < NUM_DYN; i++) {
        slot_t *s = &slots[mix_owner[i]];
        if (s->words) continue;
        float l, r;
        spatialize(s, &l, &r);
        if (l + r < bv) { bv = l + r; best = i; }
    }
    if (best < 0) return -1;
    slot_t *s = &slots[mix_owner[best]];
    if (s->loop) slot_release_mix(s);       // goes virtual, restarts when there is room
    else slot_free(s);
    return best;
}

static void start_wave(slot_t *s, int ch, wave_t *w, int pitch, float gain_l, float gain_r, int startpct)
{
    mix_owner[ch] = s - slots;
    s->mix = ch;
    wav64_play(&w->wav, ch);
    float fq = w->wav.wave.frequency * pitch / 100.0f;
    mixer_ch_set_freq(ch, fq > 22050 * 2.0f ? 22050 * 2.0f : fq);
    mixer_ch_set_vol(ch, gain_l, gain_r);
    if (startpct > 0 && startpct < 100) wav64_seek(&w->wav, ch, (double)w->wav.wave.len * startpct / 100.0 / w->wav.wave.frequency);
}

// start the next word of a sentence; false when it's over
static bool sentence_next_word(slot_t *s)
{
    while (s->word < s->nwords) {
        word_t *wd = &s->words[s->word++];
        wave_t *w = wave_get(wd->file);
        if (!w) continue;
        if (s->wave) s->wave->refs--;
        s->wave = w;
        w->refs++;
        float l, r;
        spatialize(s, &l, &r);
        float wv = wd->vol / 100.0f;
        int ch = s->mix;
        if (ch < 0) { ch = alloc_mix(); if (ch < 0) return false; }
        start_wave(s, ch, w, wd->pitch * s->pitch / 100, l * wv, r * wv, wd->start);
        s->word_end = wd->end < 100 ? w->wav.wave.len * wd->end / 100.0f : 1e30f;
        return true;
    }
    return false;
}

static slot_t *slot_new(int ent, int chan)
{
    // a sound on an entity channel replaces what that channel was playing (CHAN_AUTO / STATIC stack)
    if (chan != CHAN_AUTO && chan != CHAN_STATIC)
        for (int i = 0; i < MAX_SLOTS; i++)
            if (slots[i].active && slots[i].ent == ent && slots[i].chan == chan) slot_free(&slots[i]);
    for (int i = 0; i < MAX_SLOTS; i++) if (!slots[i].active) { memset(&slots[i], 0, sizeof(slot_t)); slots[i].mix = -1; return &slots[i]; }
    return NULL;
}

static int play_sentence_index(int ent, int chan, int si, float vol, float attn, int pitch, const float *origin)
{
    word_t tmp[MAX_WORDS];
    int n = sentence_words(sents[si].text, tmp, MAX_WORDS);
    if (!n) return -1;
    slot_t *s = slot_new(ent, chan);
    if (!s) return -1;
    s->active = true;
    s->ent = ent; s->chan = chan; s->vol = vol; s->attn = attn; s->pitch = pitch > 0 ? pitch : 100;
    if (origin) { memcpy(s->origin, origin, 12); s->has_origin = ent < 0; }
    s->words = malloc(sizeof(word_t) * n);
    memcpy(s->words, tmp, sizeof(word_t) * n);
    s->nwords = n;
    strncpy(s->sname, sents[si].name, 23);
#ifdef NPCDEBUG
    debugf("say %s (ent %d)\n", sents[si].name, ent);
#endif
    if (!sentence_next_word(s)) { slot_free(s); return -1; }
    return s - slots;
}

// EMIT_SOUND_DYN. sample: "x/y.wav", or "!NAME" for a sentence. ent < 0: a fixed position
int snd_emit(int ent, int chan, const char *sample, float vol, float attn, int pitch, const float *origin)
{
#ifdef NOSOUND
    return -1;
#endif
    if (!audio_ok || !sample || !sample[0] || vol <= 0) return -1;
    if (sample[0] == '!') {
        int si = sentence_find(sample + 1);
        if (si < 0) { debugf("snd: no sentence %s\n", sample); return -1; }
        return play_sentence_index(ent, chan, si, vol, attn, pitch, origin);
    }
    if (sample[0] == '*') sample++;     // "*" = streaming flag in GoldSrc
    wave_t *w = wave_get(sample);
    if (!w) return -1;
    slot_t tmp = {0};
    tmp.ent = ent; tmp.vol = vol; tmp.attn = attn;
    if (origin) { memcpy(tmp.origin, origin, 12); tmp.has_origin = ent < 0; }
    float l, r;
    spatialize(&tmp, &l, &r);
    bool loop = w->wav.wave.loop_len > 0;
    if (!loop && l + r < 0.002f) return -1;            // out of earshot: nothing to hear
    slot_t *s = slot_new(ent, chan);
    if (!s) return -1;
    *s = tmp;
    s->active = true;
    s->chan = chan;
    s->pitch = pitch > 0 ? pitch : 100;
    s->wave = w;
    w->refs++;
    s->loop = loop;
    s->mix = -1;
    if (l + r >= 0.002f) {
        int ch = alloc_mix();
        if (ch < 0) { if (!loop) { slot_free(s); return -1; } }
        else start_wave(s, ch, w, s->pitch, l, r, 0);
    }
    s->started = true;
    return s - slots;
}

// a sentence group or "!NAME" (SENTENCEG_PlayRndSz / EMIT_SOUND_DYN with '!')
int snd_sentence(int ent, const char *name, float vol, float attn, int pitch)
{
    if (!audio_ok || !name) return -1;
    if (name[0] == '!') return snd_emit(ent, CHAN_VOICE, name, vol, attn, pitch, NULL);
    int si = sentence_group_pick(name);
    if (si < 0) { debugf("snd: no sentence group %s\n", name); return -1; }
    return play_sentence_index(ent, CHAN_VOICE, si, vol, attn, pitch, NULL);
}

void snd_stop(int ent, int chan, const char *sample)
{
    char nm[56] = "";
    if (sample) lower_copy(nm, sample[0] == '*' ? sample + 1 : sample, sizeof(nm));
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &slots[i];
        if (!s->active || s->ent != ent) continue;
        if (chan >= 0 && s->chan != chan) continue;
        if (sample && sample[0] == '!') { if (strcasecmp(s->sname, sample + 1)) continue; }
        else if (sample && (!s->wave || s->words || strcmp(s->wave->name, nm))) continue;
        slot_free(s);
    }
}

bool snd_playing(int ent, int chan)
{
    for (int i = 0; i < MAX_SLOTS; i++)
        if (slots[i].active && slots[i].ent == ent && (chan < 0 || slots[i].chan == chan)) return true;
    return false;
}

bool snd_slot_playing(int id) { return id >= 0 && id < MAX_SLOTS && slots[id].active; }

// SND_CHANGE_PITCH / SND_CHANGE_VOL on a playing sound
void snd_adjust(int id, float vol, int pitch)
{
    if (id < 0 || id >= MAX_SLOTS || !slots[id].active) return;
    slot_t *s = &slots[id];
    if (vol >= 0) s->vol = vol;
    if (pitch > 0 && pitch != s->pitch) {
        s->pitch = pitch;
        if (s->mix >= 0 && s->wave && !s->words) mixer_ch_set_freq(s->mix, fminf(s->wave->wav.wave.frequency * pitch / 100.0f, 22050 * 2.0f));
    }
}

// the player's own sounds (footsteps, UI): non-spatial
void snd_play(const char *sample, float volume, int channel)
{
    snd_emit(0, channel, sample, volume, ATTN_NORM, PITCH_NORM, NULL);
}

// ------------------------------------------------------------------ music (CD tracks)
static const char *track_names[] = {
    NULL, NULL, "half-life01", "prospero01", "half-life12", "half-life07", "half-life10", "suspense01",
    "suspense03", "half-life09", "half-life02", "half-life13", "half-life04", "half-life15", "half-life14",
    "half-life16", "suspense02", "half-life03", "half-life08", "prospero02", "half-life05", "prospero04",
    "half-life11", "half-life06", "prospero03", "half-life17", "prospero05", "suspense05", "suspense07",
};

void snd_music_stop(void)
{
    if (!audio_ok) return;
    mixer_ch_stop(MUSIC_CH);
    music_resume = -1;
    music_reopen = false;
    if (music_open) { wav64_close(&music_wave.wav); music_open = false; }
    music_track = 0;
}

void snd_music_play(int track)
{
    if (!audio_ok) return;
    snd_music_stop();
    if (track < 2 || track >= (int)(sizeof(track_names) / sizeof(track_names[0]))) return;
    char path[64];
    snprintf(path, sizeof(path), "rom:/music/%s.wav64", track_names[track]);
    if (!file_exists(path)) return;
    wav64_open(&music_wave.wav, path);
    music_open = true;
    music_track = track;
    wav64_play(&music_wave.wav, MUSIC_CH);
}

int snd_music_track(void) { return music_track; }

// ------------------------------------------------------------------ per frame
void snd_update(const float *listener, const float *right)
{
    if (!audio_ok) return;
    if (listener) memcpy(lis_org, listener, 12);
    if (right) memcpy(lis_right, right, 12);
    extern int trace_frames;
    for (int i = 0; i < MAX_SLOTS; i++) {
        slot_t *s = &slots[i];
        if (!s->active) continue;
        float l, r;
        spatialize(s, &l, &r);
        if (s->mix >= 0) {
            if (s->words && s->word_end < 1e29f && mixer_ch_get_pos(s->mix) >= s->word_end) mixer_ch_stop(s->mix);
            if (!mixer_ch_playing(s->mix)) {
                if (s->words && sentence_next_word(s)) continue;
                slot_free(s);
                continue;
            }
            if (s->loop && l + r < 0.001f) { slot_release_mix(s); continue; }
            float wv = s->words ? s->words[s->word - 1].vol / 100.0f : 1.0f;
            mixer_ch_set_vol_ramp(s->mix, l * wv, r * wv, 256);
        } else if (s->loop && l + r >= 0.002f) {
            int ch = alloc_mix();
            if (ch >= 0) start_wave(s, ch, s->wave, s->pitch, l, r, 0);
        } else if (!s->loop) {
            slot_free(s);
        }
    }
    if (music_reopen) {
        music_reopen = false;
        char path[64];
        snprintf(path, sizeof(path), "rom:/music/%s.wav64", track_names[music_track]);
        wav64_open(&music_wave.wav, path);
        music_open = true;
    }
    if (music_open && music_resume >= 0) {
        wav64_play(&music_wave.wav, MUSIC_CH);
        if (music_resume > 0) wav64_seek(&music_wave.wav, MUSIC_CH, music_resume / music_wave.wav.wave.frequency);
        music_resume = -1;
    }
    float mv = master_volume * 0.7f;
    mixer_ch_set_vol(MUSIC_CH, mv, mv);
    if (music_open && !mixer_ch_playing(MUSIC_CH)) snd_music_stop();   // CD tracks play once
    mixer_try_play();
}

// silence everything (before a level load, which stalls the main loop for a moment)
void snd_stop_all(void)
{
    if (!audio_ok) return;
    for (int i = 0; i < MAX_SLOTS; i++) if (slots[i].active) slot_free(&slots[i]);
    // music carries on after the load, from where it was (closed meanwhile so its buffers
    // don't sit in the middle of the heap while the next map loads)
    if (music_open && music_resume < 0 && mixer_ch_playing(MUSIC_CH)) music_resume = mixer_ch_get_pos(MUSIC_CH);
    for (int ch = 0; ch < NUM_MIX; ch++) mixer_ch_stop(ch);
    if (music_open) { wav64_close(&music_wave.wav); music_open = false; music_reopen = music_resume >= 0; }
    mixer_try_play();
    // drop the cache too: sounds are reopened as needed, so their allocations don't sit
    // between the big per-map buffers
    for (int i = 0; i < MAX_CACHED; i++) {
        if (cache[i].name[0] && cache[i].ok) wav64_close(&cache[i].wav);
        cache[i].name[0] = 0;
        cache[i].refs = 0;
    }
}

// keep the mixer fed during long loads
void snd_poll(void)
{
    if (audio_ok) mixer_try_play();
}
