using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

internal static class GrantSessionNamedObjectAccess
{
    private const uint ReadControl = 0x00020000;
    private const uint WriteDac = 0x00040000;
    private const uint DirectoryAllAccess = 0x000F000F;
    private const uint ObjCaseInsensitive = 0x00000040;
    private const int SeKernelObject = 6;
    private const uint DaclSecurityInformation = 0x00000004;
    private const int GrantAccess = 1;
    private const int TrusteeIsSid = 0;
    private const int TrusteeIsUser = 1;
    private const uint SddlRevision1 = 1;

    [StructLayout(LayoutKind.Sequential)]
    private struct UnicodeString
    {
        public ushort Length;
        public ushort MaximumLength;
        public IntPtr Buffer;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ObjectAttributes
    {
        public int Length;
        public IntPtr RootDirectory;
        public IntPtr ObjectName;
        public uint Attributes;
        public IntPtr SecurityDescriptor;
        public IntPtr SecurityQualityOfService;
    }

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

    [DllImport("ntdll.dll")]
    private static extern int NtOpenDirectoryObject(
        out IntPtr directoryHandle,
        uint desiredAccess,
        ref ObjectAttributes objectAttributes);

    [DllImport("ntdll.dll")]
    private static extern int NtClose(IntPtr handle);

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

    private static string SecurityDescriptorToSddl(IntPtr securityDescriptor)
    {
        IntPtr sddl;
        uint length;
        if (!ConvertSecurityDescriptorToStringSecurityDescriptor(
                securityDescriptor,
                SddlRevision1,
                DaclSecurityInformation,
                out sddl,
                out length))
        {
            return "sddl-error:" + Marshal.GetLastWin32Error();
        }

        try
        {
            return Marshal.PtrToStringUni(sddl);
        }
        finally
        {
            LocalFree(sddl);
        }
    }

    private static string AddAccess(IntPtr handle, string sidText)
    {
        IntPtr sid = IntPtr.Zero;
        IntPtr securityDescriptor = IntPtr.Zero;
        IntPtr newDacl = IntPtr.Zero;
        try
        {
            if (!ConvertStringSidToSid(sidText, out sid))
            {
                throw new InvalidOperationException(
                    "ConvertStringSidToSid failed for " + sidText + ": " + Marshal.GetLastWin32Error());
            }

            IntPtr owner;
            IntPtr group;
            IntPtr oldDacl;
            IntPtr sacl;
            uint error = GetSecurityInfo(
                handle,
                SeKernelObject,
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

            string originalSddl = SecurityDescriptorToSddl(securityDescriptor);
            ExplicitAccess entry = new ExplicitAccess();
            entry.grfAccessPermissions = DirectoryAllAccess;
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
                SeKernelObject,
                DaclSecurityInformation,
                IntPtr.Zero,
                IntPtr.Zero,
                newDacl,
                IntPtr.Zero);
            if (error != 0)
            {
                throw new InvalidOperationException("SetSecurityInfo failed: " + error);
            }

            return "sid=" + sidText + ";original=" + originalSddl;
        }
        finally
        {
            if (newDacl != IntPtr.Zero) LocalFree(newDacl);
            if (securityDescriptor != IntPtr.Zero) LocalFree(securityDescriptor);
            if (sid != IntPtr.Zero) LocalFree(sid);
        }
    }

    private static IntPtr OpenDirectory(string path)
    {
        IntPtr nameBuffer = IntPtr.Zero;
        IntPtr unicodeStringPointer = IntPtr.Zero;
        try
        {
            nameBuffer = Marshal.StringToHGlobalUni(path);
            UnicodeString unicodeString = new UnicodeString();
            unicodeString.Length = checked((ushort) (path.Length * 2));
            unicodeString.MaximumLength = checked((ushort) ((path.Length + 1) * 2));
            unicodeString.Buffer = nameBuffer;
            unicodeStringPointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(UnicodeString)));
            Marshal.StructureToPtr(unicodeString, unicodeStringPointer, false);

            ObjectAttributes attributes = new ObjectAttributes();
            attributes.Length = Marshal.SizeOf(typeof(ObjectAttributes));
            attributes.ObjectName = unicodeStringPointer;
            attributes.Attributes = ObjCaseInsensitive;

            IntPtr handle;
            int status = NtOpenDirectoryObject(
                out handle,
                ReadControl | WriteDac,
                ref attributes);
            if (status < 0)
            {
                throw new InvalidOperationException(
                    "NtOpenDirectoryObject(" + path + ") failed: 0x" + status.ToString("X8"));
            }
            return handle;
        }
        finally
        {
            if (unicodeStringPointer != IntPtr.Zero) Marshal.FreeHGlobal(unicodeStringPointer);
            if (nameBuffer != IntPtr.Zero) Marshal.FreeHGlobal(nameBuffer);
        }
    }

    private static int Main(string[] args)
    {
        if (args.Length < 3)
        {
            Console.Error.WriteLine(
                "usage: grant_session_named_object_access <session-id> <result-path> <sid> [sid ...]");
            return 2;
        }

        string directoryPath = "\\Sessions\\" + args[0] + "\\BaseNamedObjects";
        string resultPath = Path.GetFullPath(args[1]);
        StringBuilder result = new StringBuilder();
        result.AppendLine("Identity=" + WindowsIdentity.GetCurrent().Name);
        result.AppendLine("ProcessSessionId=" + System.Diagnostics.Process.GetCurrentProcess().SessionId);
        result.AppendLine("Directory=" + directoryPath);

        IntPtr directory = IntPtr.Zero;
        try
        {
            directory = OpenDirectory(directoryPath);
            for (int index = 2; index < args.Length; ++index)
            {
                result.AppendLine("Grant=" + AddAccess(directory, args[index]));
            }
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
            if (directory != IntPtr.Zero) NtClose(directory);
        }
    }
}
