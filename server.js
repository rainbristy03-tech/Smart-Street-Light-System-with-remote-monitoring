const express = require('express');
const { DatabaseSync } = require('node:sqlite'); // Node-এর নিজস্ব SQLite, আলাদা ইনস্টল লাগে না
const path = require('path');

const PORT = process.env.PORT || 5000;                 // ESP32 কোডের সাথে মিলিয়ে 5000
const SIMULATE = process.env.SIMULATE === 'true';      // আসল ESP32 থাকলে ডিফল্ট false; ডেমো চাইলে SIMULATE=true
const OFFLINE_AFTER_SEC = 15;                          // এত সেকেন্ড ডেটা না এলে Offline

const db = new DatabaseSync(path.join(__dirname, 'smartlight.db'));
db.exec(`
CREATE TABLE IF NOT EXISTS poles(
  id TEXT PRIMARY KEY, location TEXT, status TEXT DEFAULT 'working',
  is_on INTEGER DEFAULT 1, brightness INTEGER DEFAULT 80,
  motion INTEGER DEFAULT 0, obstacle INTEGER DEFAULT 0, voltage REAL DEFAULT 230,
  x INTEGER, y INTEGER, updated_at TEXT DEFAULT CURRENT_TIMESTAMP);
CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT);
`);
if (db.prepare('SELECT COUNT(*) c FROM poles').get().c === 0) {
  // ৪টা পোল, ESP32 এর সাথে মেলে। ESP32 ডেটা না পাঠানো পর্যন্ত offline থাকবে
  const places = ['Main Rd', 'Station Rd', 'Lake Side', 'Park Ave'];
  const ins = db.prepare("INSERT INTO poles(id,location,status,is_on,brightness,x,y) VALUES (?,?,'offline',1,0,?,?)");
  places.forEach((p, i) => ins.run('Pole-' + String(i + 1).padStart(2, '0'), p, 60 + i * 160, 90));
  db.prepare("INSERT INTO settings VALUES('mode','auto'),('manual_brightness','80')").run();
}
const getSet = k => db.prepare('SELECT value FROM settings WHERE key=?').get(k).value;
const setSet = (k, v) => db.prepare('INSERT OR REPLACE INTO settings VALUES(?,?)').run(k, String(v));
const poleName = n => 'Pole-' + String(n).padStart(2, '0');

const app = express();
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// সব পোল + mode
app.get('/api/poles', (req, res) => {
  res.json({ mode: getSet('mode'), manualBrightness: +getSet('manual_brightness'),
    poles: db.prepare('SELECT * FROM poles ORDER BY id').all() });
});

app.get('/api/stats', (req, res) => {
  const c = s => db.prepare('SELECT COUNT(*) c FROM poles WHERE status=?').get(s).c;
  res.json({ total: db.prepare('SELECT COUNT(*) c FROM poles').get().c,
    working: c('working'), offline: c('offline'), fault: c('fault') });
});

app.get('/api/alerts', (req, res) => {
  const out = [];
  db.prepare('SELECT * FROM poles').all().forEach(p => {
    if (p.status === 'fault') out.push({ pole: p.id, message: 'Light Fault Detected', level: 'red' });
    if (p.status === 'offline') out.push({ pole: p.id, message: 'Offline', level: 'yellow' });
    if (p.voltage < 200) out.push({ pole: p.id, message: 'Voltage Low', level: 'orange' });
  });
  res.json(out);
});

// একটা পোল ON/OFF
app.post('/api/poles/:id/toggle', (req, res) => {
  const p = db.prepare('SELECT * FROM poles WHERE id=?').get(req.params.id);
  if (!p) return res.status(404).json({ error: 'not found' });
  if (p.status !== 'working') return res.status(400).json({ error: 'pole not working' });
  const on = p.is_on ? 0 : 1;
  const br = on ? (getSet('mode') === 'manual' ? +getSet('manual_brightness') : 80) : 0;
  db.prepare('UPDATE poles SET is_on=?, brightness=? WHERE id=?').run(on, br, p.id);
  res.json({ ok: true });
});

