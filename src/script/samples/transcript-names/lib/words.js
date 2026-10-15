// Shared helpers of the Transcript Clip Names sample (evaluated before main.js; scripts share the global scope).

/// Every word of the sequence's transcript, read in pages, as objects {text, clip, ...}.
function sequenceWords() {
    const words = [];
    let first = 0;
    for (;;) {
        const page = wabcut.project.query('transcript.words', {first, limit: 5000});
        const fields = page.wordFields;
        for (const row of page.words) {
            const word = {};
            fields.forEach((name, i) => { word[name] = row[i]; });
            words.push(word);
        }
        if (page.words.length < 5000) {
            return words;
        }
        first = words[words.length - 1].index + 1;
    }
}

/// "So, um... welcome to the show!" -> "So um welcome": the first `count` words without punctuation.
function nameFrom(words, count) {
    const punctuation = /[.,!?;:"()\[\]{}…“”¿¡]+/g;
    const cleaned = words.map((w) => w.text.replace(punctuation, '')).filter((t) => t.length > 0);
    const name = cleaned.slice(0, count).join(' ');
    return name.length > 60 ? `${name.slice(0, 59)}…` : name;
}
