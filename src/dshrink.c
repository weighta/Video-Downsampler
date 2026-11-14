/* dshrink - shrink a video to fit Discord's upload limit.
 *
 * Win32, plain C, no C runtime. Drop videos anywhere on the window (or use
 * Explorer's right-click "Downsize with dshrink"); each one is re-encoded with
 * ffmpeg (2-pass x264 veryslow + AAC, MP4 faststart) at the highest
 * resolution / frame rate that still looks decent at the bitrate that fits,
 * checked against the target size (retrying lower if it overshoots), decoded
 * once end to end, and written next to the source as <name>_d.mp4. The source
 * is never touched and nothing is ever overwritten.
 *
 * Needs ffmpeg.exe (with libx264): next to dshrink.exe, in a bin\ folder
 * beside it, on PATH, or named by the DSHRINK_FFMPEG environment variable.
 * If none is found, a "Download ffmpeg" button fetches gyan.dev's release
 * essentials build (SHA-256 checked) into bin\ - only when clicked.
 *
 * Command line:
 *   dshrink.exe [video ...]          open the window (or hand the videos to
 *                                    the window already open) and start
 *   dshrink.exe --install-menu       add "Downsize with dshrink" to Explorer
 *   dshrink.exe --uninstall-menu     remove it
 * Testing:
 *   --selftest-drop FILE  (repeatable) post a real WM_DROPFILES to the window
 *   --selftest-ole        ...or feed a shell data object to the OLE drop target
 *   --autoclose MS        close MS milliseconds after the queue empties
 *   --log FILE            append one RESULT line per finished video
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <ole2.h>
#include <shlobj.h>
#include <intrin.h>
#include <wininet.h>
#include <bcrypt.h>

/* ---------------------------------------------------------------- no CRT */

int _fltused = 0x9875;
#pragma function(memset, memcpy, memcmp)
void *memset(void *d, int c, size_t n) { __stosb((unsigned char *)d, (unsigned char)c, n); return d; }
void *memcpy(void *d, const void *s, size_t n) { __movsb((unsigned char *)d, (const unsigned char *)s, n); return d; }
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

static void *xalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static double fabs_(double x) { return x < 0 ? -x : x; }
static double ln_(double x)                       /* x > 0 */
{
    int e = 0, k;
    double y, y2, s = 0, t;
    while (x > 2) { x *= 0.5; e++; }
    while (x < 1) { x *= 2; e--; }
    y = (x - 1) / (x + 1); y2 = y * y; t = y;
    for (k = 1; k < 41; k += 2) { s += t / k; t *= y2; }
    return 2 * s + e * 0.69314718055994531;
}
static double exp_(double x)
{
    int n = (int)(x / 0.69314718055994531), k;
    double r = x - n * 0.69314718055994531, s = 1, t = 1;
    for (k = 1; k < 30; k++) { t *= r / k; s += t; }
    for (; n > 0; n--) s *= 2;
    for (; n < 0; n++) s *= 0.5;
    return s;
}
static double pow_(double b, double e) { return b <= 0 ? 0 : exp_(e * ln_(b)); }

/* wide-string builder */
typedef struct { WCHAR *p; int n, cap; } WB;
static void wb_init(WB *b, WCHAR *buf, int cap) { b->p = buf; b->n = 0; b->cap = cap; buf[0] = 0; }
static void wb_c(WB *b, WCHAR c) { if (b->n < b->cap - 1) { b->p[b->n++] = c; b->p[b->n] = 0; } }
static void wb_s(WB *b, const WCHAR *s) { while (s && *s) wb_c(b, *s++); }
static void wb_a(WB *b, const char *s) { while (s && *s) wb_c(b, (WCHAR)(unsigned char)*s++); }
static void wb_u(WB *b, long long v)
{
    WCHAR t[24]; int i = 0;
    if (v < 0) { wb_c(b, '-'); v = -v; }
    do { t[i++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (i) wb_c(b, t[--i]);
}
static void wb_f(WB *b, double v, int dec)          /* fixed decimals */
{
    long long m = 1, x, ip, fp; int i; WCHAR t[20];
    if (v < 0) { wb_c(b, '-'); v = -v; }
    for (i = 0; i < dec; i++) m *= 10;
    x = (long long)(v * m + 0.5); ip = x / m; fp = x % m;
    wb_u(b, ip);
    if (dec) {
        wb_c(b, '.');
        for (i = dec - 1; i >= 0; i--) { t[i] = (WCHAR)('0' + fp % 10); fp /= 10; }
        for (i = 0; i < dec; i++) wb_c(b, t[i]);
    }
}
static void wb_g(WB *b, double v)                   /* 60, 29.97, 23.98 */
{
    double r = (double)(long long)(v + 0.5);
    if (fabs_(v - r) < 0.01) { wb_u(b, (long long)r); return; }
    wb_f(b, v, 2);
    while (b->n && b->p[b->n - 1] == '0') b->p[--b->n] = 0;
}
static void wb_rate(WB *b, double kbps)
{
    if (kbps >= 1000) { wb_f(b, kbps / 1000, 2); wb_s(b, L" Mbps"); }
    else { wb_f(b, kbps, 0); wb_s(b, L" kbps"); }
}
static void wb_mb(WB *b, double bytes) { wb_f(b, bytes / 1e6, 2); wb_s(b, L" MB"); }
static void wcpy(WCHAR *d, const WCHAR *s, int cap) { lstrcpynW(d, s, cap); }

/* ascii helpers for ffmpeg output */
static int isdig(char c) { return c >= '0' && c <= '9'; }
static const char *sfind(const char *h, const char *n)
{
    int i;
    for (; *h; h++) {
        for (i = 0; n[i] && h[i] == n[i]; i++) ;
        if (!n[i]) return h;
    }
    return 0;
}
static double satof(const char *s)
{
    double v = 0, f = 0.1; int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (isdig(*s)) v = v * 10 + (*s++ - '0');
    if (*s == '.') for (s++; isdig(*s); s++) { v += (*s - '0') * f; f *= 0.1; }
    return neg ? -v : v;
}
static double num_before(const char *line, const char *at)   /* "59.94 fps" */
{
    const char *p = at;
    while (p > line && (isdig(p[-1]) || p[-1] == '.')) p--;
    return p == at ? 0 : satof(p);
}

/* ------------------------------------------------------------ the planner */

#define TARGET_MARGIN 0.05
#define CONTAINER_OVERHEAD 0.015
#define MAX_BPP 0.20
#define MIN_VIDEO_KBPS 60
static const int LAD_H[8] = { 2160, 1440, 1080, 720, 540, 480, 360, 240 };
static const int LAD_K[8] = { 9000, 4500, 2500, 1300, 800, 600, 250, 120 };

typedef struct { double dur; int w, h; double fps; int has_audio; double akbps; int ach; } Probe;
typedef struct {
    int w, h; double fps; int change_fps; double vkbps; int akbps, ach; double bpp, total;
    WCHAR reason[1600], prev[64], chosen[96]; int padded, floor_;
} Plan;

static int even(double x) { int v = (int)(x / 2 + 0.5) * 2; return v < 2 ? 2 : v; }

static void pick_audio(double total, const Probe *s, int *kb, int *ch)
{
    *kb = 0; *ch = 0;
    if (!s->has_audio) return;
    *ch = s->ach > 2 ? 2 : (s->ach < 1 ? 2 : s->ach);
    if (total >= 2500) *kb = 160;
    else if (total >= 1200) *kb = 128;
    else if (total >= 700) *kb = 96;
    else if (total >= 350) *kb = 64;
    else if (total >= 200) *kb = 48;
    else { *kb = 32; *ch = 1; }
    if (*ch == 1 && *kb > 96) *kb = 96;
    if (s->akbps > 0) { int src = (int)s->akbps; if (src < 32) src = 32; if (*kb > src) *kb = src; }
}

static double needed_kbps(int h, double f)
{
    int i, rung = 7;
    for (i = 0; i < 8; i++) if (LAD_H[i] <= h) { rung = i; break; }
    return LAD_K[rung] * pow_(f / 30, 0.6);
}

static void cand_label(WB *b, int h, double f) { wb_u(b, h); wb_c(b, 'p'); wb_g(b, f); }

static void make_plan(const Probe *s, double target, double max_fps, Plan *p)
{
    double total = target * 8 / (1 + CONTAINER_OVERHEAD) / s->dur / 1000, v;
    double srcf = s->fps > 0 ? s->fps : 30, cap_f, fo[4], f;
    int nfo = 1, i, j, heights[9], nh = 0, ls, ss, cap, ch[40], nc = 0, chosen = -1;
    double cf[40];
    WB r, t;

    pick_audio(total, s, &p->akbps, &p->ach);
    v = total - p->akbps;
    wb_init(&r, p->reason, 1600);
    wb_f(&r, target / 1e6, 1); wb_s(&r, L" MB over "); wb_f(&r, s->dur, 1);
    wb_s(&r, L" s = "); wb_rate(&r, total); wb_s(&r, L" total; audio ");
    wb_u(&r, p->akbps); wb_s(&r, L" kbps -> "); wb_rate(&r, v); wb_s(&r, L" for video");

    cap_f = max_fps > 0 && max_fps < srcf ? max_fps : srcf;
    fo[0] = cap_f;
    if (30 < fo[nfo - 1] - 0.5) fo[nfo++] = 30;
    if (24 < fo[nfo - 1] - 0.5) fo[nfo++] = 24;
    if (15 < fo[nfo - 1] - 0.5) fo[nfo++] = 15;

    ls = s->w > s->h ? s->w : s->h;
    ss = s->w > s->h ? s->h : s->w;
    cap = ss;
    heights[nh++] = cap;
    for (i = 0; i < 8; i++) if (LAD_H[i] < cap) heights[nh++] = LAD_H[i];

    /* resolution first, then frame rate; 24/15 fps only at the bottom rung */
    for (i = 0; i < nh; i++)
        for (j = 0; j < nfo; j++) {
            if (fo[j] < 30 && fo[j] < cap_f && i != nh - 1) continue;
            ch[nc] = heights[i]; cf[nc] = fo[j]; nc++;
        }
    p->prev[0] = 0; p->chosen[0] = 0; p->floor_ = 0;
    for (i = 0; i < nc; i++) {
        double need = needed_kbps(ch[i], cf[i]);
        int ok = v >= need;
        wb_c(&r, '\n'); cand_label(&r, ch[i], cf[i]); wb_s(&r, L" needs ");
        wb_rate(&r, need); wb_s(&r, ok ? L": fits - chosen" : L": too little");
        wb_init(&t, ok ? p->chosen : p->prev, ok ? 96 : 64);
        cand_label(&t, ch[i], cf[i]); wb_s(&t, L" needs "); wb_rate(&t, need);
        if (ok) { wb_s(&t, L" - fits"); chosen = i; break; }
    }
    if (chosen < 0) {
        chosen = nc - 1; p->floor_ = 1;
        wb_s(&r, L"\nnothing fits comfortably - using the floor, ");
        cand_label(&r, ch[chosen], cf[chosen]);
        wb_init(&t, p->chosen, 96); wb_s(&t, L"nothing fits well - floor ");
        cand_label(&t, ch[chosen], cf[chosen]);
    }
    {
        int sh = ch[chosen]; double lg = (double)ls * sh / ss;
        if (s->w >= s->h) { p->w = even(lg); p->h = even(sh); }
        else { p->w = even(sh); p->h = even(lg); }
    }
    f = cf[chosen];
    p->fps = f;
    p->change_fps = fabs_(f - srcf) > 0.5;
    p->bpp = v * 1000 / ((double)p->w * p->h * f);
    p->padded = 0;
    if (p->bpp > MAX_BPP) {                 /* plenty of room: don't bloat the file */
        v = MAX_BPP * p->w * p->h * f / 1000;
        p->bpp = MAX_BPP; p->padded = 1;
        wb_s(&r, L"\nplenty of room: capped video at "); wb_rate(&r, v);
        wb_s(&r, L" (max 0.2 bits/pixel) instead of filling the limit");
    }
    p->vkbps = v;
    p->total = total;
}

/* ------------------------------------------------------- shared UI state */

#define WM_APP_REFRESH (WM_APP + 1)
#define WM_APP_DONE    (WM_APP + 2)
#define WM_APP_START   (WM_APP + 3)
#define WM_APP_SELFTEST (WM_APP + 4)

enum { K_DONE, K_NOTHING, K_CANCELLED, K_FAILED };
typedef struct {
    int kind; WCHAR out[1024]; WCHAR msg[512]; long long size; int w, h; double fps;
} Result;

static CRITICAL_SECTION g_cs;
static struct {
    LONG prog;                 /* 0..1000 */
    WCHAR stage[96];
    WCHAR detect[600];
    WCHAR reason[1600];
    WCHAR note[200];
    DWORD t0;                  /* tick when pass 1 of the current attempt began */
} g_sh;
static volatile LONG g_posted, g_cancel;
static HANDLE g_proc;          /* running ffmpeg, guarded by g_cs */
static HWND g_wnd;
static WCHAR g_ffmpeg[MAX_PATH];

/* job handed to the worker */
static WCHAR g_src[1024];
static double g_limit_mb, g_max_fps;
static Result g_res;

static void post_refresh(void)
{
    if (InterlockedExchange(&g_posted, 1) == 0) PostMessageW(g_wnd, WM_APP_REFRESH, 0, 0);
}
static void set_stage(const WCHAR *s, int restart)
{
    EnterCriticalSection(&g_cs);
    wcpy(g_sh.stage, s, 96);
    if (restart) { g_sh.t0 = GetTickCount(); g_sh.prog = 0; }
    LeaveCriticalSection(&g_cs);
    post_refresh();
}
static void set_prog(int v)
{
    if (v != g_sh.prog) { InterlockedExchange(&g_sh.prog, v); post_refresh(); }
}

/* ------------------------------------------------------- running ffmpeg */

typedef struct {
    char *cap; int capn, capmax;     /* full capture (probe), optional */
    char tail[1600]; int tailn;      /* last non-progress output (errors) */
    double dur; int lo, hi, progress;
} Run;

static void run_line(Run *r, char *line)
{
    int n;
    if (r->progress) {
        /* -progress lines are "key=value" (value may be padded: "speed= 240x") */
        const char *k = line;
        while ((*k >= 'a' && *k <= 'z') || isdig(*k) || *k == '_') k++;
        if (k == line || *k != '=') goto text;
        if (sfind(line, "out_time_us=") == line || sfind(line, "out_time_ms=") == line) {
            double sec = satof(line + 12) / 1e6, fr = r->dur > 0 ? sec / r->dur : 0;
            if (fr < 0) fr = 0;
            if (fr > 1) fr = 1;
            set_prog(r->lo + (int)((r->hi - r->lo) * fr));
        }
        return;
    }
text:
    n = lstrlenA(line);
    if (!n) return;
    if (r->tailn + n + 2 >= (int)sizeof r->tail) {     /* keep the newest text */
        int drop = r->tailn + n + 2 - (int)sizeof r->tail + 200;
        if (drop > r->tailn) drop = r->tailn;
        memcpy(r->tail, r->tail + drop, r->tailn - drop);
        r->tailn -= drop;
    }
    if (n > (int)sizeof r->tail - 2) n = (int)sizeof r->tail - 2;
    memcpy(r->tail + r->tailn, line, n); r->tailn += n;
    r->tail[r->tailn++] = '\n'; r->tail[r->tailn] = 0;
}

/* runs app (hidden) with output to r; returns exit code, -1 = could not start, -2 = cancelled */
static int run_proc(const WCHAR *app, WCHAR *cmd, Run *r)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    static char buf[8192], line[4096];   /* worker thread only */
    int ln = 0;
    DWORD got, code = 1;

    if (g_cancel) return -2;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, 0);
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    if (!CreateProcessW(app, cmd, 0, 0, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        0, 0, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); CloseHandle(nul);
        return -1;
    }
    CloseHandle(wr); CloseHandle(nul); CloseHandle(pi.hThread);
    EnterCriticalSection(&g_cs); g_proc = pi.hProcess; LeaveCriticalSection(&g_cs);
    if (g_cancel) TerminateProcess(pi.hProcess, 1);

    r->tailn = 0; r->tail[0] = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, 0) && got) {
        DWORD i;
        if (r->cap && r->capn < r->capmax - 1) {
            DWORD k = got; if ((int)k > r->capmax - 1 - r->capn) k = r->capmax - 1 - r->capn;
            memcpy(r->cap + r->capn, buf, k); r->capn += k; r->cap[r->capn] = 0;
        }
        for (i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') { line[ln] = 0; if (ln) run_line(r, line); ln = 0; }
            else if (ln < (int)sizeof line - 1) line[ln++] = c;
        }
    }
    if (ln) { line[ln] = 0; run_line(r, line); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    EnterCriticalSection(&g_cs); g_proc = 0; LeaveCriticalSection(&g_cs);
    CloseHandle(pi.hProcess); CloseHandle(rd);
    if (g_cancel) return -2;
    return (int)code;
}

