@echo off
cd /d "%~dp0"
rem Plateau optimizer. Reads input.txt (board + solution lines) in this folder.
if not exist input.txt if exist "..\input.txt" copy /y "..\input.txt" input.txt >nul
echo What should it minimise first?
echo   1 = moves  (then pushes)
echo   2 = pushes (then moves)
choice /c 12 /n /m "Pick 1 or 2: "
if errorlevel 2 (set P=1& set GOAL=pushes) else (set P=0& set GOAL=moves)
echo Goal: fewest %GOAL%. New best lines go to best_%GOAL%.txt, progress to log_%GOAL%.txt
plateau.exe input.txt P=%P% TIME=7200 THREADS=%NUMBER_OF_PROCESSORS% OUT=best_%GOAL%.txt LOG=log_%GOAL%.txt
pause
