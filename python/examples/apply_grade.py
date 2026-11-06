# wabcut-name: Grade every clip of a media
# wabcut-description: Gives every clip that uses one media file the same colour grade, in every sequence, as one undo step.
# wabcut-permissions: Read, EditSequence
"""Apply one colour grade to every clip that uses a media file.

From a terminal:      python apply_grade.py film.wabcut gameplay.mp4
Inside Wabcut:        File > Run Python Script... (it grades MEDIA below)
"""

import sys

import wabcut

MEDIA = "gameplay.mp4"  # the media's name in the project (or its id)
GRADE = {"basic.exposure": 0.3, "basic.contrast": 12, "basic.saturation": 110}  # effect.list lists the keys


def main(session, media=MEDIA):
    graded = 0
    with session.transaction(f"Grade {media}"):  # one undo step for all sequences
        for sequence in session.project.summary()["sequences"]:
            clips = wabcut.records(session.clip.find(media=media, sequence=sequence["id"], limit=1000), "clips")
            if clips:
                session.color.grade.set(clips=[c["id"] for c in clips], params=GRADE, sequence=sequence["id"])
                graded += len(clips)
    print(f"Graded {graded} clip(s) of {media}.")
    return graded


if __name__ == "__main__":
    # Inside Wabcut there is no argument: connect() gives the open project. From a terminal, name the project file.
    project = sys.argv[1] if len(sys.argv) > 1 else None
    media = sys.argv[2] if len(sys.argv) > 2 else MEDIA
    with wabcut.connect(project, edit=True, save="exit") as session:
        main(session, media)
