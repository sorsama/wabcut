# wabcut-name: Rename clips from a spreadsheet
# wabcut-description: Renames clips from renames.csv (old, new): every clip whose name contains "old" gets the name "new".
# wabcut-permissions: Read, EditSequence
"""Rename timeline clips from a table.

renames.csv (next to this script):

    old,new
    GX010023,Opening drive
    GX010024,Pit stop

A row renames every clip whose name contains the "old" text. Everything is one undo step.

From a terminal:      python rename_from_csv.py film.wabcut [renames.csv] [sequence name or id]
Inside Wabcut:        File > Run Python Script...  (the sequence open in the timeline)
"""

import csv
import sys
from pathlib import Path

import wabcut


def main(session, table):
    with open(table, newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    renamed = 0
    with session.transaction(f"Rename clips from {Path(table).name}"):
        for row in rows:
            clips = wabcut.records(session.clip.find(name=row["old"], limit=1000), "clips")
            if clips:
                session.clip.rename(clips=[c["id"] for c in clips], name=row["new"])
                renamed += len(clips)
            else:
                print(f"No clip is named like '{row['old']}'.")
    print(f"Renamed {renamed} clip(s).")
    return renamed


if __name__ == "__main__":
    project = sys.argv[1] if len(sys.argv) > 1 else None
    table = sys.argv[2] if len(sys.argv) > 2 else Path(__file__).with_name("renames.csv")
    sequence = sys.argv[3] if len(sys.argv) > 3 else None  # needed when the project has several sequences
    with wabcut.connect(project, edit=True, save="exit", sequence=sequence) as session:
        main(session, table)
