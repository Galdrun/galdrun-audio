// One push connection per visible hardware panel. Configuration refreshes never
// recreate the feed or overwrite unsaved bindings; states also reach closed cards.
function connectHardwareInputEvents(container, url) {
  if (container.inputEventFeed) return;
  let source = null, retry = null;
  let usageReady, usageSamples = 0;
  container.inputUsageReady = new Promise(resolve => {usageReady = resolve});
  container.inputStates = new Map();
  const state = (id, pressed) => {
    container.inputStates.set(id, pressed);
    for (const item of container.querySelectorAll('.input-info')) {
      if (item.dataset.inputId !== id) continue;
      const badge = item.querySelector('.input-badge');
      badge.classList.toggle('pressed', pressed === true);
      badge.setAttribute('aria-label', tt(pressed === true ? 'inputs.pressed' :
        pressed === false ? 'inputs.released' : 'inputs.stateUnavailable', {id}));
    }
  };
  const reset = () => {
    for (const id of container.inputStates.keys()) state(id, null);
  };
  const close = () => {clearTimeout(retry); retry = null; source?.close(); source = null; reset()};
  const open = () => {
    if (source || document.hidden) return;
    const stream = new EventSource(url); source = stream;
    stream.addEventListener('system-usage', event => {
      if (source !== stream) return;
      try {
        const data = JSON.parse(event.data);
        window.updateUsageCharts?.(data);
        // CPU needs two counter samples; unsupported counters remain unknown.
        if (Number.isFinite(data?.cpuPercent) || ++usageSamples >= 2) usageReady();
      } catch (_) {window.updateUsageCharts?.(null)}
    });
    stream.addEventListener('input-snapshot', event => {
      if (source !== stream) return;
      try {
        const states = JSON.parse(event.data);
        if (!Array.isArray(states)) return;
        reset();
        for (const entry of states)
          if (typeof entry.id === 'string' && typeof entry.pressed === 'boolean') state(entry.id, entry.pressed);
      } catch (_) {reset()}
    });
    stream.addEventListener('input-state', event => {
      if (source !== stream) return;
      try {const entry = JSON.parse(event.data);
        if (typeof entry.id === 'string' && typeof entry.pressed === 'boolean') state(entry.id, entry.pressed);
      } catch (_) {reset()}
    });
    stream.onerror = () => {
      if (source !== stream) return;
      reset();
      window.updateUsageCharts?.(null);
      usageReady(); // Optional live metrics must not block an otherwise loaded page.
      // EventSource retries interrupted streams itself. A rejected connection
      // (e.g. the device's viewer limit) closes it permanently, so retry that too.
      if (source?.readyState === EventSource.CLOSED) {
        source.close(); source = null;
        retry = setTimeout(() => {retry = null; open()}, 3000);
      }
    };
  };
  container.inputEventFeed = {close};
  document.addEventListener('visibilitychange', () => document.hidden ? close() : open());
  window.addEventListener('pagehide', close);
  window.addEventListener('pageshow', open);
  open();
}
