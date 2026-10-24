// Windows/Edge integration check against an isolated built client. No real device is opened.
const assert = require('node:assert/strict'), cp = require('node:child_process');
const fs = require('node:fs'), os = require('node:os'), path = require('node:path');
const vm = require('node:vm');
for (const file of ['server-gui/www/app.js']) {
  const declaration=fs.readFileSync(file,'utf8').match(/const INPUT_ACTIONS = [\s\S]*?;/)[0];
  const actions=vm.runInNewContext(declaration+' INPUT_ACTIONS');
  assert.deepEqual(Array.from(actions.slice(actions.indexOf('toggleMute')+1,actions.indexOf('toggleMute')+5)),['standbyOn','standbyOff','toggleStandby','deepSleep']);
}
const {sleep, launchEdge, attachPage} = require('./edge_cdp.cjs');
const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'galdrun-power-web-'));
const configPath = path.join(temp, 'client.json'), cachePath = path.join(temp, 'audio.json');
const defaults = JSON.parse(fs.readFileSync('hardware/buildroot/board/connector2/client-default.json', 'utf8'));
fs.writeFileSync(configPath, JSON.stringify({...defaults, clientId:'power-web-test', name:'Power test', server:'', audioDeviceId:'test-only',
  controlPort:14720, controlBindAddress:'127.0.0.1', standbyAfterSeconds:0}));
fs.writeFileSync(cachePath, JSON.stringify({schemaVersion:3, platform:'windows', generatedAt:'2026-10-11T00:00:00Z',
  devices:[{id:'test-only',name:'Test only',isDefault:true,sharedFormats:[],
    exclusiveFormats:[{sampleRate:48000,channels:2,sampleType:'int16'}],
    hardwareClock:{available:true,control:'Drift compensator',minPpm:-500,maxPpm:500,stepPpm:1}},
    {id:'plain',name:'Plain',isDefault:false,sharedFormats:[],
    exclusiveFormats:[{sampleRate:44100,channels:2,sampleType:'int16'}]}]}));
const client = cp.spawn(path.resolve(process.argv[2] || 'client/build/windows-x64/Debug/galdrun-client.exe'),
  ['--config', configPath, '--audio-cache', cachePath, '--log-file', path.join(temp,'client.log'), '--log-level', 'debug'], {windowsHide:true, stdio:'ignore'});
