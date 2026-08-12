using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

internal static class UMgrTokenHandoffProof
{
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const uint TokenAssignPrimary = 0x0001;
    private const uint TokenDuplicate = 0x0002;
    private const uint TokenQuery = 0x0008;
    private const uint TokenAdjustPrivileges = 0x0020;
    private const uint MaximumAllowed = 0x02000000;
    private const int SecurityImpersonation = 2;
    private const int TokenPrimary = 1;
    private const int TokenSessionId = 12;
    private const int TokenBnoIsolationClass = 44;
    private const uint SePrivilegeEnabled = 0x00000002;
    private const uint CreateUnicodeEnvironment = 0x00000400;
    private const uint CreateNoWindow = 0x08000000;
    private const uint WaitObject0 = 0x00000000;

    [StructLayout(LayoutKind.Sequential)]
    private struct Luid
    {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenPrivileges
    {
        public uint PrivilegeCount;
        public Luid Luid;
        public uint Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenBnoIsolationInformation
    {
        public IntPtr IsolationPrefix;
        [MarshalAs(UnmanagedType.U1)]
        public bool IsolationEnabled;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ProcessIdToSessionId(int processId, out uint sessionId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool OpenProcessToken(IntPtr process, uint desiredAccess, out IntPtr token);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DuplicateTokenEx(
        IntPtr existingToken,
        uint desiredAccess,
        IntPtr tokenAttributes,
        int impersonationLevel,
        int tokenType,
        out IntPtr newToken);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetTokenInformation(
        IntPtr token,
        int informationClass,
        ref uint tokenInformation,
        int tokenInformationLength);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(
        IntPtr token,
        int informationClass,
        IntPtr tokenInformation,
        int tokenInformationLength,
        out int returnLength);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool LookupPrivilegeValue(string systemName, string name, out Luid luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AdjustTokenPrivileges(
        IntPtr token,
        bool disableAllPrivileges,
        ref TokenPrivileges newState,
        int bufferLength,
        IntPtr previousState,
        IntPtr returnLength);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessAsUser(
        IntPtr token,
        string applicationName,
        StringBuilder commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        bool inheritHandles,
        uint creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref StartupInfo startupInfo,
        out ProcessInformation processInformation);

    [DllImport("userenv.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateEnvironmentBlock(out IntPtr environment, IntPtr token, bool inherit);

    [DllImport("userenv.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyEnvironmentBlock(IntPtr environment);

    [DllImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WTSQueryUserToken(uint sessionId, out IntPtr token);

    [DllImport("usermgrcli.dll")]
    private static extern int UMgrChangeSessionUserToken(IntPtr token);

    [DllImport("usermgrcli.dll")]
    private static extern int UMgrQuerySessionUserToken(uint sessionId, out IntPtr token);

    private static string TokenIdentity(IntPtr token)
    {
        if (token == IntPtr.Zero)
        {
            return "(null)";
        }

        using (WindowsIdentity identity = new WindowsIdentity(token))
        {
            return identity.Name + "|" + (identity.User == null ? "(no SID)" : identity.User.Value);
        }
    }

    private static string TokenBnoIsolation(IntPtr token)
    {
        int required;
        GetTokenInformation(token, TokenBnoIsolationClass, IntPtr.Zero, 0, out required);
        int firstError = Marshal.GetLastWin32Error();
        if (required <= 0)
        {
            return "query-size-failed:" + firstError;
        }

        IntPtr buffer = Marshal.AllocHGlobal(required);
        try
        {
            if (!GetTokenInformation(token, TokenBnoIsolationClass, buffer, required, out required))
            {
                return "query-failed:" + Marshal.GetLastWin32Error();
            }

            TokenBnoIsolationInformation information =
                (TokenBnoIsolationInformation) Marshal.PtrToStructure(
                    buffer,
                    typeof(TokenBnoIsolationInformation));
            string prefix = information.IsolationPrefix == IntPtr.Zero
                ? "(none)"
                : Marshal.PtrToStringUni(information.IsolationPrefix);
            return "enabled=" + information.IsolationEnabled + ";prefix=" + prefix;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static bool EnablePrivilege(IntPtr processToken, string name, List<string> log)
    {
        Luid luid;
        if (!LookupPrivilegeValue(null, name, out luid))
        {
            log.Add("Privilege." + name + "=LookupFailed:" + Marshal.GetLastWin32Error());
            return false;
        }

        TokenPrivileges privileges = new TokenPrivileges();
        privileges.PrivilegeCount = 1;
        privileges.Luid = luid;
        privileges.Attributes = SePrivilegeEnabled;
        if (!AdjustTokenPrivileges(processToken, false, ref privileges, 0, IntPtr.Zero, IntPtr.Zero))
        {
            log.Add("Privilege." + name + "=AdjustFailed:" + Marshal.GetLastWin32Error());
            return false;
        }

        int error = Marshal.GetLastWin32Error();
        log.Add("Privilege." + name + "=" + (error == 0 ? "Enabled" : "NotAssigned:" + error));
        return error == 0;
    }

    private static int LaunchIdentityProbe(IntPtr sessionToken, uint targetSession, string resultPath, List<string> log)
    {
        IntPtr primaryToken = IntPtr.Zero;
        IntPtr environment = IntPtr.Zero;
        ProcessInformation processInformation = new ProcessInformation();
        try
        {
            if (!DuplicateTokenEx(
                    sessionToken,
                    MaximumAllowed,
                    IntPtr.Zero,
                    SecurityImpersonation,
                    TokenPrimary,
                    out primaryToken))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateTokenEx for launch failed");
            }

            if (!SetTokenInformation(primaryToken, TokenSessionId, ref targetSession, sizeof(uint)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation for launch failed");
            }

            if (!CreateEnvironmentBlock(out environment, primaryToken, false))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateEnvironmentBlock failed");
            }

            string windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            string cmd = Path.Combine(windows, "System32", "cmd.exe");
            string escapedResult = resultPath.Replace("\"", "\"\"");
            StringBuilder commandLine = new StringBuilder(
                "cmd.exe /d /c \"(whoami /user & echo SessionId=" + targetSession + ") > \"\"" + escapedResult + "\"\" 2>&1\"");
            StartupInfo startupInfo = new StartupInfo();
            startupInfo.cb = Marshal.SizeOf(typeof(StartupInfo));
            startupInfo.lpDesktop = "winsta0\\default";

            if (!CreateProcessAsUser(
                    primaryToken,
                    cmd,
                    commandLine,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    false,
                    CreateUnicodeEnvironment | CreateNoWindow,
                    environment,
                    Path.GetDirectoryName(resultPath),
                    ref startupInfo,
                    out processInformation))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessAsUser failed");
            }

            log.Add("Launch.ProcessId=" + processInformation.dwProcessId);
            return processInformation.dwProcessId;
        }
        finally
        {
            if (processInformation.hThread != IntPtr.Zero) CloseHandle(processInformation.hThread);
            if (processInformation.hProcess != IntPtr.Zero) CloseHandle(processInformation.hProcess);
            if (environment != IntPtr.Zero) DestroyEnvironmentBlock(environment);
            if (primaryToken != IntPtr.Zero) CloseHandle(primaryToken);
        }
    }

    private static int LaunchProcess(
        IntPtr sessionToken,
        uint targetSession,
        string application,
        string commandLine,
        string workingDirectory,
        List<string> log)
    {
        IntPtr primaryToken = IntPtr.Zero;
        IntPtr environment = IntPtr.Zero;
        ProcessInformation processInformation = new ProcessInformation();
        try
        {
            if (!DuplicateTokenEx(
                    sessionToken,
                    MaximumAllowed,
                    IntPtr.Zero,
                    SecurityImpersonation,
                    TokenPrimary,
                    out primaryToken))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateTokenEx for direct launch failed");
            }
            if (!SetTokenInformation(primaryToken, TokenSessionId, ref targetSession, sizeof(uint)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation for direct launch failed");
            }
            if (!CreateEnvironmentBlock(out environment, primaryToken, false))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateEnvironmentBlock for direct launch failed");
            }

            StartupInfo startupInfo = new StartupInfo();
            startupInfo.cb = Marshal.SizeOf(typeof(StartupInfo));
            startupInfo.lpDesktop = "winsta0\\default";
            StringBuilder mutableCommandLine = new StringBuilder(commandLine);
            if (!CreateProcessAsUser(
                    primaryToken,
                    application,
                    mutableCommandLine,
                    IntPtr.Zero,
                    IntPtr.Zero,
                    false,
                    CreateUnicodeEnvironment,
                    environment,
                    workingDirectory,
                    ref startupInfo,
                    out processInformation))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcessAsUser direct launch failed");
            }

            uint launchedSession;
            if (!ProcessIdToSessionId(processInformation.dwProcessId, out launchedSession))
            {
                launchedSession = UInt32.MaxValue;
            }
            log.Add("DirectLaunch.ProcessId=" + processInformation.dwProcessId);
            log.Add("DirectLaunch.SessionId=" + launchedSession);
            log.Add("DirectLaunch.Application=" + application);
            uint waitResult = WaitForSingleObject(processInformation.hProcess, 10000);
            log.Add("DirectLaunch.WaitResult=0x" + waitResult.ToString("X8"));
            if (waitResult == WaitObject0)
            {
                uint exitCode;
                if (GetExitCodeProcess(processInformation.hProcess, out exitCode))
                {
                    log.Add("DirectLaunch.ExitCode=0x" + exitCode.ToString("X8"));
                }
                else
                {
                    log.Add("DirectLaunch.GetExitCodeError=" + Marshal.GetLastWin32Error());
                }
            }
            return processInformation.dwProcessId;
        }
        finally
        {
            if (processInformation.hThread != IntPtr.Zero) CloseHandle(processInformation.hThread);
            if (processInformation.hProcess != IntPtr.Zero) CloseHandle(processInformation.hProcess);
            if (environment != IntPtr.Zero) DestroyEnvironmentBlock(environment);
            if (primaryToken != IntPtr.Zero) CloseHandle(primaryToken);
        }
    }

    private static Dictionary<string, string> ParseArguments(string[] args)
    {
        Dictionary<string, string> values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < args.Length; index++)
        {
            if (!args[index].StartsWith("--", StringComparison.Ordinal) || index + 1 >= args.Length)
            {
                throw new ArgumentException("Expected --name value arguments.");
            }
            values[args[index]] = args[++index];
        }
        return values;
    }

    private static int Main(string[] args)
    {
        List<string> log = new List<string>();
        string resultPath = null;
        IntPtr process = IntPtr.Zero;
        IntPtr processToken = IntPtr.Zero;
        IntPtr sourceToken = IntPtr.Zero;
        IntPtr targetToken = IntPtr.Zero;
        IntPtr beforeToken = IntPtr.Zero;
        IntPtr afterToken = IntPtr.Zero;
        IntPtr wtsToken = IntPtr.Zero;

        try
        {
            Dictionary<string, string> values = ParseArguments(args);
            int sourceProcessId = int.Parse(values["--source-pid"]);
            uint expectedSourceSession = uint.Parse(values["--source-session"]);
            uint targetSession = uint.Parse(values["--target-session"]);
            resultPath = Path.GetFullPath(values["--result"]);
            string launchResultPath = Path.GetFullPath(values["--launch-result"]);

            log.Add("Proof.ProcessId=" + Process.GetCurrentProcess().Id);
            log.Add("Proof.SessionId=" + Process.GetCurrentProcess().SessionId);
            log.Add("Proof.Identity=" + WindowsIdentity.GetCurrent().Name);
            log.Add("Source.ProcessId=" + sourceProcessId);
            log.Add("Target.SessionId=" + targetSession);

            process = OpenProcess(ProcessQueryLimitedInformation, false, Process.GetCurrentProcess().Id);
            if (process == IntPtr.Zero || !OpenProcessToken(process, TokenQuery | TokenAdjustPrivileges, out processToken))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Opening proof process token failed");
            }
            EnablePrivilege(processToken, "SeDebugPrivilege", log);
            EnablePrivilege(processToken, "SeTcbPrivilege", log);
            EnablePrivilege(processToken, "SeAssignPrimaryTokenPrivilege", log);
            EnablePrivilege(processToken, "SeIncreaseQuotaPrivilege", log);

            uint actualSourceSession;
            if (!ProcessIdToSessionId(sourceProcessId, out actualSourceSession) || actualSourceSession != expectedSourceSession)
            {
                throw new InvalidOperationException(
                    "Source process session mismatch. expected=" + expectedSourceSession + " actual=" + actualSourceSession);
            }

            IntPtr sourceProcess = OpenProcess(ProcessQueryLimitedInformation, false, sourceProcessId);
            if (sourceProcess == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess(source) failed");
            }
            try
            {
                if (!OpenProcessToken(sourceProcess, TokenDuplicate | TokenQuery | TokenAssignPrimary, out sourceToken))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken(source) failed");
                }
            }
            finally
            {
                CloseHandle(sourceProcess);
            }

            log.Add("Source.Identity=" + TokenIdentity(sourceToken));
            log.Add("Source.BnoIsolation=" + TokenBnoIsolation(sourceToken));
            if (!DuplicateTokenEx(sourceToken, MaximumAllowed, IntPtr.Zero, SecurityImpersonation, TokenPrimary, out targetToken))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "DuplicateTokenEx(source) failed");
            }
            if (!SetTokenInformation(targetToken, TokenSessionId, ref targetSession, sizeof(uint)))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetTokenInformation(TokenSessionId) failed");
            }
            log.Add("TargetToken.Identity=" + TokenIdentity(targetToken));
            log.Add("TargetToken.BnoIsolation=" + TokenBnoIsolation(targetToken));

            string directApplication;
            if (values.TryGetValue("--direct-application", out directApplication))
            {
                string encodedArguments = values["--direct-arguments-base64"];
                string directArguments = Encoding.UTF8.GetString(Convert.FromBase64String(encodedArguments));
                string directWorkingDirectory = Path.GetFullPath(values["--direct-working-directory"]);
                LaunchProcess(
                    targetToken,
                    targetSession,
                    Path.GetFullPath(directApplication),
                    directArguments,
                    directWorkingDirectory,
                    log);
                log.Add("Proof.Status=success");
                return 0;
            }

            int beforeHr = UMgrQuerySessionUserToken(targetSession, out beforeToken);
            log.Add("UMgrQuery.Before.HResult=0x" + beforeHr.ToString("X8"));
            log.Add("UMgrQuery.Before.Identity=" + TokenIdentity(beforeToken));

            int changeHr = UMgrChangeSessionUserToken(targetToken);
            log.Add("UMgrChangeSessionUserToken.HResult=0x" + changeHr.ToString("X8"));
            if (changeHr < 0)
            {
                Marshal.ThrowExceptionForHR(changeHr);
            }

            int afterHr = UMgrQuerySessionUserToken(targetSession, out afterToken);
            log.Add("UMgrQuery.After.HResult=0x" + afterHr.ToString("X8"));
            log.Add("UMgrQuery.After.Identity=" + TokenIdentity(afterToken));
            if (afterHr < 0 || afterToken == IntPtr.Zero)
            {
                Marshal.ThrowExceptionForHR(afterHr);
            }

            bool wtsOk = WTSQueryUserToken(targetSession, out wtsToken);
            log.Add("WTSQueryUserToken.Success=" + wtsOk);
            log.Add("WTSQueryUserToken.Error=" + Marshal.GetLastWin32Error());
            log.Add("WTSQueryUserToken.Identity=" + TokenIdentity(wtsToken));

            LaunchIdentityProbe(afterToken, targetSession, launchResultPath, log);
            log.Add("Proof.Status=success");
            return 0;
        }
        catch (Exception exception)
        {
            log.Add("Proof.Status=error");
            log.Add("Proof.Exception=" + exception);
            return 1;
        }
        finally
        {
            if (resultPath != null)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(resultPath));
                File.WriteAllLines(resultPath, log.ToArray(), new UTF8Encoding(false));
            }
            if (wtsToken != IntPtr.Zero) CloseHandle(wtsToken);
            if (afterToken != IntPtr.Zero) CloseHandle(afterToken);
            if (beforeToken != IntPtr.Zero) CloseHandle(beforeToken);
            if (targetToken != IntPtr.Zero) CloseHandle(targetToken);
            if (sourceToken != IntPtr.Zero) CloseHandle(sourceToken);
            if (processToken != IntPtr.Zero) CloseHandle(processToken);
            if (process != IntPtr.Zero) CloseHandle(process);
        }
    }
}
