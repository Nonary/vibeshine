using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

internal static class DisplayProbe
{
    private const int SmCxScreen = 0;
    private const int SmCyScreen = 1;

    [Flags]
    private enum DisplayDeviceStateFlags : int
    {
        AttachedToDesktop = 0x1,
        PrimaryDevice = 0x4,
        MirroringDriver = 0x8,
        VgaCompatible = 0x10,
        Removable = 0x20,
        ModesPruned = 0x8000000,
        Remote = 0x4000000,
        Disconnect = 0x2000000
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct DisplayDevice
    {
        public int cb;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceString;

        public DisplayDeviceStateFlags StateFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceId;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceKey;
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumDisplayDevices(
        string device,
        int deviceNumber,
        ref DisplayDevice displayDevice,
        int flags);

    private static int Main()
    {
        Console.WriteLine("ProcessId={0}", Process.GetCurrentProcess().Id);
        Console.WriteLine("SessionId={0}", Process.GetCurrentProcess().SessionId);
        Console.WriteLine("User={0}", Environment.UserDomainName + "\\" + Environment.UserName);
        Console.WriteLine("Desktop={0}x{1}", GetSystemMetrics(SmCxScreen), GetSystemMetrics(SmCyScreen));

        int deviceIndex = 0;
        while (true)
        {
            DisplayDevice device = new DisplayDevice();
            device.cb = Marshal.SizeOf(typeof(DisplayDevice));
            if (!EnumDisplayDevices(null, deviceIndex, ref device, 0))
            {
                break;
            }

            Console.WriteLine("Display[{0}].Name={1}", deviceIndex, device.DeviceName);
            Console.WriteLine("Display[{0}].String={1}", deviceIndex, device.DeviceString);
            Console.WriteLine("Display[{0}].Flags={1}", deviceIndex, device.StateFlags);
            Console.WriteLine("Display[{0}].Id={1}", deviceIndex, device.DeviceId);

            deviceIndex++;
        }

        Console.WriteLine("DisplayCount={0}", deviceIndex);
        return deviceIndex > 0 ? 0 : 1;
    }
}
