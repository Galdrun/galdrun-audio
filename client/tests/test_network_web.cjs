// Run from the workspace root with Node; uses the installed Edge and native CDP.
// Network writes are intercepted. This check leaves device settings unchanged.
const assert=require('node:assert/strict'),path=require('node:path');
const {sleep,launchEdge,attachPage}=require('./edge_cdp.cjs');
const url=process.argv[2]||'http://10.0.0.38:4720/';
const browser=launchEdge(9336,path.resolve('client/build/web-preview/edge-network'));
(async()=>{let page;
 try{
  page=await attachPage(9336);const {send,call,evaluate}=page;
  await call('Page.enable');await call('Emulation.setEmulatedMedia',{features:[{name:'prefers-reduced-motion',value:'reduce'}]});
  await call('Page.addScriptToEvaluateOnNewDocument',{source:`const originalFetch=window.fetch;window.networkWrites=[];window.fetch=async(...args)=>{const request=args[1]?.body?JSON.parse(args[1].body):{};if(request.command==='scanNetworks')return new Response(JSON.stringify({ok:true,scan:{pending:true}}));if(request.command==='getNetworks')return new Response(JSON.stringify({ok:true,scan:{pending:false,networks:[{ssid:'Test',bssid:'aa:bb:cc:dd:ee:01',rssi:-40,channel:1,secure:true},{ssid:'Test',bssid:'aa:bb:cc:dd:ee:02',rssi:-65,channel:6,secure:true},{ssid:'Open',bssid:'aa:bb:cc:dd:ee:03',rssi:-70,channel:11,secure:false}]}}));if(request.command==='setNetwork'){networkWrites.push(request.params);return new Response(JSON.stringify({ok:true}),{status:200})}return originalFetch(...args)}`});
  for(const width of [1200,320]){
   await call('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:width<600});
   await call('Page.navigate',{url});
   for(let i=0;i<50;i++){if(await evaluate("!document.body.classList.contains('page-loading')&&!!document.getElementById('wifiScan')"))break;await sleep(100)}
   assert(await evaluate("!document.body.classList.contains('page-loading')"),'Page failed to load');
   assert(await evaluate("!document.getElementById('networkBox').classList.contains('hidden')&&!document.getElementById('wifiScan').disabled"),'Firmware capability did not enable controls');
   assert(await evaluate("document.getElementById('save').disabled"),'Initial form should be clean');
   assert(await evaluate("!document.getElementById('deviceNameBox').classList.contains('hidden')&&!document.getElementById('deviceName').disabled"),'Device name missing on firmware');
   const clientFields=['room','clientId','deviceName','server','device'];
   if(await evaluate("!document.getElementById('audioProfileBox').classList.contains('hidden')"))clientFields.splice(4,0,'audioProfile');
   assert.deepEqual(await evaluate("[...document.getElementById('room').closest('section').querySelectorAll('label')].filter(x=>x.getClientRects().length).map(x=>x.htmlFor)"),clientFields,'Visible client fields must keep Audio board before Output');
   const initialDeviceName=await evaluate("document.getElementById('deviceName').value");
   await evaluate("document.getElementById('deviceName').value='bad name'");
   assert(await evaluate("!document.getElementById('deviceName').checkValidity()"),'Invalid hostname accepted');
   await evaluate("document.getElementById('deviceName').value="+JSON.stringify(initialDeviceName));
   assert(await evaluate("document.getElementById('card-eth-address').textContent==='Ethernet address'&&document.getElementById('card-wifi-address').textContent==='Wi-Fi address'"),'Address interfaces are ambiguous');
   assert(await evaluate("!document.getElementById('wifiSsid').required"),'Ethernet-only devices must allow an empty Wi-Fi network');
   assert(await evaluate("document.documentElement.scrollWidth<=innerWidth"),'Horizontal overflow');
   assert(await evaluate("!document.body.textContent.includes('\\u00c2')&&!document.body.textContent.includes('\\u00c3')"),'Broken text encoding');
   if(width===1200)assert(await evaluate("['wifiSsid','wifiPassword','wifiAp','ethMode','wifiMode'].every(id=>{const input=document.getElementById(id),label=document.querySelector('label[for='+id+']'),a=input.getBoundingClientRect(),b=label.getBoundingClientRect();return b.right<a.left&&Math.abs(a.top-b.top)<20})"),'Labels are not aligned with their fields');
   assert(await evaluate("document.getElementById('wifiChart').querySelector('svg')!==null"),'Signal chart missing');
   await evaluate("document.getElementById('wifiEye').dispatchEvent(new PointerEvent('pointerdown'))");
   assert(await evaluate("document.getElementById('wifiPassword').type==='text'"),'Password reveal missing');
   await evaluate("document.getElementById('wifiEye').dispatchEvent(new PointerEvent('pointerup'))");
   assert(await evaluate("document.getElementById('wifiPassword').type==='password'"),'Password stays revealed');
   await evaluate("document.getElementById('wifiScan').click()");
   assert(await evaluate("document.getElementById('wifiScan').disabled"),'Scan did not show pending state');
   for(let i=0;i<30;i++){if(await evaluate("!document.getElementById('wifiScan').disabled"))break;await sleep(100)}
   assert(await evaluate("document.getElementById('wifiNetworks').children.length===2&&document.getElementById('save').disabled"),'Scan must deduplicate SSIDs and leave the draft unchanged');
   await evaluate("document.getElementById('deviceName').value='kitchen-connector';document.getElementById('deviceName').dispatchEvent(new Event('input',{bubbles:true}))");
   assert(await evaluate("!document.getElementById('save').disabled"),'Device name edit not tracked');
   await evaluate("document.getElementById('wifiNetworks').firstChild.click()");
   assert(await evaluate("document.getElementById('wifiSsid').value==='Test'&&document.getElementById('wifiAp').options.length===3"),'Network selection must list both access points');
   await evaluate("document.getElementById('wifiAp').value='aa:bb:cc:dd:ee:02'");
   await evaluate("document.getElementById('ethMode').value='static';document.getElementById('ethMode').dispatchEvent(new Event('change',{bubbles:true}))");
   assert(await evaluate("!document.getElementById('ethStatic').classList.contains('hidden')&&!document.getElementById('ethIp').disabled&&document.getElementById('ethIp').required"),'Static fields not enabled');
   await evaluate("document.getElementById('ethIp').value='10.0.0.38';document.getElementById('ethGateway').value='10.0.0.254';document.getElementById('form').dispatchEvent(new Event('input',{bubbles:true}))");
   assert(await evaluate("!document.getElementById('save').disabled"),'Network edit not tracked');
   await evaluate("document.getElementById('save').click()");
   for(let i=0;i<20;i++){if(await evaluate("networkWrites.length===1&&document.getElementById('save').disabled"))break;await sleep(100)}
   const writes=await evaluate('networkWrites');assert.equal(writes.length,1);assert.equal(writes[0].device.name,'kitchen-connector');assert.equal(writes[0].ethernet.ipMode,'static');assert.equal(writes[0].ethernet.ip,'10.0.0.38');assert.equal(writes[0].wifi.bssid,'aa:bb:cc:dd:ee:02');
   assert(await evaluate("document.getElementById('msg').textContent.includes('Network reconnects')&&document.getElementById('save').disabled"),'Saved state/reconnect message missing');
   if(width===1200){await call('Page.navigate',{url});await sleep(1500);await evaluate("window.scrollTo({top:document.getElementById('networkBox').offsetTop-160,behavior:'instant'})");await sleep(100);const shot=await call('Page.captureScreenshot',{format:'png'});require('node:fs').writeFileSync('client/build/web-preview/raumfeld-network-scan.png',Buffer.from(shot.data,'base64'))}
   await call('Page.navigate',{url});await sleep(1500);
   await evaluate("document.getElementById('ethMode').value='static';document.getElementById('ethMode').dispatchEvent(new Event('change',{bubbles:true}));document.getElementById('ethIp').value='10.0.0.38';document.getElementById('wifiSsid').value='';document.getElementById('wifiSsid').dispatchEvent(new Event('input',{bubbles:true}))");
   assert(await evaluate("document.getElementById('form').checkValidity()&&!document.getElementById('save').disabled"),'Ethernet-only save is blocked by Wi-Fi');
   await evaluate("document.getElementById('save').click()");await sleep(300);
   assert.equal((await evaluate('networkWrites')).length,1,'Ethernet-only save was not sent');
   console.log('PASS: live network page '+width+'px; labels, Scan, access points, password reveal, DHCP/static and save payload');
  }
  await call('Page.addScriptToEvaluateOnNewDocument',{source:`const previousFetch=window.fetch;window.fetch=async(...args)=>{if(args[1]?.body&&JSON.parse(args[1].body).command==='getNetwork')return new Response(JSON.stringify({ok:true,supported:false}),{status:200});return previousFetch(...args)}`});
  await call('Page.navigate',{url});await sleep(1000);
  assert(await evaluate("document.getElementById('networkBox').classList.contains('hidden')&&document.getElementById('wifiScan').disabled&&!document.body.classList.contains('page-loading')"),'OS-managed devices must hide network controls');
  assert(await evaluate("document.getElementById('deviceNameBox').classList.contains('hidden')&&document.getElementById('deviceName').disabled"),'OS-managed devices must hide device name');
  console.log('PASS: unsupported firmware hides and disables network controls');
  await send('Browser.close');
 }catch(error){console.error(error);process.exitCode=1}
 finally{page?.ws.close();browser.kill()}
})();
