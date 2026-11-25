// Plateau GUI - Windows front end for the Plateau optimizer (plateau.exe).
// The optimizer itself is unchanged: this program writes input.txt for a level, runs plateau.exe on it
// (hidden, same arguments the .bat used) and reads the verified lines it writes to best_*.txt.
// Build (in source): windres plateau_gui.rc -O coff -o plateau_gui_res.o
//   g++ -O2 -municode -mwindows -static -o plateau_gui.exe plateau_gui.cpp plateau_gui_res.o -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <set>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include "gui_res.h"
using namespace std;

// ------------------------------------------------------------------ text helpers
static wstring W(const string& s) {
    if (s.empty()) return L"";
    UINT cp = CP_UTF8; DWORD fl = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) { cp = CP_ACP; fl = 0; n = MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), nullptr, 0); }
    wstring w(n, 0); MultiByteToWideChar(cp, fl, s.data(), (int)s.size(), &w[0], n); return w;
}
static string U8(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static bool readFile(const wstring& p, string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); out.assign((size_t)sz.QuadPart, 0); DWORD got = 0;
    bool ok = sz.QuadPart == 0 || ReadFile(h, &out[0], (DWORD)sz.QuadPart, &got, nullptr); out.resize(got); CloseHandle(h); return ok;
}
static bool readFileFrom(const wstring& p, long long off, string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz); out.clear();
    if (sz.QuadPart > off) { LARGE_INTEGER o; o.QuadPart = off; SetFilePointerEx(h, o, nullptr, FILE_BEGIN);
        out.assign((size_t)(sz.QuadPart - off), 0); DWORD got = 0; ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr); out.resize(got); }
    CloseHandle(h); return true;
}
static bool writeFile(const wstring& p, const string& s) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0; bool ok = s.empty() || WriteFile(h, s.data(), (DWORD)s.size(), &put, nullptr); CloseHandle(h); return ok && put == s.size();
}
static long long fileSize(const wstring& p) { WIN32_FILE_ATTRIBUTE_DATA a; if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return -1; return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow; }
static bool exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static string trimR(string s) { while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back(); return s; }
static string trim(string s) { s = trimR(s); size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; return s.substr(i); }
static wstring baseName(const wstring& p) { size_t k = p.find_last_of(L"\\/"); wstring b = k == wstring::npos ? p : p.substr(k + 1); size_t d = b.find_last_of(L'.'); return d == wstring::npos ? b : b.substr(0, d); }

