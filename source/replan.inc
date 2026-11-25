// ---------------------------------------------------------------- exact replan: box history re-planning
// Remove every push of k chosen boxes (k <= 3) from the whole line. The rest (the skeleton) keeps its push order.
// An exact A* re-inserts the chosen boxes: state = (skeleton step j, squares of the chosen boxes, player square).
// Moves: do skeleton push j, or push one chosen box one step. Goal: skeleton done, chosen boxes on their goals.
// Lower bound: every remaining skeleton push needs at least its walk in the skeleton-only board (chosen boxes
// removed) from where the previous skeleton push left the player, plus the push itself.

static vector<int> boxIds(const Pushes& P, vector<int>& startSq) { // id of the box moved by each push; ids by initial square
    map<int,int> at; startSq.clear();
    for (int w = 0; w < NW; w++) { u64 x = BOX0[w]; while (x) { int s = w * 64 + __builtin_ctzll(x); x &= x - 1; at[s] = startSq.size(); startSq.push_back(s); } }
    vector<int> id(P.box.size());
    for (size_t i = 0; i < P.box.size(); i++) { auto it = at.find(P.box[i]); int x = it->second; at.erase(it); id[i] = x; at[nb[P.box[i]][P.dir[i]]] = x; }
    return id;
}

struct Replanner {
    struct St { u64 key; u64 g; u32 parent; int8_t mv; int8_t dir; int16_t who; uint8_t closed; };
    vector<St> st; unordered_map<u64, u32> idx; Walker W; Freeze FZ;
    void init() { W.init(); FZ.init(); }
    // key: j (16 bits) | p (12) | up to 3 squares (12 each) sorted
    static u64 mk(int j, int p, int k, const int* r) { int s[3] = {0, 0, 0}; for (int i = 0; i < k; i++) s[i] = r[i] + 1; sort(s, s + k);
        return (u64)j | ((u64)p << 16) | ((u64)s[0] << 28) | ((u64)s[1] << 40) | ((u64)s[2] << 52); }

