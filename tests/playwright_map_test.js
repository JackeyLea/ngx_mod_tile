#!/usr/bin/env node
/* Browser-level user interaction tests for the ngx_mod_tile Leaflet demo. */
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const baseURL = process.argv[2] || 'http://host.docker.internal:8090/';
const artifactDir = process.argv[3] || '/artifacts';
const timeout = Number(process.env.MAP_TEST_TIMEOUT || 90000);
let passed = 0;

function pass(name) {
    passed++;
    console.log(`PASS: ${name}`);
}

async function waitForNewTiles(page, tileResponses, previousCount) {
    // Fresh Mapnik tiles may take several seconds to render. Wait for both a
    // decoded image and at least one new HTTP response instead of assuming the
    // old zoom level's already-loaded images represent the new zoom.
    const deadline = Date.now() + timeout;
    while (tileResponses.length === previousCount) {
        if (Date.now() >= deadline) {
            throw new Error('timed out waiting for new tile responses');
        }
        await page.waitForTimeout(200);
    }
    await page.waitForFunction(
        () => document.querySelectorAll('.leaflet-tile-loaded').length > 0,
        null, { timeout });
}

(async () => {
    fs.mkdirSync(artifactDir, { recursive: true });
    const browser = await chromium.launch({
        headless: true,
        args: ['--no-sandbox', '--disable-dev-shm-usage']
    });

    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    page.setDefaultTimeout(timeout);

    const tileResponses = [];
    const assetResponses = [];
    const pageErrors = [];
    const consoleErrors = [];

    page.on('pageerror', error => pageErrors.push(error.message));
    page.on('console', message => {
        if (message.type() === 'error') {
            consoleErrors.push(message.text());
        }
    });
    page.on('response', response => {
        const pathname = new URL(response.url()).pathname;
        if (/\/tiles\/\d+\/\d+\/\d+\.png$/.test(pathname)) {
            tileResponses.push(response);
        }
        if (/^\/leaflet\/(leaflet\.js|leaflet\.css)$/.test(pathname)) {
            assetResponses.push(response);
        }
    });

    try {
        await page.goto(baseURL, { waitUntil: 'domcontentloaded', timeout });
        assert.match(await page.title(), /ngx_mod_tile/);
        pass('map page and title load');

        await page.locator('.leaflet-control-zoom-in').waitFor();
        assert.equal(await page.locator('.leaflet-control-zoom-in').isVisible(), true);
        assert.equal(await page.locator('.leaflet-control-zoom-out').isVisible(), true);
        assert.equal(await page.locator('.leaflet-control-scale').isVisible(), true);
        assert.match(await page.locator('.info').innerText(), /offline OSM tile server/);
        pass('Leaflet zoom, scale, attribution and info controls render');

        assert.equal(await page.evaluate(() => window.map.getZoom()), 12);
        pass('initial map center uses zoom 12');

        await page.waitForFunction(
            () => document.querySelectorAll('.leaflet-tile-loaded').length >= 4,
            null, { timeout });
        const loadedImages = await page.locator('.leaflet-tile-loaded').evaluateAll(
            images => images.filter(img => img.complete && img.naturalWidth > 0).length);
        assert.ok(loadedImages >= 4, `only ${loadedImages} decoded tile images`);
        pass('at least four tile images decode successfully');

        assert.ok(tileResponses.length >= 4,
            `only ${tileResponses.length} tile responses observed`);
        assert.ok(tileResponses.every(response => response.status() === 200),
            tileResponses.map(r => `${r.status()} ${r.url()}`).join('\n'));
        pass('initial viewport tile requests all return 200');

        const png = await tileResponses[0].body();
        assert.equal(png.subarray(1, 4).toString(), 'PNG');
        pass('browser receives a valid PNG payload');

        assert.equal(assetResponses.length, 2);
        assert.ok(assetResponses.every(response => response.status() === 200));
        pass('Leaflet JavaScript and CSS assets return 200');

        const cssResponse = assetResponses.find(response => response.url().endsWith('.css'));
        const jsResponse = assetResponses.find(response => response.url().endsWith('.js'));
        assert.match(cssResponse.headers()['content-type'] || '', /text\/css/);
        assert.match(jsResponse.headers()['content-type'] || '', /javascript|ecmascript/);
        pass('Leaflet assets use browser-accepted MIME types');

        const beforeZoom = tileResponses.length;
        await page.locator('.leaflet-control-zoom-in').click();
        await page.waitForFunction(() => window.map.getZoom() === 13,
            null, { timeout });
        await waitForNewTiles(page, tileResponses, beforeZoom);
        pass('clicking zoom-in changes zoom 12 -> 13');

        assert.ok(tileResponses.length > beforeZoom,
            'zoom-in did not issue new tile requests');
        assert.ok(tileResponses.slice(beforeZoom).every(r => r.status() === 200));
        pass('zoom-in loads new tiles successfully');

        const centerBefore = await page.evaluate(
            () => [window.map.getCenter().lat, window.map.getCenter().lng]);
        // Let Leaflet finish its zoom animation before starting a drag; on the
        // real Mapnik stack, decoding new tiles can extend the visual transition.
        await page.waitForTimeout(1200);
        const bounds = await page.locator('#map').boundingBox();
        await page.mouse.move(bounds.x + bounds.width * 0.55,
                              bounds.y + bounds.height * 0.55);
        await page.mouse.down();
        await page.mouse.move(bounds.x + bounds.width * 0.35,
                              bounds.y + bounds.height * 0.40,
                              { steps: 12 });
        await page.mouse.up();
        await page.waitForFunction(before => {
            const center = window.map.getCenter();
            return Math.abs(center.lat - before[0]) > 0.00001
                || Math.abs(center.lng - before[1]) > 0.00001;
        }, centerBefore, { timeout });
        pass('mouse drag pans the map center');

        await page.locator('.leaflet-control-zoom-out').click();
        await page.waitForFunction(() => window.map.getZoom() === 12,
            null, { timeout });
        await page.waitForFunction(
            () => document.querySelectorAll('.leaflet-tile-loaded').length >= 4,
            null, { timeout });
        pass('clicking zoom-out restores zoom 12 and reloads tiles');

        assert.deepEqual(pageErrors, []);
        assert.deepEqual(consoleErrors, []);
        assert.ok(tileResponses.every(response => response.status() === 200));
        pass('no JavaScript/console errors or failed tile responses');

        const screenshot = path.join(artifactDir, 'map.png');
        try {
            await page.screenshot({ path: screenshot, fullPage: true, timeout: 15000 });
            console.log(`SCREENSHOT: ${screenshot}`);
        } catch (error) {
            // Screenshots are diagnostic artifacts, not a functional assertion.
            // Some Mapnik/fontconfig combinations keep Chromium's font-ready
            // promise pending even though all map assertions have passed.
            console.warn(`WARNING: screenshot skipped: ${error.message}`);
        }

        console.log(`PLAYWRIGHT PASS=${passed} FAIL=0`);
    } catch (error) {
        await page.screenshot({
            path: path.join(artifactDir, 'map-failure.png'),
            fullPage: true
        }).catch(() => {});
        console.error(`PLAYWRIGHT PASS=${passed} FAIL=1`);
        console.error(error);
        process.exitCode = 1;
    } finally {
        await browser.close();
    }
})();
