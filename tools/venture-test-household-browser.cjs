/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Opt-in against a disposable, populated household; no fixture writes. */
const assert = require('node:assert/strict');
const { chromium } = require('playwright-core');

async function main() {
    const required = ['VENTURE_TEST_UI_URL', 'VENTURE_TEST_UI_USERNAME',
        'VENTURE_TEST_UI_PASSWORD', 'VENTURE_TEST_HOUSEHOLD_ID', 'VENTURE_TEST_SAVINGS_GOAL_ID'];
    for (const name of required) assert(process.env[name], `${name} is required`);
    const base = process.env.VENTURE_TEST_UI_URL;
    const browser = await chromium.launch({ headless: true,
        executablePath: process.env.VENTURE_TEST_CHROMIUM || undefined });
    const failures = [];
    try {
        for (const look of ['industrial', 'classic']) {
            for (const width of [390, 1440]) {
                const context = await browser.newContext({ viewport: { width, height: 900 } });
                await context.addCookies([{ name: 'venture_look', value: look, url: base }]);
                const page = await context.newPage();
                await page.goto(base + '/login');
                await page.locator('[name=username]').fill(process.env.VENTURE_TEST_UI_USERNAME);
                await page.locator('[name=password]').fill(process.env.VENTURE_TEST_UI_PASSWORD);
                await Promise.all([page.waitForURL(u => u.pathname !== '/login'),
                    page.locator('button[type=submit]').click()]);
                await page.goto(base + '/view/venture/' + process.env.VENTURE_TEST_HOUSEHOLD_ID);
                async function check(name, run) {
                    try { await run(); console.log(`PASS ${look} ${width} ${name}`); }
                    catch (error) { failures.push(`${look} ${width} ${name}: ${error.message}`); }
                }
                await check('calendar stays inside the viewport', async () => {
                    await page.goto(base + '/money/calendar');
                    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
                    const event = page.locator('.cal-event a').first();
                    assert(await event.count(), 'fixture needs a scheduled bill');
                    await event.click();
                    assert(page.url().includes('/e/recurring_schedule/'));
                });
                await check('all report period buttons are reachable', async () => {
                    await page.goto(base + '/reports/net_worth');
                    const links = page.locator('.page-actions .btn-group a');
                    for (let i = 0; i < await links.count(); i++) {
                        const rect = await links.nth(i).boundingBox();
                        assert(rect && rect.x >= 0 && rect.x + rect.width <= width + 1);
                    }
                });
                await check('savings offers only working month shortcuts', async () => {
                    await page.goto(base + '/reports/savings_goals');
                    const hrefs = await page.locator('.page-actions .btn-group a').evaluateAll(es => es.map(e => e.getAttribute('href')));
                    assert(hrefs.length >= 2);
                    for (const href of hrefs) assert.equal((await page.goto(base + href)).status(), 200, href);
                });
                await check('linked goal does not show manual zero progress', async () => {
                    await page.goto(base + '/e/goal/' + process.env.VENTURE_TEST_SAVINGS_GOAL_ID);
                    assert.equal(await page.locator('.goal-progress').count(), 0);
                    assert.equal(await page.locator('.savings-progress').count(), 1);
                });
                await context.close();
            }
        }
    } finally { await browser.close(); }
    for (const failure of failures) console.error('FAIL ' + failure);
    assert.equal(failures.length, 0, 'household browser regressions');
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
