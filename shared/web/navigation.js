// Native anchors do the scrolling; this only tracks visibility and location.
(() => {
  const menu = document.querySelector('.section-menu');
  if (!menu) return;
  const header = document.querySelector('.topbar');
  const measure = () => {
    if (header) document.documentElement.style.setProperty('--header-height', header.getBoundingClientRect().height + 'px');
  };
  if (header) new ResizeObserver(() => { measure(); schedule(); }).observe(header);
  // Phones collapse the links behind a toggle that names the current section.
  const list = menu.querySelector('.section-menu-links');
  list.id ||= 'section-menu-links';
  const toggle = document.createElement('button');
  toggle.type = 'button';
  toggle.className = 'section-menu-toggle';
  toggle.setAttribute('aria-controls', list.id);
  toggle.setAttribute('aria-expanded', 'false');
  toggle.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" aria-hidden="true"><path class="menu-open" d="M4 6h16M4 12h16M4 18h16"/><path class="menu-close" d="M6 6l12 12M18 6L6 18"/></svg><span></span>';
  const toggleLabel = toggle.querySelector('span');
  menu.prepend(toggle);
  const setOpen = open => {
    menu.classList.toggle('open', open);
    toggle.setAttribute('aria-expanded', String(open));
  };
  toggle.addEventListener('click', () => setOpen(!menu.classList.contains('open')));
  list.addEventListener('click', event => {
    if (!event.target.closest('a') || !menu.classList.contains('open')) return;
    setOpen(false);
    // The anchor scroll follows this handler; give it the collapsed header height.
    measure();
  });
  document.addEventListener('keydown', event => {
    if (event.key === 'Escape' && menu.classList.contains('open')) { setOpen(false); toggle.focus(); }
  });
  document.addEventListener('click', event => { if (!menu.contains(event.target)) setOpen(false); });
  const groups = [...menu.querySelectorAll('a')].map(link => {
    const heading = document.getElementById(link.hash.slice(1));
    const section = heading.closest('.settings-card');
    const icon = section.querySelector('.card-icon svg');
    if (icon) link.prepend(icon.cloneNode(true));
    return {link, section};
  });
  let current, frame;
  function update() {
    frame = null;
    const visible = groups.filter(group => {
      group.link.hidden = group.section.classList.contains('hidden');
      return !group.link.hidden;
    });
    let active = visible[0];
    const edge = menu.getBoundingClientRect().bottom + 24;
    for (const group of visible) {
      if (group.section.getBoundingClientRect().top <= edge) active = group;
    }
    if (window.scrollY > 0 && Math.ceil(window.scrollY + window.innerHeight) >= document.documentElement.scrollHeight - 2)
      active = visible[visible.length - 1];
    if (document.documentElement.scrollHeight <= window.innerHeight)
      active = visible.find(group => group.link.hash === window.location.hash) || active;
    // Also refresh after a language switch renames the links.
    if (active) toggleLabel.textContent = active.link.textContent;
    if (!active || active === current) return;
    current = active;
    for (const group of groups) {
      if (group === active) group.link.setAttribute('aria-current', 'location');
      else group.link.removeAttribute('aria-current');
    }
  }
  function schedule() { if (!frame) frame = requestAnimationFrame(update); }
  window.addEventListener('scroll', schedule, {passive: true});
  window.addEventListener('resize', schedule);
  window.addEventListener('hashchange', schedule);
  const observer = new MutationObserver(schedule);
  for (const group of groups) observer.observe(group.section, {attributes: true, attributeFilter: ['class']});
  observer.observe(list, {subtree: true, childList: true, characterData: true});
  update();
})();
