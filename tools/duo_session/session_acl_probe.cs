using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

internal static class SessionAclProbe
{
    private const uint WinStaEnumDesktops = 0x0001;
    private const uint WinStaReadAttributes = 0x0002;
    private const uint DesktopReadObjects = 0x0001;
    private const uint DesktopCreateWindow = 0x0002;
    private const uint DesktopWriteObjects = 0x0080;
    private const int UoiName = 2;

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenWindowStation(string name, bool inherit, uint desiredAccess);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenDesktop(string name, uint flags, bool inherit, uint desiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetProcessWindowStation();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetThreadDesktop(uint threadId);

    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetUserObjectInformation(
        IntPtr handle,
        int index,
        StringBuilder information,
        int length,
        out int needed);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseWindowStation(IntPtr handle);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseDesktop(IntPtr handle);

    private static string ObjectName(IntPtr handle)
    {
        StringBuilder name = new StringBuilder(256);
        int needed;
        return GetUserObjectInformation(handle, UoiName, name, name.Capacity * 2, out needed)
            ? name.ToString()
            : "error:" + Marshal.GetLastWin32Error();
    }

    private static string ProbeMutex(string name)
    {
        try
        {
            using (System.Threading.Mutex mutex = new System.Threading.Mutex(false, name))
            {
                return "success";
            }
        }
        catch (Exception exception)
        {
            return exception.GetType().Name + ":" + exception.Message;
        }
    }

    private static int Main(string[] args)
    {
        string resultPath = args.Length > 0
            ? Path.GetFullPath(args[0])
            : Path.Combine(Path.GetTempPath(), "vibeshine-session-acl-probe.txt");
        StringBuilder result = new StringBuilder();
        using (WindowsIdentity identity = WindowsIdentity.GetCurrent())
        {
            result.AppendLine("Identity=" + identity.Name);
            result.AppendLine("Sid=" + (identity.User == null ? "(none)" : identity.User.Value));
        }
        result.AppendLine("ProcessId=" + Process.GetCurrentProcess().Id);
        result.AppendLine("SessionId=" + Process.GetCurrentProcess().SessionId);
        result.AppendLine("UserInteractive=" + Environment.UserInteractive);

        IntPtr processWindowStation = GetProcessWindowStation();
        result.AppendLine("ProcessWindowStation=" + ObjectName(processWindowStation));
        IntPtr threadDesktop = GetThreadDesktop(GetCurrentThreadId());
        result.AppendLine("ThreadDesktop=" + ObjectName(threadDesktop));

        IntPtr openedWindowStation = OpenWindowStation(
            "WinSta0",
            false,
            WinStaEnumDesktops | WinStaReadAttributes);
        result.AppendLine("OpenWindowStation.Success=" + (openedWindowStation != IntPtr.Zero));
        result.AppendLine("OpenWindowStation.Error=" + Marshal.GetLastWin32Error());
        if (openedWindowStation != IntPtr.Zero) CloseWindowStation(openedWindowStation);

        IntPtr openedDesktop = OpenDesktop(
            "default",
            0,
            false,
            DesktopReadObjects | DesktopCreateWindow | DesktopWriteObjects);
        result.AppendLine("OpenDesktop.Success=" + (openedDesktop != IntPtr.Zero));
        result.AppendLine("OpenDesktop.Error=" + Marshal.GetLastWin32Error());
        if (openedDesktop != IntPtr.Zero) CloseDesktop(openedDesktop);

        result.AppendLine("Mutex.Local=" + ProbeMutex("Local\\VibeshineSeatAclProbe"));
        result.AppendLine("Mutex.Global=" + ProbeMutex("Global\\VibeshineSeatAclProbe"));

        Directory.CreateDirectory(Path.GetDirectoryName(resultPath));
        File.WriteAllText(resultPath, result.ToString(), new UTF8Encoding(false));
        return 0;
    }
}
