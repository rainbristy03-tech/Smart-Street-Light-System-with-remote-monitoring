// Frontend: সার্ভার থেকে ডেটা এনে স্ক্রিনে দেখায়
const COL = { working: '#16a34a', offline: '#eab308', fault: '#dc2626' };
const LV = { red: '#dc2626', yellow: '#eab308', orange: '#f97316' };
const $ = id => document.getElementById(id);
const api = (url, method, body) => fetch(url, method ? { method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body || {}) } : undefined).then(r => r.json());

async function load() {
  const [data, stats, alerts] = await Promise.all([api('/api/poles'), api('/api/stats'), api('/api/alerts')]);
  $('sTotal').textContent = stats.total; $('sWork').textContent = stats.working;
  $('sOff').textContent = stats.offline; $('sFault').textContent = stats.fault;
  $('modeLbl').textContent = '· ' + (data.mode === 'auto' ? 'Auto Mode' : 'Manual Mode');
  $('brv').textContent = data.manualBrightness;
  if (document.activeElement !== $('br')) $('br').value = data.manualBrightness;

  $('rows').innerHTML = data.poles.map(p => `<tr>
    <td><b>${p.id}</b></td><td>${p.location}</td>
    <td><span class="pill" style="background:${COL[p.status]}">${p.status}</span></td>
    <td><span class="bar"><i style="width:${p.brightness}%"></i></span> ${p.brightness}%</td>
    <td>${p.motion ? '🚶 Detected' : '—'}</td><td>${p.obstacle ? '🚧 Yes' : '—'}</td>
    <td><button class="sm" style="background:${p.is_on ? 'var(--green)' : '#64748b'}" onclick="toggle('${p.id}')">${p.is_on ? 'ON' : 'OFF'}</button></td></tr>`).join('');

  $('alerts').innerHTML = alerts.length
    ? alerts.map(a => `<div class="alert" style="border-color:${LV[a.level]}"><b>${a.pole}</b><span style="color:${LV[a.level]}">${a.message}</span></div>`).join('')
    : '<span class="mut">No alerts</span>';

  let svg = '<path d="M0 90H600M0 210H600M120 0V300M440 0V300" stroke="#334155" stroke-width="14" fill="none" opacity=".5"/>';
  data.poles.forEach(p => {
    const lit = p.status === 'working' && p.is_on;
    const c = p.status === 'working' ? (p.is_on ? '#facc15' : '#64748b') : COL[p.status];
    svg += `<circle cx="${p.x}" cy="${p.y}" r="${lit ? 18 : 10}" fill="${c}" opacity=".25"/>
      <circle cx="${p.x}" cy="${p.y}" r="7" fill="${c}"/>
      <text x="${p.x}" y="${p.y - 14}" font-size="11" text-anchor="middle" fill="#cbd5e1">${p.id}</text>`;
  });
  $('map').innerHTML = svg;
}
const toggle = id => api('/api/poles/' + id + '/toggle', 'POST').then(load);
const control = (action, value) => api('/api/control', 'POST', { action, value }).then(load);

load(); setInterval(load, 2000);