    // returns true and new push list if strictly cheaper than bound
    bool run(const Pushes& P, const vector<int>& id, const vector<int>& startSq, const vector<int>& R, u64 bound, i64 budget, Pushes& outP, u64& outC, i64& nodesOut, bool& exhausted) {
        int k = R.size(); int n = P.box.size(); exhausted = false; nodesOut = 0;
        vector<char> isR(startSq.size(), 0); for (int r : R) isR[r] = 1;
        vector<int> sk; for (int i = 0; i < n; i++) if (!isR[id[i]]) sk.push_back(i);
        int m = sk.size();
        // skeleton layouts
        vector<vector<u64>> SL(m + 1, vector<u64>(NW)); vector<u64> c = BOX0; for (int r : R) cb(c.data(), startSq[r]);
        int rs[3], rg[3];
        { vector<u64> full = BOX0; vector<int> pos(startSq); for (int i = 0; i < n; i++) { pos[id[i]] = nb[P.box[i]][P.dir[i]]; } for (int a = 0; a < k; a++) { rs[a] = startSq[R[a]]; rg[a] = pos[R[a]]; } }
        for (int j = 0; j < m; j++) { SL[j] = c; int i = sk[j]; cb(c.data(), P.box[i]); sb(c.data(), nb[P.box[i]][P.dir[i]]); } SL[m] = c;
        // distance tables: qd[j][x] = walk from x to the push position of skeleton push j in SL[j]
        vector<vector<uint16_t>> qd(m); vector<int> qpos(m);
        for (int j = 0; j < m; j++) { int i = sk[j]; qpos[j] = nb[P.box[i]][OPP[P.dir[i]]]; W.bfs(SL[j].data(), qpos[j]); qd[j].assign(N, 65535); for (int x = 0; x < N; x++) if (W.reach(x)) qd[j][x] = W.dist[x]; }
        vector<i64> suf(m + 2, 0); // suf[j] = sum_{j'>=j} (qd[j'][box of j'-1] + 1), j>=1
        for (int j = m - 1; j >= 1; j--) { int pv = P.box[sk[j - 1]]; int d = qd[j][pv]; suf[j] = suf[j + 1] + (d == 65535 ? 0 : d) + 1; }
        int sortedG[3]; for (int a = 0; a < k; a++) sortedG[a] = rg[a] + 1; sort(sortedG, sortedG + k);
        auto rmatch = [&](const int* r) -> int { // pushes lower bound for chosen boxes to their goals (k<=3: try all perms)
            int perm[3] = {0, 1, 2}; int best = INT_MAX;
            do { int s = 0; for (int a = 0; a < k; a++) { int d = PD[(size_t)r[a] * N + rg[perm[a]]]; if (d == 65535) { s = INT_MAX; break; } s += d; } best = min(best, s); } while (next_permutation(perm, perm + k));
            return best; };
        auto hcost = [&](int j, int p, const int* r) -> u64 { int hp = rmatch(r); if (hp == INT_MAX) return UINT64_MAX; hp += m - j;
            i64 hm = 0; if (j < m) { int d = qd[j][p]; hm = (d == 65535 ? 0 : d) + 1 + suf[j + 1]; } hm = max<i64>(hm, hp); return cost(hm, hp); };
        st.clear(); idx.clear();
        struct QE { u64 f, g; u32 id; bool operator<(const QE& o) const { return f != o.f ? f > o.f : g < o.g; } };
        priority_queue<QE> pq;
        int r0[3]; for (int a = 0; a < k; a++) r0[a] = rs[a];
        u64 h0 = hcost(0, PLAYER0, r0); if (h0 == UINT64_MAX || h0 >= bound) { exhausted = true; return false; }
        st.push_back({mk(0, PLAYER0, k, r0), 0, UINT32_MAX, -1, 0, 0, 0}); idx[st[0].key] = 0; pq.push({h0, 0, 0});
        u64 best = UINT64_MAX; u32 bestId = UINT32_MAX; i64 exp = 0;
        vector<u64> lay(NW);
        while (!pq.empty()) {
            QE e = pq.top(); pq.pop(); if (e.g != st[e.id].g || st[e.id].closed) continue;
            if (e.f >= bound || e.f >= best) break;
            st[e.id].closed = 1; u64 key = st[e.id].key; int j = key & 0xffff, p = (key >> 16) & 0xfff; int r[3]; int kk = 0;
            for (int a = 0; a < 3; a++) { int v = (key >> (28 + 12 * a)) & 0xfff; if (v) r[kk++] = v - 1; }
            if (j == m) { bool ok = true; int s[3]; for (int a = 0; a < k; a++) s[a] = r[a] + 1; sort(s, s + k); for (int a = 0; a < k; a++) if (s[a] != sortedG[a]) ok = false;
                if (ok) { best = e.g; bestId = e.id; break; } }
            if (++exp > budget) { nodesOut = exp; return false; }
            lay = SL[j]; for (int a = 0; a < k; a++) sb(lay.data(), r[a]);
            W.bfs(lay.data(), p);
            auto relax = [&](int j2, int p2, const int* r2, u64 g2, int mv, int who, int dir) {
                u64 h = hcost(j2, p2, r2); if (h == UINT64_MAX) return; u64 f = g2 + h; if (f >= bound || f >= best) return;
                u64 k2 = mk(j2, p2, k, r2); auto it = idx.find(k2); u32 nid;
                if (it == idx.end()) { nid = st.size(); st.push_back({k2, UINT64_MAX, UINT32_MAX, 0, 0, 0, 0}); idx[k2] = nid; } else nid = it->second;
                if (g2 >= st[nid].g) return; st[nid].g = g2; st[nid].parent = e.id; st[nid].mv = mv; st[nid].who = who; st[nid].dir = dir; st[nid].closed = 0;
                pq.push({f, g2, nid}); };
            u64 g = st[e.id].g;
            if (j < m) { int i = sk[j]; int q = qpos[j], t = nb[P.box[i]][P.dir[i]];
                if (W.reach(q) && !tb(lay.data(), t)) relax(j + 1, P.box[i], r, g + cost(W.dist[q] + 1, 1), 0, i, P.dir[i]); }
            for (int a = 0; a < k; a++) for (int d = 0; d < 4; d++) { int b = r[a], q = nb[b][OPP[d]], t = nb[b][d];
                if (q < 0 || t < 0 || !W.reach(q) || tb(lay.data(), t) || deadSq[t]) continue;
                cb(lay.data(), b); sb(lay.data(), t); bool dl = FZ.deadlock(lay.data(), t); cb(lay.data(), t); sb(lay.data(), b); if (dl) continue;
                int r2[3] = {r[0], r[1], r[2]}; r2[a] = t; relax(j, b, r2, g + cost(W.dist[q] + 1, 1), 1, b, d); }
        }
        nodesOut = exp; exhausted = true;
        if (bestId == UINT32_MAX) return false;
        vector<pair<int,int>> v; for (u32 x = bestId; st[x].parent != UINT32_MAX; x = st[x].parent) { const St& s = st[x]; v.push_back({s.mv == 0 ? P.box[s.who] : s.who, s.dir}); }
        reverse(v.begin(), v.end()); outP.box.clear(); outP.dir.clear(); for (auto& pr : v) { outP.box.push_back(pr.first); outP.dir.push_back(pr.second); }
        outC = best; return true;
    }
};
static vector<Replanner> replanners;
