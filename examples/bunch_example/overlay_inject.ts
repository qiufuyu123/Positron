(function() {
  var overlay = document.getElementById('__positron_overlay') as HTMLElement;
  if (!overlay) return;

  var viewDefault = document.getElementById('__po_view_default') as HTMLElement;
  var viewMenu = document.getElementById('__po_view_menu') as HTMLElement;
  var inMenu = false;

  // --- opacity slider ---
  var slider = document.getElementById('__po_opacity') as HTMLInputElement;
  var label = document.getElementById('__po_opacity_val') as HTMLElement;
  if (slider) {
    slider.addEventListener('input', function() {
      var v = parseInt(slider.value);
      overlay.style.opacity = String(v / 100);
      if (label) label.textContent = v + '%';
    });
  }

  // --- drag ---
  var dragHandle = document.getElementById('__po_drag') as HTMLElement;
  if (dragHandle) {
    var dragging = false, dx = 0, dy = 0;
    dragHandle.addEventListener('mousedown', function(e: MouseEvent) {
      dragging = true;
      dx = e.clientX - overlay.offsetLeft;
      dy = e.clientY - overlay.offsetTop;
      e.preventDefault();
    });
    document.addEventListener('mousemove', function(e: MouseEvent) {
      if (!dragging) return;
      overlay.style.left = (e.clientX - dx) + 'px';
      overlay.style.top = (e.clientY - dy) + 'px';
    });
    document.addEventListener('mouseup', function() {
      dragging = false;
      resizing = false;
    });
  }

  // --- snap buttons (both quick mode and custom input mode) ---
  var snapBtns = document.querySelectorAll('.po-snap-icon');
  for (var si = 0; si < snapBtns.length; si++) {
    (function(btn) {
      btn.addEventListener('click', function(e: Event) {
        e.stopPropagation();
        (window as any).__po_snap_request = true;
        // Dim all snap icons
        for (var sj = 0; sj < snapBtns.length; sj++) (snapBtns[sj] as HTMLElement).style.opacity = '0.3';
      });
    })(snapBtns[si]);
  }

  // --- resize ---
  var resizeHandle = document.getElementById('__po_resize') as HTMLElement;
  var resizing = false, rStartX = 0, rStartY = 0, rStartW = 0, rStartH = 0;
  if (resizeHandle) {
    resizeHandle.addEventListener('mousedown', function(e: MouseEvent) {
      resizing = true;
      rStartX = e.clientX;
      rStartY = e.clientY;
      rStartW = overlay.offsetWidth;
      rStartH = overlay.offsetHeight;
      e.preventDefault();
      e.stopPropagation();
    });
    document.addEventListener('mousemove', function(e: MouseEvent) {
      if (!resizing) return;
      var w = Math.max(200, rStartW + (e.clientX - rStartX));
      var h = Math.max(150, rStartH + (e.clientY - rStartY));
      overlay.style.width = w + 'px';
      overlay.style.height = h + 'px';
    });
  }

  // --- menu buttons ---
  var btnClear = document.getElementById('__po_btn_clear_license');
  var btnReverify = document.getElementById('__po_btn_reverify');
  var menuStatus = document.getElementById('__po_menu_status');

  var clearStep = 0;
  var clearTimer: any = null;
  if (btnClear) {
    btnClear.addEventListener('click', function() {
      if (clearStep === 0) {
        clearStep = 1;
        btnClear!.textContent = 'Click again...';
        if (clearTimer) clearTimeout(clearTimer);
        clearTimer = setTimeout(function() { clearStep = 0; btnClear!.textContent = 'Clear license.dat'; }, 3000);
      } else if (clearStep === 1) {
        clearStep = 0;
        if (clearTimer) { clearTimeout(clearTimer); clearTimer = null; }
        btnClear!.textContent = 'Clear license.dat';
        if (confirm('Clear license.dat? You will need to re-enter your token.')) {
          (window as any).__po_action = 'clear_license';
          if (menuStatus) menuStatus.textContent = 'clearing...';
        }
      }
    });
  }
  if (btnReverify) {
    btnReverify.addEventListener('click', function() {
      (window as any).__po_action = 'reverify';
      if (menuStatus) menuStatus.textContent = 'verifying...';
    });
  }

  // --- settings checkboxes ---
  var chkAutoClear = document.getElementById('__po_chk_autoclear') as HTMLInputElement;
  var chkCustomInput = document.getElementById('__po_chk_custominput') as HTMLInputElement;
  var inputCustom = document.getElementById('__po_input_custom') as HTMLElement;
  var inputQuick = document.getElementById('__po_input_quick') as HTMLElement;

  // Expose settings on window for main process to read
  (window as any).__po_settings = { autoClear: true, customInput: false };

  // --- model cycle button ---
  var modelBtn = document.getElementById('__po_btn_model') as HTMLElement;
  var modelList = [
    { id: 'google/gemini-3.1-pro-preview', label: 'Gemini 3.1 Pro' },
    { id: 'anthropic/claude-opus-4.7', label: 'Claude Opus 4.7' },
    { id: 'openai/gpt-5.5-pro', label: 'GPT 5.5 Pro' }
  ];
  var modelIdx = 0;
  if (modelBtn) {
    modelBtn.addEventListener('click', function() {
      modelIdx = (modelIdx + 1) % modelList.length;
      modelBtn.textContent = 'Model: ' + modelList[modelIdx].label;
      (window as any).__po_model_change = modelList[modelIdx].id;
    });
  }

  function applyInputMode() {
    var custom = chkCustomInput && chkCustomInput.checked;
    (window as any).__po_settings.customInput = custom;
    if (inputCustom) inputCustom.style.display = custom ? 'flex' : 'none';
    if (inputQuick) inputQuick.style.display = custom ? 'none' : 'flex';
  }

  if (chkAutoClear) {
    chkAutoClear.addEventListener('change', function() {
      (window as any).__po_settings.autoClear = chkAutoClear.checked;
    });
  }
  if (chkCustomInput) {
    chkCustomInput.addEventListener('change', applyInputMode);
  }
  applyInputMode();

  // --- quick buttons ---
  var btnBrief = document.getElementById('__po_btn_brief');
  var btnDetail = document.getElementById('__po_btn_detail');

  // Quick buttons: snap screenshot → auto-send preset prompt
  // After snap completes, __po_snap_pending_prompt triggers a send
  if (btnBrief) {
    btnBrief.addEventListener('click', function() {
      (window as any).__po_snap_pending_prompt = 'Tell me what the answer is to this question';
      (window as any).__po_snap_request = true;
      if (snapBtn2) snapBtn2.style.opacity = '0.3';
    });
  }
  if (btnDetail) {
    btnDetail.addEventListener('click', function() {
      (window as any).__po_snap_pending_prompt = 'Tell me the complete solution process and answer for this question';
      (window as any).__po_snap_request = true;
      if (snapBtn2) snapBtn2.style.opacity = '0.3';
    });
  }

  function toggleMenu() {
    inMenu = !inMenu;
    if (viewDefault) viewDefault.style.display = inMenu ? 'none' : 'flex';
    if (viewMenu) viewMenu.style.display = inMenu ? 'block' : 'none';
    if (!inMenu && menuStatus) menuStatus.textContent = '';
  }

  // --- keyboard: double-C (toggle overlay), double-X (toggle menu) ---
  var mouseX = 0, mouseY = 0;
  document.addEventListener('mousemove', function(e) {
    mouseX = e.clientX;
    mouseY = e.clientY;
  });

  var lastZ = 0, lastX = 0;
  document.addEventListener('keydown', function(e) {
    var now = Date.now();

    // double-Z: toggle overlay visibility
    if (e.key === 'c' || e.key === 'C') {
      if (now - lastZ < 350) {
        lastZ = 0;
        if (overlay.classList.contains('visible')) {
          overlay.classList.remove('visible');
          // reset to default view when hiding
          if (inMenu) toggleMenu();
        } else {
          var x = mouseX + 10;
          var y = mouseY + 10;
          if (x + 280 > window.innerWidth) x = window.innerWidth - 290;
          if (y + 250 > window.innerHeight) y = window.innerHeight - 260;
          if (x < 0) x = 10;
          if (y < 0) y = 10;
          overlay.style.left = x + 'px';
          overlay.style.top = y + 'px';
          overlay.classList.add('visible');
          try {
            var el = document.getElementById('__po_url');
            if (el) el.textContent = location.href.substring(0, 35);
            var el2 = document.getElementById('__po_title');
            if (el2) el2.textContent = document.title.substring(0, 25);
          } catch(err) {}
        }
      } else {
        lastZ = now;
      }
    }

    // double-X: toggle menu (only when overlay is visible)
    if (e.key === 'x' || e.key === 'X') {
      if (now - lastX < 350) {
        lastX = 0;
        if (overlay.classList.contains('visible')) {
          toggleMenu();
        }
      } else {
        lastX = now;
      }
    }
  });
})();
