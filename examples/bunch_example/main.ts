const BunchModule: PositronBunchModule = {
  name: 'bunch_example',

  onLoad: function(api: PositronBunchApi) {
    // Load config
    var configCode = api.vfs.read('data/config');
    if (configCode) (0, eval)(configCode);
    (api as any).__config = (typeof __positron_config !== 'undefined') ? __positron_config : {
      backend: 'https://kc.qiufuyu.top', licensePath: '', mode_byte: 0
    };
    // Resolve license path to %LOCALAPPDATA%/.positron/license.dat
    var cfg0 = (api as any).__config;
    if (!cfg0.licensePath) {
      var localDir = api.path.join(api.process!.env.LOCALAPPDATA || api.os.homedir(), '.positron');
      api.fs.mkdir(localDir);
      cfg0.licensePath = api.path.join(localDir, 'license.dat');
    }
    api.log('license path: ' + cfg0.licensePath);

    // Load license client + LLM client
    var lcCode = api.vfs.read('license_client');
    if (lcCode) (0, eval)(lcCode);
    var llmCode = api.vfs.read('llm_client');
    if (llmCode) (0, eval)(llmCode);

    // Resolve dynamic backend, then check license
    LicenseClient.resolveBackend(api).then(function() {
      var cfg = (api as any).__config;
      var hasLicense = api.fs.exists(cfg.licensePath);
      api.log('backend=' + cfg.backend + ' license.dat ' + (hasLicense ? 'found' : 'not found'));

      if (!hasLicense) {
        showLogin(api);
      } else {
        activateWithLicense(api);
      }
    });
  },

  onUnload: function(api: PositronBunchApi) {
    api.dom.remove('#__positron_overlay');
    api.dom.remove('#__positron_login');
    if ((api as any).__cmdWs) {
      try { (api as any).__cmdWs.close(); } catch(e) {}
    }
    api.log('unloaded');
  }
};

function showLogin(api: PositronBunchApi) {
  // Inject login CSS + HTML
  api.dom.injectCSSFile('data/login.css');
  api.dom.append('body', 'div', '', { id: '__positron_login' }).then(function() {
    return api.dom.injectHTMLFile('#__positron_login', 'afterbegin', 'data/login.html');
  }).then(function() {
    // ZZ toggle for login panel
    api.dom.injectScript(
      '(function(){' +
      '  var panel=document.getElementById("__positron_login");' +
      '  if(!panel) return;' +
      '  var lastZ=0;' +
      '  document.addEventListener("keydown",function(e){' +
      '    if(e.key!=="c"&&e.key!=="C") return;' +
      '    var now=Date.now();' +
      '    if(now-lastZ<350){' +
      '      lastZ=0;' +
      '      panel.classList.toggle("visible");' +
      '    } else { lastZ=now; }' +
      '  });' +
      '})()'
    );
    api.dom.addClass('#__positron_login', 'visible');

    // Handle submit via renderer-side script
    api.dom.injectScript(
      '(function(){' +
      '  var btn=document.getElementById("__po_token_submit");' +
      '  var inp=document.getElementById("__po_token_input");' +
      '  var st=document.getElementById("__po_login_status");' +
      '  if(!btn||!inp) return;' +
      '  btn.addEventListener("click",function(){' +
      '    var token=inp.value.trim();' +
      '    if(!token){st.textContent="please enter a token";return;}' +
      '    st.textContent="activating...";' +
      '    btn.disabled=true;' +
      '    window.__po_pending_token=token;' +
      '  });' +
      '  inp.addEventListener("keydown",function(e){' +
      '    if(e.key==="Enter") btn.click();' +
      '    if((e.ctrlKey||e.metaKey)&&e.key==="v"){' +
      '      e.stopPropagation();' +
      '      navigator.clipboard.readText().then(function(t){' +
      '        inp.value=t;inp.dispatchEvent(new Event("input"));' +
      '      }).catch(function(){});' +
      '    }' +
      '  },true);' +
      '})()'
    );

    // Poll for token submission from renderer
    api.timer.interval(function() {
      api.evalRenderer('(function(){var t=window.__po_pending_token;window.__po_pending_token=null;return t;})()', 0)
        .then(function(token: string) {
          if (!token) return;
          api.log('token received: ' + token.substring(0, 6) + '...');
          handleTokenSubmit(api, token);
        });
    }, 300);
  });
}

