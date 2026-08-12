using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal static class NativeMethods
{
    [DllImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WTSEnableChildSessions([MarshalAs(UnmanagedType.Bool)] bool enable);

    [DllImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WTSIsChildSessionsEnabled([MarshalAs(UnmanagedType.Bool)] out bool enabled);

    [DllImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WTSGetChildSessionId(out uint childSessionId);
}

[ComImport]
[Guid("302D8188-0052-4807-806A-362B628F9AC5")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMsRdpExtendedSettings
{
    void set_Property(
        [In, MarshalAs(UnmanagedType.BStr)] string propertyName,
        [In, MarshalAs(UnmanagedType.Struct)] ref object value);

    [return: MarshalAs(UnmanagedType.Struct)]
    object get_Property([In, MarshalAs(UnmanagedType.BStr)] string propertyName);
}

internal sealed class RdpClientHost : AxHost
{
    // Microsoft RDP Client Control version 11, not safe for scripting.
    internal RdpClientHost()
        : base("A0C63C30-F08D-4AB4-907C-34905D770C7D")
    {
    }

    internal object Client
    {
        get
        {
            CreateControl();
            return GetOcx();
        }
    }
}

internal sealed class ProofForm : Form
{
    private readonly RdpClientHost rdpHost = new RdpClientHost();
    private readonly Timer pollTimer = new Timer();
    private readonly Stopwatch stopwatch = Stopwatch.StartNew();
    private readonly string resultPath;
    private bool completed;
    private int lastConnectedState = -1;
    private int extendedDisconnectReason = -1;

    internal ProofForm(string resultPath)
    {
        this.resultPath = resultPath;

        Text = "Vibeshine child-session proof";
        ShowInTaskbar = false;
        FormBorderStyle = FormBorderStyle.FixedToolWindow;
        StartPosition = FormStartPosition.Manual;
        Left = -32000;
        Top = -32000;
        Width = 64;
        Height = 64;

        rdpHost.Dock = DockStyle.Fill;
        Controls.Add(rdpHost);

        pollTimer.Interval = 250;
        pollTimer.Tick += PollChildSession;
        Shown += StartProof;
        FormClosed += delegate { pollTimer.Stop(); };
    }

    private static void SetComProperty(object target, string propertyName, object value)
    {
        target.GetType().InvokeMember(
            propertyName,
            BindingFlags.SetProperty,
            null,
            target,
            new object[] { value });
    }

    private static object GetComProperty(object target, string propertyName)
    {
        return target.GetType().InvokeMember(
            propertyName,
            BindingFlags.GetProperty,
            null,
            target,
            null);
    }

    private static object InvokeComMethod(object target, string methodName, params object[] arguments)
    {
        return target.GetType().InvokeMember(
            methodName,
            BindingFlags.InvokeMethod,
            null,
            target,
            arguments);
    }

    private void StartProof(object sender, EventArgs eventArgs)
    {
        try
        {
            bool enabled;
            if (!NativeMethods.WTSIsChildSessionsEnabled(out enabled))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WTSIsChildSessionsEnabled failed");
            }

            if (!enabled && !NativeMethods.WTSEnableChildSessions(true))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "WTSEnableChildSessions failed");
            }

            object client = rdpHost.Client;
            SetComProperty(client, "Server", "localhost");
            SetComProperty(client, "DesktopWidth", 1280);
            SetComProperty(client, "DesktopHeight", 720);
            SetComProperty(client, "ColorDepth", 32);

            object advancedSettings = GetComProperty(client, "AdvancedSettings7");
            SetComProperty(advancedSettings, "RDPPort", 3389);
            SetComProperty(advancedSettings, "EnableCredSspSupport", true);
            SetComProperty(advancedSettings, "RedirectDrives", false);
            SetComProperty(advancedSettings, "RedirectPrinters", false);
            SetComProperty(advancedSettings, "RedirectSmartCards", false);

            object connectToChildSession = true;
            ((IMsRdpExtendedSettings)client).set_Property(
                "ConnectToChildSession",
                ref connectToChildSession);

            WriteResult("connecting", null, 0);
            InvokeComMethod(client, "Connect");
            pollTimer.Start();
        }
        catch (Exception exception)
        {
            Complete("error", null, Marshal.GetHRForException(exception), exception.ToString());
        }
    }

    private void PollChildSession(object sender, EventArgs eventArgs)
    {
        try
        {
            object client = rdpHost.Client;
            lastConnectedState = Convert.ToInt32(GetComProperty(client, "Connected"));
            if (lastConnectedState == 0)
            {
                extendedDisconnectReason = Convert.ToInt32(GetComProperty(client, "ExtendedDisconnectReason"));
            }
        }
        catch
        {
            // WTS allocation is the proof boundary; retain the last COM state if
            // the control transiently rejects a status-property query.
        }

        uint childSessionId;
        if (NativeMethods.WTSGetChildSessionId(out childSessionId))
        {
            Complete("connected", childSessionId, 0, null);
            return;
        }

        int error = Marshal.GetLastWin32Error();
        if (stopwatch.Elapsed > TimeSpan.FromSeconds(45))
        {
            Complete("timeout", null, error, "No child session appeared before the deadline.");
        }
    }

    private void Complete(string status, uint? childSessionId, int error, string detail)
    {
        if (completed)
        {
            return;
        }

        completed = true;
        pollTimer.Stop();
        WriteResult(status, childSessionId, error, detail);

        if (status == "connected")
        {
            // Keep the ActiveX control and child session alive for runtime inspection.
            return;
        }

        BeginInvoke((MethodInvoker)Close);
    }

    private void WriteResult(string status, uint? childSessionId, int error)
    {
        WriteResult(status, childSessionId, error, null);
    }

    private void WriteResult(string status, uint? childSessionId, int error, string detail)
    {
        StringBuilder json = new StringBuilder();
        json.Append("{\r\n");
        json.AppendFormat("  \"status\": \"{0}\",\r\n", Escape(status));
        json.AppendFormat("  \"parent_process_id\": {0},\r\n", Process.GetCurrentProcess().Id);
        json.AppendFormat("  \"parent_session_id\": {0},\r\n", Process.GetCurrentProcess().SessionId);
        json.AppendFormat("  \"child_session_id\": {0},\r\n", childSessionId.HasValue ? childSessionId.Value.ToString() : "null");
        json.AppendFormat("  \"elapsed_ms\": {0},\r\n", stopwatch.ElapsedMilliseconds);
        json.AppendFormat("  \"rdp_connected_state\": {0},\r\n", lastConnectedState);
        json.AppendFormat("  \"extended_disconnect_reason\": {0},\r\n", extendedDisconnectReason);
        json.AppendFormat("  \"win32_or_hresult\": {0},\r\n", error);
        json.AppendFormat("  \"detail\": {0}\r\n", detail == null ? "null" : "\"" + Escape(detail) + "\"");
        json.Append("}\r\n");

        string directory = Path.GetDirectoryName(resultPath);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }
        File.WriteAllText(resultPath, json.ToString(), new UTF8Encoding(false));
    }

    private static string Escape(string value)
    {
        return value
            .Replace("\\", "\\\\")
            .Replace("\"", "\\\"")
            .Replace("\r", "\\r")
            .Replace("\n", "\\n");
    }
}

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        string resultPath = args.Length > 0
            ? Path.GetFullPath(args[0])
            : Path.Combine(Path.GetTempPath(), "vibeshine-child-session-proof.json");

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ProofForm(resultPath));

        return File.Exists(resultPath) && File.ReadAllText(resultPath).Contains("\"status\": \"connected\"")
            ? 0
            : 1;
    }
}
