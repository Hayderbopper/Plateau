// ---------------------------------------------------------------- window line: calibration and the calibrated step size
// Calibration (CALIB=1, used by plateau_calibrate.exe): every attempt starts from the same fixed line and nothing found
// is ever applied. One attempt = one window of a length inside a 10-push band at a random position, with the walk's usual
// box choice and kick, re-planned by hook and ball; a hit = the window comes back strictly better (whole-line cost
// lower). Per band: attempts, hits, hit rate, time per attempt. Then ONE smooth curve is fitted to the hit rates (weighted
// least squares, polynomial of degree <= 2 in ln(window length), weights = attempts, floored at 5% of its peak) and
// written with a per-length weight table. The engine's WL=<file> mode (ARM=-11) draws every window length from it.

static int CAL_N = 1000, CAL_LINE = 0, CAL_DEG = 3, CAL_HIT = 0; static bool CAL_SLACKMODE = false; static string CAL_OUT = "window_line.txt";   // CAL_HIT: 0 = strict hits, slack hits if strict < 20; 1 = strict only; 2 = slack only
static vector<double> WL_CUM; static int WL_MAXL = 0; static bool WL_NOSIGNAL = false;   // cumulative weights for lengths 1..WL_MAXL (WL mode)

// one calibration attempt on line L with window length len; returns false if no valid window could be set up
static bool calAttempt(const Line& L, const vector<u64>& pre, const vector<vector<u64>>& lay, const vector<int>& pl, int len,
                       mt19937& rng, HookBall& hb, vector<int>& sqId, vector<int>& sqStamp, int& sqS, bool& hit, bool slackMode) {
    int n = L.P.box.size(); if (len < 1 || len > n - 1) return false;
    int a = rng() % (n - len), b = a + len;
    sqS++; int nid = 0; vector<int> ids(len, 0), startSq;
    auto getId = [&](int sq) { return sqStamp[sq] == sqS ? sqId[sq] : -1; };
    for (int i = a; i < b; i++) { int s = L.P.box[i]; int id = getId(s);
        if (id < 0) { id = nid++; startSq.push_back(s); }
        ids[i - a] = id; sqStamp[s] = sqS; sqId[s] = -1; int t = nb[s][L.P.dir[i]]; sqStamp[t] = sqS; sqId[t] = id; }
    int bId = getId(L.P.box[b]);
    vector<int> cand; for (int x = 0; x < nid; x++) if (x != bId) cand.push_back(x);
    if (cand.empty()) return false;
    int k = max(2, min(8, (int)llround(2.0 + len / 15.0))); k = min<int>(k, cand.size()); k = min(k, FW_KMAX);
    i64 budget = max<i64>(5000, min<i64>(300000, (i64)500 * len));
    if (rng() % 2) { int s0 = cand[rng() % cand.size()]; vector<pair<int,int>> d;
        for (int x : cand) d.push_back({WD[(size_t)startSq[s0] * N + startSq[x]] * 16 + (int)(rng() % 16), x});
        sort(d.begin(), d.end()); for (int i = 0; i < k; i++) cand[i] = d[i].second; cand.resize(k); }
    else { shuffle(cand.begin(), cand.end(), rng); cand.resize(k); }
    vector<char> isR(nid, 0); for (int x : cand) isR[x] = 1;
    vector<int> wBox(L.P.box.begin() + a, L.P.box.begin() + b + 1), wDir(L.P.dir.begin() + a, L.P.dir.begin() + b + 1), oBox, oDir;
    vector<char> inR(len + 1, 0); for (int i = 0; i < len; i++) inR[i] = isR[ids[i]];
    int rs[16], rg[16];
    { vector<int> pos(startSq); for (int i = 0; i < len; i++) pos[ids[i]] = nb[wBox[i]][wDir[i]]; for (int x = 0; x < k; x++) { rs[x] = startSq[cand[x]]; rg[x] = pos[cand[x]]; } }
    int forbid = -1;
    if ((int)(rng() % 100) < FW_KICK) { vector<int> via; for (int i = 0; i < len; i++) if (inR[i]) { int t = nb[wBox[i]][wDir[i]]; bool end = false; for (int x = 0; x < k; x++) if (t == rg[x] || t == rs[x]) end = true; if (!end) via.push_back(t); }
        if (via.empty()) return false; forbid = via[rng() % via.size()]; }
    u64 oldWin = pre[b + 1] - pre[a], oc; i64 nodes;
    // strict: the window must come back strictly cheaper. slack: any different window the walk would accept (whole line within SLACK of this line)
    u64 bound = slackMode ? oldWin + ((u64)FW_SLACK << SH) + 1 : oldWin;
    bool ok = hb.run(lay[a].data(), pl[a], wBox, wDir, inR, rs, rg, k, bound, budget, forbid, oBox, oDir, oc, nodes);
    if (slackMode) hit = ok && oc < bound && (oBox != wBox || oDir != wDir); else hit = ok && oc < oldWin;
    return true;
}

