// Fake npm registry for the P30 auto-download gate (PLAN §P30 slice 5). Zero npm dependencies:
// builds npm-shaped fixtures IN MEMORY from the repo's real python plugin manifest (passed as
// argv[2]), then serves abbreviated packuments + tarballs the way registry.npmjs.org would.
//
// Fixtures (all version 1.2.3):
//   @mintplayer/polyglot-target-pyfixture   — a valid plugin whose target is "pyfixture"
//                                             (the real python manifest renamed, so its emission
//                                             is byte-identical to --target python: the lockstep
//                                             byte-equality the gate asserts)
//   @mintplayer/polyglot-target-pyfixture2  — the packument LIES about the tarball's integrity
//                                             (supply-chain tamper: the CLI must refuse)
//
// Prints "READY <port>" once listening; exits when stdin closes (the harness dying kills it).
'use strict';
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const zlib = require('node:zlib');

const manifestPath = process.argv[2];
if (!manifestPath) { console.error('usage: node registry-server.js <polyglot-plugin.json>'); process.exit(2); }
const pythonManifest = fs.readFileSync(manifestPath, 'utf8');

// --- npm-shaped tarball built by hand (ustar entries under package/, gzipped) -------------------
function tarEntry(path, data) {
  const buf = Buffer.from(data);
  const h = Buffer.alloc(512);
  h.write(path, 0);
  h.write('000644 \0', 100);
  h.write('000000 \0', 108);
  h.write('000000 \0', 116);
  h.write(buf.length.toString(8).padStart(11, '0') + '\0', 124);
  h.write('00000000000\0', 136);
  h.fill(' ', 148, 156); // checksum field spaces while summing
  h.write('0', 156);
  h.write('ustar\0' + '00', 257);
  let sum = 0;
  for (const b of h) sum += b;
  h.write(sum.toString(8).padStart(6, '0') + '\0 ', 148);
  const pad = Buffer.alloc((512 - (buf.length % 512)) % 512);
  return Buffer.concat([h, buf, pad]);
}
function npmTarball(files) {
  const parts = Object.entries(files).map(([p, d]) => tarEntry('package/' + p, d));
  parts.push(Buffer.alloc(1024));
  return zlib.gzipSync(Buffer.concat(parts), { level: 9 });
}
const sri = (buf) => 'sha512-' + crypto.createHash('sha512').update(buf).digest('base64');

// --- fixtures ------------------------------------------------------------------------------------
function renamed(name) {
  return pythonManifest.split('"name": "python"').join(`"name": "${name}"`);
}
function fixture(target, lieAboutIntegrity) {
  const name = `@mintplayer/polyglot-target-${target}`;
  const tgz = npmTarball({
    'package.json': JSON.stringify({ name, version: '1.2.3', files: ['polyglot-plugin.json'] }),
    'polyglot-plugin.json': renamed(target),
  });
  return {
    name,
    tgz,
    tarballPath: `/t/${target}-1.2.3.tgz`,
    packument: (base) => JSON.stringify({
      name,
      'dist-tags': { latest: '1.2.3' },
      versions: {
        '1.2.3': { name, version: '1.2.3', dist: {
          tarball: `${base}/t/${target}-1.2.3.tgz`,
          integrity: lieAboutIntegrity ? sri(Buffer.from('not the real bytes')) : sri(tgz),
        } },
      },
    }),
  };
}
const fixtures = [fixture('pyfixture', false), fixture('pyfixture2', true)];

// --- server --------------------------------------------------------------------------------------
const server = http.createServer((req, res) => {
  const base = `http://127.0.0.1:${server.address().port}`;
  // The CLI (like npm) encodes only the scope slash (%2F) — match on the decoded path.
  const path = decodeURIComponent(req.url);
  for (const f of fixtures) {
    if (path === '/' + f.name) {
      res.setHeader('content-type', 'application/vnd.npm.install-v1+json');
      return res.end(f.packument(base));
    }
    if (path === f.tarballPath) {
      res.setHeader('content-type', 'application/octet-stream');
      return res.end(f.tgz);
    }
  }
  res.statusCode = 404;
  res.end('{"error":"Not found"}');
});
server.listen(0, '127.0.0.1', () => console.log(`READY ${server.address().port}`));
process.stdin.on('end', () => process.exit(0));
process.stdin.resume();
