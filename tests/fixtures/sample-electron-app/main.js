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
      // sandbox:false so positron's payload (mapped via Blackbone) can open
      // a TCP socket back to the host. With Chromium's renderer sandbox on,
      // socket()/CreateEventW with Local\ namespace are blocked — that path
      // would need a Mojo-aware injector or pre-duplicated handles. v1 ships
      // sandbox-off support; sandbox-on is a future task.
      sandbox: false,
    }
  });
  w.loadFile(path.join(__dirname, 'index.html'));
});
