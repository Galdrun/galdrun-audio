// Exercise the shared ESP32 page with intercepted board routes; never changes WLAN.
const assert = require('node:assert/strict'), path = require('node:path');
const {sleep, launchEdge, attachPage} = require('./edge_cdp.cjs');
const url = process.argv[2];
assert(url, 'Pass a client page URL serving the current shared assets');
const browser = launchEdge(9347, path.resolve('client/build/web-preview/edge-wifi-advanced'));
(async () => {
  let page;
  try {
    page = await attachPage(9347);
    const {call, evaluate} = page;
    await call('Page.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `
      localStorage.setItem('galdrun.lang','en-US');
      const originalFetch = window.fetch;
      window.wifiWrites = [];
      const formats = [{sampleRate:44100,sampleType:'int16'},{sampleRate:48000,sampleType:'int32'}];
      window.fetch = async (...args) => {
        const route = new URL(args[0],location.href).pathname;
        const answer = value => new Response(JSON.stringify(value),{status:200});
        if(route==='/api/settings') {
          if(args[1]?.method==='POST') { wifiWrites.push(JSON.parse(args[1].body)); return answer({ok:true}); }
          return answer({setupMode:true,board:'esp32-s3-devkitc',ssid:'Test',passwordSet:true,country:'AT',
            deviceName:'',defaultDeviceName:'esp32-test',bssid:'',ipMode:'dhcp',ip:'',netmask:'255.255.255.0',gateway:'',dns:'',
            name:'Test board',clientId:'esp32-test',server:'10.0.0.6:4711',active:true,audioDeviceId:'i2s',
            i2sClockMode:'master',i2sSlaveSampleRate:48000,bufferMs:2500,i2sRates:[44100,48000],
            i2sExternalFormats:formats,es8388Formats:formats});
        }
        if(route==='/api/inputs') return answer({ok:true,inputs:[]});
        if(route==='/api/networks') return answer([]);
        if(args[1]?.method==='POST') throw Error('Unexpected device write: '+route);
        return originalFetch(...args);
      };`});
    for (const width of [1200,320]) {
      await call('Emulation.setDeviceMetricsOverride',{width,height:1000,deviceScaleFactor:1,mobile:width<600});
      await call('Page.navigate',{url});
      for(let i=0;i<80;i++) {
        if(await evaluate("!document.body.classList.contains('page-loading')")) break;
        await sleep(100);
      }
      assert(await evaluate("!document.body.classList.contains('page-loading')"),'ESP32 page failed to load');
      assert(await evaluate("!document.getElementById('wifiAdvanced').classList.contains('hidden')"),'ESP32 advanced settings hidden');
      assert(await evaluate("document.getElementById('wifiEthernetPriority').classList.contains('hidden')"),'ESP32 shows Ethernet-only advice');
      assert.equal(await evaluate("document.getElementById('wifiCountry').value"),'AT');
      assert(await evaluate("document.getElementById('save').disabled"),'Loaded settings must be clean');
      await evaluate("document.getElementById('wifiAdvanced').open=true");
      assert(await evaluate("document.getElementById('wifiCountry').getBoundingClientRect().top-document.querySelector('#wifiAdvanced summary').getBoundingClientRect().bottom>=16"),'Missing heading gap');
      await evaluate("document.getElementById('wifiCountry').value='at'");
      assert(await evaluate("!document.getElementById('wifiCountry').checkValidity()"),'Lowercase country accepted');
      const country = width===1200 ? 'DE' : '';
      await evaluate(`document.getElementById('wifiCountry').value=${JSON.stringify(country)};
        document.getElementById('wifiClearPassword').checked=true;
        document.getElementById('form').dispatchEvent(new Event('input',{bubbles:true}))`);
      assert(await evaluate("document.getElementById('form').checkValidity()&&!document.getElementById('save').disabled"),'Advanced changes not saveable');
      assert(await evaluate("document.documentElement.scrollWidth<=innerWidth"),'Horizontal overflow');
      await evaluate("document.getElementById('save').click()");
      for(let i=0;i<30;i++) { if(await evaluate('wifiWrites.length===1')) break; await sleep(100); }
      const writes = await evaluate('wifiWrites');
      assert.equal(writes.length,1);
      assert.equal(writes[0].country,country);
      assert.equal(writes[0].clearPassword,true);
      assert(!('password' in writes[0]),'A saved password must not be invented or disclosed');
      console.log('PASS: ESP32 advanced Wi-Fi '+width+'px; spacing, country/automatic, explicit password clearing');
    }
  } finally {
    if(page) {await page.send('Browser.close').catch(()=>{});page.ws.close();}
    browser.kill();
  }
})().catch(error=>{console.error(error);process.exitCode=1});
