# Changelog

## Window line (2026-11-24)
- Settings > Window line: modes Equal (default), S-Curve, Range and Curve, with Adaptive and Inverse switches and a live preview graph of the odds per window size.
- **Equal + Adaptive** became the default. Odds ~ 1 / measured hit rate per window size, re-measured on the best line every 10 s for up to 3 s. Measuring is skipped if the best line hasn't changed. `WMODE=scurve` gives the previous default with identical output.
- Re-measure "after each improvement" option (`RECAL=0`), adjustable measure time (`RECALBUD`) and adapt speed (`WRATE`).
- Window-line calibration (`plateau_calibrate.exe`, `WL=<file>`) and recalibration during the run (`ARM=-12`) kept as command-line options.
- Race results (8 levels, 6-9 seeds):
  - Calibrated and recalibrating window lines tied or lost against the S-curve: 17-21-10, 16-15-14 and 28-30-14, and lost 1-5 on Let's Logic 1555.
  - At 5 minutes, Equal + Inverse was about even with the S-curve (11-15-10 over 4 seeds), and plain Equal was behind it (6-20-10).
  - In the owner's own 16-thread GUI tests from bad start lines, plain Equal did best, so it is the default.

## Step sizes, version 2 (2026-11-24)
- Default became the S-curve (`ARM=-3`): medium windows (20-60 pushes) while a walker improves; the longer it goes without improving, the likelier a big window (60 pushes up to the whole line).
- Over 9 seeds it scored 32-20-20 against medium. Earlier step sizes remain available (`ARM=1` medium, `0` short, `2` long, `3` whole line, and others).

## Cleaned-up source (2026-11-22)
- Same search and output as version 5.
- Removed:
  - the old optimizer (`FAST=0`)
  - the A* step search (`FWHB`)
  - bridged crossover (`XOB`)
  - unused experiments
- `plateau --help` added; unknown options are reported and ignored.

## Earlier versions
- **Version 5:** hook and ball keeps exact path costs (11-35% more steps per minute than version 4).
- **Version 4:** leaner memory (about 45 MB per run instead of about 560 MB), 13-21% faster.
- **Version 3:** hook and ball step search replaced the A* step search (11 wins, 6 losses, 7 ties on 8 levels).
- **Version 2:** crossover, exact replan slots, kick 50%, restart after 6000 steps, trade-off lines written to OUT.
