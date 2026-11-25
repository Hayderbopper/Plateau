Plateau - Sokoban solution optimizer
====================================
Give Plateau a level and one or more solutions; it looks for shorter solutions (fewest moves, or fewest pushes)
and writes every new best line and every new moves/pushes trade-off line to a file, each checked by exact replay.

Quick start
- Put the level and your solution lines in input.txt (the board, then the solution lines; run-length lines are fine).
- Double-click plateau.bat and pick 1 (moves first) or 2 (pushes first). It runs for 2 hours on all cores.
  New lines go to best_moves.txt / best_pushes.txt, the console to log_moves.txt / log_pushes.txt.
- Or use plateau_gui.exe, which runs plateau.exe for you and shows the lines.
- Command line: plateau LEVELFILE [OPTION=value ...]   (plateau --help lists every option and its default)

How it works
- Plateau walk: every thread walks on its own line. A step takes a stretch of 20-60 pushes, picks 2-6 boxes pushed in
  it (half the time boxes next to each other), deletes their pushes there and re-inserts them with hook and ball, a
  fast bidirectional layered search (forward pushes from the start, backward pulls from the end, joined where they
  meet). In half the steps one square those boxes used is banned (a "kick"), so the line really changes. The new line
  is kept if it is at most SLACK worse than that walker's best, so the walk crosses flat stretches and small humps
  instead of stalling. A walker that gets stuck escalates to longer stretches and later restarts from the pool.
- Crossover: every second one walker joins pieces of the elite lines and the last 256 accepted walker lines wherever
  two lines pass through the same position (same boxes, same player area), and takes the cheapest combination.
- Exact replan: every 0.25 s one walker exactly re-plans the complete histories of 1, 2 and 3 nearby boxes across
  its whole line and keeps any strict improvement.
- Metric switching: the run alternates between the target metric and a shorter detour on the other metric; the
  detour's lines seed the next round. Fewer pushes often opens the way to fewer moves and the other way round.
- Every new best and every new moves/pushes trade-off line is re-rendered with shortest walks, verified by replay
  and appended to OUT.

Output format (OUT file; plateau_gui.exe reads it, so it stays fixed)
  Solution: <moves> moves, <pushes> pushes (Plateau <tag>, verified by replay)
  <the solution in lurd letters>
  <blank line>
Tags: "start, shortest walks" (the best input line, re-rendered), "<how it was found>" for a new best,
"pareto, <how it was found>" for a new moves/pushes trade-off line.

Options (KEY=value; defaults in brackets)
  P        [0]       what to minimise first: 0 = moves (then pushes), 1 = pushes (then moves)
  TIME     [600]     run time in seconds
  THREADS  [4]       walker threads (use the number of cores)
  SEED     [1]       random seed (different seeds walk differently)
  OUT      [plateau_best.txt]  file that new lines are appended to
  LOG      []        file that the console output is also appended to
  SOLS     []        extra file with solution lines (besides those in LEVELFILE)
  POOL     [8]       size of the elite pool
  SLACK    [10 / 4]  how much worse a walker's line may get (moves runs / pushes runs)
  SLACKM, SLACKP     slack while walking on moves / on pushes (override SLACK; SLACKP=6 helps some pushes runs)
  KICK     [50]      percent of steps that ban a square
  RESTART  [6000]    steps without improvement before a walker restarts from the pool
  ARM      [1]       step size: 0 short, 1 medium, 2 long, 3 whole line, -1 automatic mix (EPS [0.25] = exploration)
  ESC, ESC1, ESC2 [1, 3000, 12000]  a stuck walker escalates to longer steps after ESC1 and ESC2 steps
  ML1 ML2 MK1 MK2 MB [20 60 2 6 20000]  medium step: stretch length, boxes re-planned (at most 8), node budget
  HBLAY [50], HBBR [10], HBWH [2]       hook and ball: nodes per layer, children per node, estimate weight
  ALT [1], CYCLE [TIME/8], ALTF [0.6]   metric switching on/off, cycle length (60-900 s), share on the target metric
  PFSEED   [0]       1 = the trade-off lines also seed the metric detours
  XO [1], BANK [256], XOT [1]           crossover on/off, lines in the bank, seconds between crossovers
  RP [1], RPT [0.25], RPK [3], RPMAX [3], RPB [200000]  exact replan on/off, interval, boxes, seconds per slot, nodes

Tip: ping-pong works. Run moves, then pushes, then moves again, feeding the best_*.txt lines back into input.txt.

Build (MSYS2 mingw64 g++): see source\BUILD.txt.

Version history
- 2026-11-22  cleaned-up source (this version): same search and same output as version 5; old optimizer (FAST=0),
  A* step search (FWHB), bridged crossover (XOB) and unused experiments removed; plateau --help added; unknown
  options are reported and ignored.
