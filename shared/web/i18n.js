// The pages' languages: en-US and de-AT. Every text has a dotted key
// (area.name), and the dictionaries (*.en.js, *.de.js) give each key its text.
// tt(key, args) answers the key's text in the chosen language, {name} filled
// from args; a key without a translation falls back to English, and a key
// that English lacks too is shown as it is (a server's own message passes
// through unchanged). tv(group, value, otherwise) is for a value the server
// names, a state or a kind: the text of 'group.value', else `otherwise` (the
// value itself unless given). The choice
// is kept per browser and origin (every device page is its own origin);
// without one, the browser's language picks.
const i18n = (() => {
  const key = 'galdrun.lang';
  const names = {'en-US': 'English', 'de-AT': 'Deutsch'};
  // Inline flags: Windows has no emoji flags.
  const flags = {
    'en-US': '<svg viewBox="0 0 60 30" preserveAspectRatio="xMidYMid slice" aria-hidden="true">'
      + '<clipPath id="flag-uk-t"><path d="M30 15h30v15zv15H0zH0V0zV0h30z"/></clipPath>'
      + '<rect width="60" height="30" fill="#012169"/><path d="M0 0l60 30m0-30L0 30" stroke="#fff" stroke-width="6"/>'
      + '<path d="M0 0l60 30m0-30L0 30" clip-path="url(#flag-uk-t)" stroke="#c8102e" stroke-width="4"/>'
      + '<path d="M30 0v30M0 15h60" stroke="#fff" stroke-width="10"/><path d="M30 0v30M0 15h60" stroke="#c8102e" stroke-width="6"/></svg>',
    'de-AT': '<svg viewBox="0 0 5 3" aria-hidden="true"><rect width="5" height="1" fill="#000"/>'
      + '<rect y="1" width="5" height="1" fill="#d00"/><rect y="2" width="5" height="1" fill="#ffce00"/></svg>',
  };
  const texts = {};
  let lang = null;
  try { lang = localStorage.getItem(key); } catch (_) { /* per-viewer convenience only */ }
  if (!names[lang]) lang = /^de\b/i.test(navigator.language || '') ? 'de-AT' : 'en-US';
  document.documentElement.lang = lang;
  const text = (id) => texts[lang]?.[id] ?? texts['en-US']?.[id];
  const t = (id, args) => {
    const s = text(id) ?? id;
    return args ? s.replace(/\{(\w+)\}/g, (m, k) => (k in args ? String(args[k]) : m)) : s;
  };
  return {
    lang,
    t,
    value: (group, v, otherwise = v ?? '') => (v == null || v === '' ? otherwise : text(`${group}.${v}`) ?? String(otherwise)),
    // A language's texts, by key.
    add(language, entries) { Object.assign(texts[language] ??= {}, entries); },
    // The page's own markup, before any data is put in: data-i18n names the
    // key of an element's text, data-i18n-html of its markup (the
    // dictionaries' own <em>, <code> and <a>), and data-i18n-title,
    // -aria-label and -placeholder of those attributes.
    translate(root = document) {
      for (const el of root.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
      for (const el of root.querySelectorAll('[data-i18n-html]')) el.innerHTML = t(el.dataset.i18nHtml);
      for (const name of ['title', 'aria-label', 'placeholder']) {
        for (const el of root.querySelectorAll(`[data-i18n-${name}]`)) el.setAttribute(name, t(el.getAttribute(`data-i18n-${name}`)));
      }
    },
    // Fills a container with a flag button per language; a choice is kept and
    // the page reloads in it.
    bind(box) {
      box.setAttribute('role', 'group');
      box.replaceChildren(...Object.entries(names).map(([value, name]) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.lang = value;
        b.title = name;
        b.setAttribute('aria-label', name);
        b.setAttribute('aria-pressed', String(value === lang));
        b.innerHTML = flags[value];
        b.onclick = () => {
          if (value === lang) return;
          try { localStorage.setItem(key, value); } catch (_) { /* per-viewer convenience only */ }
          location.reload();
        };
        return b;
      }));
    },
  };
})();
const tt = i18n.t, tv = i18n.value;
