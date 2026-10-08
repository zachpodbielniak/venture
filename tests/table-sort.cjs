/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Run with node: the table sorter in data/static/venture.js against a
 * fake table just large enough for it -- the figures a cell can hold, the
 * tables it must leave alone, and the groups and totals a sort must not
 * scatter. A grep for a selector would pass with every one of these
 * broken.
 */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const noop = () => {};
function attrs(element, initial) {
    element.attrs = Object.assign({}, initial || {});
    element.getAttribute = (name) => (name in element.attrs ? element.attrs[name] : null);
    element.hasAttribute = (name) => name in element.attrs;
    element.setAttribute = (name, value) => { element.attrs[name] = String(value); };
    element.removeAttribute = (name) => { delete element.attrs[name]; };
    element.matches = (selector) => selector.split(',').map((s) => s.trim()).some((s) =>
        (s.startsWith('[') && element.hasAttribute(s.slice(1, -1))) ||
        (s.startsWith('.') && (element.className || '').split(/\s+/).includes(s.slice(1))));
    return element;
}
function cell(text, options) {
    options = options || {};
    return attrs({
        textContent: text, colSpan: options.colspan || 1, rowSpan: options.rowspan || 1,
        querySelector: (s) => (s === 'time[datetime]' && options.datetime
            ? attrs({}, { datetime: options.datetime }) : null),
        className: ''
    }, options.attrs);
}
function row(texts, options) {
    options = options || {};
    const r = attrs({ className: options.className || '', hidden: false }, options.attrs);
    r.cells = texts.map((t) => (typeof t === 'object' ? t : cell(t)));
    r.controls = options.controls || [];
    r.querySelectorAll = () => r.controls;
    r.contains = (node) => r.controls.some((c) => c.form === node && c.inRow);
    return r;
}
function table(headings, bodies, options) {
    options = options || {};
    const t = attrs({ className: options.className || '' }, options.attrs);
    const head = { children: [] };
    head.cells = head.children;
    headings.forEach((label) => {
        const th = attrs({ textContent: label, colSpan: 1, parentNode: head, closest: (s) => (s === 'table' ? t : null) });
        head.children.push(th);
    });
    t.tHead = { rows: [head] };
    t.tBodies = bodies.map((rows) => {
        const tbody = attrs({ rows: rows.slice() });
        tbody.appendChild = (r) => {
            const at = tbody.rows.indexOf(r);
            if (at >= 0) tbody.rows.splice(at, 1);
            tbody.rows.push(r);
        };
        return tbody;
    });
    t.closest = (s) => (options.inside && s === '[data-no-sort]' ? {} : null);
    t.heading = (i) => head.children[i];
    return t;
}
const column = (t, i) => t.tBodies.flatMap((b) => b.rows).map((r) => r.cells[i].textContent);

const document = { readyState: 'loading', addEventListener: noop, querySelector: () => null, querySelectorAll: () => [] };
const window = { document, addEventListener: noop, location: { href: 'http://x/', origin: 'http://x' } };
vm.runInNewContext(fs.readFileSync('data/static/venture.js', 'utf8'), { window, document, console });
const { figure, refusal, sortBy } = window.venture.tables;

/* Figures: money in any currency, the bracketed negative, gold, dates. */
assert.equal(figure('$1,234.56'), 1234.56);
assert.equal(figure('1,234.56 USD'), 1234.56);
assert.equal(figure('USD 1,234.56'), 1234.56);
assert.equal(figure('-$5.00'), -5);
assert.equal(figure('$-5.00'), -5);
assert.equal(figure('(5.00)'), -5);
assert.equal(figure('(€12.50)'), -12.5);
assert.equal(figure('€ 12.50'), 12.5);
assert.equal(figure('US$3'), 3);
assert.equal(figure('12.5%'), 12.5);
assert.equal(figure('1.2345 GOLD'), 1.2345);
assert.equal(figure('9,999g 98s'), 99999800);
assert.equal(figure('2g 50s 3c'), 25003);
assert.equal(figure('2026-10-08'), Date.UTC(2026, 9, 8));
assert.equal(figure('2026-10-08 12:30'), Date.UTC(2026, 9, 8, 12, 30));
assert.equal(figure('2026-10-08T12:30:00Z'), Date.UTC(2026, 9, 8, 12, 30));
assert.equal(figure('2026-10-08T12:30:00-04:00'), Date.UTC(2026, 9, 8, 16, 30));
assert.equal(figure('2026-10-08 12:30 UTC'), Date.UTC(2026, 9, 8, 12, 30));
assert.equal(figure('2026-10'), Date.UTC(2026, 9, 1));
assert.equal(figure('8 Oct 2026'), Date.UTC(2026, 9, 8));
assert.equal(figure('Oct 8, 2026'), Date.UTC(2026, 9, 8));
assert.equal(figure('Thu 08 Oct 2026 12:30 UTC'), Date.UTC(2026, 9, 8, 12, 30));
assert.ok(figure('12 min ago') < figure('3 min ago'));
assert.ok(figure('3 h ago') < figure('12m ago'));
assert.ok(figure('2 days ago') < figure('just now'));
assert.ok(figure('in 3 h') > figure('just now'));
assert.ok(isNaN(figure('Mayor 2026')), 'a name that starts like a month is not a date');
assert.equal(figure('3 days'), 3, 'a quantity with its unit is a figure');
assert.ok(isNaN(figure('Item 12')));
assert.ok(isNaN(figure('v1.2.3')));
assert.ok(isNaN(figure('Cloth Bracers')));

