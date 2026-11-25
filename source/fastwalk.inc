// ---------------------------------------------------------------- plateau walk
// The optimizer is one loop per thread ("walker"), built for speed: thousands of cheap, good-enough steps.
// A step:
//   1. pick a stretch [a, b] of the walker's line (push b stays) and k boxes pushed inside it;
//   2. delete those boxes' pushes inside the stretch; the other pushes keep their order (the skeleton);
//   3. hook and ball (hookball.inc) re-inserts the k boxes' pushes around the skeleton, from the board before push a
//      to the same board after push b. In KICK% of the steps one square those boxes used is banned, so the result is a
//      different line, not the same one;
//   4. splice the new stretch in. Keep it if the whole line is at most SLACK worse than this walker's best.
// Between steps the walkers take turns at two exact moves: crossover of many lines (crossover.inc) and exact
// re-planning of 1-3 boxes' whole histories (replan.inc). Every new best line is re-rendered with shortest walks,
// checked by exact replay and appended to OUT.
#include "hookball.inc"

static int FW_SLM = -1, FW_SLP = -1;   // SLACKM / SLACKP: slack on moves walks / pushes walks (-1 = SLACK)
static int FW_PFSEED = 0; static double FW_ALTF = 0.6;
static int FW_RP = 1, FW_RPK = 3; static double FW_RPT = 0.25, FW_RPMAX = 3; static i64 FW_RPB = 200000; static atomic<i64> RP_RUNS{0}, RP_WINS{0};
static const int FW_KMAX = 8;          // most boxes re-planned in one step (hook and ball keys hold up to 8)

// per-push cost of a line (walk + 1 for each push), so a stretch's old cost is a prefix-sum difference
static void pushCosts(const Pushes& P, vector<u64>& pre, vector<vector<u64>>& lay, vector<int>& pl) {
    static thread_local Walker W; if ((int)W.dist.size() != N) W.init();
    int n = P.box.size(); pre.assign(n + 1, 0); lay.resize(n + 1); pl.resize(n + 1);
    vector<u64> c = BOX0; int p = PLAYER0;
    for (int i = 0; i < n; i++) { lay[i] = c; pl[i] = p; int b = P.box[i], d = P.dir[i]; W.bfs(c.data(), p); int q = nb[b][OPP[d]];
        pre[i + 1] = pre[i] + cost(W.reach(q) ? W.dist[q] + 1 : 1000000, 1); cb(c.data(), b); sb(c.data(), nb[b][d]); p = b; }
    lay[n] = c; pl[n] = p;
}

static int FW_ESC = 1, FW_ESC1 = 3000, FW_ESC2 = 12000; static int FW_SLACK = 4, FW_RESTART = 6000; static int FW_KICK = 50;
static atomic<i64> FW_STEPS{0}, FW_ACC{0};
// step sizes ("arms"): stretch length, boxes re-planned, node budget. ARM=1 (medium) is the default; a walker that has
// not improved for ESC1 / ESC2 steps on its line escalates to the long / whole-line arm. ARM=-1 measures, for each arm,
// how much the walkers' best lines improved per second spent on it and gives most of the time to the arms that pay.
struct FArm { int lmin, lmax, k1, k2; i64 budget; const char* name; };
static vector<FArm> FARMS = { {6, 20, 2, 4, 5000, "short"}, {20, 60, 2, 6, 20000, "medium"}, {60, 150, 4, 8, 80000, "long"}, {100000, 100000, 5, 8, 300000, "whole line"} };
static double FW_BELLC = 40, FW_BELLS = 0.6, FW_LOGS0 = 1500, FW_LOGW = 400, FW_LOGTAU = 1500; static int FW_LOGCAP = 0; static double FW_BANDLO = 0, FW_BANDHI = 0.1, FW_LOGT = 30; static int FW_PROF = 0;   // ARM=-10 band (fractions of the line), log interval, PROF=1 logs best moves/pushes lines
// ARM=-8: per-level bandit over the S-curve midpoint (shared by all walkers): 500, 1500, 4000 steps, or never big
static const double B8_MID[4] = {500, 1500, 4000, -1}; static double B8_TIME[4], B8_GAIN[4]; static i64 B8_SEGS[4];   // LOGCAP: ARM=-3/-6 big steps at most this long (0 = whole line)   // ARM=-3/-4: S-curve of steps since a walker last improved   // ARM=-2: bell curve (log-normal) of stretch lengths, median BELLC pushes, sigma BELLS in log space
static double FA_TIME[8], FA_GAIN[8]; static i64 FA_STEPS[8]; static int FW_ARM = 1; static unsigned FW_SEED = 1; static double FW_EPS = 0.25;
static int pickArm(mt19937& rng) {
    int na = FARMS.size(); if (FW_ARM >= 0) return FW_ARM;
    if ((rng() % 1000) < FW_EPS * 1000) return rng() % na;
    int best = 0; double bs = -1;
    for (int a = 0; a < na; a++) { double sc;
        #pragma omp atomic read
        sc = FA_GAIN[a];
        double t;
        #pragma omp atomic read
        t = FA_TIME[a];
        sc = (sc + 1.0) / (t + 30.0); if (sc > bs) { bs = sc; best = a; } }
    return best; }

