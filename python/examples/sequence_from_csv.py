# wabcut-name: Rough cut from a shot list
# wabcut-description: Builds a new sequence from shotlist.csv (media, in, out, title): the shots in order, a chapter marker on each.
# wabcut-permissions: Read, EditSequence, EditProject
"""Build a sequence from a shot list.

shotlist.csv (next to this script):

    media,in,out,title
    gameplay.mp4,00:00:10:00,00:00:18:00,The setup
    gameplay.mp4,00:02:05:00,00:02:20:00,The clutch

The media must be in the project already (import it first). Everything is one undo step named after the file.

From a terminal:      python sequence_from_csv.py film.wabcut [shotlist.csv]
Inside Wabcut:        File > Run Python Script...
"""

import csv
import sys
from pathlib import Path

import wabcut


def main(session, shotlist, preset="1080p60", name="Rough cut"):
    with open(shotlist, newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    with session.transaction(f"Rough cut from {Path(shotlist).name}"):
        sequence = session.sequence.create(preset=preset, name=name).created_id
        for row in rows:
            # Where the sequence ends now: the next shot goes there.
            end = next(s["duration"] for s in session.project.summary()["sequences"] if s["id"] == sequence)
            session.clip.insert(sequence=sequence, media=row["media"], at=end, sourceIn=row["in"], sourceOut=row["out"])
            if row.get("title"):
                session.marker.add(sequence=sequence, time=end, name=row["title"], type="chapter")
    print(f"Made the sequence '{name}' from {len(rows)} shot(s).")
    return sequence


if __name__ == "__main__":
    project = sys.argv[1] if len(sys.argv) > 1 else None
    shotlist = sys.argv[2] if len(sys.argv) > 2 else Path(__file__).with_name("shotlist.csv")
    with wabcut.connect(project, edit=True, save="exit") as session:
        main(session, shotlist)