#define run_ffmpeg(cmd, r) run_proc(g_ffmpeg, cmd, r)

static void q_arg(WB *b, const WCHAR *s) { wb_c(b, '"'); wb_s(b, s); wb_c(b, '"'); }

/* read clip info from `ffmpeg -i` (no ffprobe needed) */
static int probe(const WCHAR *path, Probe *p, WCHAR *err, int errcap)
{
    static WCHAR cmd[2048];
    static char cap[65536];
    Run r; WB b; char *ln, *nx; const char *q;
    int rot = 0;

    memset(p, 0, sizeof *p);
    memset(&r, 0, sizeof r);
    r.cap = cap; r.capmax = sizeof cap; cap[0] = 0;
    wb_init(&b, cmd, 2048);
    q_arg(&b, g_ffmpeg); wb_s(&b, L" -hide_banner -nostdin -i "); q_arg(&b, path);
    if (run_ffmpeg(cmd, &r) == -1) { wcpy(err, L"could not start ffmpeg", errcap); return 0; }
    if (g_cancel) return 0;

    if ((q = sfind(cap, "rotation of ")) != 0) rot = (int)satof(q + 12);
    else if ((q = sfind(cap, "rotate ")) != 0) {
        q += 7; while (*q == ' ' || *q == ':') q++;
        rot = (int)satof(q);
    }
    if (rot < 0) rot = -rot;
    rot %= 360;

    for (ln = cap; *ln; ln = nx) {
        for (nx = ln; *nx && *nx != '\n' && *nx != '\r'; nx++) ;
        if (*nx) { *nx = 0; nx++; while (*nx == '\n' || *nx == '\r') nx++; }
        if ((q = sfind(ln, "Duration: ")) != 0 && !p->dur && isdig(q[10])) {
            q += 10;
            p->dur = satof(q) * 3600 + satof(q + 3) * 60 + satof(q + 6);
        } else if (sfind(ln, "Stream #") && sfind(ln, "Video:") && !sfind(ln, "attached pic") && !p->w) {
            const char *s;
            for (s = ln; *s; s++) {          /* ", 1920x1080" */
                const char *a; int w = 0, h = 0, nw = 0, nhh = 0;
                if (*s != ',') continue;
                a = s + 1; while (*a == ' ') a++;
                while (isdig(*a) && nw < 6) { w = w * 10 + (*a++ - '0'); nw++; }
                if (*a != 'x' || nw < 2) continue;
                a++;
                while (isdig(*a) && nhh < 6) { h = h * 10 + (*a++ - '0'); nhh++; }
                if (nhh < 2 || isdig(*a)) continue;
                p->w = w; p->h = h; break;
            }
            if ((q = sfind(ln, " fps")) != 0) p->fps = num_before(ln, q);
            if (!p->fps && (q = sfind(ln, " tbr")) != 0) p->fps = num_before(ln, q);
        } else if (sfind(ln, "Stream #") && sfind(ln, "Audio:") && !p->has_audio) {
            p->has_audio = 1;
            p->ach = sfind(ln, " mono") ? 1 : 2;
            if ((q = sfind(sfind(ln, "Audio:"), " kb/s")) != 0) p->akbps = num_before(ln, q);
        }
    }
    if (rot == 90 || rot == 270) { int t = p->w; p->w = p->h; p->h = t; }
    if (!p->fps) p->fps = 30;
    if (!p->w || !p->h) { wcpy(err, L"no video stream found", errcap); return 0; }
    if (p->dur <= 0) { wcpy(err, L"could not read the clip's duration", errcap); return 0; }
    return 1;
}