- Version 5: hook and ball keeps exact path costs (11-35% more steps per minute than version 4).
- Version 4: leaner memory (about 45 MB per run instead of about 560 MB), 13-21% faster.
- Version 3: hook and ball step search replaced the A* step search (11 wins, 6 losses, 7 ties on 8 levels).
- Version 2: crossover, exact replan slots, kick 50%, restart after 6000 steps, trade-off lines written to OUT.
Older builds and sources: _archive_2026-11-24\ (not part of the release).

Step sizes (version 2, 2026-11-24)
----------------------------------
Default: S-curve chance (midpoint 1500, width 400) (ARM=-3). Medium steps are used while a walker keeps improving; the longer it goes without improving
(steps = attempts), the more likely each step takes a big window (60 pushes up to the whole line), on an S-curve.
Presets (add to the command line):
  ARM=1      Medium   - the version 1 default: windows of 20-60 pushes, escalating to long/whole line when stuck
  ARM=0      Short    - 6-20 pushes          ARM=2  Long - 60-150 pushes          ARM=3  Full - the whole line
  ARM=-3     S-curve  - LOGS0 [1500] midpoint, LOGW [400] width, LOGCAP [0] longest big window (0 = whole line)
  ARM=-1 auto mix; ARM=-2 bell; ARM=-4/-5 growing windows; ARM=-6..-9 adaptive variants; ARM=-10 BANDLO/BANDHI band
Older engines and sources are in _archive_2026-11-24\ (not part of the release).

Default (2026-11-24 evening): Equal with Adaptive on, re-measuring every 10 s for up to 3 s, and only when the best line
has changed since the last measurement (otherwise the log says "re-measure skipped"). The version 2/3 default is
WMODE=scurve (identical output). In the GUI: Settings > Window line > Equal (default).

Window line (version 4, 2026-11-24)
-----------------------------------
Each optimizer step re-plans a stretch ("window") of the line. Settings > Window line chooses how long that window is;
the graph under it shows the chance of each window size for the open level's line, live as you change anything.
  Range     every size from Bottom to Top equally likely (default 20-60 pushes, the old Medium step)
  S-Curve   the version 2/3 default: Bottom-Top while the walk improves; the longer it goes without improving (steps = attempts),
            the likelier a big window (Top up to the whole line), on an S-curve with Midpoint 1500 and Width 400 steps.
            The slider under the graph shows the odds at a given number of steps without improvement.
  Curve     a bell around Centre (default 40 pushes) with Width (standard deviation, default 15 pushes)
  Equal     odds ~ 1 / measured hit rate of each window size (floored at 5% of the best), so every size is expected to
            contribute about equally many improvements. Measured like the adaptive Curve; Adaptive is on by default
            (off = measured once at the start). Uniform from Bottom to Top until the first measurement. Inverse: odds
            follow the hit rate itself. The preview shows the curve measured in the level's last run.
  Adaptive  Range / S-Curve: the window sizes move during the run towards the sizes that improved the line
            (moving average of ln(size) of improving steps, limited to 4 .. 4 x Top pushes).
            Curve: every 150 s one walker re-measures, on the current best line, how often each 10-push band of window
            sizes finds an improvement, and the bell is replaced by a smooth fitted curve (cubic in ln(size)).
  Inverse   experimental: the odds are flipped, p'(w) = max(p) - p(w) over window sizes 1..min(line, 300), normalised
            (for Range: every size outside Bottom-Top equally likely).
Adaptive timing (Settings): Curve "Re-measure every" RECAL [150] s and "Measure time" RECALBUD [8] s (limited to half
the interval; for 1-minute tests try 10 s and 3 s); Range / S-Curve "Adapt speed" WRATE [5%]: each improving step moves
the window sizes that far towards its own size.
Re-measure "after each improvement" (RECAL=0): after each new best line, at least 3 s apart (RECALMIN) and at least every
30 s (RECALMAX), measure time up to 3 s. WINV=2 (command line only): equal-contribution inverse for any mode.
Command line: WMODE=range|scurve|equal|curve|equal WLO= WHI= WCENTER= WSIGMA= WADAPT=1 WINV=1 (LOGS0= LOGW= for the S-curve). RECAL= RECALBUD= WRATE=
With none of these, plateau.exe runs exactly as before (S-Curve, adaptive and inverse off). Older options still work:
ARM=..., WL=<file> (a calibrated window line), ARM=-12, and plateau_calibrate.exe.
Tests so far (8 levels, 6-9 seeds, 5 min): no variant beat the S-curve; calibrated / recalibrating window lines tied or
lost (17-21-10, 16-15-14, 28-30-14) and lost 1-5 on Let's Logic 1555. Range, Curve and the switches are new and untested.
Source: source (build notes in source\BUILD.txt). Screenshots of the settings page: screenshots\.
