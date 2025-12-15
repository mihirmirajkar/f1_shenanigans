const express = require('express');
const app = express();

app.use(express.urlencoded({ extended: true }));
app.use(express.static(__dirname));

const MIN_LAP_MS = 0; // rely on client gating

const state = {
  maxCars: 6,
  count: 2,
  cars: [
    { id: 'car1', label: 'Car 1', color: '#ff4d4f', lastLapMs: 0, laps: [] },
    { id: 'car2', label: 'Car 2', color: '#4dabff', lastLapMs: 0, laps: [] },
  ],
};

function format(ms) {
  const totalSeconds = Math.floor(ms / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  const millis = ms % 1000;
  return `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}.${String(millis).padStart(3, '0')}`;
}

app.get('/car_config', (req, res) => {
  res.json(state);
});

app.post('/car_config', (req, res) => {
  const newCount = Math.max(1, Math.min(state.maxCars, parseInt(req.body.count || state.count, 10)));
  const namesCsv = req.body.names || '';
  const colorsCsv = req.body.colors || '';
  const names = namesCsv.split(',').map(s => decodeURIComponent(s || '').trim()).filter(Boolean);
  const colors = colorsCsv.split(',').map(s => decodeURIComponent(s || '').trim()).filter(Boolean);

  while (state.cars.length < newCount) {
    const n = state.cars.length + 1;
    state.cars.push({ id: `car${n}`, label: `Car ${n}`, color: '#ff4d4f', lastLapMs: 0, laps: [] });
  }
  while (state.cars.length > newCount) state.cars.pop();

  state.cars.forEach((c, i) => {
    if (i < names.length) c.label = names[i];
    if (i < colors.length) c.color = colors[i];
    c.lastLapMs = 0;
    c.lastLapMsFormatted = '';
    c.laps = [];
  });
  state.count = newCount;
  res.send('OK');
});

app.get('/car_status', (req, res) => {
  const cars = state.cars.map((c) => ({
    ...c,
    lastLapMsFormatted: c.lastLapMs > 0 ? format(c.lastLapMs) : '',
    laps: (c.laps || []).map((lap) => {
      if (lap && typeof lap === 'object') {
        const ms = typeof lap.ms === 'number' ? lap.ms : 0;
        return { ms, formatted: lap.formatted || (ms ? format(ms) : '') };
      }
      const ms = typeof lap === 'number' ? lap : 0;
      return { ms, formatted: ms ? format(ms) : '' };
    }),
  }));

  res.json({ ...state, cars });
});

app.get('/lap_ping', (req, res) => {
  const carId = String(req.query.car || '').toLowerCase();
  const car = state.cars.find(c => c.id.toLowerCase() === carId);
  if (!car) return res.status(404).send('Unknown car');
  const lap = Math.floor(Math.random() * 4000) + 1500; // fake lap
  car.lastLapMs = lap;
  car.lastLapMsFormatted = format(lap);
  car.laps = car.laps || [];
  car.laps.unshift({ ms: lap, formatted: format(lap) });
  if (car.laps.length > 10) car.laps.pop();
  res.send('LAP');
});

app.get('/reset', (req, res) => {
  state.cars.forEach(c => {
    c.lastLapMs = 0;
    c.lastLapMsFormatted = '';
    c.laps = [];
  });
  res.send('OK');
});

const port = process.env.PORT || 8000;
app.listen(port, () => {
  console.log(`Open http://localhost:${port}/camera_test.html`);
});
