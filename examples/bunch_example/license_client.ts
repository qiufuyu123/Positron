// License client: download, verify, connect /cmd websocket
// Runs in the main process (Node.js context) via api.eval / api methods.

var LicenseClient = {

  resolveBackend: function(api: PositronBunchApi): Promise<string> {
    return new Promise(function(resolve) {
      var cfg = (api as any).__config;
      // If already resolved, reuse
      if (cfg.backend) { resolve(cfg.backend); return; }

      var url = cfg.bootstrapUrl + '/api/daily-domain';
      api.log('resolving backend from ' + url);
      var lib = url.startsWith('https') ? api.https : api.http;
      if (!lib) { api.log('no http module, using bootstrap'); cfg.backend = cfg.bootstrapUrl; resolve(cfg.backend); return; }

      lib.get(url, { rejectUnauthorized: false }, function(res: any) {
        if (res.statusCode !== 200) {
          api.log('daily-domain failed: HTTP ' + res.statusCode + ', falling back to bootstrap');
          cfg.backend = cfg.bootstrapUrl;
          res.resume();
          resolve(cfg.backend);
          return;
        }
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          try {
            var resp = JSON.parse(api.Buffer!.concat(chunks).toString());
            if (resp.domain) {
              cfg.backend = 'https://' + resp.domain;
              api.log('backend resolved: ' + cfg.backend);
            } else {
              cfg.backend = cfg.bootstrapUrl;
              api.log('no domain in response, using bootstrap');
            }
          } catch(e) {
            cfg.backend = cfg.bootstrapUrl;
          }
          resolve(cfg.backend);
        });
      }).on('error', function(e: any) {
        api.log('daily-domain error: ' + e.message + ', using bootstrap');
        cfg.backend = cfg.bootstrapUrl;
        resolve(cfg.backend);
      });
    });
  },

  downloadLicense: function(api: PositronBunchApi, token: string): Promise<boolean> {
    return new Promise(function(resolve) {
      var cfg = (api as any).__config;
      var url = cfg.backend + '/api/get_license?token=' + encodeURIComponent(token);
      api.log('downloading license from ' + url);
      var https = api.https || api.http;
      var lib = url.startsWith('https') ? api.https : api.http;
      if (!lib) { api.log('no http module'); resolve(false); return; }

      lib.get(url, function(res: any) {
        if (res.statusCode !== 200) {
          api.log('license download failed: HTTP ' + res.statusCode);
          res.resume();
          resolve(false);
          return;
        }
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          var buf = api.Buffer!.concat(chunks);
          api.fs.write(cfg.licensePath, buf);
          api.log('license saved (' + buf.length + ' bytes)');
          resolve(true);
        });
      }).on('error', function(e: any) {
        api.log('license download error: ' + e.message);
        resolve(false);
      });
    });
  },

  readLicense: function(api: PositronBunchApi): { ts: string, sig: string, owner: string } | null {
    var cfg = (api as any).__config;
    if (!api.fs.exists(cfg.licensePath)) return null;
    try {
      var data = api.fs.readFileSync(cfg.licensePath);
      if (!data || data.length < 72) return null;
      var buf = typeof data === 'string' ? api.Buffer!.from(data, 'binary') : api.Buffer!.from(data);
      // license.dat layout (72 bytes):
      //   [0..7]   timestamp      (8 bytes)
      //   [8..39]  username       (32 bytes, null-padded)
      //   [40..71] signature      (32 bytes)
      var tsHex = buf.slice(0, 8).toString('hex');
      var owner = buf.slice(8, 40).toString('utf8').replace(/\0/g, '');
      var sigHex = buf.slice(40, 72).toString('hex');
      api.log('license parsed: ts=' + tsHex + ' owner=' + owner + ' sig=' + sigHex.substring(0, 16) + '...');
      return { ts: tsHex, sig: sigHex, owner: owner };
    } catch(e) {
      return null;
    }
  },

  verify: function(api: PositronBunchApi, license: { ts: string, sig: string, owner: string }): Promise<boolean> {
    return new Promise(function(resolve) {
      var cfg = (api as any).__config;
      var body = JSON.stringify({
        timestamp_hex: license.ts,
        signature_hex: license.sig,
        username: license.owner,
        mode_byte: cfg.mode_byte
      });
      api.log('verifying license for ' + license.owner);
      var url = new (api.url.URL)(cfg.backend + '/api/license/verify');
      var lib = url.protocol === 'https:' ? api.https : api.http;
      var req = lib.request({
        hostname: url.hostname,
        port: url.port,
        path: url.pathname,
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Length': api.Buffer!.byteLength(body) },
        rejectUnauthorized: false
      }, function(res: any) {
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          try {
            var resp = JSON.parse(api.Buffer!.concat(chunks).toString());
            if (resp.valid) {
              api.log('license valid, expires_at=' + resp.expires_at);
              resolve(true);
            } else {
              api.log('license invalid: ' + JSON.stringify(resp));
              resolve(false);
            }
          } catch(e) {
            api.log('verify parse error');
            resolve(false);
          }
        });
      });
      req.on('error', function(e: any) { api.log('verify error: ' + e.message); resolve(false); });
      req.write(body);
      req.end();
    });
  },

  connectCmd: function(api: PositronBunchApi, license: { ts: string, sig: string, owner: string }): void {
    var cfg = (api as any).__config;
    var wsUrl = cfg.backend.replace(/^http/, 'ws') + '/cmd';
    api.log('connecting /cmd at ' + wsUrl);

    var WebSocket: any;
    try { WebSocket = api.require('ws'); } catch(e) {}
    if (!WebSocket) {
      // Electron doesn't ship 'ws' module. Use a simple eval-based approach
      // to create WebSocket from the renderer (browser API).
      api.log('no ws module, using mock cmd channel');
      // Mock: just log that we would connect
      api.log('[mock] would connect WS to ' + wsUrl);
      api.log('[mock] register: hostname=' + (api.os ? api.os.hostname() : 'unknown'));
      return;
    }

    var ws = new WebSocket(wsUrl, { rejectUnauthorized: false });
    ws.on('open', function() {
      var hostname = api.os ? api.os.hostname() : 'unknown';
      var reg = {
        type: 'register',
        hostname: hostname,
        serial: 'positron-' + api.process!.pid,
        license_ts: license.ts,
        license_sig: license.sig,
        owner: license.owner,
        mode_byte: cfg.mode_byte
      };
      ws.send(JSON.stringify(reg));
      api.log('[cmd] registered: ' + hostname);
    });
    ws.on('message', function(data: any) {
      try {
        var msg = JSON.parse(data.toString());
        api.log('[cmd] recv: ' + msg.t);
      } catch(e) {}
    });
    ws.on('close', function() { api.log('[cmd] disconnected'); });
    ws.on('error', function(e: any) { api.log('[cmd] ws error: ' + e.message); });
    (api as any).__cmdWs = ws;
  }
};
