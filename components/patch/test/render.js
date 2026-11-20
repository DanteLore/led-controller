// The same as render.c, using patch.js. Usage: node render.js <file.json> <leds>
const fs = require('fs');
const Patch = require('../patch.js');

const TIMES_MS = [0, 1, 250, 999, 1000, 4321, 12345, 29999, 30000, 61234, 89999, 123456, 3600000, 86400017];
const [file, leds] = process.argv.slice(2);
let set;
try {
    set = Patch.parse(JSON.parse(fs.readFileSync(file, 'utf8')));
} catch (e) {
    console.log('error ' + (e instanceof SyntaxError ? 'file: not valid JSON' : e.message));
    process.exit(1);
}
const count = Number(leds);
const frame = [];
const out = [];
for (let p = 0; p < set.count; p++)
    for (const t of TIMES_MS) {
        Patch.render(set, p, t, t * 2, frame, count);   // beat clock at 120 BPM
        for (let i = 0; i < count; i++)
            out.push(`${set.patches[p].name} ${t} ${i} ${frame[i].join(' ')}`);
    }
console.log(out.join('\n'));
