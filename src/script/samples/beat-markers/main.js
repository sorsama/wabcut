// Beat Markers: a marker at every beat (or downbeat) of the active sequence's beat grid (docs/extensions.md).
// Arguments (headless runs, `wabcut-cli ext run ... --args`): {"every": "beat" | "downbeat", "bpm": 120, "color": 4}
// skip the prompt.

const COLORS = [
    {value: '0', label: 'Green'}, {value: '1', label: 'Red'}, {value: '2', label: 'Purple'},
    {value: '3', label: 'Orange'}, {value: '4', label: 'Yellow'}, {value: '6', label: 'Blue'},
];

function beatsOrNull() {
    try {
        return wabcut.project.query('sequence.beats', {limit: 100000});
    } catch (e) {
        if (e instanceof wabcut.CommandError) {
            return null; // no beat grid yet
        }
        throw e;
    }
}

/// The options from the arguments, or asked for: a Promise of {every, color, bpm} (null when cancelled).
function chooseOptions(args, needTempo) {
    if (args.every !== undefined && (!needTempo || args.bpm !== undefined)) {
        return Promise.resolve({every: args.every, bpm: args.bpm, color: String(args.color ?? 4)});
    }
    const fields = [
        {id: 'every', type: 'choice', label: 'Mark', value: wabcut.storage.get('every', 'beat'),
         options: [{value: 'beat', label: 'Every beat'}, {value: 'downbeat', label: 'Every downbeat (bar)'}]},
        {id: 'color', type: 'choice', label: 'Colour', value: wabcut.storage.get('color', '4'), options: COLORS},
    ];
    if (needTempo) {
        fields.unshift({id: 'bpm', type: 'number', label: 'Tempo (BPM)', value: 120, min: 20, max: 300, step: 1});
    }
    return wabcut.ui.prompt({
        title: 'Add Markers at Every Beat',
        message: needTempo ? 'The sequence has no beat grid yet: set its tempo first.' : undefined,
        submitLabel: 'Add Markers',
        fields,
    }).then((answer) => {
        if (answer !== null) {
            wabcut.storage.set('every', answer.every);
            wabcut.storage.set('color', answer.color);
        }
        return answer;
    });
}

function addMarkers(beats, chosen) {
    const time = beats.beatFields.indexOf('time');
    const inBar = beats.beatFields.indexOf('inBar');
    const index = beats.beatFields.indexOf('index');
    const picked = beats.beats.filter((b) => chosen.every !== 'downbeat' || b[inBar] === 1);
    if (picked.length === 0) {
        wabcut.ui.notify('The sequence has no beats to mark.', {level: 'warning'});
        return {markers: 0};
    }
    const steps = picked.map((b) => ({
        command: 'marker.add',
        params: {
            time: b[time],
            name: chosen.every === 'downbeat' ? `Bar ${Math.floor(b[index] / beats.beatsPerBar) + 1}` : `Beat ${b[index] + 1}`,
            colorIndex: Number(chosen.color),
        },
    }));
    wabcut.commands.batch(steps, {label: 'Add Markers at Every Beat'}); // one undo step
    wabcut.ui.toast(`${picked.length} beat markers added`);
    return {markers: picked.length, bpm: beats.bpm};
}

wabcut.ui.registerCommand('addBeatMarkers', 'Add Markers at Every Beat', (args) => {
    const beats = beatsOrNull();
    return chooseOptions(args, beats === null).then((chosen) => {
        if (chosen === null) {
            return {cancelled: true};
        }
        if (beats !== null) {
            return addMarkers(beats, chosen);
        }
        wabcut.commands.run('sequence.setBeatGrid', {bpm: Number(chosen.bpm)});
        return addMarkers(wabcut.project.query('sequence.beats', {limit: 100000}), chosen);
    });
});