static vector<HookBall> hbs;
static int drawWindowLine(int maxLen, mt19937& rng);   // calib.inc
static double FW_RECAL_EXT(); static bool dynRecalibrate(const Line& L, mt19937& rng, HookBall& hb, vector<int>& sqId, vector<int>& sqStamp, int& sqS, vector<double>& cum, double tRun);
// ARM=-20 (set by WMODE etc.): window shape. FW_WM 1 = range, 2 = S-curve, 3 = curve; adaptive / inverse switches.
static double FW_WRATE = 0.05; static int FW_WM = 0, FW_WADAPT = 0, FW_WINV = 0, FW_WLO = 20, FW_WHI = 60, FW_WCAP = 300; static double FW_WCENTER = 40, FW_WSIGMA = 15, FW_ADC = 0;
// probability of each window length 1..M (p[0] unused) for the current shape; sinceBest drives the S-curve
static void windowProbs(int whole, i64 sinceBest, const vector<double>* fitted, vector<double>& pr) {
    int M = FW_WINV ? max(1, min(whole, FW_WCAP)) : max(1, whole); pr.assign(M + 1, 0.0);
    double lo = FW_WLO, hi = FW_WHI, c = FW_WCENTER;
    if (FW_WADAPT && FW_WM != 3) { double m = 0.5 * (FW_WLO + FW_WHI), cc;
        #pragma omp atomic read
        cc = FW_ADC;
        double s = exp(cc) / m; lo = FW_WLO * s; hi = FW_WHI * s; }
    lo = max(1.0, min(lo, (double)M)); hi = max(lo, min(hi, (double)M));
    int ilo = (int)llround(lo), ihi = max(ilo, (int)llround(hi));
    if (FW_WM == 4) {   // Equal: 1 / hit rate (inverse: the hit rate); uniform over WLO..WHI until the first measurement
        bool fit = fitted && fitted->size() > 1;
        if (!fit) { for (int w = ilo; w <= ihi; w++) pr[w] = 1; return; }
        int top = min<int>(M, (int)fitted->size() - 1); pr.resize(top + 1);
        for (int w = 1; w <= top; w++) { double q = (*fitted)[w] - (*fitted)[w - 1]; pr[w] = FW_WINV ? q : 1.0 / max(q, 1e-12); }
        return; }
    if (FW_WM == 1) { for (int w = ilo; w <= ihi; w++) pr[w] = 1; }
    else if (FW_WM == 2) { double s = 1.0 / (1.0 + exp(-((double)sinceBest - FW_LOGS0) / FW_LOGW));
        for (int w = ilo; w <= ihi; w++) pr[w] += (1 - s) / (ihi - ilo + 1);
        int bot = max(ihi, 60), top = max(bot, whole); double z = 0; for (int w = bot; w <= top; w++) z += 1.0 / w;
        if (z > 0) for (int w = bot; w <= min(top, M); w++) pr[w] += s * (1.0 / w) / z; }
    else { bool fit = fitted && fitted->size() > 1;
        for (int w = 1; w <= M; w++) pr[w] = fit ? ((*fitted)[min<int>(w, (int)fitted->size() - 1)] - (*fitted)[min<int>(w - 1, (int)fitted->size() - 1)])
                                              : exp(-(w - c) * (w - c) / (2 * FW_WSIGMA * FW_WSIGMA)); }
    if (FW_WINV == 2) { double mx = 0; for (int w = 1; w <= M; w++) mx = max(mx, pr[w]); if (mx <= 0) mx = 1;
        for (int w = 1; w <= M; w++) pr[w] = 1.0 / max(pr[w], 0.05 * mx); }
    else if (FW_WINV) { double mx = 0; for (int w = 1; w <= M; w++) mx = max(mx, pr[w]); bool any = false;
        for (int w = 1; w <= M; w++) { pr[w] = mx - pr[w]; if (pr[w] > 1e-15) any = true; }
        if (!any) for (int w = 1; w <= M; w++) pr[w] = 1; }
}
static int drawWindow(int whole, i64 sinceBest, const vector<double>* fitted, mt19937& rng, vector<double>& pr) {
    windowProbs(whole, sinceBest, fitted, pr); double t = 0; for (size_t w = 1; w < pr.size(); w++) t += pr[w];
    if (t <= 0) return max(1, min(whole, FW_WLO));
    double u = ((double)rng() + 0.5) / 4294967296.0 * t;
    for (size_t w = 1; w < pr.size(); w++) { u -= pr[w]; if (u <= 0) return (int)w; }
    return (int)pr.size() - 1;
}
static int FW_WLSAVE = 0; static double FW_RECALMIN = 3, FW_RECALMAX = 30; static u64 DYN_LASTC = ~0ULL;   // RECAL=0: re-measure on each new best (at least RECALMIN s apart, at least every RECALMAX s)
static vector<double> DYN_CUM; static int DYN_VER = 0; static bool DYN_OK = false; static double DYN_T = -1e18;   // ARM=-12 shared curve
// seeds: start lines (empty = the elite pool); finals (optional): each walker's best line at the end
static void fastWalk(double tend, const vector<Line>& seeds, vector<Line>* finals) {
    if (finals) finals->assign(THREADS, Line());
    #pragma omp parallel num_threads(THREADS)
    {
        int tid = 0;
#ifdef _OPENMP
        tid = omp_get_thread_num();
#endif
        mt19937 rng(FW_SEED * 1000003u + 7919 * tid);
        Line L; {
            #pragma omp critical
            { L = seeds.empty() ? pool[tid % pool.size()].L : seeds[tid % seeds.size()]; } }
        Line bestL = L;
        u64 Lc = lineCost(L), bestC = Lc; i64 sinceBest = 0;
        vector<u64> pre; vector<vector<u64>> lay; vector<int> pl; pushCosts(L.P, pre, lay, pl);
        vector<int> wBox, wDir, oBox, oDir, ids, sqId(N, -1), sqStamp(N, 0); vector<char> inR; int sqS = 0;
        vector<double> dynCum, wpr; int dynVer = -1; bool dynOK = false;   // ARM=-12: this walker's copy of the recalibrated curve
        vector<i64> impGaps; i64 impSteps = 0; u64 impC = ~0ULL;   // ARM=-6/-7: this walker's recent steps between improvements
        int c8 = -1; i64 seg8 = 0; double t8 = 0, g8 = 0; u64 c8C = ~0ULL;   // ARM=-8: current midpoint choice, its segment's steps, start time, gain, last best seen
        i64 localSteps = 0; double lastLog = nowSec();
        // memory of steps already tried on the current line (cleared whenever the line changes)
        unordered_set<u64> tried; i64 stale = 0;
        auto lineChanged = [&]() { tried.clear(); stale = 0; };
        while (true) {
            if ((localSteps & 7) == 0) { double now = nowSec(); if (now >= tend) break;
                if (FW_XO) { bool mine = false; vector<Pushes> lines; u64 beat = 0;   // crossover slot: one walker every XOT seconds
                    #pragma omp critical
                    { static double xoLast = 0;
                      if (now - xoLast >= FW_XOT && XBANK.size() >= 2) { xoLast = now; mine = true;
                        if (pool.empty()) mine = false; else { beat = pool[0].c; for (auto& e : pool) lines.push_back(e.L.P); }
                        for (auto& p : XBANK) lines.push_back(p); } }
                    if (mine) { Pushes xp; u64 xc; XO_RUNS++;
                        if (crossover(lines, beat, xp, xc)) { Line nl; if (render(xp, nl) && lineCost(nl) < beat) { XO_WINS++;
                            #pragma omp critical
                            { addElite(nl, "crossover"); }
                            L = nl; Lc = lineCost(L); bestC = Lc; bestL = L; sinceBest = 0; pushCosts(L.P, pre, lay, pl); lineChanged(); } } } }
                if (FW_ARM == -12 || (FW_ARM == -20 && ((FW_WM == 3 && FW_WADAPT) || FW_WM == 4))) { bool mine = false; Line snap;   // recalibration slot: one walker every RECAL seconds
                    #pragma omp critical
                    { if (!pool.empty() && (FW_RECAL_EXT() > 0 ? now - DYN_T >= FW_RECAL_EXT() : now - DYN_T >= FW_RECALMIN)) {
                        if (pool[0].c != DYN_LASTC) { DYN_T = now; DYN_LASTC = pool[0].c; mine = true; snap = pool[0].L; }
                        else if (FW_RECAL_EXT() > 0) { DYN_T = now; LOG("[%7.1fs] window line re-measure skipped: best line %lld/%lld unchanged since the last measurement\n", now - T0, (long long)pool[0].L.moves, (long long)pool[0].L.pushes); } } }
                    if (mine) { vector<double> cum; bool ok = dynRecalibrate(snap, rng, hbs[tid], sqId, sqStamp, sqS, cum, now - T0);
                        #pragma omp critical
                        { DYN_CUM = cum; DYN_OK = ok; DYN_VER++; }
                        if (ok && FW_WLSAVE) { FILE* wf = fopen("window_line_last.txt", "w"); if (wf) {
                            fprintf(wf, "# window line measured at %.0f s on %lld/%lld (hit rate per window length, smooth fit)\ncurve (L weight)\n", now - T0, (long long)snap.moves, (long long)snap.pushes);
                            for (size_t l = 1; l < cum.size(); l++) fprintf(wf, "%zu %.6g\n", l, cum[l] - cum[l - 1]); fclose(wf); } } } }
                if (FW_RP) { bool mine = false;   // exact replan slot: one walker every RPT seconds
                    #pragma omp critical
                    { static double rpLast = 0; if (now - rpLast >= FW_RPT) { rpLast = now; mine = true; } }
                    if (mine) { // exact whole-line re-planning of 1, 2 and 3 boxes' histories on this walker's line
                        double t0r = nowSec(); RP_RUNS++; bool imp = false;
                        for (int kk = 1; kk <= FW_RPK && !imp && nowSec() - t0r < FW_RPMAX; kk++) {
                            vector<int> ss; vector<int> id = boxIds(L.P, ss); int nbx = ss.size(); vector<vector<int>> sets;
                            if (kk == 1) for (int x = 0; x < nbx; x++) sets.push_back({x}); else if (kk == 2) { for (int x = 0; x < nbx; x++) for (int y = x + 1; y < nbx; y++) sets.push_back({x, y}); }
                            else for (int t = 0; t < 4 * nbx; t++) { int x = rng() % nbx, y = rng() % nbx, z = rng() % nbx; if (x == y || y == z || x == z) continue; if (WD[(size_t)ss[x] * N + ss[y]] > 6 || WD[(size_t)ss[x] * N + ss[z]] > 6) continue; sets.push_back({x, y, z}); }
                            shuffle(sets.begin(), sets.end(), rng);
                            for (auto& R : sets) { if (nowSec() - t0r >= FW_RPMAX) break; Pushes np; u64 c; i64 nd; bool ex;
                                if (replanners[tid].run(L.P, id, ss, R, Lc, kk == 3 ? FW_RPB / 4 : FW_RPB, np, c, nd, ex)) { Line nl; if (render(np, nl) && lineCost(nl) < Lc) {
                                    imp = true; RP_WINS++; L = nl; Lc = lineCost(L); pushCosts(L.P, pre, lay, pl); lineChanged(); FW_ACC++;
                                    if (Lc < bestC) { bestC = Lc; bestL = L; sinceBest = 0; }
                                    #pragma omp critical
                                    { if (FW_XO) bankAdd(L.P); addElite(L, "exact replan"); }
                                    break; } } } } } }
                if (tid == 0 && now - lastLog >= FW_LOGT) { lastLog = now;
                    #pragma omp critical
                    { LOG("[%7.1fs] fast walk: %lld steps (%.0f/s), %lld new lines kept, best %lld/%lld |", now - T0, (long long)FW_STEPS.load(), FW_STEPS.load() / max(1e-9, now - T0), (long long)FW_ACC.load(), (long long)pool[0].L.moves, (long long)pool[0].L.pushes);
                      for (int q = 0; q < (int)FARMS.size(); q++) LOG(" %s: %lld steps %.0fs gain %.2f |", FARMS[q].name, (long long)FA_STEPS[q], FA_TIME[q], FA_GAIN[q]); if (FW_ARM == -20 && FW_WADAPT && (FW_WM == 1 || FW_WM == 2)) LOG(" window centre %.0f pushes |", exp(FW_ADC)); LOG("\n");
                      if (FW_PROF) { pair<i64,i64> bm{1LL << 60, 1LL << 60}, bp{1LL << 60, 1LL << 60};
                          for (auto& q : FRONT) { if (q < bm) bm = q; if (q.second < bp.second || (q.second == bp.second && q.first < bp.first)) bp = q; }
                          LOG("PROF %.1f moves %lld/%lld pushes %lld/%lld\n", now - T0, (long long)bm.first, (long long)bm.second, (long long)bp.first, (long long)bp.second); } } } }
            localSteps++; FW_STEPS++; sinceBest++;
            if (sinceBest > FW_RESTART) { sinceBest = 0;   // restart from the pool's best, a random pool line or a random seed line
                #pragma omp critical
                { int r3 = rng() % 3; L = (r3 == 0 || seeds.empty()) ? pool[0].L : (r3 == 1 ? pool[rng() % pool.size()].L : seeds[rng() % seeds.size()]); }
                Lc = lineCost(L); bestC = Lc; pushCosts(L.P, pre, lay, pl); lineChanged(); }
            int n = L.P.box.size(); if (n < 2) break;
            // stretch [a, b]: b is a skeleton push that stays
            stale++; int arm, bellLen = -1, bellK = 0; i64 bellBud = 0;
            if (FW_ARM == -12 || (FW_ARM == -20 && ((FW_WM == 3 && FW_WADAPT) || FW_WM == 4))) { int v;
                #pragma omp atomic read
                v = DYN_VER;
                if (v != dynVer) {
                    #pragma omp critical
                    { dynCum = DYN_CUM; dynOK = DYN_OK; dynVer = DYN_VER; } } }
            if (FW_ARM == -20) {   // window shape (WMODE)
                int Lb = drawWindow(max(1, n - 1), sinceBest, (((FW_WM == 3 && FW_WADAPT) || FW_WM == 4) && dynOK) ? &dynCum : nullptr, rng, wpr);
                bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; }
            else if (FW_ARM == -12 && dynOK && dynCum.size() > 1) {   // recalibrated window line
                int top = min<int>(max(1, n - 1), (int)dynCum.size() - 1); double u = ((double)rng() + 0.5) / 4294967296.0 * dynCum[top];
                int Lb = max(1, (int)(lower_bound(dynCum.begin() + 1, dynCum.begin() + top + 1, u) - dynCum.begin()));
                bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; }
            else if (FW_ARM == -11) {   // window line: length drawn from the level's calibrated curve (WL=file)
                int Lb = drawWindowLine(max(1, n - 1), rng);
                bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; }
            else if (FW_ARM == -10) {   // band: length uniform in [BANDLO, BANDHI] x line length (at least 4 pushes), k and budget scale with length
                double v = ((double)rng() + 0.5) / 4294967296.0; int whole = max(1, n - 1);
                double lo = max(4.0, FW_BANDLO * whole), hi = max(4.0, FW_BANDHI * whole); int Lb = (int)llround(lo + v * (hi - lo));
                Lb = max(Lb, 4); if (Lb > whole) Lb = whole;
                bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; }
            else if (FW_ARM == -3 || FW_ARM == -4 || FW_ARM == -5 || FW_ARM <= -6) {   // S-curve of the walker's steps since its last improvement: w = logistic((s - LOGS0) / LOGW)
                double s0 = FW_LOGS0, sw = FW_LOGW;
                if (FW_ARM == -6) {   // adaptive: midpoint = 3 x the median of the last 15 gaps between this walker's improvements, width scaled with it
                    impSteps++; if (bestC < impC) { if (impC != ~0ULL) { impGaps.push_back(impSteps); if (impGaps.size() > 15) impGaps.erase(impGaps.begin()); } impSteps = 0; } impC = bestC;
                    if (impGaps.size() >= 3) { vector<i64> g(impGaps); nth_element(g.begin(), g.begin() + g.size() / 2, g.end()); s0 = max(50.0, 3.0 * (double)g[g.size() / 2]); sw = s0 * FW_LOGW / FW_LOGS0; } }
                bool never = false;
                if (FW_ARM == -7) {   // gap-based: midpoint = max(500, 8 x median of the last 10 gaps between this walker's improvements), width = midpoint/4
                    impSteps++; if (bestC < impC) { if (impC != ~0ULL) { impGaps.push_back(impSteps); if (impGaps.size() > 10) impGaps.erase(impGaps.begin()); } impSteps = 0; } impC = bestC;
                    if (impGaps.size() >= 5) { vector<i64> g(impGaps); nth_element(g.begin(), g.begin() + g.size() / 2, g.end()); s0 = max(500.0, 8.0 * (double)g[g.size() / 2]); sw = s0 / 4; } }
                if (FW_ARM == -9) { s0 = max(500.0, min(6000.0, 1500.0 * sqrt(NBOX / 30.0))); sw = s0 / 4; }   // board-scaled
                if (FW_ARM == -8) {   // payoff bandit over the midpoint; a segment = 2000 steps, credited with the walker's improvement per second
                    if (c8C != ~0ULL && bestC < c8C) { double dp = (double)(c8C >> SH) - (double)(bestC >> SH), ds = (double)(c8C & ((1ULL << SH) - 1)) - (double)(bestC & ((1ULL << SH) - 1)); g8 += dp + 0.01 * ds; }
                    c8C = bestC;
                    if (c8 < 0 || ++seg8 >= 2000) { double now8 = nowSec();
                        if (c8 >= 0) {
                            #pragma omp atomic
                            B8_TIME[c8] += now8 - t8;
                            #pragma omp atomic
                            B8_GAIN[c8] += g8;
                            #pragma omp atomic
                            B8_SEGS[c8]++; }
                        if (rng() % 1000 < 200) c8 = rng() % 4;
                        else { double bs = -1; for (int q = 0; q < 4; q++) { double gq, tq;
                                #pragma omp atomic read
                                gq = B8_GAIN[q];
                                #pragma omp atomic read
                                tq = B8_TIME[q];
                                double sc = (gq + 1.0) / (tq + 30.0); if (sc > bs) { bs = sc; c8 = q; } } }
                        seg8 = 0; t8 = now8; g8 = 0; }
                    if (B8_MID[c8] < 0) never = true; else { s0 = B8_MID[c8]; sw = s0 / 4; } }
                double w = never ? 0.0 : 1.0 / (1.0 + exp(-((double)sinceBest - s0) / sw)); int whole = max(1, n - 1); int Lb;
                if (FW_ARM == -3 || FW_ARM <= -6) {   // probability: big step with chance w (length log-uniform from 60 pushes to the whole line), else a normal medium step
                    double u = ((double)rng() + 0.5) / 4294967296.0;
                    if (u < w) { double v = ((double)rng() + 0.5) / 4294967296.0; int top = FW_LOGCAP > 0 ? min(whole, FW_LOGCAP) : whole; double bot = FW_ARM == -9 ? max(6.0, 0.25 * whole) : max(60.0, (double)FARMS[1].lmax); Lb = top <= bot ? top : (int)llround(exp(log(bot) + v * (log((double)top) - log(bot)))); }
                    else if (FW_ARM == -9) { double v = ((double)rng() + 0.5) / 4294967296.0, lo = max(6.0, 0.08 * whole), hi = max(6.0, 0.25 * whole); Lb = (int)llround(lo + v * (hi - lo)); }   // board-scaled medium window
                    else Lb = -1; }
                else if (FW_ARM == -5) {   // concave growth: fastest right after an improvement, slowing towards the whole line, jitter +-10%
                    double v = ((double)rng() + 0.5) / 4294967296.0; Lb = (int)llround((20.0 + (whole - 20.0) * (1.0 - exp(-(double)sinceBest / FW_LOGTAU))) * (0.9 + 0.2 * v)); }
                else {   // growth: the length itself follows the S-curve from 20 pushes to the whole line, jitter +-10%
                    double v = ((double)rng() + 0.5) / 4294967296.0; Lb = (int)llround((20.0 + (whole - 20.0) * w) * (0.9 + 0.2 * v)); }
                if (Lb < 0) arm = 1;
                else { Lb = max(Lb, 6); if (Lb > whole) Lb = whole;
                    bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                    bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                    arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; } }
            else if (FW_ARM == -2) {   // bell curve: L log-normal around BELLC, k and node budget grow with L
                double u1 = ((double)rng() + 0.5) / 4294967296.0, u2 = ((double)rng() + 0.5) / 4294967296.0;
                double z = sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
                double lc = log(FW_BELLC);
                double Ld = exp(lc + FW_BELLS * z); int Lb = (int)llround(min(Ld, 1e9)); Lb = max(Lb, 6); if (Lb > n - 1) Lb = n - 1;
                bellLen = Lb; bellK = (int)llround(2.0 + Lb / 15.0); bellK = max(2, min(8, bellK));
                bellBud = max<i64>(5000, min<i64>(300000, (i64)500 * Lb));
                arm = Lb < 20 ? 0 : Lb < 60 ? 1 : Lb <= 150 ? 2 : 3; }   // bucket only for the statistics log
            else { arm = pickArm(rng); if (FW_ESC && FW_ARM >= 0 && stale > FW_ESC1) arm = min<int>(FARMS.size() - 1, FW_ARM + 1 + (stale > FW_ESC2 ? 1 : 0)); }
            const FArm& A = FARMS[arm]; double tStep = nowSec(); u64 bestBefore = bestC;
            struct ArmDone { int arm; double t0; u64* bestC; u64 before; ~ArmDone() { double dt = nowSec() - t0; double g = 0; if (*bestC < before) { double dp = (double)(before >> SH) - (double)(*bestC >> SH), ds = (double)(before & ((1ULL << SH) - 1)) - (double)(*bestC & ((1ULL << SH) - 1)); g = dp + 0.01 * ds; }
                #pragma omp atomic
                FA_TIME[arm] += dt;
                #pragma omp atomic
                FA_GAIN[arm] += g;
                #pragma omp atomic
                FA_STEPS[arm]++; } } armDone{arm, tStep, &bestC, bestBefore};
            int len = bellLen >= 0 ? bellLen : (A.lmin >= 100000 ? n - 1 : A.lmin + rng() % (A.lmax - A.lmin + 1)); len = min(len, n - 1);
            int a = rng() % (n - len);
            int b = a + len; // pushes a..b-1 may change, b stays
            // box ids inside the stretch
            sqS++; int nid = 0; ids.assign(len, 0); vector<int> startSq;
            auto getId = [&](int sq) { return sqStamp[sq] == sqS ? sqId[sq] : -1; };
            for (int i = a; i < b; i++) { int s = L.P.box[i]; int id = getId(s);
                if (id < 0) { id = nid++; startSq.push_back(s); }
                ids[i - a] = id; sqStamp[s] = sqS; sqId[s] = -1; int t = nb[s][L.P.dir[i]]; sqStamp[t] = sqS; sqId[t] = id; }
            int bId = getId(L.P.box[b]);
            // choose k distinct boxes among those pushed in the stretch (not the box pushed at b)
            vector<int> cand; for (int x = 0; x < nid; x++) if (x != bId) cand.push_back(x);
            if (cand.empty()) continue;
            int k = bellLen >= 0 ? bellK : A.k1 + rng() % (A.k2 - A.k1 + 1); k = min<int>(k, cand.size()); k = min(k, FW_KMAX);
            if (rng() % 2) { // boxes near one seed box (they interact), else any boxes of the stretch
                int s0 = cand[rng() % cand.size()]; vector<pair<int,int>> d;
                for (int x : cand) d.push_back({WD[(size_t)startSq[s0] * N + startSq[x]] * 16 + (int)(rng() % 16), x});
                sort(d.begin(), d.end()); for (int i = 0; i < k; i++) cand[i] = d[i].second; cand.resize(k); }
            else { shuffle(cand.begin(), cand.end(), rng); cand.resize(k); }
            vector<char> isR(nid, 0); for (int x : cand) isR[x] = 1;
            wBox.assign(L.P.box.begin() + a, L.P.box.begin() + b + 1); wDir.assign(L.P.dir.begin() + a, L.P.dir.begin() + b + 1);
            inR.assign(len + 1, 0); for (int i = 0; i < len; i++) inR[i] = isR[ids[i]];
            int rs[16], rg[16];
            { vector<int> pos(startSq); for (int i = 0; i < len; i++) pos[ids[i]] = nb[wBox[i]][wDir[i]]; for (int x = 0; x < k; x++) { rs[x] = startSq[cand[x]]; rg[x] = pos[cand[x]]; } }
            int forbid = -1;   // kick: ban one square the chosen boxes passed through (not their start or end squares)
            if ((int)(rng() % 100) < FW_KICK) { vector<int> via; for (int i = 0; i < len; i++) if (inR[i]) { int t = nb[wBox[i]][wDir[i]]; bool end = false; for (int x = 0; x < k; x++) if (t == rg[x] || t == rs[x]) end = true; if (!end) via.push_back(t); }
                if (via.empty()) continue; forbid = via[rng() % via.size()]; }
            { u64 h = 0x9E3779B97F4A7C15ULL * (u64)(a + 1) ^ ((u64)b << 20) ^ ((u64)(forbid + 1) << 40); vector<int> cs(cand.begin(), cand.end()); for (int x = 0; x < k; x++) cs[x] = rs[x]; sort(cs.begin(), cs.end());
              for (int v : cs) { h ^= (u64)v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2); }
              if (!tried.insert(h).second) continue; }
            u64 oldWin = pre[b + 1] - pre[a];
            int slackNow = (FW_SLM >= 0 && METRIC == 0) ? FW_SLM : (FW_SLP >= 0 && METRIC == 1) ? FW_SLP : FW_SLACK;
            u64 lim = bestC + ((u64)slackNow << SH); if (Lc > lim) lim = Lc;     // accept total <= lim
            u64 winBound = lim - (Lc - oldWin) + 1;
            u64 oc; i64 nodes;
            (void)rng();   // the version 5 engine drew a number here (it could also use A*); kept so a SEED gives the same lines
            bool okRun = hbs[tid].run(lay[a].data(), pl[a], wBox, wDir, inR, rs, rg, k, winBound, bellLen >= 0 ? bellBud : A.budget, forbid, oBox, oDir, oc, nodes);
            if (!okRun) continue;
            Pushes np; np.box.assign(L.P.box.begin(), L.P.box.begin() + a); np.dir.assign(L.P.dir.begin(), L.P.dir.begin() + a);
            np.box.insert(np.box.end(), oBox.begin(), oBox.end()); np.dir.insert(np.dir.end(), oDir.begin(), oDir.end());
            np.box.insert(np.box.end(), L.P.box.begin() + b + 1, L.P.box.end()); np.dir.insert(np.dir.end(), L.P.dir.begin() + b + 1, L.P.dir.end());
            u64 newC = Lc - oldWin + oc; if (newC > lim) continue;
            if (oBox == vector<int>(L.P.box.begin() + a, L.P.box.begin() + b + 1) && oDir == vector<int>(L.P.dir.begin() + a, L.P.dir.begin() + b + 1)) continue;   // same stretch
            L.P = np; Lc = newC; FW_ACC++; pushCosts(L.P, pre, lay, pl); Lc = pre[L.P.box.size()]; lineChanged();
            if (FW_XO) {
                #pragma omp critical
                bankAdd(L.P); }
            if (Lc < bestC && FW_ARM == -20 && FW_WADAPT && (FW_WM == 1 || FW_WM == 2)) { double lx = log((double)len), lim0 = log(4.0), lim1 = log(4.0 * FW_WHI);
                #pragma omp critical
                { FW_ADC = min(lim1, max(lim0, (1 - FW_WRATE) * FW_ADC + FW_WRATE * lx)); } }
            if (Lc < bestC) { bestC = Lc; sinceBest = 0;
                Line nl; if (render(L.P, nl) && lineCost(nl) == Lc) { L = nl; bestL = nl;
                    #pragma omp critical
                    { if (Lc < pool[0].c) { char tag[64]; snprintf(tag, sizeof tag, "fast walk %d %s k=%d len=%d", tid, bellLen >= 0 ? (FW_ARM == -11 ? "window line" : "bell") : A.name, k, len); addElite(nl, tag); } else addElite(nl, "fast walk"); } } }
        }
        if (finals) (*finals)[tid] = bestL;
    }
}

