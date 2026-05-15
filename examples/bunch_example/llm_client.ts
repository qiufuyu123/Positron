// LLM client — calls OpenRouter from main process (bypasses renderer CSP).
// Streams response chunks back to renderer via window.__po_chat_stream.

var LLMClient = {
  model: 'google/gemini-3.1-pro-preview',
  systemPrompt: 'You are a helpful assistant. Answer concisely. Use LaTeX for math (wrap with $ for inline, $$ for display). Use markdown for formatting.',
  history: [] as Array<{role: string, content: string}>,
  _activeReq: null as any,
  _activeTimeout: null as any,
  _activeFinished: false,

  configure: function(opts: { model?: string, systemPrompt?: string }) {
    if (opts.model) LLMClient.model = opts.model;
    if (opts.systemPrompt) LLMClient.systemPrompt = opts.systemPrompt;
  },

  ensureKatex: function(api: PositronBunchApi): Promise<void> {
    if (_katex) return Promise.resolve();
    return new Promise(function(resolve) {
      var cfg = (api as any).__config || {};
      var tmpDir = api.path.join(api.os.tmpdir(), '.positron_katex');
      var jsPath = api.path.join(tmpDir, 'katex.min.js');

      // Already downloaded?
      if (api.fs.exists(jsPath)) {
        try { _katex = api.require(jsPath); api.log('katex loaded from cache'); } catch(e) {}
        resolve(); return;
      }

      var cdnBase = cfg.katexCdn || 'https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/';
      var url = cdnBase + 'katex.min.js';
      api.log('downloading katex from ' + url);

      var lib = url.startsWith('https') ? api.https : api.http;
      if (!lib) { api.log('no https module'); resolve(); return; }

      api.fs.mkdir(tmpDir);
      lib.get(url, { rejectUnauthorized: false }, function(res: any) {
        if (res.statusCode !== 200) { res.resume(); api.log('katex download failed: ' + res.statusCode); resolve(); return; }
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          var buf = api.Buffer!.concat(chunks);
          api.fs.write(jsPath, buf);
          api.log('katex saved (' + buf.length + ' bytes)');
          try { _katex = api.require(jsPath); api.log('katex loaded'); } catch(e: any) { api.log('katex require err: ' + e.message); }
          resolve();
        });
      }).on('error', function(e: any) { api.log('katex download err: ' + e.message); resolve(); });
    });
  },

  ensureKatexCss: function(api: PositronBunchApi): Promise<void> {
    return new Promise(function(resolve) {
      var cfg = (api as any).__config || {};
      var tmpDir = api.path.join(api.os.tmpdir(), '.positron_katex');
      var cssPath = api.path.join(tmpDir, 'katex.min.css');

      if (api.fs.exists(cssPath)) {
        var css = api.fs.read(cssPath, 'utf8');
        if (css) api.dom.injectCSS(css);
        resolve(); return;
      }

      var cdnBase = cfg.katexCdn || 'https://cdn.jsdelivr.net/npm/katex@0.16.11/dist/';
      var url = cdnBase + 'katex.min.css';
      var lib = url.startsWith('https') ? api.https : api.http;
      if (!lib) { resolve(); return; }

      api.fs.mkdir(tmpDir);
      lib.get(url, { rejectUnauthorized: false }, function(res: any) {
        if (res.statusCode !== 200) { res.resume(); resolve(); return; }
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          var css = api.Buffer!.concat(chunks).toString('utf8');
          api.fs.write(cssPath, css);
          api.dom.injectCSS(css);
          api.log('katex css injected (' + css.length + ' chars)');
          resolve();
        });
      }).on('error', function() { resolve(); });
    });
  },

  ensureMarked: function(api: PositronBunchApi): Promise<void> {
    if (_marked) return Promise.resolve();
    return new Promise(function(resolve) {
      var tmpDir = api.path.join(api.os.tmpdir(), '.positron_katex');
      var jsPath = api.path.join(tmpDir, 'marked.min.js');

      if (api.fs.exists(jsPath)) {
        try { _marked = api.require(jsPath); api.log('marked loaded from cache'); } catch(e) {}
        resolve(); return;
      }

      var url = 'https://cdn.jsdelivr.net/npm/marked@15.0.7/lib/marked.cjs';
      api.log('downloading marked from ' + url);
      var lib = api.https;
      if (!lib) { resolve(); return; }

      api.fs.mkdir(tmpDir);
      lib.get(url, { rejectUnauthorized: false }, function(res: any) {
        if (res.statusCode !== 200) { res.resume(); api.log('marked download failed: ' + res.statusCode); resolve(); return; }
        var chunks: any[] = [];
        res.on('data', function(c: any) { chunks.push(c); });
        res.on('end', function() {
          var buf = api.Buffer!.concat(chunks);
          api.fs.write(jsPath, buf);
          api.log('marked saved (' + buf.length + ' bytes)');
          try { _marked = api.require(jsPath); api.log('marked loaded'); } catch(e: any) { api.log('marked require err: ' + e.message); }
          resolve();
        });
      }).on('error', function(e: any) { api.log('marked download err: ' + e.message); resolve(); });
    });
  },

  abort: function() {
    // Kill the in-flight request if any
    LLMClient._activeFinished = true;
    if (LLMClient._activeTimeout) { clearTimeout(LLMClient._activeTimeout); LLMClient._activeTimeout = null; }
    if (LLMClient._activeReq) { try { LLMClient._activeReq.destroy(); } catch(e) {} LLMClient._activeReq = null; }
  },

  chat: function(api: PositronBunchApi, userMsg: string, images?: string[]): void {
    LLMClient.abort();
    api_ref = api;

    var cfg = (api as any).__config || {};
    var backend = cfg.backend || cfg.bootstrapUrl;
    if (!backend) { pushToRenderer(api, 'Error: no backend configured', true); return; }

    // Read license for auth
    var lic = typeof LicenseClient !== 'undefined' ? LicenseClient.readLicense(api) : null;
    if (!lic) { pushToRenderer(api, 'Error: no license for LLM auth', true); return; }

    // Build user message content
    var userContent: any;
    if (images && images.length > 0) {
      var parts: any[] = [];
      for (var ii = 0; ii < images.length; ii++) {
        parts.push({ type: 'image_url', image_url: { url: 'data:image/jpeg;base64,' + images[ii] } });
      }
      if (userMsg) parts.push({ type: 'text', text: userMsg });
      userContent = parts;
    } else {
      userContent = userMsg;
    }
    LLMClient.history.push({ role: 'user', content: userContent });

    var messages: any[] = [{ role: 'system', content: LLMClient.systemPrompt }];
    var hist = LLMClient.history.slice(-20);
    messages = messages.concat(hist);

    // POST /api/llm/completions — license auth in body, SSE response
    var bodyObj: any = {
      timestamp_hex: lic.ts,
      signature_hex: lic.sig,
      username: lic.owner,
      mode_byte: cfg.mode_byte || 0,
      model: LLMClient.model,
      messages: messages,
      stream: true
    };
    var body = JSON.stringify(bodyObj);
    var url = backend + '/api/llm/completions';
    api.log('[llm] POST ' + url + ' model=' + LLMClient.model);

    var urlObj = new (api.url.URL)(url);
    var lib = urlObj.protocol === 'https:' ? api.https : api.http;

    var req = lib.request({
      hostname: urlObj.hostname,
      port: urlObj.port || (urlObj.protocol === 'https:' ? 443 : 80),
      path: urlObj.pathname,
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Content-Length': api.Buffer!.byteLength(body),
        'Accept': 'text/event-stream'
      },
      rejectUnauthorized: false
    }, function(res: any) {
      if (res.statusCode !== 200) {
        var errChunks: any[] = [];
        res.on('data', function(c: any) { errChunks.push(c); });
        res.on('end', function() {
          var errMsg = 'LLM error ' + res.statusCode + ': ' + api.Buffer!.concat(errChunks).toString().substring(0, 300);
          api.log(errMsg);
          pushToRenderer(api, errMsg, true);
        });
        return;
      }

      // SSE stream — backend returns data: {"chunk":"...","done":false} lines
      var full = '';
      var buffer = '';
      LLMClient._activeFinished = false;

      LLMClient._activeTimeout = setTimeout(function() {
        if (!LLMClient._activeFinished) {
          LLMClient._activeFinished = true;
          api.log('LLM timeout');
          if (full) LLMClient.history.push({ role: 'assistant', content: full });
          pushToRenderer(api, full || '(timeout)', true);
        }
      }, 120000);

      function finish() {
        if (LLMClient._activeFinished) return;
        LLMClient._activeFinished = true;
        LLMClient._activeReq = null;
        if (LLMClient._activeTimeout) { clearTimeout(LLMClient._activeTimeout); LLMClient._activeTimeout = null; }
        if (full) LLMClient.history.push({ role: 'assistant', content: full });
        pushToRenderer(api, full || '(empty response)', true);
      }

      res.on('data', function(chunk: any) {
        if (LLMClient._activeFinished) return;
        buffer += chunk.toString();
        var lines = buffer.split('\n');
        buffer = lines.pop() || '';
        for (var i = 0; i < lines.length; i++) {
          var line = lines[i].trim();
          if (!line || !line.startsWith('data: ')) continue;
          var data = line.substring(6);
          if (data === '[DONE]') { finish(); return; }
          try {
            var parsed = JSON.parse(data);
            // Backend SSE format: {"chunk":"...","done":false} or {"text":"...","done":true}
            if (parsed.done) {
              full = parsed.text || full;
              finish();
              return;
            }
            if (parsed.chunk) {
              full += parsed.chunk;
              pushToRenderer(api, full, false);
            }
            // Also handle OpenRouter raw format
            if (parsed.choices && parsed.choices[0] && parsed.choices[0].delta && parsed.choices[0].delta.content) {
              full += parsed.choices[0].delta.content;
              pushToRenderer(api, full, false);
            }
          } catch(e) {}
        }
      });

      res.on('end', function() { finish(); });
      res.on('error', function(e: any) {
        if (!LLMClient._activeFinished) {
          api.log('LLM stream error: ' + e.message);
          full += '\n\n(stream error)';
          finish();
        }
      });
    });

    req.on('error', function(e: any) {
      api.log('LLM request error: ' + e.message);
      pushToRenderer(api, 'Error: ' + e.message, true);
    });

    LLMClient._activeReq = req;
    req.write(body);
    req.end();
  }
};

