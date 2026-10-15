// Transcript Clip Names: names each clip after the first words heard in it (docs/extensions.md). The transcript must
// exist already (Transcribe in Wabcut); this extension only reads it, so it needs no AI permission.
// Arguments (headless): {"words": 4} skips the prompt.

function askWordCount(args) {
    if (args.words !== undefined) {
        return Promise.resolve(Number(args.words));
    }
    return wabcut.ui.prompt({
        title: 'Rename Clips from Transcript',
        submitLabel: 'Rename',
        fields: [{id: 'words', type: 'number', label: 'Words per name', value: wabcut.storage.get('words', 4),
                  min: 1, max: 12, step: 1}],
    }).then((answer) => {
        if (answer === null) {
            return null;
        }
        wabcut.storage.set('words', answer.words);
        return answer.words;
    });
}

function rename(count) {
    const words = sequenceWords();
    if (words.length === 0) {
        wabcut.ui.notify('The sequence has no transcript yet: transcribe it first.', {level: 'warning'});
        return {renamed: 0};
    }
    // Words by the audio clip they are heard on.
    const byClip = new Map();
    for (const w of words) {
        if (!byClip.has(w.clip)) {
            byClip.set(w.clip, []);
        }
        byClip.get(w.clip).push(w);
    }
    // Video clips take the words of an audio clip linked to them (same link group).
    const summary = wabcut.project.query('sequence.summary', {maxClips: 100000});
    const id = summary.clipFields.indexOf('id');
    const link = summary.clipFields.indexOf('link');
    const audioOfLink = new Map();
    for (const track of summary.tracks) {
        for (const clip of track.clips) {
            if (byClip.has(clip[id]) && clip[link] !== null) {
                audioOfLink.set(clip[link], clip[id]);
            }
        }
    }
    const steps = [];
    for (const track of summary.tracks) {
        for (const clip of track.clips) {
            const source = byClip.has(clip[id]) ? clip[id] : audioOfLink.get(clip[link]);
            const name = source === undefined ? '' : nameFrom(byClip.get(source), count);
            if (name !== '') {
                steps.push({command: 'clip.rename', params: {clips: [clip[id]], name}});
            }
        }
    }
    if (steps.length === 0) {
        return {renamed: 0};
    }
    wabcut.commands.batch(steps, {label: 'Rename Clips from Transcript'});
    wabcut.ui.toast(`${steps.length} clips renamed`);
    return {renamed: steps.length};
}

wabcut.ui.registerCommand('renameFromTranscript', 'Rename Clips from Transcript', (args) =>
    askWordCount(args).then((count) => (count === null ? {cancelled: true} : rename(count))));
