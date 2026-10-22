// Fixed percentage/dBm scales, 60 samples (half a second apart unless sampleMs
// says otherwise). Missing data creates gaps. Returns the update function, which
// is also window.updateUsageCharts for pages with one set of charts.
function createUsageCharts(container, wifiContainer, sampleMs = 500) {
  const definitions = [
    {name:'CPU', key:'cpuPercent', min:0, max:100, unit:'%', parent:container},
    {name:'RAM', key:'ramPercent', min:0, max:100, unit:'%', parent:container}
  ];
  if (wifiContainer) definitions.push({name:'Signal', key:'wifiRssiDbm', min:-100, max:-30, unit:' dBm', parent:wifiContainer});
  const charts = definitions.map(({name, key, min, max, unit, parent}) => {
    const card = document.createElement('figure');
    card.className = 'usage-chart';
    card.innerHTML = '<figcaption><strong>' + name + '</strong><output>—</output></figcaption>' +
      '<p class="usage-size">' + (key === 'cpuPercent' ? '<span class="usage-cpu-model">CPU unavailable</span><span class="usage-cpu-clock">Clock unavailable</span>' : '') +
      '</p><div class="usage-plot"><span class="usage-max">' + max + unit +
      '</span><span class="usage-min">' + min + unit + '</span>' +
      '<svg viewBox="0 0 240 100" preserveAspectRatio="none" role="img" aria-label="' + name +
      ' history"><path class="usage-grid" d="M24 8H236M24 50H236M24 92H236"/>' +
      '<path class="usage-area"/><path class="usage-line"/></svg></div>' +
      '<div class="usage-times"><span>' + Math.round(60 * sampleMs / 1000) + 's ago</span><span>Now</span></div>';
    parent.append(card);
    return {card, key, min, max, unit, history: []};
  });
  const bytes = value => {
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let index = 0;
    while (value >= 1024 && index < units.length - 1) {value /= 1024; ++index;}
    return value.toLocaleString(undefined, {maximumFractionDigits:index ? 2 : 0}) + ' ' + units[index];
  };
  const update = data => {
    for (const {card, key, min, max, unit, history} of charts) {
      const raw = data?.[key];
      const value = typeof raw === 'number' && Number.isFinite(raw) ? raw : null;
      history.push(value); if (history.length > 60) history.shift();
      card.querySelector('output').textContent = value === null ? '—' : value.toFixed(key === 'wifiRssiDbm' ? 0 : 1) + unit;
      const size = card.querySelector('.usage-size');
      if (key === 'cpuPercent' && data) {
        const model = typeof data.cpuModel === 'string' && data.cpuModel.trim() ? data.cpuModel : 'CPU unavailable';
        const clock = Number.isFinite(data.cpuMhz) && data.cpuMhz > 0 ? data.cpuMhz.toLocaleString() + ' MHz' : 'Clock unavailable';
        size.querySelector('.usage-cpu-model').textContent = model;
        size.querySelector('.usage-cpu-clock').textContent = clock;
        size.title = model + ' · ' + clock;
      }
      if (key === 'ramPercent') {
        const used = data?.ramUsedBytes, total = data?.ramTotalBytes;
        size.textContent = Number.isFinite(used) && Number.isFinite(total) && used >= 0 && total > 0 && used <= total ?
          bytes(used) + ' used / ' + bytes(total) + ' total' : 'Size unavailable';
        size.title = Number.isFinite(used) && Number.isFinite(total) ? used.toLocaleString() + ' / ' + total.toLocaleString() + ' bytes' : '';
      }
      card.querySelector('svg').setAttribute('aria-label', card.querySelector('strong').textContent +
        (value === null ? ' unavailable' : ' ' + value.toFixed(1) + (unit === '%' ? ' percent' : unit)) +
        ', scale ' + min + ' to ' + max + (unit === '%' ? ' percent' : unit));
      let line = '', area = '', segment = [], previous = false;
      const finish = () => {if (segment.length) {
        area += 'M' + segment[0][0] + ',92L' + segment.map(p => p.join(',')).join('L') +
          'L' + segment.at(-1)[0] + ',92Z'; segment = [];
      }};
      history.forEach((v, i) => {
        if (v === null) {previous = false; finish(); return;}
        const x = 24 + (60 - history.length + i) * 212 / 59,
          y = 92 - (Math.max(min, Math.min(max, v)) - min) * 84 / (max - min);
        line += (previous ? 'L' : 'M') + x.toFixed(2) + ',' + y.toFixed(2);
        segment.push([x.toFixed(2), y.toFixed(2)]); previous = true;
      });
      finish();
      card.querySelector('.usage-line').setAttribute('d', line);
      card.querySelector('.usage-area').setAttribute('d', area);
    }
  };
  window.updateUsageCharts = update;
  return update;
}