// ------------------------------------------------------------------ level text format (same rules as plateau.exe)
static string expandRLE(const string& s) {
    string o; int n = 0; bool hn = false; size_t i = 0;
    while (i < s.size()) {
        char ch = s[i];
        if (isdigit((unsigned char)ch)) { n = n * 10 + (ch - '0'); hn = true; i++; continue; }
        if (ch == '(') { int depth = 1; size_t j = i + 1; while (j < s.size() && depth) { if (s[j] == '(') depth++; else if (s[j] == ')') depth--; j++; }
            string inner = expandRLE(s.substr(i + 1, j - i - 2)); int rep = hn ? n : 1; for (int k = 0; k < rep; k++) o += inner;
            n = 0; hn = false; i = j; continue; }
        int rep = hn ? n : 1; for (int k = 0; k < rep && o.size() < 50000000; k++) o += ch; n = 0; hn = false; i++;
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
static string solLetters(const string& l) { string s; for (char c : expandRLE(l)) if (strchr("lurdLURD", c)) s += c; return s; }

struct Board { int W = 0, H = 0; vector<char> wall, goal, box, inside; int player = -1; int nbox = 0, ngoal = 0; };
static bool makeBoard(const vector<string>& rows, Board& b) {
    b = Board(); b.H = rows.size(); for (auto& r : rows) b.W = max(b.W, (int)r.size());
    if (!b.W || !b.H) return false; int n = b.W * b.H;
    b.wall.assign(n, 0); b.goal.assign(n, 0); b.box.assign(n, 0); b.inside.assign(n, 0);
    for (int y = 0; y < b.H; y++) for (int x = 0; x < (int)rows[y].size(); x++) { char c = rows[y][x]; int i = y * b.W + x;
        if (c == '#') b.wall[i] = 1; if (c == '.' || c == '*' || c == '+') b.goal[i] = 1; if (c == '$' || c == '*') b.box[i] = 1; if (c == '@' || c == '+') b.player = i; }
    if (b.player < 0) return false;
    vector<int> q{b.player}; b.inside[b.player] = 1;
    for (size_t h = 0; h < q.size(); h++) { int c = q[h], x = c % b.W, y = c / b.W; const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
        for (int d = 0; d < 4; d++) { int nx = x + dx[d], ny = y + dy[d]; if (nx < 0 || ny < 0 || nx >= b.W || ny >= b.H) continue; int t = ny * b.W + nx;
            if (!b.inside[t] && !b.wall[t]) { b.inside[t] = 1; q.push_back(t); } } }
    for (int i = 0; i < n; i++) { if (!b.inside[i]) { b.box[i] = 0; b.goal[i] = 0; } b.nbox += b.box[i]; b.ngoal += b.goal[i]; }
    return b.nbox > 0;
}
static int dirOf(char c) { switch (c | 32) { case 'l': return 0; case 'r': return 1; case 'u': return 2; case 'd': return 3; } return -1; }
static const int DX[4] = {-1, 1, 0, 0}, DY[4] = {0, 0, -1, 1};
// exact replay, same rule as plateau.exe: every step stays on the floor, pushes need a free square, all boxes end on goals
static bool simulate(const Board& b, const string& s, int& moves, int& pushes, vector<char>* pf) {
    vector<char> bx = b.box; int p = b.player; moves = pushes = 0; if (pf) pf->assign(s.size(), 0);
    for (size_t i = 0; i < s.size(); i++) { int d = dirOf(s[i]); if (d < 0) return false;
        int x = p % b.W + DX[d], y = p / b.W + DY[d]; if (x < 0 || y < 0 || x >= b.W || y >= b.H) return false; int t = y * b.W + x; if (!b.inside[t]) return false;
        if (bx[t]) { int x2 = x + DX[d], y2 = y + DY[d]; if (x2 < 0 || y2 < 0 || x2 >= b.W || y2 >= b.H) return false; int t2 = y2 * b.W + x2;
            if (!b.inside[t2] || bx[t2]) return false; bx[t] = 0; bx[t2] = 1; pushes++; if (pf) (*pf)[i] = 1; }
        moves++; p = t; }
    for (size_t i = 0; i < bx.size(); i++) if (b.inside[i] && bx[i] != b.goal[i]) return false;
    return true;
}

// ------------------------------------------------------------------ levels
// use = ticked: the line is handed to plateau.exe as a starting line on the next run
struct Sol { string lurd; int moves = 0, pushes = 0; wstring src; int order = 0; bool use = true; };
static bool gTickNew = true; // tick lines found by runs (Settings: ping-pong)
struct Level {
    wstring id, title, name, folder; vector<string> rows; Board b; vector<Sol> sols; set<string> seen; int nextOrder = 0;
    set<string> deleted, ticked, unticked; // your choices, kept in results\<level>\gui_state.txt
    // 1 added, 0 already known (or deleted by you), -1 not a solution of this level. explicit: you added it yourself, so it comes back even if deleted
    int add(const string& lurd, const wstring& src, bool explicitAdd = false) {
        if (lurd.empty()) return -1; if (seen.count(lurd)) return 0;
        if (deleted.count(lurd)) { if (!explicitAdd) return 0; }
        Sol s; if (!simulate(b, lurd, s.moves, s.pushes, nullptr)) return -1;
        if (explicitAdd) deleted.erase(lurd);
        seen.insert(lurd); s.lurd = lurd; s.src = src; s.order = nextOrder++;
        s.use = ticked.count(lurd) ? true : unticked.count(lurd) ? false : (src == L"your input" || gTickNew);
        sols.push_back(s); return 1; }
    int nTicked() const { int n = 0; for (auto& s : sols) n += s.use; return n; }
};
typedef shared_ptr<Level> LevelP;
struct RawLevel { vector<string> rows; string title, notes; vector<vector<string>> groups; };
struct Parsed { vector<RawLevel> levels; vector<vector<string>> orphan; string orphanNotes; };

static bool keyLine(const string& s, string& key, string& val) {
    size_t c = s.find(':'); if (c == string::npos || c == 0 || c > 24) return false;
    for (size_t i = 0; i < c; i++) if (!(isalpha((unsigned char)s[i]) || s[i] == ' ' || s[i] == '-' || s[i] == '_')) return false;
    key = s.substr(0, c); for (auto& ch : key) ch = tolower((unsigned char)ch); val = trim(s.substr(c + 1)); return true;
}
static void parseText(const string& text, Parsed& R) {
    vector<string> lines; { string cur; for (char c : text) { if (c == '\n') { lines.push_back(trimR(cur)); cur.clear(); } else cur += c; } if (!cur.empty()) lines.push_back(trimR(cur)); }
    int cur = -1; bool inBoard = false, blankSince = false; string pending, pendingNotes; vector<string> run;
    auto flush = [&]() { if (run.empty()) return; if (cur >= 0) R.levels[cur].groups.push_back(run); else R.orphan.push_back(run); run.clear(); };
    auto mergeDup = [&]() { // a board just ended: if the same board appeared earlier in this text (a results file that repeats it), merge the two
        for (int k = 0; k < cur; k++) if (R.levels[k].rows == R.levels[cur].rows) { RawLevel& a = R.levels[k]; RawLevel& b = R.levels[cur];
            if (a.title.empty()) a.title = b.title; a.notes += b.notes; R.levels.pop_back(); cur = k; break; } };
    for (auto& raw : lines) {
        if (!raw.empty() && raw.size() < 100000) {
            string ex = expandRLE(raw); vector<string> parts; { string c; for (char ch : ex) { if (ch == '|') { parts.push_back(c); c.clear(); } else c += ch; } parts.push_back(c); }
            bool all = !ex.empty(); for (auto& p : parts) if (!isBoardLine(p)) all = false;
            if (all) { flush();
                if (!inBoard) { R.levels.push_back(RawLevel()); cur = R.levels.size() - 1; R.levels[cur].title = pending; R.levels[cur].notes = pendingNotes; pending.clear(); pendingNotes.clear(); inBoard = true; blankSince = false; }
                for (auto& p : parts) R.levels[cur].rows.push_back(p); continue; }
        }
        if (inBoard) mergeDup();
        inBoard = false; string t = trim(raw);
        bool sep = !t.empty(); for (char c : t) if (!strchr("-_=~ ", c)) sep = false;
        if (t.empty() || sep) { flush(); if (cur >= 0) blankSince = true; continue; }
        if (isSolLine(t)) { run.push_back(solLetters(t)); continue; }
        { size_t c = t.rfind(':'); if (c != string::npos && c < 60) { string after = trim(t.substr(c + 1)); if (after.size() >= 8 && isSolLine(after)) { run.push_back(solLetters(after)); continue; } } }
        flush();
        string s = t; while (!s.empty() && s[0] == ';') s = trim(s.substr(1)); if (s.empty()) continue;
        string key, val;
        if (keyLine(s, key, val)) {
            if (key == "title") { if (cur >= 0 && R.levels[cur].title.empty() && R.levels[cur].groups.empty()) { R.levels[cur].title = val; R.levels[cur].notes += s + "\n"; } else { pending = val; pendingNotes += s + "\n"; } }
            else if (cur >= 0) R.levels[cur].notes += s + "\n"; else pendingNotes += s + "\n";
            continue; }
        if (cur >= 0 && !blankSince && R.levels[cur].title.empty() && R.levels[cur].groups.empty()) { R.levels[cur].title = s; R.levels[cur].notes += s + "\n"; }
        else { pending = s; pendingNotes += s + "\n"; }
    }
    if (inBoard) mergeDup(); flush(); R.orphanNotes = pendingNotes;
}
// level ids: "LL27292", "letslogic 85687", "ID: 85683", or a file name that starts with the number
static wstring findId(const string& t0) {
    string t = trim(t0); { bool dig = t.size() >= 3 && t.size() <= 9; for (char c : t) if (!isdigit((unsigned char)c)) dig = false; if (dig) return W(t); }
    for (size_t i = 0; i + 2 < t.size(); i++) if (t[i] == 'L' && t[i + 1] == 'L' && isdigit((unsigned char)t[i + 2]) && (i == 0 || !isalnum((unsigned char)t[i - 1]))) {
        size_t j = i + 2; string d; while (j < t.size() && isdigit((unsigned char)t[j])) d += t[j++]; return W(d); }
    string lo = t; for (auto& c : lo) c = tolower((unsigned char)c);
    for (const char* kw : {"letslogic", "level id", "id", "level"}) { size_t k = 0;
        while ((k = lo.find(kw, k)) != string::npos) { bool startOk = k == 0 || !isalnum((unsigned char)lo[k - 1]); size_t j = k + strlen(kw); k = j; if (!startOk) continue;
            int skip = 0; while (j < lo.size() && !isdigit((unsigned char)lo[j]) && skip < 3 && strchr(" :#=.-", lo[j])) { j++; skip++; }
            string d; while (j < lo.size() && isdigit((unsigned char)lo[j])) d += lo[j++]; if (d.size() >= 2) return W(d); } }
    return L"";
}
static wstring idFromFileName(const wstring& base) { wstring d; for (wchar_t c : base) { if (iswdigit(c)) d += c; else break; } return d.size() >= 3 ? d : L""; }
static wstring safeName(wstring s) {
    for (auto& c : s) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back(); while (!s.empty() && s[0] == L' ') s.erase(0, 1);
    if (s.size() > 48) s = s.substr(0, 48); while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back(); return s;
}
static int addGroups(Level& L, const vector<vector<string>>& groups, const wstring& src, int* bad = nullptr, bool explicitAdd = false) {
    int added = 0;
    for (auto& g : groups) { int ok = 0;
        for (auto& l : g) { int r = L.add(l, src, explicitAdd); if (r >= 0) ok++; if (r > 0) added++; }
        if (ok == 0 && g.size() > 1) { string j; for (auto& l : g) j += l; int r = L.add(j, src, explicitAdd); if (r > 0) added++; if (r < 0 && bad) (*bad)++; }
        else if (bad) *bad += (int)g.size() - ok; }
    return added;
}

// ------------------------------------------------------------------ settings
struct Settings { int goal = 0, minutes = 120, threads = 4; wstring slack, seed, extra; bool lowPri = false, feed = true;
    // window line: wm 1 = range, 2 = S-curve (default), 3 = curve; lo/hi = range (and the S-curve's normal window); centre/sigma = curve
    int wm = 4, wlo = 20, whi = 60, wcenter = 40, wsigma = 15, logs0 = 1500, logw = 400, recal = 10, recalbud = 3, wrate = 5; bool wadapt = true, winv = true, recalimp = false; };   // default: Equal + Adaptive + Inverse, every 10 s
static Settings gSet; static wstring gExeDir, gIni, gEngine;
static int numCpus() { wchar_t b[32]; DWORD n = GetEnvironmentVariableW(L"NUMBER_OF_PROCESSORS", b, 32); int v = n ? _wtoi(b) : 0; if (v <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); v = si.dwNumberOfProcessors; } return max(1, v); }
static Settings defaultSettings() { Settings s; s.threads = numCpus(); return s; }
static wstring iniGet(const wchar_t* k, const wchar_t* d) { wchar_t b[2048]; GetPrivateProfileStringW(L"plateau", k, d, b, 2048, gIni.c_str()); return b; }
static void iniPut(const wchar_t* k, const wstring& v) { WritePrivateProfileStringW(L"plateau", k, v.c_str(), gIni.c_str()); }
static void loadSettings() { Settings d = defaultSettings(); gSet = d;
    gSet.goal = _wtoi(iniGet(L"goal", L"0").c_str()) ? 1 : 0; int m = _wtoi(iniGet(L"minutes", L"120").c_str()); gSet.minutes = m > 0 ? m : 120;
    int t = _wtoi(iniGet(L"threads", L"0").c_str()); gSet.threads = t > 0 ? t : d.threads;
    gSet.slack = iniGet(L"slack", L""); gSet.seed = iniGet(L"seed", L""); gSet.extra = iniGet(L"extra", L"");
    gSet.lowPri = _wtoi(iniGet(L"lowpriority", L"0").c_str()) != 0; gSet.feed = _wtoi(iniGet(L"feedback", L"1").c_str()) != 0; gTickNew = gSet.feed;
    auto gi = [&](const wchar_t* k, int dv, int lo, int hi) { int v = _wtoi(iniGet(k, to_wstring(dv).c_str()).c_str()); return max(lo, min(hi, v)); };
    gSet.wm = gi(L"windowmode", 4, 1, 4); gSet.wlo = gi(L"windowlo", 20, 1, 100000); gSet.whi = max(gSet.wlo, gi(L"windowhi", 60, 1, 100000));
    gSet.wcenter = gi(L"windowcentre", 40, 1, 100000); gSet.wsigma = gi(L"windowwidth", 15, 1, 100000); gSet.logs0 = gi(L"scurvemid", 1500, 0, 10000000); gSet.logw = gi(L"scurvewidth", 400, 1, 10000000);
    gSet.wadapt = gi(L"windowadaptive", 1, 0, 1) != 0; gSet.winv = gi(L"windowinverse", 1, 0, 1) != 0;
    gSet.recal = gi(L"remeasureevery", 10, 5, 100000); gSet.recalbud = gi(L"measuretime", 3, 1, 100000); gSet.wrate = gi(L"adaptspeed", 5, 1, 100); gSet.recalimp = gi(L"remeasureonimprovement", 0, 0, 1) != 0; }
static void saveSettings() { iniPut(L"goal", to_wstring(gSet.goal)); iniPut(L"minutes", to_wstring(gSet.minutes)); iniPut(L"threads", to_wstring(gSet.threads));
    iniPut(L"slack", gSet.slack); iniPut(L"seed", gSet.seed); iniPut(L"extra", gSet.extra); iniPut(L"lowpriority", gSet.lowPri ? L"1" : L"0"); iniPut(L"feedback", gSet.feed ? L"1" : L"0");
    iniPut(L"windowmode", to_wstring(gSet.wm)); iniPut(L"windowlo", to_wstring(gSet.wlo)); iniPut(L"windowhi", to_wstring(gSet.whi)); iniPut(L"windowcentre", to_wstring(gSet.wcenter));
    iniPut(L"windowwidth", to_wstring(gSet.wsigma)); iniPut(L"scurvemid", to_wstring(gSet.logs0)); iniPut(L"scurvewidth", to_wstring(gSet.logw));
    iniPut(L"windowadaptive", gSet.wadapt ? L"1" : L"0"); iniPut(L"windowinverse", gSet.winv ? L"1" : L"0");
    iniPut(L"remeasureevery", to_wstring(gSet.recal)); iniPut(L"measuretime", to_wstring(gSet.recalbud)); iniPut(L"adaptspeed", to_wstring(gSet.wrate)); iniPut(L"remeasureonimprovement", gSet.recalimp ? L"1" : L"0"); }
static const wchar_t* goalName(int g) { return g ? L"pushes" : L"moves"; }
// the arguments after the exe name; with default settings they are exactly the ones plateau.bat passes
static int runSeconds(const Settings& s) { int t = s.minutes * 60; size_t k = 0; wstring e = L" " + s.extra;
    while ((k = e.find(L" TIME=", k)) != wstring::npos) { k += 6; int v = _wtoi(e.c_str() + k); if (v > 0) t = v; } return t; }
static bool windowDefault(const Settings& s) { return s.wm == 2 && s.wlo == 20 && s.whi == 60 && s.logs0 == 1500 && s.logw == 400 && !s.wadapt && !s.winv; }
static const wchar_t* windowName(const Settings& s) { return s.wm == 1 ? L"Range" : s.wm == 3 ? L"Curve" : s.wm == 4 ? L"Equal" : L"S-Curve"; }
static wstring windowArgs(const Settings& s) {
    wstring a = wstring(L"WMODE=") + (s.wm == 1 ? L"range" : s.wm == 3 ? L"curve" : s.wm == 4 ? L"equal" : L"scurve") + L" WLO=" + to_wstring(s.wlo) + L" WHI=" + to_wstring(s.whi);
    bool measured = s.wm == 4 || (s.wm == 3 && s.wadapt);
    if (s.wm == 3) a += L" WCENTER=" + to_wstring(s.wcenter) + L" WSIGMA=" + to_wstring(s.wsigma);
    if (s.wm == 2 && (s.logs0 != 1500 || s.logw != 400)) a += L" LOGS0=" + to_wstring(s.logs0) + L" LOGW=" + to_wstring(s.logw);
    if (s.wm == 4 && !s.wadapt) a += L" WADAPT=0";
    if (measured) { a += L" WLSAVE=1"; if (s.wadapt && s.recalimp) a += L" RECAL=0 RECALBUD=" + to_wstring(s.recalbud);
        else if (s.wadapt && (s.recal != 150 || s.recalbud != 8)) a += L" RECAL=" + to_wstring(s.recal) + L" RECALBUD=" + to_wstring(s.recalbud); }
    if (s.wadapt) { if (s.wm != 4) a += L" WADAPT=1";
        if (s.wm != 3 && s.wrate != 5) { wchar_t b[32]; swprintf(b, 32, L" WRATE=%.2f", s.wrate / 100.0); a += b; } }
    if (s.winv) a += L" WINV=1"; return a; }
// odds of each window length 1..M as the engine computes them (plateau.cpp windowProbs), at 'stuck' steps without improvement
static wstring gWLLastPath; static bool gWLLastFound = false;
static vector<double> lastMeasured() { vector<double> q; gWLLastFound = false; if (gWLLastPath.empty()) return q;
    string t; long long off = 0; if (!readFileFrom(gWLLastPath, off, t)) return q; size_t k = t.find("curve"); if (k == string::npos) return q;
    k = t.find('\n', k); if (k == string::npos) return q; const char* c = t.c_str() + k + 1; int l; double w; int nn = 0;
    while (sscanf(c, "%d %lf%n", &l, &w, &nn) == 2) { if (l >= 1) { if ((int)q.size() <= l) q.resize(l + 1, 0.0); q[l] = w; } c += nn; }
    gWLLastFound = q.size() > 1; return q; }
static vector<double> windowOdds(const Settings& s, int whole, int stuck) {
    if (s.wm == 4) { vector<double> q = lastMeasured(); int lo0 = max(1, min(s.wlo, whole)), hi0 = max(lo0, min(s.whi, whole));
        vector<double> p;
        if (q.size() > 1) { int M = min<int>(whole, (int)q.size() - 1); if (s.winv) M = min(M, 300); p.assign(M + 1, 0.0);
            for (int w = 1; w <= M; w++) p[w] = s.winv ? q[w] : 1.0 / max(q[w], 1e-12); }
        else { p.assign(hi0 + 1, 0.0); for (int w = lo0; w <= hi0; w++) p[w] = 1; }
        double t = 0; for (size_t w = 1; w < p.size(); w++) t += p[w]; if (t > 0) for (size_t w = 1; w < p.size(); w++) p[w] /= t; return p; }
    int M = s.winv ? max(1, min(whole, 300)) : max(1, whole); vector<double> p(M + 1, 0.0);
    int lo = max(1, min(s.wlo, M)), hi = max(lo, min(s.whi, M));
    if (s.wm == 1) { for (int w = lo; w <= hi; w++) p[w] = 1; }
    else if (s.wm == 2) { double f = 1.0 / (1.0 + exp(-((double)stuck - s.logs0) / max(1, s.logw)));
        for (int w = lo; w <= hi; w++) p[w] += (1 - f) / (hi - lo + 1);
        int bot = max(hi, 60), top = max(bot, whole); double z = 0; for (int w = bot; w <= top; w++) z += 1.0 / w;
        if (z > 0) for (int w = bot; w <= min(top, M); w++) p[w] += f * (1.0 / w) / z; }
    else { for (int w = 1; w <= M; w++) p[w] = exp(-(double)(w - s.wcenter) * (w - s.wcenter) / (2.0 * s.wsigma * s.wsigma)); }
    if (s.winv) { double mx = 0; for (int w = 1; w <= M; w++) mx = max(mx, p[w]); bool any = false; for (int w = 1; w <= M; w++) { p[w] = mx - p[w]; if (p[w] > 1e-15) any = true; } if (!any) for (int w = 1; w <= M; w++) p[w] = 1; }
    double t = 0; for (int w = 1; w <= M; w++) t += p[w]; if (t > 0) for (int w = 1; w <= M; w++) p[w] /= t; return p; }
static wstring engineArgs(const Settings& s) {
    wstring g = goalName(s.goal);
    wstring a = L"input.txt P=" + to_wstring(s.goal) + L" TIME=" + to_wstring((long long)s.minutes * 60) + L" THREADS=" + to_wstring(s.threads) + L" OUT=best_" + g + L".txt LOG=log_" + g + L".txt";
    if (!s.slack.empty()) a += L" SLACK=" + s.slack; if (!s.seed.empty()) a += L" SEED=" + s.seed; { wstring w = windowArgs(s); if (!w.empty()) a += L" " + w; } if (!s.extra.empty()) a += L" " + s.extra;
    return a;
}

// ------------------------------------------------------------------ globals
enum { ID_OPEN_FILE = 2001, ID_PASTE, ID_ADDSOLS, ID_SAVE, ID_FOLDER, ID_COPY, ID_BTN_OPEN, ID_BTN_SETTINGS, ID_BTN_RUN, ID_BTN_RUNALL, ID_BTN_STOP,
       ID_COMBO, ID_LIST, ID_BTN_COPY, ID_BTN_SAVE, ID_P_START, ID_P_BACK, ID_P_PLAY, ID_P_FWD, ID_P_END, ID_POS, ID_SPEED, ID_LOG, ID_STATUS, ID_RENAME, ID_BTN_DELETE, ID_RUNONLY, ID_TICKALL, ID_UNTICKALL, ID_TICKBEST };
#define WM_APP_LOG  (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)
static HINSTANCE gInst; static HWND gBtnDelete; static HWND gMain, gBtnOpen, gBtnSettings, gBtnRun, gBtnRunAll, gBtnStop, gGoalLbl, gLevelLbl, gCombo, gSolLbl, gList, gBtnCopy, gBtnSave,
    gBoard, gBtnStart, gBtnBack, gBtnPlay, gBtnFwd, gBtnEnd, gPos, gPosLbl, gSpeed, gSpeedLbl, gLogLbl, gLog, gStatus;
static HFONT gFont, gFontBold, gMono; static double gS = 1.0;
static int S(int v) { return (int)lround(v * gS); }
static vector<LevelP> gLevels; static int gCur = -1; static wstring gSourceName;
static int gSortCol = 0; static bool gFilling = false;

struct Player { LevelP lvl; int sol = -1; string lurd; vector<char> pf; vector<char> boxes; int ppos = -1, step = 0; bool playing = false; double acc = 0; ULONGLONG last = 0; } P;
struct Run { bool active = false, stop = false; HANDLE proc = nullptr, job = nullptr, thread = nullptr; LevelP lvl; int goal = 0, seconds = 0; ULONGLONG t0 = 0;
    wstring outPath; long long outOff = 0; string lastHeader, partial; } gRun;
static vector<LevelP> gQueue;

static LevelP curLevel() { return gCur >= 0 && gCur < (int)gLevels.size() ? gLevels[gCur] : nullptr; }
static wstring fmtTime(long long s) { wchar_t b[32]; if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60); else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60); return b; }
static const Sol* bestSol(const Level& L, int goal) { const Sol* b = nullptr;
    for (auto& s : L.sols) if (!b || (goal ? make_pair(s.pushes, s.moves) < make_pair(b->pushes, b->moves) : make_pair(s.moves, s.pushes) < make_pair(b->moves, b->pushes))) b = &s; return b; }

