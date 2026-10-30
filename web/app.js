(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const tabNames = { overview: 'Обзор', performance: 'Производительность', input: 'Управление', diagnostics: 'Диагностика' };
  const presetNames = { balanced: 'Сбалансированный', performance: 'Производительность', quality: 'Качество изображения', custom: 'Свои настройки' };
  const defaults = { preset: 'balanced', width: 1600, height: 900, target_fps: 120, cpu_cores: 4, memory_mb: 4096, dpi: 800, sensitivity: 1, cm360: 35, raw_input: true };
  let state = null;
  let settings = { ...defaults };
  let diagnostics = null;
  let demoRunning = false;
  let benchmarkRunning = false;
  let toastTimer;
  let activeTab = 'overview';
  let yaw = 0;
  let pitch = 0;
  let movementCount = 0;
  let absoluteX = 0;
  let drawPending = false;
  let pointerMode = 'Режим ввода определяется при захвате';
  let pointerRequestPending = false;
  const canvas = $('aim-canvas');
  const context = canvas.getContext('2d');

  const number = (value, fallback = 0) => Number.isFinite(Number(value)) ? Number(value) : fallback;
  const fixed = (value, digits = 2) => number(value).toLocaleString('ru-RU', { minimumFractionDigits: digits, maximumFractionDigits: digits });
  const milliseconds = (value) => `${fixed(value, 3)} ms`;
  const setText = (id, text) => { $(id).textContent = String(text); };
  const safeError = (error) => error instanceof Error ? error.message : 'Неизвестная ошибка';

  async function api(path, body) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch(path, {
        method: body === undefined ? 'GET' : 'POST',
        credentials: 'same-origin',
        cache: 'no-store',
        headers: body === undefined ? { Accept: 'application/json' } : { Accept: 'application/json', 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: controller.signal,
      });
      const text = await response.text();
      let payload;
      try { payload = JSON.parse(text); }
      catch { throw new Error(`Ядро вернуло некорректный ответ (HTTP ${response.status}).`); }
      if (!response.ok) throw new Error(typeof payload.error === 'string' ? payload.error : `Запрос завершился ошибкой HTTP ${response.status}.`);
      return payload;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('Ядро не ответило за 15 секунд. Проверьте процесс Sifa и обновите состояние.');
      if (error instanceof TypeError) throw new Error('Не удалось связаться с ядром. Убедитесь, что Sifa запущен, и обновите страницу.');
      throw error;
    } finally { clearTimeout(timeout); }
  }

  function showError(error) {
    const box = $('global-error');
    box.textContent = safeError(error);
    box.hidden = false;
  }

  function clearError() { $('global-error').hidden = true; }

  function toast(message) {
    clearTimeout(toastTimer);
    setText('toast', message);
    $('toast').hidden = false;
    toastTimer = setTimeout(() => { $('toast').hidden = true; }, 3500);
  }

  function connected(value) {
    $('connection').classList.toggle('online', value);
    $('connection').classList.toggle('offline', !value);
    setText('connection-text', value ? 'Ядро подключено' : 'Нет связи с ядром');
    for (const id of ['run-demo', 'run-benchmark', 'download-diagnostics']) $(id).disabled = !value;
    for (const form of [$('performance-form'), $('input-form')]) form.querySelector('button[type=submit]').disabled = !value;
  }

  function updateProfile() {
    setText('profile-name', presetNames[settings.preset] || 'Свои настройки');
    setText('profile-resolution', `${settings.width} × ${settings.height}`);
    setText('profile-fps', `${settings.target_fps} FPS`);
    setText('profile-cpu', settings.cpu_cores);
    setText('profile-memory', `${fixed(number(settings.memory_mb) / 1024, 1)} ГБ`);
  }

  function syncSettings() {
    $('preset').value = settings.preset in presetNames ? settings.preset : 'custom';
    $('width').value = settings.width;
    $('height').value = settings.height;
    $('target-fps').value = settings.target_fps;
    $('cpu-cores').value = settings.cpu_cores;
    $('guest-memory').value = settings.memory_mb;
    $('dpi').value = settings.dpi;
    $('cm360').value = settings.cm360;
    $('sensitivity').value = settings.sensitivity;
    $('raw-input').checked = Boolean(settings.raw_input);
    updateRanges();
    updateProfile();
  }

  function setHostLimits(host) {
    const cpus = Math.max(1, number(host.logical_cpus, 1));
    $('cpu-cores').max = Math.min(16, cpus > 2 ? cpus - 1 : cpus);
    const ram = number(host.memory_mb);
    const ramBudget = ram > 0 ? Math.min(16384, Math.max(1, ram - Math.min(2048, Math.floor(ram / 2)))) : 1024;
    $('guest-memory').max = ramBudget;
    $('guest-memory').min = Math.min(256, ramBudget);
  }

  function renderState(next, updateForms = true) {
    if (!next || !next.host || !next.hypervisor || !next.settings) throw new Error('Ответ ядра не содержит состояния системы.');
    state = next;
    settings = { ...defaults, ...next.settings };
    $('config-warning').hidden = !next.config_warning;
    if (next.config_warning) setText('config-warning', `Предупреждение о сохранённом профиле: ${next.config_warning}`);
    const host = next.host;
    const hypervisor = next.hypervisor;
    setText('version', next.version || 'preview');
    setText('cpu-value', host.logical_cpus || '—');
    setText('cpu-detail', [host.arch, host.os].filter(Boolean).join(' · ') || 'Архитектура неизвестна');
    setText('memory-value', number(host.memory_mb) > 0 ? fixed(number(host.memory_mb) / 1024, 1) : '—');
    setText('hypervisor-value', hypervisor.available ? String(hypervisor.backend).toUpperCase() : 'Программная');
    setText('hypervisor-detail', hypervisor.available ? 'Аппаратный бэкенд доступен' : 'Аппаратный бэкенд недоступен');
    $('hypervisor-detail').title = hypervisor.detail || '';
    setText('diag-os', host.os || 'Не определена');
    setText('diag-arch', host.arch || 'Не определена');
    setText('diag-backend', hypervisor.backend || 'Нет');
    setText('diag-available', hypervisor.available ? 'Доступен' : 'Недоступен');
    setText('diag-detail', hypervisor.detail || 'Дополнительные сведения отсутствуют.');
    setText('footer-os', String(host.os || 'LOCAL').toUpperCase());
    setHostLimits(host);
    if (updateForms) syncSettings(); else updateProfile();
    if (next.last_run) renderRun(next.last_run);
    connected(true);
  }

  async function refreshState() {
    $('refresh-state').disabled = true;
    try {
      clearError();
      renderState(await api('/api/state'));
      if (activeTab === 'diagnostics') await loadDiagnostics();
    } catch (error) { connected(false); showError(error); }
    finally { $('refresh-state').disabled = false; }
  }

  function navigate(requested) {
    const tab = Object.hasOwn(tabNames, requested) ? requested : 'overview';
    activeTab = tab;
    for (const [key] of Object.entries(tabNames)) {
      const page = $(`page-${key}`);
      page.hidden = key !== tab;
      page.classList.toggle('active', key === tab);
    }
    document.querySelectorAll('[data-tab]').forEach((link) => {
      const active = link.dataset.tab === tab;
      link.classList.toggle('active', active);
      if (active) link.setAttribute('aria-current', 'page'); else link.removeAttribute('aria-current');
    });
    setText('page-name', tabNames[tab]);
    document.title = `Sifa — ${tabNames[tab].toLowerCase()}`;
    if (tab === 'input') scheduleDraw();
    else if (document.pointerLockElement === canvas) document.exitPointerLock();
    if (tab === 'diagnostics' && state) loadDiagnostics().catch(showError);
  }

  function renderRun(result) {
    const status = $('vm-status');
    status.className = `pill ${result.ok ? 'pill-mint' : 'pill-error'}`;
    status.textContent = result.ok ? 'Тест завершён' : 'Ошибка запуска';
    setText('vm-timing', milliseconds(result.elapsed_ms));
    const count = number(result.instructions);
    setText('vm-instructions', count > 0 ? `${count.toLocaleString('ru-RU')} инструкций` : 'Счётчик недоступен');
    const lines = [`sifa > ${result.backend || 'engine'}`];
    if (result.output) lines.push(String(result.output).trimEnd());
    if (result.error) lines.push(`Ошибка: ${result.error}`);
    lines.push(result.ok ? '[HALT] Гость завершил выполнение.' : '[STOP] Тест не пройден.');
    setText('guest-console', lines.join('\n'));
  }

  async function runDemo() {
    if (demoRunning || !state) return;
    demoRunning = true;
    const button = $('run-demo');
    const label = button.querySelector('span');
    button.disabled = true;
    label.textContent = 'Выполняется тест…';
    $('vm-status').className = 'pill';
    setText('vm-status', 'Выполняется');
    setText('guest-console', 'sifa > Запуск изолированного тестового гостя…');
    try {
      clearError();
      const result = await api('/api/demo', { backend: $('backend').value });
      renderRun(result);
      state.last_run = result;
      toast(result.ok ? 'Тестовая виртуальная машина завершила работу.' : 'Тест не пройден. Подробности — в консоли.');
    } catch (error) {
      renderRun({ ok: false, backend: $('backend').value, error: safeError(error), elapsed_ms: 0, instructions: 0 });
      showError(error);
    } finally {
      demoRunning = false;
      button.disabled = false;
      label.textContent = 'Запустить тестовую ВМ';
    }
  }

  function performanceFields() {
    return {
      preset: $('preset').value,
      width: Number($('width').value), height: Number($('height').value),
      target_fps: Number($('target-fps').value), cpu_cores: Number($('cpu-cores').value),
      memory_mb: Number($('guest-memory').value),
    };
  }

  function inputFields() {
    return {
      dpi: Number($('dpi').value), sensitivity: Number($('sensitivity').value),
      cm360: Number($('cm360').value), raw_input: $('raw-input').checked,
    };
  }

  async function saveSettings(event, kind) {
    event.preventDefault();
    const form = event.currentTarget;
    if (!form.reportValidity() || !state) return;
    const button = form.querySelector('button[type=submit]');
    const status = $(`${kind}-status`);
    const next = { ...settings, ...(kind === 'performance' ? performanceFields() : inputFields()) };
    button.disabled = true;
    status.classList.remove('error');
    status.textContent = 'Сохраняем…';
    try {
      const payload = await api('/api/settings', next);
      settings = { ...defaults, ...(payload.settings || payload) };
      state.settings = { ...settings };
      state.config_warning = '';
      $('config-warning').hidden = true;
      syncSettings();
      status.textContent = 'Сохранено на этом ПК';
      if (kind === 'input') resetLab();
      toast(kind === 'performance' ? 'Профиль сохранён.' : 'Калибровка сохранена.');
    } catch (error) { status.classList.add('error'); status.textContent = safeError(error); }
    finally { button.disabled = false; }
  }

  function updateRanges() {
    setText('fps-output', `${$('target-fps').value}`);
    setText('sensitivity-output', `${fixed($('sensitivity').value, 2)}×`);
    const cm = Number($('cm360').value);
    const sensitivity = Number($('sensitivity').value);
    setText('effective-cm', cm > 0 && sensitivity > 0 ? fixed(cm / sensitivity, 1) : '—');
  }

  function applyPreset() {
    const preset = $('preset').value;
    const values = {
      performance: { width: 1280, height: 720, target_fps: 120, cpu_cores: 2, memory_mb: 2048 },
      balanced: { width: 1600, height: 900, target_fps: 120, cpu_cores: 4, memory_mb: 4096 },
      quality: { width: 1920, height: 1080, target_fps: 144, cpu_cores: 6, memory_mb: 6144 },
    }[preset];
    if (!values) return;
    $('width').value = values.width;
    $('height').value = values.height;
    $('target-fps').value = values.target_fps;
    $('cpu-cores').value = Math.min(values.cpu_cores, Number($('cpu-cores').max));
    $('guest-memory').value = Math.min(values.memory_mb, Number($('guest-memory').max));
    updateRanges();
    setText('performance-status', 'Изменения ещё не сохранены');
    $('performance-status').classList.remove('error');
  }

  async function runBenchmark() {
    if (benchmarkRunning || !state) return;
    benchmarkRunning = true;
    const button = $('run-benchmark');
    const label = button.querySelector('span');
    button.disabled = true;
    label.textContent = 'Измеряем…';
    try {
      clearError();
      const result = await api('/api/benchmark', {});
      if (result.ok === false || !Number.isFinite(result.mean_ms) || !(number(result.samples) > 0)) throw new Error(result.error || 'Ядро не вернуло результаты измерения.');
      setText('benchmark-value', fixed(result.mean_ms, 3));
      setText('benchmark-p50', milliseconds(result.p50_ms));
      setText('benchmark-p95', milliseconds(result.p95_ms));
      setText('benchmark-max', milliseconds(result.max_ms));
      setText('benchmark-samples', number(result.samples).toLocaleString('ru-RU'));
      setText('benchmark-scope', result.scope || 'Только выполнение тестового гостя. Не игровой FPS.');
      toast('Измерение завершено.');
    } catch (error) { showError(error); }
    finally { benchmarkRunning = false; button.disabled = false; label.textContent = 'Запустить измерение'; }
  }

  async function loadDiagnostics() {
    const report = await api('/api/diagnostics');
    diagnostics = report;
    setText('diagnostics-json', JSON.stringify(report, null, 2));
  }

  async function downloadDiagnostics() {
    const button = $('download-diagnostics');
    button.disabled = true;
    try {
      await loadDiagnostics();
      const blob = new Blob([JSON.stringify(diagnostics, null, 2) + '\n'], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const anchor = document.createElement('a');
      anchor.href = url;
      anchor.download = 'sifa-diagnostics.json';
      document.body.appendChild(anchor);
      anchor.click();
      anchor.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (error) { showError(error); }
    finally { button.disabled = false; }
  }

  function mouseCalibration() {
    // The lab intentionally uses the form values so users can feel a change before saving it.
    const dpi = Math.min(32000, Math.max(100, number($('dpi').value, 800)));
    const cm = Math.min(200, Math.max(1, number($('cm360').value, 35)));
    const sensitivity = Math.min(10, Math.max(0.05, number($('sensitivity').value, 1)));
    return { degreesPerCount: 360 * 2.54 * sensitivity / (dpi * cm), cmPerCount: 2.54 / dpi };
  }

  function updateLabMetrics() {
    setText('yaw-value', `${fixed(yaw)}°`);
    setText('pitch-value', `${fixed(pitch)}°`);
    setText('events-value', movementCount.toLocaleString('ru-RU'));
    setText('travel-value', `${fixed(absoluteX)} см`);
  }

  function resetLab() {
    yaw = 0; pitch = 0; movementCount = 0; absoluteX = 0;
    updateLabMetrics();
    scheduleDraw();
  }

  function scheduleDraw() {
    if (drawPending || !context) return;
    drawPending = true;
    requestAnimationFrame(() => { drawPending = false; drawLab(); });
  }

  function drawLab() {
    updateLabMetrics();
    const rect = canvas.getBoundingClientRect();
    if (!(rect.width > 0 && rect.height > 0)) return;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const backingWidth = Math.round(rect.width * dpr);
    const backingHeight = Math.round(rect.height * dpr);
    if (canvas.width !== backingWidth || canvas.height !== backingHeight) {
      canvas.width = backingWidth;
      canvas.height = backingHeight;
    }
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    const w = rect.width;
    const h = rect.height;
    const cx = w / 2;
    const cy = h / 2;
    context.fillStyle = '#0b141a';
    context.fillRect(0, 0, w, h);
    const spacing = 45;
    const pxPerDegree = w / 90;
    const xOffset = ((yaw * pxPerDegree) % spacing + spacing) % spacing;
    const yOffset = ((pitch * pxPerDegree) % spacing + spacing) % spacing;
    context.strokeStyle = '#22383d';
    context.lineWidth = 0.5;
    context.beginPath();
    for (let x = -xOffset; x < w; x += spacing) { context.moveTo(x, 0); context.lineTo(x, h); }
    for (let y = -yOffset; y < h; y += spacing) { context.moveTo(0, y); context.lineTo(w, y); }
    context.stroke();
    const wrappedYaw = ((yaw + 180) % 360 + 360) % 360 - 180;
    const wrappedPitch = ((pitch + 180) % 360 + 360) % 360 - 180;
    const targets = [{ x: 0, y: 0, radius: 22 }, { x: 22, y: -12, radius: 14 }, { x: -25, y: 10, radius: 14 }, { x: 46, y: 8, radius: 14 }, { x: -48, y: -14, radius: 14 }];
    for (const target of targets) {
      let dx = target.x - wrappedYaw;
      dx = ((dx + 180) % 360 + 360) % 360 - 180;
      const x = cx + dx * pxPerDegree;
      const y = cy + (target.y - wrappedPitch) * pxPerDegree;
      if (x < -40 || x > w + 40 || y < -40 || y > h + 40) continue;
      context.fillStyle = '#183e33';
      context.strokeStyle = '#497b64';
      context.lineWidth = 1;
      context.beginPath(); context.arc(x, y, target.radius, 0, Math.PI * 2); context.fill(); context.stroke();
      context.strokeStyle = '#6fba92';
      context.beginPath(); context.arc(x, y, target.radius * 0.6, 0, Math.PI * 2); context.stroke();
      context.fillStyle = '#96f7ca';
      context.beginPath(); context.arc(x, y, 2.5, 0, Math.PI * 2); context.fill();
    }
    context.strokeStyle = '#e0fff0';
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(cx - 11, cy); context.lineTo(cx - 4, cy);
    context.moveTo(cx + 4, cy); context.lineTo(cx + 11, cy);
    context.moveTo(cx, cy - 11); context.lineTo(cx, cy - 4);
    context.moveTo(cx, cy + 4); context.lineTo(cx, cy + 11);
    context.stroke();
    context.fillStyle = '#e0fff0';
    context.fillRect(cx - 1, cy - 1, 2, 2);
    context.font = '9px monospace';
    context.fillStyle = '#628375';
    context.fillText('SIFA / INPUT LAB', 17, h - 18);
    context.textAlign = 'right';
    context.fillText('90° HORIZONTAL FOV', w - 17, h - 18);
    context.textAlign = 'left';
  }

  async function capturePointer() {
    if (pointerRequestPending || document.pointerLockElement === canvas) return;
    if (typeof canvas.requestPointerLock !== 'function') {
      toast('Этот браузер не поддерживает захват курсора.');
      return;
    }
    pointerRequestPending = true;
    const preferRaw = $('raw-input').checked;
    try {
      pointerMode = preferRaw ? 'Raw-ввод запрошен · подтверждение браузера' : 'Браузерный ввод · ускорение ОС возможно';
      const request = preferRaw ? canvas.requestPointerLock({ unadjustedMovement: true }) : canvas.requestPointerLock();
      if (request && typeof request.then === 'function') {
        await request;
        pointerMode = preferRaw ? 'Захват подтверждён · raw-ввод запрошен' : 'Браузерный ввод · ускорение ОС возможно';
      } else if (preferRaw) {
        pointerMode = 'Браузерный ввод · raw-режим не подтверждён';
      }
      setText('pointer-mode', pointerMode);
    } catch (error) {
      if (preferRaw && error.name === 'NotSupportedError') {
        try {
          await canvas.requestPointerLock();
          pointerMode = 'Браузерный ввод · raw-режим недоступен';
          setText('pointer-mode', pointerMode);
          toast('Raw-ввод недоступен. Используется обычный захват курсора.');
        } catch { toast('Захват курсора отклонён. Повторите клик в активной вкладке.'); }
      } else {
        toast('Захват курсора отклонён. Повторите клик в активной вкладке.');
      }
    } finally { pointerRequestPending = false; }
  }

  document.addEventListener('pointerlockchange', () => {
    const locked = document.pointerLockElement === canvas;
    $('lab-overlay').hidden = locked;
    $('lab-hud').hidden = !locked;
    $('pointer-status').className = `pill${locked ? ' pill-mint' : ''}`;
    setText('pointer-status', locked ? 'Мышь захвачена' : 'Мышь свободна');
    setText('pointer-mode', pointerMode);
    scheduleDraw();
  });

  document.addEventListener('mousemove', (event) => {
    if (document.pointerLockElement !== canvas) return;
    const dx = number(event.movementX);
    const dy = number(event.movementY);
    const calibration = mouseCalibration();
    yaw += dx * calibration.degreesPerCount;
    pitch += dy * calibration.degreesPerCount;
    movementCount += 1;
    absoluteX += Math.abs(dx) * calibration.cmPerCount;
    scheduleDraw();
  });

  document.addEventListener('visibilitychange', () => {
    if (document.hidden && document.pointerLockElement === canvas) document.exitPointerLock();
  });
  window.addEventListener('resize', scheduleDraw);
  window.addEventListener('hashchange', () => navigate(location.hash.slice(1)));
  document.querySelectorAll('[data-go]').forEach((button) => button.addEventListener('click', () => { location.hash = button.dataset.go; }));
  $('run-demo').addEventListener('click', runDemo);
  $('run-benchmark').addEventListener('click', runBenchmark);
  $('refresh-state').addEventListener('click', refreshState);
  $('download-diagnostics').addEventListener('click', downloadDiagnostics);
  $('performance-form').addEventListener('submit', (event) => saveSettings(event, 'performance'));
  $('input-form').addEventListener('submit', (event) => saveSettings(event, 'input'));
  $('preset').addEventListener('change', applyPreset);
  ['width', 'height', 'target-fps', 'cpu-cores', 'guest-memory'].forEach((id) => $(id).addEventListener('input', () => {
    $('preset').value = 'custom';
    $('performance-status').classList.remove('error');
    setText('performance-status', 'Изменения ещё не сохранены');
    updateRanges();
  }));
  ['dpi', 'cm360', 'sensitivity', 'raw-input'].forEach((id) => $(id).addEventListener('input', () => {
    $('input-status').classList.remove('error');
    setText('input-status', 'Изменения ещё не сохранены');
    updateRanges();
    resetLab();
  }));
  $('capture-pointer').addEventListener('click', capturePointer);
  canvas.addEventListener('click', capturePointer);
  $('reset-lab').addEventListener('click', resetLab);
  $('sensitivity').min = '0.05';
  $('sensitivity').max = '10';
  connected(false);
  syncSettings();
  navigate(location.hash.slice(1));
  refreshState();
})();
