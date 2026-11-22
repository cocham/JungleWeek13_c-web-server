const list = document.querySelector('#messages');
const statusMessage = document.querySelector('#messages-status');
const form = document.querySelector('#message-form');
const messageInput = document.querySelector('#message');
const submitButton = form.querySelector('button');
const submitStatus = document.querySelector('#submit-status');
const encoder = new TextEncoder();
let listVersion = 0;

async function loadMessages() {
  const version = ++listVersion;
  try {
    const response = await fetch('/api/messages', { cache: 'no-store' });
    if (!response.ok) {
      throw new Error('Could not load messages');
    }
    const { messages } = await response.json();
    if (version !== listVersion) {
      return;
    }
    const entries = messages.map((message) => {
      const entry = document.createElement('li');
      entry.textContent = message;
      return entry;
    });
    list.replaceChildren(...entries);
    statusMessage.classList.remove('is-error');
    statusMessage.textContent = entries.length === 0 ? '아직 남긴 글이 없습니다.' : '';
    statusMessage.hidden = entries.length > 0;
  } catch {
    if (version !== listVersion) {
      return;
    }
    statusMessage.hidden = false;
    statusMessage.classList.add('is-error');
    statusMessage.textContent = '글 목록을 불러오지 못했습니다.';
  }
}

function showSubmitStatus(message, isError = false) {
  submitStatus.hidden = false;
  submitStatus.classList.toggle('is-error', isError);
  submitStatus.textContent = message;
}

async function submitMessage(event) {
  event.preventDefault();
  if (submitButton.disabled) {
    return;
  }

  const message = messageInput.value.trim();
  if (!message) {
    showSubmitStatus('남길 글을 입력해 주세요.', true);
    return;
  }
  if (encoder.encode(message).length > 240) {
    showSubmitStatus('글은 UTF-8 240바이트까지 등록할 수 있습니다.', true);
    return;
  }

  submitButton.disabled = true;
  messageInput.readOnly = true;
  showSubmitStatus('등록 중입니다.');
  try {
    const response = await fetch('/api/messages', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: message,
    });
    if (response.status === 201) {
      messageInput.value = '';
      messageInput.focus();
      showSubmitStatus('글을 등록했습니다.');
      await loadMessages();
    } else if (response.status === 409) {
      showSubmitStatus('방명록이 가득 찼습니다.', true);
    } else if (response.status === 504) {
      showSubmitStatus('등록 응답이 늦어졌습니다. 다시 등록하기 전에 글 목록을 확인해 주세요.', true);
    } else {
      showSubmitStatus('글을 등록하지 못했습니다.', true);
    }
  } catch {
    showSubmitStatus('글을 등록하지 못했습니다.', true);
  } finally {
    submitButton.disabled = false;
    messageInput.readOnly = false;
  }
}

form.addEventListener('submit', submitMessage);
loadMessages();