// ------------------------------------------------------------------ log pane
static void logAppend(const string& s8) {
    wstring w = W(s8), o; o.reserve(w.size() + 64); for (size_t i = 0; i < w.size(); i++) { if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) o += L'\r'; o += w[i]; }
    int len = GetWindowTextLengthW(gLog);
    if (len > 600000) { SendMessageW(gLog, EM_SETSEL, 0, 300000); SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)L""); len = GetWindowTextLengthW(gLog); }
    SendMessageW(gLog, EM_SETSEL, len, len); SendMessageW(gLog, EM_REPLACESEL, FALSE, (LPARAM)o.c_str());
}
static void logLine(const wstring& w) { logAppend(U8(w) + "\n"); }

// ------------------------------------------------------------------ replay player
static void playerReset() { if (!P.lvl) return; P.boxes = P.lvl->b.box; P.ppos = P.lvl->b.player; P.step = 0; }
static void stepFwd() { if (!P.lvl || P.step >= (int)P.lurd.size()) return; const Board& b = P.lvl->b; int d = dirOf(P.lurd[P.step]); int off = DX[d] + DY[d] * b.W; int t = P.ppos + off;
    if (P.pf[P.step]) { P.boxes[t] = 0; P.boxes[t + off] = 1; } P.ppos = t; P.step++; }
static void stepBack() { if (!P.lvl || P.step <= 0) return; const Board& b = P.lvl->b; int i = P.step - 1; int d = dirOf(P.lurd[i]); int off = DX[d] + DY[d] * b.W;
    if (P.pf[i]) { P.boxes[P.ppos + off] = 0; P.boxes[P.ppos] = 1; } P.ppos -= off; P.step--; }
static void updatePlayerUi() {
    int n = P.lurd.size(); int pushes = 0, totalP = 0; for (int i = 0; i < n; i++) { if (P.pf[i]) { totalP++; if (i < P.step) pushes++; } }
    SendMessageW(gPos, TBM_SETRANGEMAX, TRUE, max(1, n)); SendMessageW(gPos, TBM_SETPOS, TRUE, P.step);
    wchar_t b[160]; if (P.sol >= 0) swprintf(b, 160, L"Move %d of %d    Push %d of %d", P.step, n, pushes, totalP); else wcscpy(b, P.lvl ? L"Pick a solution on the left to replay it" : L"");
    SetWindowTextW(gPosLbl, b); SetWindowTextW(gBtnPlay, P.playing ? L"Pause" : L"Play");
    BOOL en = P.sol >= 0; for (HWND h : {gBtnStart, gBtnBack, gBtnPlay, gBtnFwd, gBtnEnd, gPos}) EnableWindow(h, en);
    InvalidateRect(gBoard, nullptr, FALSE);
}
static void setPlaying(bool on) { if (on && P.sol < 0) on = false; if (on && P.step >= (int)P.lurd.size()) playerReset();
    P.playing = on; P.acc = 0; P.last = GetTickCount64(); if (on) SetTimer(gMain, 2, 15, nullptr); else KillTimer(gMain, 2); updatePlayerUi(); }
static void seek(int k) { k = max(0, min(k, (int)P.lurd.size())); if (k < P.step && P.step - k > k) playerReset(); while (P.step < k) stepFwd(); while (P.step > k) stepBack(); updatePlayerUi(); }
static void loadSolution(int idx) { LevelP L = curLevel(); P.lvl = L; P.sol = -1; P.lurd.clear(); P.pf.clear();
    if (L && idx >= 0 && idx < (int)L->sols.size()) { int m, p; P.lurd = L->sols[idx].lurd; if (simulate(L->b, P.lurd, m, p, &P.pf)) P.sol = idx; else P.lurd.clear(); }
    playerReset(); setPlaying(false); }
static double speedMps() { int pos = (int)SendMessageW(gSpeed, TBM_GETPOS, 0, 0); return pow(10.0, pos / 100.0 * 3.0); }
static void updateSpeedLbl() { wchar_t b[64]; double v = speedMps(); swprintf(b, 64, v < 10 ? L"%.1f moves/s" : L"%.0f moves/s", v); SetWindowTextW(gSpeedLbl, b); }

// ------------------------------------------------------------------ board drawing
static void fillRect(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) { RECT r{x0, y0, x1, y1}; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }
static void shape(HDC dc, int x0, int y0, int x1, int y1, COLORREF fill, COLORREF edge, bool ellipse, int round, int pen) {
    HBRUSH b = CreateSolidBrush(fill); HPEN p = CreatePen(PS_SOLID, max(1, pen), edge); HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    if (ellipse) Ellipse(dc, x0, y0, x1, y1); else RoundRect(dc, x0, y0, x1, y1, round, round);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p); }
static void paintBoard(HDC dc, const RECT& rc) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    fillRect(dc, 0, 0, w, h, RGB(38, 42, 48));
    LevelP L = P.lvl;
    if (!L) return;
    const Board& b = L->b; int pad = S(12);
    int cell = max(2, min((w - 2 * pad) / b.W, (h - 2 * pad) / b.H)); cell = min(cell, S(64));
    int ox = (w - cell * b.W) / 2, oy = (h - cell * b.H) / 2;
    const vector<char>& bx = P.boxes.empty() ? b.box : P.boxes; int pp = P.ppos >= 0 ? P.ppos : b.player;
    for (int y = 0; y < b.H; y++) for (int x = 0; x < b.W; x++) { int i = y * b.W + x; int X = ox + x * cell, Y = oy + y * cell;
        if (b.wall[i]) { fillRect(dc, X, Y, X + cell, Y + cell, RGB(112, 98, 88)); fillRect(dc, X, Y, X + cell, Y + max(1, cell / 8), RGB(138, 122, 110)); continue; }
        if (!b.inside[i]) continue;
        fillRect(dc, X, Y, X + cell, Y + cell, ((x + y) & 1) ? RGB(222, 214, 196) : RGB(228, 221, 204));
        if (b.goal[i] && !bx[i]) { int r = max(2, cell * 22 / 100); shape(dc, X + cell / 2 - r, Y + cell / 2 - r, X + cell / 2 + r, Y + cell / 2 + r, RGB(214, 92, 72), RGB(170, 64, 50), true, 0, 1); }
        if (bx[i]) { int m = max(1, cell / 9); bool on = b.goal[i];
            shape(dc, X + m, Y + m, X + cell - m, Y + cell - m, on ? RGB(98, 172, 102) : RGB(214, 150, 66), on ? RGB(58, 122, 64) : RGB(160, 104, 36), false, cell / 4, max(1, cell / 16));
            if (cell >= 14) { int k = cell * 30 / 100; HPEN p = CreatePen(PS_SOLID, max(1, cell / 18), on ? RGB(58, 122, 64) : RGB(160, 104, 36)); HGDIOBJ op = SelectObject(dc, p);
                MoveToEx(dc, X + k, Y + k, nullptr); LineTo(dc, X + cell - k, Y + cell - k); MoveToEx(dc, X + cell - k, Y + k, nullptr); LineTo(dc, X + k, Y + cell - k);
                SelectObject(dc, op); DeleteObject(p); } }
        if (i == pp) { int m = max(1, cell * 18 / 100); shape(dc, X + m, Y + m, X + cell - m, Y + cell - m, RGB(64, 120, 220), RGB(30, 70, 160), true, 0, max(1, cell / 16));
            if (b.goal[i]) { int r = max(1, cell / 10); shape(dc, X + cell / 2 - r, Y + cell / 2 - r, X + cell / 2 + r, Y + cell / 2 + r, RGB(214, 92, 72), RGB(214, 92, 72), true, 0, 1); } }
    }
}
static LRESULT CALLBACK BoardProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc); HBITMAP bm = CreateCompatibleBitmap(dc, max(1L, rc.right), max(1L, rc.bottom)); HGDIOBJ ob = SelectObject(mem, bm);
        paintBoard(mem, rc);
        if (!P.lvl) { SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(200, 204, 210)); HGDIOBJ of = SelectObject(mem, gFont); RECT r = rc; r.top = rc.bottom / 2 - S(18);
            DrawTextW(mem, L"Open a level or a collection with Open, or drop a file here.\nPlateau improves the solutions you give it.", -1, &r, DT_CENTER | DT_WORDBREAK); SelectObject(mem, of); }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY); SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem); EndPaint(h, &ps); return 0; }
    case WM_LBUTTONDOWN: SetFocus(h); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ------------------------------------------------------------------ sidebar
