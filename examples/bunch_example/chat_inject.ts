(function() {
  try {
  var chatArea = document.getElementById('__po_chat_area') as HTMLElement;
  var chatInput = document.getElementById('__po_chat_input') as HTMLInputElement;
  var sendBtn = document.getElementById('__po_chat_send_btn') as HTMLElement;
  if (!chatArea) { console.error('[chat_inject] no chat area'); (window as any).__po_chat_inject_err = 'no chat area'; return; }
  if (!sendBtn) { console.error('[chat_inject] no send btn'); (window as any).__po_chat_inject_err = 'no send btn (custom input hidden?)'; }
  (window as any).__po_chat_inject_ok = true;

  function escHtml(s: string): string {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function addMsg(role: string, html: string): HTMLElement {
    var div = document.createElement('div');
    div.className = 'po-msg po-msg-' + role;
    div.innerHTML = html;
    chatArea.appendChild(div);
    chatArea.scrollTop = chatArea.scrollHeight;
    return div;
  }

  // --- streaming ---
  var currentAiEl: HTMLElement | null = null;
  (window as any).__po_chat_stream = '';
  (window as any).__po_chat_done = false;

  setInterval(function() {
    var text = (window as any).__po_chat_stream as string;
    var done = (window as any).__po_chat_done as boolean;
    if (!text && !done) return;
    // Find the latest AI bubble (works for both custom input and quick mode)
    var target = currentAiEl;
    if (!target && chatArea) {
      var all = chatArea.querySelectorAll('.po-msg-ai');
      if (all.length > 0) target = all[all.length - 1] as HTMLElement;
    }
    if (text && target) {
      target.innerHTML = text;
      chatArea.scrollTop = chatArea.scrollHeight;
    }
    if (done) {
      (window as any).__po_chat_done = false;
      (window as any).__po_chat_stream = '';
      currentAiEl = null;
    }
  }, 200);

  // --- send ---
  function sendMessage() {
    var msg = chatInput.value.trim();
    // Collect pending images (set by main process directly on window)
    var imgs = (window as any).__po_pending_imgs || [];
    var images: string[] = [];
    for (var i = 0; i < imgs.length; i++) {
      if (imgs[i]) images.push(imgs[i]);
    }
    if (!msg && images.length === 0) return;
    chatInput.value = '';

    // Show user message with thumbnails
    var userHtml = '';
    if (images.length > 0) {
      userHtml += '<div style="display:flex;gap:3px;margin-bottom:3px">';
      for (var j = 0; j < images.length; j++) {
        userHtml += '<img src="data:image/jpeg;base64,' + images[j] + '" style="height:32px;border-radius:2px">';
      }
      userHtml += '</div>';
    }
    if (msg) userHtml += escHtml(msg);
    addMsg('user', userHtml);

    // Clear thumbs
    (window as any).__po_pending_imgs = [];
    var thumbs = document.getElementById('__po_chat_thumbs');
    if (thumbs) thumbs.innerHTML = '';

    if (currentAiEl) currentAiEl = null;
    currentAiEl = addMsg('ai', '<span class="po-typing">thinking...</span>');
    (window as any).__po_chat_stream = '';
    (window as any).__po_chat_done = false;

    // Signal main process
    (window as any).__po_chat_send = { text: msg, images: images };
  }

  if (sendBtn) {
    sendBtn.addEventListener('click', sendMessage);
  }
  if (chatInput) {
    chatInput.addEventListener('keydown', function(e: KeyboardEvent) {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        sendMessage();
      }
    });
  }
  } catch(e) { (window as any).__po_chat_inject_err = (e as any).message; console.error('[chat_inject]', e); }
})();
