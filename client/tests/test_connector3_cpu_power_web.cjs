// Hardware integration check: changes CPU speed and standby, then restores them.
// Run explicitly with the Connector 3 WebGui URL; never runs in CTest.
const assert = require('node:assert/strict'), path = require('node:path');
const {sleep, launchEdge, attachPage} = require('./edge_cdp.cjs');
const url = process.argv[2];
assert(url, 'Pass the Connector 3 WebGui URL');
async function api(command, params = {}) {
  const r = await (await fetch(new URL('/api', url), {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({command, params})
  })).json();
  assert.equal(r.ok, true, JSON.stringify(r));
  return r;
}
(async () => {
  const initial = await api('getCpu'), power = await api('getPower');
  assert.equal(initial.supported, true);
  assert.deepEqual(initial.cpu.frequenciesMhz, [792, 996]);
  assert.equal(power.state, 'on', 'Run this check with the client awake');
  const browser = launchEdge(9342, path.resolve('client/build/web-preview/edge-connector3-cpu'));
  let page;
  try {
    page = await attachPage(9342);
    await page.call('Page.enable');
    await page.call('Page.addScriptToEvaluateOnNewDocument', {
      source: "localStorage.setItem('galdrun.lang','en-US')"
    });
    for (const width of [1200, 320]) {
      await page.call('Emulation.setDeviceMetricsOverride', {
        width, height: 1000, deviceScaleFactor: 1, mobile: width < 400
      });
      await page.call('Page.navigate', {url});
      for (let i = 0; i < 60; ++i) {
        if (await page.evaluate("!!document.getElementById('cpuMhz') && !document.getElementById('cpuMhz').disabled && !document.getElementById('powerBox').classList.contains('hidden')")) break;
        await sleep(100);
      }
      assert(await page.evaluate("!document.getElementById('cpuBox').classList.contains('hidden')"));
      assert(await page.evaluate("document.getElementById('cpuNow').textContent.includes('MHz')"));
      assert(await page.evaluate("!document.getElementById('powerBox').classList.contains('hidden')"));
      assert(await page.evaluate("document.documentElement.scrollWidth <= innerWidth"));
      // Every save must really change the speed, including the second viewport.
      for (const mhz of [792, 996]) {
        await page.evaluate(`document.getElementById('cpuMhz').value='${mhz}';document.getElementById('cpuMhz').dispatchEvent(new Event('change',{bubbles:true}));document.getElementById('save').click()`);
        let cpu;
        for (let i = 0; i < 40; ++i) {
          cpu = (await api('getCpu')).cpu;
          if (cpu.configured && cpu.mhz === mhz && cpu.currentMhz === mhz) break;
          await sleep(100);
        }
        assert.equal(cpu.mhz, mhz);
        assert.equal(cpu.currentMhz, mhz);
        await sleep(2200);
        assert(await page.evaluate(`document.querySelector('.usage-cpu-clock').textContent.includes('${mhz}')`));
      }
      await page.evaluate("document.getElementById('standbyOn').click()");
      await sleep(500);
      assert.equal((await api('getPower')).state, 'standbyManual');
      await page.evaluate("document.getElementById('standbyOff').click()");
      await sleep(500);
      assert.equal((await api('getPower')).state, 'on');
      assert.equal(await page.evaluate("document.getElementById('deepSleepNow').classList.contains('hidden')"), !power.deepSleep);
      console.log(`${width}px: CPU display/save/readback, usage clock, standby/wake and capability gating passed`);
    }
  } finally {
    await api('standbyOff');
    await api('setCpu', {mhz: initial.cpu.configured ? initial.cpu.mhz : initial.cpu.currentMhz});
    if (page) {
      await page.send('Browser.close').catch(() => {});
      page.ws.close();
    }
    browser.kill();
  }
})().catch(error => {console.error(error); process.exitCode = 1;});
