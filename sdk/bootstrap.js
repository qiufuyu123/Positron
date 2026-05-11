// positron v2 transport bootstrap
//
// Loaded from disk by the SDK on every attach. Resolution order:
//   1. POSITRON_BOOTSTRAP_JS_PATH env (absolute path)
//   2. <host.exe-dir>/bootstrap.js  (msbuild copy step)

(function(){
try {
  if (globalThis.__positron_v2 && globalThis.__positron_v2.status === 'ready') return;

  var req = (function(){
    try { if (typeof require === 'function') return require; } catch (e) {}
    try { if (process.mainModule && process.mainModule.require)
            return process.mainModule.require.bind(process.mainModule); } catch (e) {}
    throw new Error('no require available in main process');
  })();
  var net = req('net');

  var PORT = 60000 + (process.pid % 5000);

  function getElectron() {
    try { return req('electron'); } catch (e) {}
    try { return process._linkedBinding('electron_browser_window'); } catch (e) {}
    return null;
  }

  function makeFramer(socket, onMessage) {
    var buf = Buffer.alloc(0);
    socket.on('data', function (chunk) {
      buf = Buffer.concat([buf, chunk]);
      while (buf.length >= 4) {
        var len = buf.readUInt32LE(0);
        if (buf.length < 4 + len) break;
        var msg;
        try { msg = JSON.parse(buf.slice(4, 4 + len).toString('utf8')); }
        catch (e) { socket.destroy(); return; }
        buf = buf.slice(4 + len);
        try { onMessage(msg); } catch (e) { /* keep loop alive */ }
      }
    });
    function send(obj) {
      try {
        var s = Buffer.from(JSON.stringify(obj), 'utf8');
        var hdr = Buffer.alloc(4);
        hdr.writeUInt32LE(s.length, 0);
        socket.write(hdr); socket.write(s);
      } catch (e) {}
    }
    return { send: send };
  }

  function evalResponseOk(id, value) {
    var json;
    try { json = JSON.stringify(value); }
    catch (e) {
      try { json = JSON.stringify(String(value)); } catch (e2) { json = '"<unserializable>"'; }
    }
    if (json === undefined) json = 'null';
    return { kind: 'eval.response', id: id, ok: true,
             result: { type_tag: typeof value, json: json } };
  }
  function evalResponseErr(id, e) {
    var msg, stk;
    try { msg = (e && e.message) ? String(e.message) : String(e); } catch (_) { msg = 'unknown'; }
    try { stk = (e && e.stack) ? String(e.stack) : ''; } catch (_) { stk = ''; }
    return { kind: 'eval.response', id: id, ok: false,
             error: { message: msg, stack: stk } };
  }

  var indirectEval = (0, eval);

  // =========================================================================
  // Module system
  // =========================================================================
  // Each module is a JS string that, when eval'd, returns an object:
  //   { name: string, onLoad(api): void|Promise, onUnload(): void|Promise }
  //
  // `api` passed to onLoad:
  //   api.eval(code)                -> Promise<any>   (main-process eval)
  //   api.evalRenderer(code, idx)   -> Promise<any>   (renderer hop)
  //   api.send(msg)                 -> void            (send frame to host)
  //   api.getElectron()             -> electron module or null
  //   api.require                   -> the resolved require function
  //   api.log(str)                  -> void            (send log to host)

  var modules = {};      // name -> { exports, api }
  var modFramer = null;  // current framer (set per-connection)

  function makeModuleApi(name) {
    return {
      eval: function(code) {
        return new Promise(function(resolve, reject) {
          try {
            var v = indirectEval(code);
            if (v && typeof v.then === 'function') v.then(resolve, reject);
            else resolve(v);
          } catch (e) { reject(e); }
        });
      },
      evalRenderer: function(code, idx) {
        idx = idx || 0;
        return new Promise(function(resolve, reject) {
          var electron = getElectron();
          if (!electron || !electron.BrowserWindow) { reject(new Error('no BrowserWindow')); return; }
          var wins = electron.BrowserWindow.getAllWindows();
          if (!wins[idx]) { reject(new Error('no window at index ' + idx)); return; }
          wins[idx].webContents.executeJavaScript(code, true).then(resolve, reject);
        });
      },
      send: function(msg) {
        if (modFramer) modFramer.send({ kind: 'mod.event', module: name, data: msg });
      },
      getElectron: getElectron,
      require: req,
      log: function(str) {
        if (modFramer) modFramer.send({ kind: 'log', level: 'info',
                                        message: '[mod:' + name + '] ' + str });
      }
    };
  }

  function handleModLoad(msg, framer) {
    var id   = msg.id;
    var code = msg.code;
    var name = msg.name;
    try {
      if (modules[name]) {
        framer.send(evalResponseErr(id, new Error('module "' + name + '" already loaded; unload first')));
        return;
      }
      var exports = indirectEval(code);
      if (!exports || typeof exports !== 'object' || !exports.name) {
        framer.send(evalResponseErr(id, new Error('module must return {name, onLoad?, onUnload?}')));
        return;
      }
      name = exports.name;
      if (modules[name]) {
        framer.send(evalResponseErr(id, new Error('module "' + name + '" already loaded')));
        return;
      }
      var api = makeModuleApi(name);
      modules[name] = { exports: exports, api: api };

      if (typeof exports.onLoad === 'function') {
        var ret = exports.onLoad(api);
        if (ret && typeof ret.then === 'function') {
          ret.then(function() {
            framer.send(evalResponseOk(id, { loaded: name }));
          }, function(e) {
            delete modules[name];
            framer.send(evalResponseErr(id, e));
          });
          return;
        }
      }
      framer.send(evalResponseOk(id, { loaded: name }));
    } catch (e) {
      if (name && modules[name]) delete modules[name];
      framer.send(evalResponseErr(id, e));
    }
  }

  function handleModUnload(msg, framer) {
    var id   = msg.id;
    var name = msg.name;
    try {
      if (!modules[name]) {
        framer.send(evalResponseErr(id, new Error('module "' + name + '" not loaded')));
        return;
      }
      var m = modules[name];
      var api = m.api || makeModuleApi(name);
      if (m.exports && typeof m.exports.onUnload === 'function') {
        var ret = m.exports.onUnload(api);
        if (ret && typeof ret.then === 'function') {
          ret.then(function() {
            delete modules[name];
            framer.send(evalResponseOk(id, { unloaded: name }));
          }, function(e) {
            delete modules[name];
            framer.send(evalResponseErr(id, e));
          });
          return;
        }
      }
      delete modules[name];
      framer.send(evalResponseOk(id, { unloaded: name }));
    } catch (e) {
      delete modules[name];
      framer.send(evalResponseErr(id, e));
    }
  }

  function handleModList(msg, framer) {
    var id = msg.id;
    var names = Object.keys(modules);
    framer.send(evalResponseOk(id, names));
  }

  // =========================================================================
  // Eval handler (unchanged except extracted for clarity)
  // =========================================================================
  function handleEval(r, framer) {
    var id   = r.id;
    var code = r.code;
    var w    = r.world || 'auto';
    if (w === 'renderer') {
      var idx = (r.world_index | 0) || 0;
      var electron = getElectron();
      if (!electron || !electron.BrowserWindow) {
        framer.send(evalResponseErr(id, new Error('electron.BrowserWindow unavailable')));
        return;
      }
      var wins = electron.BrowserWindow.getAllWindows();
      if (!wins[idx]) {
        framer.send(evalResponseErr(id, new Error(
          'no BrowserWindow at index ' + idx + ' (have ' + wins.length + ')')));
        return;
      }
      try {
        wins[idx].webContents.executeJavaScript(code, true).then(
          function (v) { framer.send(evalResponseOk(id, v)); },
          function (e) { framer.send(evalResponseErr(id, e)); });
      } catch (e) { framer.send(evalResponseErr(id, e)); }
      return;
    }
    try {
      var v = indirectEval(code);
      if (v && typeof v.then === 'function') {
        v.then(function (r) { framer.send(evalResponseOk(id, r)); },
               function (e) { framer.send(evalResponseErr(id, e)); });
        return;
      }
      framer.send(evalResponseOk(id, v));
    } catch (e) { framer.send(evalResponseErr(id, e)); }
  }

  // =========================================================================
  // Server
  // =========================================================================
  var server = net.createServer(function (socket) {
    socket.setNoDelay(true);
    var framer;
    framer = makeFramer(socket, function (msg) {
      modFramer = framer;
      var k = msg && msg.kind;
      if (k === 'eval')             handleEval(msg, framer);
      else if (k === 'mod.load')    handleModLoad(msg, framer);
      else if (k === 'mod.unload')  handleModUnload(msg, framer);
      else if (k === 'mod.list')    handleModList(msg, framer);
      else if (k === 'detach')      { try { socket.end(); } catch (e) {} }
      else if (k === 'hook.install') {
        framer.send({ kind: 'log', level: 'warn',
                      message: 'native hooks unavailable in v2 (reattach with --native)' });
      }
    });
    var ev = '';
    try { ev = (process.versions && process.versions.electron) || ''; } catch (e) {}
    framer.send({
      kind: 'hello', electron_version: ev, target_type: 'main',
      napi_symbols_present: [], pid: process.pid
    });
    socket.on('error', function () {});
  });

  server.on('error', function (e) {
    globalThis.__positron_v2 = {
      status: 'error',
      message: 'listen failed: ' + ((e && e.message) || String(e))
    };
  });
  server.listen(PORT, '127.0.0.1', function () {
    globalThis.__positron_v2 = { status: 'ready', port: PORT };
  });
  // Expose helpers so the REPL's .mod commands can call them via eval
  // without reconstructing the api or require chain.
  function loadModuleFromCode(code) {
    var exports = indirectEval(code);
    if (!exports || typeof exports !== 'object' || !exports.name)
      throw new Error('module must return {name, onLoad?, onUnload?}');
    var name = exports.name;
    if (modules[name])
      throw new Error('module "' + name + '" already loaded; unload first');
    var api = makeModuleApi(name);
    modules[name] = { exports: exports, api: api };
    if (typeof exports.onLoad === 'function') {
      var ret = exports.onLoad(api);
      if (ret && typeof ret.then === 'function')
        return ret.then(function(){ return { loaded: name }; });
    }
    return { loaded: name };
  }

  function unloadModuleByName(name) {
    if (!modules[name])
      throw new Error('module "' + name + '" not loaded');
    var m = modules[name];
    // Always provide a working api — either the stored one or a fresh one.
    var api = m.api || makeModuleApi(name);
    if (m.exports && typeof m.exports.onUnload === 'function') {
      var ret = m.exports.onUnload(api);
      if (ret && typeof ret.then === 'function')
        return ret.then(function(){ delete modules[name]; return { unloaded: name }; });
    }
    delete modules[name];
    return { unloaded: name };
  }

  function loadModuleFromFile(filePath) {
    var fs = req('fs');
    var code = fs.readFileSync(filePath, 'utf8');
    return loadModuleFromCode(code);
  }

  globalThis.__positron_v2_internal = {
    server: server, modules: modules,
    loadModule: loadModuleFromCode,
    loadModuleFromFile: loadModuleFromFile,
    unloadModule: unloadModuleByName
  };

} catch (e) {
  globalThis.__positron_v2 = {
    status: 'error',
    message: (e && e.message) ? String(e.message) : String(e),
    stack:   (e && e.stack)   ? String(e.stack)   : ''
  };
}
})();
