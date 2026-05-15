// positron v2 transport bootstrap
//
// Loaded from disk by the SDK on every attach. Resolution order:
//   1. POSITRON_BOOTSTRAP_JS_PATH env (absolute path)
//   2. <host.exe-dir>/bootstrap.js  (msbuild copy step)

(function(){
try {
  // Use a randomized global key to avoid fingerprinting
  var _gk = '_' + Math.random().toString(36).substring(2, 10) + Math.random().toString(36).substring(2, 6);
  // Check if already bootstrapped (any key matching pattern)
  for (var _ek in globalThis) {
    if (_ek.charAt(0) === '_' && globalThis[_ek] && globalThis[_ek].__positron_status === 'ready') {
      _gk = _ek; // reuse existing key
      return;
    }
  }

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

  // Resolve commonly-needed Node/Electron modules once, cache for all modules.
  var _fs = null, _path = null, _os = null, _cp = null, _http = null, _https = null, _url = null, _crypto = null;
  try { _fs     = req('fs');     } catch(e){}
  try { _path   = req('path');   } catch(e){}
  try { _os     = req('os');     } catch(e){}
  try { _cp     = req('child_process'); } catch(e){}
  try { _http   = req('http');   } catch(e){}
  try { _https  = req('https');  } catch(e){}
  try { _url    = req('url');    } catch(e){}
  try { _crypto = req('crypto'); } catch(e){}

  function makeModuleApi(name) {
    var electron = getElectron();
    var wins = (electron && electron.BrowserWindow) ? electron.BrowserWindow.getAllWindows() : [];

    return {
      // --- eval ---
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
          var el = getElectron();
          if (!el || !el.BrowserWindow) { reject(new Error('no BrowserWindow')); return; }
          var w = el.BrowserWindow.getAllWindows();
          if (!w[idx]) { reject(new Error('no window at index ' + idx)); return; }
          w[idx].webContents.executeJavaScript(code, true).then(resolve, reject);
        });
      },

      // --- communication ---
      send: function(msg) {
        if (modFramer) modFramer.send({ kind: 'mod.event', module: name, data: msg });
      },
      log: function(str) {
        if (modFramer) modFramer.send({ kind: 'log', level: 'info',
                                        message: '[mod:' + name + '] ' + str });
      },

      // --- Node.js core modules ---
      require: req,
      fs: (function(){
        var o = _fs || {};
        o.read  = function(p, enc) { return _fs ? _fs.readFileSync(p, enc || 'utf8') : null; };
        o.write = function(p, data) { if (_fs) _fs.writeFileSync(p, data); };
        o.exists = function(p) { return _fs ? _fs.existsSync(p) : false; };
        o.mkdir = function(p) { if (_fs) _fs.mkdirSync(p, {recursive:true}); };
        o.readdir = function(p) { return _fs ? _fs.readdirSync(p) : []; };
        o.remove = function(p) { if (_fs) _fs.rmSync(p, {recursive:true, force:true}); };
        o.stat = function(p) { return _fs ? _fs.statSync(p) : null; };
        return o;
      })(),
      path: _path,
      os: _os,
      childProcess: _cp,
      http: _http,
      https: _https,
      url: _url,
      crypto: _crypto,

      // --- Node.js globals ---
      process: (typeof process !== 'undefined') ? process : null,
      Buffer: (typeof Buffer !== 'undefined') ? Buffer : null,

      // --- Electron ---
      electron: electron,
      getElectron: getElectron,
      getWindows: function() {
        var el = getElectron();
        return (el && el.BrowserWindow) ? el.BrowserWindow.getAllWindows() : [];
      },
      getWebContents: function(idx) {
        var w = this.getWindows();
        return w[idx || 0] ? w[idx || 0].webContents : null;
      },

      // --- convenience ---
      exec: function(cmd) {
        if (!_cp) return null;
        return _cp.execSync(cmd, { encoding: 'utf8' });
      },
      setTimeout: setTimeout,
      setInterval: setInterval,
      clearTimeout: clearTimeout,
      clearInterval: clearInterval
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
    if (_serverShutdown) { try { socket.destroy(); } catch(e) {} return; }
    socket.setNoDelay(true);
    _conns.push(socket);
    socket.on('close', function() {
      var idx = _conns.indexOf(socket);
      if (idx >= 0) _conns.splice(idx, 1);
    });
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

  // Create the global object first, then listen callback updates status in-place
  globalThis[_gk] = { __positron_status: 'pending', port: 0 };

  server.on('error', function (e) {
    globalThis[_gk].__positron_status = 'error';
    globalThis[_gk].message = 'listen failed: ' + ((e && e.message) || String(e));
  });
  server.listen(PORT, '127.0.0.1', function () {
    globalThis[_gk].__positron_status = 'ready';
    globalThis[_gk].port = PORT;
  });
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

  function listLoadedModules() {
    return Object.keys(modules);
  }

  var _conns = []; // track all connected sockets

  var _serverShutdown = false;
  function shutdownServer() {
    _serverShutdown = true;
    // Don't touch the server object at all — just reject new connections
    // at the application level. The port stays open but idle.
    return { shutdown: true };
  }

  globalThis[_gk].i = {
    server: server, modules: modules,
    loadModule: loadModuleFromCode,
    loadModuleFromFile: loadModuleFromFile,
    unloadModule: unloadModuleByName,
    listModules: listLoadedModules,
    shutdown: shutdownServer
  };

} catch (e) {
  globalThis[_gk] = {
    __positron_status: 'error',
    message: (e && e.message) ? String(e.message) : String(e),
    stack:   (e && e.stack)   ? String(e.stack)   : ''
  };
}
})();
