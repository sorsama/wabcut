# wabcut-name: Export markers
# wabcut-description: Writes the chapter list for a video description and the markers as a spreadsheet, next to the project.
# wabcut-permissions: Read
"""Write the sequence's markers to files: YouTube chapters (youtube-chapters.txt) and a CSV (markers.csv).

It only reads the project, so no permission to edit is needed and nothing in the project changes.

From a terminal:      python export_markers.py film.wabcut [output folder] [sequence name or id]
Inside Wabcut:        File > Run Python Script...  (the sequence open in the timeline; the files go next to the script)
"""

import sys
from pathlib import Path

import wabcut


def main(session, folder):
    folder = Path(folder)
    chapters = session.sequence.exportMarkers(format="youtube", chaptersOnly=True)
    (folder / "youtube-chapters.txt").write_text(chapters["text"], encoding="utf-8")
    for warning in chapters.get("warnings", []):
        print("YouTube:", warning)  # e.g. the first chapter must be at 0:00
    table = session.sequence.exportMarkers(format="csv")
    (folder / "markers.csv").write_text(table["text"], encoding="utf-8")
    print(f"Wrote youtube-chapters.txt and markers.csv in {folder}.")


if __name__ == "__main__":
    project = sys.argv[1] if len(sys.argv) > 1 else None
    folder = sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parent
    sequence = sys.argv[3] if len(sys.argv) > 3 else None  # needed when the project has several sequences
    with wabcut.connect(project, sequence=sequence) as session:  # read-only
        main(session, folder)
