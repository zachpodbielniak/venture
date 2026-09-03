/*
 * check-hx-encoding.mjs - Prove that hx-encoding actually sends multipart
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Not part of `make test`, deliberately: it needs jsdom from npm, and a test
 * suite that reaches the network to run is a suite that fails offline and in
 * the container build. Run it by hand after touching venture-hx.js:
 *
 *   npm install jsdom && node tools/check-hx-encoding.mjs
 *
 * It exists because of a bug that no server-side test could have caught. The
 * knowledge-base import form carried hx-encoding="multipart/form-data", the
 * server accepted multipart, and curl proved the endpoint worked -- but
 * venture-hx.js is a compact hx-* client that did not implement hx-encoding
 * at all. It ignored the attribute, serialised the form as
 * application/x-www-form-urlencoded, and a File became the string
 * "[object File]". The upload had never worked in a browser, and the failure
 * surfaced as a 500 from a parser that was handed a body of the wrong shape.
 *
 * So this drives the real client against a real DOM and asserts the two
 * things that were wrong: the body is a FormData, and no Content-Type is set
 * -- only the browser knows the boundary it chose, and naming the type here
 * produces a request whose declared boundary does not match its body.
 */

import { JSDOM } from 'jsdom';
import { readFileSync } from 'fs';

const html = `<!doctype html><html><body>
  <form id="up" hx-post="/api/v1/kb/1/import" hx-encoding="multipart/form-data" hx-swap="none">
    <input type="file" name="files">
  </form>
  <form id="plain" hx-post="/plain" hx-swap="none">
    <input type="text" name="q" value="hello">
  </form>
</body></html>`;

const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://localhost/kb' });
const { window } = dom;

let captured = null;
window.fetch = (url, opts) => {
  captured = { url, opts };
  return Promise.resolve({
    ok: true, status: 200, statusText: 'OK',
    headers: { get: () => null },
    text: () => Promise.resolve('')
  });
};
window.AbortController = class { constructor(){ this.signal = {}; } abort(){} };

/*
 * jsdom has no DataTransfer, so the file itself is not attached. That is not
 * what is under test: the bug was the body's *shape*. A URLSearchParams body
 * stringifies a File to "[object File]", so what has to be proven is that the
 * request goes out as FormData with the browser choosing the boundary.
 */

const src = readFileSync(new URL('../data/static/venture-hx.js', import.meta.url), 'utf8');
window.eval(src);
window.document.dispatchEvent(new window.Event('DOMContentLoaded'));

const check = (id, label) => new Promise(res => {
  captured = null;
  window.document.getElementById(id).dispatchEvent(new window.Event('submit', { bubbles: true, cancelable: true }));
  setTimeout(() => {
    const ct = captured?.opts?.headers?.['Content-Type'];
    const body = captured?.opts?.body;
    const isFD = body && body.constructor && body.constructor.name === 'FormData';
    console.log(`  ${label}`);
    console.log(`    body is FormData : ${isFD}`);
    console.log(`    Content-Type set : ${ct === undefined ? 'no (browser picks boundary)' : ct}`);
    console.log(`    body class       : ${body ? body.constructor.name : 'none'}`);
    res();
  }, 60);
});

await check('up', 'form WITH hx-encoding=multipart/form-data');
await check('plain', 'form WITHOUT hx-encoding (must stay url-encoded)');