var _katex: any = null;
var _marked: any = null;
var api_ref: any = null;

function renderText(text: string): string {
  // 1. Protect LaTeX by replacing with placeholders before markdown
  var mathBlocks: string[] = [];
  function stash(html: string): string {
    var idx = mathBlocks.length;
    mathBlocks.push(html);
    return '\x00MATH' + idx + '\x00';
  }

  if (_katex) {
    // Display: $$...$$ and \[...\]
    text = text.replace(/\$\$([\s\S]+?)\$\$/g, function(_: string, math: string) {
      try { return stash(_katex.renderToString(math.trim(), { displayMode: true, throwOnError: false })); }
      catch(e) { return '$$' + math + '$$'; }
    });
    text = text.replace(/\\\[([\s\S]+?)\\\]/g, function(_: string, math: string) {
      try { return stash(_katex.renderToString(math.trim(), { displayMode: true, throwOnError: false })); }
      catch(e) { return '\\[' + math + '\\]'; }
    });
    // Inline: $..$ and \(...\)
    text = text.replace(/\$([^$\n]+?)\$/g, function(_: string, math: string) {
      try { return stash(_katex.renderToString(math.trim(), { displayMode: false, throwOnError: false })); }
      catch(e) { return '$' + math + '$'; }
    });
    text = text.replace(/\\\(([\s\S]+?)\\\)/g, function(_: string, math: string) {
      try { return stash(_katex.renderToString(math.trim(), { displayMode: false, throwOnError: false })); }
      catch(e) { return '\\(' + math + '\\)'; }
    });
  }

  // 2. Markdown rendering
  if (_marked) {
    try {
      text = _marked.parse(text, { breaks: true });
    } catch(e) {}
  }

  // 3. Restore LaTeX placeholders
  text = text.replace(/\x00MATH(\d+)\x00/g, function(_: string, idx: string) {
    return mathBlocks[parseInt(idx)] || '';
  });

  return text;
}

// Store result on main-side global — a poll timer in main.ts picks it up
function pushToRenderer(api: PositronBunchApi, text: string, done: boolean) {
  var rendered = renderText(text);
  (globalThis as any).__po_llm_result = { text: rendered, done: done };
}
