// Cardputer browser proxy: fetches a page, strips it to simple text blocks.
// Runs on Wasmer Edge (WinterJS), or locally with `node proxy/src/index.js`.
//
// Output (text/plain, one record per line):
//   T<title>  B<final url>  U<link url> (in id order)  then blocks:
//   H h1 | h h2-h6 | P para | L list item | Q quote | C code | R rule | E error
// Inline links are \x01<2 id chars>text\x02, id = (c1-48)*64 + (c2-48).

const KEY = '';                 // optional shared secret; set the same in device Settings
const MAX_BYTES = 600000, MAX_LINKS = 1500, MAX_BLOCKS = 3000;
const UA = 'Mozilla/5.0 (X11; Linux x86_64) CardputerProxy/1.0';

const ENT = { amp:'&', lt:'<', gt:'>', quot:'"', apos:"'", nbsp:' ', ndash:'-', mdash:'-', hellip:'...',
  rsquo:"'", lsquo:"'", ldquo:'"', rdquo:'"', bull:'*', middot:'*', copy:'(c)', reg:'(R)', trade:'(TM)',
  raquo:'>>', laquo:'<<', times:'x', deg:'deg', euro:'EUR', pound:'GBP' };

function decode(s) {
  return s.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (m, e) => {
    if (e[0] === '#') {
      const n = e[1].toLowerCase() === 'x' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      try { return String.fromCodePoint(n); } catch { return ''; }
    }
    return ENT[e.toLowerCase()] ?? m;
  });
}
function ascii(s) {
  return s.replace(/\r/g, '').replace(/\t/g, '  ')
    .replace(/[\u2018\u2019\u2032]/g, "'").replace(/[\u201c\u201d]/g, '"')
    .replace(/[\u2010-\u2015\u2212]/g, '-').replace(/\u2026/g, '...')
    .replace(/[\u2022\u00b7\u25cf]/g, '*').replace(/[\u00a0\u2002\u2003\u2009\u202f\u3000]/g, ' ')
    .replace(/[\u00ad\u200b-\u200d\ufeff]/g, '')
    .normalize('NFD').replace(/[\u0300-\u036f]/g, '')
    .replace(/[^\x20-\x7e\n]/g, '?');
}
const attr = (a, n) => {
  const m = a.match(new RegExp('(?:^|\\s)' + n + '\\s*=\\s*(?:"([^"]*)"|\'([^\']*)\'|([^\\s>]+))', 'i'));
  return m ? (m[1] ?? m[2] ?? m[3] ?? '') : '';
};
const abs = (h, b) => {
  if (!h) return '';
  h = decode(h).trim();
  if (/^(javascript|mailto|tel|data|sms):/i.test(h)) return '';
  try {
    const u = new URL(h, b);
    if (!/^https?:$/.test(u.protocol)) return '';
    u.hash = '';
    return u.href === b ? '' : u.href;
  } catch { return ''; }
};

const BLOCK = new Set(('p div section article header footer nav main aside form ul ol dl table thead tbody tfoot ' +
  'tr dd dt figure figcaption center fieldset details summary address caption').split(' '));

