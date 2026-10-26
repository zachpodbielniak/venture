/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Keep the real request lifecycle; isolate DOM serialization and painting. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const tick = () => new Promise(resolve => setImmediate(resolve));
async function scenario(ignoreAbort) {
    const requests = [];
    const document = {readyState: 'loading', addEventListener() {}, body: {}};
    const window = {location: {href: 'http://example.test/list'}, setTimeout(fn) { fn(); }};
    const fetch = (url, options) => new Promise((resolve, reject) => {
        requests.push({signal: options.signal, reply(text) { resolve({ok: true, headers: {get() { return null; }}, text: async () => text}); }});
        options.signal.addEventListener('abort', () => {
            if (!ignoreAbort) reject(Object.assign(new Error('cancelled'), {name: 'AbortError'}));
        });
    });
    const expose = `window.testIssue = issueRequest;
        collectValues = function () { return new URLSearchParams(); };
        resolveTarget = function () { return document.body; };
        setRequesting = function (el, busy) { el.busy = busy; };
        trigger = function () { return true; };
        performSwap = function (target, html) { target.text = html; };
        window.htmx = {`;
    const source = fs.readFileSync(process.env.HX_TEST_SOURCE || 'data/static/venture-hx.js', 'utf8').replace('window.htmx = {', expose);
    vm.runInNewContext(source, {window, document, fetch, AbortController, URLSearchParams, WeakMap, console});
    const form = {getAttribute() { return null; }};
    window.testIssue(form, 'get', '/list?search=first');
    window.testIssue(form, 'get', '/list?search=second');
    await tick();
    assert.equal(form.busy, true, 'an aborted predecessor cannot clear the successor busy state');
    window.testIssue(form, 'get', '/list?search=latest');
    assert.equal(requests[1].signal.aborted, true, 'the latest query still owns and cancels its predecessor');
    requests[2].reply('latest');
    await tick();
    assert.equal(document.body.text, 'latest');
    requests[1].reply('stale');
    requests[0].reply('oldest');
    await tick();
    assert.equal(document.body.text, 'latest', 'late responses must never replace newer results');
    assert.equal(form.busy, false);
}
(async () => {
    await scenario(false);
    await scenario(true);
    console.log('Overlapping search: cancellation ownership and stale responses passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
