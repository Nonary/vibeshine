using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

internal static class GrantSessionDesktopAccess
{
    private const uint ReadControl = 0x00020000;
    private const uint WriteDac = 0x00040000;
    private const uint WinStaAllAccess = 0x000F037F;
    private const uint DesktopAllAccess = 0x000F01FF;
    private const int SeWindowObject = 7;
    private const uint DaclSecurityInformation = 0x00000004;
    private const int GrantAccess = 1;
    private const int TrusteeIsSid = 0;
    private const int TrusteeIsUser = 1;
    private const uint SddlRevision1 = 1;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Trustee
    {
        public IntPtr pMultipleTrustee;
        public int MultipleTrusteeOperation;
        public int TrusteeForm;
        public int TrusteeType;
        public IntPtr ptstrName;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ExplicitAccess
    {
        public uint grfAccessPermissions;
        public int grfAccessMode;
        public uint grfInheritance;
        public Trustee Trustee;
    }

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenWindowStation(string name, bool inherit, uint desiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetProcessWindowStation(IntPtr windowStation);

    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr OpenDesktop(string name, uint flags, bool inherit, uint desiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseWindowStation(IntPtr handle);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseDesktop(IntPtr handle);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern uint GetSecurityInfo(
        IntPtr handle,
        int objectType,
        uint securityInformation,
        out IntPtr owner,
        out IntPtr group,
        out IntPtr dacl,
        out IntPtr sacl,
        out IntPtr securityDescriptor);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern uint SetSecurityInfo(
        IntPtr handle,
        int objectType,
        uint securityInformation,
        IntPtr owner,
        IntPtr group,
        IntPtr dacl,
        IntPtr sacl);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint SetEntriesInAcl(
        int count,
        [In] ExplicitAccess[] entries,
        IntPtr oldAcl,
        out IntPtr newAcl);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ConvertStringSidToSid(string stringSid, out IntPtr sid);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ConvertSecurityDescriptorToStringSecurityDescriptor(
        IntPtr securityDescriptor,
        uint revision,
        uint securityInformation,
        out IntPtr stringSecurityDescriptor,
        out uint stringSecurityDescriptorLength);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);

    private static string AddAccess(IntPtr handle, uint accessMask, IntPtr sid)
    {
        IntPtr owner;
        IntPtr group;
        IntPtr oldDacl;
        IntPtr sacl;
        IntPtr securityDescriptor;
        uint error = GetSecurityInfo(
            handle,
            SeWindowObject,
            DaclSecurityInformation,
            out owner,
            out group,
            out oldDacl,
            out sacl,
            out securityDescriptor);
        if (error != 0)
        {
            throw new InvalidOperationException("GetSecurityInfo failed: " + error);
        }

        IntPtr newDacl = IntPtr.Zero;
        try
        {
            ExplicitAccess entry = new ExplicitAccess();
            entry.grfAccessPermissions = accessMask;
            entry.grfAccessMode = GrantAccess;
            entry.grfInheritance = 0;
            entry.Trustee.TrusteeForm = TrusteeIsSid;
            entry.Trustee.TrusteeType = TrusteeIsUser;
            entry.Trustee.ptstrName = sid;

            error = SetEntriesInAcl(1, new[] { entry }, oldDacl, out newDacl);
            if (error != 0)
            {
                throw new InvalidOperationException("SetEntriesInAcl failed: " + error);
            }

            error = SetSecurityInfo(
                handle,
                SeWindowObject,
                DaclSecurityInformation,
                IntPtr.Zero,
                IntPtr.Zero,
                newDacl,
                IntPtr.Zero);
            if (error != 0)
            {
                throw new InvalidOperationException("SetSecurityInfo failed: " + error);
            }

            IntPtr sddl;
            uint sddlLength;
            if (!ConvertSecurityDescriptorToStringSecurityDescriptor(
                    securityDescriptor,
                    SddlRevision1,
                    DaclSecurityInformation,
                    out sddl,
                    out sddlLength))
            {
                return "access-added; original-sddl-error=" + Marshal.GetLastWin32Error();
            }
            try
            {
                return "access-added; original=" + Marshal.PtrToStringUni(sddl);
            }
            finally
            {
                LocalFree(sddl);
            }
        }
        finally
        {
            if (newDacl != IntPtr.Zero) LocalFree(newDacl);
            if (securityDescriptor != IntPtr.Zero) LocalFree(securityDescriptor);
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: grant_session_desktop_access <user-sid> <result-path>");
            return 2;
        }

        string sidText = args[0];
        string resultPath = Path.GetFullPath(args[1]);
        StringBuilder result = new StringBuilder();
        result.AppendLine("Identity=" + WindowsIdentity.GetCurrent().Name);
        result.AppendLine("SessionId=" + System.Diagnostics.Process.GetCurrentProcess().SessionId);
        result.AppendLine("GrantedSid=" + sidText);

        IntPtr sid = IntPtr.Zero;
        IntPtr windowStation = IntPtr.Zero;
        IntPtr desktop = IntPtr.Zero;
        try
        {
            if (!ConvertStringSidToSid(sidText, out sid))
            {
                throw new InvalidOperationException("ConvertStringSidToSid failed: " + Marshal.GetLastWin32Error());
            }

            windowStation = OpenWindowStation("WinSta0", false, ReadControl | WriteDac | WinStaAllAccess);
            if (windowStation == IntPtr.Zero)
            {
                throw new InvalidOperationException("OpenWindowStation failed: " + Marshal.GetLastWin32Error());
            }
            if (!SetProcessWindowStation(windowStation))
            {
                throw new InvalidOperationException("SetProcessWindowStation failed: " + Marshal.GetLastWin32Error());
            }

            desktop = OpenDesktop("default", 0, false, ReadControl | WriteDac | DesktopAllAccess);
            if (desktop == IntPtr.Zero)
            {
                throw new InvalidOperationException("OpenDesktop failed: " + Marshal.GetLastWin32Error());
            }

            result.AppendLine("WinSta0=" + AddAccess(windowStation, WinStaAllAccess, sid));
            result.AppendLine("Desktop=" + AddAccess(desktop, DesktopAllAccess, sid));
            result.AppendLine("Status=success");
            return 0;
        }
        catch (Exception exception)
        {
            result.AppendLine("Status=error");
            result.AppendLine("Exception=" + exception);
            return 1;
        }
        finally
        {
            Directory.CreateDirectory(Path.GetDirectoryName(resultPath));
            File.WriteAllText(resultPath, result.ToString(), new UTF8Encoding(false));
            if (desktop != IntPtr.Zero) CloseDesktop(desktop);
            if (windowStation != IntPtr.Zero) CloseWindowStation(windowStation);
            if (sid != IntPtr.Zero) LocalFree(sid);
        }
    }
}
