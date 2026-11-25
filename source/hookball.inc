// ---------------------------------------------------------------- hook and ball: the step search
// Re-inserts k boxes' pushes around a fixed skeleton (the other pushes of a stretch, in their order), from the
// board before the stretch to the board after it. Both sides search layer by layer, at most HB_BRANCH children per
// node and HB_LAYER nodes per layer, each layer sorted by g + HB_WH * (push distance still needed):
//   forward ("hook"):  from the start, doing the next skeleton push or pushing one of the k boxes;
//   backward ("ball"): from the goal, undoing pushes (pulls).
// The sides meet when a forward and a backward state have the same skeleton step and box squares and the player can
// walk from one to the other. The search stops at the end of the layer in which the first meeting point was found
// and returns the cheapest meeting point. A cheaper path to a state already seen becomes a new node, so every stored
// cost is exact and a meeting point is priced as forward cost + backward cost + the walk between them.
// State key: (skeleton step j, player square, sorted squares of the k boxes) packed in 128 bits.
struct FKey { u64 a, b; bool operator==(const FKey& o) const { return a == o.a && b == o.b; } };
static inline u64 fkHash(const FKey& k) { u64 h = k.a * 0x9E3779B97F4A7C15ULL ^ (k.b + 0x632BE59BD9B4E019ULL + (k.a << 6)); h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 29; return h; }
static FKey fkMake(int j, int p, int k, const int* r) {
    int s[16]; for (int i = 0; i < k; i++) s[i] = r[i]; sort(s, s + k);
    // j, then 11 bits for the player and for each box (k <= 8 boxes fits in 128 bits)
    unsigned __int128 v = (unsigned)j; v = (v << 11) | (unsigned)p; for (int i = 0; i < k; i++) v = (v << 11) | (unsigned)s[i];
    FKey K; K.a = (u64)v; K.b = (u64)(v >> 64); return K; }