// weighted least squares polynomial fit y ~ sum c_j x^j (j <= deg)
static vector<double> polyFit(const vector<double>& x, const vector<double>& y, const vector<double>& w, int deg) {
    int m = deg + 1; vector<vector<double>> A(m, vector<double>(m + 1, 0));
    for (size_t i = 0; i < x.size(); i++) { vector<double> p(2 * m, 1); for (int j = 1; j < 2 * m; j++) p[j] = p[j - 1] * x[i];
        for (int r = 0; r < m; r++) { for (int c = 0; c < m; c++) A[r][c] += w[i] * p[r + c]; A[r][m] += w[i] * p[r] * y[i]; } }
    for (int c = 0; c < m; c++) { int piv = c; for (int r = c + 1; r < m; r++) if (fabs(A[r][c]) > fabs(A[piv][c])) piv = r; swap(A[c], A[piv]);
        if (fabs(A[c][c]) < 1e-12) continue;
        for (int r = 0; r < m; r++) if (r != c) { double f = A[r][c] / A[c][c]; for (int k = c; k <= m; k++) A[r][k] -= f * A[c][k]; } }
    vector<double> co(m, 0); for (int c = 0; c < m; c++) if (fabs(A[c][c]) >= 1e-12) co[c] = A[c][m] / A[c][c]; return co;
}