static long long file_size(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

static void path_dir(const WCHAR *path, WCHAR *dir, int cap)       /* "W:\x\" */
{
    int i, cut = 0;
    wcpy(dir, path, cap);
    for (i = 0; dir[i]; i++) if (dir[i] == '\\' || dir[i] == '/') cut = i + 1;
    dir[cut] = 0;
}
static const WCHAR *path_name(const WCHAR *path)
{
    const WCHAR *n = path, *s;
    for (s = path; *s; s++) if (*s == '\\' || *s == '/') n = s + 1;
    return n;
}

static void output_name(const WCHAR *src, WCHAR *out, int cap, int n)
{
    WCHAR stem[512]; int i, dot = -1; WB b;
    const WCHAR *name = path_name(src);
    wcpy(stem, name, 512);
    for (i = 0; stem[i]; i++) if (stem[i] == '.') dot = i;
    if (dot > 0) stem[dot] = 0;
    path_dir(src, out, cap);
    b.p = out; b.n = lstrlenW(out); b.cap = cap;
    wb_s(&b, stem); wb_s(&b, L"_d");
    if (n > 1) { wb_s(&b, L" ("); wb_u(&b, n); wb_c(&b, ')'); }
    wb_s(&b, L".mp4");
}

static void rm_tree(const WCHAR *dir)           /* a temp folder of ours, recursively */
{
    WCHAR pat[1100], f[1100]; WIN32_FIND_DATAW fd; HANDLE h; WB b;
    wb_init(&b, pat, 1100); wb_s(&b, dir); wb_s(&b, L"\\*");
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            wb_init(&b, f, 1100); wb_s(&b, dir); wb_c(&b, '\\'); wb_s(&b, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) rm_tree(f);
                else RemoveDirectoryW(f);           /* never follow links out */
                continue;
            }
            SetFileAttributesW(f, FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void tail_to(WCHAR *dst, int cap, const char *tail)
{
    WB b; int n = lstrlenA(tail);
    const char *s = n > 300 ? tail + n - 300 : tail;
    wb_init(&b, dst, cap);
    for (; *s; s++) wb_c(&b, *s == '\n' ? ' ' : (WCHAR)(unsigned char)*s);
}

/* ------------------------------------------------------------ the worker */

static void detect_text(const Probe *s, const Plan *p, double target_mb)
{
    WB b;
    EnterCriticalSection(&g_cs);
    wb_init(&b, g_sh.detect, 600);
    wb_s(&b, L"Auto: "); wb_u(&b, s->w); wb_c(&b, 'x'); wb_u(&b, s->h); wb_c(&b, ' ');
    wb_g(&b, s->fps); wb_s(&b, L" fps "); wb_f(&b, s->dur, 0); wb_s(&b, L" s");
    if (!p) {
        wb_s(&b, L" - already under the limit");
        g_sh.reason[0] = 0;
    } else {
        wb_s(&b, L"  \x2192  "); wb_u(&b, p->w); wb_c(&b, 'x'); wb_u(&b, p->h); wb_c(&b, ' ');
        wb_g(&b, p->fps); wb_s(&b, L" fps, "); wb_f(&b, p->vkbps / 1000, 2);
        wb_s(&b, L" Mbps  (target "); wb_f(&b, target_mb, 1); wb_s(&b, L" MB)\nWhy: ");
        if (p->prev[0] && !p->floor_) { wb_s(&b, p->prev); wb_s(&b, L" (too much), "); }
        wb_s(&b, p->chosen);
        if (p->padded) wb_s(&b, L", not padded");
        if (p->vkbps < MIN_VIDEO_KBPS) wb_s(&b, L" - too long to look good, consider trimming");
        wb_s(&b, L"  \x24d8");
        wcpy(g_sh.reason, p->reason, 1600);
    }
    LeaveCriticalSection(&g_cs);
    post_refresh();
}

static void encode_cmd(WB *b, const WCHAR *src, const Probe *s, const Plan *p,
                       const WCHAR *passlog, int pass, const WCHAR *out)
{
    int vk = (int)p->vkbps;
    q_arg(b, g_ffmpeg);
    wb_s(b, L" -hide_banner -nostdin -loglevel error -nostats -progress pipe:1 -y -i ");
    q_arg(b, src);
    wb_s(b, L" -map 0:v:0 -vf \"");
    if (p->change_fps) { wb_s(b, L"fps="); wb_g(b, p->fps); wb_c(b, ','); }
    if (p->w != s->w || p->h != s->h) {
        wb_s(b, L"scale="); wb_u(b, p->w); wb_c(b, ':'); wb_u(b, p->h); wb_s(b, L":flags=lanczos,");
    } else if ((s->w & 1) || (s->h & 1)) {
        wb_s(b, L"scale=trunc(iw/2)*2:trunc(ih/2)*2,");
    }
    wb_s(b, L"format=yuv420p\" -c:v libx264 -preset veryslow -profile:v high -b:v ");
    wb_u(b, vk); wb_s(b, L"k -maxrate "); wb_u(b, vk * 2); wb_s(b, L"k -bufsize ");
    wb_u(b, vk * 4); wb_s(b, L"k -passlogfile "); q_arg(b, passlog);
    if (pass == 1) {
        wb_s(b, L" -pass 1 -an -sn -dn -f null NUL");
    } else {
        wb_s(b, L" -pass 2 ");
        if (p->akbps) {
            wb_s(b, L"-map 0:a:0 -c:a aac -b:a "); wb_u(b, p->akbps);
            wb_s(b, L"k -ac "); wb_u(b, p->ach);
        } else {
            wb_s(b, L"-an");
        }
        wb_s(b, L" -sn -dn -map_metadata 0 -write_tmcd 0 -movflags +faststart ");
        q_arg(b, out);
    }
}

static DWORD WINAPI worker(LPVOID unused)
{
    static WCHAR cmd[8192], tmpdir[1100], passlog[1100], tmpout[1100], dir[1024];
    static Plan plan;
    Probe s, chk;
    Result *res = &g_res;
    WCHAR err[200];
    Run r;
    WB b;
    double limit_b = g_limit_mb * 1e6, target = limit_b * (1 - TARGET_MARGIN);
    long long size, out = 0;
    int attempt, rc, n;
    (void)unused;

    memset(res, 0, sizeof *res);
    res->kind = K_FAILED;
    set_stage(L"reading clip info", 1);
    if (!probe(g_src, &s, err, 200)) {
        if (g_cancel) { res->kind = K_CANCELLED; goto done; }
        wcpy(res->msg, err, 512); goto done;
    }
    size = file_size(g_src);
    if (size >= 0 && size <= (long long)target) {
        detect_text(&s, 0, target / 1e6);
        res->kind = K_NOTHING; goto done;
    }
    make_plan(&s, target, g_max_fps, &plan);
    detect_text(&s, &plan, target / 1e6);

    path_dir(g_src, dir, 1024);
    wb_init(&b, tmpdir, 1100); wb_s(&b, dir); wb_s(&b, L".dshrink-");
    wb_u(&b, GetCurrentProcessId()); wb_c(&b, '-'); wb_u(&b, GetTickCount() & 0xFFFFF);
    if (!CreateDirectoryW(tmpdir, 0)) {
        wcpy(res->msg, L"cannot write to the video's folder", 512); goto done;
    }
    SetFileAttributesW(tmpdir, FILE_ATTRIBUTE_HIDDEN);
    wb_init(&b, passlog, 1100); wb_s(&b, tmpdir); wb_s(&b, L"\\x264");
    wb_init(&b, tmpout, 1100); wb_s(&b, tmpdir); wb_s(&b, L"\\out.mp4");

    memset(&r, 0, sizeof r);
    r.progress = 1; r.dur = s.dur;
    for (attempt = 1; attempt <= 4; attempt++) {
        WCHAR st[64];
        wb_init(&b, st, 64); wb_s(&b, L"pass 1/2");
        if (attempt > 1) { wb_s(&b, L" (retry "); wb_u(&b, attempt - 1); wb_c(&b, ')'); }
        set_stage(st, 1);
        wb_init(&b, cmd, 8192); encode_cmd(&b, g_src, &s, &plan, passlog, 1, 0);
        r.lo = 0; r.hi = 350;
        rc = run_ffmpeg(cmd, &r);
        if (rc == -2) { res->kind = K_CANCELLED; goto clean; }
        if (rc) { wcpy(res->msg, L"ffmpeg failed (pass 1): ", 512); tail_to(res->msg + lstrlenW(res->msg), 400, r.tail); goto clean; }

        st[5] = '2';
        set_stage(st, 0);
        wb_init(&b, cmd, 8192); encode_cmd(&b, g_src, &s, &plan, passlog, 2, tmpout);
        r.lo = 350; r.hi = 950;
        rc = run_ffmpeg(cmd, &r);
        if (rc == -2) { res->kind = K_CANCELLED; goto clean; }
        if (rc) { wcpy(res->msg, L"ffmpeg failed (pass 2): ", 512); tail_to(res->msg + lstrlenW(res->msg), 400, r.tail); goto clean; }

        out = file_size(tmpout);
        if (out <= (long long)target) break;
        if (attempt == 4) break;
        {   /* overshoot is all video (AAC CBR is predictable): scale it down + 3 % */
            double audio_b = plan.akbps * 1000.0 / 8 * s.dur;
            double ratio = (target - audio_b) / ((double)out - audio_b > 1 ? (double)out - audio_b : 1);
            if (ratio < 0.1) ratio = 0.1;
            plan.vkbps *= ratio * 0.97;
            if (plan.vkbps < 20) plan.vkbps = 20;
            plan.bpp = plan.vkbps * 1000 / ((double)plan.w * plan.h * plan.fps);
            EnterCriticalSection(&g_cs);
            wcpy(g_sh.note, L"Slightly over the target - re-encoding a bit smaller", 200);
            LeaveCriticalSection(&g_cs);
        }
    }

    /* decode it end to end: any error output means it doesn't play cleanly */
    set_stage(L"checking it plays", 0);
    wb_init(&b, cmd, 8192);
    q_arg(&b, g_ffmpeg); wb_s(&b, L" -hide_banner -nostdin -v error -nostats -progress pipe:1 -i ");
    q_arg(&b, tmpout); wb_s(&b, L" -f null NUL");
    r.lo = 950; r.hi = 1000;
    rc = run_ffmpeg(cmd, &r);
    if (rc == -2) { res->kind = K_CANCELLED; goto clean; }

    for (n = 1; n < 1000; n++) {            /* next free name; never overwrite */
        output_name(g_src, res->out, 1024, n);
        if (lstrcmpiW(res->out, g_src) == 0) continue;
        if (MoveFileExW(tmpout, res->out, 0)) break;
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) {
            wcpy(res->msg, L"could not save the output file", 512); res->out[0] = 0; goto clean;
        }
    }
    res->size = file_size(res->out);
    if (probe(res->out, &chk, err, 200)) { res->w = chk.w; res->h = chk.h; res->fps = chk.fps; }
    if (rc || r.tailn) {
        wcpy(res->msg, L"decode check reported errors: ", 512);
        tail_to(res->msg + lstrlenW(res->msg), 400, r.tail);
    } else if (out > (long long)target) {
        wb_init(&b, res->msg, 512); wb_s(&b, L"still "); wb_mb(&b, (double)out);
        wb_s(&b, L" after 3 retries");
    } else {
        res->kind = K_DONE;
    }
clean:
    rm_tree(tmpdir);
done:
    PostMessageW(g_wnd, WM_APP_DONE, 0, 0);
    return 0;
}

/* ------------------------------------------- one-click ffmpeg download */

/* gyan.dev's "release essentials" build: official Windows builds linked from
   ffmpeg.org, includes libx264, publishes a SHA-256 next to each package. */
#define FF_URL L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
#define WM_APP_FFDONE (WM_APP + 5)
static int g_ff_ok, g_ff_cancelled;
static WCHAR g_ff_msg[400], g_ff_path[MAX_PATH];

/* 1 = ok, 0 = failed (g_ff_msg says why), -2 = cancelled */
static int http_get(HINTERNET net, const WCHAR *url, HANDLE file, char *mem, int memcap,
                    BCRYPT_HASH_HANDLE hh, WCHAR *final_url, int lo, int hi)
{
    static BYTE buf[65536];
    HINTERNET u; DWORD status = 0, sl = sizeof status, got, wr, lb; WCHAR lenb[32], t[96];
    long long total = 0, done = 0, shown = -1; int memn = 0, ok = 1, i; WB b;
    u = InternetOpenUrlW(net, url, 0, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                         INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0);
    if (!u) { wcpy(g_ff_msg, L"could not reach gyan.dev - check your internet connection", 400); return 0; }
    HttpQueryInfoW(u, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sl, 0);
    if (status != 200) {
        wb_init(&b, g_ff_msg, 400); wb_s(&b, L"gyan.dev answered HTTP "); wb_u(&b, status);
        InternetCloseHandle(u); return 0;
    }
    lb = sizeof lenb;
    if (HttpQueryInfoW(u, HTTP_QUERY_CONTENT_LENGTH, lenb, &lb, 0))
        for (i = 0; lenb[i] >= '0' && lenb[i] <= '9'; i++) total = total * 10 + (lenb[i] - '0');
    if (final_url) {                         /* the versioned package after redirects */
        DWORD fl = 2048 * sizeof(WCHAR);
        if (!InternetQueryOptionW(u, INTERNET_OPTION_URL, final_url, &fl)) wcpy(final_url, url, 2048);
    }
    for (;;) {
        if (g_cancel) { InternetCloseHandle(u); return -2; }
        if (!InternetReadFile(u, buf, sizeof buf, &got)) { ok = 0; break; }
        if (!got) break;
        if (file && (!WriteFile(file, buf, got, &wr, 0) || wr != got)) {
            wcpy(g_ff_msg, L"could not write the download (disk full?)", 400);
            InternetCloseHandle(u); return 0;
        }
        if (mem) { DWORD k = got; if ((int)k > memcap - 1 - memn) k = memcap - 1 - memn; memcpy(mem + memn, buf, k); memn += k; }
        if (hh) BCryptHashData(hh, buf, got, 0);
        done += got;
        if (total > 0) set_prog(lo + (int)((hi - lo) * ((double)done / total)));
        if (file && done / 1000000 != shown) {
            shown = done / 1000000;
            wb_init(&b, t, 96); wb_s(&b, L"downloading ffmpeg  -  "); wb_u(&b, shown);
            if (total > 0) { wb_s(&b, L" of "); wb_u(&b, (total + 500000) / 1000000); }
            wb_s(&b, L" MB");
            set_stage(t, 0);
        }
    }
    InternetCloseHandle(u);
    if (!ok || (total > 0 && done != total)) { wcpy(g_ff_msg, L"the download was interrupted - try again", 400); return 0; }
    if (mem) mem[memn] = 0;
    return 1;
}

static DWORD WINAPI ff_worker(LPVOID unused)
{
    static WCHAR exe[MAX_PATH], bindir[MAX_PATH], zip[MAX_PATH], tmp[MAX_PATH], tar[MAX_PATH],
                 final_url[2048], shaurl[2100], cmd[4096], sub[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    static char sha[256], cap[65536];
    HINTERNET net = 0; BCRYPT_ALG_HANDLE alg = 0; BCRYPT_HASH_HANDLE hh = 0; BYTE dig[32];
    HANDLE f, fh; WIN32_FIND_DATAW fd; Run r; WB b; int rc, i, made_tmp = 0;
    (void)unused;

    g_ff_ok = 0; g_ff_cancelled = 0; g_ff_msg[0] = 0;
    set_stage(L"connecting to gyan.dev", 1);

    /* <dshrink folder>\bin, or %LOCALAPPDATA%\dshrink\bin if that isn't writable */
    GetModuleFileNameW(0, exe, MAX_PATH);
    path_dir(exe, bindir, MAX_PATH); lstrcatW(bindir, L"bin");
    CreateDirectoryW(bindir, 0);
    wb_init(&b, zip, MAX_PATH); wb_s(&b, bindir); wb_s(&b, L"\\ffmpeg-download.zip.part");
    f = CreateFileW(zip, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, 0);
    if (f == INVALID_HANDLE_VALUE) {
        GetEnvironmentVariableW(L"LOCALAPPDATA", bindir, MAX_PATH - 20);
        lstrcatW(bindir, L"\\dshrink"); CreateDirectoryW(bindir, 0);
        lstrcatW(bindir, L"\\bin"); CreateDirectoryW(bindir, 0);
        wb_init(&b, zip, MAX_PATH); wb_s(&b, bindir); wb_s(&b, L"\\ffmpeg-download.zip.part");
        f = CreateFileW(zip, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, 0);
        if (f == INVALID_HANDLE_VALUE) { wcpy(g_ff_msg, L"no folder to save ffmpeg in", 400); goto done; }
    }

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0) ||
        BCryptCreateHash(alg, &hh, 0, 0, 0, 0, 0)) {
        CloseHandle(f); wcpy(g_ff_msg, L"SHA-256 is unavailable", 400); goto clean;
    }
    net = InternetOpenW(L"dshrink/1.0", INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0);
    rc = net ? http_get(net, FF_URL, f, 0, 0, hh, final_url, 0, 850) : 0;
    CloseHandle(f);
    if (!net) wcpy(g_ff_msg, L"no internet access", 400);
    if (rc == -2) { g_ff_cancelled = 1; goto clean; }
    if (rc != 1) goto clean;

    /* compare against the checksum gyan.dev publishes for that exact package */
    set_stage(L"checking the download", 0);
    BCryptFinishHash(hh, dig, 32, 0);
    wb_init(&b, shaurl, 2100); wb_s(&b, final_url); wb_s(&b, L".sha256");
    rc = http_get(net, shaurl, 0, sha, sizeof sha, 0, 0, 850, 860);
    if (rc == -2) { g_ff_cancelled = 1; goto clean; }
    if (rc != 1) goto clean;
    for (i = 0; i < 32; i++) {
        static const char hx[] = "0123456789abcdef";
        char c1 = sha[2 * i], c2 = sha[2 * i + 1];
        if (c1 >= 'A' && c1 <= 'F') c1 += 32;
        if (c2 >= 'A' && c2 <= 'F') c2 += 32;
        if (c1 != hx[dig[i] >> 4] || c2 != hx[dig[i] & 15]) {
            wcpy(g_ff_msg, L"the download didn't match its SHA-256 checksum - try again", 400);
            goto clean;
        }
    }

    /* unpack just ffmpeg.exe and its license with Windows' own tar.exe */
    set_stage(L"unpacking", 0); set_prog(870);
    wb_init(&b, tmp, MAX_PATH); wb_s(&b, bindir); wb_s(&b, L"\\.ffmpeg-unpack");
    rm_tree(tmp);
    CreateDirectoryW(tmp, 0); made_tmp = 1;
    GetSystemDirectoryW(tar, MAX_PATH - 10); lstrcatW(tar, L"\\tar.exe");
    wb_init(&b, cmd, 4096); q_arg(&b, tar); wb_s(&b, L" -xf "); q_arg(&b, zip);
    wb_s(&b, L" -C "); q_arg(&b, tmp); wb_s(&b, L" \"*/bin/ffmpeg.exe\" \"*/LICENSE\"");
    memset(&r, 0, sizeof r);
    rc = run_proc(tar, cmd, &r);
    if (rc == -2) { g_ff_cancelled = 1; goto clean; }
    if (rc == -1) { wcpy(g_ff_msg, L"Windows' tar.exe is missing - install ffmpeg yourself (winget install Gyan.FFmpeg)", 400); goto clean; }
    if (rc) { wcpy(g_ff_msg, L"could not unpack: ", 400); tail_to(g_ff_msg + lstrlenW(g_ff_msg), 300, r.tail); goto clean; }
    sub[0] = 0;
    wb_init(&b, src, MAX_PATH); wb_s(&b, tmp); wb_s(&b, L"\\*");
    fh = FindFirstFileW(src, &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            wb_init(&b, sub, MAX_PATH); wb_s(&b, tmp); wb_c(&b, '\\'); wb_s(&b, fd.cFileName); break;
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    wb_init(&b, src, MAX_PATH); wb_s(&b, sub); wb_s(&b, L"\\bin\\ffmpeg.exe");
    wb_init(&b, dst, MAX_PATH); wb_s(&b, bindir); wb_s(&b, L"\\ffmpeg.exe");
    if (!sub[0] || !MoveFileExW(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        wcpy(g_ff_msg, L"ffmpeg.exe wasn't in the download as expected", 400); goto clean;
    }
    wb_init(&b, src, MAX_PATH); wb_s(&b, sub); wb_s(&b, L"\\LICENSE");
    wb_init(&b, cmd, 4096); wb_s(&b, bindir); wb_s(&b, L"\\ffmpeg-LICENSE.txt");
    MoveFileExW(src, cmd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);

    /* make sure it runs and has the encoder we need */
    set_stage(L"testing ffmpeg", 0); set_prog(950);
    memset(&r, 0, sizeof r); r.cap = cap; r.capmax = sizeof cap; cap[0] = 0;
    wb_init(&b, cmd, 4096); q_arg(&b, dst); wb_s(&b, L" -hide_banner -encoders");
    run_proc(dst, cmd, &r);
    if (!sfind(cap, "libx264")) {
        DeleteFileW(dst);
        wcpy(g_ff_msg, L"the downloaded ffmpeg doesn't run or has no libx264", 400); goto clean;
    }
    wcpy(g_ff_path, dst, MAX_PATH);
    set_prog(1000);
    g_ff_ok = 1;
clean:
    DeleteFileW(zip);
    if (made_tmp) rm_tree(tmp);
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (net) InternetCloseHandle(net);
done:
    if (g_cancel) g_ff_cancelled = 1;
    PostMessageW(g_wnd, WM_APP_FFDONE, 0, 0);
    return 0;
}

/* --------------------------------------------------- Explorer right-click */

static const WCHAR *VIDEO_EXT[] = { L".mp4", L".mov", L".mkv", L".webm", L".avi", L".m4v",
    L".wmv", L".flv", L".ts", L".mts", L".m2ts", L".mpg", L".mpeg", L".3gp", L".gif", 0 };
#define MENU_KEY L"Software\\Classes\\SystemFileAssociations\\%s\\shell\\dshrink"

static int menu_installed(void)
{
    WCHAR k[200]; HKEY h;
    wsprintfW(k, MENU_KEY L"\\command", L".mp4");
    if (RegOpenKeyExW(HKEY_CURRENT_USER, k, 0, KEY_READ, &h)) return 0;
    RegCloseKey(h);
    return 1;
}
static void reg_sz(HKEY h, const WCHAR *name, const WCHAR *v)
{
    RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * 2);
}
static void menu_install(int on)
{
    WCHAR exe[MAX_PATH], k[200], cmd[MAX_PATH + 20], icon[MAX_PATH + 4]; HKEY h; int i; WB b;
    GetModuleFileNameW(0, exe, MAX_PATH);
    wb_init(&b, cmd, MAX_PATH + 20); q_arg(&b, exe); wb_s(&b, L" \"%1\"");
    wb_init(&b, icon, MAX_PATH + 4); wb_s(&b, exe); wb_s(&b, L",0");
    for (i = 0; VIDEO_EXT[i]; i++) {
        if (!lstrcmpW(VIDEO_EXT[i], L".gif")) continue;
        wsprintfW(k, MENU_KEY, VIDEO_EXT[i]);
        if (!on) { RegDeleteTreeW(HKEY_CURRENT_USER, k); RegDeleteKeyW(HKEY_CURRENT_USER, k); continue; }
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, k, 0, 0, 0, KEY_WRITE, 0, &h, 0)) {
            reg_sz(h, 0, L"Downsize with dshrink");
            reg_sz(h, L"Icon", icon);
            reg_sz(h, L"MultiSelectModel", L"Player");   /* one window gets them all */
            RegCloseKey(h);
        }
        lstrcatW(k, L"\\command");
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, k, 0, 0, 0, KEY_WRITE, 0, &h, 0)) {
            reg_sz(h, 0, cmd);
            RegCloseKey(h);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
}

