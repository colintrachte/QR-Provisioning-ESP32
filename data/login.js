/**
 * login.js — Placeholder authentication page.
 *
 * API contract (implement on firmware side):
 *   POST /api/login → body: {username, password}
 *   Response: {ok: true, token: "jwt"} or {ok: false, error: "msg"}
 *
 * On success: stores token in sessionStorage (or localStorage if remember me)
 * and redirects to /.
 */

const API_LOGIN = '/api/login';

const elForm     = document.getElementById('login-form');
const elUser     = document.getElementById('username');
const elPass     = document.getElementById('password');
const elToggle   = document.getElementById('togglePw');
const elRemember = document.getElementById('remember');
const elStatus   = document.getElementById('login-status');

/* Password visibility toggle */
if (elToggle) {
  elToggle.addEventListener('click', () => {
    const isPass = elPass.type === 'password';
    elPass.type = isPass ? 'text' : 'password';
  });
}

/* Form submit */
elForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const user = elUser.value.trim();
  const pass = elPass.value;

  if (!user || !pass) {
    setStatus('Please enter both username and password.', 'error');
    return;
  }

  setStatus('Authenticating…', '');

  try {
    const r = await fetch(API_LOGIN, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: user, password: pass }),
    });

    let data = {};
    try { data = await r.json(); } catch (_) {}

    if (!r.ok || !data.ok) {
      setStatus(data.error || 'Invalid credentials.', 'error');
      return;
    }

    // Store token
    const storage = elRemember.checked ? localStorage : sessionStorage;
    storage.setItem('robot_auth_token', data.token || '');

    setStatus('Success — redirecting…', 'ok');
    setTimeout(() => { window.location.href = '/'; }, 400);

  } catch (err) {
    setStatus('Network error. Is the robot online?', 'error');
  }
});

function setStatus(msg, type) {
  elStatus.textContent = msg;
  elStatus.className = 'login-status' + (type ? ' ' + type : '');
}

/* If already authenticated, skip login (optional UX improvement) */
const existing = sessionStorage.getItem('robot_auth_token') || localStorage.getItem('robot_auth_token');
if (existing) {
  // Optionally verify token validity here before redirecting
  // window.location.href = '/';
}
