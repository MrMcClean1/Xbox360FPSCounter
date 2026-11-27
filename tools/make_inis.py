"""Builds a folder ready to FTP to Hdd1:\\PeerPressure\\SideLoading\\ for every Xbox 360 game
in Aurora's library.

Usage (from C:\\Xbox360FPSCounter):
    python tools\\make_inis.py <content.db> [output folder]

<content.db> is Aurora's library database, copied from the console:
    Hdd1:\\PeerPressure\\Dashboard\\Data\\Databases\\content.db
The output folder (default out\\sideload_all) gets one <titleid>.ini per game, plus
fps_hello.xex and fps.ini, plus games.txt listing which ini is which game.

Skipped: apps and dashboards, and original Xbox games (they run through the console's
backward-compatibility emulator, where the overlay is untested).
"""
import os
import shutil
import sqlite3
import sys

APP_IDS = {0x00000000, 0xFFFF011D, 0x03602025, 0xFFFF0055, 0xFFFE07D1, 0xC0DE9999}

INI = ('[DllConfig]\r\n'
       'TitleID = 0x{tid:08X}\r\n'
       'ModuleVersion = "*.*.*.*"\r\n'
       'FileName = "fps_hello.xex"\r\n')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    db, out = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else os.path.join('out', 'sideload_all'))
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    con = sqlite3.connect('file:%s?mode=ro' % os.path.abspath(db).replace('\\', '/'), uri=True)
    rows = con.execute('SELECT TitleId, TitleName, Directory FROM ContentItems').fetchall()

    games, skipped = {}, []
    for tid, name, folder in rows:
        tid &= 0xFFFFFFFF
        name = (name or '?').encode('ascii', 'replace').decode()
        folder = folder or ''
        if tid in APP_IDS or not folder.lower().startswith(('\\games', '\\content')):
            skipped.append('%08X  %s  (app or dashboard)' % (tid, name))
        elif (tid & 0xFFFF) < 0x07D0:
            skipped.append('%08X  %s  (original Xbox game)' % (tid, name))
        else:
            games.setdefault(tid, name)

    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                    # start clean: drop inis from a previous run
        if f.lower().endswith('.ini') and f.lower() != 'fps.ini':
            os.remove(os.path.join(out, f))

    lines = []
    for tid, name in sorted(games.items(), key=lambda kv: kv[1].lower()):
        with open(os.path.join(out, '%08x.ini' % tid), 'w', newline='') as f:
            f.write(INI.format(tid=tid))
        lines.append('%08X  %s' % (tid, name))

    with open(os.path.join(out, 'games.txt'), 'w', newline='') as f:
        f.write('Inis made (%d):\r\n' % len(lines) + '\r\n'.join(lines) + '\r\n')
        f.write('\r\nSkipped (%d):\r\n' % len(set(skipped)) + '\r\n'.join(sorted(set(skipped))) + '\r\n')

    for name in (os.path.join('out', 'fps_hello.xex'), 'fps.ini'):
        src = os.path.join(here, name)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(out, os.path.basename(name)))
        else:
            print('WARNING: %s not found, not copied. Run build.bat overlay first.' % src)

    print('%d game inis written to %s (%d skipped, see games.txt)' % (len(lines), out, len(set(skipped))))
    print('FTP everything in that folder to Hdd1:\\PeerPressure\\SideLoading\\')
    return 0


if __name__ == '__main__':
    sys.exit(main())