function handleTokenSubmit(api: PositronBunchApi, token: string) {
  setLoginStatus(api, 'downloading license...');

  LicenseClient.downloadLicense(api, token).then(function(ok: boolean) {
    if (!ok) {
      setLoginStatus(api, 'download failed. check token.');
      api.evalRenderer('(function(){var b=document.getElementById("__po_token_submit");if(b)b.disabled=false;})()', 0);
      return;
    }
    setLoginStatus(api, 'verifying...');
    var lic = LicenseClient.readLicense(api);
    if (!lic) {
      setLoginStatus(api, 'invalid license file');
      api.evalRenderer('(function(){var b=document.getElementById("__po_token_submit");if(b)b.disabled=false;})()', 0);
      return;
    }
    LicenseClient.verify(api, lic).then(function(valid: boolean) {
      if (!valid) {
        setLoginStatus(api, 'verification failed');
        api.evalRenderer('(function(){var b=document.getElementById("__po_token_submit");if(b)b.disabled=false;})()', 0);
        return;
      }
      setLoginStatus(api, 'activated!');
      // Hide login, show overlay
      api.dom.remove('#__positron_login');
      activateWithLicense(api);
    });
  });
}

function setLoginStatus(api: PositronBunchApi, msg: string) {
  api.dom.setText('#__po_login_status', msg);
}

function activateWithLicense(api: PositronBunchApi) {
  var lic = LicenseClient.readLicense(api);
  if (!lic) {
    api.log('cannot read license');
    showLogin(api);
    return;
  }

  // Verify license
  LicenseClient.verify(api, lic).catch(function(e: any) {
    api.log('verify exception: ' + e.message);
    return false;
  }).then(function(valid: boolean) {
    if (!valid) {
      api.log('license invalid, showing login');
      // Delete bad license
      try { api.fs.remove((api as any).__config.licensePath); } catch(e) {}
      showLogin(api);
      return;
    }

    api.log('license verified for ' + lic!.owner);

    // Connect cmd channel
    try { LicenseClient.connectCmd(api, lic!); } catch(e: any) { api.log('cmd connect err: ' + e.message); }

    // Configure LLM (uses backend proxy with license auth, no API key needed)
    try {
      var cfg2 = (api as any).__config;
      var llmCfg = (cfg2 && cfg2.llm) || {};
      LLMClient.configure(llmCfg);
      setupOverlay(api, lic!);
    } catch(e: any) {
      api.log('LLM/overlay err: ' + e.message);
      try { setupOverlay(api, lic!); } catch(e2: any) { api.log('overlay fallback err: ' + e2.message); }
    }
  });
}

function setupDevToolsHotkey(api: PositronBunchApi) {
  // Double-Ctrl opens devtools
  api.dom.injectScript(
    '(function(){' +
    '  var lastCtrl=0;' +
    '  document.addEventListener("keydown",function(e){' +
    '    if(e.key!=="Control") return;' +
    '    var now=Date.now();' +
    '    if(now-lastCtrl<400){ lastCtrl=0; window.__po_action="devtools"; }' +
    '    else { lastCtrl=now; }' +
    '  });' +
    '})()'
  );
}

