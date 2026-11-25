// Plateau - Sokoban solution optimizer
//
// Give it a level and one or more solutions; it looks for shorter ones (fewest moves, or fewest pushes) and writes
// every new best line and every new moves/pushes trade-off line to OUT, each checked by exact replay first.
//
// How: a "plateau walk" on every thread (fastwalk.inc). Each step deletes a few boxes' pushes from a stretch of the
// current line and re-inserts them with a fast bidirectional search (hook and ball, hookball.inc); the new line is kept
// if it is at most SLACK worse than that walker's best, so the walk drifts across plateaus instead of stalling. Between
// steps, walkers take turns at two exact moves: crossover of many lines through shared positions (crossover.inc) and
// exact re-planning of 1-3 boxes' whole histories (replan.inc). The run switches between the two metrics in cycles
// (fewer pushes often opens the way to fewer moves and the other way round).
//
// Build: g++ -O3 -march=native -fopenmp -static -o plateau.exe plateau.cpp   (see BUILD.txt; add plateau_res.o for the icon)
// Run:   plateau LEVELFILE [OPTION=value ...]   (plateau --help lists the options)
#include <bits/stdc++.h>
#ifdef _OPENMP
#include <omp.h>
#endif
using namespace std;
typedef uint64_t u64; typedef uint32_t u32; typedef int64_t i64;

static double nowSec() { return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count(); }

// ------------------------------------------------------------------ level
static const int MAXW = 32;  // up to 2048 floor squares
static int NW;                 // words per layout bitset
static int N;                  // floor squares
static int GW, GH;             // grid size
static vector<int> cellOf, sqOf;   // square -> cell, cell -> square (-1 wall/outside)
static vector<array<int,4>> nb;    // neighbors: 0 L,1 R,2 U,3 D
static const char DCH[4] = {'l','r','u','d'};
static const int OPP[4] = {1,0,3,2};
static vector<u64> GOALS, BOX0;
static int PLAYER0;
static vector<char> isGoal, deadSq;
static vector<uint16_t> PD;    // push distance PD[a*N+b] ignoring other boxes, 65535 = impossible
static vector<uint16_t> WD;    // walking distance (walls only)
static int NBOX;

static inline bool tb(const u64* c, int s) { return (c[s >> 6] >> (s & 63)) & 1; }
static inline void sb(u64* c, int s) { c[s >> 6] |= 1ULL << (s & 63); }
static inline void cb(u64* c, int s) { c[s >> 6] &= ~(1ULL << (s & 63)); }

static string expandRLE(const string& s) {
    string o; int n = 0; bool hn = false; size_t i = 0;
    while (i < s.size()) {
        char ch = s[i];
        if (isdigit((unsigned char)ch)) { n = n * 10 + (ch - '0'); hn = true; i++; continue; }
        if (ch == '(') { // group
            int depth = 1; size_t j = i + 1; while (j < s.size() && depth) { if (s[j] == '(') depth++; else if (s[j] == ')') depth--; j++; }
            string inner = expandRLE(s.substr(i + 1, j - i - 2)); int rep = hn ? n : 1; for (int k = 0; k < rep; k++) o += inner;
            n = 0; hn = false; i = j; continue; }
        int rep = hn ? n : 1; for (int k = 0; k < rep; k++) o += ch; n = 0; hn = false; i++;
    }
    return o;
}
static bool isBoardLine(const string& l) {
    if (l.empty()) return false; bool hasWall = false;
    for (char c : l) { if (c == '#') hasWall = true; else if (!strchr(" -_@+$*.pPbB", c)) return false; }
    return hasWall;
}
static bool isSolLine(const string& l) {
    if (l.size() < 2) return false; bool letter = false;
    for (char c : l) { if (strchr("lurdLURD", c)) letter = true; else if (!(isdigit((unsigned char)c) || c == '(' || c == ')' || c == ' ' || c == '\r')) return false; }
    return letter;
}
static string trimR(string s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back(); return s; }

struct Pushes { vector<int> box; vector<int> dir; };   // box square before the push, push direction

