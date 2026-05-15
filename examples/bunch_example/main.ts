const BunchModule: PositronBunchModule = {
  name: 'bunch_example',

  onLoad: function(api: PositronBunchApi) {
    api.log('bunch_example loaded! VFS files: ' + api.vfs.list().join(', '));

    // Inject CSS from VFS into renderer
    api.dom.injectCSSFile('data/style.css').then(function(id: string) {
      api.log('injected css: ' + id);
    });

    // Inject HTML overlay from VFS
    api.dom.append('body', 'div', '', { id: '__bunch_demo' }).then(function() {
      return api.dom.injectHTMLFile('#__bunch_demo', 'afterbegin', 'data/overlay.html');
    }).then(function() {
      api.log('overlay injected');
    });

    // Execute a bundled JS file in the renderer
    api.vfs.execRenderer('renderer_script', 0).then(function(r: any) {
      api.log('renderer_script returned: ' + r);
    });

    // Auto-cleanup timer: update uptime every second
    var start = Date.now();
    api.timer.interval(function() {
      var s = Math.floor((Date.now() - start) / 1000);
      api.dom.setText('#__bunch_uptime', s + 's');
    }, 1000);

    // Read from renderer localStorage
    api.store.local.keys().then(function(keys: string[]) {
      api.log('localStorage has ' + keys.length + ' keys');
    });
  },

  onUnload: function(api: PositronBunchApi) {
    api.dom.remove('#__bunch_demo');
    api.log('bunch_example unloaded');
  }
};
