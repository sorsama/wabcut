# wabcut-name: Markers from a table
# wabcut-description: One marker per row of markers.csv (time, name, type): chapters for an essay, kills and clutches for a stream.
# wabcut-permissions: Read, EditSequence
"""Add a marker for each row of a table: chapters for a video essay, or the moments in a game's event log.

markers.csv (next to this script):

    time,name,type
    00:00:00:00,Intro,chapter
    00:01:30:00,The turn,chapter
    00:04:12:10,Ace,comment

Times are timecodes in the sequence's frame rate. Everything is one undo step.

From a terminal:      python markers_from_csv.py film.wabcut [markers.csv] [sequence name or id]
Inside Wabcut:        File > Run Python Script...  (the sequence open in the timeline)
"""

import csv
import sys
from pathlib import Path

import wabcut


def main(session, table):
    with open(table, newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    with session.transaction(f"Markers from {Path(table).name}"):
        for row in rows:
            session.marker.add(time=row["time"], name=row["name"], type=row.get("type") or "comment")
    print(f"Added {len(rows)} marker(s).")
    return len(rows)


if __name__ == "__main__":
    project = sys.argv[1] if len(sys.argv) > 1 else None
    table = sys.argv[2] if len(sys.argv) > 2 else Path(__file__).with_name("markers.csv")
    sequence = sys.argv[3] if len(sys.argv) > 3 else None  # needed when the project has several sequences
    with wabcut.connect(project, edit=True, save="exit", sequence=sequence) as session:
        main(session, table)
