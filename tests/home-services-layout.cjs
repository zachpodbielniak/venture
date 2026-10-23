/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Run with Playwright available to Node. Exercises both shipped stylesheets
 * without external services; the launcher must never sit over document totals. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {chromium} = require('playwright');
(async () => {
  const browser = await chromium.launch({headless: true, executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE});
  try {
    for (const look of ['classic', 'industrial']) {
      const page = await browser.newPage({viewport: {width: 390, height: 844}});
      const css = fs.readFileSync(`data/static/venture-${look}.css`, 'utf8');
      await page.setContent(`<style>${css}</style><main style="margin:0;padding:16px">
        <div style="height:690px"></div><section class="card" id="totals" style="height:180px">Discount, tax and total</section>
        <div style="height:900px"></div></main><button class="ai-fab">Ask VENTURE</button>`);
      const before = await page.locator('.ai-fab').boundingBox();
      await page.evaluate(() => window.scrollTo(0, 70));
      const button = await page.locator('.ai-fab').boundingBox();
      const totals = await page.locator('#totals').boundingBox();
      const overlap = button.x < totals.x + totals.width && button.x + button.width > totals.x &&
        button.y < totals.y + totals.height && button.y + button.height > totals.y;
      assert.equal(overlap, false, `${look}: assistant launcher covers totals at 390px`);
      assert.notEqual(button.y, before.y, `${look}: phone launcher stays fixed over scrolling content`);
      console.log(`PASS ${look}: phone launcher stays in document flow`);
      await page.close();
    }
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
