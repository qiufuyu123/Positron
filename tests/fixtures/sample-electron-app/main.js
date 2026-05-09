const { app, BrowserWindow } = require('electron');
const path = require('path');

try {
  require(path.join(__dirname, '..', 'sample-addon', 'build', 'Release', 'sample_addon.node'));
  console.log('[fixture] sample-addon loaded');
} catch (e) {
  console.log('[fixture] sample-addon not loaded:', e.message);
}

global.__positron_fixture__ = { hello: 'main' };

app.whenReady().then(() => {
  const w = new BrowserWindow({
    width: 800, height: 600,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
    }
  });
  w.loadFile(path.join(__dirname, 'index.html'));
});
