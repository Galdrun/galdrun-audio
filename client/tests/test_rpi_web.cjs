// Exercise Pi profile/radio/SSH controls on the served page. Writes are intercepted.
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path');
const {sleep, launchEdge, attachPage} = require('./edge_cdp.cjs');
const url = process.argv[2];
assert(url, 'Pass the URL of an isolated Pi test client');
const profiles = JSON.parse(fs.readFileSync('hardware/buildroot/board/rpi/audio-profiles.json', 'utf8'));
const browser = launchEdge(9341, path.resolve('client/build/web-preview/edge-rpi'));
(async () => {
  let page;
  try {
    page = await attachPage(9341);
    const {call, evaluate, send} = page;
    await call('Page.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `
      const originalFetch = window.fetch;
      const failStartup = sessionStorage.getItem('piRestartRetryTest') === 'fail';
      if (failStartup) sessionStorage.setItem('piRestartRetryTest', 'retrying');
      window.piWrites = [];
      window.confirm = () => true;
      let profile = 'usb', enabled = false;
      const profiles = ${JSON.stringify([...profiles].reverse())};
      window.fetch = async (...args) => {
        const r = args[1]?.body ? JSON.parse(args[1].body) : {};
        if (failStartup && r.command === 'getCpu') throw Error('Simulated transient startup failure');
        const answer = value => new Response(JSON.stringify({ok:true, ...value}));
        if (r.command === 'getAudioProfile') return answer({supported:true, profiles, profile, rebootRequired:profile!=='usb'});
        if (r.command === 'audioDevices') return answer({requiresConfiguredDevice:true, configuredDeviceId:'',
          cache:{devices:[{id:'hw:CARD=Test,DEV=0', name:'Test DAC', exclusiveFormats:[{sampleRate:48000, channels:2, sampleType:'int16'}]}]}});
        if (r.command === 'setAudioProfile') {
          piWrites.push(r);
          profile = r.params.profile === 'rollback' ? 'usb' : r.params.profile;
          return answer({supported:true, restarting:true});
        }
        if (r.command === 'getPower') {
          const value = await (await originalFetch(...args)).json();
          return answer({...value, restart:true});
        }
        if (r.command === 'restart') { piWrites.push(r); return answer({restarting:true}); }
        if (r.command === 'getSsh' || r.command === 'setSsh') {
          if (r.command === 'setSsh') { piWrites.push(r); enabled = r.params.enabled; }
          return answer({supported:true, ssh:{enabled, running:enabled, passwordSet:enabled}});
        }
        if (r.command === 'getNetwork') {
          const response = await originalFetch(...args), value = await response.json();
          Object.assign(value.network.wifi, {radio:'02:11:22:33:44:01', activeInterface:'wlan1', apSupported:false,
            radios:[{id:'02:11:22:33:44:00',interface:'wlan0'},{id:'02:11:22:33:44:01',interface:'wlan1'}]});
          return answer(value);
        }
        if (r.command?.startsWith('set') || r.command === 'resetDac') throw Error('Unexpected write: '+r.command);
        return originalFetch(...args);
      };`});
    for (const width of [1200, 320]) {
      await call('Emulation.setDeviceMetricsOverride', {width, height:900, deviceScaleFactor:1, mobile:width<600});
      await call('Page.navigate', {url});
      for (let i=0; i<80; i++) {
        if (await evaluate("!document.body.classList.contains('page-loading')")) break;
        await sleep(100);
      }
      assert(await evaluate("!document.body.classList.contains('page-loading')"), 'Page failed to load');
      assert.equal(await evaluate("document.getElementById('audioProfile').options.length"), profiles.length, 'Missing profile catalogue');
      const boardOptions = await evaluate("[...document.getElementById('audioProfile').options].map(o => ({id:o.value, name:o.text}))");
      assert.equal(boardOptions.shift().id, 'usb', 'USB / onboard audio must stay first');
      const boardNames = boardOptions.map(o => o.name);
      assert.deepEqual(boardNames, [...boardNames].sort((a, b) => a.localeCompare(b, 'en', {sensitivity:'base'})), 'Audio boards must be alphabetical');
      assert.equal(await evaluate("document.getElementById('audioProfile').value"), 'usb', 'Sorting changed the selected profile');
      assert(await evaluate("Math.abs(document.getElementById('audioProfileRollback').getBoundingClientRect().right-document.getElementById('audioProfileRollback').parentElement.getBoundingClientRect().right)<1"), 'Profile buttons must align with the right edge');
      assert(await evaluate("document.getElementById('audioProfile').closest('section')===document.getElementById('device').closest('section') && !!(document.getElementById('audioProfile').compareDocumentPosition(document.getElementById('device')) & Node.DOCUMENT_POSITION_FOLLOWING)"), 'Audio board must precede Output in Client');
      assert(await evaluate("!document.getElementById('cpuBox').textContent.includes('300 MHz') && !document.getElementById('cpuBox').textContent.includes('720 MHz')"), 'Device-specific CPU guidance');
      await evaluate("document.getElementById('audioProfile').value='inno-maker-hifi-dac';document.getElementById('audioProfile').dispatchEvent(new Event('change'))");
      assert(await evaluate("document.getElementById('audioProfileHint').textContent.includes('BossDAC')"), 'INNO-MAKER driver identification missing');
      assert(await evaluate("!document.getElementById('audioFormatsEmpty').classList.contains('hidden') && document.getElementById('audioFormats').classList.contains('hidden') && Math.abs(document.getElementById('audioFormatsEmpty').getBoundingClientRect().width-document.getElementById('formatsBox').getBoundingClientRect().width)<1 && getComputedStyle(document.getElementById('audioFormatsEmpty')).fontSize===getComputedStyle(document.body).fontSize"), 'Empty formats message must use full content width and normal text size');
      await evaluate("document.getElementById('device').value='hw:CARD=Test,DEV=0';document.getElementById('device').dispatchEvent(new Event('change'))");
      assert(await evaluate("document.getElementById('audioFormatsEmpty').classList.contains('hidden') && !document.getElementById('audioFormats').classList.contains('hidden') && document.getElementById('audioFormats').textContent.includes('48 kHz')"), 'Cached formats must replace the empty message');
      await evaluate("document.getElementById('device').value='';document.getElementById('device').dispatchEvent(new Event('change'))");
      assert(await evaluate("!document.getElementById('audioFormatsEmpty').classList.contains('hidden') && document.getElementById('audioFormats').classList.contains('hidden')"), 'Empty message must return when output is cleared');
      assert(await evaluate("document.getElementById('wifiRadio').value==='02:11:22:33:44:01'"), 'Radio selection missing');
      assert(await evaluate("document.getElementById('wifiRadioHint').textContent.includes('station mode only')"));
      assert(await evaluate("document.documentElement.scrollWidth<=innerWidth"), 'Horizontal overflow');
      await evaluate("document.getElementById('audioProfile').value='hifiberry-dacplusdsp';document.getElementById('audioProfile').dispatchEvent(new Event('change'))");
      assert(await evaluate("document.getElementById('audioProfileHint').textContent.includes('DSP')"), 'DSP prerequisite missing');
      await evaluate("document.getElementById('sshToggle').click()");
      await sleep(100);
      assert(await evaluate("!piWrites.some(r=>r.command==='setSsh')"), 'SSH enabled without choosing a password');
      await evaluate("document.getElementById('sshPassword').value='browser-test-only';document.getElementById('sshRepeat').value='browser-test-only';document.getElementById('sshToggle').click()");
      for (let i=0;i<30;i++) { if (await evaluate("document.getElementById('sshState').textContent.includes('SSH is on:')")) break; await sleep(100); }
      assert(await evaluate("piWrites.some(r=>r.command==='setSsh'&&r.params.enabled&&r.params.password==='browser-test-only')"));
      assert(await evaluate("document.getElementById('deepSleepNow').classList.contains('hidden')"), 'Unsupported deep sleep exposed');
      assert(await evaluate("!document.getElementById('restart').classList.contains('hidden')"), 'Supported restart must be visible');
      await evaluate("document.getElementById('audioProfileApply').click()");
      for (let i=0;i<30;i++) { if (await evaluate("document.getElementById('msg').textContent.includes('Restarting and scanning')")) break; await sleep(100); }
      assert(await evaluate("piWrites.some(r=>r.command==='setAudioProfile'&&r.params.profile==='hifiberry-dacplusdsp')"));
      assert(await evaluate("document.querySelector('main').inert && document.getElementById('save').disabled && document.getElementById('msg').textContent.includes('Waiting for the device')"), 'Profile changes must enter automatic restart/reconnect state');
      console.log('PASS: Pi profiles/DSP prerequisite/reboot, radio capability, web SSH opt-in and no deep sleep at '+width+'px');
    }
    await call('Page.navigate', {url});
    for (let i=0;i<80;i++) { if (await evaluate("!document.body.classList.contains('page-loading')")) break; await sleep(100); }
    await evaluate("document.getElementById('restart').click()");
    for (let i=0;i<30;i++) { if (await evaluate("piWrites.some(r=>r.command==='restart')")) break; await sleep(100); }
    assert(await evaluate("piWrites.filter(r=>r.command==='restart').length===1 && document.getElementById('restart').disabled && document.querySelector('main').inert"), 'Manual restart must send exactly one command and wait for reconnection');
    console.log('PASS: manual restart capability and reconnect state');
    await evaluate("sessionStorage.setItem('piRestartRetryTest','fail');sessionStorage.setItem('galdrunRestartUntil',String(Date.now()+60000))");
    await call('Page.navigate', {url});
    let recovered = false;
    for (let i=0;i<180;i++) {
      if (await evaluate("sessionStorage.getItem('piRestartRetryTest')==='retrying' && !document.body.classList.contains('page-loading') && !sessionStorage.getItem('galdrunRestartUntil')")) { recovered = true; break; }
      await sleep(100);
    }
    assert(recovered, 'A transient startup failure after reboot must retry and reconnect automatically');
    await evaluate("sessionStorage.removeItem('piRestartRetryTest')");
    console.log('PASS: automatic recovery from a transient page-load failure after restart');
    await send('Browser.close');
  } finally { page?.ws.close(); browser.kill(); }
})().catch(e => {console.error(e); process.exitCode=1;});
