/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DIGISNAKE-DT: DIGISNAKE (a Cartesian step sequencer after MDD Snake and Make
 * Noise's Rene) for the Digitakt mk1, OS 1.53 and 1.54.
 *
 * Eight steps sit in a 4 x 2 grid that matches the encoders: A-D are the top
 * row, E-H the bottom. A path (the "snake") walks the grid in one of seven
 * shapes, and each sequencer step of the track moves it on (or every DIV
 * steps). Where it lands decides the step:
 *
 *   page 1  NOTES   each cell's note, -24..+24 semitones from the track's
 *                   NOTE, quantised to SCALE (rooted on the track's NOTE)
 *   page 2  GATES   each cell fires or rests
 *   page 3  VELOC   each cell's velocity, 1-127
 *   page 4  PATH    ON, SHAPE, DIR, LEN, DIV, SCALE, RANGE, SHIFT
 *
 * With SNAKE on, the track plays the snake in place of its own trigs (lock
 * trigs still play). Every track has its own: the 8 audio tracks (a note is
 * the sample's pitch, as a note lock) and the 8 MIDI tracks. Nothing is saved
 * with the project yet (the Digitakt's core has no project data): PRINT the
 * snake into the track to keep it.
 *
 * The Snake page opens with TRIG pressed on the TRIG page (press TRIG twice).
 * The Digitakt's core cannot add a page to a view, so DIGISNAKE-DT draws its
 * page over the screen and takes the keys and knobs while it is open; TRIG,
 * another page key, or any screen opening over the pages (a menu) closes it.
 *   knobs A-H        the cells of the page shown (PATH: its eight settings)
 *   LEFT / RIGHT     the previous / next page
 *   UP / DOWN        NOTES: all notes up/down a semitone (FUNC: an octave);
 *                    GATES: rotate; VELOC: all velocities up/down
 *   trig keys 1-8    GATES page: toggle that cell (with TRK held: stock)
 *   hold TRIG + YES  randomise the page shown (PATH: all but SNAKE and SCALE)
 *   hold TRIG + NO   back to how it was when TRIG went down (a print too)
 *   hold TRIG + PLAY clear the page shown
 *   hold TRIG + REC / STOP  copy the page shown / paste it (another track)
 *   hold TRIG + FUNC + REC  PRINT: the snake into the track's own trigs and
 *                    notes and velocities, then SNAKE off
 *   FUNC + YES       randomise NOTES, GATES and VELOC together
 *   FUNC + NO        back to before the last run of FUNC + YES
 *   FUNC + knob      jump: NOTES -24/-12/0/+12/+24, VELOC 1/32/64/96/127,
 *                    PATH's RANGE 1/12/24
 *   TRIG on its own  back to the TRIG page
 *
 * The sequencer side uses seq-events (step-fire events), as the Digitone's
 * DIGISNAKE does. The firmware's pattern pointer and transport come from the
 * same per-OS addresses seq-events uses (mod.json's cflags); the UI reads the
 * key dispatcher's screens to know when the TRIG page is up.
 */
#include "digitakt-mk1/core3.h"
#include "digitakt-mk1/seq.h"

#ifndef SNK_PATTERN_PP
#error "build with -DSNK_PATTERN_PP and -DSNK_TRANSPORT (mod.json cflags)"
#endif
#define TRANSPORT   (*(volatile uint32_t *)SNK_TRANSPORT)        /* 0 stopped, 1 playing */
#define PATTERN     (*(uint8_t *const volatile *)SNK_PATTERN_PP) /* the active pattern */

#define TRACKS    SQ_TRACKS     /* 0-7 the audio tracks, 8-15 the MIDI tracks */
#define CELLS     8
#define NOTE_MAX  24

/* ---- each track's snake ----------------------------------------------- */
enum { P_ON, P_SHAPE, P_DIR, P_LEN, P_DIV, P_SCALE, P_RANGE, P_SHIFT, NSET };

struct snake {
    signed char note[CELLS];    /* semitones from NOTE */
    unsigned char vel[CELLS];   /* 1-127 */
    unsigned char set[NSET];    /* the PATH page */
    unsigned char gate;         /* bit c: cell c fires */
    unsigned char pad[7];
};                              /* 32 bytes, as the Digitone's */

struct snake digisnake_data[TRACKS];

#define SHAPES 7
static const unsigned char shapes[SHAPES][CELLS] = {
    {0, 1, 2, 3, 4, 5, 6, 7},   /* ROWS */
    {0, 1, 2, 3, 7, 6, 5, 4},   /* SNAKE */
    {0, 4, 1, 5, 2, 6, 3, 7},   /* COLS */
    {0, 4, 5, 1, 2, 6, 7, 3},   /* ZIGZAG */
    {0, 5, 2, 7, 4, 1, 6, 3},   /* DIAG */
    {0, 3, 7, 4, 1, 2, 6, 5},   /* CORNER */
    {0, 6, 1, 7, 2, 4, 3, 5},   /* HOP */
};
static const char *const shape_names[SHAPES] = {
    "ROWS", "SNAKE", "COLS", "ZIGZAG", "DIAG", "CORNER", "HOP"
};

enum { D_FWD, D_BWD, D_FB, D_BF, D_RND, DIRS };
static const char *const dir_names[DIRS] = { "FWD", "BWD", "F/B", "B/F", "RND" };

#define SCALES 12
static const unsigned short scale_mask[SCALES] = {
    0xfff, 0xab5, 0x5ad, 0x6ad, 0x5ab, 0xad5, 0x6b5, 0x9ad, 0x295, 0x4a9, 0x4e9, 0x555
};
static const char *const scale_names[SCALES] = {
    "CHR", "MAJ", "MIN", "DOR", "PHR", "LYD", "MIX", "HMIN", "PENT", "mPNT", "BLUE", "WHOL"
};

static const unsigned char set_min[NSET] = { 0, 0, 0, 1, 0, 0, 1, 0 };
static const unsigned char set_max[NSET] = { 1, SHAPES - 1, DIRS - 1, CELLS, 8, SCALES - 1, NOTE_MAX, CELLS - 1 };
static const char *const set_labels[NSET] = { "SNAKE", "SHAPE", "DIR", "LEN", "DIV", "SCALE", "RANGE", "SHIFT" };

/* DIV: sequencer steps a snake step (1 = 1X, 2 = 1/2, 4 = 1/4, 8 = 1/8), or 0:
 * 2X, the track itself at twice its speed (per-track SCALE mode) and the snake
 * a step every step, as on the Digitone. */
#define DIVS 5
static const unsigned char div_list[DIVS] = { 8, 4, 2, 1, 0 };
static const char *div_name(int v)
{
    switch (v) {
    case 0: return "2X";
    case 1: return "1X";
    case 2: return "1/2";
    case 4: return "1/4";
    case 8: return "1/8";
    default: return "1/?";
    }
}
static int div_turn(int v, long d)
{
    int i, best = 3;                    /* 1X */
    for (i = 0; i < DIVS; i++)
        if (div_list[i] == v)
            best = i;
    best += (int)d;
    if (best < 0) best = 0;
    if (best > DIVS - 1) best = DIVS - 1;
    return div_list[best];
}

static void track_defaults(struct snake *s)
{
    static const signed char demo[CELLS] = { 0, 3, 7, 10, 12, 10, 7, 3 };
    int c;
    for (c = 0; c < CELLS; c++) {
        s->note[c] = demo[c];
        s->vel[c] = 100;
    }
    s->gate = 0xff;
    s->set[P_ON] = 0;
    s->set[P_SHAPE] = 1;        /* SNAKE */
    s->set[P_DIR] = D_FWD;
    s->set[P_LEN] = CELLS;
    s->set[P_DIV] = 1;
    s->set[P_SCALE] = 0;
    s->set[P_RANGE] = 12;
    s->set[P_SHIFT] = 0;
    for (c = 0; c < 7; c++)
        s->pad[c] = 0;
}

static unsigned char data_ready;
static volatile unsigned char reset_req[TRACKS];
/* Starting from STOP: the sequencer prepares step 1 while stopped (a fire with
 * the transport at 0); the first fire after PLAY lines the path up with its
 * step, so step 1 is always the path's start (DIGISNAKE 1.2). */
static volatile unsigned char prep[TRACKS], rewind[TRACKS];

/* ---- playback (interrupt level, in the sequencer's step fire) ---------- */
struct play {
    unsigned char k;            /* the path step, 0..len-1 */
    unsigned char count;        /* sequencer steps since the last move */
    signed char pp;             /* ping-pong's way */
    unsigned char cell;         /* the cell the step plays */
    unsigned char moved;        /* this step is a snake step (DIV: the others rest) */
};
static struct play play[TRACKS];
volatile unsigned char digisnake_cell[TRACKS];      /* for the screen */
static unsigned long irq_rand = 0x2545f491UL;

static unsigned long xorshift(unsigned long *s)
{
    unsigned long x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x2545f491UL;
    return *s;
}

static int track_on(int32_t track)
{
    return data_ready && (uint32_t)track < TRACKS && digisnake_data[track].set[P_ON];
}

static void step_on(struct play *p, const struct snake *s, int len, unsigned long *rnd)
{
    int k = p->k;
    switch (s->set[P_DIR]) {
    case D_BWD:
        k = k > 0 ? k - 1 : len - 1;
        break;
    case D_FB:
    case D_BF:
        if (len < 2) {
            k = 0;
            break;
        }
        if (p->pp >= 0) {
            if (k + 1 >= len) { p->pp = -1; k = len - 2; } else k++;
        } else {
            if (k <= 0) { p->pp = 1; k = 1; } else k--;
        }
        break;
    case D_RND:
        k = (int)(xorshift(rnd) % (unsigned long)len);
        break;
    default:
        k = k + 1 < len ? k + 1 : 0;
        break;
    }
    if (k >= len)
        k = 0;
    p->k = (unsigned char)k;
}

static void start(struct play *p, const struct snake *s, int len)
{
    p->count = 0;
    p->moved = 1;
    if (s->set[P_DIR] == D_BWD || s->set[P_DIR] == D_BF) {
        p->k = (unsigned char)(len - 1);
        p->pp = -1;
    } else {
        p->k = 0;
        p->pp = 1;
    }
}

static void advance(struct play *p, const struct snake *s, int len, unsigned long *rnd)
{
    if (++p->count >= s->set[P_DIV]) {
        p->count = 0;
        p->moved = 1;
        step_on(p, s, len, rnd);
    } else {
        p->moved = 0;                       /* DIV 1/2 .. 1/8: between snake steps */
    }
}

static int cell_of(const struct snake *s, int k, int len)
{
    int i = (k + s->set[P_SHIFT]) % len;
    return shapes[s->set[P_SHAPE] % SHAPES][i];
}

static int snake_len(const struct snake *s)
{
    int len = s->set[P_LEN];
    return len < 1 || len > CELLS ? CELLS : len;
}

void digisnake_trig(int32_t track, int32_t step, int32_t first, uint32_t *trig)
{
    const struct snake *s;
    struct play *p;
    int len;
    if (!track_on(track))
        return;
    s = &digisnake_data[track];
    p = &play[track];
    len = snake_len(s);
    if (first) {
        if (TRANSPORT == 0) {               /* stopped: prepare the first step */
            prep[track] = 1;
            rewind[track] = reset_req[track] = 0;
            start(p, s, len);
        } else if (prep[track] || rewind[track]) {
            int n;                          /* the first fire after PLAY */
            prep[track] = rewind[track] = reset_req[track] = 0;
            start(p, s, len);
            for (n = 0; n < step && n < SQ_STEPS; n++)
                advance(p, s, len, &irq_rand);
        } else if (reset_req[track]) {
            reset_req[track] = 0;
            start(p, s, len);
        } else {
            advance(p, s, len, &irq_rand);
        }
        if (p->k >= len)
            p->k = 0;
        p->cell = (unsigned char)cell_of(s, p->k, len);
        digisnake_cell[track] = p->cell;
    }
    if (p->moved && ((s->gate >> p->cell) & 1))
        *trig |= SQ_TRIG;
    else
        *trig &= ~SQ_TRIG;
}

static int quantise(int note, int root, int scale)
{
    unsigned short m;
    int i, deg;
    if (scale <= 0 || scale >= SCALES)
        return note;
    m = scale_mask[scale];
    for (i = 0; i < 12; i++) {              /* down to the nearest scale tone */
        deg = (note - root - i) % 12;
        if (deg < 0)
            deg += 12;
        if ((m >> deg) & 1)
            return note - i;
    }
    return note;
}

/* The track's NOTE and the scale's root: the Digitakt keeps no keyboard key,
 * so a scale is rooted on the track's own NOTE. */
static int track_base(int32_t track, int *root)
{
    struct sq_track info;
    int base = 60;
    if (sq_track_info(track, &info, sizeof info) == 0)
        base = info.note;
    *root = base % 12;
    return base;
}

static int cell_note(const struct snake *s, int base, int root, int cell)
{
    int n = quantise(base + s->note[cell], root, s->set[P_SCALE]);
    return n < 0 ? 0 : n > 127 ? 127 : n;
}

void digisnake_notes(int32_t track, int32_t step, int32_t generated, int8_t notes[SQ_NOTES])
{
    int root, base;
    (void)step;
    (void)generated;
    if (!track_on(track))
        return;
    base = track_base(track, &root);
    notes[0] = (int8_t)cell_note(&digisnake_data[track], base, root, play[track].cell);
}

void digisnake_vel(int32_t track, int32_t step, int32_t *vel)
{
    (void)step;
    if (!track_on(track))
        return;
    *vel = digisnake_data[track].vel[play[track].cell];
}

/* ---- 2X: the track's own speed --------------------------------------------
 * The pattern keeps each track's SCALE at +0x38b (measured in digiemu: 1X 2,
 * 1/2X 4), used in per-track SCALE mode (the pattern's settings at +0x1ec40,
 * their +0x19 == 1): an index into the stock speeds 2X, 3/2X, 1X, 3/4X, 1/2X,
 * 1/4X, 1/8X, as on the Digitone. DIV 2X halves the track's step: 1X -> 2X,
 * 1/2X -> 1X and so on; switching 2X or SNAKE off, or the pattern changing,
 * puts it back. */
#define TRACK_STRIDE  911
#define TR_SPEED      0x38b
#define PAT_MODE      (0x1ec40 + 0x19)
static uint8_t *mod_pat[TRACKS];            /* the pattern we sped up, 0 none */
static signed char mod_orig[TRACKS];        /* its track's speed before */

static int per_track_mode(void)
{
    uint8_t *pat = PATTERN;
    return pat && pat[PAT_MODE] == 1;
}

static int speed_half(int i)                /* the index of half the ticks */
{
    static const signed char h[7] = { 0, 1, 0, 1, 2, 4, 5 };
    return i >= 0 && i < 7 ? h[i] : i;
}

static void speed_sync(void)
{
    uint8_t *pat = PATTERN;
    int t, pt = pat && pat[PAT_MODE] == 1;
    for (t = 0; t < TRACKS; t++) {
        struct snake *s = &digisnake_data[t];
        int want = pt && s->set[P_ON] && s->set[P_DIV] == 0;
        if (mod_pat[t] && (mod_pat[t] != pat || !want)) {       /* put it back */
            uint8_t *tr = mod_pat[t] + TRACK_STRIDE * t;
            if ((signed char)tr[TR_SPEED] == speed_half(mod_orig[t]))
                tr[TR_SPEED] = (uint8_t)mod_orig[t];
            mod_pat[t] = 0;
        }
        if (want && !mod_pat[t]) {
            uint8_t *tr = pat + TRACK_STRIDE * t;
            int o = (signed char)tr[TR_SPEED], h;
            if ((h = speed_half(o)) != o) {
                mod_orig[t] = (signed char)o;
                tr[TR_SPEED] = (uint8_t)h;
                mod_pat[t] = pat;
            }
        }
    }
}

/* ---- PRINT (TRIG + FUNC + REC): the snake into the track's steps ------------
 * The pattern (seq-events-dt1's dev.h): 911 bytes a track, its step words
 * big-endian u16 at +0 (bit 0 the trig, 0x0080 the SYN trig and 0x0800 its
 * override of the track's default at +0x382), note lock 1 a step at +0x280
 * (-1: NOTE), NOTE at +0x384, and each step's VEL at +0x80 (255: the track's;
 * measured in digiemu: a VEL lock on step 1 wrote +0x80, on step 4 +0x83). A
 * snake step that fires becomes a note trig with the snake's note as its note
 * lock and the snake's velocity as its VEL; a rest clears a note trig and
 * keeps a lock trig. A 2X speed stays. Playing, it carries on from where the snake is; stopped,
 * it starts the path on step 1, as PLAY does. TRIG + NO, TRIG still held,
 * puts the track back. */
#define TR_VEL        0x080                 /* a step's VEL, 255: the track's */
#define TR_NOTE_LOCK  0x280
#define TR_DEF_TRIGS  0x382
#define W_TRIG        0x0001
#define W_SYN         0x0080
#define W_SYN_SET     0x0800

static uint8_t print_bak[TRACK_STRIDE];
static signed char print_t = -1;            /* the track print_bak holds, -1 none */
static uint8_t *print_pat;

static int word_syn(const uint8_t *trk, unsigned w)
{
    return (w & W_SYN_SET) ? (w & W_SYN) != 0 : (trk[TR_DEF_TRIGS + 1] & W_SYN) != 0;
}

static int print_track(int t)
{
    uint8_t *pat = PATTERN, *trk;
    struct snake *s = &digisnake_data[t];
    struct sq_track info;
    struct play q;
    unsigned long rnd = 0x9e3779b9UL ^ irq_rand;
    int len, plen, base, root, step, i, first, playing, pending;
    uint32_t last;
    if (!pat || sq_track_info(t, &info, sizeof info) != 0)
        return -1;
    len = info.len;
    if (len < 1 || len > SQ_STEPS)
        return -1;
    base = track_base(t, &root);
    trk = pat + TRACK_STRIDE * t;
    for (i = 0; i < TRACK_STRIDE; i++)
        print_bak[i] = trk[i];
    print_t = (signed char)t;
    print_pat = pat;
    plen = snake_len(s);
    last = sq_clock[t].last;
    playing = TRANSPORT == 1 && s->set[P_ON] && last < (uint32_t)len;
    pending = !playing || reset_req[t] || prep[t] || rewind[t];
    first = playing ? ((int)last + 1) % len : 0;
    q = play[t];
    for (i = 0; i < len; i++) {
        int cell;
        unsigned w;
        step = (first + i) % len;
        if (i == 0 && pending)
            start(&q, s, plen);
        else
            advance(&q, s, plen, &rnd);
        if (q.k >= plen)
            q.k = 0;
        cell = cell_of(s, q.k, plen);
        w = (unsigned)(trk[2 * step] << 8 | trk[2 * step + 1]);
        if (q.moved && ((s->gate >> cell) & 1)) {
            w |= W_TRIG;
            if (!word_syn(trk, w))
                w |= W_SYN_SET | W_SYN;     /* a note trig, whatever the default */
            trk[TR_NOTE_LOCK + step] = (uint8_t)cell_note(s, base, root, cell);
            trk[TR_VEL + step] = s->vel[cell];
        } else if ((w & W_TRIG) && word_syn(trk, w)) {
            w &= ~(unsigned)W_TRIG;         /* a rest: no note trig (lock trigs stay) */
        }
        trk[2 * step] = (uint8_t)(w >> 8);
        trk[2 * step + 1] = (uint8_t)w;
    }
    mod_pat[t] = 0;                         /* keep the 2X the print was made at */
    s->set[P_ON] = 0;
    sq_rearm(1u << t);
    return len;
}

static void unprint(int t)
{
    uint8_t *trk;
    int i;
    if (print_t != t || !print_pat || print_pat != PATTERN)
        return;
    trk = print_pat + TRACK_STRIDE * t;
    for (i = 0; i < TRACK_STRIDE; i++)
        trk[i] = print_bak[i];
    print_t = -1;
    sq_rearm(1u << t);
}

/* ---- the screens: is the TRIG page up? ----------------------------------
 * The key dispatcher's brain (each key event's first argument) keeps the
 * screens shown: +0x54 the bottom one (the page views), +0x5c the top one
 * (another while a menu or a browser is open over the pages). The bottom
 * one's +8 is the page manager, whose +0xb4 is the view shown (the Digitone's
 * keeps it at +0xc0); a view's +136 is its page key. Measured in digiemu on
 * OS 1.53: TRIG 19, FLTR 21; a menu (GLOBAL) moves +0x5c. */
#define BR_BOTTOM 0x54
#define BR_TOP    0x5c
#define SCR_PM    8
#define PM_VIEW   0xb4
#define VIEW_KEY  136

#define KEY_FUNC  1
#define KEY_TRK   2
#define KEY_REC   9
#define KEY_PLAY  10
#define KEY_STOP  11
#define KEY_YES   12
#define KEY_NO    13
#define KEY_UP    14
#define KEY_DOWN  15
#define KEY_LEFT  16
#define KEY_RIGHT 17
#define KEY_PAGE  18
#define KEY_TRIG  19
#define KEY_LFO   23
#define KEY_STEP1 24
#define EV_KEY(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_FLAGS(ev) (*(unsigned long *)((char *)(ev) + 16))
#define EV_ENC(ev)   (*(unsigned long *)((char *)(ev) + 12))
#define EV_DELTA(ev) (*(long *)((char *)(ev) + 16))
#define F_DOWN    1
#define F_FUNC    2
#define F_REPEAT  8
#define F_UP      0x10

static uint32_t rd(uint32_t a)
{
    return a >= 0x40000000u && a < 0x48000000u ? *(volatile uint32_t *)a : 0;
}

/* the page key whose view is on screen with nothing over it, or 0 */
static uint32_t page_view_key(void *brain)
{
    uint32_t b = (uint32_t)brain, bottom, pm, view;
    if (!b || !(bottom = rd(b + BR_BOTTOM)) || bottom != rd(b + BR_TOP))
        return 0;
    if (!(pm = rd(bottom + SCR_PM)) || !(view = rd(pm + PM_VIEW)))
        return 0;
    return rd(view + VIEW_KEY);
}

/* ---- the Snake page ----------------------------------------------------- */
enum { PG_NOTES, PG_GATES, PG_VEL, PG_PATH, PAGES };
static const char *const page_names[PAGES] = { "NOTES", "GATES", "VELOCITY", "PATH" };
static const char *const what_names[PAGES + 1] = { "Notes", "Gates", "Velocity", "Path", "Snake" };

unsigned char digisnake_open;           /* the page is up */
unsigned char digisnake_pg;             /* the page shown */
void *digisnake_brain;
static unsigned char trig_held, trig_used, stock_held, stock_used;
static unsigned char func_held, trk_held;
static unsigned char redraw;
static long enc_acc[8];
#define COUNTS    4
static unsigned long ui_rand = 0x9e3779b9UL;
static unsigned long frames;            /* UI frames: the random numbers' seed */
static const char *msg;                 /* a short message on the page's bottom line */
static unsigned char msg_ticks;

static void say(const char *m)
{
    msg = m;
    msg_ticks = 60;
    redraw = 1;
}

static int ui_track(void)
{
    uint32_t t = fw_active_track;
    return t < TRACKS ? (int)t : -1;
}

static struct snake hold_copy[TRACKS], run_copy[TRACKS];
static unsigned char hold_valid[TRACKS], run_valid[TRACKS], run_active[TRACKS];

static void randomise(struct snake *s, int pg)
{
    int c, r = s->set[P_RANGE];
    if (r < 1 || r > NOTE_MAX)
        r = 12;
    ui_rand ^= frames * 2654435761UL;
    for (c = 0; c < CELLS; c++) {
        unsigned long x = xorshift(&ui_rand);
        if (pg == PG_NOTES)
            s->note[c] = (signed char)((long)(x % (unsigned long)(2 * r + 1)) - r);
        else if (pg == PG_VEL)
            s->vel[c] = (unsigned char)(40 + x % 88);
        else if (pg == PG_GATES) {
            if ((x >> 16) % 100 < 65)
                s->gate |= (unsigned char)(1u << c);
            else
                s->gate &= (unsigned char)~(1u << c);
        }
    }
    if (pg == PG_GATES && !s->gate)
        s->gate = (unsigned char)(1u << (xorshift(&ui_rand) % CELLS));
    if (pg == PG_PATH) {
        static const unsigned char divs[8] = { 1, 1, 1, 1, 1, 2, 2, 4 };
        s->set[P_SHAPE] = (unsigned char)(xorshift(&ui_rand) % SHAPES);
        s->set[P_DIR] = (unsigned char)(xorshift(&ui_rand) % DIRS);
        s->set[P_LEN] = (unsigned char)(3 + xorshift(&ui_rand) % (CELLS - 2));
        s->set[P_DIV] = divs[xorshift(&ui_rand) & 7];
        s->set[P_RANGE] = (unsigned char)(5 + xorshift(&ui_rand) % (NOTE_MAX - 4));
        s->set[P_SHIFT] = (unsigned char)(xorshift(&ui_rand) % CELLS);
    }
}

static void reset_page(struct snake *s, int pg)
{
    struct snake d;
    int c;
    track_defaults(&d);
    for (c = 0; c < CELLS; c++) {
        if (pg == PG_NOTES)
            s->note[c] = 0;
        else if (pg == PG_VEL)
            s->vel[c] = d.vel[c];
    }
    if (pg == PG_GATES)
        s->gate = 0xff;
    if (pg == PG_PATH) {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++)
            s->set[c] = d.set[c];
        s->set[P_ON] = on;
    }
}

static int clampi(long v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : (int)v;
}

static void nudge_page(struct snake *s, int pg, int d, int func)
{
    int c;
    if (pg == PG_NOTES) {
        for (c = 0; c < CELLS; c++)
            s->note[c] = (signed char)clampi(s->note[c] + d * (func ? 12 : 1), -NOTE_MAX, NOTE_MAX);
    } else if (pg == PG_VEL) {
        for (c = 0; c < CELLS; c++)
            s->vel[c] = (unsigned char)clampi(s->vel[c] + d * 8, 1, 127);
    } else if (pg == PG_GATES) {
        unsigned g = s->gate;
        g = d > 0 ? ((g << 1) | (g >> 7)) : ((g >> 1) | (g << 7));
        s->gate = (unsigned char)g;
    }
}

static struct snake clip;
static signed char clip_pg = -1;

static void paste_page(struct snake *s, const struct snake *from, int pg)
{
    int c;
    if (pg == PG_NOTES)
        for (c = 0; c < CELLS; c++) s->note[c] = from->note[c];
    else if (pg == PG_VEL)
        for (c = 0; c < CELLS; c++) s->vel[c] = from->vel[c];
    else if (pg == PG_GATES)
        s->gate = from->gate;
    else {
        unsigned char on = s->set[P_ON];
        for (c = 0; c < NSET; c++) s->set[c] = from->set[c];
        s->set[P_ON] = on;
    }
}

static void copy_snake(struct snake *to, const struct snake *from)
{
    const unsigned char *f = (const unsigned char *)from;
    unsigned char *d = (unsigned char *)to;
    unsigned i;
    for (i = 0; i < sizeof *to; i++)
        d[i] = f[i];
}

static void reload(int t, const struct snake *from, int valid)
{
    if (!valid)
        return;
    copy_snake(&digisnake_data[t], from);
    reset_req[t] = 1;
    say("SNAKE RELOADED");
}

static int next_stop(int v, long d, const signed char *stops, int n)
{
    int i;
    if (d > 0) {
        for (i = 0; i < n; i++)
            if (stops[i] > v)
                return stops[i];
        return stops[n - 1];
    }
    for (i = n - 1; i >= 0; i--)
        if (stops[i] < v)
            return stops[i];
    return stops[0];
}
static const signed char note_stops[5] = { -24, -12, 0, 12, 24 };
static const signed char vel_stops[5] = { 1, 32, 64, 96, 127 };
static const signed char range_stops[3] = { 1, 12, 24 };

static void open_page(void)
{
    digisnake_open = 1;
    trig_held = trig_used = 0;
    redraw = 1;
}

static void close_page(void)
{
    digisnake_open = 0;
    trig_held = 0;
    redraw = 1;
}

/* A key while the page is open: -> 1 to take it. */
static int page_key(int t, unsigned long key, unsigned long flags)
{
    struct snake *s = &digisnake_data[t];
    int down = (flags & F_DOWN) && !(flags & F_REPEAT);
    redraw = 1;
    if (key == KEY_TRIG) {
        if ((flags & F_UP) || !(flags & F_DOWN)) {      /* its release */
            if (trig_held && !trig_used)
                close_page();                           /* back to the TRIG page */
            trig_held = 0;
            return 1;
        }
        if (down) {
            trig_held = 1;
            trig_used = 0;
            print_t = -1;
            copy_snake(&hold_copy[t], s);
            hold_valid[t] = 1;
        }
        return 1;
    }
    if (key > KEY_TRIG && key <= KEY_LFO) {             /* another page key: to its page */
        close_page();
        return 0;
    }
    if (key == KEY_PLAY || key == KEY_STOP || key == KEY_REC) {
        if (!trig_held)
            return 0;                                   /* the transport, as ever */
    }
    if (!down)
        return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN
            || key == KEY_YES || key == KEY_NO
            || (key >= KEY_STEP1 && key < KEY_STEP1 + CELLS && digisnake_pg == PG_GATES && !trk_held);

    if (key == KEY_YES && ((flags & F_FUNC) || func_held)) {    /* randomise all */
        trig_used = 1;
        if (!run_active[t]) {
            copy_snake(&run_copy[t], s);
            run_valid[t] = run_active[t] = 1;
        }
        randomise(s, PG_NOTES);
        randomise(s, PG_GATES);
        randomise(s, PG_VEL);
        say("SNAKE RANDOMISED");
        return 1;
    }
    if (key == KEY_NO && trig_held) {
        trig_used = 1;
        unprint(t);
        reload(t, &hold_copy[t], hold_valid[t]);
        run_active[t] = 0;
        return 1;
    }
    if (key == KEY_NO && ((flags & F_FUNC) || func_held)) {
        trig_used = 1;
        reload(t, &run_copy[t], run_valid[t]);
        run_active[t] = 0;
        return 1;
    }
    if (key == KEY_YES && trig_held) {
        trig_used = 1;
        randomise(s, digisnake_pg);
        if (digisnake_pg == PG_PATH)
            reset_req[t] = 1;
        run_active[t] = 0;
        say("PAGE RANDOMISED");
        return 1;
    }
    if (key == KEY_REC && trig_held && ((flags & F_FUNC) || func_held)) {     /* PRINT */
        trig_used = 1;
        if (print_track(t) < 0) {
            say("NO PATTERN TO PRINT TO");
        } else {
            reset_req[t] = 1;
            run_active[t] = 0;
            say("PRINTED, SNAKE OFF");
        }
        return 1;
    }
    if (key == KEY_REC && trig_held) {                                       /* COPY */
        trig_used = 1;
        copy_snake(&clip, s);
        clip_pg = (signed char)digisnake_pg;
        say("PAGE COPIED");
        return 1;
    }
    if (key == KEY_STOP && trig_held) {                                      /* PASTE */
        trig_used = 1;
        if (clip_pg < 0) {
            say("NOTHING TO PASTE");
            return 1;
        }
        digisnake_pg = (unsigned char)clip_pg;
        paste_page(s, &clip, clip_pg);
        reset_req[t] = 1;
        run_active[t] = 0;
        say("PAGE PASTED");
        return 1;
    }
    if (key == KEY_PLAY && trig_held) {                                      /* CLEAR */
        trig_used = 1;
        reset_page(s, digisnake_pg);
        reset_req[t] = 1;
        run_active[t] = 0;
        say("PAGE CLEARED");
        return 1;
    }
    if (key == KEY_LEFT) {
        digisnake_pg = (unsigned char)(digisnake_pg ? digisnake_pg - 1 : PAGES - 1);
        return 1;
    }
    if (key == KEY_RIGHT) {
        digisnake_pg = (unsigned char)((digisnake_pg + 1) % PAGES);
        return 1;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        nudge_page(s, digisnake_pg, key == KEY_UP ? 1 : -1, (flags & F_FUNC) || func_held);
        run_active[t] = 0;
        return 1;
    }
    if (digisnake_pg == PG_GATES && !trk_held && key >= KEY_STEP1 && key < KEY_STEP1 + CELLS) {
        s->gate ^= (unsigned char)(1u << (key - KEY_STEP1));
        run_active[t] = 0;
        return 1;
    }
    return key == KEY_YES || key == KEY_NO;
}

int digisnake_key(void *brain, void *ev)
{
    unsigned long key = EV_KEY(ev), flags = EV_FLAGS(ev);
    int t, down = (flags & F_DOWN) && !(flags & F_REPEAT);
    digisnake_brain = brain;
    /* FUNC and TRK, followed always, never taken: a release is 0x10, or 0 when
     * another key went down meanwhile */
    if (key == KEY_FUNC) {
        if (!(flags & F_REPEAT))
            func_held = (unsigned char)(flags & F_DOWN);
        return 0;
    }
    if (key == KEY_TRK) {
        if (!(flags & F_REPEAT))
            trk_held = (unsigned char)(flags & F_DOWN);
        return 0;
    }
    if (flags & F_FUNC)
        func_held = 1;
    t = ui_track();
    if (digisnake_open)
        return t < 0 ? (close_page(), 0) : page_key(t, key, flags);
    /* TRIG on the TRIG page: ours, so the page opens however long TRIG was
     * held; another key while it is down leaves it to that key. */
    if (stock_held) {
        if (key == KEY_TRIG) {
            if ((flags & F_UP) || !(flags & F_DOWN)) {
                stock_held = 0;
                if (!stock_used && t >= 0)
                    open_page();
            }
            return 1;
        }
        if (flags & F_DOWN)
            stock_used = 1;
        return 0;
    }
    if (key == KEY_TRIG && down && t >= 0 && page_view_key(brain) == KEY_TRIG) {
        stock_held = 1;
        stock_used = 0;
        return 1;
    }
    return 0;
}

int digisnake_enc(void *brain, void *ev)
{
    unsigned long e = EV_ENC(ev);
    long d;
    int t, c;
    struct snake *s;
    digisnake_brain = brain;
    if (!digisnake_open || e < 1 || e > 8)
        return 0;
    t = ui_track();
    if (t < 0)
        return 0;
    enc_acc[e - 1] += EV_DELTA(ev);
    d = enc_acc[e - 1] / COUNTS;
    enc_acc[e - 1] -= d * COUNTS;
    if (d == 0)
        return 1;
    s = &digisnake_data[t];
    c = (int)e - 1;
    redraw = 1;
    run_active[t] = 0;
    switch (digisnake_pg) {
    case PG_NOTES:
        s->note[c] = (signed char)(func_held ? next_stop(s->note[c], d, note_stops, 5)
                                             : clampi(s->note[c] + d, -NOTE_MAX, NOTE_MAX));
        break;
    case PG_GATES:
        if (d > 0)
            s->gate |= (unsigned char)(1u << c);
        else
            s->gate &= (unsigned char)~(1u << c);
        break;
    case PG_VEL:
        s->vel[c] = (unsigned char)(func_held ? next_stop(s->vel[c], d, vel_stops, 5)
                                              : clampi(s->vel[c] + d, 1, 127));
        break;
    default:
        if (func_held && c == P_RANGE)
            s->set[c] = (unsigned char)next_stop(s->set[c], d, range_stops, 3);
        else if (c == P_DIV) {
            s->set[c] = (unsigned char)div_turn(s->set[c], d);
            if (s->set[c] == 0 && !per_track_mode())
                say("2X NEEDS SCALE PER TRACK");
        }
        else
            s->set[c] = (unsigned char)clampi(s->set[c] + d, set_min[c], set_max[c]);
        if (c == P_ON || c == P_DIR || c == P_LEN || c == P_SHAPE)
            reset_req[t] = 1;
        break;
    }
    return 1;
}

/* ---- drawing (ev_tick, ev_draw): 128 x 64, y = 0 the bottom row --------- */
typedef void (*rect_fn)(void *bmp, int x0, int y0, int x1, int y1, int colour);
typedef int (*text_fn)(void *bmp, const void *font, int x, int y, int maxlen, const char *fmt, ...);
#define FILLRECT  ((rect_fn)fw_fillrect)
#define FRAMERECT ((rect_fn)fw_framerect)
#define TEXTF     ((text_fn)fw_textf)
#define FONT5     ((const void *)fw_font5)

static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void digisnake_tick(void *ctrl)
{
    int t;
    frames++;
    if (!data_ready) {
        for (t = 0; t < TRACKS; t++)
            track_defaults(&digisnake_data[t]);
        data_ready = 1;
    }
    if (TRANSPORT == 0)
        for (t = 0; t < TRACKS; t++)
            rewind[t] = 1;                  /* PLAY starts every path again */
    if (msg_ticks && !--msg_ticks)
        redraw = 1;
    speed_sync();
    if (digisnake_open) {
        /* a menu over the pages, or another view (a page key we passed on):
         * the page closes */
        uint32_t k = page_view_key(digisnake_brain);
        if (k != KEY_TRIG || ui_track() < 0) {
            close_page();
        } else {
            static int last_cell = -1, last_play = -1, last_t = -1;
            int ut = ui_track(), cell = digisnake_cell[ut], pl = (int)TRANSPORT;
            if (redraw || cell != last_cell || pl != last_play || ut != last_t) {
                last_cell = cell;
                last_play = pl;
                last_t = ut;
                redraw = 1;
            }
        }
    }
    if (redraw) {
        redraw = 0;
        *((unsigned char *)ctrl + 0x20) = 1;
    }
}

static int path_order(const struct snake *s, int cell)
{
    int len = snake_len(s), k;
    for (k = 0; k < len; k++)
        if (cell_of(s, k, len) == cell)
            return k + 1;
    return 0;
}

void digisnake_draw(void *bmp, void *ctrl)
{
    int t, c, playing, base, root;
    const struct snake *s;
    (void)ctrl;
    if (!digisnake_open)
        return;
    t = ui_track();
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    if (t < 0)
        return;
    s = &digisnake_data[t];
    base = track_base(t, &root);
    playing = s->set[P_ON] && TRANSPORT == 1;

    /* the title line */
    TEXTF(bmp, FONT5, 1, 57, -1, "%s%d SNAKE", t < SQ_AUDIO ? "T" : "MIDI ", (t & 7) + 1);
    TEXTF(bmp, FONT5, 72, 57, -1, "%s %d/4", page_names[digisnake_pg], digisnake_pg + 1);
    FILLRECT(bmp, 0, 54, 127, 54, 1);

    for (c = 0; c < CELLS; c++) {
        int x0 = 32 * (c & 3), y0 = (c < 4) ? 31 : 8;
        int x1 = x0 + 30, y1 = y0 + 21, order;
        FRAMERECT(bmp, x0, y0, x1, y1, 1);
        if (digisnake_pg == PG_PATH) {
            int v = s->set[c];
            TEXTF(bmp, FONT5, x0 + 3, y0 + 14, -1, "%s", set_labels[c]);
            switch (c) {
            case P_ON:    TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", v ? "ON" : "OFF"); break;
            case P_SHAPE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", shape_names[v % SHAPES]); break;
            case P_DIR:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", dir_names[v % DIRS]); break;
            case P_SCALE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", scale_names[v % SCALES]); break;
            case P_RANGE: TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "+-%d", v); break;
            case P_DIV:   TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%s", div_name(v)); break;
            default:      TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%d", v); break;
            }
            continue;
        }
        order = path_order(s, c);
        if (order)
            TEXTF(bmp, FONT5, x1 - 6, y1 - 6, -1, "%d", order);
        if (playing && digisnake_cell[t] == c) {
            FRAMERECT(bmp, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);
            FRAMERECT(bmp, x0 + 2, y0 + 2, x1 - 2, y1 - 2, 1);
        }
        if (digisnake_pg == PG_NOTES) {
            int n = cell_note(s, base, root, c);   /* named as the TRIG page's NOTE: 60 is C5 */
            TEXTF(bmp, FONT5, x0 + 3, y0 + 12, -1, "%s%d", note_names[n % 12], n / 12);
            TEXTF(bmp, FONT5, x0 + 3, y0 + 4, -1, "%c%d", s->note[c] < 0 ? '-' : '+',
                  s->note[c] < 0 ? -s->note[c] : s->note[c]);
        } else if (digisnake_pg == PG_GATES) {
            if ((s->gate >> c) & 1)
                FILLRECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
            else
                FRAMERECT(bmp, x0 + 8, y0 + 5, x1 - 10, y1 - 5, 1);
        } else {
            int h = (s->vel[c] * 14) / 127;
            FILLRECT(bmp, x0 + 4, y0 + 4, x0 + 7, y0 + 4 + h, 1);
            TEXTF(bmp, FONT5, x0 + 11, y0 + 8, -1, "%d", s->vel[c]);
        }
    }
    if (msg_ticks && msg)
        TEXTF(bmp, FONT5, 1, 1, -1, "%s", msg);
    else
        TEXTF(bmp, FONT5, 1, 1, -1, "SNAKE %s  <> PAGE  TRIG BACK", s->set[P_ON] ? "ON" : "OFF");
}
