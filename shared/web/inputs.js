// Shared hardware details. An optional callback saves bindings on the client.
function renderHardwareInputs(data, container, saveBindings) {
  const rows = [];
  const expanded = new Set([...container.querySelectorAll('.input-info details[open]')]
    .map(details => details.dataset.inputId));
  const snapshot = JSON.stringify(data);
  if (saveBindings && (container.dataset.snapshot === snapshot ||
      container.inputEdits?.dirty || container.inputEdits?.saving || container.contains(document.activeElement))) return;
  const text = (tag, value, className = '') => {
    const element = document.createElement(tag);
    element.textContent = value;
    element.className = className;
    return element;
  };
  // Identifiers by their key (inputs.name.togglePause), else as words: "togglePause" -> "Toggle Pause".
  const friendly = value => {
    if (/^action\d+$/.test(value)) return tt('inputs.userAction', {n: value.slice(6)});
    const name = tv('inputs.name', value);
    return name !== value ? name : value.replace(/([a-z])([A-Z0-9])/g, '$1 $2').replace(/^./, letter => letter.toUpperCase());
  };
  const percent = value => Number((value * 100).toFixed(2)) + '%';
  const icons = {
    button: '<rect x="4" y="4" width="16" height="16" rx="5"/>',
    encoder: '<circle cx="12" cy="12" r="8"/><path d="M12 4v6"/>',
    click: '<path d="M9 12V5a2 2 0 0 1 4 0v5l4 1a3 3 0 0 1 2 3l-1 6H9l-5-6a2 2 0 0 1 3-2l2 2"/>',
    doubleClick: '<circle cx="9" cy="12" r="6"/><circle cx="15" cy="12" r="6"/>',
    longPress: '<circle cx="12" cy="13" r="8"/><path d="M12 9v4l2 2M10 2h4M12 2v3"/>',
    clockwise: '<path d="M20 9a8 8 0 1 0 0 6M20 3v6h-6"/>',
    counterClockwise: '<path d="M4 9a8 8 0 1 1 0 6M4 3v6h6"/>',
    none: '<circle cx="12" cy="12" r="8" stroke-dasharray="3 3"/>',
    play: '<path d="m8 5 11 7-11 7Z"/>',
    pause: '<path d="M8 5v14M16 5v14"/>',
    togglePause: '<path d="m4 5 9 7-9 7ZM18 5v14"/>',
    stop: '<rect x="5" y="5" width="14" height="14" rx="1"/>',
    next: '<path d="m4 5 11 7-11 7ZM19 5v14"/>',
    previous: '<path d="m20 5-11 7 11 7ZM5 5v14"/>',
    volumeUp: '<path d="M3 9h4l5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8M19 5a10 10 0 0 1 0 14"/>',
    volumeDown: '<path d="M3 9h4l5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8"/>',
    setVolume: '<path d="M3 9h4l5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8"/>',
    toggleMute: '<path d="M3 9h4l5-4v14l-5-4H3ZM17 9l5 6M22 9l-5 6"/>',
    standbyOff: '<path d="M12 2v10M6 5a9 9 0 1 0 12 0"/>',
    standbyOn: '<path d="M12 2v10M6 5a9 9 0 1 0 12 0"/>',
    toggleStandby: '<path d="M12 2v10M6 5a9 9 0 1 0 12 0"/>',
    deepSleep: '<path d="M20 15A8 8 0 0 1 9 4a8 8 0 1 0 11 11Z"/>',
    room: '<path d="m3 10 9-7 9 7M5 9v12h14V9M10 21v-8h4v8"/>',
    zone: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>'
  };
  const icon = (name, className) => {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    for (const [key, value] of Object.entries({viewBox: '0 0 24 24', fill: 'none',
      stroke: 'currentColor', 'stroke-width': '1.6', 'stroke-linecap': 'round',
      'stroke-linejoin': 'round', 'aria-hidden': 'true', class: className})) svg.setAttribute(key, value);
    svg.innerHTML = icons[name] || icons.button;
    return svg;
  };
  // User actions: the server only announces them as room.input on its event stream.
  const userActions = Array.from({length: 10}, (_, i) => 'action' + (i + 1));
  // A gesture's or an action's hint, by key: inputs.gestureHint.click, inputs.actionHint.play.
  const actionHint = value => /^action\d+$/.test(value) ? tt('inputs.userActionHint') : tv('inputs.actionHint', value, '');
  const normalize = binding => {
    const result = {action: binding.action};
    if (binding.action === 'volumeUp' || binding.action === 'volumeDown') result.step = binding.step ?? .05;
    if (binding.action === 'setVolume') result.value = binding.value;
    if (['volumeUp', 'volumeDown', 'setVolume', 'toggleMute'].includes(binding.action)) result.scope = binding.scope || 'room';
    return result;
  };
  const changed = () => container.dispatchEvent(new Event('bindingschange', {bubbles: true}));
  const content = [];
  if (data.backend) content.push(text('p', tt('inputs.inputBackend', {name: data.backend}), 'note'));
  if (data.backendError) content.push(text('p', data.backendError, 'message bad'));
  if (data.setupButtonPin >= 0) content.push(text('p',
    tt('inputs.setupButtonGpio', {pin: data.setupButtonPin}), 'note'));
  if (!data.inputs.length) content.push(text('p',
    tt('inputs.noAudioButtons'), 'note'));
  for (const input of data.inputs) {
    const item = document.createElement('article');
    item.className = 'input-info';
    item.dataset.inputId = input.id;
    const details = document.createElement('details'); details.dataset.inputId = input.id;
    details.open = expanded.has(input.id);
    const toggle = document.createElement('summary'); toggle.className = 'input-toggle';
    const header = document.createElement('div');
    header.className = 'input-heading';
    const badge = text('span', '', 'input-badge'); badge.append(icon(input.kind, ''));
    badge.setAttribute('role', 'img');
    const pressed = container.inputStates?.get(input.id);
    badge.classList.toggle('pressed', pressed === true);
    badge.setAttribute('aria-label', tt(pressed === true ? 'inputs.pressed' :
      pressed === false ? 'inputs.released' : 'inputs.stateUnavailable', {id: input.id}));
    const title = text('div', '', 'input-title');
    title.append(text('h3', input.id + ' \u00b7 ' + friendly(input.kind)),
      text('p', tt(input.kind === 'encoder' ? 'inputs.rotaryEncoder' : 'inputs.hardwareButton'), 'note'));
    header.append(badge, title);
    const wiring = input.device
      ? input.device + ' \u00b7 ' + (input.key || input.axis)
      : input.kind === 'button' ? 'GPIO ' + input.pin
      : 'GPIO A: ' + input.pinA + ' \u00b7 GPIO B: ' + input.pinB;
    header.append(text('p', wiring, 'note input-wiring'));
    const overview = text('p', '', 'input-binding-summary');
    toggle.append(header, overview); details.append(toggle); item.append(details);
    const bindings = document.createElement('dl');
    const editors = [];
    function summarize() {
      const current = editors.length ? editors.map(({gesture, action, amount, scope}) =>
        [gesture, {action: action.value,
          ...(['volumeUp', 'volumeDown'].includes(action.value) ? {step: Number(amount.value) / 100} : {}),
          ...(action.value === 'setVolume' ? {value: Number(amount.value) / 100} : {}),
          ...(['volumeUp', 'volumeDown', 'setVolume', 'toggleMute'].includes(action.value) ? {scope: scope.value} : {})}])
        : Object.entries(input.bindings);
      const assigned = current.filter(([, binding]) => binding.action !== 'none').map(([gesture, binding]) => {
        let description = friendly(gesture) + ': ' + friendly(binding.action);
        if (binding.step !== undefined) description += ' ' + percent(binding.step);
        if (binding.value !== undefined) description += ' ' + percent(binding.value);
        if (binding.scope) description += ' (' + tv('inputs.scopes', binding.scope) + ')';
        return description;
      });
      overview.textContent = assigned.join(' \u00b7 ') || tt('inputs.noActionsAssigned');
    }
    for (const [gesture, binding] of Object.entries(input.bindings)) {
      if (saveBindings) {
        const label = text('label', tt('inputs.action'));
        const controls = document.createElement('dd');
        controls.className = 'binding-controls';
        const action = document.createElement('select');
        action.setAttribute('aria-label', tt('inputs.gestureAction', {id: input.id, gesture: friendly(gesture)}));
        for (const value of ['none', 'play', 'pause', 'togglePause', 'stop', 'next', 'previous',
          'volumeUp', 'volumeDown', 'setVolume', 'toggleMute', 'standbyOn', 'standbyOff', 'toggleStandby', 'deepSleep', ...userActions])
          // Deep sleep only where the client can do it, or to show a binding it already has.
          if (value !== 'deepSleep' || data.deepSleep || binding.action === 'deepSleep') action.add(new Option(value === 'none' ? tt('inputs.unassigned') : friendly(value), value));
        action.value = binding.action;
        label.htmlFor = 'binding-' + container.id + '-' + editors.length + '-' + content.length;
        action.id = label.htmlFor;
        const amountLabel = text('label', '', 'binding-amount');
        const amountTitle = text('span', tt('inputs.amount')); amountLabel.append(amountTitle);
        const amount = document.createElement('input');
        amount.type = 'number'; amount.max = '100'; amount.step = 'any'; amount.required = true;
        amount.value = Number(((binding.step ?? binding.value ?? .05) * 100).toFixed(6));
        const unit = text('span', '', 'binding-unit'); unit.append(amount, text('span', '%'));
        amountLabel.append(unit);
        const scopeLabel = text('label', tt('inputs.scope'), 'binding-scope');
        const scope = document.createElement('select');
        scope.add(new Option(tt('inputs.room'), 'room')); scope.add(new Option(tt('inputs.zone'), 'zone'));
        scope.value = binding.scope || 'room';
        const scopeControl = text('span', '', 'binding-select');
        scopeControl.append(icon(scope.value, 'binding-select-icon'), scope); scopeLabel.append(scopeControl);
        scope.addEventListener('change', () => {scopeControl.firstChild.replaceWith(icon(scope.value, 'binding-select-icon')); changed()});
        const actionControl = text('span', '', 'binding-select');
        actionControl.append(icon(action.value, 'binding-select-icon'), action); label.append(actionControl);
        const hint = text('p', '', 'note binding-hint');
        function update() {
          const stepped = action.value === 'volumeUp' || action.value === 'volumeDown';
          amountTitle.textContent = tt('inputs.amount');
          amountLabel.hidden = !stepped && action.value !== 'setVolume';
          amount.disabled = amountLabel.hidden;
          amount.min = stepped ? '0.000001' : '0';
          scopeLabel.hidden = amountLabel.hidden && action.value !== 'toggleMute';
          scope.disabled = scopeLabel.hidden;
          controls.classList.toggle('has-amount', !amountLabel.hidden);
          actionControl.firstChild.replaceWith(icon(action.value, 'binding-select-icon'));
          hint.textContent = actionHint(action.value);
        }
        action.addEventListener('change', () => {update(); summarize(); changed()}); update();
        controls.append(label, amountLabel, scopeLabel, hint);
        const term = document.createElement('dt'); term.className = 'gesture-heading';
        const gestureIcon = text('span', '', 'gesture-icon'); gestureIcon.append(icon(gesture, ''));
        const gestureTitle = text('div', '');
        gestureTitle.append(text('strong', friendly(gesture)), text('small', tv('inputs.gestureHint', gesture, tt('inputs.hardwareEvent'))));
        term.append(gestureIcon, gestureTitle);
        bindings.append(term, controls);
        editors.push({gesture, action, amount, scope});
        continue;
      }
      let action = binding.action === 'none' ? tt('inputs.unassigned') : friendly(binding.action);
      if (binding.step !== undefined) action += ' ' + tt('inputs.by', {amount: percent(binding.step)});
      if (binding.value !== undefined) action += ' ' + tt('inputs.to', {amount: percent(binding.value)});
      if (binding.scope) action += ' (' + tv('inputs.scopes', binding.scope) + ')';
      bindings.append(text('dt', friendly(gesture)), text('dd', action));
    }
    summarize(); details.append(bindings);
    if (saveBindings) {
      const feedback = text('p', '', 'message input-feedback'); feedback.setAttribute('role', 'status');
      details.append(feedback);
      const patch = () => {
        const changes = {};
        for (const {gesture, action, amount, scope} of editors) {
          const binding = {action: action.value};
          if (action.value === 'volumeUp' || action.value === 'volumeDown') binding.step = Number(amount.value) / 100;
          if (action.value === 'setVolume') binding.value = Number(amount.value) / 100;
          if (['volumeUp', 'volumeDown', 'setVolume', 'toggleMute'].includes(action.value)) binding.scope = scope.value;
          if (JSON.stringify(binding) !== JSON.stringify(normalize(input.bindings[gesture]))) changes[gesture] = binding;
        }
        return changes;
      };
      rows.push({input, item, editors, feedback, patch});
      item.addEventListener('input', () => {feedback.textContent = ''; summarize(); changed()});
      item.addEventListener('change', summarize);
    }
    content.push(item);
  }
  container.replaceChildren(...content);
  container.dataset.snapshot = snapshot;
  container.inputEdits = saveBindings ? {
    saving: false,
    get dirty() { return rows.some(row => Object.keys(row.patch()).length > 0); },
    validate() {
      for (const row of rows) for (const {amount} of row.editors)
        if (!amount.disabled && !amount.checkValidity()) {
          row.item.querySelector('details').open = true;
          amount.reportValidity(); return false;
        }
      return true;
    },
    async save() {
      if (this.saving) throw Error(tt('inputs.alreadySavingButton'));
      if (!this.validate()) throw Error(tt('inputs.checkButtonAction'));
      this.saving = true;
      const controls = [...container.querySelectorAll('input, select')];
      const disabled = controls.map(control => control.disabled);
      const drafts = rows.map(row => ({row, patch: row.patch()}));
      controls.forEach(control => control.disabled = true);
      try {
        for (const {row, patch} of drafts) {
          if (!Object.keys(patch).length) continue;
          row.feedback.textContent = '';
          try {
            const result = await saveBindings(row.input.id, patch);
            row.input.bindings = result.bindings;
            container.dataset.snapshot = JSON.stringify(data);
          } catch (error) {
            row.item.querySelector('details').open = true;
            row.feedback.textContent = error.message; row.feedback.className = 'message input-feedback bad';
            throw Error(row.input.id + ': ' + error.message);
          }
        }
      } finally {
        controls.forEach((control, index) => control.disabled = disabled[index]);
        this.saving = false; changed();
      }
    }
  } : null;
  changed();
}