static bool loadLevel(const string& path, vector<string>& solsOut) {
    ifstream f(path); if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    vector<string> lines; string l; while (getline(f, l)) lines.push_back(trimR(l));
    vector<string> rows; bool inBoard = false, done = false;
    for (auto& raw : lines) {
        // a line may hold several rows separated by '|'
        string ex = expandRLE(raw);
        vector<string> parts; { string cur; for (char c : ex) { if (c == '|') { parts.push_back(cur); cur.clear(); } else cur += c; } parts.push_back(cur); }
        bool all = !ex.empty(); for (auto& p : parts) if (!isBoardLine(p)) all = false;
        if (!done && all) { for (auto& p : parts) rows.push_back(p); inBoard = true; continue; }
        if (inBoard) { done = true; inBoard = false; }
        if (isSolLine(raw)) { string s; for (char c : expandRLE(raw)) if (strchr("lurdLURD", c)) s += c; solsOut.push_back(s); }
    }
    if (rows.empty()) { fprintf(stderr, "no board found in %s\n", path.c_str()); return false; }
    GH = rows.size(); GW = 0; for (auto& r : rows) GW = max(GW, (int)r.size());
    vector<char> g(GW * GH, ' ');
    for (int y = 0; y < GH; y++) for (int x = 0; x < (int)rows[y].size(); x++) { char c = rows[y][x]; if (c == '-' || c == '_') c = ' '; g[y * GW + x] = c; }
    int pc = -1; for (int i = 0; i < GW * GH; i++) if (g[i] == '@' || g[i] == '+') pc = i;
    if (pc < 0) { fprintf(stderr, "no player\n"); return false; }
    // floor = cells reachable from the player through non-walls
    vector<char> fl(GW * GH, 0); deque<int> q{pc}; fl[pc] = 1;
    const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
    while (!q.empty()) { int c = q.front(); q.pop_front(); int x = c % GW, y = c / GW;
        for (int d = 0; d < 4; d++) { int nx = x + dx[d], ny = y + dy[d]; if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) continue; int n2 = ny * GW + nx;
            if (!fl[n2] && g[n2] != '#') { fl[n2] = 1; q.push_back(n2); } } }
    sqOf.assign(GW * GH, -1); cellOf.clear();
    for (int i = 0; i < GW * GH; i++) if (fl[i]) { sqOf[i] = cellOf.size(); cellOf.push_back(i); }
    N = cellOf.size(); NW = (N + 63) / 64;
    if (NW > MAXW) { fprintf(stderr, "level too big: %d floor squares (max %d)\n", N, MAXW * 64); return false; }
    nb.assign(N, {-1, -1, -1, -1});
    for (int s = 0; s < N; s++) { int c = cellOf[s], x = c % GW, y = c / GW;
        for (int d = 0; d < 4; d++) { int nx = x + dx[d], ny = y + dy[d]; if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) continue; nb[s][d] = sqOf[ny * GW + nx]; } }
    GOALS.assign(NW, 0); BOX0.assign(NW, 0); isGoal.assign(N, 0); NBOX = 0; int ng = 0;
    for (int s = 0; s < N; s++) { char c = g[cellOf[s]];
        if (c == '.' || c == '*' || c == '+') { sb(GOALS.data(), s); isGoal[s] = 1; ng++; }
        if (c == '$' || c == '*') { sb(BOX0.data(), s); NBOX++; } }
    PLAYER0 = sqOf[pc];
    if (ng != NBOX) fprintf(stderr, "warning: %d boxes, %d goals\n", NBOX, ng);
    // push distances: box from a to b, ignoring other boxes, player must stand behind
    PD.assign((size_t)N * N, 65535);
    for (int a = 0; a < N; a++) { uint16_t* d = &PD[(size_t)a * N]; d[a] = 0; deque<int> qq{a};
        while (!qq.empty()) { int x = qq.front(); qq.pop_front();
            for (int dd = 0; dd < 4; dd++) { int y = nb[x][dd], p = nb[x][OPP[dd]]; if (y < 0 || p < 0 || d[y] != 65535) continue; d[y] = d[x] + 1; qq.push_back(y); } } }
    WD.assign((size_t)N * N, 65535);
    for (int a = 0; a < N; a++) { uint16_t* d = &WD[(size_t)a * N]; d[a] = 0; deque<int> qq{a};
        while (!qq.empty()) { int x = qq.front(); qq.pop_front(); for (int dd = 0; dd < 4; dd++) { int y = nb[x][dd]; if (y < 0 || d[y] != 65535) continue; d[y] = d[x] + 1; qq.push_back(y); } } }
    deadSq.assign(N, 1);
    for (int s = 0; s < N; s++) for (int t = 0; t < N; t++) if (isGoal[t] && PD[(size_t)s * N + t] != 65535) { deadSq[s] = 0; break; }
    return true;
}

// ------------------------------------------------------------------ replay / render
struct Line { Pushes P; i64 moves = 0, pushes = 0; string lurd; };