// কন্ট্রোল প্যানেল: {action:'on'|'off'|'mode'|'brightness', value}
app.post('/api/control', (req, res) => {
  const { action, value } = req.body;
  if (action === 'on' || action === 'off') {
    const on = action === 'on' ? 1 : 0;
    const br = on ? (getSet('mode') === 'manual' ? +getSet('manual_brightness') : 80) : 0;
    db.prepare("UPDATE poles SET is_on=?, brightness=? WHERE status='working'").run(on, br);
  } else if (action === 'mode' && ['auto', 'manual'].includes(value)) {
    setSet('mode', value);
    if (value === 'auto') db.prepare('UPDATE poles SET is_on=1').run(); // Auto চাপলে সব পোল আবার auto তে ফিরবে
    if (value === 'manual') db.prepare("UPDATE poles SET brightness=? WHERE status='working' AND is_on=1").run(+getSet('manual_brightness'));
  } else if (action === 'brightness') {
    const v = Math.max(0, Math.min(100, +value));
    setSet('mode', 'manual'); setSet('manual_brightness', v);
    db.prepare("UPDATE poles SET brightness=? WHERE status='working' AND is_on=1").run(v);
  } else return res.status(400).json({ error: 'bad request' });
  res.json({ ok: true });
});

// ==========================================
// ESP32 (4 পোল) এই endpoint এ ডেটা পাঠায় এবং কমান্ড ফেরত পায়
// body: {id, ldr, motion, obstacle, rssi, poles:[{id, brightness, status, mode, fault}]}
// ==========================================
app.post('/api/hardware/update', (req, res) => {
  const { motion = false, obstacle = false, poles: reported = [] } = req.body;

  const upd = db.prepare(
    "UPDATE poles SET motion=?,obstacle=?,brightness=?,status=?,updated_at=CURRENT_TIMESTAMP WHERE id=?");
  reported.forEach(r => {
    upd.run(motion ? 1 : 0, obstacle ? 1 : 0, Math.round(+r.brightness || 0),
      r.fault ? 'fault' : 'working', poleName(r.id));
  });

  // কমান্ড: Auto mode এ is_on=1 পোল ESP32 নিজে dim/bright করবে।
  // OFF করা পোল (is_on=0) বা Manual mode এ সার্ভারের ব্রাইটনেস মানবে
  const mode = getSet('mode'), mb = +getSet('manual_brightness');
  const lights = db.prepare('SELECT * FROM poles ORDER BY id').all().map(p => ({
    id: parseInt(p.id.replace('Pole-', ''), 10),
    status: p.is_on ? 'ON' : 'OFF',
    brightness: p.is_on ? mb : 0,
    mode: (mode === 'auto' && p.is_on) ? 'auto' : 'manual'
  }));
  res.json({ ok: true, mode, lights });
});

// পুরনো endpoint (আগের মতোই থাকল)
app.post('/api/telemetry', (req, res) => {
  const { id, motion = 0, obstacle = 0, voltage = 230, status = 'working' } = req.body;
  const p = db.prepare('SELECT * FROM poles WHERE id=?').get(id);
  if (!p) return res.status(404).json({ error: 'unknown pole' });
  let br = p.brightness;
  if (getSet('mode') === 'auto' && p.is_on && status === 'working') br = motion ? 100 : 30;
  db.prepare("UPDATE poles SET motion=?,obstacle=?,voltage=?,status=?,brightness=?,updated_at=CURRENT_TIMESTAMP WHERE id=?")
    .run(motion ? 1 : 0, obstacle ? 1 : 0, voltage, status, br, id);
  res.json({ ok: true, brightness: br, is_on: p.is_on });
});

// Offline watchdog: ১৫ সেকেন্ড ডেটা না এলে পোল Offline
if (!SIMULATE) setInterval(() => {
  db.prepare(`UPDATE poles SET status='offline', motion=0, obstacle=0
    WHERE status!='offline' AND (julianday('now') - julianday(updated_at)) * 86400 > ?`).run(OFFLINE_AFTER_SEC);
}, 5000);

// ডেমো সিমুলেশন (SIMULATE=true দিলে)
if (SIMULATE) setInterval(() => {
  const auto = getSet('mode') === 'auto';
  db.prepare("SELECT * FROM poles WHERE status='working'").all().forEach(p => {
    const motion = Math.random() < 0.3 ? 1 : 0, obs = Math.random() < 0.1 ? 1 : 0;
    const br = auto && p.is_on ? (motion ? 100 : 30) : p.brightness;
    db.prepare('UPDATE poles SET motion=?,obstacle=?,brightness=? WHERE id=?').run(motion, obs, br, p.id);
  });
}, 3000);

app.listen(PORT, '0.0.0.0', () => console.log('SmartLight running: http://localhost:' + PORT));