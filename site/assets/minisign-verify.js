// Self-contained minisign verifier for the browser.
// Verifies a prehashed ("ED") minisign signature: BLAKE2b-512(file) then Ed25519.
// Ed25519 via WebCrypto (secure contexts); BLAKE2b-512 bundled below.
// BLAKE2b adapted from blakejs (github.com/dcposch/blakejs), public domain (CC0).
(function (root) {
  'use strict';

  var BLAKE2B_IV32 = new Uint32Array([
    0xf3bcc908, 0x6a09e667, 0x84caa73b, 0xbb67ae85, 0xfe94f82b, 0x3c6ef372,
    0x5f1d36f1, 0xa54ff53a, 0xade682d1, 0x510e527f, 0x2b3e6c1f, 0x9b05688c,
    0xfb41bd6b, 0x1f83d9ab, 0x137e2179, 0x5be0cd19
  ]);
  var SIGMA8 = [
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, 14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3,
    11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4, 7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8,
    9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13, 2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9,
    12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11, 13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10,
    6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5, 10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, 14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3
  ];
  var SIGMA82 = new Uint8Array(SIGMA8.map(function (x) { return x * 2; }));
  var v = new Uint32Array(32);
  var m = new Uint32Array(32);

  function ADD64AA(vv, a, b) {
    var o0 = vv[a] + vv[b], o1 = vv[a + 1] + vv[b + 1];
    if (o0 >= 0x100000000) o1++;
    vv[a] = o0; vv[a + 1] = o1;
  }
  function ADD64AC(vv, a, b0, b1) {
    var o0 = vv[a] + b0;
    if (b0 < 0) o0 += 0x100000000;
    var o1 = vv[a + 1] + b1;
    if (o0 >= 0x100000000) o1++;
    vv[a] = o0; vv[a + 1] = o1;
  }
  function B2B_GET32(arr, i) {
    return arr[i] ^ (arr[i + 1] << 8) ^ (arr[i + 2] << 16) ^ (arr[i + 3] << 24);
  }
  function B2B_G(a, b, c, d, ix, iy) {
    var x0 = m[ix], x1 = m[ix + 1], y0 = m[iy], y1 = m[iy + 1];
    ADD64AA(v, a, b); ADD64AC(v, a, x0, x1);
    var xor0 = v[d] ^ v[a], xor1 = v[d + 1] ^ v[a + 1];
    v[d] = xor1; v[d + 1] = xor0;
    ADD64AA(v, c, d);
    xor0 = v[b] ^ v[c]; xor1 = v[b + 1] ^ v[c + 1];
    v[b] = (xor0 >>> 24) ^ (xor1 << 8); v[b + 1] = (xor1 >>> 24) ^ (xor0 << 8);
    ADD64AA(v, a, b); ADD64AC(v, a, y0, y1);
    xor0 = v[d] ^ v[a]; xor1 = v[d + 1] ^ v[a + 1];
    v[d] = (xor0 >>> 16) ^ (xor1 << 16); v[d + 1] = (xor1 >>> 16) ^ (xor0 << 16);
    ADD64AA(v, c, d);
    xor0 = v[b] ^ v[c]; xor1 = v[b + 1] ^ v[c + 1];
    v[b] = (xor1 >>> 31) ^ (xor0 << 1); v[b + 1] = (xor0 >>> 31) ^ (xor1 << 1);
  }
  function compress(ctx, last) {
    var i;
    for (i = 0; i < 16; i++) { v[i] = ctx.h[i]; v[i + 16] = BLAKE2B_IV32[i]; }
    v[24] = v[24] ^ ctx.t; v[25] = v[25] ^ (ctx.t / 0x100000000);
    if (last) { v[28] = ~v[28]; v[29] = ~v[29]; }
    for (i = 0; i < 32; i++) m[i] = B2B_GET32(ctx.b, 4 * i);
    for (i = 0; i < 12; i++) {
      B2B_G(0, 8, 16, 24, SIGMA82[i * 16 + 0], SIGMA82[i * 16 + 1]);
      B2B_G(2, 10, 18, 26, SIGMA82[i * 16 + 2], SIGMA82[i * 16 + 3]);
      B2B_G(4, 12, 20, 28, SIGMA82[i * 16 + 4], SIGMA82[i * 16 + 5]);
      B2B_G(6, 14, 22, 30, SIGMA82[i * 16 + 6], SIGMA82[i * 16 + 7]);
      B2B_G(0, 10, 20, 30, SIGMA82[i * 16 + 8], SIGMA82[i * 16 + 9]);
      B2B_G(2, 12, 22, 24, SIGMA82[i * 16 + 10], SIGMA82[i * 16 + 11]);
      B2B_G(4, 14, 16, 26, SIGMA82[i * 16 + 12], SIGMA82[i * 16 + 13]);
      B2B_G(6, 8, 18, 28, SIGMA82[i * 16 + 14], SIGMA82[i * 16 + 15]);
    }
    for (i = 0; i < 16; i++) ctx.h[i] = ctx.h[i] ^ v[i] ^ v[i + 16];
  }
  function blake2b512(input) {
    var ctx = { b: new Uint8Array(128), h: new Uint32Array(16), t: 0, c: 0, outlen: 64 };
    var i;
    for (i = 0; i < 16; i++) ctx.h[i] = BLAKE2B_IV32[i];
    ctx.h[0] ^= 0x01010000 ^ 64; // no key, 64-byte output
    for (i = 0; i < input.length; i++) {
      if (ctx.c === 128) { ctx.t += ctx.c; compress(ctx, false); ctx.c = 0; }
      ctx.b[ctx.c++] = input[i];
    }
    ctx.t += ctx.c;
    while (ctx.c < 128) ctx.b[ctx.c++] = 0;
    compress(ctx, true);
    var out = new Uint8Array(64);
    for (i = 0; i < 64; i++) out[i] = (ctx.h[i >> 2] >> (8 * (i & 3))) & 0xff;
    return out;
  }

  function b64(s) {
    var bin = (typeof atob === 'function') ? atob(s) : Buffer.from(s, 'base64').toString('binary');
    var out = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  function concat(a, b) { var o = new Uint8Array(a.length + b.length); o.set(a, 0); o.set(b, a.length); return o; }

  // Parse a minisign public-key blob (2-line .pub file, or just the base64 line).
  function parsePub(text) {
    var lines = text.trim().split(/\r?\n/);
    var line = lines.length >= 2 ? lines[1].trim() : lines[0].trim();
    var raw = b64(line);
    if (raw.length !== 42) throw new Error('bad public key length');
    return { algo: String.fromCharCode(raw[0], raw[1]), keyId: raw.slice(2, 10), key: raw.slice(10, 42) };
  }
  // Parse a .minisig file.
  function parseSig(text) {
    var lines = text.trim().split(/\r?\n/);
    if (lines.length < 4) throw new Error('bad signature file');
    var raw = b64(lines[1].trim());
    var trustedPrefix = 'trusted comment: ';
    var tc = lines[2].indexOf(trustedPrefix) === 0 ? lines[2].slice(trustedPrefix.length) : '';
    return {
      algo: String.fromCharCode(raw[0], raw[1]),
      keyId: raw.slice(2, 10),
      sig: raw.slice(10, 74),
      trustedComment: tc,
      globalSig: b64(lines[3].trim())
    };
  }
  function eqBytes(a, b) { if (a.length !== b.length) return false; var d = 0; for (var i = 0; i < a.length; i++) d |= a[i] ^ b[i]; return d === 0; }

  function getSubtle() {
    if (typeof crypto !== 'undefined' && crypto.subtle) return crypto.subtle;
    return null;
  }
  async function ed25519Verify(subtle, pub32, sig64, msg) {
    var key = await subtle.importKey('raw', pub32, { name: 'Ed25519' }, false, ['verify']);
    return subtle.verify({ name: 'Ed25519' }, key, sig64, msg);
  }

  // Verify fileBytes against minisigText using pubText. Resolves to a result object.
  async function verify(fileBytes, minisigText, pubText) {
    var subtle = getSubtle();
    if (!subtle) return { ok: false, reason: 'unavailable', message: 'This browser cannot verify signatures (no Web Crypto).' };
    var pub, sig;
    try { pub = parsePub(pubText); sig = parseSig(minisigText); }
    catch (e) { return { ok: false, reason: 'malformed', message: 'Signature or key is malformed.' }; }
    if (!eqBytes(pub.keyId, sig.keyId)) return { ok: false, reason: 'wrongkey', message: 'Signature was made by a different key.' };
    if (sig.algo !== 'ED') return { ok: false, reason: 'algo', message: 'Unexpected signature type (' + sig.algo + ').' };
    var msg = blake2b512(fileBytes); // prehashed
    var okFile, okComment;
    try {
      okFile = await ed25519Verify(subtle, pub.key, sig.sig, msg);
      okComment = await ed25519Verify(subtle, pub.key, sig.globalSig, concat(sig.sig, new TextEncoder().encode(sig.trustedComment)));
    } catch (e) {
      return { ok: false, reason: 'crypto', message: 'Verification failed to run: ' + (e && e.message || e) };
    }
    if (okFile && okComment) return { ok: true, reason: 'ok', trustedComment: sig.trustedComment, message: 'Signature verified.' };
    return { ok: false, reason: 'invalid', message: 'Signature does NOT match - do not trust this file.' };
  }

  var api = { verify: verify, blake2b512: blake2b512 };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.MinisignVerify = api;
})(typeof self !== 'undefined' ? self : this);
