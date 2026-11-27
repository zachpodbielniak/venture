/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * "every" pollers that swap themselves with outerHTML must not leak
 * intervals: the replaced element's interval used to survive, ticked on
 * the detached node, re-bound the (detached) swapped-in markup with a NEW
 * interval and so doubled the poller count every period.
 *
 * Real bindElement/issueRequest/performSwap/process/observe run; only the
 * DOM, timers, fetch and the cosmetic helpers are faked.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const tick = () => new Promise(resolve => setImmediate(resolve));
const POLLER = '<div id="poll" hx-get="/poll" hx-trigger="every 50ms" hx-swap="outerHTML">x</div>';
const MAX_REQUESTS = 2000;

/* ---- fake DOM ------------------------------------------------------- */
function makeWorld(useObserver) {
    const world = {requests: [], pending: [], records: [], swaps: 0};
    let now = 0;
    let nextId = 1;
    const timers = new Map();

    /* fake timers: setInterval/setTimeout/clear*, driven by advance() */
    const addTimer = (fn, ms, repeat) => {
        const id = nextId++;
        timers.set(id, {fn, ms: Math.max(ms || 0, 1), due: now + Math.max(ms || 0, 1), repeat});
        return id;
    };
    world.live = (kind) => [...timers.values()].filter(t => kind === undefined || t.repeat === kind).length;
    world.advance = async (ms) => {
        const end = now + ms;
        for (;;) {
            let best = null;
            for (const [id, t] of timers) if (t.due <= end && (!best || t.due < best[1].due)) best = [id, t];
            if (!best || world.requests.length > MAX_REQUESTS) break;
            const [id, t] = best;
            now = Math.max(now, t.due);
            if (t.repeat) t.due += t.ms; else timers.delete(id);
            t.fn();
            await tick();
        }
        now = end;
    };

    class Frag { constructor() { this.nodeType = 11; this.children = []; } }
    class El {
        constructor(attrs) {
            this.nodeType = 1; this.tagName = 'DIV'; this.attrs = attrs || {};
            this.children = []; this.parent = null; this.listeners = {};
            this.classList = {add() {}};
            this.replaceCalls = 0;
        }
        get isConnected() { let n = this; while (n.parent) n = n.parent; return n === world.body; }
        get parentElement() { return this.parent; }
        get id() { return this.attrs.id || ''; }
        getAttribute(n) { return n in this.attrs ? this.attrs[n] : null; }
        addEventListener(n, fn) { (this.listeners[n] = this.listeners[n] || []).push(fn); }
        dispatch(n) { (this.listeners[n] || []).forEach(fn => fn({type: n, preventDefault() {}})); }
        contains(o) { for (let n = o; n; n = n.parent) if (n === this) return true; return false; }
        appendChild(c) { c.parent = this; this.children.push(c); world.records.push({addedNodes: [c], removedNodes: []}); }
        descendants() { return this.children.flatMap(c => [c, ...c.descendants()]); }
        querySelectorAll(sel) {
            if (sel === '[hx-trigger]') return this.descendants().filter(d => d.getAttribute('hx-trigger') !== null);
            return this.descendants().filter(d => d.getAttribute('hx-get') !== null);
        }
        remove() {
            if (!this.parent) return;
            this.parent.children.splice(this.parent.children.indexOf(this), 1);
            this.parent = null;
            world.records.push({addedNodes: [], removedNodes: [this]});
        }
        replaceWith(frag) {
            this.replaceCalls++;
            /* like the DOM: a parentless node ignores replaceWith */
            if (!this.parent) return;
            const p = this.parent;
            const at = p.children.indexOf(this);
            p.children.splice(at, 1, ...frag.children);
            frag.children.forEach(c => { c.parent = p; });
            frag.children = [];
            this.parent = null;
            world.records.push({addedNodes: [...p.children], removedNodes: [this]});
        }
    }
    world.El = El;
    world.body = new El({});
    world.body.tagName = 'BODY';

    const parseFragment = (html) => {
        const frag = new Frag();
        const m = /<div([^>]*)>/.exec(html);
        const attrs = {};
        m[1].replace(/([\w-]+)="([^"]*)"/g, (_, k, v) => { attrs[k] = v; });
        const el = new El(attrs);
        el.parent = frag;
        frag.children.push(el);
        return frag;
    };

    const document = {readyState: 'loading', addEventListener() {}, body: world.body,
                      activeElement: null, getElementById() { return null; }};
    const window = {location: {href: 'http://example.test/'},
                    setTimeout: (fn, ms) => addTimer(fn, ms, false),
                    clearTimeout: (id) => timers.delete(id),
                    setInterval: (fn, ms) => addTimer(fn, ms, true),
                    clearInterval: (id) => timers.delete(id)};

    /* fetch: auto-answers with the poller markup unless held */
    world.hold = false;
    const fetch = (url, options) => new Promise((resolve) => {
        const reply = (text) => resolve({ok: true, headers: {get() { return null; }}, text: async () => text});
        world.requests.push({url, signal: options.signal});
        if (world.hold) world.pending.push(reply); else reply(POLLER);
    });

    /* MutationObserver stub: delivers records on a microtask */
    let observerCallback = null;
    const MutationObserver = function (cb) {
        observerCallback = cb;
        this.observe = () => {};
    };
    world.flushObserver = () => {
        if (!useObserver || !observerCallback) return;
        const batch = world.records.splice(0);
        if (batch.length) observerCallback(batch);
    };
    const origPush = world.records.push.bind(world.records);
    world.records.push = (r) => { origPush(r); if (useObserver) queueMicrotask(world.flushObserver); return world.records.length; };

    const expose = `window.testIssue = issueRequest;
        window.testProcess = process;
        window.testObserve = observe;
        collectValues = function () { return new URLSearchParams(); };
        setRequesting = function () {};
        trigger = function () { return true; };
        applyOobSwaps = function () {};
        parseFragment = ${parseFragment.toString()};
        window.htmx = {`;
    const source = fs.readFileSync(process.env.HX_TEST_SOURCE || 'data/static/venture-hx.js', 'utf8').replace('window.htmx = {', expose);
    /* parseFragment needs Frag/El/world from this scope: hand them in */
    vm.runInNewContext(source, {window, document, fetch, AbortController, URLSearchParams, WeakMap,
                                MutationObserver, console, Frag, El, world, Date: {now: () => now}});
    world.window = window;
    return world;
}

