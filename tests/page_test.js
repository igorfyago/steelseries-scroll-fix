// page_test.js : checks how the page colours held wheel ticks. Cuts
// isHeldTick, eventClass and settle out of web/index.html and feeds them the
// rows the program sends in the two shapes where a timer settles two held
// ticks: a slow reversal and the next tick timing out (the misread is the
// OLDER row), and the first two ticks since the program started disagreeing
// with nothing after them (both dropped).
//   node tests\page_test.js web\index.html     exit 1 on a wrong colour
// Optional: needs Node.js, which nothing else here does.
'use strict';
const html = require('fs').readFileSync(process.argv[2], 'utf8');
function cut(name) {
  const at = html.indexOf('function ' + name + '(');
  let i = html.indexOf('{', at), depth = 0;
  for (; i < html.length; i++) { if (html[i] === '{') depth++; else if (html[i] === '}') { if (--depth === 0) break; } }
  return html.slice(at, i + 1);
}
const page = new Function([cut('isHeldTick'), cut('eventClass'), cut('settle')].join('\n') + '\nreturn {eventClass, settle};')();
const tick = (s, t, w, v) => ({ s, t, k: 1, b: 0, d: 0, w, src: 0, x: v ? 1 : 0, v: v || 0, st: 0, sd: 0, g: -1, dev: 1, j: 1 });
const timer = (s, t, st, sd) => ({ s, t, k: 3, b: 0, d: 0, w: 0, src: 0, x: 0, v: 0, st, sd, g: -1, dev: 0, j: 1 });
const sent = (s, t, w) => ({ s, t, k: 1, b: 0, d: 0, w, src: 2, x: 0, v: 0, st: 0, sd: 0, g: -1, dev: 0, j: 0 });
let failed = 0;
function expect(name, rows, want) {
  page.settle(rows);
  for (const [s, cls] of Object.entries(want)) {
    const got = page.eventClass(rows.find(r => r.s === +s));
    if (got !== cls) { failed++; console.log('FAIL ' + name + ': row ' + s + ' is "' + got + '", should be "' + cls + '"'); }
  }
}
// Owner settings. 0 and 50 down start the stroke; 1500 up is a misread after a
// pause (held reversal), 1600 down disagrees (pair); nothing for 200 ms: the up
// goes out turned round, the down as it came. The rows in the program's order.
expect('slow reversal pair times out', [
  tick(1, 0, -120, 7), Object.assign(tick(2, 50, -120, 0), { st: 7 }), sent(3, 50, -120),
  tick(4, 1500, 120, 5), tick(5, 1600, -120, 7),
  timer(6, 1800, 5, 120), timer(7, 1800, 6, 0), sent(8, 1800, -120), sent(9, 1800, -120),
], { 1: '', 4: 'dropped', 5: '' });
// Program start: up, then down 100 ms later, nothing after: neither goes out.
// A new stroke 5 s later must not sweep the first one up as delivered.
expect('first pair since start let go whole', [
  tick(1, 0, 120, 7), tick(2, 100, -120, 7), timer(3, 300, 4, 0),
  tick(4, 5000, -120, 7), Object.assign(tick(5, 5050, -120, 0), { st: 7 }), sent(6, 5050, -120),
], { 1: 'dropped', 2: 'dropped', 4: '' });
console.log(failed ? failed + ' wrong colour(s)' : 'all colours right');
process.exit(failed ? 1 : 0);
