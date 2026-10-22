// Native anchors do the scrolling; this only tracks visibility and location.
(() => {
  const menu = document.querySelector('.section-menu');
  if (!menu) return;
  const header = document.querySelector('.topbar');
  if (header) new ResizeObserver(() => {
    document.documentElement.style.setProperty('--header-height', header.getBoundingClientRect().height + 'px');
    schedule();
  }).observe(header);
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
  update();
})();