static void refreshCombo() {
    SendMessageW(gCombo, CB_RESETCONTENT, 0, 0);
    for (auto& L : gLevels) { wstring s = L->name; if (!L->id.empty() && !L->title.empty() && L->title.find(L->id) == wstring::npos) s += L"    " + L->title; else if (!L->id.empty() && !L->title.empty()) s += L"    " + L->title;
        if (!L->sols.empty()) { const Sol* b = bestSol(*L, gSet.goal); wchar_t t[64]; swprintf(t, 64, L"    (%d/%d)", b->moves, b->pushes); s += t; }
        SendMessageW(gCombo, CB_ADDSTRING, 0, (LPARAM)s.c_str()); }
    if (gCur >= 0) SendMessageW(gCombo, CB_SETCURSEL, gCur, 0);
    wchar_t b[64]; swprintf(b, 64, gLevels.size() > 1 ? L"Level (%d in this collection)" : L"Level", (int)gLevels.size()); SetWindowTextW(gLevelLbl, b);
}
static void refreshList(bool keepSel) {
    LevelP L = curLevel(); gFilling = true; int top = keepSel ? ListView_GetTopIndex(gList) : 0; SendMessageW(gList, WM_SETREDRAW, FALSE, 0); ListView_DeleteAllItems(gList);
    if (L) { vector<int> ord(L->sols.size()); for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
        auto& s = L->sols;
        sort(ord.begin(), ord.end(), [&](int a, int b) {
            if (gSortCol == 0) return make_tuple(s[a].moves, s[a].pushes, s[a].order) < make_tuple(s[b].moves, s[b].pushes, s[b].order);
            if (gSortCol == 1) return make_tuple(s[a].pushes, s[a].moves, s[a].order) < make_tuple(s[b].pushes, s[b].moves, s[b].order);
            return s[a].order > s[b].order; });
        int row = 0;
        for (int i : ord) { wchar_t m[16], p[16]; swprintf(m, 16, L"%d", s[i].moves); swprintf(p, 16, L"%d", s[i].pushes);
            LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.pszText = m; it.lParam = i; ListView_InsertItem(gList, &it);
            ListView_SetItemText(gList, row, 1, p); ListView_SetItemText(gList, row, 2, (LPWSTR)s[i].src.c_str()); ListView_SetCheckState(gList, row, s[i].use);
            if (keepSel && P.lvl == L && i == P.sol) ListView_SetItemState(gList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            row++; }
        if (top > 0 && row > 0) { RECT ir; if (ListView_GetItemRect(gList, 0, &ir, LVIR_BOUNDS)) ListView_Scroll(gList, 0, top * (ir.bottom - ir.top)); }
        wchar_t b[128]; swprintf(b, 128, L"Solutions (%d, %d ticked to start from)", (int)s.size(), L->nTicked()); SetWindowTextW(gSolLbl, b);
    } else SetWindowTextW(gSolLbl, L"Solutions");
    SendMessageW(gList, WM_SETREDRAW, TRUE, 0); InvalidateRect(gList, nullptr, TRUE); gFilling = false;
    BOOL has = L && !L->sols.empty(); EnableWindow(gBtnCopy, has); EnableWindow(gBtnDelete, has); EnableWindow(gBtnSave, L != nullptr);
}
static void selectLevel(int i) {
    gCur = i; SendMessageW(gCombo, CB_SETCURSEL, i, 0); refreshList(false);
    LevelP L = curLevel(); P.lvl = L; P.sol = -1; P.lurd.clear(); P.pf.clear(); playerReset(); setPlaying(false);
    if (L && !L->sols.empty()) { ListView_SetItemState(gList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); }
    wstring t = L"Plateau"; if (L) t = L->name + L" - Plateau"; SetWindowTextW(gMain, t.c_str());
}

// ------------------------------------------------------------------ status / buttons
static void updateButtons() {
    LevelP L = curLevel(); EnableWindow(gBtnRun, !gRun.active && L && L->nTicked() > 0); EnableWindow(gBtnRunAll, !gRun.active && gLevels.size() > 1);
    EnableWindow(gBtnStop, gRun.active);
    wchar_t b[200]; int rs = runSeconds(gSet); swprintf(b, 200, L"Goal: fewest %ls    %ls per level    %d threads    Window line: %ls%ls%ls", goalName(gSet.goal), (rs % 60 ? fmtTime(rs) : to_wstring(rs / 60) + L" min").c_str(), gSet.threads,
        windowName(gSet), gSet.wadapt ? L", adaptive" : L"", gSet.winv ? L", inverse" : L""); SetWindowTextW(gGoalLbl, b);
}
static void updateStatus() {
    wstring s0, s1;
    if (gRun.active) { long long el = (GetTickCount64() - gRun.t0) / 1000; s0 = L"Running " + gRun.lvl->name + L" (fewest " + goalName(gRun.goal) + L"): " + fmtTime(el) + L" of " + fmtTime(gRun.seconds);
        if (!gQueue.empty()) s0 += L",  " + to_wstring(gQueue.size()) + L" more level(s) queued";
        const Sol* b = bestSol(*gRun.lvl, gRun.goal); if (b) { wchar_t t[80]; swprintf(t, 80, L"    best %d/%d", b->moves, b->pushes); s0 += t; } }
    else s0 = gLevels.empty() ? L"No level open" : L"Ready";
    LevelP L = curLevel(); if (L) { const Sol* b = bestSol(*L, gSet.goal); wchar_t t[160]; if (b) swprintf(t, 160, L"%ls: %dx%d, %d boxes, best %d moves / %d pushes", L->name.c_str(), L->b.W, L->b.H, L->b.nbox, b->moves, b->pushes);
        else swprintf(t, 160, L"%ls: %dx%d, %d boxes, no solution yet (Plateau needs one to start from)", L->name.c_str(), L->b.W, L->b.H, L->b.nbox); s1 = t;
        if (b && L->nTicked() == 0) s1 += L"    Tick a solution to start a run from it"; }
    SendMessageW(gStatus, SB_SETTEXTW, 0, (LPARAM)s0.c_str()); SendMessageW(gStatus, SB_SETTEXTW, 1, (LPARAM)s1.c_str());
}

// ------------------------------------------------------------------ results files written by plateau.exe
static wstring tagOf(const string& header, int goal) {
    string t; size_t a = header.find('('), b = header.rfind(')'); if (a != string::npos && b != string::npos && b > a) t = header.substr(a + 1, b - a - 1);
    if (t.rfind("Plateau ", 0) == 0) t = t.substr(8); size_t v = t.find(", verified"); if (v != string::npos) t = t.substr(0, v);
    if (t.empty()) t = "line"; return wstring(goalName(goal)) + L" run, " + W(t);
}
static int parseOutText(Level& L, const string& text, int goal, string& lastHeader, const wstring& when) {
    int added = 0; size_t i = 0;
    while (i < text.size()) { size_t e = text.find('\n', i); if (e == string::npos) e = text.size(); string l = trim(text.substr(i, e - i)); i = e + 1;
        if (l.rfind("Solution:", 0) == 0) { lastHeader = l; continue; }
        if (isSolLine(l)) { if (L.add(solLetters(l), tagOf(lastHeader, goal) + when) > 0) added++; } }
    return added;
}
static void loadEarlierResults(Level& L) {
    for (int g = 0; g < 2; g++) { string t, h; if (readFile(L.folder + L"\\best_" + goalName(g) + L".txt", t)) parseOutText(L, t, g, h, L"  (earlier)"); }
}
// your choices for a level's lines: D = deleted from the list, T = ticked, U = unticked (results\<level>\gui_state.txt)
static void loadState(Level& L) {
    L.deleted.clear(); L.ticked.clear(); L.unticked.clear(); string t; if (!readFile(L.folder + L"\\gui_state.txt", t)) return;
    size_t i = 0; while (i < t.size()) { size_t e = t.find('\n', i); if (e == string::npos) e = t.size(); string l = trim(t.substr(i, e - i)); i = e + 1;
        if (l.size() < 3 || l[1] != ' ') continue; string x = l.substr(2);
        if (l[0] == 'D') L.deleted.insert(x); else if (l[0] == 'T') L.ticked.insert(x); else if (l[0] == 'U') L.unticked.insert(x); }
}
static void saveState(const Level& L) {
    string t; for (auto& x : L.deleted) t += "D " + x + "\n"; for (auto& x : L.ticked) t += "T " + x + "\n"; for (auto& x : L.unticked) t += "U " + x + "\n";
    CreateDirectoryW((L.folder.substr(0, L.folder.find_last_of(L'\\'))).c_str(), nullptr); CreateDirectoryW(L.folder.c_str(), nullptr);
    writeFile(L.folder + L"\\gui_state.txt", t);
}
static bool sameBoardInFolder(const wstring& folder, const vector<string>& rows) {
    string t; if (!readFile(folder + L"\\input.txt", t)) return true; Parsed R; parseText(t, R); return R.levels.empty() || R.levels[0].rows == rows;
}

// ------------------------------------------------------------------ opening levels and solutions
static wstring resultsDir() { return gExeDir + L"\\results"; }
// names you gave levels, remembered by board (plateau_gui.ini, section [names])
static wstring boardKey(const vector<string>& rows) { unsigned long long h = 1469598103934665603ULL; for (auto& r : rows) { for (char c : r) { h ^= (unsigned char)c; h *= 1099511628211ULL; } h ^= 10; h *= 1099511628211ULL; }
    wchar_t b[24]; swprintf(b, 24, L"%016llx", h); return b; }
static wstring savedName(const vector<string>& rows) { wchar_t b[256]; GetPrivateProfileStringW(L"names", boardKey(rows).c_str(), L"", b, 256, gIni.c_str()); return b; }
// give a level a new name (an id if it is a number); its results folder follows the name
static bool renameLevel(LevelP L, const wstring& nm0) {
    wstring nm = nm0; while (!nm.empty() && nm.back() == L' ') nm.pop_back(); while (!nm.empty() && nm[0] == L' ') nm.erase(0, 1);
    wstring fn = safeName(nm); if (fn.empty() || (gRun.active && gRun.lvl == L)) return false;
    bool digits = true; for (wchar_t c : nm) if (!iswdigit(c)) digits = false;
    wstring f = resultsDir() + L"\\" + fn; int sfx = 2;
    auto taken = [&](const wstring& x) { for (auto& o : gLevels) if (o != L && o->folder == x) return true; return !sameBoardInFolder(x, L->rows); };
    while (taken(f)) f = resultsDir() + L"\\" + fn + L"_" + to_wstring(sfx++);
    L->name = nm; if (digits) L->id = nm; L->folder = f; { Level tmp; tmp.folder = f; loadState(tmp); L->deleted.insert(tmp.deleted.begin(), tmp.deleted.end()); } saveState(*L); loadEarlierResults(*L);
    WritePrivateProfileStringW(L"names", boardKey(L->rows).c_str(), nm.c_str(), gIni.c_str()); return true;
}
static int openText(const string& text, const wstring& sourceName, bool solutionsOnly, wstring& msg) {
    Parsed R; parseText(text, R);
    LevelP C = curLevel();
    if (solutionsOnly || R.levels.empty()) {
        if (!C) { msg = L"No level found in " + sourceName + L". Open a level first, then add solution lines to it."; return -1; }
        vector<vector<string>> all = R.orphan; for (auto& l : R.levels) for (auto& g : l.groups) all.push_back(g);
        int total = 0; for (auto& g : all) total += g.size(); int bad = 0; int added = addGroups(*C, all, L"your input", &bad, true); saveState(*C);
        if (total == 0) { msg = L"No level and no solution lines found in " + sourceName + L"."; return -1; }
        msg = L"Added " + to_wstring(added) + L" new solution(s) to " + C->name + L"."; if (bad) msg += L" " + to_wstring(bad) + L" line(s) are not solutions of this level and were skipped.";
        if (C->id.empty() && bad < total) { // the solutions file knows the level's id (e.g. 85687_624_116.txt): name the level by it
            wstring id = idFromFileName(sourceName); if (id.empty()) id = findId(R.orphanNotes);
            if (!id.empty() && renameLevel(C, id)) msg += L" The level is now called " + id + L" (from " + sourceName + L").";
        }
        refreshList(true); refreshCombo(); updateButtons(); updateStatus(); return added; }
    vector<LevelP> out; set<wstring> used; int skipped = 0;
    for (size_t k = 0; k < R.levels.size(); k++) { RawLevel& r = R.levels[k]; auto L = make_shared<Level>(); L->rows = r.rows;
        if (!makeBoard(r.rows, L->b)) { skipped++; continue; }
        L->title = W(r.title); L->id = findId(r.title); if (L->id.empty()) L->id = findId(r.notes);
        if (L->id.empty() && R.levels.size() == 1) { L->id = idFromFileName(sourceName); if (L->id.empty()) L->id = findId(R.orphanNotes); }
        if (L->id.empty() && R.levels.size() == 1) L->id = findId(U8(sourceName));
        wstring nm = !L->id.empty() ? L->id : !L->title.empty() ? safeName(L->title) : L"";
        if (nm.empty()) nm = (R.levels.size() > 1 ? safeName(sourceName) + L" " : wstring(L"Level ")) + to_wstring(k + 1);
        { wstring saved = savedName(L->rows); if (!saved.empty()) { nm = saved; bool dg = true; for (wchar_t c : saved) if (!iswdigit(c)) dg = false; if (dg) L->id = saved; } } // a name you gave this board wins
        L->name = nm; wstring fn = safeName(nm); if (fn.empty()) fn = L"level";
        wstring f = resultsDir() + L"\\" + fn; int sfx = 2; while (used.count(f) || !sameBoardInFolder(f, r.rows)) f = resultsDir() + L"\\" + fn + L"_" + to_wstring(sfx++);
        used.insert(f); L->folder = f; loadState(*L);
        if (gRun.active && gRun.lvl && gRun.lvl->folder == f && gRun.lvl->rows == r.rows) L = gRun.lvl; // same level as the one running: share it
        addGroups(*L, r.groups, L"your input");
        if (k == 0 && R.levels.size() == 1) addGroups(*L, R.orphan, L"your input");
        loadEarlierResults(*L);
        out.push_back(L); }
    if (out.empty()) { msg = L"No usable level found in " + sourceName + L" (a level needs walls, a player and at least one box)."; return -1; }
    gLevels = out; gSourceName = sourceName; gQueue.clear(); refreshCombo(); selectLevel(0);
    int nsol = 0; for (auto& L : out) nsol += L->sols.size();
    msg = L"Opened " + sourceName + L": " + to_wstring(out.size()) + L" level(s), " + to_wstring(nsol) + L" valid solution(s).";
    if (skipped) msg += L" " + to_wstring(skipped) + L" board(s) could not be read.";
    updateButtons(); updateStatus(); return (int)out.size();
}
static bool openFile(const wstring& path, bool solutionsOnly) {
    string t; if (!readFile(path, t)) { MessageBoxW(gMain, (L"Could not read " + path).c_str(), L"Plateau", MB_ICONERROR); return false; }
    if (t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t = t.substr(3);
    if (t.size() >= 2 && (unsigned char)t[0] == 0xFF && (unsigned char)t[1] == 0xFE) { wstring w((const wchar_t*)(t.data() + 2), (t.size() - 2) / 2); t = U8(w); }
    wstring msg; int r = openText(t, baseName(path), solutionsOnly, msg); logLine(msg);
    if (r < 0) { MessageBoxW(gMain, msg.c_str(), L"Plateau", MB_ICONWARNING); return false; }
    if (!solutionsOnly && r > 0 && gCur >= 0 && gLevels.size() > 0 && !gLevels.empty()) iniPut(L"lastfile", path);
    return true;
}
static wstring pickFile(bool save, const wstring& suggest = L"") {
    wchar_t buf[4096] = {0}; if (!suggest.empty()) wcsncpy(buf, suggest.c_str(), 4095);
    OPENFILENAMEW o{}; o.lStructSize = sizeof o; o.hwndOwner = gMain; o.lpstrFile = buf; o.nMaxFile = 4096;
    o.lpstrFilter = L"Sokoban levels and solutions (*.txt;*.sok;*.xsb)\0*.txt;*.sok;*.xsb\0All files\0*.*\0"; o.lpstrDefExt = L"txt";
    o.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    if (save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o)) return buf; return L"";
}
static string levelText(const Level& L, bool allSols) {
    string t; for (auto& r : L.rows) t += r + "\n"; if (!L.title.empty()) t += "Title: " + U8(L.title) + "\n"; else if (!L.id.empty()) t += "Title: " + U8(L.id) + "\n";
    if (allSols) { vector<const Sol*> v; for (auto& s : L.sols) v.push_back(&s); sort(v.begin(), v.end(), [](const Sol* a, const Sol* b) { return make_pair(a->moves, a->pushes) < make_pair(b->moves, b->pushes); });
        for (auto* s : v) t += "\nSolution: " + to_string(s->moves) + " moves, " + to_string(s->pushes) + " pushes (" + U8(s->src) + ")\n" + s->lurd + "\n"; }
    return t;
}

// ------------------------------------------------------------------ running plateau.exe
// input.txt for plateau.exe: the board, its title, then the ticked solution lines (your lines first, in file order)
static string runInput(const Level& L) {
    string t; for (auto& r : L.rows) t += r + "\n"; if (!L.title.empty()) t += "Title: " + U8(L.title) + "\n"; t += "\n";
    for (auto& s : L.sols) if (s.use) t += s.lurd + "\n";
    return t;
}
static DWORD WINAPI readerThread(LPVOID p) {
    HANDLE rd = (HANDLE)p; char buf[4096]; DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) PostMessageW(gMain, WM_APP_LOG, 0, (LPARAM)new string(buf, n));
    CloseHandle(rd); PostMessageW(gMain, WM_APP_DONE, 0, 0); return 0;
}
static void tailOut() {
    if (!gRun.lvl) return; string t; if (!readFileFrom(gRun.outPath, gRun.outOff, t) || t.empty()) return;
    size_t e = t.rfind('\n'); if (e == string::npos) return; t = t.substr(0, e + 1); gRun.outOff += t.size();
    SYSTEMTIME st; GetLocalTime(&st); wchar_t w[32]; swprintf(w, 32, L"  (%02d:%02d)", st.wHour, st.wMinute);
    int added = parseOutText(*gRun.lvl, t, gRun.goal, gRun.lastHeader, w);
    if (added) { if (curLevel() == gRun.lvl) refreshList(true); refreshCombo(); updateStatus(); }
}
static bool startRun(LevelP L) {
    if (!L || L->nTicked() == 0) return false;
    if (!exists(gEngine)) { MessageBoxW(gMain, (L"plateau.exe was not found next to this program:\n" + gEngine).c_str(), L"Plateau", MB_ICONERROR); return false; }
    CreateDirectoryW(resultsDir().c_str(), nullptr); CreateDirectoryW(L->folder.c_str(), nullptr);
    { string t = runInput(*L);
      if (!writeFile(L->folder + L"\\input.txt", t)) { MessageBoxW(gMain, (L"Could not write " + L->folder + L"\\input.txt").c_str(), L"Plateau", MB_ICONERROR); return false; } }
    gRun = Run(); gRun.lvl = L; gRun.goal = gSet.goal; gRun.seconds = runSeconds(gSet); gRun.outPath = L->folder + L"\\best_" + goalName(gSet.goal) + L".txt";
    gRun.outOff = max(0LL, fileSize(gRun.outPath));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE}; HANDLE rd, wr; if (!CreatePipe(&rd, &wr, &sa, 0)) return false; SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
    wstring cmd = L"\"" + gEngine + L"\" " + engineArgs(gSet); vector<wchar_t> cb(cmd.begin(), cmd.end()); cb.push_back(0);
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (gSet.lowPri ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    BOOL ok = CreateProcessW(gEngine.c_str(), cb.data(), nullptr, nullptr, TRUE, flags, nullptr, L->folder.c_str(), &si, &pi);
    CloseHandle(wr); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); MessageBoxW(gMain, L"Could not start plateau.exe.", L"Plateau", MB_ICONERROR); return false; }
    gRun.job = CreateJobObjectW(nullptr, nullptr);
    if (gRun.job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(gRun.job, JobObjectExtendedLimitInformation, &li, sizeof li); AssignProcessToJobObject(gRun.job, pi.hProcess); }
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); gRun.proc = pi.hProcess; gRun.active = true; gRun.t0 = GetTickCount64();
    gRun.thread = CreateThread(nullptr, 0, readerThread, rd, 0, nullptr);
    logLine(L"");
    logLine(L"=== " + L->name + L": fewest " + goalName(gRun.goal) + L", " + (runSeconds(gSet) % 60 ? fmtTime(runSeconds(gSet)) : to_wstring(runSeconds(gSet) / 60) + L" min") + L", " + to_wstring(gSet.threads) + L" threads. Results in " + L->folder);
    logLine(L"plateau.exe " + engineArgs(gSet));
    updateButtons(); updateStatus(); return true;
}
static void stopRun() { if (!gRun.active) return; gRun.stop = true; gQueue.clear(); if (gRun.job) TerminateJobObject(gRun.job, 1); else TerminateProcess(gRun.proc, 1); }
static void startNextQueued() { while (!gQueue.empty()) { LevelP L = gQueue.front(); gQueue.erase(gQueue.begin()); if (L->nTicked() == 0) { logLine(L->name + L": skipped, no solution is ticked to start from."); continue; } if (startRun(L)) return; gQueue.clear(); } }
static void runDone() {
    DWORD code = 0; if (gRun.proc) { WaitForSingleObject(gRun.proc, 5000); GetExitCodeProcess(gRun.proc, &code); CloseHandle(gRun.proc); }
    if (gRun.thread) { WaitForSingleObject(gRun.thread, 2000); CloseHandle(gRun.thread); }
    tailOut(); if (gRun.job) CloseHandle(gRun.job);
    bool stopped = gRun.stop; LevelP L = gRun.lvl; const Sol* b = L ? bestSol(*L, gRun.goal) : nullptr;
    wchar_t t[200]; swprintf(t, 200, L"=== %ls: %ls after %ls%ls", L ? L->name.c_str() : L"", stopped ? L"stopped" : (code == 0 ? L"finished" : L"ended with an error"),
        fmtTime((GetTickCount64() - gRun.t0) / 1000).c_str(), b ? (L", best " + to_wstring(b->moves) + L"/" + to_wstring(b->pushes)).c_str() : L"");
    logLine(t);
    gRun.active = false; gRun.proc = gRun.job = gRun.thread = nullptr;
    if (!stopped) startNextQueued();
    updateButtons(); updateStatus(); refreshCombo();
}