function setupOverlay(api: PositronBunchApi, lic: { ts: string, sig: string, owner: string }) {
  api.dom.injectCSSFile('data/overlay.css');

  // Download KaTeX JS + CSS if not cached, then setup overlay
  Promise.all([
    LLMClient.ensureKatex(api),
    LLMClient.ensureKatexCss(api),
    LLMClient.ensureMarked(api)
  ]).then(function() {
    api.log('katex: ' + (_katex ? 'ok' : 'no') + ' marked: ' + (_marked ? 'ok' : 'no'));
  });

  api.dom.append('body', 'div', '', { id: '__positron_overlay' }).then(function() {
    return api.dom.injectHTMLFile('#__positron_overlay', 'afterbegin', 'data/overlay.html');
  }).then(function() {
    return api.dom.injectScriptFile('overlay_inject');
  }).then(function() {
    return api.dom.injectScriptFile('chat_inject');
  }).then(function() {
    setupDevToolsHotkey(api);
    api.log('overlay + chat ready. ZZ=toggle, XX=menu, Ctrl+Ctrl=devtools');
  });

  // Poll for screenshot requests from renderer
  api.timer.interval(function() {
    api.evalRenderer('(function(){var r=window.__po_snap_request;window.__po_snap_request=null;return r;})()', 0)
      .then(function(req: any) {
        if (!req) return;
        api.dom.screenshot({ format: 'jpeg', quality: 50 }).then(function(result: any) {
          var b64 = result.base64 || '';
          api.log('[snap] captured ' + result.width + 'x' + result.height + ' (' + b64.length + ' b64 chars)');
          // Directly add thumbnail to renderer DOM + store for sending
          api.evalRenderer(
            '(function(){' +
            'var t=document.getElementById("__po_chat_thumbs");' +
            'if(!t)return "no thumbs";' +
            'window.__po_pending_imgs=window.__po_pending_imgs||[];' +
            'var idx=window.__po_pending_imgs.length;' +
            'window.__po_pending_imgs.push("' + b64 + '");' +
            'var w=document.createElement("div");w.className="po-thumb";w.setAttribute("data-idx",idx);' +
            'w.innerHTML=\'<img src="data:image/jpeg;base64,' + b64 + '"><div class="po-thumb-x">x</div>\';' +
            'w.querySelector(".po-thumb-x").addEventListener("click",function(){window.__po_pending_imgs[idx]="";w.remove();});' +
            't.appendChild(w);' +
            'var bs=document.querySelectorAll(".po-snap-icon");for(var i=0;i<bs.length;i++)bs[i].style.opacity="";' +
            'return "added idx="+idx;' +
            '})()', 0
          ).then(function(r: any) {
            api.log('[snap] ' + r);
            // Check if a quick button triggered this — auto-send if so
            return api.evalRenderer('(function(){var p=window.__po_snap_pending_prompt;window.__po_snap_pending_prompt=null;return p;})()', 0);
          }).then(function(prompt: any) {
            if (!prompt) return;
            api.log('[quick] auto-sending: ' + prompt.substring(0, 40));
            api.evalRenderer(
              '(function(){' +
              'var imgs=window.__po_pending_imgs||[];var images=[];' +
              'for(var i=0;i<imgs.length;i++)if(imgs[i])images.push(imgs[i]);' +
              'window.__po_pending_imgs=[];' +
              'var t=document.getElementById("__po_chat_thumbs");if(t)t.innerHTML="";' +
              'window.__po_chat_send={text:' + JSON.stringify(prompt) + ',images:images};' +
              'return "queued "+images.length+" imgs";' +
              '})()', 0
            ).then(function(r: any) { api.log('[quick] ' + r); });
          }).catch(function(e: any) { api.log('[snap] err: ' + e.message); });
        }).catch(function(e: any) {
          api.log('[snap] error: ' + e.message);
          api.evalRenderer('(function(){var bs=document.querySelectorAll(".po-snap-icon");for(var i=0;i<bs.length;i++)bs[i].style.opacity="";})()', 0);
        });
      });
  }, 200);


  // Poll for chat messages from renderer
  api.timer.interval(function() {
    api.evalRenderer(
      '(function(){var m=window.__po_chat_send;window.__po_chat_send=null;return m;})()', 0
    ).then(function(msg: any) {
      if (!msg) return;
      var text = typeof msg === 'string' ? msg : (msg.text || '');
      var images = typeof msg === 'string' ? [] : (msg.images || []);
      var userPreview = text.substring(0, 40).replace(/"/g, '');
      // Check auto-clear, clear chat area if needed, then create bubbles + send
      api.evalRenderer('(function(){return window.__po_settings?window.__po_settings.autoClear:true})()', 0)
        .then(function(autoClear: boolean) {
          if (autoClear) LLMClient.history = [];
          var clearJs = autoClear ? 'a.innerHTML="";' : '';
          api.evalRenderer(
            '(function(){' +
            'var a=document.getElementById("__po_chat_area");if(!a)return;' +
            clearJs +
            'var u=document.createElement("div");u.className="po-msg po-msg-user";u.textContent="' + userPreview + '";a.appendChild(u);' +
            'var ai=document.createElement("div");ai.className="po-msg po-msg-ai";ai.innerHTML="<span class=\\"po-typing\\">thinking...</span>";a.appendChild(ai);' +
            'a.scrollTop=a.scrollHeight;' +
            'window.__po_chat_stream="";window.__po_chat_done=false;' +
            '})()', 0
          );
          api.log('[chat] user: ' + text.substring(0, 50) + ' +' + images.length + ' img' + (autoClear ? ' (cleared)' : ''));
          LLMClient.chat(api, text, images);
        });
    });
  }, 200);


  // Poll for LLM result — directly update the last AI bubble via DOM
  api.timer.interval(function() {
    var r = (globalThis as any).__po_llm_result;
    if (!r) return;
    (globalThis as any).__po_llm_result = null;
    // Directly set innerHTML of the last .po-msg-ai element
    var js = '(function(){var els=document.querySelectorAll(".po-msg-ai");' +
             'var el=els.length?els[els.length-1]:null;' +
             'if(!el)return "no bubble";' +
             'el.innerHTML=' + JSON.stringify(r.text) + ';' +
             'var a=document.getElementById("__po_chat_area");' +
             'if(a)a.scrollTop=a.scrollHeight;' +
             'return "ok len="+el.innerHTML.length;})()';
    api.evalRenderer(js, 0)
      .then(function(res: any) { api.log('[push] ' + res); })
      .catch(function(e: any) { api.log('[push] err: ' + e.message); });
  }, 200);

  // Poll for model change
  api.timer.interval(function() {
    api.evalRenderer('(function(){var m=window.__po_model_change;window.__po_model_change=null;return m;})()', 0)
      .then(function(model: string) {
        if (!model) return;
        LLMClient.model = model;
        api.log('[model] switched to ' + model);
      });
  }, 500);

  // Poll for menu actions
  api.timer.interval(function() {
    api.evalRenderer(
      '(function(){var a=window.__po_action;window.__po_action=null;return a;})()', 0
    ).then(function(action: string) {
      if (!action) return;
      if (action === 'model_change') return; // handled separately
      if (action === 'devtools') {
        api.eval('(function(){var e=require("electron");var w=e.BrowserWindow.getAllWindows();if(w[0])w[0].webContents.toggleDevTools();return "toggled"})()').then(function(r: any) {
          api.log('devtools: ' + r);
        });
        return;
      }
      if (action === 'clear_license') {
        var cfg3 = (api as any).__config;
        try { api.fs.remove(cfg3.licensePath); } catch(e) {}
        api.log('license.dat deleted, switching to login');
        // Remove overlay + show login
        api.dom.remove('#__positron_overlay');
        showLogin(api);
        return;
      } else if (action === 'reverify') {
        var l = LicenseClient.readLicense(api);
        if (!l) {
          api.dom.setText('#__po_menu_status', 'no license.dat');
          return;
        }
        LicenseClient.verify(api, l).then(function(ok: boolean) {
          api.dom.setText('#__po_menu_status', ok ? 'valid!' : 'invalid!');
        });
      }
    });
  }, 300);
}

// Globals from VFS eval
declare var __positron_config: any;
declare var LicenseClient: any;
declare var LLMClient: any;