/* Money sorts as money, largest first on the first click. */
let t = table(['Name', 'Amount'], [[
    row(['a', '$5.00']), row(['b', '(12.00)']), row(['c', '1,234.56 USD']), row(['d', '—']), row(['e', '-$3.00'])
]]);
assert.equal(refusal(t), null);
sortBy(t.heading(1));
assert.deepEqual(column(t, 0), ['c', 'a', 'e', 'b', 'd']);
assert.equal(t.heading(1).getAttribute('aria-sort'), 'descending');
sortBy(t.heading(1));
assert.deepEqual(column(t, 0), ['b', 'e', 'a', 'c', 'd'], 'turned round, the empty cell still last');

/* A <time datetime> sorts by the moment, not by "3 min ago". */
t = table(['When'], [[
    row([cell('2 days ago', { datetime: '2026-10-06T00:00:00Z' })]),
    row([cell('just now', { datetime: '2026-10-08T12:00:00Z' })]),
    row([cell('3 h ago', { datetime: '2026-10-08T09:00:00Z' })])
]]);
sortBy(t.heading(0));
assert.deepEqual(column(t, 0), ['just now', '3 h ago', '2 days ago']);

/* Groups sort inside themselves; their headings and totals stay put. */
const divider = (label) => row([cell(label, { colspan: 2, attrs: { scope: 'rowgroup' } })]);
t = table(['Name', 'Gold'], [[
    divider('Realm A'), row(['a1', '1g']), row(['a2', '5g']), row(['Total A', '6g'], { className: 'total' }),
    divider('Realm B'), row(['b1', '9g']), row(['b2', '2g'])
]]);
sortBy(t.heading(1));
assert.deepEqual(column(t, 0), ['Realm A', 'a2', 'a1', 'Total A', 'Realm B', 'b1', 'b2']);

/* A single "nothing here" row is left alone. */
t = table(['Name', 'Gold'], [[row([cell('Nothing matches.', { colspan: 2 })])]]);
sortBy(t.heading(0));
assert.deepEqual(column(t, 0), ['Nothing matches.']);

/* What must not sort, and why. */
assert.equal(refusal(table(['A'], [[row(['1'])]], { attrs: { 'data-no-sort': '' } })), 'marked');
assert.equal(refusal(table(['A'], [[row(['1'])]], { inside: true })), 'marked');
assert.equal(refusal(table(['A'], [[row(['1'])]], { className: 'visually-hidden chart-data' })), 'marked');
assert.equal(refusal(table(['A'], [[row(['1'])]], { attrs: { 'data-server-sort': '' } })), 'server');
t = table(['A'], [[row(['1'])]]);
t.tHead = null;
assert.equal(refusal(t), 'no heading');
t = table(['A'], [[row(['1'])]]);
t.tBodies[0].setAttribute('data-lines', '');
assert.equal(refusal(t), 'editor');
assert.equal(refusal(table(['A', 'B'], [[row([cell('x', { rowspan: 2 }), '1']), row(['2'])]])), 'rowspan');
const outerForm = {};
assert.equal(refusal(table(['Item', 'Qty'], [[row(['a', ''], { controls: [{ type: 'text', form: outerForm }] })]])),
    'form controls', 'a line editor posts its rows in order');
assert.equal(refusal(table(['Item', 'Qty'], [[row(['a', ''], { controls: [{ type: 'number', form: null }] })]])),
    'form controls', 'a control a script reads in order');
const ownForm = {};
assert.equal(refusal(table(['Item', ''], [[row(['a', ''], { controls: [{ type: 'hidden', form: ownForm, inRow: true }] })]])),
    null, 'a form inside its own row moves with it');
assert.equal(refusal(table(['', 'Item'], [[row(['', 'a'], { controls: [{ type: 'checkbox', form: null }] })]])),
    null, 'a tick box is the row\'s own');

console.log('table sort: figures, groups, totals and refusals hold');