// replay a LURD string exactly; returns false if illegal or unsolved
static bool replay(const string& s, Pushes& out, i64& moves, i64& pushes) {
    vector<u64> c = BOX0; int p = PLAYER0; out.box.clear(); out.dir.clear(); moves = pushes = 0;
    for (char ch : s) { int d; switch (tolower(ch)) { case 'l': d = 0; break; case 'r': d = 1; break; case 'u': d = 2; break; case 'd': d = 3; break; default: return false; }
        int t = nb[p][d]; if (t < 0) return false;
        if (tb(c.data(), t)) { int t2 = nb[t][d]; if (t2 < 0 || tb(c.data(), t2)) return false; cb(c.data(), t); sb(c.data(), t2); out.box.push_back(t); out.dir.push_back(d); pushes++; }
        moves++; p = t; }
    return c == GOALS;
}
// BFS walk from a to b in layout c; returns path letters
struct Walker { vector<int> dist, par, st; vector<int> q; int stamp = 0;
    void init() { dist.assign(N, 0); par.assign(N, -1); st.assign(N, 0); q.resize(N); }
    // full BFS from p; fills dist for reachable squares (st==stamp)
    void bfs(const u64* c, int p) { stamp++; int h = 0, t = 0; q[t++] = p; st[p] = stamp; dist[p] = 0; par[p] = -1;
        while (h < t) { int x = q[h++]; for (int d = 0; d < 4; d++) { int y = nb[x][d]; if (y < 0 || st[y] == stamp || tb(c, y)) continue; st[y] = stamp; dist[y] = dist[x] + 1; par[y] = d; q[t++] = y; } } }
    bool reach(int s) const { return st[s] == stamp; }
    string path(int to) const { string r; int x = to; while (par[x] >= 0) { int d = par[x]; r += DCH[d]; x = nb[x][OPP[d]]; } reverse(r.begin(), r.end()); return r; }
};
// render push list into LURD with shortest walks; returns false if illegal / unsolved
static bool render(const Pushes& P, Line& L, bool wantText = true) {
    static thread_local Walker W; if ((int)W.dist.size() != N) W.init();
    vector<u64> c = BOX0; int p = PLAYER0; L.moves = 0; L.pushes = 0; L.lurd.clear(); L.P = P;
    for (size_t i = 0; i < P.box.size(); i++) { int b = P.box[i], d = P.dir[i]; int q = nb[b][OPP[d]], t = nb[b][d];
        if (q < 0 || t < 0 || !tb(c.data(), b) || tb(c.data(), t)) return false;
        W.bfs(c.data(), p); if (!W.reach(q)) return false;
        L.moves += W.dist[q] + 1; L.pushes++;
        if (wantText) { L.lurd += W.path(q); L.lurd += (char)toupper(DCH[d]); }
        cb(c.data(), b); sb(c.data(), t); p = b; }
    return c == GOALS;
}

// ------------------------------------------------------------------ metric
static int METRIC = 0;    // 0: moves then pushes; 1: pushes then moves
static const int SH = 24;
static inline u64 cost(i64 m, i64 p) { return METRIC == 0 ? ((u64)m << SH) | (u64)p : ((u64)p << SH) | (u64)m; }
static inline u64 lineCost(const Line& L) { return cost(L.moves, L.pushes); }

// ------------------------------------------------------------------ deadlocks
// freeze deadlock test for the box just pushed onto s (layout c already updated)
struct Freeze { vector<int> mark; int stamp = 0; vector<int> frozen;
    void init() { mark.assign(N, 0); }
    bool axisBlocked(const u64* c, int s, int ax) {
        int a = nb[s][ax * 2], b = nb[s][ax * 2 + 1];
        if (a < 0 || b < 0) return true;
        if (mark[a] == stamp || mark[b] == stamp) return true;
        if (deadSq[a] && deadSq[b]) return true;
        if (tb(c, a) && rec(c, a)) return true;
        if (tb(c, b) && rec(c, b)) return true;
        return false; }
    bool rec(const u64* c, int s) { mark[s] = stamp; bool r = axisBlocked(c, s, 0) && axisBlocked(c, s, 1); if (r) frozen.push_back(s); return r; }
    bool deadlock(const u64* c, int s) { stamp++; frozen.clear(); if (!rec(c, s)) return false;
        for (int x : frozen) if (!isGoal[x]) return true; return false; }
};


// ------------------------------------------------------------------ elite pool and output
struct Elite { Line L; u64 c; };
static vector<Elite> pool; static int POOLMAX = 8;   // the best distinct lines on the current metric, cheapest first
static set<string> seenLines;
static string OUTF = "plateau_best.txt";
static FILE* LOGFP = nullptr;
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); if (LOGFP) { fprintf(LOGFP, __VA_ARGS__); fflush(LOGFP); } } while (0)
static double T0;
static int THREADS = 1;
static bool QUIET = false;   // during a metric detour: no NEW BEST lines (they are on the other metric)
// moves/pushes Pareto front of everything written to OUT (and the input lines): any verified line that no line on it
// dominates is written too, so good lines of the other metric (found during detours) are not lost
static vector<pair<i64,i64>> FRONT; static vector<Line> FRONTL;   // FRONTL[i] is the line of FRONT[i]
static bool frontAdd(const Line& L) { i64 m = L.moves, p = L.pushes; for (auto& q : FRONT) if (q.first <= m && q.second <= p) return false;
    vector<pair<i64,i64>> nf; vector<Line> nl; for (size_t i = 0; i < FRONT.size(); i++) if (!(m <= FRONT[i].first && p <= FRONT[i].second)) { nf.push_back(FRONT[i]); nl.push_back(FRONTL[i]); }
    nf.push_back({m, p}); nl.push_back(L); FRONT = nf; FRONTL = nl; return true; }
