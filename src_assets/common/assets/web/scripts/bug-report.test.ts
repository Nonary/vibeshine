import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import {
  buildReportLinks,
  bugReportUrl,
  crashReportUrl,
} from '../../web-legacy/utils/bugReport.ts';

test('bug and crash links use the correct GitHub forms for this project', () => {
  for (const [link, filename] of [
    [bugReportUrl, 'bug-report.yml'],
    [crashReportUrl, 'report-a-crash.yml'],
  ]) {
    const url = new URL(link);
    assert.equal(url.origin, 'https://github.com');
    assert.equal(url.pathname, '/Nonary/vibeshine/issues/new');
    assert.deepEqual([...url.searchParams], [['template', filename]]);
    const path = new URL(`../../../../../.github/ISSUE_TEMPLATE/${filename}`, import.meta.url);
    const form = readFileSync(path, 'utf8');
    if (link === bugReportUrl) {
      assert.match(form, /type: textarea\s+id: symptoms\s/);
      assert.match(form, /id: os-version[\s\S]*?Linux, macOS, or other OS/);
    }
  }
});

test('only a bounded public version and a canonical platform can be shared', () => {
  const version = 'v123456.123456.123456-nightly.12345678901234.12345678901234';
  const report = buildReportLinks(version, 'macos');
  const url = new URL(report.prefilled);
  assert.deepEqual(
    [...url.searchParams],
    [
      ['template', 'bug-report.yml'],
      ['symptoms', `Version: ${version}\nHost platform: macOS\n\n`],
    ],
  );
  assert.equal(report.details + '\n\n', url.searchParams.get('symptoms'));
  assert.ok(report.prefilled.length < 256);
  assert.equal(
    buildReportLinks('2.0.4-beta.2', 'windows').details,
    'Version: 2.0.4-beta.2\nHost platform: Windows',
  );
  assert.equal(
    buildReportLinks('2.0.4', 'freebsd').details,
    'Version: 2.0.4\nHost platform: FreeBSD',
  );
  for (const invalid of [
    '2.0.4-username-machine',
    '2.0.4+abcdef01234567',
    '2.0.4\n',
    '2.0.4&logs=secret',
    '1234567.0.4',
    '/home/example',
    '192.168.1.2',
    ['2.0.4'],
    { token: 'secret' },
    undefined,
  ]) {
    assert.deepEqual(buildReportLinks(invalid, 'linux-192.168.0.1'), {
      bare: bugReportUrl,
      prefilled: bugReportUrl,
      details: '',
    });
  }
});
