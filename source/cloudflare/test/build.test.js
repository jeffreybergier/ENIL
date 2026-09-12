import { describe, it, expect } from 'vitest';
import { mkdtemp, mkdir, cp, writeFile, readFile, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { zipSync, strToU8 } from 'fflate';

const root = fileURLToPath(new URL('../', import.meta.url));

describe('download validation', () => {
  for (const [name, manifest, expected] of [
    ['rejects an unsupported extension version', { version: '0.0.0' }, 'extractor supports 3.7.2'],
    ['rejects modified extension code', { version: '3.7.2' }, 'integrity check failed'],
  ]) {
    it(name + ' and removes stale output', async () => {
      const dir = await mkdtemp(join(tmpdir(), 'enil-build-test-'));
      try {
        await cp(join(root, 'scripts'), join(dir, 'scripts'), { recursive: true });
        await symlink(join(root, 'node_modules'), join(dir, 'node_modules'), 'dir');
        await mkdir(join(dir, 'src'));
        await writeFile(join(dir, 'src/ltsm-worker.js'), 'stale output');
        await writeFile(join(dir, 'src/ltsm.wasm'), 'stale output');
        const bytes = zipSync({
          'manifest.json': strToU8(JSON.stringify(manifest)),
          'static/js/ltsmSandbox.js': strToU8('modified'),
          'static/js/ltsm.wasm': strToU8('modified'),
        });
        // No external requests or LINE code are needed for these failure cases.
        const preload = `globalThis.fetch = async () => new Response(Buffer.from(${JSON.stringify(Buffer.from(bytes).toString('base64'))}, 'base64'));`;
        const result = spawnSync(process.execPath, ['--import', `data:text/javascript,${encodeURIComponent(preload)}`, 'scripts/build.mjs', '--refresh'], { cwd: dir, encoding: 'utf8', timeout: 10000 });
        expect(result.status).toBe(1);
        expect(result.stderr).toContain(expected);
        await expect(readFile(join(dir, 'src/ltsm-worker.js'))).rejects.toMatchObject({ code: 'ENOENT' });
        await expect(readFile(join(dir, 'src/ltsm.wasm'))).rejects.toMatchObject({ code: 'ENOENT' });
      } finally {
        await rm(dir, { recursive: true, force: true });
      }
    });
  }
});