// offer a line to the pool; writes it to OUT if it is a new best or a new trade-off. Returns true for a new best.
// Output format (read by plateau_gui.exe; keep it): "Solution: <m> moves, <p> pushes (Plateau <tag>...)", the line, a blank line.
static bool addElite(const Line& L, const char* tag) {
    // verify by exact replay of the text
    Pushes chk; i64 m, p; if (!replay(L.lurd, chk, m, p) || m != L.moves || p != L.pushes) { LOG("!! verification failed for a %s line\n", tag); return false; }
    if (seenLines.count(L.lurd)) return false; seenLines.insert(L.lurd);
    Elite e; e.L = L; e.c = lineCost(L);
    bool newBest = pool.empty() || e.c < pool[0].c;
    pool.push_back(move(e)); sort(pool.begin(), pool.end(), [](const Elite& a, const Elite& b) { return a.c < b.c; });
    if ((int)pool.size() > POOLMAX) pool.resize(POOLMAX);
    if (!(newBest && !QUIET) && frontAdd(L)) { // not a new best, but a new moves/pushes trade-off
        FILE* f = fopen(OUTF.c_str(), "a"); if (f) { fprintf(f, "Solution: %lld moves, %lld pushes (Plateau pareto, %s, verified by replay)\n%s\n\n", (long long)L.moves, (long long)L.pushes, tag, L.lurd.c_str()); fclose(f); } }
    if (newBest && !QUIET) { frontAdd(L);
        LOG("[%7.1fs] NEW BEST %lld/%lld (%s)\n", nowSec() - T0, (long long)L.moves, (long long)L.pushes, tag);
        FILE* f = fopen(OUTF.c_str(), "a"); if (f) { fprintf(f, "Solution: %lld moves, %lld pushes (Plateau %s, verified by replay)\n%s\n\n", (long long)L.moves, (long long)L.pushes, tag, L.lurd.c_str()); fclose(f); }
    }
    return newBest;
}

#include "replan.inc"
#include "crossover.inc"
#include "fastwalk.inc"
#include "calib.inc"

static Line readBest(const vector<string>& sols, vector<Line>& all) {
    for (auto& s : sols) { Pushes P; i64 m, p; if (!replay(s, P, m, p)) continue; Line L; if (!render(P, L)) continue; all.push_back(L); }
    sort(all.begin(), all.end(), [](const Line& a, const Line& b) { return lineCost(a) < lineCost(b); });
    return all.empty() ? Line() : all[0];
}