static int HB_WH = 2, HB_BRANCH = 10, HB_LAYER = 50;
struct HookBall {
    Walker W; Freeze FZ;
    vector<vector<u64>> SL; vector<int> qpos, skBox, skDir;   // SL[j]: skeleton-only board before skeleton push j
    struct Node { int j, p; int r[9]; u32 parent; int box, dir; u64 g; int h; u32 nextLay; };
    vector<Node> S[2];   // 0 forward, 1 backward
    // flat open-addressing tables (small, grow on demand): exact (j, player, boxes) -> node, and (j, boxes) -> first node
    // of a chain through Node::nextLay. Slots hold 32 hash bits + index+1, so misses never touch the node array.
    struct Flat { vector<u64> sl; vector<FKey> keys; vector<u32> vals; u64 mask = 0;
        void reset(int lg) { if (sl.size() != ((size_t)1 << lg)) sl.assign((size_t)1 << lg, 0); else memset(sl.data(), 0, sl.size() * 8); mask = sl.size() - 1; keys.clear(); vals.clear(); }
        void grow() { vector<u64> o; o.swap(sl); sl.assign(o.size() * 2, 0); mask = sl.size() - 1;
            for (u64 v : o) if (v) { u64 h = fkHash(keys[(u32)v - 1]); u64 x = h & mask; while (sl[x]) x = (x + 1) & mask; sl[x] = v; } }
        int find(const FKey& K) const { u64 h = fkHash(K), x = h & mask, tag = (h >> 32) << 32;
            while (true) { u64 v = sl[x]; if (!v) return -1; if ((v & 0xFFFFFFFF00000000ULL) == tag && keys[(u32)v - 1] == K) return (int)(u32)v - 1; x = (x + 1) & mask; } }
        int insert(const FKey& K, u32 val, bool& isNew) { if ((keys.size() + 1) * 2 > sl.size()) grow();
            u64 h = fkHash(K), x = h & mask, tag = (h >> 32) << 32;
            while (true) { u64 v = sl[x]; if (!v) { keys.push_back(K); vals.push_back(val); sl[x] = tag | keys.size(); isNew = true; return keys.size() - 1; }
                if ((v & 0xFFFFFFFF00000000ULL) == tag && keys[(u32)v - 1] == K) { isNew = false; return (int)(u32)v - 1; } x = (x + 1) & mask; } } };
    Flat seen[2], byLay[2];
    void init() { W.init(); FZ.init(); }
    static FKey layKey(int j, int k, const int* r) { return fkMake(j, 0, k, r); }
    void path(u32 f, u32 b, vector<int>& oBox, vector<int>& oDir) {
        oBox.clear(); oDir.clear();
        for (u32 x = f; S[0][x].parent != UINT32_MAX; x = S[0][x].parent) { oBox.push_back(S[0][x].box); oDir.push_back(S[0][x].dir); }
        reverse(oBox.begin(), oBox.end()); reverse(oDir.begin(), oDir.end());
        for (u32 x = b; S[1][x].parent != UINT32_MAX; x = S[1][x].parent) { oBox.push_back(S[1][x].box); oDir.push_back(S[1][x].dir); } }
    // c0, p0: board and player before the stretch; wBox/wDir: the stretch's pushes (the last one stays); inR[i]: push i
    // belongs to a re-planned box; rs/rg: start and end squares of the k re-planned boxes; forbidSq: square they may not
    // be pushed onto (-1 = none). Returns the cheapest new stretch cheaper than bound (oBox/oDir, cost oC).
    bool run(const u64* c0, int p0, const vector<int>& wBox, const vector<int>& wDir, const vector<char>& inR, const int* rs, const int* rg, int k,
             u64 bound, i64 budget, int forbidSq, vector<int>& oBox, vector<int>& oDir, u64& oC, i64& nodes) {
        int n = wBox.size(); skBox.clear(); skDir.clear();
        for (int i = 0; i < n; i++) if (!inR[i]) { skBox.push_back(wBox[i]); skDir.push_back(wDir[i]); }
        int m = skBox.size(); if (m == 0) return false;
        SL.resize(m + 1); qpos.assign(m, 0);
        vector<u64> c(c0, c0 + NW); for (int a = 0; a < k; a++) cb(c.data(), rs[a]);
        for (int j = 0; j < m; j++) { SL[j] = c; cb(c.data(), skBox[j]); sb(c.data(), nb[skBox[j]][skDir[j]]); qpos[j] = nb[skBox[j]][OPP[skDir[j]]]; } SL[m] = c;
        int sg[16]; for (int a = 0; a < k; a++) sg[a] = rg[a]; sort(sg, sg + k);
        int ss[16]; for (int a = 0; a < k; a++) ss[a] = rs[a]; sort(ss, ss + k);
        // pushes still needed (each box to its nearest end / start square) plus the skeleton pushes left; -1 = impossible
        auto hF = [&](int j, const int* r) { int h = 0; for (int a = 0; a < k; a++) { int best = 65535; for (int b = 0; b < k; b++) best = min<int>(best, PD[(size_t)r[a] * N + sg[b]]); if (best == 65535) return -1; h += best; } return h + (m - j); };
        auto hB = [&](int j, const int* r) { int h = 0; for (int a = 0; a < k; a++) { int best = 65535; for (int b = 0; b < k; b++) best = min<int>(best, PD[(size_t)ss[b] * N + r[a]]); if (best == 65535) return -1; h += best; } return h + j; };
        for (int s = 0; s < 2; s++) { S[s].clear(); seen[s].reset(10); byLay[s].reset(10); }
        auto add = [&](int s, Node& nd) -> int {
            FKey K = fkMake(nd.j, nd.p, k, nd.r); bool isNew; int e = seen[s].insert(K, S[s].size(), isNew);
            if (!isNew) { u32 oid = seen[s].vals[e]; if (!(nd.g < S[s][oid].g)) return -1; seen[s].vals[e] = S[s].size(); }   // cheaper: a new node
            u32 id = S[s].size(); int le = byLay[s].insert(layKey(nd.j, k, nd.r), UINT32_MAX, isNew); nd.nextLay = byLay[s].vals[le]; byLay[s].vals[le] = id; S[s].push_back(nd); return id; };
        Node st0; st0.j = 0; st0.p = p0; memcpy(st0.r, rs, sizeof(int) * k); st0.parent = UINT32_MAX; st0.box = st0.dir = -1; st0.g = 0; st0.h = hF(0, rs);
        Node gl; gl.j = m; gl.p = skBox[m - 1]; memcpy(gl.r, sg, sizeof(int) * k); gl.parent = UINT32_MAX; gl.box = gl.dir = -1; gl.g = 0; gl.h = hB(m, sg);
        if (st0.h < 0) return false;
        add(0, st0); add(1, gl);
        vector<u32> layer[2] = {{0}, {0}};
        u64 best = UINT64_MAX; u32 bf = 0, bb = 0; int meetDepth = -1, depth = 0; nodes = 0; vector<u64> lay(NW);
        struct Ch { int h; Node nd; };
        vector<Ch> ch;
        auto offer = [&](Node& y, int h) { if (h < 0) return; y.h = (int)(y.g >> SH) + HB_WH * h; ch.push_back({y.h, y}); };
        auto tryMeet = [&](int s, u32 id) {   // W holds the BFS from node id's player in its layout
            const Node& x = S[s][id]; int le = byLay[1 - s].find(layKey(x.j, k, x.r)); if (le < 0) return;
            for (u32 o = byLay[1 - s].vals[le]; o != UINT32_MAX; o = S[1 - s][o].nextLay) { const Node& y = S[1 - s][o]; if (!W.reach(y.p)) continue;
                u64 tot = x.g + y.g + cost(W.dist[y.p], 0); if (!(tot < best && tot < bound)) continue;
                best = tot; bf = s == 0 ? id : o; bb = s == 0 ? o : id; if (meetDepth < 0) meetDepth = depth; } };
        while (meetDepth < 0 || depth < meetDepth) {
            bool any = false;
            for (int s = 0; s < 2; s++) {
                vector<u32>& L = layer[s]; if (L.empty()) continue;
                sort(L.begin(), L.end(), [&](u32 a, u32 b) { const Node& A = S[s][a]; const Node& B = S[s][b]; return A.h != B.h ? A.h < B.h : A.g < B.g; });
                vector<u32> next;
                for (u32 id : L) {
                    if (++nodes > budget) goto done;
                    Node x = S[s][id];
                    for (int w = 0; w < NW; w++) lay[w] = SL[x.j][w]; for (int a = 0; a < k; a++) sb(lay.data(), x.r[a]);
                    W.bfs(lay.data(), x.p);
                    tryMeet(s, id);
                    ch.clear();
                    if (s == 0) {
                        if (x.j == m) continue;
                        { int q = qpos[x.j], t = nb[skBox[x.j]][skDir[x.j]];   // the next skeleton push
                          if (W.reach(q) && !tb(lay.data(), t)) { Node y = x; y.j = x.j + 1; y.p = skBox[x.j]; y.parent = id; y.box = skBox[x.j]; y.dir = skDir[x.j]; y.g = x.g + cost(W.dist[q] + 1, 1); offer(y, hF(y.j, y.r)); } }
                        for (int a = 0; a < k; a++) for (int d = 0; d < 4; d++) { int b = x.r[a], q = nb[b][OPP[d]], t = nb[b][d];   // push a re-planned box
                            if (q < 0 || t < 0 || t == forbidSq || !W.reach(q) || tb(lay.data(), t) || deadSq[t]) continue;
                            cb(lay.data(), b); sb(lay.data(), t); bool dl = !isGoal[t] && FZ.deadlock(lay.data(), t); cb(lay.data(), t); sb(lay.data(), b); if (dl) continue;
                            Node y = x; y.r[a] = t; y.p = b; y.parent = id; y.box = b; y.dir = d; y.g = x.g + cost(W.dist[q] + 1, 1); offer(y, hF(y.j, y.r)); }
                    } else {
                        if (x.j >= 1) { int sq = skBox[x.j - 1], qp = qpos[x.j - 1];   // undo skeleton push j-1
                            if (W.reach(sq)) { bool clash = false; for (int a = 0; a < k; a++) if (x.r[a] == qp || x.r[a] == sq) clash = true;
                                if (!clash) { Node y = x; y.j = x.j - 1; y.p = qp; y.parent = id; y.box = sq; y.dir = skDir[x.j - 1]; y.g = x.g + cost(W.dist[sq] + 1, 1); offer(y, hB(y.j, y.r)); } } }
                        if (x.j < m) for (int a = 0; a < k; a++) for (int d = 0; d < 4; d++) { int t = x.r[a], b = nb[t][OPP[d]]; if (b < 0) continue; int pl = nb[b][OPP[d]];   // undo a box push
                            if (pl < 0 || t == forbidSq || deadSq[b] || !W.reach(b) || tb(lay.data(), pl)) continue;
                            cb(lay.data(), t); sb(lay.data(), b); bool dl = !isGoal[b] && FZ.deadlock(lay.data(), b); cb(lay.data(), b); sb(lay.data(), t); if (dl) continue;
                            Node y = x; y.r[a] = b; y.p = pl; y.parent = id; y.box = b; y.dir = d; y.g = x.g + cost(W.dist[b] + 1, 1); offer(y, hB(y.j, y.r)); }
                    }
                    if ((int)ch.size() > HB_BRANCH) { nth_element(ch.begin(), ch.begin() + HB_BRANCH, ch.end(), [](const Ch& a, const Ch& b) { return a.h < b.h; }); ch.resize(HB_BRANCH); }
                    for (auto& c2 : ch) { if (c2.nd.g >= bound) continue; int nid = add(s, c2.nd); if (nid >= 0) next.push_back(nid); }
                    if ((int)next.size() > HB_LAYER) break;
                }
                L.swap(next); any = any || !L.empty();
            }
            depth++;
            if (!any) break;   // both sides are exhausted (meets are checked on expansion)
        }
    done:
        if (best == UINT64_MAX) return false;
        path(bf, bb, oBox, oDir); oC = best;
        return true;
    }
};