/* ------------------------------------------------------------- settings */

static void load_settings(WCHAR *limit, int cap, int *fps30)
{
    HKEY h; DWORD t, n = cap * 2, d = 0, dn = 4;
    wcpy(limit, L"20", cap); *fps30 = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\dshrink", 0, KEY_READ, &h)) return;
    if (RegQueryValueExW(h, L"LimitMB", 0, &t, (BYTE *)limit, &n) || t != REG_SZ) wcpy(limit, L"20", cap);
    limit[cap - 1] = 0;
    if (!RegQueryValueExW(h, L"Fps30", 0, &t, (BYTE *)&d, &dn) && t == REG_DWORD) *fps30 = d != 0;
    RegCloseKey(h);
}
static void save_settings(const WCHAR *limit, int fps30)
{
    HKEY h; DWORD d = fps30;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\dshrink", 0, 0, 0, KEY_WRITE, 0, &h, 0)) return;
    reg_sz(h, L"LimitMB", limit);
    RegSetValueExW(h, L"Fps30", 0, REG_DWORD, (const BYTE *)&d, 4);
    RegCloseKey(h);
}

/* ---------------------------------------------------------------- window */

#define CLASS_NAME L"dshrinkWnd"
#define COPYDATA_ID 0x44534852     /* 'DSHR' */
#define IDM_MENU 0x100             /* system-menu items must be < 0xF000 and multiples of 16 */
enum { ID_TITLE = 10, ID_PROG, ID_STAGE, ID_STATUS, ID_DETECT, ID_LIMIT_L, ID_LIMIT,
       ID_MB_L, ID_FPS_L, ID_FPS, ID_CANCEL, ID_FOLDER, ID_GETFF };