function render(html, base, note) {
  const tm = html.match(/<title[^>]*>([\s\S]*?)<\/title>/i);
  const title = tm ? ascii(decode(tm[1])).replace(/\s+/g, ' ').trim().slice(0, 90) : '';
  html = html.replace(/<!--[\s\S]*?-->/g, '')
    .replace(/<head\b[^>]*>[\s\S]*?<\/head>/i, '')
    .replace(/<(script|style|noscript|svg|template|iframe|select|object|canvas)\b[\s\S]*?<\/\1\s*>/gi, '');

  const out = [], links = [];
  let cur = '', kind = 'P', pre = false, open = -1;
  const mk = id => '\x01' + String.fromCharCode(48 + (id >> 6), 48 + (id & 63));
  const vis = s => s.replace(/\x01[\x30-\x6f]{2}|\x02/g, '').trim();
  const flush = () => {
    if (vis(cur)) out.push(kind + (kind === 'C' ? cur.replace(/\s+$/, '') : cur.replace(/\s+/g, ' ').trim()));
    cur = open >= 0 ? mk(open) : '';
  };
  const closeLink = () => { if (open >= 0) { cur += '\x02'; open = -1; } };

  for (const part of html.split(/(<[^>]*>)/)) {
    if (!part) continue;
    if (part[0] !== '<') {
      let t = ascii(decode(part));
      if (pre) t.split('\n').forEach((l, i) => { if (i) flush(); cur += l; });
      else {
        t = t.replace(/\s+/g, ' ');
        if (!cur || cur.endsWith(' ')) t = t.replace(/^ /, '');
        cur += t;
      }
      continue;
    }
    const m = part.match(/^<\s*(\/?)\s*([a-zA-Z][a-zA-Z0-9]*)([^>]*)>$/);
    if (!m) continue;
    const closing = !!m[1], name = m[2].toLowerCase(), a = m[3];
    if (name === 'br') flush();
    else if (name === 'hr') { flush(); out.push('R'); }
    else if (name === 'a') {
      closeLink();
      if (!closing) {
        const u = abs(attr(a, 'href'), base);
        if (u && links.length < MAX_LINKS) { links.push(u); open = links.length - 1; cur += mk(open); }
      }
    } else if (name === 'img') {
      const alt = ascii(decode(attr(a, 'alt'))).trim().slice(0, 40);
      if (alt && !closing) cur += (cur && !cur.endsWith(' ') ? ' ' : '') + '[' + alt + '] ';
    } else if (/^h[1-6]$/.test(name)) { flush(); kind = closing ? 'P' : (name === 'h1' ? 'H' : 'h'); }
    else if (name === 'li') { flush(); kind = closing ? 'P' : 'L'; }
    else if (name === 'blockquote') { flush(); kind = closing ? 'P' : 'Q'; }
    else if (name === 'pre') { flush(); pre = !closing; kind = closing ? 'P' : 'C'; }
    else if (name === 'td' || name === 'th') { if (!closing && vis(cur)) cur += (cur.endsWith(' ') ? '' : ' ') + '| '; }
    else if (BLOCK.has(name)) { flush(); if (['ul', 'ol', 'dl', 'table'].includes(name)) kind = 'P'; }
  }
  flush();
  if (note) out.unshift('E' + note);
  return ['T' + title, 'B' + base, ...links.map(u => 'U' + u), ...out.slice(0, MAX_BLOCKS)].join('\n');
}

function blocked(u) {
  const h = u.hostname.toLowerCase();
  return h === 'localhost' || h.endsWith('.local') || h.endsWith('.internal') ||
    /^(127|10|0)\./.test(h) || /^192\.168\./.test(h) || /^172\.(1[6-9]|2\d|3[01])\./.test(h) ||
    /^169\.254\./.test(h) || h === '[::1]' || h.startsWith('[fd') || h.startsWith('[fe80');
}

const reply = body => new Response(body, { headers: {
  'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store', 'access-control-allow-origin': '*' } });
const err = msg => reply('TError\nE' + msg);

async function handle(req) {
  const url = new URL(req.url);
  if (url.pathname !== '/r') return reply('Cardputer proxy OK.\nUse /r?u=https://example.com\n');
  if (KEY && url.searchParams.get('k') !== KEY) return err('Wrong proxy key. Set it in Settings on the device.');
  let target;
  try { target = new URL(url.searchParams.get('u')); } catch { return err('Bad URL'); }
  if (!/^https?:$/.test(target.protocol) || blocked(target)) return err('URL not allowed');
  try {
    const ctl = new AbortController();
    const to = setTimeout(() => ctl.abort(), 15000);
    const r = await fetch(target.href, { redirect: 'follow', signal: ctl.signal,
      headers: { 'User-Agent': UA, 'Accept': 'text/html,text/plain;q=0.9,*/*;q=0.5', 'Accept-Language': 'en' } });
    clearTimeout(to);
    const ct = (r.headers.get('content-type') || '').toLowerCase();
    if (ct && !/(text|xml|json)/.test(ct)) return err('Unsupported content type: ' + ct.slice(0, 40));
    let body = (await r.text()).slice(0, MAX_BYTES);
    if (ct.includes('text/plain') || ct.includes('json'))
      body = '<pre>' + body.replace(/&/g, '&amp;').replace(/</g, '&lt;') + '</pre>';
    return reply(render(body, r.url || target.href, r.ok ? '' : 'HTTP ' + r.status));
  } catch (e) { return err('Fetch failed: ' + String(e && e.message || e).slice(0, 80)); }
}

if (typeof addEventListener === 'function') {
  addEventListener('fetch', e => e.respondWith(handle(e.request)));
} else if (typeof require === 'function') {
  if (typeof module !== 'undefined') module.exports = { render, handle };
  if (require.main === module) {
    require('http').createServer(async (q, s) => {
      const r = await handle(new Request('http://localhost' + q.url));
      s.writeHead(r.status, Object.fromEntries(r.headers)); s.end(await r.text());
    }).listen(8787, () => console.log('proxy on http://localhost:8787'));
  }
}
