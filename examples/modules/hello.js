// Example positron module: hello
//
// Usage from REPL:
//   .mod load examples\modules\hello.js
//   .mod list
//   .mod unload hello
//
// Demonstrates the module API:
//   - onLoad / onUnload lifecycle
//   - api.eval() to run code in main process
//   - api.evalRenderer() to run code in renderer
//   - api.send() to push events back to host
//   - api.log() for diagnostics
//   - api.getElectron() to access Electron APIs

(function() {
  var timer = null;

  return {
    name: 'hello',

    onLoad: function(api) {
      api.log('hello module loaded!');

      // Demonstrate: read some process info and send it back
      api.eval('({pid: process.pid, uptime: process.uptime()})').then(function(info) {
        api.send({ type: 'process-info', info: info });
      });

      // Demonstrate: periodic heartbeat every 5 seconds
      timer = setInterval(function() {
        api.eval('process.memoryUsage().heapUsed').then(function(heap) {
          api.send({ type: 'heartbeat', heapUsed: heap });
        });
      }, 5000);

      api.log('heartbeat timer armed (5s interval)');
    },

    onUnload: function() {
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
      // api is no longer available here, just clean up local state
    }
  };
})()
