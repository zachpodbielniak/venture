/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Exercise the shipped submit handler with a transport whose reply can be lost
 * after committing a write. Retrying such a request can create a second bill. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
class Element {
    constructor() { this.children = []; this.disabled = false; this.attributes = []; }
    setAttribute() {}
    appendChild(child) { this.children.push(child); }
    scrollIntoView() {}
    set textContent(value) { this.children = []; this.text = value; }
    get textContent() { return (this.text || '') + this.children.map(x => x.textContent).join(''); }
}
class Form extends Element {
    constructor(flags) { super(); this.flags = flags || []; this.buttons = [new Element(), new Element()]; this.buttons[1].disabled = true; }
    getAttribute(name) { return name === 'method' ? 'post' : name === 'action' ? '/fixture/save' : null; }
    hasAttribute(name) { return this.flags.includes(name); }
    querySelector() { return this.error || null; }
    querySelectorAll() { return this.buttons; }
    insertBefore(box) { this.error = box; }
}
async function scenario(kind) {
    let listener, settle, options, requests = 0;
    const document = {
        readyState: 'loading',
        addEventListener(name, callback) { if (name === 'submit') listener = callback; },
        body: {addEventListener() {}},
        createElement() { return new Element(); },
        createTextNode(textContent) { return {textContent}; }
    };
    const fetch = (url, init) => { requests++; options = init; return new Promise((resolve, reject) => { settle = kind === 'lost' ? reject : resolve; }); };
    const window = {fetch, location: {href: 'http://fixture/form'}, matchMedia: () => ({matches: false})};
    const context = {window, document, HTMLFormElement: Form, FormData: class {append() {}}, URLSearchParams: class {}, fetch};
    const source = fs.readFileSync('data/static/venture.js', 'utf8').replace('window.venture = {', 'window.testWireInlineForms = wireInlineForms; window.venture = {');
    vm.runInNewContext(source, context);
    window.testWireInlineForms();
    const form = new Form(kind === 'no-inline' ? ['data-no-inline'] : []);
    let prevented = false;
    function submit() { listener({target: form, defaultPrevented: false, preventDefault() { prevented = true; this.defaultPrevented = true; }}); }
    submit();
    if (kind === 'no-inline') {
        /* The checkout and identity-provider forms answer with a redirect to
         * another origin. Posted by script, the operator never reaches it and
         * each retry opens another payment session: the browser must post. */
        assert.equal(requests, 0, 'a data-no-inline form must not be posted by script');
        assert.equal(prevented, false, 'a data-no-inline form must be left to the browser');
        return;
    }
    /* A cross-origin redirect must be refused, not followed on the far
     * site's CORS policy: the write is done and only a check is safe. */
    assert.equal(options.mode, 'same-origin');
    assert.equal(requests, 1);
    submit();
    assert.equal(requests, 1, 'a second submit while the first is pending must not duplicate the write');
    if (kind === 'lost') settle(new Error('reply lost after server commit'));
    else settle({ok: false, headers: {get: () => 'application/json'}, text: async () => JSON.stringify({message: 'At least one line is required'})});
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(form.buttons[0].disabled, false);
    assert.equal(form.buttons[1].disabled, true, 'a refusal must not enable a control that was disabled before submission');
    if (kind === 'lost') {
        assert.doesNotMatch(form.error.textContent, /nothing was saved|didn.t save/i);
        assert.match(form.error.textContent, /check.*before.*again/i);
    } else assert.match(form.error.textContent, /At least one line is required/);
}
(async () => { await scenario('lost'); await scenario('refused'); await scenario('no-inline'); console.log('data-no-inline left to the browser; cross-origin redirects refused; Lost response is uncertain; duplicate submits blocked; original disabled state preserved; validation reason shown.'); })().catch(error => { console.error(error); process.exit(1); });