enum { S_IDLE, S_BUSY, S_DONE };

static HINSTANCE g_inst;
static HWND h_title, h_prog, h_stage, h_status, h_detect, h_limit_l, h_limit, h_mb_l,
            h_fps_l, h_fps, h_cancel, h_folder, h_getff, h_tip;
static int g_dl;               /* the ffmpeg download is running */
static HFONT f_title, f_norm, f_bold, f_small;
static HBRUSH br_state[3], br_hover;
static COLORREF col_state[3] = { RGB(0xee, 0xf2, 0xf7), RGB(0xff, 0xf7, 0xe6), RGB(0xea, 0xf7, 0xee) };
static int g_state = S_IDLE, g_hover, g_dpi = 96, g_status_kind;
static HANDLE g_thread;
static WCHAR **g_queue; static int g_qn, g_qcap;
static WCHAR g_last_out[1024], g_cur_name[300], g_tipbuf[1600];
static WCHAR g_logpath[MAX_PATH]; static int g_autoclose = -1;
static WCHAR **g_selftest; static int g_selftest_n;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static int is_video(const WCHAR *p)
{
    const WCHAR *ext = 0, *s; int i; DWORD a;
    for (s = p; *s; s++) if (*s == '.') ext = s; else if (*s == '\\') ext = 0;
    if (!ext) return 0;
    for (i = 0; VIDEO_EXT[i]; i++) if (!lstrcmpiW(ext, VIDEO_EXT[i])) break;
    if (!VIDEO_EXT[i]) return 0;
    a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void set_text(HWND h, const WCHAR *t)
{
    WCHAR cur[1600];
    GetWindowTextW(h, cur, 1600);
    if (lstrcmpW(cur, t)) SetWindowTextW(h, t);
}
static void set_status(const WCHAR *t, int kind) { g_status_kind = kind; set_text(h_status, t); InvalidateRect(h_status, 0, TRUE); }

static void update_title(void)
{
    WCHAR t[400]; WB b;
    wb_init(&b, t, 400);
    if (g_hover) wb_s(&b, L"Release to shrink");
    else if (g_dl) wb_s(&b, L"Getting ffmpeg (one time only)");
    else if (g_state == S_BUSY) {
        wb_s(&b, g_cur_name);
        if (g_qn) { wb_s(&b, L"   (+"); wb_u(&b, g_qn); wb_s(&b, L" queued)"); }
    } else wb_s(&b, L"Drop videos anywhere in this window");
    set_text(h_title, t);
}

static void set_state(int s)
{
    g_state = s;
    ShowWindow(h_cancel, s == S_BUSY ? SW_SHOW : SW_HIDE);
    ShowWindow(h_folder, s != S_BUSY && g_last_out[0] && g_ffmpeg[0] ? SW_SHOW : SW_HIDE);
    ShowWindow(h_getff, s != S_BUSY && !g_ffmpeg[0] ? SW_SHOW : SW_HIDE);
    update_title();
    InvalidateRect(g_wnd, 0, TRUE);
}

static void layout(void)
{
    RECT rc; int w, h, x0, x1, y, by;
    GetClientRect(g_wnd, &rc);
    w = rc.right; h = rc.bottom;
    x0 = S(18); x1 = w - S(18);
    y = S(14);
    MoveWindow(h_title, x0, y, x1 - x0, S(24), TRUE); y += S(30);
    MoveWindow(h_prog, x0, y, x1 - x0, S(16), TRUE); y += S(20);
    MoveWindow(h_stage, x0, y, x1 - x0, S(16), TRUE); y += S(20);
    MoveWindow(h_status, x0, y, x1 - x0, S(34), TRUE); y += S(36);
    by = h - S(40);
    MoveWindow(h_detect, x0, y, x1 - x0, by - y - S(4) > S(30) ? by - y - S(4) : S(30), TRUE);
    MoveWindow(h_limit_l, x0, by + S(4), S(30), S(18), TRUE);
    MoveWindow(h_limit, x0 + S(32), by, S(56), S(200), TRUE);
    MoveWindow(h_mb_l, x0 + S(92), by + S(4), S(24), S(18), TRUE);
    MoveWindow(h_fps_l, x0 + S(124), by + S(4), S(66), S(18), TRUE);
    MoveWindow(h_fps, x0 + S(192), by, S(62), S(200), TRUE);
    MoveWindow(h_cancel, x1 - S(80), by - S(1), S(80), S(26), TRUE);
    MoveWindow(h_folder, x1 - S(96), by - S(1), S(96), S(26), TRUE);
    MoveWindow(h_getff, x1 - S(128), by - S(1), S(128), S(26), TRUE);
}

static HWND mk(const WCHAR *cls, const WCHAR *txt, DWORD style, int id, HFONT f)
{
    HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                             g_wnd, (HMENU)(INT_PTR)id, g_inst, 0);
    SendMessageW(h, WM_SETFONT, (WPARAM)f, 0);
    return h;
}

