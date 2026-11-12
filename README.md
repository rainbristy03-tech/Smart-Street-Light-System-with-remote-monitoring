# SmartLight – Full Stack Smart Street Light Dashboard

Stack: HTML + CSS + JavaScript (frontend), Node.js + Express (backend), SQLite (database)

## Run করার নিয়ম
1. Node.js (v22.13 বা নতুন, যেমন v24) ইনস্টল করুন: https://nodejs.org
2. এই ফোল্ডারে terminal খুলে:
   npm install
   npm start
3. ব্রাউজারে খুলুন: http://localhost:3000

## ফোল্ডার
- server.js        → backend + API + database (smartlight.db নিজে তৈরি হবে)
- public/index.html → পেজের structure
- public/style.css  → ডিজাইন
- public/app.js     → frontend logic (API call, UI update)

## API
GET  /api/poles | /api/stats | /api/alerts
POST /api/poles/:id/toggle
POST /api/control      {"action":"on|off|mode|brightness","value":...}
POST /api/telemetry    ESP32 থেকে ডেটা নেয়

## ESP32 থেকে ডেটা পাঠানো (SIMULATE=false দিয়ে চালান)
Windows: set SIMULATE=false && npm start
Linux/Mac: SIMULATE=false npm start

Test (curl):
curl -X POST http://localhost:3000/api/telemetry -H "Content-Type: application/json" -d '{"id":"Pole-01","motion":1,"obstacle":0,"voltage":228,"status":"working"}'

ESP32 Arduino কোডে HTTPClient দিয়ে এই URL-এ একই JSON POST করবেন। response-এ "brightness" ফেরত আসবে, সেটা দিয়ে LED চালাবেন।

## পরে বাড়াতে পারেন
Login (JWT), আসল Map (Leaflet), Analytics chart (Chart.js), MQTT, history table।
