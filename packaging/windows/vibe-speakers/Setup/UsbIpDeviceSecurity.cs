// Public per-device SecuritySDS only. No registry, class ACL, package or global device mutation.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace VibeSpeakers.Setup {
 public sealed class SecurityProperty { public bool Present; public uint Type; public string BytesBase64,Sddl; }
 public sealed class UsbSecuritySnapshot { public string InstanceId,Service,InfName,InfSha256; public string[] HardwareIds; public uint DeviceStatus,ProblemCode; public bool Started; public SecurityProperty Security; }
 public static class UsbIpDeviceSecurity {
  public static string OwnedInstance { get; private set; }
  internal static void ClearBinding() { OwnedInstance=null; }
  public static void Bind(string id) {
   if(!System.Text.RegularExpressions.Regex.IsMatch(id,@"^ROOT\\USB\\[0-9A-Fa-f]{4,32}$"))throw new ArgumentException("Invalid exact controller instance");
   if(OwnedInstance!=null&&!String.Equals(OwnedInstance,id,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Helper is already bound to another controller");
   OwnedInstance=id;
  }
  const string HardwareId=@"ROOT\USBIP_WIN2\UDE",ExpectedInfHash="1AE9E8DC497929DE1A6BE1291AE1DCA6A19E7FD6C0351E8C266F65DD91F15073";
  static readonly Guid Usb=new Guid("36fc9e60-c465-11cf-8056-444553540000"),Interface=new Guid("b4030c06-dc5f-4fcc-87eb-e5515a0935c0");
  [StructLayout(LayoutKind.Sequential)] struct Info { public uint Size;public Guid Class;public uint DevInst;public IntPtr Reserved; }
  [StructLayout(LayoutKind.Sequential)] struct IfInfo { public uint Size;public Guid Class;public uint Flags;public IntPtr Reserved; }
  [StructLayout(LayoutKind.Sequential)] struct Key { public Guid Format;public uint Id;public Key(string g,uint id){Format=new Guid(g);Id=id;} }
  static Key SecurityKey=new Key("a45c254e-df1c-4efd-8020-67d146a850e0",26);
  [DllImport("setupapi.dll",SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g,IntPtr p);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiOpenDeviceInfoW(IntPtr s,string id,IntPtr p,uint flags,ref Info i);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceInstanceIdW(IntPtr s,ref Info i,StringBuilder b,uint chars,out uint n);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceRegistryPropertyW(IntPtr s,ref Info i,uint prop,out uint type,byte[] b,uint size,out uint n);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDevicePropertyW(IntPtr s,ref Info i,ref Key k,out uint type,byte[] b,uint size,out uint n,uint flags);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr SetupDiGetClassDevsW(ref Guid g,string e,IntPtr p,uint flags);
  [DllImport("setupapi.dll",SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s,IntPtr d,ref Guid g,uint index,ref IfInfo i);
  [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr s,ref IfInfo i,IntPtr detail,uint size,out uint n,ref Info d);
  [DllImport("setupapi.dll",SetLastError=true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status,out uint problem,uint node,uint flags);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFileW(string n,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h,uint code,byte[] input,uint insize,byte[] output,uint outsize,out uint returned,IntPtr overlap);
  static void Check(bool ok){if(!ok)throw new Win32Exception(Marshal.GetLastWin32Error());}
  static Info NewInfo(){return new Info{Size=(uint)Marshal.SizeOf(typeof(Info))};}
  static IntPtr Open(out Info i){Guid g=Usb;IntPtr s=SetupDiCreateDeviceInfoList(ref g,IntPtr.Zero);if(s==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());i=NewInfo();if(!SetupDiOpenDeviceInfoW(s,OwnedInstance,IntPtr.Zero,0,ref i)){int e=Marshal.GetLastWin32Error();SetupDiDestroyDeviceInfoList(s);throw new Win32Exception(e);}return s;}
  static string Instance(IntPtr s,ref Info i){var b=new StringBuilder(1024);uint n;Check(SetupDiGetDeviceInstanceIdW(s,ref i,b,1024,out n));return b.ToString();}
  static string[] RegistryStrings(IntPtr s,ref Info i,uint prop){byte[] b=new byte[16384];uint type,n;Check(SetupDiGetDeviceRegistryPropertyW(s,ref i,prop,out type,b,(uint)b.Length,out n));if((type!=1&&type!=7)||n<2||n>b.Length||(n%2)!=0||b[n-1]!=0||b[n-2]!=0)throw new InvalidDataException("Invalid device identity string");return Encoding.Unicode.GetString(b,0,(int)n).Split(new[]{'\0'},StringSplitOptions.RemoveEmptyEntries);}
  static byte[] Property(IntPtr s,ref Info i,ref Key k,out uint type,out bool present){uint n;byte[] b=new byte[65536];if(!SetupDiGetDevicePropertyW(s,ref i,ref k,out type,b,(uint)b.Length,out n,0)){int e=Marshal.GetLastWin32Error();if(e==1168){present=false;type=0;return new byte[0];}throw new Win32Exception(e);}if(n>b.Length)throw new InvalidDataException("Property bound");present=true;Array.Resize(ref b,(int)n);return b;}
  static string Text(byte[] b){if(b.Length<2||b.Length%2!=0||b[b.Length-1]!=0||b[b.Length-2]!=0)throw new InvalidDataException("Property string termination");return Encoding.Unicode.GetString(b).TrimEnd('\0');}
  static string Hash(string p){using(var s=File.OpenRead(p))using(var h=System.Security.Cryptography.SHA256.Create())return BitConverter.ToString(h.ComputeHash(s)).Replace("-","");}
  static UsbSecuritySnapshot Inspect(IntPtr s,ref Info i,bool removal=false){
   string id=Instance(s,ref i);if(!String.Equals(id,OwnedInstance,StringComparison.OrdinalIgnoreCase)||i.Class!=Usb)throw new InvalidDataException("Not exact owned USB root");
   var ids=RegistryStrings(s,ref i,1);if(Array.FindIndex(ids,x=>String.Equals(x,HardwareId,StringComparison.OrdinalIgnoreCase))<0)throw new InvalidDataException("Wrong hardware ID");
   uint status,problem;
   uint cr=CM_Get_DevNode_Status(out status,out problem,i.DevInst,0);
   if(cr!=0)throw new InvalidOperationException("CM status CR="+cr);
   uint type;bool exists;
   var security=Property(s,ref i,ref SecurityKey,out type,out exists);
   if(exists&&type!=20)throw new InvalidDataException("SecuritySDS type mismatch");
   var snapshot=new UsbSecuritySnapshot{InstanceId=id,HardwareIds=ids,DeviceStatus=status,ProblemCode=problem,Started=(status&8)!=0,
    Security=new SecurityProperty{Present=exists,Type=type,BytesBase64=Convert.ToBase64String(security),Sddl=exists?Text(security):null}};
   // An inactive owned root may have no functioning driver, but its hardware
   // identity and exact ACL must still match. The caller also checks its children.
   if(removal&&!snapshot.Started)return snapshot;
   var services=RegistryStrings(s,ref i,4);if(services.Length!=1||!String.Equals(services[0],"usbip2_ude",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Wrong service");
   var infKey=new Key("a8b865dd-2e3d-4094-ad97-e593a70c75d6",5);var raw=Property(s,ref i,ref infKey,out type,out exists);if(!exists||type!=18)throw new InvalidDataException("Driver INF missing");string inf=Text(raw);
   if(!System.Text.RegularExpressions.Regex.IsMatch(inf,@"^oem[0-9]+\.inf$",System.Text.RegularExpressions.RegexOptions.IgnoreCase))throw new InvalidDataException("Invalid OEM INF");
   string hash=Hash(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"INF",inf));if(hash!=ExpectedInfHash)throw new InvalidDataException("Loaded INF binding differs from signed release");
   snapshot.Service=services[0];snapshot.InfName=inf;snapshot.InfSha256=hash;
   return snapshot;
  }
  public static UsbSecuritySnapshot Inspect(){Info i;IntPtr s=Open(out i);try{return Inspect(s,ref i);}finally{SetupDiDestroyDeviceInfoList(s);}}
  public static UsbSecuritySnapshot InspectForRemoval(){Info i;IntPtr s=Open(out i);try{return Inspect(s,ref i,true);}finally{SetupDiDestroyDeviceInfoList(s);}}
  public static string Sddl(string userSid){var sid=new SecurityIdentifier(userSid);if(!sid.IsAccountSid())throw new ArgumentException("Account SID required");return "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;"+sid.Value+")";}
  public static SecurityProperty FromSddl(string s){new RawSecurityDescriptor(s);return new SecurityProperty{Present=true,Type=20,Sddl=s,BytesBase64=Convert.ToBase64String(Encoding.Unicode.GetBytes(s+"\0"))};}
  public static bool Equivalent(SecurityProperty value,string expected){return value.Present&&value.Type==20&&new RawSecurityDescriptor(value.Sddl).GetSddlForm(AccessControlSections.All)==new RawSecurityDescriptor(expected).GetSddlForm(AccessControlSections.All);}
  static string InterfacePath(){
   Guid g=Interface;IntPtr s=SetupDiGetClassDevsW(ref g,null,IntPtr.Zero,0x12);if(s==new IntPtr(-1))throw new Win32Exception(Marshal.GetLastWin32Error());
   try{for(uint n=0;n<1024;n++){var f=new IfInfo{Size=(uint)Marshal.SizeOf(typeof(IfInfo))};if(!SetupDiEnumDeviceInterfaces(s,IntPtr.Zero,ref g,n,ref f)){int e=Marshal.GetLastWin32Error();if(e==259)break;throw new Win32Exception(e);}var d=NewInfo();uint bytes;SetupDiGetDeviceInterfaceDetailW(s,ref f,IntPtr.Zero,0,out bytes,ref d);if(Marshal.GetLastWin32Error()!=122||bytes<8||bytes>65536)throw new InvalidDataException("Interface detail size");IntPtr b=Marshal.AllocHGlobal((int)bytes);try{Marshal.WriteInt32(b,IntPtr.Size==8?8:6);Check(SetupDiGetDeviceInterfaceDetailW(s,ref f,b,bytes,out bytes,ref d));if(String.Equals(Instance(s,ref d),OwnedInstance,StringComparison.OrdinalIgnoreCase)){Inspect(s,ref d);return Marshal.PtrToStringUni(IntPtr.Add(b,4));}}finally{Marshal.FreeHGlobal(b);}}throw new InvalidOperationException("Owned controller interface unavailable");}finally{SetupDiDestroyDeviceInfoList(s);}
  }
  public static int ImportedCount(){
   string path=InterfacePath();using(var h=CreateFileW(path,0xc0000000,3,IntPtr.Zero,3,0,IntPtr.Zero)){if(h.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error());
    byte[] input=BitConverter.GetBytes(1136u),output=new byte[4+1132*1024];uint count;
    Check(DeviceIoControl(h,0x22e008,input,4,output,(uint)output.Length,out count,IntPtr.Zero)); // GET_IMPORTED_DEVICES 0x802, BUFFERED, RW
    if(count<4||count>output.Length||(count-4)%1132!=0)throw new InvalidDataException("USBIP ABI response");return (int)((count-4)/1132);
   }
  }
 }
}
