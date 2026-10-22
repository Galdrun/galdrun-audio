// Headless Edge driven over native CDP, shared by the *_web.cjs browser tests.
// Each test keeps its own assertions and request interception.
const assert = require('node:assert/strict'), cp = require('node:child_process');

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function launchEdge(port, userDataDir) {
  return cp.spawn('C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', [
    '--headless', '--disable-gpu', '--no-first-run', '--remote-debugging-port=' + port,
    '--user-data-dir=' + userDataDir], {windowsHide: true, stdio: 'ignore'});
}

// Waits for Edge's debugger, opens a blank page and attaches to it.
// `send` talks to the browser, `call` and `evaluate` to the page.
async function attachPage(port, attempts = 50) {
  let info;
  for (let i = 0; i < attempts; i++) { try { info = await (await fetch(`http://localhost:${port}/json/version`)).json(); break; } catch { await sleep(100); } }
  assert(info, 'Edge did not start');
  const ws = new WebSocket(info.webSocketDebuggerUrl); await new Promise(resolve => ws.onopen = resolve);
  let id = 0; const pending = new Map();
  ws.onmessage = e => {
    const m = JSON.parse(e.data), p = pending.get(m.id);
    if (m.method === 'Network.loadingFailed') console.error('Network error', m.params);
    if (p) { pending.delete(m.id); m.error ? p.reject(Error(JSON.stringify(m.error))) : p.resolve(m.result); }
  };
  const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => { pending.set(++id, {resolve, reject}); ws.send(JSON.stringify({id, method, params, sessionId})); });
  const {targetId} = await send('Target.createTarget', {url: 'about:blank'});
  const {sessionId} = await send('Target.attachToTarget', {targetId, flatten: true});
  const call = (method, params) => send(method, params, sessionId);
  const evaluate = async expression => { const r = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true}); assert(!r.exceptionDetails, JSON.stringify(r.exceptionDetails)); return r.result.value; };
  return {ws, send, call, evaluate};
}

module.exports = {sleep, launchEdge, attachPage};
