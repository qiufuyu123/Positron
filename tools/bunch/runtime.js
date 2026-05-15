// Positron Bunch Runtime
// Injected into every .b.js bundle. Provides VFS, DOM helpers, auto-cleanup
// timers, storage access, and wraps PositronBunchModule -> PositronModule.
//
// Template variables replaced by the build tool:
//   __BUNCH_NAME__    -> bunch.json "name"
//   __BUNCH_ENTRY__   -> bunch.json "entry" (without extension)
//
// After this block, the build tool appends:
//   _vfs.write("path", "content");  // for each bundled file

(function() {
  // =========================================================================
  // VFS
  // =========================================================================
  function _b64(s) {
    // atob works in renderer; in Node use Buffer
    if (typeof atob === 'function') return atob(s);
    return Buffer.from(s, 'base64').toString('utf8');
  }
  var _files = {};

  var _vfs = {
    read: function(p) { return _files[p] !== undefined ? _files[p] : null; },
    write: function(p, d) { _files[p] = d; return d; },
    list: function(prefix) {
      prefix = prefix || '';
      var out = [];
      for (var k in _files) {
        if (_files.hasOwnProperty(k) && k.indexOf(prefix) === 0) out.push(k);
      }
      return out;
    },
    fullsize: function() {
      var n = 0;
      for (var k in _files) { if (_files.hasOwnProperty(k)) n += _files[k].length; }
      return n;
    },
    filesize: function(p) { return _files[p] ? _files[p].length : 0; },

    exec: null,          // patched below after api is available
    execRenderer: null
  };

  // =========================================================================
  // VFS file entries (appended by build tool)
  // =========================================================================

  // __BUNCH_VFS_ENTRIES__

  // =========================================================================
  // Module wrapper
  // =========================================================================
  return {
    name: '__BUNCH_NAME__',

    onLoad: function(api) {
      // --- patch vfs.exec / vfs.execRenderer ---
      _vfs.exec = function(p) {
        var code = _vfs.read(p);
        if (code === null) return Promise.reject(new Error('vfs: file not found: ' + p));
        return api.eval(code);
      };
      _vfs.execRenderer = function(p, idx) {
        var code = _vfs.read(p);
        if (code === null) return Promise.reject(new Error('vfs: file not found: ' + p));
        return api.evalRenderer(code, idx || 0);
      };

      // --- dom helpers (all operate on renderer[0] by default) ---
      var _domEval = function(code, idx) { return api.evalRenderer(code, idx || 0); };
      function _esc(s) { return s.replace(/\\/g,'\\\\').replace(/"/g,'\\"').replace(/\n/g,'\\n').replace(/\r/g,'\\r'); }
      function _sel(s) { return s.replace(/"/g,'\\"'); }

      var dom = {
        // --- query ---
        query: function(sel, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(!e)return null;return{text:e.textContent,html:e.innerHTML,' +
            'tag:e.tagName.toLowerCase(),id:e.id,className:e.className,' +
            'attrs:(function(){var o={};for(var i=0;i<e.attributes.length;i++)o[e.attributes[i].name]=e.attributes[i].value;return o})()};})()', idx);
        },
        queryAll: function(sel, idx) {
          return _domEval(
            '(function(){var els=document.querySelectorAll("'+_sel(sel)+'");' +
            'var r=[];for(var i=0;i<els.length;i++){var e=els[i];' +
            'r.push({text:e.textContent,tag:e.tagName.toLowerCase(),id:e.id,className:e.className});}return r;})()', idx);
        },

        // --- modify ---
        setText: function(sel, text, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.textContent="'+_esc(text)+'";return !!e;})()', idx);
        },
        setHTML: function(sel, html, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.innerHTML="'+_esc(html)+'";return !!e;})()', idx);
        },
        setAttr: function(sel, attr, val, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.setAttribute("'+_esc(attr)+'","'+_esc(val)+'");return !!e;})()', idx);
        },
        removeAttr: function(sel, attr, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.removeAttribute("'+_esc(attr)+'");return !!e;})()', idx);
        },
        setStyle: function(sel, prop, val, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.style["'+_esc(prop)+'"]="'+_esc(val)+'";return !!e;})()', idx);
        },
        addClass: function(sel, cls, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.classList.add("'+cls+'");return !!e;})()', idx);
        },
        removeClass: function(sel, cls, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.classList.remove("'+cls+'");return !!e;})()', idx);
        },
        toggleClass: function(sel, cls, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.classList.toggle("'+cls+'");return !!e;})()', idx);
        },
        hide: function(sel, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.style.display="none";return !!e;})()', idx);
        },
        show: function(sel, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.style.display="";return !!e;})()', idx);
        },
        remove: function(sel, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e)e.remove();return !!e;})()', idx);
        },
        setVal: function(sel, val, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(e){e.value="'+_esc(val)+'";e.dispatchEvent(new Event("input",{bubbles:true}));}return !!e;})()', idx);
        },

        // --- inject ---
        append: function(parentSel, tag, html, attrs, idx) {
          var attrStr = '';
          if (attrs) { for (var k in attrs) { if (attrs.hasOwnProperty(k)) attrStr += 'el.setAttribute("'+_esc(k)+'","'+_esc(attrs[k])+'");'; } }
          return _domEval(
            '(function(){var p=document.querySelector("'+_sel(parentSel)+'");' +
            'if(!p)return null;var el=document.createElement("'+_esc(tag)+'");' +
            'el.innerHTML="'+_esc(html||'')+'";'+attrStr+
            'p.appendChild(el);return el.outerHTML;})()', idx);
        },
        prepend: function(parentSel, tag, html, attrs, idx) {
          var attrStr = '';
          if (attrs) { for (var k in attrs) { if (attrs.hasOwnProperty(k)) attrStr += 'el.setAttribute("'+_esc(k)+'","'+_esc(attrs[k])+'");'; } }
          return _domEval(
            '(function(){var p=document.querySelector("'+_sel(parentSel)+'");' +
            'if(!p)return null;var el=document.createElement("'+_esc(tag)+'");' +
            'el.innerHTML="'+_esc(html||'')+'";'+attrStr+
            'p.insertBefore(el,p.firstChild);return el.outerHTML;})()', idx);
        },
        insertHTML: function(sel, position, html, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(!e)return false;e.insertAdjacentHTML("'+_esc(position)+'","'+_esc(html)+'");return true;})()', idx);
        },
        injectHTMLFile: function(sel, position, vfsPath, idx) {
          var html = _vfs.read(vfsPath);
          if (!html) return Promise.reject(new Error('vfs: file not found: ' + vfsPath));
          return dom.insertHTML(sel, position, html, idx);
        },
        replaceWith: function(sel, html, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(!e)return false;e.outerHTML="'+_esc(html)+'";return true;})()', idx);
        },

        // --- inject CSS/JS ---
        injectCSS: function(css, idx) {
          var id = '__bunch_css_' + Math.random().toString(36).substring(2, 8);
          return _domEval(
            '(function(){var s=document.createElement("style");s.id="'+id+'";' +
            's.textContent="'+_esc(css)+'";' +
            'document.head.appendChild(s);return "'+id+'";})()', idx);
        },
        injectCSSFile: function(vfsPath, idx) {
          var css = _vfs.read(vfsPath);
          if (!css) return Promise.reject(new Error('vfs: css not found: ' + vfsPath));
          return dom.injectCSS(css, idx);
        },
        removeCSS: function(id, idx) {
          return _domEval(
            '(function(){var s=document.getElementById("'+_esc(id)+'");' +
            'if(s)s.remove();return !!s;})()', idx);
        },
        injectScript: function(code, idx) {
          var id = '__bunch_js_' + Math.random().toString(36).substring(2, 8);
          return _domEval(
            '(function(){var s=document.createElement("script");s.id="'+id+'";' +
            's.textContent="'+_esc(code)+'";' +
            'document.body.appendChild(s);return "'+id+'";})()', idx);
        },
        injectScriptFile: function(vfsPath, idx) {
          var code = _vfs.read(vfsPath);
          if (!code) return Promise.reject(new Error('vfs: file not found: ' + vfsPath));
          return dom.injectScript(code, idx);
        },

        // --- events ---
        onClick: function(sel, handlerCode, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(!e)return false;e.addEventListener("click",function(ev){'+handlerCode+'});return true;})()', idx);
        },
        on: function(sel, event, handlerCode, idx) {
          return _domEval(
            '(function(){var e=document.querySelector("'+_sel(sel)+'");' +
            'if(!e)return false;e.addEventListener("'+_esc(event)+'",function(ev){'+handlerCode+'});return true;})()', idx);
        },

        // --- wait / observe ---
        waitFor: function(sel, timeoutMs, idx) {
          timeoutMs = timeoutMs || 5000;
          return _domEval(
            '(function(){return new Promise(function(resolve){' +
            'var el=document.querySelector("'+_sel(sel)+'");' +
            'if(el){resolve(true);return;}' +
            'var obs=new MutationObserver(function(){' +
            '  if(document.querySelector("'+_sel(sel)+'")){obs.disconnect();resolve(true);}' +
            '});' +
            'obs.observe(document.body,{childList:true,subtree:true});' +
            'setTimeout(function(){obs.disconnect();resolve(false);},'+timeoutMs+');' +
            '});})()', idx);
        },

        // --- read page info ---
        getTitle: function(idx) {
          return _domEval('document.title', idx);
        },
        getURL: function(idx) {
          return _domEval('location.href', idx);
        },
        getHTML: function(idx) {
          return _domEval('document.documentElement.outerHTML', idx);
        },
        getBounds: function(sel, idx) {
          return _domEval(
            '(function(){var e='+(sel?'document.querySelector("'+_sel(sel)+'")':'document.body')+';' +
            'if(!e)return null;var r=e.getBoundingClientRect();' +
            'return{x:r.x,y:r.y,w:r.width,h:r.height,top:r.top,left:r.left};})()', idx);
        },

        // capturePage screenshot — uses Electron API directly (no eval)
        screenshot: function(opts) {
          opts = opts || {};
          var winIdx = opts.windowIndex || 0;
          var format = opts.format || 'png';
          var quality = opts.quality || 80;
          var saveTo = opts.saveTo || null;
          var rect = opts.rect || null;

          var el = api.getElectron();
          if (!el || !el.BrowserWindow) return Promise.reject(new Error('no electron'));
          var wins = el.BrowserWindow.getAllWindows();
          var w = wins[winIdx];
          if (!w) return Promise.reject(new Error('no window at index ' + winIdx));

          var p = rect ? w.webContents.capturePage(rect) : w.webContents.capturePage();
          return p.then(function(img) {
            var buf = format === 'jpeg' ? img.toJPEG(quality) : img.toPNG();
            var result = {
              size: buf.length,
              width: img.getSize().width,
              height: img.getSize().height,
              format: format,
              savedTo: null,
              base64: null
            };
            if (saveTo) {
              api.fs.write(saveTo, buf);
              result.savedTo = saveTo;
            } else {
              result.base64 = buf.toString('base64');
            }
            return result;
          });
        }
      };
      api.dom = dom;

      // --- timer with auto-cleanup ---
      var _timers = { intervals: [], timeouts: [] };
      var timer = {
        interval: function(fn, ms) {
          var id = setInterval(fn, ms);
          _timers.intervals.push(id);
          return id;
        },
        timeout: function(fn, ms) {
          var id = setTimeout(fn, ms);
          _timers.timeouts.push(id);
          return id;
        },
        clear: function(id) {
          clearInterval(id); clearTimeout(id);
          _timers.intervals = _timers.intervals.filter(function(x){return x!==id;});
          _timers.timeouts = _timers.timeouts.filter(function(x){return x!==id;});
        }
      };
      api.timer = timer;

      // --- store (renderer localStorage / sessionStorage) ---
      var _storeEval = function(code, idx) { return api.evalRenderer(code, idx || 0); };
      function makeStore(storageName) {
        return {
          get: function(key, idx) {
            return _storeEval('(function(){var v=' + storageName + '.getItem("' + key + '");return v;})()', idx);
          },
          set: function(key, val, idx) {
            return _storeEval(storageName + '.setItem("' + key.replace(/"/g, '\\"') + '","' + val.replace(/"/g, '\\"') + '")', idx);
          },
          keys: function(idx) {
            return _storeEval('(function(){var a=[];for(var i=0;i<' + storageName + '.length;i++)a.push(' + storageName + '.key(i));return a;})()', idx);
          },
          remove: function(key, idx) {
            return _storeEval(storageName + '.removeItem("' + key.replace(/"/g, '\\"') + '")', idx);
          }
        };
      }
      api.store = {
        local: makeStore('localStorage'),
        session: makeStore('sessionStorage')
      };

      // --- vfs ref on api ---
      api.vfs = _vfs;

      // --- load the entry module ---
      var entryCode = _vfs.read('__BUNCH_ENTRY__');
      if (!entryCode) throw new Error('bunch entry not found: __BUNCH_ENTRY__');
      // Entry declares `var BunchModule = {...}`. Wrap in IIFE to capture it.
      var _entryMod = (0, eval)('(function(){' + entryCode + ';return typeof BunchModule!=="undefined"?BunchModule:null;})()');
      if (!_entryMod) throw new Error('bunch entry did not declare BunchModule');
      api.__bunchEntry = _entryMod;
      api.__bunchTimers = _timers;

      if (_entryMod && typeof _entryMod.onLoad === 'function') {
        return _entryMod.onLoad(api);
      }
    },

    onUnload: function(api) {
      // auto-cleanup timers
      var t = api.__bunchTimers;
      if (t) {
        t.intervals.forEach(function(id) { clearInterval(id); });
        t.timeouts.forEach(function(id) { clearTimeout(id); });
      }
      var entry = api.__bunchEntry;
      if (entry && typeof entry.onUnload === 'function') {
        return entry.onUnload(api);
      }
    }
  };
})()
