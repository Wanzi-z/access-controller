import { execFileSync } from 'child_process';
import { mkdtempSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { dirname, join, resolve } from 'path';
import { fileURLToPath } from 'url';

const here = dirname(fileURLToPath(import.meta.url));
const services = resolve(here, '../../main/services');
const temporaryDirectory = mkdtempSync(join(tmpdir(), 'access-controller-wiegand-format-'));
const binary = join(temporaryDirectory, 'wiegand-format-test');

try {
  execFileSync('cc', [
    '-std=c11', '-Wall', '-Wextra', '-Werror',
    '-I', services,
    join(here, 'wiegand-format-test.c'),
    join(services, 'wiegand_format.c'),
    '-o', binary,
  ], { stdio: 'inherit' });
  execFileSync(binary, [], { stdio: 'inherit' });
} finally {
  rmSync(temporaryDirectory, { recursive: true, force: true });
}
