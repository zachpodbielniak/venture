/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Exercise the actual loader and submit listeners with stalled transports. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('data/static/forms.js', 'utf8');
const flush = () => new Promise(resolve => setImmediate(resolve));
function element() {
    return {children: [], hidden: true, appendChild(child) {this.children.push(child);},
        setAttribute() {}, removeAttribute() {}, focus() {this.focused = true;}};
}
async function exercise(mode) {
    const deadlines = new Map(), diagnostics = [], requests = [];
    let id = 0, xhr, phase = mode.startsWith('load') ? 'stall' : 'initial';
    const button = {disabled: false}, summary = element(), listeners = {};
    const form = {
        action: mode === 'upload-cross' ? 'https://venture.example.test/pub/form/token' : 'https://site.example.test/pub/form/token',
        elements: {namedItem() {return null;}},
        getAttribute() {return null;},
        addEventListener(name, callback) {listeners[name] = callback;},
        querySelector(selector) {return selector === '.vf-submit' ? button : selector === '.vf-errors' ? summary : null;},
        querySelectorAll(selector) {
            return selector === 'input[type="file"]' && mode.startsWith('upload') ?
                [{files: [], getAttribute() {return '10';}, setCustomValidity() {}}] : [];
        }, appendChild() {}
    };
    const target = {getAttribute(name) {return name === 'data-venture-form' ? 'https://venture.example.test/pub/form/token/fragment' : null;},
        querySelectorAll() {return [form];}};
    const sandbox = {
        URL, URLSearchParams, Promise, Object, Array, Number, Error, JSON, AbortController,
        FormData: class extends URLSearchParams {constructor() {super('_vf_payment=same-private-nonce');}},
        console: {warn(message) {diagnostics.push(message);}},
        location: {href: 'https://site.example.test/page', origin: 'https://site.example.test'},
        document: {readyState: 'complete', baseURI: 'https://site.example.test/page',
            querySelectorAll() {return [target];}, createElement: element},
        setTimeout(callback, delay) {const key = ++id; deadlines.set(key, {callback, delay}); return key;},
        clearTimeout(key) {deadlines.delete(key);},
        XMLHttpRequest: class {
            constructor() {xhr = this; this.upload = {};}
            open() {} setRequestHeader() {} send(body) {requests.push(String(body));}
        },
        fetch: async (url, options) => {
            requests.push(options.body ? String(options.body) : 'GET');
            if (phase === 'initial') return {ok: true, text: async () => '<form></form>'};
            const stalled = () => new Promise((resolve, reject) => {
                if (options.signal) options.signal.addEventListener('abort', () => reject(new Error('aborted')), {once: true});
            });
            if (mode.endsWith('body')) return {ok: true, json: stalled, text: stalled};
            return stalled();
        }
    };
    sandbox.window = sandbox;
    vm.runInNewContext(source, sandbox);
    await flush();
    if (!mode.startsWith('load')) {
        phase = 'stall';
        assert.equal(deadlines.size, 0, 'successful load releases timer');
        listeners.submit({preventDefault() {}});
        await flush();
        assert.equal(button.disabled, true);
        /* Duplicate clicks during uncertainty must not issue a second POST. */
        const sent = requests.length;
        listeners.submit({preventDefault() {}});
        assert.equal(requests.length, sent);
    }
    if (mode === 'upload-local') {
        assert.ok(xhr.timeout > 0 && xhr.timeout <= 120000, 'native upload needs a deadline');
        assert.equal(typeof xhr.ontimeout, 'function');
        xhr.ontimeout();
    } else {
        assert.equal(deadlines.size, 1, 'hung transport needs one whole-response deadline');
        const timer = [...deadlines.values()][0];
        assert.ok(timer.delay > 0 && timer.delay <= 120000);
        timer.callback();
    }
    await flush();
    assert.equal(deadlines.size, 0);
    assert.equal(diagnostics.length, 1, 'one redacted failure diagnostic');
    assert.match(diagnostics[0], /timeout|deadline/i);
    assert.ok(!diagnostics[0].includes('token') && !diagnostics[0].includes('nonce'));
    if (mode.startsWith('load')) assert.match(target.textContent, /not available/);
    else {
        assert.equal(form.ventureSending, false);
        assert.equal(button.disabled, false);
        assert.equal(summary.hidden, false);
        assert.equal(summary.focused, true);
        assert.match(summary.children[0].textContent, /could not confirm/);
        assert.ok(requests.at(-1).includes('_vf_payment=same-private-nonce'));
    }
}
(async () => {
    for (const mode of ['load-headers', 'load-body', 'post-headers', 'post-body', 'upload-cross', 'upload-local']) await exercise(mode);
    console.log('Forms transport: six stalled header/body/upload flows release controls and retain retry identity');
})().catch(error => {console.error(error); process.exitCode = 1;});