static HFONT mkfont(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void read_settings_ui(WCHAR *limit, int cap, int *fps30)
{
    GetWindowTextW(h_limit, limit, cap);
    *fps30 = (int)SendMessageW(h_fps, CB_GETCURSEL, 0, 0) == 1;
}

static void enqueue(const WCHAR *path)
{
    WCHAR full[1024]; int n;
    if (!GetFullPathNameW(path, 1024, full, 0)) wcpy(full, path, 1024);
    if (!is_video(full)) {
        WCHAR t[400]; WB b; wb_init(&b, t, 400);
        wb_s(&b, L"Not a video: "); wb_s(&b, path_name(full));
        set_status(t, 2);
        return;
    }
    if (g_qn == g_qcap) {
        WCHAR **nq; g_qcap = g_qcap ? g_qcap * 2 : 16;
        nq = (WCHAR **)xalloc(g_qcap * sizeof *nq);
        if (g_queue) { memcpy(nq, g_queue, g_qn * sizeof *nq); xfree(g_queue); }
        g_queue = nq;
    }
    n = lstrlenW(full) + 1;
    g_queue[g_qn] = (WCHAR *)xalloc(n * 2);
    wcpy(g_queue[g_qn++], full, n);
}

static void start_next(void)
{
    WCHAR limit[32]; int fps30; double mb;
    if (g_thread || !g_qn) { update_title(); return; }
    if (!g_ffmpeg[0]) {
        WCHAR t[200]; WB b; wb_init(&b, t, 200);
        wb_u(&b, g_qn); wb_s(&b, g_qn == 1 ? L" video is" : L" videos are");
        wb_s(&b, L" waiting. Click Download ffmpeg (free, about 115 MB, one time) and "
                 L"encoding starts right after.");
        set_status(t, 0);
        return;
    }
    read_settings_ui(limit, 32, &fps30);
    {   char a[32]; int i; for (i = 0; i < 31 && limit[i]; i++) a[i] = (char)limit[i]; a[i] = 0; mb = satof(a); }
    if (mb <= 0) { set_status(L"The limit must be a number of MB, e.g. 20.", 2); return; }
    save_settings(limit, fps30);

    wcpy(g_src, g_queue[0], 1024);
    xfree(g_queue[0]);
    memcpy(g_queue, g_queue + 1, (g_qn - 1) * sizeof *g_queue); g_qn--;
    g_limit_mb = mb; g_max_fps = fps30 ? 30 : 60;
    wcpy(g_cur_name, path_name(g_src), 300);
    g_cancel = 0;
    EnterCriticalSection(&g_cs);
    g_sh.detect[0] = 0; g_sh.reason[0] = 0; g_sh.note[0] = 0; g_sh.stage[0] = 0; g_sh.prog = 0;
    LeaveCriticalSection(&g_cs);
    SendMessageW(h_prog, PBM_SETPOS, 0, 0);
    set_status(L"", 0);
    set_text(h_detect, L"");
    g_last_out[0] = 0;
    set_state(S_BUSY);
    g_thread = CreateThread(0, 1 << 20, worker, 0, 0, 0);   /* committed: no stack probes */
}

static void refresh(void)
{
    WCHAR stage[200], det[600], note[200]; WB b; LONG prog; DWORD t0, el;
    InterlockedExchange(&g_posted, 0);
    EnterCriticalSection(&g_cs);
    prog = g_sh.prog; t0 = g_sh.t0;
    wcpy(det, g_sh.detect, 600); wcpy(note, g_sh.note, 200);
    if (lstrcmpW(g_tipbuf, g_sh.reason)) {
        TOOLINFOW ti;
        wcpy(g_tipbuf, g_sh.reason, 1600);
        memset(&ti, 0, sizeof ti); ti.cbSize = sizeof ti; ti.hwnd = g_wnd;
        ti.uId = (UINT_PTR)h_detect; ti.lpszText = g_tipbuf;
        SendMessageW(h_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
    wb_init(&b, stage, 200); wb_s(&b, g_sh.stage);
    LeaveCriticalSection(&g_cs);
    if (g_state != S_BUSY) return;
    SendMessageW(h_prog, PBM_SETPOS, prog, 0);
    if (stage[0] && prog > 0) {
        wb_s(&b, L"  -  "); wb_u(&b, prog / 10); wb_c(&b, '%');
        el = GetTickCount() - t0;
        if (prog > 30 && prog < 1000 && el > 2000) {
            DWORD rem = (DWORD)((double)el / prog * (1000 - prog) / 1000);
            wb_s(&b, L"  -  "); wb_u(&b, rem / 60); wb_c(&b, ':');
            if (rem % 60 < 10) wb_c(&b, '0');
            wb_u(&b, rem % 60); wb_s(&b, L" left");
        }
    }
    set_text(h_stage, stage);
    set_text(h_detect, det);
    if (note[0]) set_status(note, 0);
}

static void log_result(const Result *r)
{
    char line[2048]; WCHAR w[1600]; WB b; HANDLE f; DWORD wr; int n;
    if (!g_logpath[0]) return;
    wb_init(&b, w, 1600);
    wb_s(&b, L"RESULT kind="); wb_u(&b, r->kind); wb_s(&b, L" out="); wb_s(&b, r->out);
    wb_s(&b, L" size="); wb_u(&b, r->size); wb_s(&b, L" res="); wb_u(&b, r->w); wb_c(&b, 'x');
    wb_u(&b, r->h); wb_c(&b, '@'); wb_g(&b, r->fps); wb_s(&b, L" msg="); wb_s(&b, r->msg);
    wb_s(&b, L"\r\n");
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, line, sizeof line, 0, 0) - 1;
    f = CreateFileW(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ, 0, OPEN_ALWAYS, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { WriteFile(f, line, n, &wr, 0); CloseHandle(f); }
}

static void finished(void)
{
    Result *r = &g_res; WCHAR t[700]; WB b;
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread); g_thread = 0;
    refresh();
    wb_init(&b, t, 700);
    if (r->kind == K_DONE) {
        SendMessageW(h_prog, PBM_SETPOS, 1000, 0);
        wcpy(g_last_out, r->out, 1024);
        wb_s(&b, L"\x2714 "); wb_s(&b, path_name(r->out)); wb_s(&b, L"  \x00b7  ");
        wb_mb(&b, (double)r->size); wb_s(&b, L"  \x00b7  "); wb_u(&b, r->w); wb_c(&b, 'x');
        wb_u(&b, r->h); wb_c(&b, '@'); wb_g(&b, r->fps); wb_s(&b, L"  \x00b7  plays OK");
        set_status(t, 1);
        set_text(h_stage, L"Finished - 100%");
        set_state(S_DONE);
    } else if (r->kind == K_NOTHING) {
        SendMessageW(h_prog, PBM_SETPOS, 0, 0);
        wb_s(&b, g_cur_name); wb_s(&b, L" is already under the limit - nothing to do.");
        set_status(t, 1);
        set_text(h_stage, L"Nothing to do");
        set_state(S_IDLE);
    } else if (r->kind == K_CANCELLED) {
        SendMessageW(h_prog, PBM_SETPOS, 0, 0);
        set_status(L"", 0);
        set_text(h_stage, L"Cancelled - nothing was written");
        set_state(S_IDLE);
    } else {
        if (r->out[0]) wcpy(g_last_out, r->out, 1024);
        wb_s(&b, g_cur_name); wb_s(&b, L": "); wb_s(&b, r->msg);
        set_status(t, 2);
        set_text(h_stage, L"Failed");
        set_state(S_IDLE);
    }
    log_result(r);
    start_next();
    if (!g_thread && !g_qn && g_autoclose >= 0) SetTimer(g_wnd, 2, g_autoclose, 0);
}

static void start_ff_download(void)
{
    if (g_thread || g_ffmpeg[0]) return;
    g_cancel = 0; g_dl = 1;
    EnterCriticalSection(&g_cs);
    g_sh.detect[0] = 0; g_sh.reason[0] = 0; g_sh.note[0] = 0; g_sh.stage[0] = 0; g_sh.prog = 0;
    LeaveCriticalSection(&g_cs);
    SendMessageW(h_prog, PBM_SETPOS, 0, 0);
    set_status(L"", 0);
    set_state(S_BUSY);
    g_thread = CreateThread(0, 1 << 20, ff_worker, 0, 0, 0);
}

static void ff_finished(void)
{
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread); g_thread = 0;
    g_dl = 0;
    if (g_ff_ok) {
        WCHAR t[MAX_PATH + 80]; WB b;
        wcpy(g_ffmpeg, g_ff_path, MAX_PATH);
        SendMessageW(h_prog, PBM_SETPOS, 1000, 0);
        set_text(h_stage, L"ffmpeg is ready");
        wb_init(&b, t, MAX_PATH + 80); wb_s(&b, L"\x2714 ffmpeg installed: "); wb_s(&b, g_ffmpeg);
        set_status(t, 1);
        set_state(S_DONE);
    } else if (g_ff_cancelled) {
        SendMessageW(h_prog, PBM_SETPOS, 0, 0);
        set_text(h_stage, L"Download cancelled - nothing was kept");
        set_status(L"", 0);
        set_state(S_IDLE);
    } else {
        WCHAR t[500]; WB b;
        SendMessageW(h_prog, PBM_SETPOS, 0, 0);
        set_text(h_stage, L"Download failed");
        wb_init(&b, t, 500); wb_s(&b, L"ffmpeg download failed: "); wb_s(&b, g_ff_msg);
        set_status(t, 2);
        set_state(S_IDLE);
    }
    start_next();                          /* anything dropped meanwhile */
    if (!g_thread && !g_qn && g_autoclose >= 0) SetTimer(g_wnd, 2, g_autoclose, 0);
}

static void browse(void)
{
    static WCHAR buf[32768];
    OPENFILENAMEW of; WCHAR *p, dir[1024], f[2048]; WB b;
    memset(&of, 0, sizeof of); buf[0] = 0;
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd; of.lpstrFile = buf; of.nMaxFile = 32768;
    of.lpstrFilter = L"Videos\0*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.wmv;*.flv;*.ts;*.mts;*.m2ts;*.mpg;*.mpeg;*.3gp;*.gif\0All files\0*.*\0";
    of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    p = buf + lstrlenW(buf) + 1;
    if (!*p) { enqueue(buf); }
    else {
        wcpy(dir, buf, 1024);
        for (; *p; p += lstrlenW(p) + 1) {
            wb_init(&b, f, 2048); wb_s(&b, dir); wb_c(&b, '\\'); wb_s(&b, p);
            enqueue(f);
        }
    }
    start_next();
}

static void add_hdrop(HDROP hd)
{
    UINT n = DragQueryFileW(hd, 0xFFFFFFFF, 0, 0), i;
    WCHAR f[1024];
    for (i = 0; i < n; i++) if (DragQueryFileW(hd, i, f, 1024)) enqueue(f);
    start_next();
}

/* ---- OLE drop target: works over every child and shows hover feedback */

typedef struct { IDropTarget dt; int ok; } DropT;
static HRESULT STDMETHODCALLTYPE dt_qi(IDropTarget *t, REFIID riid, void **pv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) { *pv = t; return S_OK; }
    *pv = 0; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dt_ref(IDropTarget *t) { (void)t; return 1; }
static void hover(int on) { g_hover = on; update_title(); InvalidateRect(g_wnd, 0, TRUE); }
static FORMATETC fe_hdrop = { CF_HDROP, 0, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
static HRESULT STDMETHODCALLTYPE dt_enter(IDropTarget *t, IDataObject *d, DWORD k, POINTL pt, DWORD *eff)
{
    DropT *me = (DropT *)t; (void)k; (void)pt;
    me->ok = IDataObject_QueryGetData(d, &fe_hdrop) == S_OK;
    *eff = me->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (me->ok) hover(1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dt_over(IDropTarget *t, DWORD k, POINTL pt, DWORD *eff)
{
    (void)k; (void)pt; *eff = ((DropT *)t)->ok ? DROPEFFECT_COPY : DROPEFFECT_NONE; return S_OK;
}
static HRESULT STDMETHODCALLTYPE dt_leave(IDropTarget *t) { (void)t; hover(0); return S_OK; }
static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *t, IDataObject *d, DWORD k, POINTL pt, DWORD *eff)
{
    STGMEDIUM m; (void)t; (void)k; (void)pt;
    hover(0);
    *eff = DROPEFFECT_NONE;
    if (IDataObject_GetData(d, &fe_hdrop, &m) == S_OK) {
        add_hdrop((HDROP)m.hGlobal);       /* DragQueryFile takes the handle itself */
        *eff = DROPEFFECT_COPY;
        ReleaseStgMedium(&m);
    }
    SetForegroundWindow(g_wnd);
    return S_OK;
}
static IDropTargetVtbl dt_vtbl = { dt_qi, dt_ref, dt_ref, dt_enter, dt_over, dt_leave, dt_drop };
static DropT g_drop = { { &dt_vtbl }, 0 };

/* self-test: build an HDROP exactly as Explorer's legacy path does */
static void post_selftest_drop(void)
{
    SIZE_T total = sizeof(DROPFILES) + 2; int i; HGLOBAL h; BYTE *p; DROPFILES *df; WCHAR *w;
    RECT rc;
    for (i = 0; i < g_selftest_n; i++) total += (lstrlenW(g_selftest[i]) + 1) * 2;
    h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    p = (BYTE *)GlobalLock(h); df = (DROPFILES *)p;
    GetClientRect(g_wnd, &rc);
    df->pFiles = sizeof(DROPFILES); df->fWide = TRUE;
    df->pt.x = rc.right - 20; df->pt.y = rc.bottom - 20;   /* a corner, away from the title */
    w = (WCHAR *)(p + sizeof(DROPFILES));
    for (i = 0; i < g_selftest_n; i++) { lstrcpyW(w, g_selftest[i]); w += lstrlenW(w) + 1; }
    GlobalUnlock(h);
    PostMessageW(g_wnd, WM_DROPFILES, (WPARAM)h, 0);
}

/* self-test of the OLE path: a shell data object for the files (what Explorer
   hands a drop target), fed through DragEnter + Drop */
static int g_selftest_ole;
static IDataObject *g_st_data;
static void selftest_ole_drop(void)          /* DragEnter now, Drop 2.5 s later */
{
    PIDLIST_ABSOLUTE pidl[64]; IShellItemArray *arr = 0;
    int i, n = g_selftest_n < 64 ? g_selftest_n : 64; DWORD eff = DROPEFFECT_COPY; POINTL pt = { 0, 0 };
    for (i = 0; i < n; i++) pidl[i] = ILCreateFromPathW(g_selftest[i]);
    if (SHCreateShellItemArrayFromIDLists(n, (LPCITEMIDLIST *)pidl, &arr) == S_OK &&
        IShellItemArray_BindToHandler(arr, 0, &BHID_DataObject, &IID_IDataObject, (void **)&g_st_data) == S_OK) {
        g_drop.dt.lpVtbl->DragEnter(&g_drop.dt, g_st_data, 0, pt, &eff);
        SetTimer(g_wnd, 3, 2500, 0);         /* long enough to see "Release to shrink" */
    } else {
        set_status(L"self-test: could not build a shell data object", 2);
    }
    if (arr) IShellItemArray_Release(arr);
    for (i = 0; i < n; i++) ILFree(pidl[i]);
}
static void selftest_ole_finish(void)
{
    DWORD eff = DROPEFFECT_COPY; POINTL pt = { 0, 0 };
    if (!g_st_data) return;
    g_drop.dt.lpVtbl->Drop(&g_drop.dt, g_st_data, 0, pt, &eff);
    IDataObject_Release(g_st_data); g_st_data = 0;
}

static void paint_bg(HDC dc)
{
    RECT rc; HPEN pen, op; HBRUSH ob;
    GetClientRect(g_wnd, &rc);
    FillRect(dc, &rc, br_state[g_state]);
    pen = CreatePen(PS_SOLID, S(2), g_hover ? RGB(0x2f, 0x7d, 0xe1) : RGB(0x9f, 0xb3, 0xc8));
    op = (HPEN)SelectObject(dc, pen); ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, S(6), S(6), rc.right - S(6), rc.bottom - S(6), S(10), S(10));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: {
        WCHAR limit[32]; int fps30; TOOLINFOW ti; HMENU sm;
        g_wnd = w;
        f_title = mkfont(12, FW_NORMAL); f_norm = mkfont(9, FW_NORMAL);
        f_bold = mkfont(9, FW_BOLD); f_small = mkfont(8, FW_NORMAL);
        h_title = mk(L"STATIC", L"", SS_CENTER | SS_NOTIFY | SS_ENDELLIPSIS, ID_TITLE, f_title);
        h_prog = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PROG, f_norm);
        SendMessageW(h_prog, PBM_SETRANGE32, 0, 1000);
        h_stage = mk(L"STATIC", L"or click here to choose a file", SS_NOTIFY | SS_ENDELLIPSIS, ID_STAGE, f_norm);
        h_status = mk(L"STATIC", L"", 0, ID_STATUS, f_bold);
        h_detect = mk(L"STATIC", L"", SS_NOTIFY, ID_DETECT, f_small);
        h_limit_l = mk(L"STATIC", L"Limit", 0, ID_LIMIT_L, f_norm);
        h_limit = mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL, ID_LIMIT, f_norm);
        h_mb_l = mk(L"STATIC", L"MB", 0, ID_MB_L, f_norm);
        h_fps_l = mk(L"STATIC", L"Frame rate", 0, ID_FPS_L, f_norm);
        h_fps = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_FPS, f_norm);
        h_cancel = mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ID_CANCEL, f_norm);
        h_folder = mk(L"BUTTON", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, ID_FOLDER, f_norm);
        h_getff = mk(L"BUTTON", L"Download ffmpeg", BS_PUSHBUTTON | WS_TABSTOP, ID_GETFF, f_norm);
        ShowWindow(h_getff, SW_HIDE);
        SendMessageW(h_limit, CB_ADDSTRING, 0, (LPARAM)L"10");
        SendMessageW(h_limit, CB_ADDSTRING, 0, (LPARAM)L"20");
        SendMessageW(h_limit, CB_ADDSTRING, 0, (LPARAM)L"50");
        SendMessageW(h_limit, CB_ADDSTRING, 0, (LPARAM)L"500");
        SendMessageW(h_fps, CB_ADDSTRING, 0, (LPARAM)L"Auto");
        SendMessageW(h_fps, CB_ADDSTRING, 0, (LPARAM)L"30");
        load_settings(limit, 32, &fps30);
        SetWindowTextW(h_limit, limit);
        SendMessageW(h_limit, CB_SETEDITSEL, 0, MAKELPARAM(-1, 0));   /* no stray highlight */
        SendMessageW(h_fps, CB_SETCURSEL, fps30, 0);
        ShowWindow(h_cancel, SW_HIDE); ShowWindow(h_folder, SW_HIDE);

        h_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, 0, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                0, 0, 0, 0, w, 0, g_inst, 0);
        memset(&ti, 0, sizeof ti); ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS; ti.hwnd = w; ti.uId = (UINT_PTR)h_detect;
        ti.lpszText = g_tipbuf;
        SendMessageW(h_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(h_tip, TTM_SETMAXTIPWIDTH, 0, S(520));
        SendMessageW(h_tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);

        sm = GetSystemMenu(w, FALSE);
        AppendMenuW(sm, MF_SEPARATOR, 0, 0);
        AppendMenuW(sm, MF_STRING | (menu_installed() ? MF_CHECKED : 0), IDM_MENU,
                    L"Show \"Downsize with dshrink\" in Explorer's right-click menu");
        DragAcceptFiles(w, TRUE);                 /* legacy path (and the self-test) */
        RegisterDragDrop(w, &g_drop.dt);          /* OLE path: hover + any child */
        update_title();
        layout();
        return 0;
    }
    case WM_SIZE: layout(); InvalidateRect(w, 0, TRUE); return 0;
    case WM_ACTIVATE:                       /* keep focus off the Limit box unless clicked */
        if (LOWORD(wp) == WA_ACTIVE) { SetFocus(w); return 0; }
        break;
    case WM_GETMINMAXINFO: {
        RECT r = { 0, 0, S(400), S(236) };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = r.right - r.left;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_ERASEBKGND: paint_bg((HDC)wp); return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps); paint_bg(dc); EndPaint(w, &ps); return 0; }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp; HWND c = (HWND)lp; COLORREF fg = RGB(0x5d, 0x6d, 0x7e);
        if (c == h_title) fg = RGB(0x2c, 0x3e, 0x50);
        else if (c == h_status)
            fg = g_status_kind == 1 ? RGB(0x1a, 0x7f, 0x37) : g_status_kind == 2 ? RGB(0xc0, 0x39, 0x2b) : RGB(0x2c, 0x3e, 0x50);
        SetTextColor(dc, fg);
        SetBkColor(dc, col_state[g_state]);
        return (LRESULT)br_state[g_state];
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_LIMIT:                      /* the combo's edit keeps its selection drawn */
            if (HIWORD(wp) == CBN_KILLFOCUS)
                SendMessageW(GetWindow(h_limit, GW_CHILD), EM_SETSEL, (WPARAM)-1, 0);
            return 0;
        case ID_TITLE: case ID_STAGE:
            if (HIWORD(wp) == STN_CLICKED && g_state != S_BUSY) browse();
            return 0;
        case ID_CANCEL:
            g_qn = 0;
            InterlockedExchange(&g_cancel, 1);
            EnterCriticalSection(&g_cs);
            if (g_proc) TerminateProcess(g_proc, 1);
            LeaveCriticalSection(&g_cs);
            set_text(h_stage, L"Cancelling...");
            return 0;
        case ID_GETFF:
            start_ff_download();
            return 0;
        case ID_FOLDER:
            if (g_last_out[0]) {
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(g_last_out);
                if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, 0, 0); ILFree(pidl); }
            }
            return 0;
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == IDM_MENU) {
            int on = !menu_installed();
            menu_install(on);
            CheckMenuItem(GetSystemMenu(w, FALSE), IDM_MENU, menu_installed() ? MF_CHECKED : MF_UNCHECKED);
            set_status(on ? L"Added \"Downsize with dshrink\" to Explorer's right-click menu."
                          : L"Removed \"Downsize with dshrink\" from Explorer's right-click menu.", 1);
            return 0;
        }
        break;
    case WM_DROPFILES:
        add_hdrop((HDROP)wp);
        DragFinish((HDROP)wp);
        return 0;
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp; const WCHAR *p;
        if (cd->dwData != COPYDATA_ID) return FALSE;
        for (p = (const WCHAR *)cd->lpData; *p; p += lstrlenW(p) + 1) enqueue(p);
        start_next();
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        return TRUE;
    }
    case WM_APP_REFRESH: refresh(); return 0;
    case WM_APP_DONE: finished(); return 0;
    case WM_APP_FFDONE: ff_finished(); return 0;
    case WM_APP_START:
        SendMessageW(GetWindow(h_limit, GW_CHILD), EM_SETSEL, (WPARAM)-1, 0);
        start_next();
        return 0;
    case WM_APP_SELFTEST: if (g_selftest_ole) selftest_ole_drop(); else post_selftest_drop(); return 0;
    case WM_TIMER:
        if (wp == 2) { KillTimer(w, 2); DestroyWindow(w); }
        if (wp == 3) { KillTimer(w, 3); selftest_ole_finish(); }
        return 0;
    case WM_CLOSE:
        if (g_thread) {                       /* stop ffmpeg; the worker cleans up */
            g_qn = 0;
            InterlockedExchange(&g_cancel, 1);
            EnterCriticalSection(&g_cs);
            if (g_proc) TerminateProcess(g_proc, 1);
            LeaveCriticalSection(&g_cs);
            WaitForSingleObject(g_thread, 5000);
        }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: {
        WCHAR limit[32]; int fps30;
        read_settings_ui(limit, 32, &fps30);
        save_settings(limit, fps30);
        RevokeDragDrop(w);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* ------------------------------------------------------------- start-up */

static void find_ffmpeg(void)
{
    WCHAR exe[MAX_PATH], t[MAX_PATH]; WB b; int i;
    static const WCHAR *rel[] = { L"ffmpeg.exe", L"bin\\ffmpeg.exe" };
    DWORD n = GetEnvironmentVariableW(L"DSHRINK_FFMPEG", t, MAX_PATH);
    if (n && n < MAX_PATH && GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) { wcpy(g_ffmpeg, t, MAX_PATH); return; }
    GetModuleFileNameW(0, exe, MAX_PATH);
    for (i = 0; i < 2; i++) {
        path_dir(exe, t, MAX_PATH);
        b.p = t; b.n = lstrlenW(t); b.cap = MAX_PATH; wb_s(&b, rel[i]);
        if (GetFileAttributesW(t) != INVALID_FILE_ATTRIBUTES) { wcpy(g_ffmpeg, t, MAX_PATH); return; }
    }
    if (SearchPathW(0, L"ffmpeg.exe", 0, MAX_PATH, t, 0)) wcpy(g_ffmpeg, t, MAX_PATH);
}

/* a second copy hands its files to the window already open, then exits */
static int hand_off(WCHAR **files, int n)
{
    WCHAR *buf; int i, len = 1, tries; WCHAR full[1024]; WB b;
    for (i = 0; i < n; i++) len += 1025;
    buf = (WCHAR *)xalloc(len * 2);
    b.p = buf; b.n = 0; b.cap = len;
    for (i = 0; i < n; i++) {
        if (!GetFullPathNameW(files[i], 1024, full, 0)) wcpy(full, files[i], 1024);
        wb_s(&b, full); b.n++;                 /* keep the NUL as a separator */
    }
    for (tries = 0; tries < 60; tries++) {
        HWND h = FindWindowW(CLASS_NAME, 0);
        if (h) {
            COPYDATASTRUCT cd; DWORD pid; DWORD_PTR r;
            GetWindowThreadProcessId(h, &pid);
            AllowSetForegroundWindow(pid);
            cd.dwData = COPYDATA_ID; cd.cbData = (b.n + 1) * 2; cd.lpData = buf;
            if (SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r)) return 1;
        }
        Sleep(100);
    }
    return 0;
}

