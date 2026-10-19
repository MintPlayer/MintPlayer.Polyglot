// P40 A3 — the number-text corpus: "<16-hex-bits> <String(x)>" per line, ECMAScript Number::toString being
// the reference the C#, Python and PHP helpers must reproduce. Ported from the slice-0 spike
// (docs/prd/issue-73-library-grade/spikes/F64Exact/gen.js), with a SEEDED generator so every run checks the
// same ~237k doubles: the edge values, every power of two (the asymmetric-interval case "R" got wrong on
// .NET 10.0.13), 1eN/5eN.., subnormals, random bit patterns, short "human" decimals and integers.
//
// Usage: node tests/numfmt/gen-corpus.js <out-file>
'use strict';
const fs = require('fs');

let state = 0x9E3779B97F4A7C15n; // xorshift64*, fixed seed
const MASK = (1n << 64n) - 1n;
function next64() {
  state ^= state >> 12n; state ^= (state << 25n) & MASK; state ^= state >> 27n;
  return (state * 0x2545F4914F6CDD1Dn) & MASK;
}
function rand() { return Number(next64() >> 11n) / 2 ** 53; } // [0, 1)

const buf = new ArrayBuffer(8), dv = new DataView(buf);
const out = [];
function addD(x) { dv.setFloat64(0, x); out.push(dv.getBigUint64(0).toString(16).padStart(16, '0') + ' ' + String(x)); }
function addBits(b) { dv.setBigUint64(0, b); addD(dv.getFloat64(0)); }

[0, -0, NaN, Infinity, -Infinity, Number.MIN_VALUE, -Number.MIN_VALUE, Number.MAX_VALUE, Number.EPSILON,
 2.2250738585072014e-308, 2.225073858507201e-308, 0.1, 0.2, 0.1 + 0.2, 1 / 3, 2 / 3, 123456789012345680000, 1e21,
 1e-7, 1.5e-7, 9.999999999999999e20, 5e-324, 1.7976931348623157e308, 4.35, 0.3, 2.675, 1.005, 100, 1e15, 1e16,
 1e17, 9007199254740993, 9007199254740992].forEach(addD);
for (let e = -1074; e <= 1023; e++) { addD(Math.pow(2, e)); addD(-Math.pow(2, e)); addD(Math.pow(2, e) * 3); }
for (let n = -324; n <= 308; n++) { for (const m of [1, 5, 2, 3, 7, 9]) addD(Number(m + 'e' + n)); }
for (let m = 1n; m < 2000n; m++) addBits(m);                                   // the smallest subnormals
for (let i = 0; i < 5000; i++) addBits(next64() & 0x000FFFFFFFFFFFFFn);        // random subnormals
for (let i = 0; i < 150000; i++) addBits(next64());                            // random bit patterns
for (let i = 0; i < 50000; i++) {                                              // short "human" decimals
  const d = Math.floor(rand() * 1e6); const e = Math.floor(rand() * 40) - 20; addD(Number(d + 'e' + e));
}
for (let i = 0; i < 20000; i++) addD(rand() * Math.pow(10, Math.floor(rand() * 25))); // plain-range values
// f32 literals widened to f64 (S13): the text a widened f32 prints must follow the same rule.
for (let i = 0; i < 10000; i++) addD(Math.fround(Number(Math.floor(rand() * 1e6) + 'e' + (Math.floor(rand() * 60) - 30))));

fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
console.log('corpus: ' + out.length + ' doubles');
