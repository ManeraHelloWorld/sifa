#!/usr/bin/env node
'use strict';

// Optional real-browser integration test. Install Playwright outside the source
// checkout and point PLAYWRIGHT_MODULE at that package when it is not on NODE_PATH.
// SIFA_EXECUTABLE defaults to build/sifa (or build/Sifa.exe on Windows).
// CHROMIUM_EXECUTABLE_PATH selects an already installed Chromium; otherwise
// Playwright uses its own installed browser. No dependencies are downloaded here.
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

const root = path.resolve(__dirname, '..');
const executable = path.resolve(process.env.SIFA_EXECUTABLE || path.join(root, 'build', process.platform === 'win32' ? 'Sifa.exe' : 'sifa'));
const artifacts = path.resolve(process.env.SIFA_UI_ARTIFACTS || path.join(root, 'build'));

async function main() {
  const temp = await fs.mkdtemp(path.join(os.tmpdir(), 'sifa-ui-'));
  let app;
  let browser;
  let appErrors = '';
  const failures = [];
  let pointerResult = { status: 'unrun' };
  try {
    await fs.mkdir(artifacts, { recursive: true });
    const url = await new Promise((resolve, reject) => {
      app = spawn(executable, ['--no-open', '--port', '0', '--data-dir', temp], { stdio: ['ignore', 'pipe', 'pipe'] });
      let output = '';
      const timeout = setTimeout(() => reject(new Error('Sifa did not announce its local URL within 15 seconds')), 15000);
      app.on('error', (error) => { clearTimeout(timeout); reject(error); });
      app.on('exit', (code) => { clearTimeout(timeout); reject(new Error(`Sifa exited before browser startup (${code}): ${appErrors}`)); });
      app.stderr.on('data', (chunk) => { appErrors += chunk.toString(); });
      app.stdout.on('data', (chunk) => {
        output += chunk.toString();
        const match = output.match(/^SIFA_URL=(http:\/\/127\.0\.0\.1:\d+\/\?token=[a-f0-9]+)$/m);
        if (match) { clearTimeout(timeout); resolve(match[1]); }
      });
    });

    browser = await chromium.launch({
      headless: true,
      executablePath: process.env.CHROMIUM_EXECUTABLE_PATH || undefined,
      args: process.platform === 'linux' ? ['--no-sandbox', '--disable-dev-shm-usage'] : [],
    });
    const context = await browser.newContext({ viewport: { width: 1440, height: 1000 }, deviceScaleFactor: 1, acceptDownloads: true, reducedMotion: 'reduce' });
    const page = await context.newPage();
    page.setDefaultTimeout(12000);
    page.on('pageerror', (error) => failures.push(`Browser exception: ${error.message}`));
    page.on('request', (request) => {
      if (/^https?:/.test(request.url()) && new URL(request.url()).hostname !== '127.0.0.1') failures.push(`Unexpected external request: ${request.url()}`);
    });
    page.on('response', (response) => {
      if (response.status() >= 400 && !response.url().endsWith('/favicon.ico')) failures.push(`HTTP ${response.status()}: ${new URL(response.url()).pathname}`);
    });
    await page.goto(url, { waitUntil: 'networkidle' });
    await page.locator('#connection.online').waitFor();
    assert.equal(new URL(page.url()).search, '', 'session token is removed by the authentication redirect');
    const state = () => page.evaluate(async () => {
      const response = await fetch('/api/state');
      if (!response.ok) throw new Error(`State request failed: ${response.status}`);
      return response.json();
    });
    const initial = await state();
    assert.equal(initial.capabilities.android, false);
    assert.equal(initial.capabilities.game, false);
    assert.ok(initial.host.logical_cpus >= 1);

    // The visible guest console must contain actual interpreter output.
    await page.selectOption('#backend', 'software');
    await page.click('#run-demo');
    await page.waitForFunction(() => document.getElementById('guest-console').textContent.includes('[HALT]'));
    const demo = (await state()).last_run;
    assert.equal(demo.ok, true);
    assert.equal(demo.output, 'SIFA\n');
    assert.equal(demo.instructions, 11);
    assert.match(await page.locator('#guest-console').textContent(), /SIFA/);

    // Save calibration through the form, then confirm it survives a page reload.
    await page.click('[data-tab="input"]');
    await page.locator('#page-input').waitFor({ state: 'visible' });
    await page.fill('#dpi', '1200');
    await page.fill('#cm360', '41.5');
    await page.locator('#sensitivity').evaluate((input) => {
      input.value = '1.25';
      input.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await page.uncheck('#raw-input');
    await page.click('#input-form button[type="submit"]');
    await page.waitForFunction(() => document.getElementById('input-status').textContent.includes('Сохранено'));
    await page.reload({ waitUntil: 'networkidle' });
    await page.locator('#connection.online').waitFor();
    assert.equal(await page.inputValue('#dpi'), '1200');
    assert.equal(await page.inputValue('#cm360'), '41.5');
    assert.equal(await page.inputValue('#sensitivity'), '1.25');
    assert.equal(await page.isChecked('#raw-input'), false);

    // Test the browser's actual Pointer Lock API and CDP-delivered mouse events.
    // Physical mouse latency and OS raw counts are deliberately not inferred.
    await page.click('#capture-pointer');
    try {
      await page.waitForFunction(() => document.pointerLockElement?.id === 'aim-canvas', null, { timeout: 4000 });
      const box = await page.locator('#aim-canvas').boundingBox();
      assert.ok(box, 'mouse canvas has a visible rectangle');
      await page.mouse.move(box.x + box.width / 2 + 45, box.y + box.height / 2 + 15, { steps: 5 });
      await page.waitForFunction(() => Number(document.getElementById('events-value').textContent.replace(/\s/g, '')) > 0);
      assert.notEqual(await page.locator('#yaw-value').textContent(), '0,00°');
      pointerResult = { status: 'passed', input: 'Chromium pointer lock and browser mouse events; physical raw latency unmeasured' };
      // CDP keyboard events do not trigger Chromium's native Escape shortcut in
      // headless mode. Exercise release through the real Pointer Lock API.
      await page.evaluate(() => document.exitPointerLock());
      await page.waitForFunction(() => document.pointerLockElement === null);
      await page.click('#reset-lab');
      await page.waitForFunction(() => document.getElementById('events-value').textContent === '0');
      assert.equal(await page.locator('#events-value').textContent(), '0');
    } catch (error) {
      const locked = await page.evaluate(() => document.pointerLockElement !== null);
      if (locked) throw error; // A working lock with broken lab behavior is a failure.
      const message = await page.locator('#toast').textContent();
      if (!/отклонён|не поддерживает/i.test(message || '')) throw error;
      pointerResult = { status: 'unavailable', reason: message };
    }
    await page.screenshot({ path: path.join(artifacts, 'sifa-input.png'), fullPage: true });

    // Performance presets preserve mouse calibration; custom edits persist.
    await page.click('[data-tab="performance"]');
    await page.locator('#page-performance').waitFor({ state: 'visible' });
    await page.selectOption('#preset', 'performance');
    await page.click('#performance-form button[type="submit"]');
    await page.waitForFunction(() => document.getElementById('performance-status').textContent.includes('Сохранено'));
    const afterPreset = (await state()).settings;
    assert.equal(afterPreset.dpi, 1200);
    assert.equal(afterPreset.cm360, 41.5);
    assert.equal(afterPreset.sensitivity, 1.25);
    assert.equal(afterPreset.raw_input, false);
    await page.fill('#width', '1366');
    await page.fill('#height', '768');
    await page.click('#performance-form button[type="submit"]');
    await page.waitForFunction(() => document.getElementById('performance-status').textContent.includes('Сохранено'));
    assert.equal((await state()).settings.preset, 'custom');
    const saved = JSON.parse(await fs.readFile(path.join(temp, 'settings.json'), 'utf8'));
    assert.equal(saved.width, 1366);
    assert.equal(saved.height, 768);
    await page.reload({ waitUntil: 'networkidle' });
    await page.locator('#connection.online').waitFor();
    assert.equal(await page.inputValue('#width'), '1366');
    assert.equal(await page.inputValue('#height'), '768');
    assert.equal(await page.inputValue('#preset'), 'custom');

    const benchmarkResponse = page.waitForResponse((response) => response.url().endsWith('/api/benchmark'));
    await page.click('#run-benchmark');
    const benchmark = await (await benchmarkResponse).json();
    assert.equal(benchmark.samples, 200);
    assert.ok(Number.isFinite(benchmark.mean_ms) && benchmark.mean_ms > 0);
    assert.ok(benchmark.p95_ms >= benchmark.p50_ms);
    await page.waitForFunction(() => document.getElementById('benchmark-samples').textContent === '200');

    await page.click('[data-tab="diagnostics"]');
    await page.waitForFunction(() => document.getElementById('diagnostics-json').textContent.includes('"host"'));
    const downloading = page.waitForEvent('download');
    await page.click('#download-diagnostics');
    const download = await downloading;
    assert.equal(download.suggestedFilename(), 'sifa-diagnostics.json');
    const diagnosticPath = path.join(temp, 'downloaded-diagnostics.json');
    await download.saveAs(diagnosticPath);
    const diagnostic = JSON.parse(await fs.readFile(diagnosticPath, 'utf8'));
    assert.equal(diagnostic.host.os, initial.host.os);
    assert.equal(diagnostic.capabilities.android, false);
    assert.equal(diagnostic.settings.width, 1366);
    assert.equal(diagnostic.last_run.output, 'SIFA\n');

    await page.click('[data-tab="overview"]');
    await page.locator('#page-overview').waitFor({ state: 'visible' });
    await page.locator('#toast').waitFor({ state: 'hidden' });
    await page.screenshot({ path: path.join(artifacts, 'sifa-desktop.png'), fullPage: true });
    await page.setViewportSize({ width: 390, height: 844 });
    for (const tab of ['overview', 'performance', 'input', 'diagnostics']) {
      await page.click(`[data-tab="${tab}"]`);
      await page.locator(`#page-${tab}`).waitFor({ state: 'visible' });
      const overflow = await page.evaluate(() => document.documentElement.scrollWidth - innerWidth);
      assert.ok(overflow <= 1, `${tab} overflows mobile viewport by ${overflow}px`);
      if (tab === 'overview') {
        const statusOverflow = await page.locator('#hypervisor-value').evaluate((element) => element.scrollWidth - element.clientWidth);
        assert.ok(statusOverflow <= 1, `Virtualization status is clipped by ${statusOverflow}px on mobile`);
      }
    }
    await page.click('[data-tab="overview"]');
    await page.screenshot({ path: path.join(artifacts, 'sifa-mobile.png'), fullPage: true });
    assert.deepEqual(failures, [], 'no browser exceptions, API failures or external requests');
    assert.equal(appErrors, '', 'application did not report request errors');
    console.log(JSON.stringify({ result: 'passed', checks: ['authenticated local session', 'software guest execution', 'four navigation tabs', 'calibration persistence', 'preset calibration retention', 'custom profile persistence', 'real 200-sample benchmark', 'diagnostic JSON download', 'mobile overflow checks'], pointer_lab: pointerResult, artifacts }, null, 2));
  } finally {
    if (browser) await browser.close();
    if (app && app.exitCode === null) {
      app.kill();
      await new Promise((resolve) => { app.once('exit', resolve); setTimeout(resolve, 3000).unref(); });
    }
    await fs.rm(temp, { recursive: true, force: true });
  }
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
