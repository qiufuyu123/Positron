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
    width: 900, height: 700,
    webPreferences: {
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    }
  });
  w.loadFile(path.join(__dirname, 'index.html'));

  w.webContents.on('did-finish-load', () => {
    w.webContents.executeJavaScript(`
      document.getElementById('info-pid').textContent = '${process.pid}';
      document.getElementById('info-electron').textContent = '${process.versions.electron}';
      document.getElementById('info-node').textContent = '${process.versions.node}';
    `);
  });
});
