// Generates corpus.txt: "<16-hex-bits> <String(x)>" per line.
const fs = require('fs');
const crypto = require('crypto');
const buf = new ArrayBuffer(8), dv = new DataView(buf);
const out = [];
function addD(x) { dv.setFloat64(0, x); out.push(dv.getBigUint64(0).toString(16).padStart(16, '0') + ' ' + String(x)); }
function addBits(b) { dv.setBigUint64(0, b); addD(dv.getFloat64(0)); }
[0, -0, NaN, Infinity, -Infinity, Number.MIN_VALUE, -Number.MIN_VALUE, Number.MAX_VALUE, Number.EPSILON,
 2.2250738585072014e-308, 2.225073858507201e-308, 0.1, 0.2, 0.1 + 0.2, 1 / 3, 2 / 3, 123456789012345680000, 1e21, 1e-7, 1.5e-7, 9.999999999999999e20,
 5e-324, 1.7976931348623157e308, 4.35, 0.3, 2.675, 1.005, 100, 1e15, 1e16, 1e17, 9007199254740993, 9007199254740992].forEach(addD);
for (let e = -1074; e <= 1023; e++) { addD(Math.pow(2, e)); addD(-Math.pow(2, e)); addD(Math.pow(2, e) * 3); }
for (let n = -324; n <= 308; n++) { for (const m of [1, 5, 2, 3, 7, 9]) addD(Number(m + 'e' + n)); }
// subnormals: small mantissas and random mantissas with exponent 0
for (let m = 1n; m < 2000n; m++) addBits(m);
for (let i = 0; i < 5000; i++) addBits(crypto.randomBytes(8).readBigUInt64BE() & 0x000FFFFFFFFFFFFFn);
// random bit patterns
for (let i = 0; i < 150000; i++) addBits(crypto.randomBytes(8).readBigUInt64BE());
// random "human" decimals (short digit strings), the common case
for (let i = 0; i < 50000; i++) { const d = Math.floor(Math.random() * 1e6); const e = Math.floor(Math.random() * 40) - 20; addD(Number(d + 'e' + e)); }
// integers & near-integers in the plain range
for (let i = 0; i < 20000; i++) addD(Math.random() * Math.pow(10, Math.floor(Math.random() * 25)));
fs.writeFileSync(process.argv[2], out.join('\n') + '\n');
console.log('lines', out.length);
