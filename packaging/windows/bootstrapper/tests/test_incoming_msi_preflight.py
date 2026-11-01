#!/usr/bin/env python3
"""Execute the production pre-removal gate with only Windows API/log boundaries stubbed."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / 'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()

def extract(pattern):
    matches = re.findall(pattern, source, re.M | re.S)
    assert len(matches) == 1, pattern
    return matches[0]

method = extract(r'^    private static InstallerResult PreflightIncomingMsi\(.*?^    \}')
kind = extract(r'^    internal enum InstalledProductKind \{.*?^    \}')
result = extract(r'^  internal sealed class InstallerResult \{.*?^  \}')
operation = extract(r'^  internal enum InstallerOperation \{.*?^  \}')
# Both actual dispatchers must reject before reaching any removal or service stop.
for start, end in [('public static InstallerResult RunInteractiveInstall(', 'private static bool IsCertificateThumbprint('),
                   ('public static InstallerResult RunCli(', 'private static void PreserveCliInstallDirectory(')]:
    body = source.split(start, 1)[1].split(end, 1)[0]
    gate = body.index('if (preflightFailure != null) return preflightFailure;')
    for effect in ('UninstallCompetingProducts(', 'TryPreUninstallDowngradeSourceVersion('):
        assert gate < body.index(effect), (start, effect)
    if 'TryStopRelatedServicesAndProcesses(' in body:
        assert gate < body.index('TryStopRelatedServicesAndProcesses(')
assert 'HasProperty(cliArgs, "TRANSFORMS") || HasProperty(cliArgs, "PATCH")' in source
assert method.index('ValidateMsiPayloadImmediatelyBeforeExecution(') < method.index('MsiOpenPackageEx(')

harness = r'''
using System;
using System.Collections.Generic;
using System.Linq;
namespace VibeshineInstaller {
RESULT
OPERATION
static class InstallerRunner {
KIND
  const uint MsiErrorSuccess = 0;
  static string Failure, Root;
  static bool Throw, Verified;
  static int Closed, Checks;
  static List<string> Calls;
  static string BuildLogPath(string phase) { return phase; }
  static void AppendInstallerLogMessage(string path, string message) { }
  static void ValidateMsiPayloadImmediatelyBeforeExecution(IReadOnlyList<string> args) {
    Check(args.SequenceEqual(new[] { "/i", "incoming.msi" }), "exact incoming payload verified");
    if (Failure == "verify") throw new InvalidOperationException("signature rejected");
    Verified = true;
  }
  static uint Outcome(string call) {
    Calls.Add(call);
    if (Failure != call) return 0;
    if (Throw) throw new InvalidOperationException("API exception");
    return 1603;
  }
  static uint MsiOpenPackageEx(string path, uint flags, out IntPtr handle) {
    Check(Verified && flags == 0, "verified state-aware MSI session");
    handle = Failure == "null-handle" ? IntPtr.Zero : new IntPtr(42);
    return Outcome("open");
  }
  static uint MsiSetProperty(IntPtr handle, string name, string value) {
    Check(handle == new IntPtr(42), "properties target opened package");
    if (name == "INSTALL_ROOT") Check(value == Root, "exact root preserved");
    if (name == "REMOVE") Check(value == "", "not uninstalling");
    if (name == "SKIP_REMOVE_CONFLICTING_PRODUCTS") Check(value == "0", "all competing roots checked");
    return Outcome(name);
  }
  static uint MsiDoAction(IntPtr handle, string action) {
    Check(handle == new IntPtr(42), "action targets opened package");
    Check(new[] { "CostInitialize", "FileCost", "CostFinalize", "ValidateInstallRootPreview" }.Contains(action), "no installation or removal action");
    return Outcome(action);
  }
  static uint MsiSetInternalUI(uint level, IntPtr window) { return 5; }
  static uint MsiCloseHandle(IntPtr handle) { Closed++; return 0; }
METHOD
  static void Check(bool ok, string why) { Checks++; if (!ok) throw new Exception(why); }
  static void Run(string failure, bool throws, string root) {
    Failure = failure; Throw = throws; Root = root; Verified = false; Closed = 0; Calls = new List<string>();
    var result = PreflightIncomingMsi("incoming.msi", Root, "preflight");
    bool good = failure == null;
    Check((result == null) == good, "rejected preflight cannot authorize removal: " + failure);
    Check(Closed == (failure == "verify" || failure == "null-handle" ? 0 : 1), "handles closed on success and failure");
    if (good) {
      var expected = new List<string> { "open" };
      if (!string.IsNullOrWhiteSpace(root)) expected.Add("INSTALL_ROOT");
      expected.AddRange(new[] { "REMOVE", "SKIP_REMOVE_CONFLICTING_PRODUCTS", "SUPPRESSMSGBOXES", "CostInitialize", "FileCost", "CostFinalize", "ValidateInstallRootPreview" });
      Check(Calls.SequenceEqual(expected), "cost then native guard ordering");
    } else {
      Check(result.ExitCode == 1603 && result.Message.Contains("has not been removed"), "failure explains preserved installation");
      if (Calls.Contains(failure)) Check(Calls.Last() == failure, "stop at first failure");
    }
  }
  static void Main() {
    Run(null, false, @"C:\Program Files\Sunshine");
    Run(null, false, null);
    foreach (var failure in new[] { "verify", "open", "null-handle", "INSTALL_ROOT", "REMOVE", "SKIP_REMOVE_CONFLICTING_PRODUCTS", "SUPPRESSMSGBOXES", "CostInitialize", "FileCost", "CostFinalize", "ValidateInstallRootPreview" }) {
      Run(failure, false, @"C:\Program Files\O'Brien");
      Run(failure, true, @"C:\Program Files\Sunshine");
    }
    Console.WriteLine(Checks + " incoming MSI preflight checks passed; Windows boundaries stubbed.");
  }
}
}
'''.replace('KIND', kind).replace('RESULT', result).replace('OPERATION', operation).replace('METHOD', method)
dotnet = sys.argv[1] if len(sys.argv) > 1 else 'dotnet'
major = subprocess.check_output([dotnet, '--version'], text=True).strip().split('.')[0]
with tempfile.TemporaryDirectory(prefix='incoming-msi-preflight-') as directory:
    path = Path(directory)
    (path / 'Test.csproj').write_text(f'<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net{major}.0</TargetFramework></PropertyGroup></Project>')
    (path / 'NuGet.Config').write_text('<configuration><packageSources><clear /></packageSources></configuration>')
    (path / 'Test.cs').write_text(harness)
    env = dict(os.environ, DOTNET_CLI_HOME=str(path / 'home'), DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    subprocess.run([dotnet, 'run', '--project', str(path / 'Test.csproj')], check=True, env=env)
