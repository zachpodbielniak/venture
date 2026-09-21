/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The site's consent manager calls grant/withdraw explicitly. Loading this
 * script neither reads storage nor sends analytics; email choice is separate. */
(function () {
    'use strict';
    const script = document.currentScript;
    const site = script && script.dataset.site;
    if (!site) return;
    const base = new URL(script.src).origin + '/attribution/' + encodeURIComponent(site) + '/';
    const key = 'venture-analytics:' + base;
    let active = null, remembered = null, generation = 0, consentWork = Promise.resolve();
    function serialize(operation) {
        const next = consentWork.catch(() => {}).then(operation);
        consentWork = next;
        return next;
    }
    function read() {
        if (remembered) return remembered;
        try { return JSON.parse(localStorage.getItem(key)); } catch (_) { return null; }
    }
    function remember(value) {
        remembered = value;
        try { if (value) localStorage.setItem(key, JSON.stringify(value)); else localStorage.removeItem(key); } catch (_) { /* Memory-only consent when browser storage is unavailable. */ }
    }
    async function send(operation, payload) {
        const response = await fetch(base + operation, {
            method: 'POST', credentials: 'omit', cache: 'no-store', referrerPolicy: 'no-referrer',
            headers: {'Content-Type': 'text/plain;charset=UTF-8'}, body: JSON.stringify(payload)
        });
        if (!response.ok) {
            const error = new Error('Attribution request refused (' + response.status + ')');
            error.status = response.status;
            throw error;
        }
        return response.json();
    }
    async function revoke(record) {
        if (!record || !record.token) { remember(null); return; }
        remember(Object.assign({}, record, {pending_withdrawal: true}));
        try { await send('withdraw', {token: record.token}); }
        catch (error) {
            /* Retention may already have forgotten the capability. That is
             * completed withdrawal, not an endless retry blocking new consent. */
            if (error.status !== 404) throw error;
        }
        remember(null);
    }
    window.VentureAttribution = Object.freeze({
        grant(policy) {
            const turn = ++generation;
            active = null;
            return serialize(async () => {
                if (turn !== generation) return;
                const previous = read();
                if (previous && previous.pending_withdrawal) await revoke(previous);
                if (turn !== generation) return;
                if (previous && !previous.pending_withdrawal && previous.policy === policy && Date.parse(previous.expires_at) > Date.now()) {
                    active = previous;
                    return;
                }
                const result = await send('grant', {policy: policy});
                const record = {token: result.token, expires_at: result.expires_at, policy: policy};
                /* Withdrawal can arrive while the consent response is in flight.
                 * Such a response must never re-enable analytics. */
                if (turn !== generation) { remember(Object.assign({}, record, {pending_withdrawal: true})); return; }
                remember(record); active = record;
            });
        },
        async page(eventId) {
            const record = active;
            if (!record || Date.parse(record.expires_at) <= Date.now()) { active = null; return; }
            const url = new URL(location.href);
            const fields = {page: url.origin + url.pathname, referrer: document.referrer || ''};
            ['utm_source', 'utm_medium', 'utm_campaign'].forEach(name => {
                const value = url.searchParams.get(name);
                if (value) fields[name] = value;
            });
            try { return await send('observe', {token: record.token, event_id: eventId || crypto.randomUUID(), fields: fields}); }
            catch (error) { if (error.status === 404 && active === record) { active = null; remember(null); } throw error; }
        },
        createSubmission(fields, marketing) {
            const envelope = {version: 1, submission_id: crypto.randomUUID(), fields: Object.assign({}, fields)};
            if (active && Date.parse(active.expires_at) > Date.now()) envelope.visitor_token = active.token;
            if (marketing !== undefined) envelope.marketing = Object.assign({}, marketing);
            return envelope;
        },
        submit(envelope) {
            /* The caller retains this exact in-memory envelope across retry.
             * No inquiry fields or email permissions enter browser storage. */
            return send('capture', envelope);
        },
        withdraw() {
            ++generation;
            active = null;
            return serialize(() => revoke(read()));
        }
    });
}());
