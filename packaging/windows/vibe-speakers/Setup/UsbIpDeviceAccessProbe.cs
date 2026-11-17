using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace VibeSpeakers.Setup {
 public sealed class DeviceAccessProof {
  public bool Passed,NormalReadWriteOpenSucceeded,RestrictedReadWriteOpenDenied,ImpersonationReverted;
  public int NormalWin32Error,RestrictedWin32Error;
  public string InstanceId,RestrictingSid="S-1-1-0";
  public bool ActualDeviceOpenTest=true,ActualForeignLoginTest=false,IoctlSent=false;
 }
 public static class UsbIpDeviceAccessProbe {
  [StructLayout(LayoutKind.Sequential)] struct SidAttr { public IntPtr Sid;public uint Attributes; }
  [StructLayout(LayoutKind.Sequential)] struct Key { public Guid Format;public uint Id; }
  [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_Interface_List_SizeW(out uint chars,ref Guid guid,string instance,uint flags);
  [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_Interface_ListW(ref Guid guid,string instance,[Out] char[] buffer,uint chars,uint flags);
  [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_Interface_PropertyW(string path,ref Key key,out uint type,byte[] data,ref uint bytes,uint flags);
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFileW(string path,uint access,uint share,IntPtr sd,uint disposition,uint flags,IntPtr template);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool CreateRestrictedToken(IntPtr token,uint flags,uint disabled,IntPtr disables,uint deleted,IntPtr deletes,uint count,ref SidAttr sid,out IntPtr restricted);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool DuplicateToken(IntPtr token,int level,out IntPtr duplicate);
  [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool ConvertStringSidToSidW(string text,out IntPtr sid);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool ImpersonateLoggedOnUser(IntPtr token);
  [DllImport("advapi32.dll",SetLastError=true)] static extern bool RevertToSelf();
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  static void Check(bool ok){if(!ok)throw new Win32Exception(Marshal.GetLastWin32Error());}
  static string Path(){
   UsbIpDeviceSecurity.Inspect(); // existing exact instance/HWID/service/INF guard, before impersonation
   var guid=new Guid("b4030c06-dc5f-4fcc-87eb-e5515a0935c0");uint chars;
   for(int retry=0;retry<4;retry++){
    uint cr=CM_Get_Device_Interface_List_SizeW(out chars,ref guid,UsbIpDeviceSecurity.OwnedInstance,0);if(cr!=0||chars<2||chars>65536)throw new InvalidOperationException("Owned interface list size CR="+cr);
    var buffer=new char[chars];cr=CM_Get_Device_Interface_ListW(ref guid,UsbIpDeviceSecurity.OwnedInstance,buffer,chars,0);if(cr==26)continue;if(cr!=0)throw new InvalidOperationException("Owned interface list CR="+cr);
    var paths=new string(buffer).Split(new[]{'\0'},StringSplitOptions.RemoveEmptyEntries);if(paths.Length!=1)throw new InvalidOperationException("Expected exactly one owned interface");
    var key=new Key{Format=new Guid("78c34fc8-104a-4aca-9ea4-524d52996e57"),Id=256};uint type,bytes=4096;var id=new byte[bytes];cr=CM_Get_Device_Interface_PropertyW(paths[0],ref key,out type,id,ref bytes,0);
    if(cr!=0||type!=18||bytes<2||bytes>id.Length||(bytes%2)!=0||id[bytes-1]!=0||id[bytes-2]!=0||!String.Equals(Encoding.Unicode.GetString(id,0,(int)bytes).TrimEnd('\0'),UsbIpDeviceSecurity.OwnedInstance,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Interface instance binding mismatch");
    return paths[0];
   }throw new InvalidOperationException("Owned interface changed repeatedly");
  }
  public static DeviceAccessProof Run(){
   string path=Path();var proof=new DeviceAccessProof{InstanceId=UsbIpDeviceSecurity.OwnedInstance};
   using(var h=CreateFileW(path,0xc0000000,3,IntPtr.Zero,3,0,IntPtr.Zero)){proof.NormalWin32Error=h.IsInvalid?Marshal.GetLastWin32Error():0;proof.NormalReadWriteOpenSucceeded=!h.IsInvalid;}
   IntPtr process=IntPtr.Zero,restricted=IntPtr.Zero,impersonation=IntPtr.Zero,sid=IntPtr.Zero;bool active=false;
   try{
    Check(OpenProcessToken(GetCurrentProcess(),10,out process));Check(ConvertStringSidToSidW("S-1-1-0",out sid));var a=new SidAttr{Sid=sid,Attributes=0};Check(CreateRestrictedToken(process,1,0,IntPtr.Zero,0,IntPtr.Zero,1,ref a,out restricted));Check(DuplicateToken(restricted,2,out impersonation));
    Check(ImpersonateLoggedOnUser(impersonation));active=true;
    using(var h=CreateFileW(path,0xc0000000,3,IntPtr.Zero,3,0,IntPtr.Zero)){proof.RestrictedWin32Error=h.IsInvalid?Marshal.GetLastWin32Error():0;proof.RestrictedReadWriteOpenDenied=h.IsInvalid&&proof.RestrictedWin32Error==5;}
   }finally{
    if(active){if(!RevertToSelf())Environment.FailFast("Cannot revert USBIP access probe impersonation; terminating this probe process");proof.ImpersonationReverted=true;}
    if(impersonation!=IntPtr.Zero)CloseHandle(impersonation);if(restricted!=IntPtr.Zero)CloseHandle(restricted);if(process!=IntPtr.Zero)CloseHandle(process);if(sid!=IntPtr.Zero)LocalFree(sid);
   }
   proof.Passed=proof.NormalReadWriteOpenSucceeded&&proof.RestrictedReadWriteOpenDenied&&proof.ImpersonationReverted;return proof;
  }
 }
}
