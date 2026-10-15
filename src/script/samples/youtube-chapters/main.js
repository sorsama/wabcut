// YouTube Chapters: the chapter markers of every sequence as YouTube description text, ready to copy
// (docs/extensions.md). Read only. Arguments (headless): {"quiet": true} returns the text without showing it;
// {"sequences": [ids]} limits the batch.

function describe(args) {
    const sequences = wabcut.project.summary().sequences
        .filter((s) => !Array.isArray(args.sequences) || args.sequences.includes(s.id));
    const parts = [];
    const warnings = [];
    for (const seq of sequences) {
        const chapters = wabcut.project.query('sequence.exportMarkers', {sequence: seq.id, format: 'youtube'});
        if (chapters.count === 0 || chapters.text.trim() === '') {
            continue;
        }
        parts.push(sequences.length > 1 ? `${seq.name}\n${chapters.text.trim()}` : chapters.text.trim());
        for (const w of chapters.warnings) {
            warnings.push(`${seq.name}: ${w}`);
        }
    }
    return {text: parts.join('\n\n'), sequences: parts.length, warnings};
}

wabcut.ui.registerCommand('chaptersToDescription', 'YouTube Description from Chapters', (args) => {
    const result = describe(args);
    if (result.sequences === 0) {
        wabcut.ui.notify('No sequence has markers to turn into chapters.', {level: 'warning'});
        return result;
    }
    wabcut.storage.set('lastDescription', result.text);
    if (args.quiet) {
        return result;
    }
    return wabcut.ui.prompt({
        title: 'YouTube Description',
        message: result.warnings.length > 0 ? result.warnings.join('\n') : 'Copy the chapters into the video description.',
        submitLabel: 'Done',
        fields: [{id: 'text', type: 'text', label: 'Chapters', value: result.text, multiline: true, readOnly: true}],
    }).then(() => result);
});
