// site.js -- the only script of fauxcloud.org's own pages (the /try/ demo has its own): the theme toggle, the phone
// menu and the docs sidebar, the code copy buttons, the photo popout. No network, no cookies; the theme choice is kept in localStorage
// under fc-theme, as on the device. Dark is the default; there is no follow-the-system state.
(function () {
  var root = document.documentElement;
  var tint = document.querySelector('meta[name="theme-color"]');
  function paint() { if (tint) tint.content = root.dataset.theme === 'light' ? '#6fa6ec' : '#0c1430'; }
  paint();
  var btn = document.getElementById('theme');
  if (btn) btn.onclick = function () {
    var next = root.dataset.theme === 'light' ? 'dark' : 'light';
    root.dataset.theme = next;
    try { localStorage.setItem('fc-theme', next); } catch (e) {}
    paint();
  };

  // the phone menu: a <details> that works without script; this closes it on a link, a click outside or Escape
  var menu = document.querySelector('details.menu');
  if (menu) {
    document.addEventListener('click', function (e) { if (menu.open && (!menu.contains(e.target) || e.target.closest('a'))) menu.open = false; });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && menu.open) { menu.open = false; menu.querySelector('summary').focus(); } });
  }

  // the docs sidebar: a sheet under its summary on a phone (closed), always open from 900 px
  var side = document.querySelector('details.side');
  if (side) {
    var narrow = matchMedia('(max-width: 899px)');
    side.open = !narrow.matches;
    narrow.addEventListener('change', function (m) { side.open = !m.matches; });
  }

  // the current page's sections in the sidebar: mark the one being read
  var subs = document.querySelectorAll('.side .sub a');
  if (subs.length && window.IntersectionObserver) {
    var io = new IntersectionObserver(function (es) {
      es.forEach(function (e) {
        if (!e.isIntersecting) return;
        subs.forEach(function (a) { a.classList.toggle('on', a.getAttribute('href') === '#' + e.target.id); });
        var h3 = document.querySelector('.side .sub ul a.on');  // an h3 row: mark its h2 row too
        if (h3) h3.closest('ul').parentNode.firstElementChild.classList.add('on');
      });
    }, { rootMargin: '-80px 0px -70% 0px' });
    subs.forEach(function (a) { var h = document.getElementById(a.getAttribute('href').slice(1)); if (h) io.observe(h); });
  }

  // code blocks: copy the text, say so for a moment
  document.querySelectorAll('.code button').forEach(function (b) {
    b.onclick = function () {
      var text = b.closest('.code').querySelector('pre').innerText;
      var done = function () { b.textContent = 'Copied'; setTimeout(function () { b.textContent = 'Copy'; }, 1500); };
      if (navigator.clipboard) navigator.clipboard.writeText(text).then(done, function () {});
    };
  });

  // photos and screenshots: the image becomes a button that opens its largest variant in a <dialog> (Escape, a click
  // outside the image or Close shut it; focus goes back to the image); without script they stay plain images
  var pics = document.querySelectorAll('figure.ph img, figure.shot img');
  if (pics.length && window.HTMLDialogElement) {
    var dlg = document.createElement('dialog'), big = new Image(), shut = document.createElement('button'), from;
    dlg.className = 'zoom';
    shut.type = 'button'; shut.className = 'zx'; shut.textContent = 'Close';
    dlg.append(shut, big);
    document.body.append(dlg);
    shut.onclick = function () { dlg.close(); };
    dlg.onclick = function (e) { if (e.target === dlg) dlg.close(); };
    dlg.addEventListener('close', function () { big.removeAttribute('src'); if (from) from.focus(); });
    pics.forEach(function (img) {
      var el = img.parentNode.tagName === 'PICTURE' ? img.parentNode : img, b = document.createElement('button');
      b.type = 'button'; b.className = 'zb'; b.setAttribute('aria-label', 'Enlarge: ' + img.alt);
      el.replaceWith(b); b.append(el);
      b.onclick = function () {
        var best = img.src, w = 0;
        (img.getAttribute('srcset') || '').split(',').forEach(function (c) {
          var p = c.trim().split(/\s+/), n = parseInt(p[1], 10);
          if (n > w) { w = n; best = p[0]; }
        });
        big.src = best; big.alt = img.alt; from = b;
        dlg.setAttribute('aria-label', img.alt);
        dlg.showModal();
      };
    });
  }
})();
