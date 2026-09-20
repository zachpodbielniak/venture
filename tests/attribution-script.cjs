/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Run with node; the browser boundary is exercised without a network or DOM library. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const calls = [], storage = new Map();
let reads = 0, writes = 0, failWithdrawal = false;
const sandbox = {
    URL, Date, JSON, Error, Promise, Object,
    document: {currentScript: {src: 'https://venture.example.test/attribution.js', dataset: {site: 'configured-uuid'}}, referrer: 'https://ref.example.test/search?q=private'},
    location: {href: 'https://site.example.test/page?utm_source=newsletter'},
    crypto: {randomUUID: () => 'stable-random-id'},
    localStorage: {getItem(key) {reads++; return storage.get(key) || null;}, setItem(key, value) {writes++; storage.set(key, value);}, removeItem(key) {writes++; storage.delete(key);}},
    fetch: async (url, options) => {
        calls.push({url, options, body: JSON.parse(options.body)});
        if (failWithdrawal && url.endsWith('/withdraw')) throw new Error('offline');
        return {ok: true, status: 200, json: async () => url.endsWith('/grant') ? {token: 'a'.repeat(64), expires_at: '2999-01-01T00:00:00Z'} : {accepted: true}};
    }
};
sandbox.window = sandbox;
vm.runInNewContext(fs.readFileSync('data/static/attribution.js', 'utf8'), sandbox);
(async () => {
    const tracker = sandbox.VentureAttribution;
    assert.equal(calls.length, 0); assert.equal(reads, 0); assert.equal(writes, 0);
    await tracker.page(); assert.equal(calls.length, 0);
    const plain = tracker.createSubmission({name: 'Inquiry', email: 'person@example.test'});
    assert.equal(plain.visitor_token, undefined); assert.equal(plain.marketing, undefined);
    await tracker.submit(plain); assert.equal(calls.length, 1); assert.equal(reads, 0);
    await tracker.grant('analytics-v1');
    assert.equal(calls.filter(call => call.url.endsWith('/grant')).length, 1);
    await tracker.page('first-page');
    const observation = calls.at(-1); assert.equal(observation.body.event_id, 'first-page'); assert.equal(observation.body.fields.utm_source, 'newsletter');
    assert.equal(observation.options.credentials, 'omit'); assert.equal(observation.options.headers['Content-Type'], 'text/plain;charset=UTF-8');
    const envelope = tracker.createSubmission({name: 'Consented', email: 'person@example.test'});
    assert.equal(envelope.visitor_token, 'a'.repeat(64)); assert.equal(envelope.marketing, undefined);
    await tracker.submit(envelope); await tracker.submit(envelope);
    assert.equal(calls.at(-1).options.body, calls.at(-2).options.body);
    failWithdrawal = true;
    await assert.rejects(tracker.withdraw(), /offline/);
    const count = calls.length; await tracker.page(); assert.equal(calls.length, count);
    assert.equal(JSON.parse([...storage.values()][0]).pending_withdrawal, true);
    failWithdrawal = false; await tracker.withdraw(); assert.equal(storage.size, 0);
    await tracker.page(); assert.equal(calls.at(-1).url.endsWith('/withdraw'), true);
    let release;
    const normalFetch = sandbox.fetch;
    sandbox.fetch = async (url, options) => {
        if (url.endsWith('/grant')) return new Promise(resolve => {release = () => resolve({ok: true, status: 200, json: async () => ({token: 'b'.repeat(64), expires_at: '2999-01-01T00:00:00Z'})});});
        return normalFetch(url, options);
    };
    const granting = tracker.grant('analytics-v2');
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(typeof release, 'function');
    const withdrawing = tracker.withdraw();
    release(); await granting; await withdrawing;
    const afterWithdrawal = calls.length;
    await tracker.page(); assert.equal(calls.length, afterWithdrawal); assert.equal(storage.size, 0);
    let firstRelease, requests = 0;
    sandbox.fetch = async (url, options) => {
        if (!url.endsWith('/grant')) return normalFetch(url, options);
        requests++;
        const response = token => ({ok: true, status: 200, json: async () => ({token: token.repeat(64), expires_at: '2999-01-01T00:00:00Z'})});
        if (requests === 1) return new Promise(resolve => {firstRelease = () => resolve(response('c'));});
        return response('d');
    };
    const firstGrant = tracker.grant('first-policy');
    await new Promise(resolve => setImmediate(resolve));
    const secondGrant = tracker.grant('second-policy');
    await new Promise(resolve => setImmediate(resolve));
    firstRelease(); await Promise.all([firstGrant, secondGrant]);
    assert.equal(JSON.parse([...storage.values()][0]).policy, 'second-policy');
    await tracker.withdraw();
    sandbox.fetch = normalFetch;
    await tracker.grant('expired-policy');
    sandbox.fetch = async (url, options) => url.endsWith('/withdraw') ? {ok: false, status: 404, json: async () => ({})} : normalFetch(url, options);
    await tracker.withdraw();
    assert.equal(storage.size, 0);
    await tracker.grant('current-policy');
    assert.equal(JSON.parse([...storage.values()][0]).policy, 'current-policy');
    console.log('Attribution script: no pre-consent storage/tracking; independent form/email choice; stable replay; offline withdrawal stops tracking');
})().catch(error => {console.error(error); process.exitCode = 1;});
