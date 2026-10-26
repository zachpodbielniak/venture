/* SPDX-License-Identifier: AGPL-3.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
class Link {
    constructor(href, attrs = {}) {
        this.attrs = new Map(Object.entries({href, ...attrs}));
        this.href = new URL(href, 'https://example.test/e/company').href;
        this.tagName = 'A';
        const classes = new Set();
        this.classList = {add: x => classes.add(x), remove: x => classes.delete(x), contains: x => classes.has(x)};
    }
    closest(selector) { return selector === 'a[href]' ? this : null; }
    getAttribute(name) { return this.attrs.has(name) ? this.attrs.get(name) : null; }
    hasAttribute(name) { return this.attrs.has(name); }
    setAttribute(name, value) { this.attrs.set(name, value); }
    removeAttribute(name) { this.attrs.delete(name); }
}
function scenario() {
    const listeners = {}, windowListeners = {}, timers = new Map();
    const body = new Link('/');
    body.addEventListener = () => {};
    const document = {
        readyState: 'loading', body,
        addEventListener(name, callback) { (listeners[name] ||= []).push(callback); },
        querySelectorAll() { return []; }
    };
    const window = {
        location: {href: 'https://example.test/e/company', origin: 'https://example.test'},
        matchMedia: () => ({matches: false}),
        addEventListener(name, callback) { (windowListeners[name] ||= []).push(callback); },
        setTimeout(callback) { const id = timers.size + 1; timers.set(id, callback); return id; },
        clearTimeout(id) { timers.delete(id); }
    };
    let source = fs.readFileSync('data/static/venture.js', 'utf8');
    source = source.replace('window.venture = {', 'window.testWireNavigation = typeof wireNavigationFeedback === "function" ? wireNavigationFeedback : function () {}; window.venture = {');
    vm.runInNewContext(source, {window, document, URL, setTimeout: window.setTimeout, clearTimeout: window.clearTimeout});
    window.testWireNavigation();
    function click(link, extra = {}) {
        const event = {target: link, button: 0, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, ...extra};
        for (const fn of listeners.click || []) fn(event);
        return event;
    }
    return {click, expire() { for (const fn of [...timers.values()]) fn(); }, escape() { for (const fn of listeners.keydown || []) fn({key: 'Escape'}); }, reset() { for (const fn of windowListeners.pageshow || []) fn({}); }};
}
{
    const {click, reset} = scenario();
    const link = new Link('/e/contact');
    assert.equal(click(link).defaultPrevented, false, 'the first click follows its normal browser link');
    assert.equal(link.getAttribute('aria-busy'), 'true', 'navigation must be acknowledged synchronously');
    assert.equal(click(link).defaultPrevented, true, 'a repeated pending navigation must not queue another request');
    assert.equal(click(new Link('/e/contact')).defaultPrevented, true, 'duplicate destinations are blocked across links');
    reset();
    assert.equal(link.hasAttribute('aria-busy'), false, 'back/forward restoration clears pending state');
    assert.equal(click(link).defaultPrevented, false);
}
for (const [href, attrs, modifiers] of [
    ['#section', {}, {}], ['/e/contact', {}, {ctrlKey: true}],
    ['/e/contact', {}, {shiftKey: true}], ['/e/contact', {}, {altKey: true}],
    ['/e/contact', {}, {button: 1}], ['/e/company#details', {}, {}],
    ['/e/contact', {}, {metaKey: true}], ['/e/contact', {download: ''}, {}],
    ['/e/contact', {target: '_blank'}, {}], ['https://outside.example/', {}, {}],
    ['/e/contact', {'hx-get': '/e/contact'}, {}], ['/e/contact', {}, {defaultPrevented: true}]
]) {
    const {click} = scenario();
    const link = new Link(href, attrs);
    const event = click(link, modifiers);
    assert.equal(event.defaultPrevented, !!modifiers.defaultPrevented, 'special navigation keeps its browser behavior');
    assert.equal(link.hasAttribute('aria-busy'), false, 'special navigation must not leave the current page busy');
}
console.log('Navigation acknowledges immediately, blocks duplicate destinations, resets after history restoration, and preserves special links.');

for (const action of ['expire', 'escape']) {
    const fixture = scenario();
    const link = new Link('/reports/pnl');
    fixture.click(link);
    fixture[action]();
    assert.equal(link.hasAttribute('aria-busy'), false);
    assert.equal(fixture.click(link).defaultPrevented, false, 'cancelled navigation is retryable');
}
{
    const fixture = scenario();
    const first = new Link('/e/contact');
    const second = new Link('/e/company');
    fixture.click(first);
    assert.equal(fixture.click(second).defaultPrevented, false);
    assert.equal(first.hasAttribute('aria-busy'), false);
    assert.equal(second.getAttribute('aria-busy'), 'true');
}