// ------------------------------------------------------------------ dialogs
static Settings readSettingsDlg(HWND d) { Settings s = gSet; wchar_t b[2048];
    s.goal = IsDlgButtonChecked(d, IDC_GOAL_PUSHES) ? 1 : 0; s.minutes = max(1, (int)GetDlgItemInt(d, IDC_MINUTES, nullptr, FALSE)); s.threads = max(1, (int)GetDlgItemInt(d, IDC_THREADS, nullptr, FALSE));
    GetDlgItemTextW(d, IDC_SLACK, b, 2048); s.slack = b; GetDlgItemTextW(d, IDC_SEED, b, 2048); s.seed = b; GetDlgItemTextW(d, IDC_EXTRA, b, 2048); s.extra = b;
    while (!s.extra.empty() && s.extra.back() == L' ') s.extra.pop_back(); while (!s.extra.empty() && s.extra[0] == L' ') s.extra.erase(0, 1);
    s.lowPri = IsDlgButtonChecked(d, IDC_LOWPRI) == BST_CHECKED; s.feed = IsDlgButtonChecked(d, IDC_FEED) == BST_CHECKED;
    s.wm = IsDlgButtonChecked(d, IDC_WM_RANGE) ? 1 : IsDlgButtonChecked(d, IDC_WM_CURVE) ? 3 : IsDlgButtonChecked(d, IDC_WM_EQUAL) ? 4 : 2;
    auto gv = [&](int id, int dv, int lo) { BOOL ok; int v = (int)GetDlgItemInt(d, id, &ok, FALSE); return ok ? max(lo, v) : dv; };
    s.wlo = gv(IDC_WLO, 20, 1); s.whi = max(s.wlo, gv(IDC_WHI, 60, 1)); s.wcenter = gv(IDC_WCENTER, 40, 1); s.wsigma = gv(IDC_WSIGMA, 15, 1);
    s.logs0 = gv(IDC_LOGS0, 1500, 0); s.logw = gv(IDC_LOGW, 400, 1);
    s.wadapt = IsDlgButtonChecked(d, IDC_WADAPT) == BST_CHECKED; s.winv = IsDlgButtonChecked(d, IDC_WINV) == BST_CHECKED;
    s.recal = gv(IDC_WRECAL, 150, 5); s.recalbud = gv(IDC_WRECALBUD, 8, 1); s.wrate = min(100, gv(IDC_WRATE, 5, 1)); s.recalimp = IsDlgButtonChecked(d, IDC_WRECALIMP) == BST_CHECKED; return s; }
