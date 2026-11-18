// User-mode SetupAPI glue derived from the existing Vibeshine targeted installer.
// Only an explicitly created, journal-marked root is removed. Driver packages are never removed.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace VibeSpeakers.Setup {
 public sealed class RootResult { public string InstanceId,Marker; public bool RebootRequired; }
 public static class UsbControllerSetup {
  public static bool LastCreateRolledBack {get;private set;}
  static readonly Guid Class=new Guid("36fc9e60-c465-11cf-8056-444553540000");
  [StructLayout(LayoutKind.Sequential)] struct Info { public uint Size;public Guid Class;public uint DevInst;public IntPtr Reserved; }
  [StructLayout(LayoutKind.Sequential)] struct Key { public Guid Format;public uint Id; }
  [DllImport("setupapi.dll",SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid guid,IntPtr parent);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiCreateDeviceInfoW(IntPtr set,string name,ref Guid guid,string description,IntPtr parent,uint flags,ref Info info);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiOpenDeviceInfoW(IntPtr set,string id,IntPtr parent,uint flags,ref Info info);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set,ref Info info,uint property,byte[] bytes,uint size);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr set,ref Info info,uint property,out uint type,byte[] bytes,uint size,out uint needed);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiSetDevicePropertyW(IntPtr set,ref Info info,ref Key key,uint type,byte[] bytes,uint size,uint flags);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set,ref Info info,StringBuilder text,uint chars,out uint required);
  [DllImport("setupapi.dll",SetLastError=true)] static extern bool SetupDiCallClassInstaller(uint function,IntPtr set,ref Info info);
  [DllImport("setupapi.dll")] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll",SetLastError=true)] static extern bool DiInstallDevice(IntPtr parent,IntPtr set,ref Info info,IntPtr driver,uint flags,out bool reboot);
  [DllImport("cfgmgr32.dll")] static extern uint CM_Get_Child(out uint child,uint parent,uint flags);
  static void Check(bool ok){if(!ok)throw new Win32Exception(Marshal.GetLastWin32Error());}
  static IntPtr NewSet(){Guid g=Class;IntPtr s=SetupDiCreateDeviceInfoList(ref g,IntPtr.Zero);if(s==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());return s;}
  static Info NewInfo(){return new Info{Size=(uint)Marshal.SizeOf(typeof(Info))};}
  static string Marker(string token){Guid g;if(!Guid.TryParseExact(token,"N",out g))throw new ArgumentException("Invalid installation nonce");return "Vibe Speakers "+g.ToString("N");}
  static string ReadMarker(IntPtr s,ref Info i){uint t,n;var b=new byte[1024];Check(SetupDiGetDeviceRegistryPropertyW(s,ref i,12,out t,b,(uint)b.Length,out n));if(t!=1||n<2||n>b.Length||n%2!=0)throw new InvalidOperationException("Invalid controller marker");return Encoding.Unicode.GetString(b,0,(int)n).TrimEnd('\0');}
  public static string CheckMarker(string id,string token){
   if(!System.Text.RegularExpressions.Regex.IsMatch(id,@"^ROOT\\USB\\[0-9A-Fa-f]{4,32}$"))throw new ArgumentException("Invalid exact controller instance");
   IntPtr s=NewSet();
   try {
    var i=NewInfo();
    if(!SetupDiOpenDeviceInfoW(s,id,IntPtr.Zero,0,ref i)){
     int error=Marshal.GetLastWin32Error();
     if(unchecked((uint)error)==0xE000020B)return null; // ERROR_NO_SUCH_DEVINST
     throw new Win32Exception(error);
    }
    string marker=ReadMarker(s,ref i);
    if(i.Class!=Class||marker!=Marker(token))throw new InvalidOperationException("Root is not owned by this installation; nothing changed");
    return marker;
   }finally{SetupDiDestroyDeviceInfoList(s);}
  }
  public static RootResult Create(string token,string sddl,Action<string> registeredJournal){
   string marker=Marker(token);var descriptor=UsbIpDeviceSecurity.FromSddl(sddl);
   var security=Convert.FromBase64String(descriptor.BytesBase64);
   IntPtr s=NewSet();bool registered=false,installed=false;string createdId=null;var i=NewInfo();LastCreateRolledBack=false;
   try{
    Guid g=Class;Check(SetupDiCreateDeviceInfoW(s,"USB",ref g,"Vibe Speakers transport",IntPtr.Zero,1,ref i));
    byte[] ids=Encoding.Unicode.GetBytes("ROOT\\USBIP_WIN2\\UDE\0\0");
    Check(SetupDiSetDeviceRegistryPropertyW(s,ref i,1,ids,(uint)ids.Length));
    byte[] name=Encoding.Unicode.GetBytes(marker+"\0");Check(SetupDiSetDeviceRegistryPropertyW(s,ref i,12,name,(uint)name.Length));
    var key=new Key{Format=new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"),Id=26};
    Check(SetupDiSetDevicePropertyW(s,ref i,ref key,20,security,(uint)security.Length,0));
    Check(SetupDiCallClassInstaller(0x19,s,ref i));registered=true;
    var id=new StringBuilder(512);uint required;Check(SetupDiGetDeviceInstanceIdW(s,ref i,id,(uint)id.Capacity,out required));
    createdId=id.ToString();registeredJournal(createdId);
    bool reboot;Check(DiInstallDevice(IntPtr.Zero,s,ref i,IntPtr.Zero,0,out reboot));
    UsbIpDeviceSecurity.Bind(id.ToString());UsbIpDeviceSecurity.Inspect();
    // Driver installation clears the pre-bind FriendlyName. Apply the ownership
    // marker after binding as well, on this exact newly created device handle.
    Check(SetupDiSetDeviceRegistryPropertyW(s,ref i,12,name,(uint)name.Length));
    if(ReadMarker(s,ref i)!=marker)throw new InvalidOperationException("Own marker changed during installation");
    installed=true;return new RootResult{InstanceId=id.ToString(),Marker=marker,RebootRequired=reboot};
   }finally{
    if(!registered)LastCreateRolledBack=true;
    if(registered&&!installed){
     // This is the private SetupAPI handle created above. It cannot refer to a
     // controller from an older journal, even when the instance-ID read failed.
     try{LastCreateRolledBack=SetupDiCallClassInstaller(5,s,ref i);}catch{}
     if(LastCreateRolledBack)UsbIpDeviceSecurity.ClearBinding();
    }
    SetupDiDestroyDeviceInfoList(s);
   }
  }
  public static void Remove(string id,string token,string expectedSddl){
   if(CheckMarker(id,token)==null)return; // Already absent, including after an interrupted removal.
   UsbIpDeviceSecurity.Bind(id);
   var before=UsbIpDeviceSecurity.InspectForRemoval();
   if(!UsbIpDeviceSecurity.Equivalent(before.Security,expectedSddl))throw new InvalidOperationException("Controller ACL changed; removal refused");
   if(before.Started&&UsbIpDeviceSecurity.ImportedCount()!=0)throw new InvalidOperationException("Controller has active imports; stop its sessions first");
   IntPtr s=NewSet();
   try {
    var i=NewInfo();
    Check(SetupDiOpenDeviceInfoW(s,id,IntPtr.Zero,0,ref i));
    if(i.Class!=Class||ReadMarker(s,ref i)!=Marker(token))throw new InvalidOperationException("Owner changed");
    if(!before.Started){
     uint child;
     const uint NoSuchDevNode=13;
     if(CM_Get_Child(out child,i.DevInst,0)!=NoSuchDevNode)throw new InvalidOperationException("Cannot confirm inactive controller has no child devices");
    }
    Check(SetupDiCallClassInstaller(5,s,ref i));
    UsbIpDeviceSecurity.ClearBinding();
   }finally{SetupDiDestroyDeviceInfoList(s);}
  }
 }
}