void WinMainCRTStartup(void)
{
    int argc, i, nfiles = 0; WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WCHAR **files = (WCHAR **)xalloc((argc + 1) * sizeof *files);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSEXW wc; MSG msg; HWND w; RECT r; HDC sdc; HANDLE mtx;

    g_selftest = (WCHAR **)xalloc((argc + 1) * sizeof *g_selftest);
    for (i = 1; i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--install-menu")) { menu_install(1); ExitProcess(0); }
        if (!lstrcmpiW(argv[i], L"--uninstall-menu")) { menu_install(0); ExitProcess(0); }
        if (!lstrcmpiW(argv[i], L"--selftest-drop") && i + 1 < argc) { g_selftest[g_selftest_n++] = argv[++i]; continue; }
        if (!lstrcmpiW(argv[i], L"--autoclose") && i + 1 < argc) { char a[16]; int k; for (k = 0; k < 15 && argv[i + 1][k]; k++) a[k] = (char)argv[i + 1][k]; a[k] = 0; g_autoclose = (int)satof(a); i++; continue; }
        if (!lstrcmpiW(argv[i], L"--selftest-ole")) { g_selftest_ole = 1; continue; }
        if (!lstrcmpiW(argv[i], L"--log") && i + 1 < argc) { wcpy(g_logpath, argv[++i], MAX_PATH); continue; }
        files[nfiles++] = argv[i];
    }

    if (!g_selftest_n) {
        mtx = CreateMutexW(0, FALSE, L"Local\\dshrink-single-window");
        if (GetLastError() == ERROR_ALREADY_EXISTS && hand_off(files, nfiles)) ExitProcess(0);
        (void)mtx;
    }

    SetProcessDPIAware();
    sdc = GetDC(0); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(0, sdc);
    OleInitialize(0);
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_cs);
    g_inst = GetModuleHandleW(0);
    find_ffmpeg();
    br_state[0] = CreateSolidBrush(col_state[0]);
    br_state[1] = CreateSolidBrush(col_state[1]);
    br_state[2] = CreateSolidBrush(col_state[2]);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wndproc; wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(0, IDC_ARROW); wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    r.left = 0; r.top = 0; r.right = S(460); r.bottom = S(250);
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    w = CreateWindowExW(g_selftest_n ? WS_EX_TOPMOST : 0, CLASS_NAME, L"dshrink - Discord video shrinker",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                        0, 0, g_inst, 0);
    ShowWindow(w, SW_SHOW);
    SetFocus(w);                     /* not the Limit box: no stray selection */
    UpdateWindow(w);
    if (!g_ffmpeg[0]) {
        set_status(L"dshrink needs ffmpeg to encode. Click Download ffmpeg to get the free official "
                   L"build (about 115 MB, once, from gyan.dev).", 0);
        set_state(S_IDLE);
    }
    for (i = 0; i < nfiles; i++) enqueue(files[i]);
    PostMessageW(w, WM_APP_START, 0, 0);
    if (g_selftest_n) PostMessageW(w, WM_APP_SELFTEST, 0, 0);

    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    OleUninitialize();
    ExitProcess(0);
}