const browser = launchEdge(9338, path.join(temp, 'edge'));
const api = async (command, params={}) => {
  const response = await fetch('http://127.0.0.1:14720/api', {method:'POST',
    headers:{'Content-Type':'application/json'},body:JSON.stringify({command,params})});
  const result = await response.json(); assert(result.ok, JSON.stringify(result)); return result;
};
(async () => {
  let page;
  try {
    let started = false;
    for (let i=0; i<100 && !started; i++) { try { await api('getPower'); started = true; } catch { await sleep(100); } }
    assert(started, 'Client did not start');
    const inputs = (await api('inputs')).inputs;
    assert.equal(inputs.find(i=>i.id==='POWER').bindings.longPress.action,'deepSleep');
    assert.deepEqual(inputs.map(i => [i.id, i.device, i.key, i.bindings.click.action]), [
      ['POWER', 'gpio-keys', 'KEY_POWER', 'toggleStandby'],
      ['SETUP', 'gpio-keys', '141', 'none'],
      ['RESET', 'gpio-keys', 'KEY_F3', 'none']]);
    await api('simulateInput', {id:'POWER',gesture:'click'});
    assert.equal((await api('getPower')).state,'standbyManual', 'Factory POWER binding must toggle off');
    await api('simulateInput', {id:'POWER',gesture:'click'});
    assert.equal((await api('getPower')).state, 'on', 'Factory POWER binding must toggle on');
    console.log('PASS: Connector 2 factory button names, wiring and default power toggle');
    page = await attachPage(9338, 100); const {send, call, evaluate} = page;
    const waitFor=async expression => { for (let i=0;i<100;i++) { if (await evaluate(expression)) return; await sleep(100); } console.error(await evaluate("({loader:document.querySelector('#pageLoader')?.textContent,message:document.querySelector('#msg')?.textContent,error:window.testError,failed:window.failedFetch})")); assert.fail(expression); };
    await call('Page.enable');
    await call('Network.enable');
    await call('Page.addScriptToEvaluateOnNewDocument',{source:"window.addEventListener('error',e=>window.testError=e.message);const originalFetch=window.fetch;window.fetch=async(...args)=>{try{return await originalFetch(...args)}catch(e){window.failedFetch=args[1]?.body;throw e}}"});
    for (const width of [1200,390,320]) {
      await api('setPower',{standbyAfterSeconds:0});
      await api('standbyOff');
      await call('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:width<600});
      await call('Page.navigate',{url:'http://127.0.0.1:14720/'});
      await waitFor("!document.body.classList.contains('page-loading')");
      // Linux displays detected hardware automatically. Exercise its shared
      // editor with the Windows client's configured simulation input.
      await evaluate("document.querySelector('#hardwareBox').classList.remove('hidden');refreshInputs(true)");
      await waitFor("!!document.querySelector('[data-input-id=POWER] select')");
      assert(await evaluate("document.querySelector('#save').disabled"));
      // The default drift mode is auto: it must load as itself, and every mode must save.
      assert.equal(await evaluate("document.querySelector('#driftCorrection').value"), 'auto');
      for (const mode of ['hardwareClock', 'resampling', 'auto']) {
        await evaluate(`document.querySelector('#msg').textContent='';document.querySelector('#driftCorrection').value='${mode}';document.querySelector('#driftCorrection').dispatchEvent(new Event('change',{bubbles:true}));document.querySelector('#save').click()`);
        await waitFor("document.querySelector('#save').disabled && document.querySelector('#msg').textContent==='Saved.'");
        assert.equal((await api('status')).status.audio.driftCorrection.configured, mode);
      }
      assert(await evaluate("!document.querySelector('#powerBox').classList.contains('hidden')"));
      assert(await evaluate("document.querySelector('#restart').classList.contains('hidden')"), 'Desktop clients must not offer a system restart');
      assert(await evaluate("!document.querySelector('#standbyOn').classList.contains('hidden') && document.querySelector('#standbyOff').classList.contains('hidden')"), 'Only Standby must show while on');
      const actions=await evaluate("[...document.querySelector('[data-input-id=POWER] select').options].map(o=>o.value)");
      assert.deepEqual(actions.slice(actions.indexOf('toggleMute')+1,actions.indexOf('toggleMute')+5),['standbyOn','standbyOff','toggleStandby','action1'], 'Deep sleep must be hidden where unsupported');
      assert(await evaluate("[...document.querySelectorAll('[data-input-id=POWER] select')].some(s=>s.value==='deepSleep')"), 'An existing deep sleep binding must stay visible');
      assert(await evaluate("document.documentElement.scrollWidth<=innerWidth"), 'Horizontal overflow');
      assert(await evaluate("document.querySelector('#deepSleepFields').classList.contains('hidden') && document.querySelector('#deepSleepNow').classList.contains('hidden')"), 'Deep sleep settings must be hidden where unsupported');
      await evaluate("document.querySelector('#standbyMinutes').value='2';document.querySelector('#standbyMinutes').dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#save').click()");
      await waitFor("document.querySelector('#save').disabled && document.querySelector('#msg').textContent==='Saved.'");
      assert.equal((await api('getPower')).standbyAfterSeconds,120);
      await evaluate("document.querySelector('#standbyMinutes').value='-1';document.querySelector('#standbyMinutes').dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#save').click()");
      await sleep(200);
      assert.equal((await api('getPower')).standbyAfterSeconds,120, 'Invalid value was saved');
      await evaluate("document.querySelector('#standbyMinutes').value='2';document.querySelector('#standbyMinutes').dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#standbyOn').click()");
      await waitFor("document.querySelector('#powerNow').textContent==='State: standbyManual'");
      assert(await evaluate("document.querySelector('#standbyOn').classList.contains('hidden') && !document.querySelector('#standbyOff').classList.contains('hidden')"), 'Only Wake must show in standby');
      assert.equal((await api('getPower')).state,'standbyManual');
      await evaluate("document.querySelector('#standbyOff').click()");
      await waitFor("document.querySelector('#powerNow').textContent==='State: on'");
      assert.equal((await api('getPower')).state,'on');
      await evaluate("const s=document.querySelector('[data-input-id=POWER] select');s.value='toggleStandby';s.dispatchEvent(new Event('change',{bubbles:true}));document.querySelector('#save').click()");
      await waitFor("document.querySelector('#save').disabled");
      await api('simulateInput',{id:'POWER',gesture:'click'});
      assert.equal((await api('getPower')).state,'standbyManual');
      await api('simulateInput',{id:'POWER',gesture:'click'});
      assert.equal((await api('getPower')).state,'on');
      console.log('PASS: power selector order, timeout save/validation, On/Off, saved button toggle and layout',width);
    }
    // An output without a clock hides the drift control; a later save of
    // anything else must not send the hidden control's empty value.
    await call('Page.navigate',{url:'http://127.0.0.1:14720/'});
    await waitFor("!document.body.classList.contains('page-loading')");
    await evaluate("document.querySelector('#msg').textContent='';const d=document.querySelector('#device');d.value='plain';d.dispatchEvent(new Event('change',{bubbles:true}));document.querySelector('#save').click()");
    await waitFor("document.querySelector('#msg').textContent==='Saved.' && document.querySelector('#driftBox').classList.contains('hidden')");
    await waitFor("document.querySelector('#save').disabled");
    await evaluate("document.querySelector('#msg').textContent='';const r=document.querySelector('#room');r.value='Power renamed';r.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#save').click()");
    await waitFor("document.querySelector('#save').disabled && document.querySelector('#msg').textContent==='Saved.'");
    const status = (await api('status')).status;
    assert.equal(status.name, 'Power renamed');
    assert.equal(status.audio.driftCorrection.configured, 'auto');
    console.log('PASS: hidden drift control is kept across a save after its output lost the clock');

    // Loaded on a nondefault output: Automatic must show the default's
    // different formats immediately, and agree with the API after saving.
    await call('Page.navigate',{url:'http://127.0.0.1:14720/'});
    await waitFor("!document.body.classList.contains('page-loading')");
    assert.equal(await evaluate("document.querySelector('#device').value"), 'plain');
    assert((await evaluate("document.querySelector('#audioFormats').textContent")).includes('44.1 kHz'));
    await evaluate("{const d=document.querySelector('#device');d.value='';d.dispatchEvent(new Event('change',{bubbles:true}))}");
    assert.equal(await evaluate("document.querySelector('#formatDevice').textContent"), 'Test only \u00b7 direct hardware formats');
    assert((await evaluate("document.querySelector('#audioFormats').textContent")).includes('48 kHz'));
    await evaluate("document.querySelector('#msg').textContent='';document.querySelector('#save').click()");
    await waitFor("document.querySelector('#msg').textContent==='Saved.' && document.querySelector('#save').disabled");
    const automatic = await api('audioDevices');
    assert.equal(automatic.configuredDeviceId, '');
    assert.equal(automatic.selectedDeviceId, 'test-only');
    assert.equal(await evaluate("document.querySelector('#formatDevice').textContent"), 'Test only \u00b7 direct hardware formats');
    assert((await evaluate("document.querySelector('#audioFormats').textContent")).includes('48 kHz'));
    console.log('PASS: Automatic output displays the default formats before and after saving');

    // Keep an unsaved output across Rescan without probing physical devices.
    // Only the scan command is stubbed; loadDevices reads the real cache/API.
    await evaluate(`(async()=>{const d=document.querySelector('#device');d.value='plain';d.dispatchEvent(new Event('change',{bubbles:true}));
      const fetchBeforeRescan=window.fetch;
      window.fetch=(url,options)=>options?.body && JSON.parse(options.body).command==='rescanAudio'
        ? Promise.resolve(new Response(JSON.stringify({ok:true,deviceCount:2}),{headers:{'Content-Type':'application/json'}}))
        : fetchBeforeRescan(url,options);
      try {await document.querySelector('#rescan').onclick()} finally {window.fetch=fetchBeforeRescan}})()`);
    assert.equal(await evaluate("document.querySelector('#msg').textContent"), '2 device(s) found.');
    assert.equal(await evaluate("document.querySelector('#device').value"), 'plain');
    assert.equal(await evaluate("document.querySelector('#formatDevice').textContent"), 'Plain \u00b7 direct hardware formats');
    assert((await evaluate("document.querySelector('#audioFormats').textContent")).includes('44.1 kHz'));
    assert(!(await evaluate("document.querySelector('#save').disabled")));
    assert.equal((await api('audioDevices')).configuredDeviceId, '');
    console.log('PASS: Rescan preserves an unsaved output and its matching formats');
    await send('Browser.close');
  } catch (error) { console.error(error, 'client exit code:',client.exitCode); if(fs.existsSync(path.join(temp,'client.log'))) console.error(fs.readFileSync(path.join(temp,'client.log'),'utf8').split('\n').slice(-15).join('\n')); process.exitCode=1; }
  finally {
    page?.ws.close(); client.kill(); browser.kill();
    // Wait for Windows to release the client/profile handles before cleanup.
    await sleep(500);
    fs.rmSync(temp,{recursive:true,force:true,maxRetries:10,retryDelay:200});
  }
})();
