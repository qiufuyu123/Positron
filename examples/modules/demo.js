// positron module: demo
//
// One-shot DOM manipulation demo. Changes title, badge, adds log entries.
// No periodic polling — safe and stable.
//
// Usage:
//   .mod load examples\modules\demo.js
//   .mod unload demo

(function() {
  return {
    name: 'demo',

    onLoad: function(api) {
      api.evalRenderer(
        '(function(){' +
        '  var t = document.getElementById("title");' +
        '  if(t) t.textContent = "Positron Injected!";' +
        '  var s = document.getElementById("subtitle");' +
        '  if(s) s.textContent = "Live JS injection active \\u2014 module system demo";' +
        '  var b = document.getElementById("status-badge");' +
        '  if(b) { b.textContent = "INJECTED"; b.className = "badge badge-ok"; }' +
        '  var msgs = [' +
        '    "Module loaded successfully",' +
        '    "Scanning renderer DOM tree...",' +
        '    "Intercepting network requests...",' +
        '    "Hooking IPC channels...",' +
        '    "Monitoring target process..."' +
        '  ];' +
        '  msgs.forEach(function(m,i){' +
        '    setTimeout(function(){ if(window.__addLog) window.__addLog(m); }, i * 800);' +
        '  });' +
        '  return "demo injected";' +
        '})()', 0
      ).then(function(r) { api.log(r); });
    },

    onUnload: function(api) {
      return api.evalRenderer(
        '(function(){' +
        '  var t = document.getElementById("title");' +
        '  if(t) t.textContent = "Positron Demo";' +
        '  var s = document.getElementById("subtitle");' +
        '  if(s) s.textContent = "Electron Process Injection Toolkit";' +
        '  var b = document.getElementById("status-badge");' +
        '  if(b) { b.textContent = "IDLE"; b.className = "badge badge-idle"; }' +
        '  if(window.__addLog) window.__addLog("Module unloaded.");' +
        '  return "restored";' +
        '})()', 0
      ).then(function(r) { api.log("cleanup: " + r); });
    }
  };
})()
