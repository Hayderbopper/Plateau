// ---------------------------------------------------------------- crossover over a bank of lines
// Lines that pass through the same position (same boxes, same player region) can be cut there and joined:
// prefix of one + suffix of another. With many walker lines in a bank, the cheapest combination of all their
// pieces is a shortest path over their positions (Dijkstra). Each state is (boxes, exact player square); from a
// state, any push that some bank line makes from a position with the same boxes and player region is allowed.
// The bank holds the last BANK lines the walkers accepted; one walker runs a crossover every XOT seconds over the
// elite pool plus the bank.
static int FW_XO = 1, FW_BANK = 256; static double FW_XOT = 1.0;
static vector<Pushes> XBANK; static size_t XBANK_POS = 0; static atomic<i64> XO_RUNS{0}, XO_WINS{0};
static void bankAdd(const Pushes& P) { // caller holds the critical section
    if ((int)XBANK.size() < FW_BANK) XBANK.push_back(P); else { XBANK[XBANK_POS] = P; XBANK_POS = (XBANK_POS + 1) % FW_BANK; } }
static inline u64 boxHash(const u64* c) { u64 h = 0x84222325CBF29CE4ULL; for (int w = 0; w < NW; w++) { h ^= c[w]; h *= 0x100000001B3ULL; h ^= h >> 29; } return h; }
// returns true and the cheapest combined push list if it is cheaper than 'beat'
static bool crossover(const vector<Pushes>& lines, u64 beat, Pushes& out, u64& outC) {
    static thread_local Walker W; if ((int)W.dist.size() != N) W.init();
    int nl = lines.size(); if (nl < 2) return false;
    // states
    unordered_map<u64, int> stateId; vector<vector<u64>> sBox; vector<int> sP; vector<u64> sKey; vector<char> sGoal;
    vector<vector<int>> nodeOf(nl);
    unordered_map<u64, vector<pair<int,int>>> group;   // region key -> (line, index) pushes made from there
    for (int l = 0; l < nl; l++) { const Pushes& P = lines[l]; int n = P.box.size(); nodeOf[l].assign(n + 1, -1);
        vector<u64> c = BOX0; int p = PLAYER0;
        for (int i = 0; i <= n; i++) {
            u64 bh = boxHash(c.data()); u64 sh = bh ^ ((u64)(p + 1) * 0x9E3779B97F4A7C15ULL);
            auto it = stateId.find(sh); int id;
            if (it == stateId.end()) { id = sBox.size(); stateId[sh] = id; sBox.push_back(c); sP.push_back(p);
                W.bfs(c.data(), p); int mr = p; for (int x = 0; x < N; x++) if (W.reach(x)) { mr = x; break; }
                sKey.push_back(bh ^ ((u64)(mr + 1) * 0xC2B2AE3D27D4EB4FULL)); sGoal.push_back(c == GOALS); }
            else id = it->second;
            nodeOf[l][i] = id;
            if (i == n) break;
            group[sKey[id]].push_back({l, i});
            int b = P.box[i], d = P.dir[i]; if (!tb(c.data(), b)) { nodeOf[l].resize(i + 1); break; } cb(c.data(), b); sb(c.data(), nb[b][d]); p = b; }
    }
    int ns = sBox.size(); vector<u64> dist(ns, UINT64_MAX); vector<int> parS(ns, -1), parL(ns, -1), parI(ns, -1);
    typedef pair<u64, int> QE; priority_queue<QE, vector<QE>, greater<QE>> pq;
    int s0 = nodeOf[0][0]; dist[s0] = 0; pq.push({0, s0}); int goal = -1;
    while (!pq.empty()) { auto [dc, s] = pq.top(); pq.pop(); if (dc != dist[s]) continue; if (dc >= beat) break;
        if (sGoal[s]) { goal = s; break; }
        auto git = group.find(sKey[s]); if (git == group.end()) continue;
        W.bfs(sBox[s].data(), sP[s]);
        for (auto& [l, i] : git->second) { const Pushes& P = lines[l]; if (i + 1 >= (int)nodeOf[l].size()) continue;
            int b = P.box[i], d = P.dir[i], q = nb[b][OPP[d]]; if (q < 0 || !W.reach(q)) continue;
            int t = nodeOf[l][i + 1]; u64 nc = dc + cost(W.dist[q] + 1, 1);
            if (nc < dist[t]) { dist[t] = nc; parS[t] = s; parL[t] = l; parI[t] = i; pq.push({nc, t}); } } }
    if (goal < 0 || dist[goal] >= beat) return false;
    out.box.clear(); out.dir.clear();
    for (int s = goal; parS[s] >= 0; s = parS[s]) {
        out.box.push_back(lines[parL[s]].box[parI[s]]); out.dir.push_back(lines[parL[s]].dir[parI[s]]); }
    reverse(out.box.begin(), out.box.end()); reverse(out.dir.begin(), out.dir.end()); outC = dist[goal]; return true;
}
