import sys
p = sys.argv[1]
s = open(p).read()
if 'steam-run.on' in s:
    print('already hooked')
    sys.exit(0)
hook = '''# Benchmark hand-off: while ~/sb-bench/steam-run.on exists, a timedemo replay of
# the dev bundle runs instead of the game (~/Games/SpeedBreaker-dev/HLEkit/steam-run.sh).
[ -f "$HOME/sb-bench/steam-run.on" ] && exec bash "$HOME/Games/SpeedBreaker-dev/HLEkit/steam-run.sh"
'''
marker = 'HERE="$(cd'
i = s.index(marker)
s = s[:i] + hook + s[i:]
open(p, 'w').write(s)
print('hooked')