// ------------------------------------------------------------------ options
struct OptInfo { const char* key; const char* def; const char* help; };
static const OptInfo OPTS[] = {
    {"P", "0", "what to minimise first: 0 = moves (then pushes), 1 = pushes (then moves)"},
    {"TIME", "600", "run time in seconds"},
    {"THREADS", "4", "walker threads (use the number of cores)"},
    {"SEED", "1", "random seed (different seeds walk differently)"},
    {"OUT", "plateau_best.txt", "file that new best and trade-off lines are appended to"},
    {"LOG", "", "file that the console output is also appended to"},
    {"SOLS", "", "extra file with solution lines (besides those in LEVELFILE)"},
    {"POOL", "8", "size of the elite pool (best distinct lines kept)"},
    {"SLACK", "10 / 4", "a walker keeps a line up to this much worse than its best (moves runs / pushes runs)"},
    {"SLACKM", "SLACK", "slack while walking on moves (overrides SLACK)"},
    {"SLACKP", "SLACK", "slack while walking on pushes (overrides SLACK); 6 helps some pushes runs"},
    {"KICK", "50", "percent of steps that ban one square the re-planned boxes used"},
    {"RESTART", "6000", "steps without improvement before a walker restarts from the pool"},
    {"ARM", "-3", "step size: 0 short, 1 medium, 2 long, 3 whole line, -1 automatic mix, -2 bell curve of stretch lengths, -3 S-curve chance of a big step while stuck, -4 S-curve growth of the stretch length while stuck, -5 concave growth (fast first, then slower) while stuck, -6 like -3 with the midpoint learned per walker (3x its median steps between improvements), -7 midpoint max(500, 8x median gap), -8 midpoint chosen by a payoff bandit per level, -9 midpoint scaled by box count, -10 band of the line length (BANDLO..BANDHI)"},
    {"BANDLO", "0", "ARM=-10 only: shortest step as a fraction of the line length"},
    {"BANDHI", "0.1", "ARM=-10 only: longest step as a fraction of the line length"},
    {"WL", "", "window line file (from plateau_calibrate): every step draws its window length from that curve (sets ARM=-11)"},
    {"WMODE", "equal", "window shape: equal (odds ~ 1 / measured hit rate; the default is equal + adaptive + WINV=1, odds ~ the hit rate), scurve (the version 2/3 default), range (uniform WLO..WHI), curve (bell around WCENTER)"},
    {"WLO", "20", "range: shortest window; scurve: shortest normal window (pushes)"},
    {"WHI", "60", "range: longest window; scurve: longest normal window, big windows start here (pushes)"},
    {"WCENTER", "(WLO+WHI)/2", "curve: centre of the bell (pushes)"},
    {"WSIGMA", "15", "curve: width of the bell (standard deviation, pushes)"},
    {"WADAPT", "1 (equal) / 0", "1 = adaptive: range/scurve move their centre towards the window sizes that improve the line; curve re-measures the hit rate on the best line every RECAL s and uses the fitted curve"},
    {"WINV", "1 (default) / 0", "1 = inverse (experimental): odds p'(w) = max(p) - p(w) over window sizes 1..min(line, WCAP); 2 = equal contribution: p'(w) ~ 1 / max(p(w), 5% of max p)"},
    {"WRATE", "0.05", "adaptive range/scurve: weight of each improving step in the moving average of ln(window size)"},
    {"WLSAVE", "0", "1 = write each measured window line to window_line_last.txt (the GUI preview reads it)"},
    {"WCAP", "300", "inverse: longest window considered (pushes)"},
    {"RECAL", "10 (equal) / 150", "ARM=-12 and adaptive curve: seconds between re-measurements of the window line on the current best line (0 = after every new best line)"},
    {"RECALMIN", "3", "RECAL=0 (re-measure after every new best line): fewest seconds between re-measurements"},
    {"RECALMAX", "30", "(unused since re-measuring only follows a new best line)"},
    {"RECALBUD", "3 (equal) / 8", "ARM=-12 and adaptive curve: seconds one walker spends on a recalibration (at most RECAL/2)"},
    {"RECALN", "200", "ARM=-12: most attempts per 10-push band in a recalibration"},
    {"CALIB", "0", "1 = calibrate the window line instead of optimizing (plateau_calibrate.exe does this)"},
    {"CALN", "1000", "calibration: attempts per 10-push band"},
    {"CALOUT", "window_line.txt", "calibration: output file"},
    {"CALLINE", "0", "calibration: 0 = the worst given line (the start line), 1 = the best"},
    {"CALHIT", "0", "calibration hits: 0 = strictly better, or slack hits if fewer than 20; 1 = strict only; 2 = slack only"},
    {"CALDEG", "3", "calibration: degree of the fitted polynomial in ln(window length)"},
    {"LOGT", "30", "seconds between progress log lines"},
    {"PROF", "0", "1 = also log the best moves and best pushes line at every progress line"},
    {"BELLC", "40", "ARM=-2 only: median stretch length (pushes); boxes ~ 2 + L/15, node budget ~ 500 per push (also for ARM=-3/-4 sized steps)"},
    {"BELLS", "0.6", "ARM=-2 only: spread of the stretch length (sigma of its logarithm)"},
    {"LOGS0", "1500", "ARM=-3/-4: steps since a walker last improved at the middle of the S-curve"},
    {"LOGW", "400", "ARM=-3/-4: steepness of the S-curve (steps; smaller = sharper)"},
    {"LOGCAP", "0", "ARM=-3/-6: longest big step in pushes (0 = whole line; 150 = long)"},
    {"LOGTAU", "1500", "ARM=-5 only: steps since the last improvement for the length to cover 63% of the way to the whole line"},
    {"EPS", "0.25", "ARM=-1 only: share of random arm choices"},
    {"ESC", "1", "1 = a stuck walker escalates to longer steps (after ESC1, ESC2 steps)"},
    {"ESC1", "3000", "steps without change before the next longer arm"},
    {"ESC2", "12000", "steps without change before the arm after that"},
    {"ML1", "20", "medium arm: shortest stretch (pushes)"},
    {"ML2", "60", "medium arm: longest stretch (pushes)"},
    {"MK1", "2", "medium arm: fewest boxes re-planned"},
    {"MK2", "6", "medium arm: most boxes re-planned (at most 8)"},
    {"SML1", "0", "S-curve modes (ARM=-3, -6..-9): shortest medium step in pushes (0 = ML1); big steps start at the longest"},
    {"SML2", "0", "S-curve modes: longest medium step in pushes"},
    {"MB", "20000", "medium arm: node budget of one step"},
    {"HBLAY", "50", "hook and ball: nodes kept per layer"},
    {"HBBR", "10", "hook and ball: children kept per node"},
    {"HBWH", "2", "hook and ball: weight of the remaining-pushes estimate when sorting a layer"},
    {"ALT", "1", "1 = switch metrics in cycles (detours on the other metric), 0 = target metric only"},
    {"CYCLE", "TIME/8", "length of one metric cycle in seconds (default TIME/8, between 60 and 900)"},
    {"ALTF", "0.6", "share of each cycle spent on the target metric"},
    {"PFSEED", "0", "1 = the moves/pushes trade-off lines also seed the metric detours"},
    {"XO", "1", "1 = crossover of the pool and the bank of recent walker lines"},
    {"BANK", "256", "crossover: number of recent walker lines kept"},
    {"XOT", "1", "crossover: seconds between crossovers"},
    {"RP", "1", "1 = exact replan slots (whole-line re-planning of 1-3 boxes)"},
    {"RPT", "0.25", "exact replan: seconds between slots"},
    {"RPK", "3", "exact replan: most boxes per set (1-3)"},
    {"RPMAX", "3", "exact replan: seconds per slot at most"},
    {"RPB", "200000", "exact replan: node budget per box set (a quarter of it for 3 boxes)"},
};
static void printHelp() {
    printf("Plateau - Sokoban solution optimizer\n\n"
           "usage: plateau LEVELFILE [OPTION=value ...]\n\n"
           "LEVELFILE holds the level (rows of # @ + $ * . and spaces, '-' or '_' for floor; run-length rows and '|' row\n"
           "separators are fine) followed by solution lines (lurd, run-length allowed). Plateau keeps improving the best\n"
           "line and appends every new best line and every new moves/pushes trade-off line to OUT, each checked by replay.\n\n"
           "options (default in brackets):\n");
    for (auto& o : OPTS) printf("  %-8s [%s]  %s\n", o.key, o.def, o.help);
}

