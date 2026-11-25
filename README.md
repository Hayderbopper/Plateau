<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/banner_dark.png">
    <img src="assets/banner_light.png" alt="Plateau: Sokoban solution optimizer" width="640">
  </picture>
</p>

# Plateau: a Sokoban solution optimizer

Give Plateau a Sokoban level and one or more solutions. It searches for shorter solutions, either **fewest moves** (then pushes) or **fewest pushes** (then moves). Every new best line and every new moves/pushes trade-off line is checked by exact replay and written to a file.

Windows, 64-bit. A GUI (`plateau_gui.exe`) and a command-line engine (`plateau.exe`).

## Quick start

**GUI**
1. Run `plateau_gui.exe`.
2. Open or paste a level with at least one solution (Open, or drag a file onto the window).
3. Pick *Moves* or *Pushes* in Settings, then **Run this level**. Found lines appear in the list as they arrive. They are also saved in `results\<level>\` (`best_moves.txt` / `best_pushes.txt`).

**Batch file**: put the level and your solution lines in `input.txt`, double-click `plateau.bat`, then choose 1 (moves) or 2 (pushes).

**Command line**
```
plateau LEVELFILE [OPTION=value ...]
plateau --help        (lists every option and its default)
```

## How it works

- **Plateau walk.** Every thread walks on its own line.
  - Each step takes a stretch ("window") of the line, picks a few boxes pushed in it, removes their pushes there, and re-inserts them with *hook and ball*, a bidirectional layered search.
  - Some steps ban a square the boxes used (a "kick"), so the line really changes.
  - A new line is kept if it is at most `SLACK` worse than the walker's best, so the walk crosses flat stretches instead of stalling.
- **Crossover.** Joins pieces of good lines wherever two lines pass through the same position.
- **Exact replan.** Re-plans the complete histories of 1-3 nearby boxes across the whole line.
- **Metric switching.** Alternates between the target metric and a short detour on the other one.

## Window line (Settings > Window line)

How long the window of each step is. The graph under the selector shows the chance of each window size for the open level, live.

| Mode | Window sizes |
|---|---|
| **Equal** *(default)* | Odds ~ 1 / measured hit rate of each size, so every size contributes about equally many improvements. Measured on the best line during the run. Uniform from Bottom to Top until the first measurement. |
| S-Curve | Bottom-Top while the walk improves. The longer it goes without improving, the likelier a big window (Top up to the whole line). |
| Range | Uniform from Bottom to Top (default 20-60 pushes). |
| Curve | A bell around Centre with the given Width. |

- **Adaptive** (on by default for Equal):
  - Curve and Equal re-measure the hit rates every *Re-measure every* seconds, for up to *Measure time* seconds, or *after each improvement*. Measuring is skipped when the best line hasn't changed.
  - Range and S-Curve move their window sizes toward the sizes that improve the line (*Adapt speed*).
- **Inverse** (experimental): flips the odds. For Equal, the odds follow the hit rate itself.

Command line: `WMODE=equal|scurve|range|curve`, `WLO`, `WHI`, `WCENTER`, `WSIGMA`, `WADAPT`, `WINV`, `RECAL` (0 = after each improvement), `RECALBUD`, `WRATE`.

## Main options

| Option | Default | Meaning |
|---|---|---|
| `P` | 0 | 0 = fewest moves first, 1 = fewest pushes first |
| `TIME` | 600 | run time in seconds |
| `THREADS` | 4 | walker threads (use your core count) |
| `SEED` | 1 | random seed |
| `OUT` | plateau_best.txt | file new lines are appended to |
| `SOLS` | | extra file with solution lines |
| `SLACK` | 10 / 4 | how much worse a walker's line may get (moves / pushes runs) |

`plateau --help` lists all options.

## Output format

```
Solution: <moves> moves, <pushes> pushes (Plateau <how it was found>, verified by replay)
<the solution in lurd letters>
```

## Building

MSYS2 mingw64 g++. See `source/BUILD.txt`. Release builds use `-march=x86-64-v2`.

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Hayden Chesnut.
