// Minimal GitHub release shape used by the UI and SunshineVersion.
// Keep only the fields we actually reference in the code to reduce noise.
export interface GitHubRelease {
  tag_name: string;
  name: string;
  html_url: string;
  body: string;
  prerelease?: boolean;
  [key: string]: any;
}

export default class SunshineVersion {
  public version: string;
  public versionParts: number[];
  public versionMajor: number;
  public versionMinor: number;
  public versionPatch: number;

  /**
   * Construct a SunshineVersion. Either pass a GitHubRelease or a version string.
   * All fields on the instance are non-nullable and initialised to sensible defaults.
   */
  constructor(version: string) {
    this.version = version || '0.0.0';
    this.versionParts = this.parseVersion(this.version);
    this.versionMajor = this.versionParts[0];
    this.versionMinor = this.versionParts[1];
    this.versionPatch = this.versionParts[2];
  }

  /** Create a SunshineVersion from a GitHubRelease */
  static fromRelease(release: GitHubRelease): SunshineVersion {
    const tag = (release && release.tag_name) || '0.0.0';
    return new SunshineVersion(tag);
  }

  /** Compare this version to a release directly */
  isGreaterRelease(release: GitHubRelease | string): boolean {
    if (typeof release === 'string') return this.isGreater(release);
    const tag = release.tag_name || '';
    return this.isGreater(tag);
  }

  /**
   * Parse a version string like "v1.2.3" or "1.2" into a 3-number array.
   * Always returns a length-3 array of numbers (no nulls).
   */
  parseVersion(version: string): number[] {
    if (!version) return [0, 0, 0];
    let v = version;
    if (v.charAt(0) === 'v') {
      v = v.substring(1);
    }
    const parts = v.split('.').map((p) => {
      const n = Number(p);
      return Number.isFinite(n) ? Math.floor(n) : 0;
    });
    // Ensure exactly 3 parts
    while (parts.length < 3) parts.push(0);
    return parts.slice(0, 3);
  }

  /**
   * Return true if this version is greater than the other.
   */
  isGreater(otherVersion: SunshineVersion | string): boolean {
    let otherVersionParts: number[];
    if (otherVersion instanceof SunshineVersion) {
      otherVersionParts = otherVersion.versionParts;
    } else if (typeof otherVersion === 'string') {
      otherVersionParts = this.parseVersion(otherVersion);
    } else {
      throw new Error(
        'Invalid argument: otherVersion must be a SunshineVersion object or a version string',
      );
    }

    for (let i = 0; i < 3; i++) {
      if (this.versionParts[i] !== otherVersionParts[i]) {
        return this.versionParts[i] > otherVersionParts[i];
      }
    }
    return false;
  }
}
