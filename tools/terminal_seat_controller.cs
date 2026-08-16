using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32.SafeHandles;

internal static class NativeMethods
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct SecurityAttributes
    {
        internal int Length;
        internal IntPtr SecurityDescriptor;
        [MarshalAs(UnmanagedType.Bool)] internal bool InheritHandle;
    }

    internal enum WtsConnectState
    {
        Active,
        Connected,
        ConnectQuery,
        Shadow,
        Disconnected,
        Idle,
        Listen,
        Reset,
        Down,
        Init,
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct WtsSessionInfo
    {
        internal uint SessionId;
        internal IntPtr WinStationName;
        internal WtsConnectState State;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool GetNamedPipeServerProcessId(SafePipeHandle pipe, out uint processId);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool ConvertStringSecurityDescriptorToSecurityDescriptor(
        string descriptor, uint revision, out IntPtr securityDescriptor, out uint size);

    [DllImport("kernel32.dll")]
    internal static extern IntPtr LocalFree(IntPtr memory);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern IntPtr CreateWindowStation(string name, uint flags, uint desiredAccess, ref SecurityAttributes attributes);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool SetProcessWindowStation(IntPtr windowStation);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    internal static extern IntPtr CreateDesktop(string name, IntPtr device, IntPtr deviceMode, uint flags, uint desiredAccess, ref SecurityAttributes attributes);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool SetThreadDesktop(IntPtr desktop);

    [DllImport("wtsapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WTSEnumerateSessions(
        IntPtr server,
        uint reserved,
        uint version,
        out IntPtr sessions,
        out uint count);

    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool WTSQuerySessionInformation(
        IntPtr server,
        uint sessionId,
        int infoClass,
        out IntPtr buffer,
        out uint bytes);

    [DllImport("wtsapi32.dll")]
    internal static extern void WTSFreeMemory(IntPtr memory);
}

internal sealed class RdpClientHost : AxHost
{
    internal RdpClientHost() : base("A0C63C30-F08D-4AB4-907C-34905D770C7D") { }

    internal object Client
    {
        get
        {
            CreateControl();
            return GetOcx();
        }
    }
}

internal sealed class ControllerForm : Form
{
    private const uint RequestMagic = 0x31525356; // VSR1
    private const uint ResponseMagic = 0x32525356; // VSR2
    private const int WtsUserName = 5;
    private const int MaximumFieldBytes = 512;
    private readonly RdpClientHost rdp = new RdpClientHost();
    private readonly System.Windows.Forms.Timer poll = new System.Windows.Forms.Timer();
    private readonly NamedPipeClientStream pipe;
    private readonly uint expectedBrokerPid;
    private readonly Stopwatch deadline = Stopwatch.StartNew();
    private string accountName;
    private bool responseSent;

    internal ControllerForm(string pipeName, uint brokerPid)
    {
        expectedBrokerPid = brokerPid;
        Text = "Vibeshine terminal seat controller";
        ShowInTaskbar = false;
        FormBorderStyle = FormBorderStyle.FixedToolWindow;
        StartPosition = FormStartPosition.Manual;
        Left = -32000;
        Top = -32000;
        Width = 64;
        Height = 64;
        rdp.Dock = DockStyle.Fill;
        Controls.Add(rdp);

        pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.None, TokenImpersonationLevel.Identification);
        poll.Interval = 250;
        poll.Tick += PollSession;
        Shown += StartConnection;
        FormClosed += delegate
        {
            poll.Stop();
            TryDisconnect();
            pipe.Dispose();
        };
    }

    private static void SetProperty(object target, string name, object value)
    {
        target.GetType().InvokeMember(name, BindingFlags.SetProperty, null, target, new object[] { value });
    }

    private static void TrySetProperty(object target, string name, object value)
    {
        try { SetProperty(target, name, value); }
        catch (MissingMethodException) { }
        catch (COMException) { }
    }

    private static object GetProperty(object target, string name)
    {
        return target.GetType().InvokeMember(name, BindingFlags.GetProperty, null, target, null);
    }

    private static object Invoke(object target, string name, params object[] arguments)
    {
        return target.GetType().InvokeMember(name, BindingFlags.InvokeMethod, null, target, arguments);
    }

    private static byte[] ReadFrame(Stream stream)
    {
        byte[] header = ReadExact(stream, 4);
        int length = BitConverter.ToInt32(header, 0);
        if (length <= 0 || length > 4096) throw new InvalidDataException("Invalid broker frame length.");
        return ReadExact(stream, length);
    }

    private static byte[] ReadExact(Stream stream, int length)
    {
        byte[] result = new byte[length];
        int offset = 0;
        while (offset < result.Length)
        {
            int count = stream.Read(result, offset, result.Length - offset);
            if (count <= 0) throw new EndOfStreamException();
            offset += count;
        }
        return result;
    }

    private static string ReadString(BinaryReader reader)
    {
        ushort length = reader.ReadUInt16();
        if (length > MaximumFieldBytes) throw new InvalidDataException("Seat controller field is too large.");
        byte[] bytes = reader.ReadBytes(length);
        if (bytes.Length != length) throw new EndOfStreamException();
        try { return new UTF8Encoding(false, true).GetString(bytes); }
        finally { Array.Clear(bytes, 0, bytes.Length); }
    }

    private void StartConnection(object sender, EventArgs eventArgs)
    {
        string password = null;
        byte[] request = null;
        try
        {
            pipe.Connect(5000);
            uint serverPid;
            if (!NativeMethods.GetNamedPipeServerProcessId(pipe.SafePipeHandle, out serverPid) || serverPid != expectedBrokerPid)
                throw new UnauthorizedAccessException("Seat controller broker identity mismatch.");

            request = ReadFrame(pipe);
            using (MemoryStream memory = new MemoryStream(request, false))
            using (BinaryReader reader = new BinaryReader(memory, Encoding.UTF8, true))
            {
                if (reader.ReadUInt32() != RequestMagic || reader.ReadUInt16() != 1)
                    throw new InvalidDataException("Unsupported seat controller request.");
                string server = ReadString(reader);
                ushort port = reader.ReadUInt16();
                string domain = ReadString(reader);
                accountName = ReadString(reader);
                password = ReadString(reader);
                ushort width = reader.ReadUInt16();
                ushort height = reader.ReadUInt16();
                if (memory.Position != memory.Length || string.IsNullOrWhiteSpace(server) || port == 0 ||
                    string.IsNullOrWhiteSpace(accountName) || string.IsNullOrEmpty(password))
                    throw new InvalidDataException("Incomplete seat controller request.");

                object client = rdp.Client;
                SetProperty(client, "Server", server);
                SetProperty(client, "Domain", domain);
                SetProperty(client, "UserName", accountName);
                SetProperty(client, "DesktopWidth", Math.Max(640, (int)width));
                SetProperty(client, "DesktopHeight", Math.Max(480, (int)height));
                SetProperty(client, "ColorDepth", 32);
                object advanced = GetProperty(client, "AdvancedSettings7");
                SetProperty(advanced, "RDPPort", (int)port);
                SetProperty(advanced, "EnableCredSspSupport", true);
                SetProperty(advanced, "ClearTextPassword", password);
                // Refuse the connection if the listener cannot authenticate for the
                // local machine's DNS name. This controller has no interactive UI in
                // which a certificate warning could be reviewed safely.
                SetProperty(advanced, "AuthenticationLevel", 1);
                TrySetProperty(advanced, "PromptForCredentials", false);
                TrySetProperty(advanced, "PromptForCredsOnClient", false);
                SetProperty(advanced, "NegotiateSecurityLayer", true);
                TrySetProperty(advanced, "RedirectDrives", false);
                TrySetProperty(advanced, "RedirectPrinters", false);
                TrySetProperty(advanced, "RedirectSmartCards", false);
                TrySetProperty(advanced, "RedirectClipboard", false);
                // Keep the managed seat's Remote Audio render endpoint alive;
                // microphone/device capture is never redirected into the seat.
                TrySetProperty(advanced, "AudioRedirectionMode", 0);
                TrySetProperty(advanced, "AudioCaptureRedirectionMode", false);
                Invoke(client, "Connect");
            }
            password = null;
            poll.Start();
            ThreadPool.QueueUserWorkItem(WaitForStopCommand);
        }
        catch (Exception exception)
        {
            SendResponse(false, 0, exception.GetType().Name + ": " + exception.Message);
            BeginInvoke((MethodInvoker)Close);
        }
        finally
        {
            if (request != null) Array.Clear(request, 0, request.Length);
            password = null;
        }
    }

    private void WaitForStopCommand(object state)
    {
        try
        {
            byte[] command = ReadFrame(pipe);
            bool stop = command.Length == 1 && command[0] == 1;
            Array.Clear(command, 0, command.Length);
            if (stop && !IsDisposed) BeginInvoke((MethodInvoker)Close);
        }
        catch
        {
            if (!IsDisposed) BeginInvoke((MethodInvoker)Close);
        }
    }

    private void PollSession(object sender, EventArgs eventArgs)
    {
        uint sessionId;
        if (TryFindSession(accountName, out sessionId))
        {
            SendResponse(true, sessionId, null);
            poll.Stop();
            return;
        }
        if (deadline.Elapsed > TimeSpan.FromSeconds(45))
        {
            int reason = -1;
            try { reason = Convert.ToInt32(GetProperty(rdp.Client, "ExtendedDisconnectReason")); } catch { }
            SendResponse(false, 0, "No desktop-ready managed session appeared; RDP reason " + reason + ".");
            poll.Stop();
            BeginInvoke((MethodInvoker)Close);
        }
    }

    private static bool TryFindSession(string userName, out uint sessionId)
    {
        sessionId = 0;
        IntPtr sessions;
        uint count;
        if (!NativeMethods.WTSEnumerateSessions(IntPtr.Zero, 0, 1, out sessions, out count)) return false;
        try
        {
            int size = Marshal.SizeOf(typeof(NativeMethods.WtsSessionInfo));
            for (uint index = 0; index < count; ++index)
            {
                NativeMethods.WtsSessionInfo info = (NativeMethods.WtsSessionInfo)Marshal.PtrToStructure(
                    IntPtr.Add(sessions, checked((int)index * size)), typeof(NativeMethods.WtsSessionInfo));
                if (info.State != NativeMethods.WtsConnectState.Active && info.State != NativeMethods.WtsConnectState.Connected)
                    continue;
                IntPtr buffer;
                uint bytes;
                if (!NativeMethods.WTSQuerySessionInformation(IntPtr.Zero, info.SessionId, WtsUserName, out buffer, out bytes))
                    continue;
                try
                {
                    string current = Marshal.PtrToStringUni(buffer) ?? string.Empty;
                    if (string.Equals(current, userName, StringComparison.OrdinalIgnoreCase))
                    {
                        sessionId = info.SessionId;
                        return true;
                    }
                }
                finally { NativeMethods.WTSFreeMemory(buffer); }
            }
        }
        finally { NativeMethods.WTSFreeMemory(sessions); }
        return false;
    }

    private void SendResponse(bool accepted, uint sessionId, string error)
    {
        if (responseSent) return;
        responseSent = true;
        byte[] body;
        using (MemoryStream memory = new MemoryStream())
        using (BinaryWriter writer = new BinaryWriter(memory, Encoding.UTF8, true))
        {
            writer.Write(ResponseMagic);
            writer.Write((ushort)1);
            writer.Write((byte)(accepted ? 1 : 0));
            writer.Write(sessionId);
            byte[] message = Encoding.UTF8.GetBytes(error ?? string.Empty);
            if (message.Length > MaximumFieldBytes) Array.Resize(ref message, MaximumFieldBytes);
            writer.Write((ushort)message.Length);
            writer.Write(message);
            Array.Clear(message, 0, message.Length);
            body = memory.ToArray();
        }
        byte[] length = BitConverter.GetBytes(body.Length);
        pipe.Write(length, 0, length.Length);
        pipe.Write(body, 0, body.Length);
        pipe.Flush();
        Array.Clear(body, 0, body.Length);
    }

    private void TryDisconnect()
    {
        try
        {
            object client = rdp.Client;
            if (Convert.ToInt32(GetProperty(client, "Connected")) != 0) Invoke(client, "Disconnect");
        }
        catch { }
    }
}

internal static class Program
{
    private const uint WindowStationAllAccess = 0x000F037F;
    private const uint DesktopAllAccess = 0x000F01FF;
    private static IntPtr privateWindowStation;
    private static IntPtr privateDesktop;

    private static bool EnterPrivateDesktop()
    {
        IntPtr descriptor;
        uint descriptorSize;
        // Owner/group and the protected DACL are LocalSystem only. The ordinary
        // console user cannot enumerate, send window messages to, or inject UI
        // into the privileged RDP controller.
        if (!NativeMethods.ConvertStringSecurityDescriptorToSecurityDescriptor(
                "O:SYG:SYD:P(A;;GA;;;SY)", 1, out descriptor, out descriptorSize) || descriptor == IntPtr.Zero)
            return false;
        try
        {
            NativeMethods.SecurityAttributes attributes = new NativeMethods.SecurityAttributes
            {
                Length = Marshal.SizeOf(typeof(NativeMethods.SecurityAttributes)),
                SecurityDescriptor = descriptor,
                InheritHandle = false,
            };
            privateWindowStation = NativeMethods.CreateWindowStation(
                "VibeshineSeatController-" + Guid.NewGuid().ToString("N"), 0, WindowStationAllAccess, ref attributes);
            if (privateWindowStation == IntPtr.Zero || !NativeMethods.SetProcessWindowStation(privateWindowStation)) return false;
            privateDesktop = NativeMethods.CreateDesktop("Default", IntPtr.Zero, IntPtr.Zero, 0, DesktopAllAccess, ref attributes);
            return privateDesktop != IntPtr.Zero && NativeMethods.SetThreadDesktop(privateDesktop);
        }
        finally { NativeMethods.LocalFree(descriptor); }
    }

    [STAThread]
    private static int Main(string[] args)
    {
        string pipe = null;
        uint brokerPid = 0;
        foreach (string argument in args)
        {
            if (argument.StartsWith("--pipe=", StringComparison.Ordinal)) pipe = argument.Substring(7);
            else if (argument.StartsWith("--broker-pid=", StringComparison.Ordinal)) uint.TryParse(argument.Substring(13), out brokerPid);
        }
        if (string.IsNullOrWhiteSpace(pipe) || brokerPid == 0) return 2;
        if (!EnterPrivateDesktop()) return 3;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new ControllerForm(pipe, brokerPid));
        return 0;
    }
}
