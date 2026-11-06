// Shared helpers for the BC-250 pages. PIN (if set on the Pico) is asked once and kept in this browser.
function pin() { try { return localStorage.getItem('pin') || ''; } catch (e) { return ''; } }
function setPin(p) { try { p ? localStorage.setItem('pin', p) : localStorage.removeItem('pin'); } catch (e) {} }

// fetch with the PIN header; on 403 ask for the PIN once and retry
async function api(path, opts) {
  opts = opts || {};
  for (let k = 0; k < 2; k++) {
    const h = Object.assign({ 'X-PIN': pin() }, opts.headers || {});
    const r = await fetch(path, Object.assign({}, opts, { headers: h }));
    if (r.status === 403 && k === 0) { setPin(prompt('PIN') || ''); continue; }
    return r;
  }
}
function post(path, form) {
  const o = { method: 'POST' };
  if (form) {
    o.headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
    o.body = new URLSearchParams(form).toString();
  }
  return api(path, o);
}
function esc(s) { return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
function $(id) { return document.getElementById(id); }