int main(int argc, char** argv) {
    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "/?") || !strcmp(argv[1], "help"))) { printHelp(); return 0; }
    if (argc < 2) { printHelp(); return 1; }
    map<string, string> opt; for (int i = 2; i < argc; i++) { string a = argv[i]; size_t e = a.find('=');
        if (e == string::npos) { fprintf(stderr, "ignoring '%s' (options are KEY=value; plateau --help lists them)\n", a.c_str()); continue; }
        string k = a.substr(0, e); bool known = false; for (auto& o : OPTS) if (k == o.key) known = true;
        if (!known) fprintf(stderr, "ignoring unknown option %s (plateau --help lists the options)\n", k.c_str());
        opt[k] = a.substr(e + 1); }
    { string me = argv[0]; for (auto& c : me) c = tolower(c); if (me.find("calibrate") != string::npos && !opt.count("CALIB")) opt["CALIB"] = "1"; }   // plateau_calibrate.exe
    auto gi = [&](const char* k, i64 d) { return opt.count(k) ? atoll(opt[k].c_str()) : d; };
    auto gd = [&](const char* k, double d) { return opt.count(k) ? atof(opt[k].c_str()) : d; };
    METRIC = gi("P", 0); double TIME = gi("TIME", 600); THREADS = gi("THREADS", 4); POOLMAX = gi("POOL", 8);
    if (opt.count("OUT")) OUTF = opt["OUT"]; if (opt.count("LOG")) LOGFP = fopen(opt["LOG"].c_str(), "a");
    vector<string> sols; if (!loadLevel(argv[1], sols)) return 1;
    if (opt.count("SOLS")) { ifstream f(opt["SOLS"]); string l; while (getline(f, l)) { l = trimR(l); if (isSolLine(l)) { string s; for (char c : expandRLE(l)) if (strchr("lurdLURD", c)) s += c; sols.push_back(s); } } }
    T0 = nowSec();
    LOG("level %dx%d, %d floor squares, %d boxes, %zu solution lines\n", GW, GH, N, NBOX, sols.size());
    vector<Line> all; readBest(sols, all);
    if (all.empty()) { LOG("no valid solution found in the input\n"); return 1; }
    for (auto& L : all) { if ((int)pool.size() >= POOLMAX && lineCost(L) > pool.back().c) break; seenLines.insert(L.lurd); Elite e; e.L = L; e.c = lineCost(L); pool.push_back(e); if ((int)pool.size() >= POOLMAX) break; }
    sort(pool.begin(), pool.end(), [](const Elite& a, const Elite& b) { return a.c < b.c; });
    for (auto& L : all) frontAdd(L);
    LOG("start: %lld/%lld  (%zu valid lines, pool %zu)\n", (long long)pool[0].L.moves, (long long)pool[0].L.pushes, all.size(), pool.size());
    if (!gi("CALIB", 0)) { // the start line, re-rendered with shortest walks
        FILE* f = fopen(OUTF.c_str(), "a"); if (f) { fprintf(f, "Solution: %lld moves, %lld pushes (Plateau start, shortest walks, verified)\n%s\n\n", (long long)pool[0].L.moves, (long long)pool[0].L.pushes, pool[0].L.lurd.c_str()); fclose(f); } }
    replanners.resize(THREADS); for (auto& r : replanners) r.init();
    hbs.resize(THREADS); for (auto& h : hbs) h.init();
    FW_SLACK = gi("SLACK", METRIC ? 4 : 10); FW_SLM = gi("SLACKM", -1); FW_SLP = gi("SLACKP", -1);
    FW_RESTART = gi("RESTART", 6000); FW_KICK = gi("KICK", 50); FW_SEED = gi("SEED", 1);
    FW_ARM = gi("ARM", -3); FW_EPS = gd("EPS", FW_EPS); FW_ESC = gi("ESC", 1); FW_ESC1 = gi("ESC1", 3000); FW_ESC2 = gi("ESC2", 12000); FW_BELLC = gd("BELLC", FW_BELLC); FW_BELLS = gd("BELLS", FW_BELLS); FW_LOGS0 = gd("LOGS0", FW_LOGS0); FW_LOGW = gd("LOGW", FW_LOGW); FW_LOGTAU = gd("LOGTAU", FW_LOGTAU); FW_LOGCAP = gi("LOGCAP", 0); FW_BANDLO = gd("BANDLO", FW_BANDLO); FW_BANDHI = gd("BANDHI", FW_BANDHI); FW_LOGT = gd("LOGT", FW_LOGT); FW_PROF = gi("PROF", 0);
    { FArm& M = FARMS[1]; M.lmin = gi("ML1", M.lmin); M.lmax = gi("ML2", M.lmax); M.k1 = gi("MK1", M.k1); M.k2 = gi("MK2", M.k2); M.budget = gi("MB", M.budget);
      if (FW_ARM <= -3 && FW_ARM != -4 && FW_ARM != -5 && gi("SML1", 0) > 0) { M.lmin = (int)gi("SML1", 0); M.lmax = max<int>(M.lmin, (int)gi("SML2", 0)); } }
    HB_LAYER = gi("HBLAY", 50); HB_BRANCH = gi("HBBR", 10); HB_WH = gi("HBWH", 2);
    FW_PFSEED = gi("PFSEED", 0); FW_ALTF = gd("ALTF", FW_ALTF);
    FW_XO = gi("XO", 1); FW_BANK = gi("BANK", 256); FW_XOT = gd("XOT", FW_XOT);
    FW_RP = gi("RP", 1); FW_RPK = gi("RPK", 3); FW_RPT = gd("RPT", FW_RPT); FW_RPMAX = gd("RPMAX", FW_RPMAX); FW_RPB = gi("RPB", 200000);
    if (gi("CALIB", 0)) { CAL_N = gi("CALN", 1000); CAL_LINE = gi("CALLINE", 0); CAL_DEG = gi("CALDEG", 3); CAL_HIT = gi("CALHIT", 0); if (opt.count("CALOUT")) CAL_OUT = opt["CALOUT"];
        return runCalibration(CAL_LINE ? all.front() : all.back()); }
    { bool any = false; for (const char* k : {"WMODE", "WLO", "WHI", "WCENTER", "WSIGMA", "WADAPT", "WINV", "WCAP"}) if (opt.count(k)) any = true;
      bool dflt = !any && !opt.count("ARM") && !opt.count("WL") && !gi("CALIB", 0);   // the default is Equal + Adaptive
      if (any || dflt) { string m = opt.count("WMODE") ? opt["WMODE"] : dflt ? "equal" : "scurve"; for (auto& ch : m) ch = tolower(ch);
        FW_WM = (m == "range" || m == "1") ? 1 : (m == "curve" || m == "bell" || m == "3") ? 3 : (m == "equal" || m == "4") ? 4 : 2;
        FW_WLO = max<int>(1, (int)gi("WLO", 20)); FW_WHI = max<int>(FW_WLO, (int)gi("WHI", 60)); FW_WCENTER = gd("WCENTER", 0.5 * (FW_WLO + FW_WHI)); FW_WSIGMA = max(1.0, gd("WSIGMA", 15));
        FW_WADAPT = gi("WADAPT", FW_WM == 4 ? 1 : 0) != 0; FW_WLSAVE = (int)gi("WLSAVE", 0); FW_WINV = (int)gi("WINV", dflt ? 1 : 0); if (FW_WINV < 0 || FW_WINV > 2) FW_WINV = 1; FW_WCAP = max<int>(2, (int)gi("WCAP", 300)); FW_WRATE = max(0.001, min(1.0, gd("WRATE", 0.05))); FW_ADC = log(0.5 * (FW_WLO + FW_WHI));
        FArm& M = FARMS[1];
        if (FW_WM == 1 && !FW_WADAPT && !FW_WINV) { FW_ARM = 1; M.lmin = FW_WLO; M.lmax = FW_WHI; FW_ESC = 0; }   // plain range = the medium step on WLO..WHI
        else if (FW_WM == 2 && !FW_WADAPT && !FW_WINV) { FW_ARM = -3; M.lmin = FW_WLO; M.lmax = FW_WHI; }        // plain S-curve (WLO/WHI 20/60 = the default exactly)
        else FW_ARM = -20; } }
    bool eqDefaultTiming = FW_ARM == -20 && FW_WM == 4; FW_RECAL = gd("RECAL", eqDefaultTiming ? 10.0 : FW_RECAL); FW_RECALMIN = max(0.5, gd("RECALMIN", 3.0)); FW_RECALMAX = max(FW_RECALMIN, gd("RECALMAX", 30.0));
    if (FW_ARM == -20 && FW_WM == 4 && !FW_WADAPT) FW_RECAL = 1e12;   // Equal, not adaptive: measure once at the start
    if (FW_RECAL > 0) { FW_RECAL = max(1.0, FW_RECAL); FW_RECALBUD = min(gd("RECALBUD", eqDefaultTiming ? 3.0 : 8.0), 0.5 * FW_RECAL); } else { FW_RECAL = 0; FW_RECALBUD = min(gd("RECALBUD", 2.5), FW_RECALMIN); }   /* measuring takes at most half the interval */ FW_RECALN = gi("RECALN", FW_RECALN);
    if (opt.count("WL")) { if (!loadWindowLine(opt["WL"])) { LOG("cannot read window line %s\n", opt["WL"].c_str()); return 1; } FW_ARM = WL_NOSIGNAL ? FW_ARM : -11; if (WL_NOSIGNAL) LOG("window line %s has no usable signal (calibration found almost no improvements); using the default step sizes\n", opt["WL"].c_str()); }
    if (FW_ARM == -11 && (WL_MAXL == 0 || WL_NOSIGNAL)) { LOG("ARM=-11 needs WL=<window line file>\n"); return 1; }
    if (FW_ARM == -11) LOG("fast walk: %.0fs on %d threads; window lengths drawn from the calibrated window line %s (lengths up to %d), keeping lines up to %d %s worse than the best\n", TIME, THREADS, opt["WL"].c_str(), WL_MAXL, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM >= 0) LOG("fast walk: %.0fs on %d threads; stretches of %d..%d pushes, %d..%d boxes re-planned, up to %lld nodes per step, keeping lines up to %d %s worse than the best\n",
        TIME, THREADS, FARMS[FW_ARM].lmin, FARMS[FW_ARM].lmax, FARMS[FW_ARM].k1, FARMS[FW_ARM].k2, (long long)FARMS[FW_ARM].budget, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -2) LOG("fast walk: %.0fs on %d threads; bell curve of stretch lengths (median %.0f pushes, sigma %.2f), keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_BELLC, FW_BELLS, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -5) LOG("fast walk: %.0fs on %d threads; stretch length grows from 20 pushes towards the whole line, fast at first (time constant %.0f steps without improvement), keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_LOGTAU, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -4) LOG("fast walk: %.0fs on %d threads; stretch length grows on an S-curve from 20 pushes to the whole line (middle at %.0f steps without improvement, width %.0f), keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_LOGS0, FW_LOGW, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -10) LOG("fast walk: %.0fs on %d threads; steps of %.0f%%..%.0f%% of the line\n", TIME, THREADS, FW_BANDLO * 100, FW_BANDHI * 100);
    else if (FW_ARM == -20) LOG("fast walk: %.0fs on %d threads; window shape %s%s%s (%d..%d pushes%s), keeping lines up to %d %s worse than the best\n", TIME, THREADS,
        FW_WM == 1 ? "range" : FW_WM == 2 ? "S-curve" : FW_WM == 4 ? "equal" : "curve", FW_WADAPT ? ", adaptive" : "", FW_WINV == 2 ? ", equal-contribution inverse" : FW_WINV ? ", inverse" : "", FW_WLO, FW_WHI,
        FW_WM == 3 ? (", centre " + to_string((int)llround(FW_WCENTER)) + " width " + to_string((int)llround(FW_WSIGMA))).c_str() : "", FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -12) LOG("fast walk: %.0fs on %d threads; window line recalibrated every %.0f s on the best line (S-curve while there is no signal), keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_RECAL, FW_SLACK, METRIC ? "pushes" : "moves");
    else if (FW_ARM == -3 || FW_ARM <= -6) LOG("fast walk: %.0fs on %d threads; medium steps, big steps with an S-curve chance (middle at %.0f steps without improvement, width %.0f), keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_LOGS0, FW_LOGW, FW_SLACK, METRIC ? "pushes" : "moves");
    else LOG("fast walk: %.0fs on %d threads; automatic mix of step sizes, keeping lines up to %d %s worse than the best\n", TIME, THREADS, FW_SLACK, METRIC ? "pushes" : "moves");
    if (gi("ALT", 1)) fastWalkAlternating(T0 + TIME, gi("CYCLE", 0)); else fastWalk(T0 + TIME, {}, nullptr);
    if (FW_RP) LOG("exact replan slots: %lld runs, %lld improvements\n", (long long)RP_RUNS.load(), (long long)RP_WINS.load());
    if (FW_XO) LOG("crossover: %lld runs, %lld new bests\n", (long long)XO_RUNS.load(), (long long)XO_WINS.load());
    LOG("done: best %lld/%lld after %lld steps (%lld lines kept)\n", (long long)pool[0].L.moves, (long long)pool[0].L.pushes, (long long)FW_STEPS.load(), (long long)FW_ACC.load());
    return 0;
}