// Metric switching: walk on the target metric, then a shorter detour on the other metric starting from the best
// lines, then back on the target metric starting from the detour's lines. Fewer pushes often opens the way to fewer
// moves and the other way round (on 85687 the moves record only fell after the pushes walk had found 118-120 lines).
// Cycle length C (CYCLE=, default TIME/8 between 60 and 900 s): ALTF of it on the target metric, the rest on the other.
static void fastWalkAlternating(double tend, int cycle) {
    int target = METRIC; vector<Line> seeds; int round = 0;
    while (nowSec() < tend - 1) {
        double left = tend - nowSec(); double C = cycle > 0 ? cycle : max(60.0, min(900.0, (tend - T0) / 8));
        if (left < C * 0.8) { fastWalk(tend, seeds, nullptr); break; }
        double tT = nowSec() + C * FW_ALTF; fastWalk(tT, seeds, nullptr);
        // detour on the other metric from the pool's lines
        vector<Line> starts; for (auto& e : pool) starts.push_back(e.L);
        if (FW_PFSEED) { // PFSEED: the moves/pushes front (all trade-off lines) also seeds the detour, best on the other metric first
            vector<Line> fl = FRONTL; int sv = METRIC; METRIC = 1 - target;
            sort(fl.begin(), fl.end(), [](const Line& a, const Line& b) { return lineCost(a) < lineCost(b); }); METRIC = sv;
            vector<Line> st2; for (size_t i = 0; i < max(starts.size(), fl.size()); i++) { if (i < fl.size()) st2.push_back(fl[i]); if (i < starts.size()) st2.push_back(starts[i]); }
            starts = st2; }
        vector<Elite> saved = pool; METRIC = 1 - target; QUIET = true;
        for (auto& e : pool) e.c = lineCost(e.L); sort(pool.begin(), pool.end(), [](const Elite& a, const Elite& b) { return a.c < b.c; });
        if ((int)pool.size() > POOLMAX) pool.resize(POOLMAX);
        vector<Line> fin; fastWalk(nowSec() + C * (1 - FW_ALTF), starts, &fin);
        vector<Line> det; for (auto& e : pool) det.push_back(e.L); for (auto& l : fin) if (!l.lurd.empty()) det.push_back(l);
        METRIC = target; QUIET = false; pool = saved; for (auto& e : pool) e.c = lineCost(e.L);
        for (auto& l : det) { Line r; if (render(l.P, r)) addElite(r, "fast walk after metric detour"); }
        seeds = det; round++;
        if (FW_PFSEED) for (auto& l : FRONTL) seeds.push_back(l);   // the whole front also seeds the next target phase
        LOG("[%7.1fs] metric detour %d done (%d lines from the %s walk); best %lld/%lld\n", nowSec() - T0, round, (int)det.size(), target ? "moves" : "pushes", (long long)pool[0].L.moves, (long long)pool[0].L.pushes);
    }
}
