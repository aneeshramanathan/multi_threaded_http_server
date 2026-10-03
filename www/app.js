async function refresh() {
  try {
    const res = await fetch('/api/stats');
    document.getElementById('stats').textContent = JSON.stringify(await res.json(), null, 2);
  } catch (err) {
    document.getElementById('stats').textContent = 'stats unavailable: ' + err;
  }
}
refresh();
setInterval(refresh, 2000);
