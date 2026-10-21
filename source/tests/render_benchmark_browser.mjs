// Node >= 22, Chromium CDP over private pipes; no npm packages or server.
import {spawn} from 'node:child_process';
import {mkdtempSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {pathToFileURL} from 'node:url';
const [chrome, pagePath, countString, repeatsString] = process.argv.slice(2);
const count = Number(countString), repeats = Number(repeatsString);
const profile = mkdtempSync(join(tmpdir(), 'enil-render-'));
const child = spawn(chrome, ['--headless', '--no-sandbox', '--disable-dev-shm-usage',
  '--disable-background-networking', '--disable-component-update', '--no-first-run',
  '--disable-default-apps', '--disable-sync', '--remote-debugging-pipe',
  `--user-data-dir=${profile}`, 'about:blank'], {stdio: ['ignore', 'ignore', 'inherit', 'pipe', 'pipe']});
let serial = 0, buffer = '', session;
const pending = new Map();
child.stdio[4].on('data', chunk => {
  buffer += chunk.toString();
  let end;
  while ((end = buffer.indexOf('\0')) >= 0) {
    const msg = JSON.parse(buffer.slice(0, end)); buffer = buffer.slice(end + 1);
    if (pending.has(msg.id)) {
      const {resolve, reject, timer} = pending.get(msg.id);
      pending.delete(msg.id); clearTimeout(timer);
      msg.error ? reject(new Error(JSON.stringify(msg.error))) : resolve(msg.result);
    }
  }
});
function call(method, params = {}, target = session) {
  return new Promise((resolve, reject) => {
    const id = ++serial;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`Timeout: ${method}`)); }, 60000);
    pending.set(id, {resolve, reject, timer});
    child.stdio[3].write(JSON.stringify({id, method, params, ...(target ? {sessionId: target} : {})}) + '\0');
  });
}
async function evaluate(expression) {
  const r = await call('Runtime.evaluate', {expression, awaitPromise: true, returnByValue: true});
  if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails));
  return r.result.value;
}
try {
  const version = await call('Browser.getVersion');
  const samples = [];
  for (let run = -2; run < repeats; run++) {
    const {targetId} = await call('Target.createTarget', {url: 'about:blank'}, null);
    session = (await call('Target.attachToTarget', {targetId, flatten: true}, null)).sessionId;
    await call('Page.enable');
    await call('Network.enable');
    await call('Network.setCacheDisabled', {cacheDisabled: true});
    await call('Network.setBlockedURLs', {urls: ['http://*', 'https://*']});
    await call('Emulation.setDeviceMetricsOverride', {width: 320, height: 568, deviceScaleFactor: 2, mobile: true});
    await call('Emulation.setUserAgentOverride', {userAgent: 'Mozilla/5.0 (iPhone; CPU iPhone OS 6_1_3 like Mac OS X) AppleWebKit/536.26 Mobile/10B329 Safari/8536.25'});
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `
      window.benchmarkReady = new Promise((resolve, reject) => addEventListener('load', async () => {
        try {
        await document.fonts.ready;
        await Promise.all(Array.from(document.images, img => img.decode()));
        requestAnimationFrame(() => requestAnimationFrame(() => resolve(performance.now())));
        } catch (error) { reject(error); }
      }));`});
    const navigation = await call('Page.navigate', {url: pathToFileURL(pagePath).href});
    if (navigation.errorText) throw new Error(navigation.errorText);
    // Wait for the new document's promise, not about:blank's execution context.
    let ready;
    for (let n = 0; n < 600; n++) {
      try { ready = await evaluate('typeof benchmarkReady !== "undefined"'); } catch {}
      if (ready) break;
      await new Promise(r => setTimeout(r, 50));
    }
    if (!ready) throw new Error('Page did not initialize');
    const load_ms = await evaluate('benchmarkReady');
    const result = await evaluate(`(async () => {
      if (document.documentElement.clientWidth !== 320 || window.innerWidth !== 320)
        throw Error('Expected a 320 CSS pixel viewport, got ' +
                    document.documentElement.clientWidth + '/' + window.innerWidth);
      const n = document.querySelectorAll('.enil-seg[id^="msg-"]').length;
      const images = Array.from(document.images);
      if (n !== ${count} || images.some(i => !i.complete || !i.naturalWidth))
        throw Error('Incomplete fixture: ' + n + ' messages');
      if (document.querySelectorAll('.enil-text').length !== ${count / 5 * 3} ||
          document.querySelector('.enil-placeholder') ||
          !document.getElementById('msg-000000').textContent.includes('Message 0000: 日本語'))
        throw Error('Missing plaintext messages');
      const emojis = document.querySelectorAll('.enil-sticon-inline').length;
      if (emojis !== ${count / 5 * 6} || document.querySelectorAll('.enil-media img').length !== ${count / 5} ||
          document.querySelectorAll('.enil-sticker img').length !== ${count / 5})
        throw Error('Missing media or emoji: ' + emojis);
      if (document.body.scrollHeight < ${count * 20} ||
          document.getElementById('msg-000000').getBoundingClientRect().height <= 0)
        throw Error('Messages are not laid out');
      const frame = () => new Promise(r => requestAnimationFrame(r));
      const start = performance.now();
      for (let i=0; i<30; i++) {
        window.scrollTo(0, (document.body.scrollHeight-innerHeight)*(i%2));
        void document.body.offsetHeight;
        await frame();
      }
      const scroll_ms = performance.now()-start;
      const node = document.querySelector('.enil-message').cloneNode(true);
      for (const e of node.querySelectorAll('[id]')) e.id = 'append-' + e.id;
      const appendStart = performance.now();
      appendMessage(node.outerHTML, true);
      void document.body.offsetHeight;
      const append_layout_ms = performance.now()-appendStart;
      await frame(); await frame();
      const older = [];
      for (let i=0; i<40; i++) {
        const old = node.cloneNode(true);
        for (const e of old.querySelectorAll('[id]')) e.id = 'older-' + i + '-' + e.id;
        older.push(old.outerHTML);
      }
      const anchor = document.getElementById('msg-000000');
      const anchorTop = anchor.getBoundingClientRect().top;
      const prependStart = performance.now();
      prependMessages(older, false);
      void document.body.offsetHeight;
      const prepend_layout_ms = performance.now()-prependStart;
      if (document.querySelectorAll('.enil-seg').length !== ${count + 41})
        throw Error('Append/prepend lost messages');
      const rows = document.querySelectorAll('.enil-message');
      for (let i=0; i<40; i++) {
        if (rows[i].querySelector('.enil-seg').id !== 'older-' + i + '-append-msg-000000')
          throw Error('Prepend changed message order');
      }
      if (rows[rows.length-1].querySelector('.enil-seg').id !== 'append-msg-000000')
        throw Error('Append did not insert at the end');
      const prepend_anchor_drift_px = Math.abs(anchor.getBoundingClientRect().top-anchorTop);
      return {scroll_ms, append_layout_ms, prepend_layout_ms, dom_nodes: document.querySelectorAll('*').length,
        images: images.length, emojis, prepend_anchor_drift_px};
    })()`);
    if (run >= 0) samples.push({load_ms, ...result});
    await call('Target.closeTarget', {targetId}, null); session = undefined;
  }
  console.log(JSON.stringify({version: version.product, viewport: '320x568@2 mobile; verified 320 CSS px; iOS UA', samples}));
} finally {
  child.kill();
  for (const p of pending.values()) clearTimeout(p.timer);
  await new Promise(resolve => child.exitCode !== null ? resolve() : child.once('exit', resolve));
  rmSync(profile, {recursive: true, force: true});
}