static bool gDlgInit = false;
static void fillSettingsDlg(HWND d, const Settings& s) { gDlgInit = true;
    CheckRadioButton(d, IDC_GOAL_MOVES, IDC_GOAL_PUSHES, s.goal ? IDC_GOAL_PUSHES : IDC_GOAL_MOVES); SetDlgItemInt(d, IDC_MINUTES, s.minutes, FALSE); SetDlgItemInt(d, IDC_THREADS, s.threads, FALSE);
    SetDlgItemTextW(d, IDC_SLACK, s.slack.c_str()); SetDlgItemTextW(d, IDC_SEED, s.seed.c_str()); SetDlgItemTextW(d, IDC_EXTRA, s.extra.c_str());
    CheckDlgButton(d, IDC_LOWPRI, s.lowPri ? BST_CHECKED : BST_UNCHECKED); CheckDlgButton(d, IDC_FEED, s.feed ? BST_CHECKED : BST_UNCHECKED);
    for (int id : {IDC_WM_RANGE, IDC_WM_SCURVE, IDC_WM_CURVE, IDC_WM_EQUAL}) CheckDlgButton(d, id, BST_UNCHECKED);
    CheckDlgButton(d, s.wm == 1 ? IDC_WM_RANGE : s.wm == 3 ? IDC_WM_CURVE : s.wm == 4 ? IDC_WM_EQUAL : IDC_WM_SCURVE, BST_CHECKED);
    CheckDlgButton(d, IDC_WRECALIMP, s.recalimp ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(d, IDC_WLO, s.wlo, FALSE); SetDlgItemInt(d, IDC_WHI, s.whi, FALSE); SetDlgItemInt(d, IDC_WCENTER, s.wcenter, FALSE); SetDlgItemInt(d, IDC_WSIGMA, s.wsigma, FALSE);
    SetDlgItemInt(d, IDC_LOGS0, s.logs0, FALSE); SetDlgItemInt(d, IDC_LOGW, s.logw, FALSE);
    CheckDlgButton(d, IDC_WADAPT, s.wadapt ? BST_CHECKED : BST_UNCHECKED); CheckDlgButton(d, IDC_WINV, s.winv ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(d, IDC_WRECAL, s.recal, FALSE); SetDlgItemInt(d, IDC_WRECALBUD, s.recalbud, FALSE); SetDlgItemInt(d, IDC_WRATE, s.wrate, FALSE); gDlgInit = false; }
// window line part of the settings dialog: enable the fields of the chosen mode, explain the switches, draw the odds
static int previewLine() { LevelP L = curLevel(); const Sol* b = L ? bestSol(*L, gSet.goal) : nullptr; return b ? max(2, b->pushes - 1) : 150; }
static void updateWindowDlg(HWND d) {
    Settings s = readSettingsDlg(d);
    { LevelP L = curLevel(); gWLLastPath = L ? L->folder + L"\\window_line_last.txt" : L""; }
    for (int id : {IDC_WLO, IDC_WHI}) EnableWindow(GetDlgItem(d, id), s.wm != 3);
    for (int id : {IDC_WCENTER, IDC_WSIGMA}) EnableWindow(GetDlgItem(d, id), s.wm == 3);
    for (int id : {IDC_LOGS0, IDC_LOGW, IDC_WSTUCK}) EnableWindow(GetDlgItem(d, id), s.wm == 2);
    EnableWindow(GetDlgItem(d, IDC_WRECAL), s.wadapt && (s.wm == 3 || s.wm == 4) && !s.recalimp);
    for (int id : {IDC_WRECALBUD, IDC_WRECALIMP}) EnableWindow(GetDlgItem(d, id), s.wadapt && (s.wm == 3 || s.wm == 4));
    EnableWindow(GetDlgItem(d, IDC_WRATE), s.wadapt && (s.wm == 1 || s.wm == 2));
    int stuck = (int)SendDlgItemMessageW(d, IDC_WSTUCK, TBM_GETPOS, 0, 0) * 100; wchar_t b[64]; swprintf(b, 64, L"%d", stuck); SetDlgItemTextW(d, IDC_WSTUCKV, b);
    wstring note = s.wm == 1 ? L"Range: every window size from Bottom to Top is equally likely."
                 : s.wm == 2 ? L"S-Curve: windows from Bottom to Top while the walk improves; the longer it goes without improving, the likelier a big window (Top up to the whole line). Move the slider to see the odds change."
                 : s.wm == 3 ? L"Curve: a bell around Centre with the given Width."
                 : s.winv ? L"Equal + Inverse (default): odds follow the measured hit rate itself, so the sizes that find improvements most often are chosen most."
                 : L"Equal: odds = 1 / measured hit rate, so every window size is expected to contribute about equally many improvements.";
    if (s.wm == 4) { lastMeasured(); note += gWLLastFound ? L" Preview: the curve measured in this level's last run." : L" Until the first measurement (and in this preview before any run) the odds are uniform from Bottom to Top."; }
    if (s.wadapt && (s.wm == 3 || s.wm == 4) && s.recalimp) { wchar_t b[200]; swprintf(b, 200, L" Adaptive: re-measured after each new best line (at least 3 s apart), up to %d s each time.", min(s.recalbud, 3)); note += b; }
    else if (s.wm == 4 && !s.wadapt) note += L" Adaptive off: measured once at the start of the run.";
    else if (s.wadapt && (s.wm == 3 || s.wm == 4)) { wchar_t b[200]; int bud = min(s.recalbud * 2, s.recal) / 2 == 0 ? 1 : min(s.recalbud, s.recal / 2);
        swprintf(b, 200, L" Adaptive: every %d s one walker re-measures the odds on the best line for up to %d s%ls (skipped if the best line has not changed) and the odds follow the fitted curve.", s.recal, max(1, bud), s.recalbud > s.recal / 2 ? L" (measure time limited to half the interval)" : L"");
        note += b; }
    else if (s.wadapt) { wchar_t b[200]; swprintf(b, 200, L" Adaptive: the window sizes move towards the sizes that improve the line; each improvement moves them %d%% of the way (adapt speed).", s.wrate); note += b; }
    if (s.winv && s.wm != 4) note += L" Inverse (experimental): odds flipped, p(w) = max - p(w) over sizes 1 to 300.";
    SetDlgItemTextW(d, IDC_WNOTE, note.c_str()); InvalidateRect(GetDlgItem(d, IDC_WGRAPH), nullptr, TRUE); }
static void drawWindowGraph(HWND d, const DRAWITEMSTRUCT* di) {
    Settings s = readSettingsDlg(d); int whole = previewLine(); int stuck = (int)SendDlgItemMessageW(d, IDC_WSTUCK, TBM_GETPOS, 0, 0) * 100;
    vector<double> p = windowOdds(s, whole, stuck); int M = (int)p.size() - 1; double mx = 0; for (int w = 1; w <= M; w++) mx = max(mx, p[w]); if (mx <= 0) mx = 1;
    HDC dc = di->hDC; RECT r = di->rcItem; FillRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(d, WM_GETFONT, 0, 0)); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(80, 80, 80));
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm); int th = tm.tmHeight;
    int L0 = r.left + 6 * th / 2 + 4, R0 = r.right - th, T0 = r.top + th + 2, B0 = r.bottom - 2 * th - 2;
    HPEN axis = CreatePen(PS_SOLID, 1, RGB(160, 160, 160)), grid = CreatePen(PS_DOT, 1, RGB(215, 215, 215)), line = CreatePen(PS_SOLID, 2, RGB(30, 100, 200));
    HPEN op = (HPEN)SelectObject(dc, grid); for (int k = 1; k <= 3; k++) { int y = B0 - (B0 - T0) * k / 4; MoveToEx(dc, L0, y, nullptr); LineTo(dc, R0, y); }
    SelectObject(dc, axis); MoveToEx(dc, L0, T0, nullptr); LineTo(dc, L0, B0); LineTo(dc, R0, B0);
    auto X = [&](double w) { return L0 + (int)lround((R0 - L0) * (w - 1) / max(1, M - 1)); }; auto Y = [&](double v) { return B0 - (int)lround((B0 - T0) * v / mx); };
    vector<POINT> pts; pts.push_back({X(1), B0}); for (int w = 1; w <= M; w++) pts.push_back({X(w), Y(p[w])}); pts.push_back({X(M), B0});
    HBRUSH fill = CreateSolidBrush(RGB(205, 225, 250)); HBRUSH ob = (HBRUSH)SelectObject(dc, fill); SelectObject(dc, GetStockObject(NULL_PEN)); Polygon(dc, pts.data(), (int)pts.size());
    SelectObject(dc, line); Polyline(dc, pts.data() + 1, (int)pts.size() - 2);
    wchar_t t[96]; swprintf(t, 96, L"%.2g%%", 100 * mx); RECT tr{r.left + 2, T0 - th / 2, L0 - 3, T0 + th}; DrawTextW(dc, t, -1, &tr, DT_RIGHT | DT_SINGLELINE);
    RECT zr{r.left + 2, B0 - th / 2, L0 - 3, B0 + th}; DrawTextW(dc, L"0", -1, &zr, DT_RIGHT | DT_SINGLELINE);
    for (int k = 0; k <= 4; k++) { int w = 1 + (M - 1) * k / 4; swprintf(t, 96, L"%d", w); RECT xr{X(w) - 30, B0 + 2, X(w) + 30, B0 + th + 2}; DrawTextW(dc, t, -1, &xr, DT_CENTER | DT_SINGLELINE); }
    swprintf(t, 96, L"window size (pushes); this level's line has about %d pushes", whole + 1); RECT lr{L0, B0 + th + 2, R0, B0 + 2 * th + 2}; DrawTextW(dc, t, -1, &lr, DT_CENTER | DT_SINGLELINE);
    swprintf(t, 96, L"chance of each window size%ls", s.wm == 2 ? L" (at the slider's steps without improvement)" : L""); RECT hr{L0, r.top + 1, R0, T0}; DrawTextW(dc, t, -1, &hr, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, op); SelectObject(dc, ob); SelectObject(dc, of); DeleteObject(axis); DeleteObject(grid); DeleteObject(line); DeleteObject(fill); }
static INT_PTR CALLBACK SettingsProc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG: { SendDlgItemMessageW(d, IDC_WSTUCK, TBM_SETRANGE, TRUE, MAKELPARAM(0, 60)); SendDlgItemMessageW(d, IDC_WSTUCK, TBM_SETPOS, TRUE, 0);
        fillSettingsDlg(d, gSet); updateWindowDlg(d); wchar_t b[64]; swprintf(b, 64, L"this PC has %d", numCpus()); SetDlgItemTextW(d, IDC_THREADS_NOTE, b);
        SetDlgItemTextW(d, IDC_PREVIEW, (L"plateau.exe " + engineArgs(readSettingsDlg(d))).c_str()); return TRUE; }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: { Settings s = readSettingsDlg(d); if (s.threads > 1024) s.threads = 1024; gSet = s; gTickNew = s.feed; saveSettings(); EndDialog(d, IDOK); return TRUE; }
        case IDCANCEL: EndDialog(d, IDCANCEL); return TRUE;
        case IDC_DEFAULTS: fillSettingsDlg(d, defaultSettings()); updateWindowDlg(d); SetDlgItemTextW(d, IDC_PREVIEW, (L"plateau.exe " + engineArgs(readSettingsDlg(d))).c_str()); return TRUE;
        default: if (!gDlgInit && LOWORD(wp) == IDC_WM_EQUAL && HIWORD(wp) == BN_CLICKED) CheckDlgButton(d, IDC_WADAPT, BST_CHECKED);
            if (!gDlgInit && (HIWORD(wp) == EN_CHANGE || HIWORD(wp) == BN_CLICKED)) { updateWindowDlg(d); SetDlgItemTextW(d, IDC_PREVIEW, (L"plateau.exe " + engineArgs(readSettingsDlg(d))).c_str()); } return FALSE; }
    case WM_HSCROLL: updateWindowDlg(d); return TRUE;
    case WM_DRAWITEM: if (wp == IDC_WGRAPH) { drawWindowGraph(d, (const DRAWITEMSTRUCT*)lp); return TRUE; } return FALSE;
    }
    return FALSE;
}
static wstring gPasted;
static INT_PTR CALLBACK RenameProc(HWND d, UINT m, WPARAM wp, LPARAM) {
    if (m == WM_INITDIALOG) { SetDlgItemTextW(d, IDC_NAME, gPasted.c_str()); SendDlgItemMessageW(d, IDC_NAME, EM_SETSEL, 0, -1); SetFocus(GetDlgItem(d, IDC_NAME)); return FALSE; }
    if (m == WM_COMMAND && LOWORD(wp) == IDOK) { wchar_t b[256]; GetDlgItemTextW(d, IDC_NAME, b, 256); gPasted = b; EndDialog(d, IDOK); return TRUE; }
    if (m == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    return FALSE;
}
static INT_PTR CALLBACK PasteProc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG: SendDlgItemMessageW(d, IDC_PASTE, EM_SETLIMITTEXT, 0, 0); SendDlgItemMessageW(d, IDC_PASTE, WM_SETFONT, (WPARAM)gMono, TRUE);
        if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(d)) { HANDLE h = GetClipboardData(CF_UNICODETEXT); if (h) { const wchar_t* p = (const wchar_t*)GlobalLock(h); if (p) { wstring s = p; GlobalUnlock(h);
            string s8 = U8(s); Parsed R; parseText(s8, R); if (!R.levels.empty() || !R.orphan.empty()) { SetDlgItemTextW(d, IDC_PASTE, s.c_str()); } } } CloseClipboard(); }
        SetFocus(GetDlgItem(d, IDC_PASTE)); return FALSE;
    case WM_SIZE: { int w = LOWORD(lp), h = HIWORD(lp); RECT r{0, 0, 7, 26}; MapDialogRect(d, &r); RECT bt{0, 0, 50, 14}; MapDialogRect(d, &bt); int m7 = r.right, top = r.bottom, bw = bt.right, bh = bt.bottom;
        MoveWindow(GetDlgItem(d, IDC_PASTE), m7, top, w - 2 * m7, h - top - bh - 2 * m7, TRUE);
        MoveWindow(GetDlgItem(d, IDCANCEL), w - m7 - bw, h - m7 - bh, bw, bh, TRUE); MoveWindow(GetDlgItem(d, IDOK), w - 2 * m7 - 2 * bw + m7 / 2, h - m7 - bh, bw, bh, TRUE); return TRUE; }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { int n = GetWindowTextLengthW(GetDlgItem(d, IDC_PASTE)); wstring s(n + 1, 0); GetDlgItemTextW(d, IDC_PASTE, &s[0], n + 1); s.resize(n); gPasted = s; EndDialog(d, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
    }
    return FALSE;
}