static int runCalibration(const Line& L) {
    vector<u64> pre; vector<vector<u64>> lay; vector<int> pl; pushCosts(L.P, pre, lay, pl);
    int n = L.P.box.size(), maxLen = n - 1; if (maxLen < 2) { LOG("line too short to calibrate\n"); return 1; }
    struct Band { int lo, hi; i64 att = 0, hit = 0, bad = 0; double sec = 0; }; vector<Band> bands;
    for (int lo = 1; lo <= maxLen; lo += 10) { Band B; B.lo = lo; B.hi = min(lo + 9, maxLen); bands.push_back(B); }
    LOG("calibration: line %lld/%lld (%d pushes), %zu bands of 10 pushes, %d attempts each, %d threads\n", (long long)L.moves, (long long)L.pushes, n, bands.size(), CAL_N, THREADS);
    double t0 = nowSec();
    CAL_SLACKMODE = CAL_HIT == 2;
  measure:
    for (auto& B : bands) { B.att = B.hit = B.bad = 0; B.sec = 0; }
    for (size_t bi = 0; bi < bands.size(); bi++) { Band& B = bands[bi]; atomic<i64> att{0}, hit{0}, bad{0}; double secSum = 0;
        #pragma omp parallel num_threads(THREADS) reduction(+:secSum)
        { int tid = 0;
#ifdef _OPENMP
          tid = omp_get_thread_num();
#endif
          mt19937 rng(FW_SEED * 7919u + 1000003u * (unsigned)bi + 131u * tid); vector<int> sqId(N, -1), sqStamp(N, 0); int sqS = 0;
          while (att.load() < CAL_N && bad.load() < 20LL * CAL_N) { int len = B.lo + rng() % (B.hi - B.lo + 1); bool h = false;
              double ts = nowSec(); bool ok = calAttempt(L, pre, lay, pl, len, rng, hbs[tid], sqId, sqStamp, sqS, h, CAL_SLACKMODE); double te = nowSec();
              if (!ok) { bad++; continue; }
              if (att.fetch_add(1) >= CAL_N) break;
              secSum += te - ts; if (h) hit++; } }
        B.att = min<i64>(att.load(), CAL_N); B.hit = hit.load(); B.bad = bad.load(); B.sec = secSum;
        LOG("CALIB %zu/%zu band %d-%d: %lld attempts, %lld hits (%.1f%%), %.2f ms per attempt\n", bi + 1, bands.size(), B.lo, B.hi, (long long)B.att, (long long)B.hit,
            B.att ? 100.0 * B.hit / B.att : 0.0, B.att ? 1000.0 * B.sec / B.att : 0.0); }
    { i64 th = 0; for (auto& B : bands) th += B.hit;
      if (!CAL_SLACKMODE && CAL_HIT == 0 && th < 20) { LOG("calibration: only %lld strict hits; measuring again counting slack hits (any different line within SLACK %d that the walk would accept)\n", (long long)th, FW_SLACK); CAL_SLACKMODE = true; goto measure; } }
    // fit one smooth curve to the hit rates over ln(length)
    vector<double> x, y, w; for (auto& B : bands) if (B.att > 0) { x.push_back(log((B.lo + B.hi) / 2.0)); y.push_back((double)B.hit / B.att); w.push_back((double)B.att); }
    int deg = min<int>(CAL_DEG, (int)x.size() - 1); if (deg < 0) { LOG("no valid attempts\n"); return 1; }
    vector<double> co = polyFit(x, y, w, deg);
    auto f = [&](double len) { double v = 0, p = 1, lx = min(x.back(), max(x.front(), log(len)));   // flat beyond the first and last band centres (no end spikes)
    for (double c : co) { v += c * p; p *= lx; } return v; };
    double peak = 0; for (int l = 1; l <= maxLen; l++) peak = max(peak, f(l));
    double floorW = peak > 0 ? 0.05 * peak : 1.0;
    FILE* o = fopen(CAL_OUT.c_str(), "w"); if (!o) { LOG("cannot write %s\n", CAL_OUT.c_str()); return 1; }
    fprintf(o, "# Plateau window line (calibration). Every attempt started from the same line; nothing was applied.\n");
    fprintf(o, "level %dx%d, %d boxes; line %lld/%lld (%d pushes); P=%d; %d attempts per band; seed %u; %.0f s\n", GW, GH, NBOX, (long long)L.moves, (long long)L.pushes, n, METRIC, CAL_N, FW_SEED, nowSec() - t0);
    fprintf(o, "hits: %s\n", CAL_SLACKMODE ? "slack (any different window the walk would accept, within SLACK)" : "strict (strictly better line)");
    fprintf(o, "bands (lo hi attempts hits hit%% ms_per_attempt)\n");
    for (auto& B : bands) fprintf(o, "band %d %d %lld %lld %.3f %.3f\n", B.lo, B.hi, (long long)B.att, (long long)B.hit, B.att ? 100.0 * B.hit / B.att : 0.0, B.att ? 1000.0 * B.sec / B.att : 0.0);
    fprintf(o, "fit hit_rate = poly(ln L), degree %d:", deg); for (double c : co) fprintf(o, " %.6g", c); fprintf(o, "; floor %.6g (5%% of peak %.6g)\n", floorW, peak);
    i64 totHits = 0; for (auto& B : bands) totHits += B.hit;
    if (totHits < 10) { fprintf(o, "nosignal: only %lld hits in all bands; the engine uses its default step sizes with this file\n", (long long)totHits);
        LOG("calibration: only %lld hits in all bands, so this line gives no usable signal; WL will fall back to the default step sizes\n", (long long)totHits); }
    fprintf(o, "curve (L weight)\n");
    for (int l = 1; l <= maxLen; l++) fprintf(o, "%d %.6g\n", l, max(floorW, f(l)));
    fclose(o);
    LOG("calibration done: %s (peak hit rate %.2f%% at about %d pushes)\n", CAL_OUT.c_str(), 100 * peak, [&]() { int bl = 1; for (int l = 1; l <= maxLen; l++) if (f(l) > f(bl)) bl = l; return bl; }());
    return 0;
}