function mount(world, markup) {
    const m = /<div([^>]*)>/.exec(markup);
    const attrs = {};
    m[1].replace(/([\w-]+)="([^"]*)"/g, (_, k, v) => { attrs[k] = v; });
    const el = new world.El(attrs);
    world.body.appendChild(el);
    world.records.splice(0);
    world.window.testProcess(el);
    return el;
}

(async () => {
    /* 1. self-swapping poller, observer on: exactly one interval survives */
    {
        const w = makeWorld(true);
        w.window.testObserve();
        mount(w, POLLER);
        assert.equal(w.live(true), 1);
        await w.advance(2000);
        assert.equal(w.live(true), 1, 'exactly one live interval after 2 s of self-swapping');
        assert.ok(w.requests.length >= 38 && w.requests.length <= 42,
                  'about 40 requests expected, got ' + w.requests.length);
    }

    /* 2. observer off: the tick guard alone stops orphan intervals */
    {
        const w = makeWorld(false);
        mount(w, POLLER);
        await w.advance(2000);
        assert.ok(w.live(true) <= 2, 'orphan intervals must not multiply, live=' + w.live(true));
        assert.ok(w.requests.length <= 42, 'requests stay bounded, got ' + w.requests.length);
        await w.advance(50);
        assert.ok(w.live(true) <= 2, 'still bounded after another tick, live=' + w.live(true));
    }

    /* 3. detached element issues no request; late response swaps nothing */
    {
        const w = makeWorld(true);
        w.window.testObserve();
        const el = mount(w, POLLER);
        el.remove();
        await tick();
        w.window.testIssue(el, 'get', '/poll', null);
        assert.equal(w.requests.length, 0, 'a detached element must not issue a request');
        await w.advance(500);
        assert.equal(w.requests.length, 0, 'no request is ever issued from a detached element');

        const w2 = makeWorld(true);
        w2.window.testObserve();
        const live = mount(w2, '<div id="t" hx-get="/poll" hx-trigger="click" hx-swap="outerHTML">x</div>');
        w2.hold = true;
        w2.window.testIssue(live, 'get', '/poll', null);
        assert.equal(w2.requests.length, 1);
        live.remove();
        await tick();
        w2.pending[0](POLLER);
        await tick();
        await tick();
        assert.equal(live.replaceCalls, 0, 'a response for a removed target swaps nothing');
        assert.equal(w2.live(true), 0, 'nothing from the late response was bound');
    }

    /* 4. removing a subtree clears its pollers and pending delayed triggers */
    {
        const w = makeWorld(true);
        w.window.testObserve();
        const box = mount(w, '<div id="box">x</div>');
        const poller = new w.El({id: 'p', 'hx-get': '/poll', 'hx-trigger': 'every 50ms', 'hx-swap': 'outerHTML'});
        const typer = new w.El({id: 'q', 'hx-get': '/q', 'hx-trigger': 'keyup delay:500ms'});
        box.appendChild(poller);
        box.appendChild(typer);
        w.records.splice(0);
        w.window.testProcess(box);
        typer.dispatch('keyup');
        assert.equal(w.live(true), 1, 'poller interval armed');
        assert.equal(w.live(false), 1, 'delayed trigger armed');
        box.remove();
        await tick();
        assert.equal(w.live(), 0, 'removing the subtree clears every timer');
        await w.advance(2000);
        assert.equal(w.requests.length, 0, 'nothing fires after the subtree is gone');
    }

    console.log('Poll leak: single live interval, no detached requests, subtree timers cleared passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
