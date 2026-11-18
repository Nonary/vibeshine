const issues = 'https://github.com/Nonary/vibeshine/issues';

export const bugReportUrl = `${issues}/new?template=bug-report.yml`;
export const crashReportUrl = `${issues}/new?template=report-a-crash.yml`;

const platforms = new Map([
  ['windows', 'Windows'],
  ['linux', 'Linux'],
  ['macos', 'macOS'],
  ['freebsd', 'FreeBSD'],
]);

// Only public version formats and platform families from /api/metadata belong in the URL.
// Leave custom version suffixes, commit hashes, branch names and host details on the host.
const publicVersion =
  /^v?\d{1,6}\.\d{1,6}\.\d{1,6}(?:-(?:stable|alpha|beta|rc|daily|nightly)(?:\.\d{1,14}){0,2})?$/;

export function buildReportLinks(version: unknown, platform: unknown) {
  const details = [];
  if (typeof version === 'string' && publicVersion.test(version) && !/\s/.test(version)) {
    details.push(`Version: ${version}`);
  }
  const family = typeof platform === 'string' ? platforms.get(platform) : undefined;
  if (family) details.push(`Host platform: ${family}`);

  const text = details.join('\n');
  const url = new URL(bugReportUrl);
  if (text) url.searchParams.set('symptoms', text + '\n\n');
  // The allowlists bound the link well below typical URL limits. Keep the bare form
  // and the copyable details available if GitHub drops the parameters during sign-in.
  return { bare: bugReportUrl, prefilled: url.href, details: text };
}

export async function downloadReportLogs(signal: AbortSignal): Promise<void> {
  const response = await fetch('/api/logs/export', {
    credentials: 'same-origin',
    redirect: 'error',
    cache: 'no-store',
    headers: { Accept: 'application/zip' },
    signal,
  });
  const type = response.headers.get('Content-Type')?.split(';')[0]?.trim().toLowerCase();
  if (!response.ok || type !== 'application/zip') throw new Error('log-export-failed');

  const blob = await response.blob();
  if (!blob.size || signal.aborted) throw new Error('log-export-failed');

  // Both server implementations return an ASCII ZIP attachment filename.
  const disposition = response.headers.get('Content-Disposition') ?? '';
  const filename = /(?:^|;)\s*filename="([\w-]{1,100}\.zip)"(?:\s*;|$)/i.exec(disposition)?.[1];
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename ?? 'diagnostics.zip';
  document.body.append(link);
  try {
    link.click();
  } finally {
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
}