// ------------------------------------------------------------------ main window
static HWND mk(const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
    HWND h = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, gMain, (HMENU)(INT_PTR)id, gInst, nullptr); SendMessageW(h, WM_SETFONT, (WPARAM)gFont, FALSE); return h; }
static void layout() {
    RECT rc; GetClientRect(gMain, &rc); int W = rc.right, H = rc.bottom;
    SendMessageW(gStatus, WM_SIZE, 0, 0); RECT sr; GetWindowRect(gStatus, &sr); int sh = sr.bottom - sr.top;
    int parts[2] = {W * 45 / 100, -1}; SendMessageW(gStatus, SB_SETPARTS, 2, (LPARAM)parts);
    int m = S(8), bh = S(28), tb = S(8) + bh + S(8);
    int x = m; auto place = [&](HWND h, int w) { MoveWindow(h, x, S(8), w, bh, TRUE); x += w + S(6); };
    place(gBtnOpen, S(84)); place(gBtnSettings, S(84)); x += S(12); place(gBtnRun, S(96)); place(gBtnRunAll, S(110)); place(gBtnStop, S(70));
    MoveWindow(gGoalLbl, x + S(10), S(8) + S(6), max(10, W - x - S(10) - m), S(18), TRUE);
    int logH = max(S(90), H / 5), logTop = H - sh - logH - m;
    MoveWindow(gLogLbl, m, logTop - S(20), S(200), S(18), TRUE); MoveWindow(gLog, m, logTop, W - 2 * m, logH, TRUE);
    int side = S(330), y = tb; int bottom = logTop - S(26);
    MoveWindow(gLevelLbl, m, y, side, S(18), TRUE); y += S(20); MoveWindow(gCombo, m, y, side, S(400), TRUE); y += S(32);
    MoveWindow(gSolLbl, m, y, side, S(18), TRUE); y += S(20);
    int btnY = bottom - bh; MoveWindow(gList, m, y, side, max(S(40), btnY - S(6) - y), TRUE);
    { int w3 = (side - S(12)) / 3; MoveWindow(gBtnCopy, m, btnY, w3, bh, TRUE); MoveWindow(gBtnDelete, m + w3 + S(6), btnY, w3, bh, TRUE); MoveWindow(gBtnSave, m + 2 * (w3 + S(6)), btnY, side - 2 * (w3 + S(6)), bh, TRUE); }
    int bx = m + side + S(10), bw = W - bx - m; int ctrlH = S(64); int boardB = bottom - ctrlH;
    MoveWindow(gBoard, bx, tb, bw, max(S(40), boardB - tb), TRUE);
    int cy = boardB + S(8); int px = bx; auto pb = [&](HWND h, int w) { MoveWindow(h, px, cy, w, bh, TRUE); px += w + S(4); };
    pb(gBtnStart, S(40)); pb(gBtnBack, S(40)); pb(gBtnPlay, S(64)); pb(gBtnFwd, S(40)); pb(gBtnEnd, S(40));
    MoveWindow(gPos, px + S(6), cy, max(S(40), bx + bw - px - S(6)), bh, TRUE);
    int cy2 = cy + bh + S(4); MoveWindow(gPosLbl, bx, cy2 + S(2), S(300), S(18), TRUE);
    int spw = S(180); MoveWindow(gSpeedLbl, bx + bw - spw - S(100), cy2 + S(2), S(96), S(18), TRUE); MoveWindow(gSpeed, bx + bw - spw, cy2, spw, S(22), TRUE);
}
static void doOpenMenu() {
    HMENU mn = CreatePopupMenu(); bool has = curLevel() != nullptr;
    AppendMenuW(mn, MF_STRING, ID_OPEN_FILE, L"Open level or collection file...\tCtrl+O");
    AppendMenuW(mn, MF_STRING, ID_PASTE, L"Paste levels or solutions...");
    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mn, MF_STRING | (has ? 0 : MF_GRAYED), ID_ADDSOLS, L"Add solutions from a file to this level...");
    AppendMenuW(mn, MF_STRING | (has ? 0 : MF_GRAYED), ID_SAVE, L"Save this level with all its solutions...");
    AppendMenuW(mn, MF_STRING | (has ? 0 : MF_GRAYED), ID_RENAME, L"Rename this level (for example to its ID)...");
    AppendMenuW(mn, MF_STRING | (has ? 0 : MF_GRAYED), ID_FOLDER, L"Open this level's results folder");
    RECT r; GetWindowRect(gBtnOpen, &r); TrackPopupMenu(mn, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, gMain, nullptr); DestroyMenu(mn);
}
static void setTick(Level& L, int i, bool on, bool quiet) {
    Sol& s = L.sols[i]; if (s.use == on && quiet) return; s.use = on;
    if (on) { L.ticked.insert(s.lurd); L.unticked.erase(s.lurd); } else { L.unticked.insert(s.lurd); L.ticked.erase(s.lurd); }
    if (!quiet) { saveState(L); wchar_t b[128]; swprintf(b, 128, L"Solutions (%d, %d ticked to start from)", (int)L.sols.size(), L.nTicked()); SetWindowTextW(gSolLbl, b); updateButtons(); updateStatus(); }
}
// remove the selected lines from the list; they stay hidden until you add them again (Open > Add solutions or Paste). Files are not changed.
static void deleteSelected() {
    LevelP L = curLevel(); if (!L) return; vector<int> idx; int row = -1;
    while ((row = ListView_GetNextItem(gList, row, LVNI_SELECTED)) >= 0) { LVITEMW it{}; it.mask = LVIF_PARAM; it.iItem = row; ListView_GetItem(gList, &it); idx.push_back((int)it.lParam); }
    if (idx.empty()) return;
    if (idx.size() > 1) { wchar_t t[200]; swprintf(t, 200, L"Delete %d solutions from the list?", (int)idx.size()); if (MessageBoxW(gMain, t, L"Plateau", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return; }
    string keep = (P.lvl == L && P.sol >= 0) ? P.lurd : "";
    sort(idx.rbegin(), idx.rend()); for (int i : idx) { const string& x = L->sols[i].lurd; L->deleted.insert(x); L->ticked.erase(x); L->unticked.erase(x); L->seen.erase(x); if (x == keep) keep.clear(); L->sols.erase(L->sols.begin() + i); }
    saveState(*L);
    if (P.lvl == L) { int ns = -1; for (size_t i = 0; i < L->sols.size(); i++) if (L->sols[i].lurd == keep && !keep.empty()) ns = i; if (ns >= 0) P.sol = ns; else loadSolution(-1); }
    refreshList(true); refreshCombo(); updateButtons(); updateStatus(); updatePlayerUi();
    logLine(L"Deleted " + to_wstring(idx.size()) + L" solution(s) from " + L->name + L". Add them again with Open > Add solutions if you need them.");
}
static void copySelected() {
    LevelP L = curLevel(); if (!L || P.sol < 0 || P.lvl != L) return; wstring s = W(L->sols[P.sol].lurd);
    if (!OpenClipboard(gMain)) return; EmptyClipboard(); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); SetClipboardData(CF_UNICODETEXT, g); } CloseClipboard();
    wchar_t t[120]; swprintf(t, 120, L"Copied the %d/%d solution to the clipboard.", L->sols[P.sol].moves, L->sols[P.sol].pushes); SendMessageW(gStatus, SB_SETTEXTW, 0, (LPARAM)t);
}
static void onCommand(int id) {
    LevelP L = curLevel();
    switch (id) {
    case ID_BTN_OPEN: doOpenMenu(); break;
    case ID_OPEN_FILE: { wstring f = pickFile(false); if (!f.empty()) openFile(f, false); break; }
    case ID_ADDSOLS: { wstring f = pickFile(false); if (!f.empty()) openFile(f, true); break; }
    case ID_PASTE: if (DialogBoxW(gInst, MAKEINTRESOURCEW(IDD_PASTE), gMain, PasteProc) == IDOK && !gPasted.empty()) { wstring msg; int r = openText(U8(gPasted), L"pasted text", false, msg); logLine(msg); if (r < 0) MessageBoxW(gMain, msg.c_str(), L"Plateau", MB_ICONWARNING); } break;
    case ID_SAVE: case ID_BTN_SAVE: if (L) { wstring f = pickFile(true, safeName(L->name) + L"_solutions.txt"); if (!f.empty()) { if (writeFile(f, levelText(*L, true))) logLine(L"Saved " + L->name + L" with " + to_wstring(L->sols.size()) + L" solution(s) to " + f); else MessageBoxW(gMain, L"Could not write the file.", L"Plateau", MB_ICONERROR); } } break;
    case ID_RENAME: if (L) {
        if (gRun.active && gRun.lvl == L) { MessageBoxW(gMain, L"This level is running. Rename it after the run.", L"Plateau", MB_ICONINFORMATION); break; }
        gPasted = L->name; if (DialogBoxW(gInst, MAKEINTRESOURCEW(IDD_RENAME), gMain, RenameProc) == IDOK && !gPasted.empty() && gPasted != L->name) {
            if (renameLevel(L, gPasted)) { logLine(L"Renamed the level to " + L->name + L". Results go to " + L->folder); refreshCombo(); selectLevel(gCur); updateButtons(); updateStatus(); }
            else MessageBoxW(gMain, L"That name cannot be used.", L"Plateau", MB_ICONWARNING); } } break;
    case ID_FOLDER: if (L) { CreateDirectoryW(resultsDir().c_str(), nullptr); CreateDirectoryW(L->folder.c_str(), nullptr); ShellExecuteW(gMain, L"open", L->folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL); } break;
    case ID_BTN_SETTINGS: if (DialogBoxW(gInst, MAKEINTRESOURCEW(IDD_SETTINGS), gMain, SettingsProc) == IDOK) { gSortCol = gSet.goal; refreshList(true); refreshCombo(); updateButtons(); updateStatus(); } break;
    case ID_BTN_RUN: if (L && !gRun.active) { gQueue.clear(); startRun(L); } break;
    case ID_BTN_RUNALL: if (!gRun.active && !gLevels.empty()) { wchar_t t[300]; swprintf(t, 300, L"Run Plateau on all %d levels, one after another, %d minutes each (about %ls in total)?", (int)gLevels.size(), gSet.minutes, fmtTime((long long)gSet.minutes * 60 * gLevels.size()).c_str());
        if (MessageBoxW(gMain, t, L"Plateau", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) { gQueue = gLevels; startNextQueued(); updateButtons(); } } break;
    case ID_BTN_STOP: stopRun(); break;
    case ID_BTN_COPY: case ID_COPY: copySelected(); break;
    case ID_BTN_DELETE: deleteSelected(); break;
    case ID_TICKALL: case ID_UNTICKALL: if (L) { for (size_t i = 0; i < L->sols.size(); i++) setTick(*L, i, id == ID_TICKALL, true); saveState(*L); refreshList(true); updateButtons(); updateStatus(); } break;
    case ID_TICKBEST: if (L) { vector<int> o(L->sols.size()); for (size_t i = 0; i < o.size(); i++) o[i] = i; auto& s = L->sols; int g = gSet.goal;
        sort(o.begin(), o.end(), [&](int a, int b) { return g ? make_pair(s[a].pushes, s[a].moves) < make_pair(s[b].pushes, s[b].moves) : make_pair(s[a].moves, s[a].pushes) < make_pair(s[b].moves, s[b].pushes); });
        for (size_t k = 0; k < o.size(); k++) setTick(*L, o[k], k < 8, true); saveState(*L); refreshList(true); updateButtons(); updateStatus(); } break;
    case ID_RUNONLY: if (L && P.lvl == L && P.sol >= 0 && !gRun.active) { for (size_t i = 0; i < L->sols.size(); i++) setTick(*L, i, (int)i == P.sol, true); saveState(*L); refreshList(true); updateButtons(); updateStatus(); gQueue.clear(); startRun(L); } break;
    case ID_P_START: setPlaying(false); seek(0); break;
    case ID_P_BACK: setPlaying(false); seek(P.step - 1); break;
    case ID_P_PLAY: setPlaying(!P.playing); break;
    case ID_P_FWD: setPlaying(false); seek(P.step + 1); break;
    case ID_P_END: setPlaying(false); seek(P.lurd.size()); break;
    }
}
static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        gMain = h;
        gBtnOpen = mk(L"BUTTON", L"Open  \x25BE", BS_PUSHBUTTON, ID_BTN_OPEN); gBtnSettings = mk(L"BUTTON", L"Settings", BS_PUSHBUTTON, ID_BTN_SETTINGS);
        gBtnRun = mk(L"BUTTON", L"Run this level", BS_PUSHBUTTON, ID_BTN_RUN); gBtnRunAll = mk(L"BUTTON", L"Run all levels", BS_PUSHBUTTON, ID_BTN_RUNALL); gBtnStop = mk(L"BUTTON", L"Stop", BS_PUSHBUTTON, ID_BTN_STOP);
        gGoalLbl = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, -1);
        gLevelLbl = mk(L"STATIC", L"Level", SS_LEFT, -1); SendMessageW(gLevelLbl, WM_SETFONT, (WPARAM)gFontBold, FALSE);
        gCombo = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_COMBO);
        gSolLbl = mk(L"STATIC", L"Solutions", SS_LEFT, -1); SendMessageW(gSolLbl, WM_SETFONT, (WPARAM)gFontBold, FALSE);
        gList = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST, WS_EX_CLIENTEDGE);
        ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
        { LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; c.fmt = LVCFMT_RIGHT; c.cx = S(74); c.pszText = (LPWSTR)L"Moves"; ListView_InsertColumn(gList, 0, &c);
          c.pszText = (LPWSTR)L"Pushes"; ListView_InsertColumn(gList, 1, &c); c.fmt = LVCFMT_LEFT; c.cx = S(172); c.pszText = (LPWSTR)L"Found by"; ListView_InsertColumn(gList, 2, &c); }
        gBtnCopy = mk(L"BUTTON", L"Copy", BS_PUSHBUTTON, ID_BTN_COPY); gBtnDelete = mk(L"BUTTON", L"Delete", BS_PUSHBUTTON, ID_BTN_DELETE); gBtnSave = mk(L"BUTTON", L"Save all...", BS_PUSHBUTTON, ID_BTN_SAVE);
        gBoard = CreateWindowExW(0, L"PlateauBoard", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, h, (HMENU)100, gInst, nullptr);
        gBtnStart = mk(L"BUTTON", L"|<", BS_PUSHBUTTON, ID_P_START); gBtnBack = mk(L"BUTTON", L"<", BS_PUSHBUTTON, ID_P_BACK); gBtnPlay = mk(L"BUTTON", L"Play", BS_PUSHBUTTON, ID_P_PLAY);
        gBtnFwd = mk(L"BUTTON", L">", BS_PUSHBUTTON, ID_P_FWD); gBtnEnd = mk(L"BUTTON", L">|", BS_PUSHBUTTON, ID_P_END);
        gPos = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_POS); gPosLbl = mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, -1);
        gSpeedLbl = mk(L"STATIC", L"", SS_RIGHT, -1); gSpeed = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, ID_SPEED);
        SendMessageW(gSpeed, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); { int sp = _wtoi(iniGet(L"speed", L"40").c_str()); SendMessageW(gSpeed, TBM_SETPOS, TRUE, max(0, min(100, sp))); }
        gLogLbl = mk(L"STATIC", L"Optimizer output", SS_LEFT, -1); SendMessageW(gLogLbl, WM_SETFONT, (WPARAM)gFontBold, FALSE);
        gLog = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | ES_NOHIDESEL, ID_LOG, WS_EX_CLIENTEDGE); SendMessageW(gLog, WM_SETFONT, (WPARAM)gMono, FALSE); SendMessageW(gLog, EM_SETLIMITTEXT, 0, 0);
        gStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, (HMENU)ID_STATUS, gInst, nullptr); SendMessageW(gStatus, WM_SETFONT, (WPARAM)gFont, FALSE);
        DragAcceptFiles(h, TRUE); SetTimer(h, 1, 1000, nullptr);
        gSortCol = gSet.goal; updateSpeedLbl(); updatePlayerUi(); updateButtons(); updateStatus(); refreshList(false);
        return 0; }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* mi = (MINMAXINFO*)lp; mi->ptMinTrackSize.x = S(900); mi->ptMinTrackSize.y = S(560); return 0; }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_COMBO && HIWORD(wp) == CBN_SELCHANGE) { int i = SendMessageW(gCombo, CB_GETCURSEL, 0, 0); if (i >= 0) { selectLevel(i); updateButtons(); updateStatus(); } return 0; }
        onCommand(LOWORD(wp)); return 0;
    case WM_NOTIFY: { NMHDR* n = (NMHDR*)lp;
        if (n->hwndFrom == gList) {
            if (n->code == LVN_ITEMCHANGED) { NMLISTVIEW* v = (NMLISTVIEW*)lp;
                if (!gFilling && (v->uChanged & LVIF_STATE) && (v->uNewState & LVIS_SELECTED) && !(v->uOldState & LVIS_SELECTED)) { loadSolution((int)v->lParam); }
                if (!gFilling && (v->uChanged & LVIF_STATE) && ((v->uNewState ^ v->uOldState) & LVIS_STATEIMAGEMASK) && (v->uOldState & LVIS_STATEIMAGEMASK)) {
                    LevelP L = curLevel(); int i = (int)v->lParam; if (L && i >= 0 && i < (int)L->sols.size()) setTick(*L, i, ((v->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2, false); } }
            else if (n->code == NM_DBLCLK) { if (P.sol >= 0) { playerReset(); setPlaying(true); } }
            else if (n->code == LVN_COLUMNCLICK) { gSortCol = ((NMLISTVIEW*)lp)->iSubItem; refreshList(true); }
            else if (n->code == NM_RCLICK) { LevelP L = curLevel(); if (L && !L->sols.empty()) { HMENU mn = CreatePopupMenu(); UINT one = P.sol >= 0 ? 0 : MF_GRAYED;
                AppendMenuW(mn, MF_STRING | one | (gRun.active ? MF_GRAYED : 0), ID_RUNONLY, L"Run from this solution only");
                AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(mn, MF_STRING, ID_TICKALL, L"Tick all (start from every solution)"); AppendMenuW(mn, MF_STRING, ID_TICKBEST, L"Tick only the best 8 for the current goal"); AppendMenuW(mn, MF_STRING, ID_UNTICKALL, L"Untick all");
                AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(mn, MF_STRING | one, ID_COPY, L"Copy solution"); AppendMenuW(mn, MF_STRING | one, ID_BTN_DELETE, L"Delete selected\tDel");
                POINT pt; GetCursorPos(&pt); TrackPopupMenu(mn, 0, pt.x, pt.y, 0, h, nullptr); DestroyMenu(mn); } }
        }
        return 0; }
    case WM_HSCROLL:
        if ((HWND)lp == gPos) { setPlaying(false); seek((int)SendMessageW(gPos, TBM_GETPOS, 0, 0)); }
        else if ((HWND)lp == gSpeed) { updateSpeedLbl(); iniPut(L"speed", to_wstring(SendMessageW(gSpeed, TBM_GETPOS, 0, 0))); }
        return 0;
    case WM_TIMER:
        if (wp == 1) { if (gRun.active) { tailOut(); updateStatus(); } }
        else if (wp == 2 && P.playing) { ULONGLONG now = GetTickCount64(); P.acc += (now - P.last) * speedMps() / 1000.0; P.last = now; bool moved = false;
            while (P.acc >= 1.0 && P.step < (int)P.lurd.size()) { stepFwd(); P.acc -= 1.0; moved = true; }
            if (P.step >= (int)P.lurd.size()) setPlaying(false); else if (moved) updatePlayerUi(); }
        return 0;
    case WM_APP_LOG: { string* s = (string*)lp; logAppend(*s); delete s; return 0; }
    case WM_APP_DONE: runDone(); return 0;
    case WM_DROPFILES: { wchar_t f[4096]; if (DragQueryFileW((HDROP)wp, 0, f, 4096)) openFile(f, false); DragFinish((HDROP)wp); return 0; }
    case WM_CTLCOLORSTATIC: if ((HWND)lp == gLog) break; return DefWindowProcW(h, m, wp, lp);
    case WM_CLOSE:
        if (gRun.active) { if (MessageBoxW(h, L"Plateau is still running. Stop it and close?\n\nEvery line it found so far is already saved in the level's results folder.", L"Plateau", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            gQueue.clear(); gRun.stop = true; if (gRun.job) TerminateJobObject(gRun.job, 1); else if (gRun.proc) TerminateProcess(gRun.proc, 1); }
        { WINDOWPLACEMENT wpl{sizeof wpl}; if (GetWindowPlacement(h, &wpl)) { RECT r = wpl.rcNormalPosition; iniPut(L"window", to_wstring(r.left) + L"," + to_wstring(r.top) + L"," + to_wstring(r.right) + L"," + to_wstring(r.bottom) + L"," + (wpl.showCmd == SW_SHOWMAXIMIZED ? L"1" : L"0")); } }
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    gInst = hi; SetProcessDPIAware();
    INITCOMMONCONTROLSEX ic{sizeof ic, ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
    wchar_t mp[MAX_PATH * 4]; GetModuleFileNameW(nullptr, mp, MAX_PATH * 4); gExeDir = mp; gExeDir = gExeDir.substr(0, gExeDir.find_last_of(L"\\/"));
    gIni = gExeDir + L"\\plateau_gui.ini"; gEngine = gExeDir + L"\\plateau.exe"; loadSettings();
    HDC sdc = GetDC(nullptr); int dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(nullptr, sdc); gS = dpi / 96.0;
    gFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    gFontBold = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    gMono = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    HICON icon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APP));
    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = MainProc; wc.hInstance = hi; wc.hIcon = icon; wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"PlateauMain"; RegisterClassExW(&wc);
    WNDCLASSEXW bc{sizeof bc}; bc.lpfnWndProc = BoardProc; bc.hInstance = hi; bc.hCursor = LoadCursor(nullptr, IDC_ARROW); bc.lpszClassName = L"PlateauBoard"; bc.style = CS_HREDRAW | CS_VREDRAW; RegisterClassExW(&bc);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = S(1180), hgt = S(800); bool maxi = false;
    { wstring wp = iniGet(L"window", L""); int a, b, c, d, e; if (swscanf(wp.c_str(), L"%d,%d,%d,%d,%d", &a, &b, &c, &d, &e) == 5 && c - a > 200 && d - b > 200) {
        RECT r{a, b, c, d}; if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) { x = a; y = b; w = c - a; hgt = d - b; maxi = e != 0; } } }
    HWND h = CreateWindowExW(0, L"PlateauMain", L"Plateau", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, hgt, nullptr, nullptr, hi, nullptr);
    ShowWindow(h, maxi ? SW_SHOWMAXIMIZED : show); UpdateWindow(h);
    // what to open first: a file given on the command line (or dropped on the exe), else the last file, else input.txt next to the exe
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    vector<wstring> files; for (int i = 1; i < argc; i++) files.push_back(argv[i]); LocalFree(argv);
    if (files.empty()) { wstring last = iniGet(L"lastfile", L""); if (!last.empty() && exists(last)) files.push_back(last); else if (exists(gExeDir + L"\\input.txt")) files.push_back(gExeDir + L"\\input.txt"); }
    for (auto& f : files) openFile(f, false); // a later file with only solution lines adds them to the level opened before it
    logLine(L"Plateau " + wstring(exists(gEngine) ? L"is ready. " : L"cannot find plateau.exe next to this program. ") + L"Results go to " + resultsDir() + L"\\<level>.");
    HACCEL acc = nullptr; { ACCEL a[1] = {{FCONTROL | FVIRTKEY, 'O', ID_OPEN_FILE}}; acc = CreateAcceleratorTableW(a, 1); }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (TranslateAcceleratorW(h, acc, &msg)) continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_DELETE && msg.hwnd == gList) { deleteSelected(); continue; }
        if (msg.message == WM_KEYDOWN && GetAncestor(msg.hwnd, GA_ROOT) == h && P.sol >= 0) {
            wchar_t cls[32] = {0}; GetClassNameW(GetFocus(), cls, 32); bool typing = !lstrcmpiW(cls, L"Edit") || !lstrcmpiW(cls, WC_COMBOBOXW);
            if (!typing) { bool used = true;
                switch (msg.wParam) { case VK_SPACE: setPlaying(!P.playing); break; case VK_LEFT: setPlaying(false); seek(P.step - 1); break; case VK_RIGHT: setPlaying(false); seek(P.step + 1); break;
                    case VK_HOME: setPlaying(false); seek(0); break; case VK_END: setPlaying(false); seek(P.lurd.size()); break; default: used = false; }
                if (used) continue; } }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