static bool loadWindowLine(const string& path) {
    ifstream in(path); if (!in) return false; string line; bool cur = false; vector<pair<int,double>> t;
    WL_NOSIGNAL = false;
    while (getline(in, line)) { if (!cur) { if (line.rfind("nosignal", 0) == 0) WL_NOSIGNAL = true; if (line.rfind("curve", 0) == 0) cur = true; continue; }
        int l; double w; if (sscanf(line.c_str(), "%d %lf", &l, &w) == 2 && l >= 1 && w >= 0) t.push_back({l, w}); }
    if (t.empty()) return false;
    WL_MAXL = 0; for (auto& p : t) WL_MAXL = max(WL_MAXL, p.first);
    vector<double> wt(WL_MAXL + 1, 0); for (auto& p : t) wt[p.first] = p.second;
    WL_CUM.assign(WL_MAXL + 1, 0); for (int l = 1; l <= WL_MAXL; l++) WL_CUM[l] = WL_CUM[l - 1] + wt[l];
    return WL_CUM[WL_MAXL] > 0;
}
// draw a window length <= maxLen from the calibrated curve
static int drawWindowLine(int maxLen, mt19937& rng) {
    int top = min(maxLen, WL_MAXL); if (top < 1) return max(1, maxLen);
    double u = ((double)rng() + 0.5) / 4294967296.0 * WL_CUM[top];
    return max(1, (int)(lower_bound(WL_CUM.begin() + 1, WL_CUM.begin() + top + 1, u) - WL_CUM.begin()));
}

// ---- ARM=-12: recalibration during the run (one walker, a few seconds, on the pool's best line)
static double FW_RECAL = 150, FW_RECALBUD = 8; static int FW_RECALN = 200;
static bool dynRecalibrate(const Line& L, mt19937& rng, HookBall& hb, vector<int>& sqId, vector<int>& sqStamp, int& sqS, vector<double>& cum, double tRun) {
    vector<u64> pre; vector<vector<u64>> lay; vector<int> pl; pushCosts(L.P, pre, lay, pl);
    int n = L.P.box.size(), maxLen = n - 1; if (maxLen < 2) return false;
    int nb = (maxLen + 9) / 10; double cap = FW_RECALBUD / 2 / nb;   // half the budget for strict hits, half for slack hits if needed
    vector<double> x, y, w; bool slack = false; i64 total = 0;
    for (int pass = 0; pass < 2; pass++) {
        x.clear(); y.clear(); w.clear(); total = 0;
        for (int lo = 1; lo <= maxLen; lo += 10) { int hi = min(lo + 9, maxLen); i64 att = 0, hit = 0, bad = 0; double t0 = nowSec();
            while (att < FW_RECALN && bad < 20LL * FW_RECALN && (att < 20 || nowSec() - t0 < cap)) { bool h = false;
                if (!calAttempt(L, pre, lay, pl, lo + rng() % (hi - lo + 1), rng, hb, sqId, sqStamp, sqS, h, slack)) { bad++; continue; }
                att++; hit += h; }
            if (att) { x.push_back(log((lo + hi) / 2.0)); y.push_back((double)hit / att); w.push_back((double)att); total += hit; } }
        if (total >= 20 || slack) break; slack = true; }
    if (total < 10 || x.empty()) { LOG("[%7.1fs] window line recalibrated on %lld/%lld: no signal (%lld %s hits), using the S-curve until the next one\n", tRun, (long long)L.moves, (long long)L.pushes, (long long)total, slack ? "slack" : "strict"); return false; }
    int deg = min<int>(CAL_DEG, (int)x.size() - 1); vector<double> co = polyFit(x, y, w, deg);
    auto fv = [&](double len) { double v = 0, pw = 1, lx = min(x.back(), max(x.front(), log(len))); for (double cc : co) { v += cc * pw; pw *= lx; } return v; };
    double peak = 0; int at = 1; for (int l = 1; l <= maxLen; l++) if (fv(l) > peak) { peak = fv(l); at = l; }
    if (peak <= 0) return false;
    cum.assign(maxLen + 1, 0); for (int l = 1; l <= maxLen; l++) cum[l] = cum[l - 1] + max(0.05 * peak, fv(l));
    LOG("[%7.1fs] window line recalibrated on %lld/%lld (%s hits, %lld): peak %.1f%% at about %d pushes\n", tRun, (long long)L.moves, (long long)L.pushes, slack ? "slack" : "strict", (long long)total, 100 * peak, at);
    return true;
}
static double FW_RECAL_EXT() { return FW_RECAL; }
