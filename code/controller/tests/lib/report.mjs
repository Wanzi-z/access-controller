import { writeFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

class TestReport {
  constructor() {
    this.startTime = new Date();
    this.endTime = null;
    this.suites = [];
    this.results = { pass: 0, fail: 0, skip: 0, total: 0 };
    this.currentSuite = null;
  }

  startSuite(name, description = '') {
    this.currentSuite = { name, description, tests: [], startTime: new Date() };
    this.suites.push(this.currentSuite);
    console.log(`\n${'═'.repeat(60)}`);
    console.log(`  ${name}`);
    if (description) console.log(`  ${description}`);
    console.log(`${'═'.repeat(60)}`);
  }

  endSuite() {
    if (this.currentSuite) {
      this.currentSuite.endTime = new Date();
      const p = this.currentSuite.tests.filter(t => t.status === 'pass').length;
      const f = this.currentSuite.tests.filter(t => t.status === 'fail').length;
      const s = this.currentSuite.tests.filter(t => t.status === 'skip').length;
      console.log(`\n  Suite: ${p} passed, ${f} failed, ${s} skipped`);
    }
  }

  addResult(name, status, detail = '', duration = 0) {
    const test = { name, status, detail, duration };
    if (this.currentSuite) {
      this.currentSuite.tests.push(test);
    }
    this.results.total++;
    if (status === 'pass') this.results.pass++;
    else if (status === 'fail') this.results.fail++;
    else this.results.skip++;

    const icon = status === 'pass' ? '✅' : status === 'fail' ? '❌' : '⏭️';
    const durStr = duration ? ` (${duration}ms)` : '';
    console.log(`  ${icon} ${name}${durStr}`);
    if (detail && status === 'fail') {
      console.log(`     ${detail.replace(/\n/g, '\n     ')}`);
    }
  }

  pass(name, detail = '', duration = 0) {
    this.addResult(name, 'pass', detail, duration);
  }

  fail(name, detail = '', duration = 0) {
    this.addResult(name, 'fail', detail, duration);
  }

  skip(name, detail = '', duration = 0) {
    this.addResult(name, 'skip', detail, duration);
  }

  finish() {
    this.endTime = new Date();
    this.printSummary();
    this.writeHtml();
  }

  printSummary() {
    const dur = Math.round((this.endTime - this.startTime) / 1000);
    console.log(`\n${'═'.repeat(60)}`);
    console.log(`  TEST REPORT SUMMARY`);
    console.log(`${'═'.repeat(60)}`);
    console.log(`  Total:  ${this.results.total}`);
    console.log(`  Passed: ${this.results.pass} ✅`);
    console.log(`  Failed: ${this.results.fail} ❌`);
    console.log(`  Skipped: ${this.results.skip} ⏭️`);
    console.log(`  Duration: ${dur}s`);
    console.log(`${'═'.repeat(60)}\n`);
  }

  writeHtml() {
    const dur = Math.round((this.endTime - this.startTime) / 1000);
    const pct = this.results.total > 0
      ? Math.round((this.results.pass / this.results.total) * 100)
      : 0;

    let suitesHtml = '';
    for (const suite of this.suites) {
      const sDur = suite.endTime
        ? Math.round((suite.endTime - suite.startTime) / 1000)
        : 0;
      suitesHtml += `
        <div class="suite">
          <h3>${this.escape(suite.name)} <span class="dur">${sDur}s</span></h3>
          ${suite.description ? `<p class="desc">${this.escape(suite.description)}</p>` : ''}
          <table>
            <tr><th>Test</th><th>Result</th><th>Duration</th><th>Details</th></tr>
            ${suite.tests.map(t => `
              <tr class="${t.status}">
                <td>${this.escape(t.name)}</td>
                <td>${t.status === 'pass' ? '✅ PASS' : t.status === 'fail' ? '❌ FAIL' : '⏭️ SKIP'}</td>
                <td>${t.duration ? t.duration + 'ms' : ''}</td>
                <td class="detail">${this.escape(t.detail || '')}</td>
              </tr>
            `).join('')}
          </table>
        </div>`;
    }

    // System info summary
    let sysInfo = '';
    if (this._systemInfo) {
      sysInfo = `
        <div class="suite">
          <h3>System Information</h3>
          <table>
            ${Object.entries(this._systemInfo).map(([k, v]) => `
              <tr><td><strong>${k}</strong></td><td>${this.escape(String(v))}</td></tr>
            `).join('')}
          </table>
        </div>`;
    }

    const html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <title>Access Controller - Test Report</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #1a1a2e; color: #e0e0e0; padding: 20px; }
    .header { text-align: center; padding: 30px; background: #16213e; border-radius: 12px; margin-bottom: 20px; }
    .header h1 { font-size: 28px; color: #00d4ff; }
    .header .meta { margin-top: 8px; color: #888; }
    .summary { display: flex; gap: 20px; justify-content: center; margin: 20px 0; flex-wrap: wrap; }
    .summary .card { background: #16213e; border-radius: 10px; padding: 20px 30px; text-align: center; min-width: 100px; }
    .summary .card .num { font-size: 36px; font-weight: bold; }
    .summary .card.pass .num { color: #00c853; }
    .summary .card.fail .num { color: #ff1744; }
    .summary .card.total .num { color: #00d4ff; }
    .progress { width: 100%; height: 10px; background: #333; border-radius: 5px; margin: 15px 0; overflow: hidden; }
    .progress .bar { height: 100%; background: linear-gradient(90deg, #00c853, #00d4ff); border-radius: 5px; transition: width 0.3s; }
    .suite { background: #16213e; border-radius: 10px; padding: 20px; margin-bottom: 20px; }
    .suite h3 { color: #00d4ff; margin-bottom: 5px; }
    .suite .dur { font-size: 14px; color: #888; font-weight: normal; }
    .suite .desc { color: #888; margin-bottom: 10px; font-style: italic; }
    table { width: 100%; border-collapse: collapse; margin-top: 10px; }
    th { text-align: left; padding: 8px 12px; background: #0f3460; color: #00d4ff; font-size: 13px; }
    td { padding: 8px 12px; border-bottom: 1px solid #333; font-size: 13px; }
    .pass td:first-child { border-left: 3px solid #00c853; }
    .fail td:first-child { border-left: 3px solid #ff1744; }
    .skip td:first-child { border-left: 3px solid #ff9800; }
    .detail { max-width: 300px; word-break: break-all; color: #888; font-size: 12px; }
  </style>
</head>
<body>
  <div class="header">
    <h1>Access Controller Test Report</h1>
    <div class="meta">
      Started: ${this.startTime.toLocaleString()} &nbsp;|&nbsp;
      Duration: ${dur}s &nbsp;|&nbsp;
      Pass rate: ${pct}%
    </div>
    <div class="summary">
      <div class="card total"><div class="num">${this.results.total}</div><div>Total</div></div>
      <div class="card pass"><div class="num">${this.results.pass}</div><div>Passed</div></div>
      <div class="card fail"><div class="num">${this.results.fail}</div><div>Failed</div></div>
    </div>
    <div class="progress"><div class="bar" style="width:${pct}%"></div></div>
  </div>
  ${sysInfo}
  ${suitesHtml}
</body>
</html>`;

    const outPath = join(__dirname, '..', 'test-report.html');
    writeFileSync(outPath, html);
    console.log(`\n📄 Report saved: ${outPath}`);
  }

  escape(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  setSystemInfo(info) {
    this._systemInfo = info;
  }
}

export default TestReport;
