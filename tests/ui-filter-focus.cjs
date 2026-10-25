/* SPDX-License-Identifier: AGPL-3.0-or-later */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
function scenario(inside, found) {
    const old = {id: 'list-search', selectionStart: 2, selectionEnd: 4, selectionDirection: 'forward'};
    let focused = false, selection;
    const replacement = {focus(options) { focused = true; assert.equal(options.preventScroll, true); }, setSelectionRange(...args) { selection = args; }};
    const document = {readyState: 'loading', activeElement: old, addEventListener() {},
        createElement() { return {content: {children: [], querySelectorAll() { return []; }}}; },
        getElementById() { return found ? replacement : null; }};
    const target = {contains(node) { return inside && node === old; }, appendChild() {},
        set innerHTML(value) { if (inside) document.activeElement = {}; }};
    const window = {};
    const source = fs.readFileSync('data/static/venture-hx.js', 'utf8').replace('window.htmx = {', 'window.testSwap = performSwap; window.htmx = {');
    vm.runInNewContext(source, {window, document, WeakMap});
    window.testSwap(target, '<input id="list-search">', {style: 'innerHTML'});
    assert.equal(focused, inside && found, 'a replaced search input retains focus; unrelated focus is untouched');
    if (focused) assert.deepEqual(selection, [2, 4, 'forward']);
}
scenario(true, true);
scenario(false, true);
scenario(true, false);
console.log('filter focus: 3 scenarios passed');
