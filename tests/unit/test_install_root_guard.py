"""Run the MSI guard's actual traversal against deterministic unsafe trees."""
from pathlib import Path
import argparse,subprocess,tempfile,xml.etree.ElementTree as ET
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');a=p.parse_args()
r=Path(__file__).resolve().parents[2]
ns={'w':'http://schemas.microsoft.com/wix/2006/wi'}
custom=ET.parse(r/'packaging/windows/wix/custom_actions.wxs')
assert custom.find('.//w:Binary[@Id="InstallRootGuard"]',ns).attrib['SourceFile'].endswith('sunshine_install_guard.dll')
guard=custom.find('.//w:CustomAction[@Id="ValidateInstallRoot"]',ns)
assert guard.attrib['Return']=='check' and guard.attrib['Execute']=='immediate'
seq=ET.parse(r/'packaging/windows/wix/patch_custom_actions.wxs')
assert seq.find('.//Custom[@Action="ValidateInstallRootPreview"]').attrib['After']=='CostFinalize'
assert seq.find('.//Custom[@Action="ValidateInstallRootPreview"]').text=='1'
assert seq.find('.//Custom[@Action="ValidateInstallRoot"]').text=='1'
assert seq.find('.//Custom[@Action="SetKillProcsQuietImmediate"]').attrib['After']=='ValidateInstallRootPreview'
assert seq.find('.//Custom[@Action="ValidateInstallRoot"]').attrib['After']=='StopSvcQuietImmediate'
assert seq.find('.//Custom[@Action="RemoveConflictingProducts"]').attrib['After']=='ValidateInstallRoot'
assert custom.find('.//w:CustomAction[@Id="BlockLegacySunshine"]',ns).get('Error')
for old in ('AskUninstallLegacySunshine','WaitForLegacyUninstall'):
 assert custom.find('.//w:CustomAction[@Id="'+old+'"]',ns) is None
 assert seq.find('.//Custom[@Action="'+old+'"]') is None
source=(r/'packaging/windows/install_root_guard.cpp').read_text()
assert 'component_path(code.c_str()' in source
assert '"MsiGetComponentPathExW"' in source
assert 'MSIINSTALLCONTEXT_MACHINE,path.data(),&length)' in source
assert 'component_state==INSTALLSTATE_LOCAL' in source
bootstrap=(r/'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()
preflight=bootstrap.split('private static InstallerResult UninstallCompetingProducts(',1)[1]
assert preflight.index('product => product.IsPerUser') < preflight.index('TryPreserveKnownMsiSourceBeforeReplacement')

for contract in ('FILE_FLAG_OPEN_REPARSE_POINT','FILE_SHARE_READ | (preview ? FILE_SHARE_WRITE : 0),nullptr,OPEN_EXISTING','GetFileInformationByHandle','FILE_ATTRIBUTE_REPARSE_POINT','GetSecurityInfo','INHERIT_ONLY_ACE','READ_CONTROL','MsiGetPropertyW','SHGetKnownFolderPath','pins.push_back'):
 assert contract in source,contract
for contract in ('GetFinalPathNameByHandleW(file,resolved,50,FILE_NAME_NORMALIZED|VOLUME_NAME_GUID)',
                 'path.size()==3', 'identity=is_volume_root(file->value,info)', 'same_file_identity(',
                 'identity_attempted?L"identity_unproven":L"untrusted_allow"'):
 assert contract in source,contract
log=source.split('static std::wstring failure_detail(',1)[1].split('static UINT validate_root(',1)[0]
for field in ('phase=', 'category=', 'role=', 'reason=', 'api=', 'status=', 'access='):
 assert field in log,field
for secret in ('SidStart', 'descriptor', 'exception.what()'):
 assert secret not in log,secret
assert 'catch(...) {}' in log
for category in ('selected','installed','related','conflicting'):
 assert 'category=L"'+category+'"' in source
code=r'''
#include "packaging/windows/install_root_guard.h"
#include <map>
#include <set>
#include <iostream>
using namespace install_root_guard;
struct Node{bool directory=true,unsafe=false,reparse=false,mutable_handle=false;std::uint32_t mask=0;std::wstring final_path={};bool same_identity=true;};
struct FS{
 std::map<std::wstring,Node> nodes;std::map<std::wstring,std::vector<std::wstring>> listing;
 std::set<std::wstring> pinned;
 node_t pin(const std::wstring&p,bool strict,role_t role){
  auto it=nodes.find(p);if(it==nodes.end())return {false,false,2};
  if(it->second.unsafe)throw failure_t(L"untrusted_allow",role,L"none",0,0x10000);
  if(it->second.reparse)throw failure_t(L"reparse",role);
  if(it->second.mutable_handle)throw failure_t(L"open",role,L"CreateFileW",32);
  const bool root_identity=p.size()==3 && is_volume_guid_root(it->second.final_path) && it->second.same_identity;
  if(!allow_ace_safe(it->second.mask,false,strict,false,false,root_identity))throw std::runtime_error("unsafe ACL");
  if(p==L"c:\\program files\\vendor")if(!strict)throw std::runtime_error("higher Program Files ancestor must be strict");
  pinned.insert(p);return {true,it->second.directory};
 }
 std::vector<std::wstring> children(const std::wstring&p,role_t){if(!pinned.contains(p))throw std::runtime_error("un-pinned enumeration");return listing[p];}
};
void require(bool b){if(!b)throw std::runtime_error("guard regression");}
FS baseline(){FS f;f.nodes[L"c:\\"]={};f.nodes[L"c:\\program files"]={};f.nodes[L"c:\\program files\\vendor"]={};return f;}
bool pass(FS &f,bool missing=true){try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",missing);return true;}catch(const std::exception&){return false;}}
int main(){
 require(critical(failure_t(L"reparse")));
 require(critical(failure_t(L"untrusted_allow")));
 require(critical(failure_t(L"owner_or_dacl")));
 require(!critical(failure_t(L"unsupported_ace")));
 require(!critical(failure_t(L"open",role_t::descendant,L"CreateFileW",32)));
 require(!critical(failure_t(L"installed_location_unresolved")));
 require(!critical(failure_t(L"known_folder")));
 require(!critical(failure_t(L"bounds")));
 require(critical(failure_t(L"identity_unproven")));
 require(!critical(failure_t(L"identity_unproven",role_t::drive_ancestor,L"GetFinalPathNameByHandleW",5)));

 anchor_diagnostic_t first_success;first_success.record(true,0);first_success.record(false,123);
 require(std::wstring(first_success.rejection(false,false))==L"known_folder" && first_success.status==123);
 require(first_success.rejection(true,false)==nullptr && first_success.rejection(false,true)==nullptr);
 anchor_diagnostic_t first_failure;first_failure.record(false,456);first_failure.record(true,0);
 require(std::wstring(first_failure.rejection(false,false))==L"known_folder" && first_failure.status==456);
 require(first_failure.rejection(true,false)==nullptr && first_failure.rejection(false,true)==nullptr);
 anchor_diagnostic_t both_success;both_success.record(true,0);both_success.record(true,0);
 require(std::wstring(both_success.rejection(false,false))==L"outside_program_files");
 require(allow_ace_safe(0x1200a9,false,true,false,false));
 require(!allow_ace_safe(0x1301bf,true,true,false,false));
 require(allow_ace_safe(0x1f01ff,true,true,false,true));
 require(!allow_ace_safe(0x1f01ff,false,true,false,true));
 require(!allow_ace_safe(0x40,false,false,false,false));
 require(allow_ace_safe(0x4,false,false,false,false));
 const std::wstring volume=L"\\\\?\\Volume{12345678-1234-1234-1234-123456789abc}\\";
 require(is_volume_guid_root(volume));
 require(!is_volume_guid_root(volume+L"ordinary-directory"));
 require(!is_volume_guid_root(volume.substr(0,volume.size()-1)));
 require(!is_volume_guid_root(L"c:\\"));
 require(!is_volume_guid_root(L""));
 require(!is_volume_guid_root(L"\\\\?\\Volume{12345678-1234-1234-1234-123456789abz}\\"));
 require(!is_volume_guid_root(L"\\\\?\\Volume{12345678_1234-1234-1234-123456789abc}\\"));
 require(same_file_identity(1,0,5,1,0,5));
 require(!same_file_identity(0,0,5,0,0,5));
 require(!same_file_identity(1,0,0,1,0,0));
 require(!same_file_identity(1,0,5,2,0,5));
 require(!same_file_identity(1,0,5,1,0,6));
 require(!same_file_identity(1,1,5,1,0,5));
 require(!allow_ace_safe(0x00010000,false,false,false,false));
 require(allow_ace_safe(0x00010000,false,false,false,false,true));
 require(!allow_ace_safe(0x00010000,false,true,false,false,true));
 require(allow_ace_safe(0x1301bf,false,false,false,false,true));
 require(!allow_ace_safe(0x1301bf,false,false,false,false));
 require(!allow_ace_safe(0x1301bf,false,true,false,false,true));
 for(auto bit : {0x40u,0x40000u,0x80000u,0x10000000u,0x40000000u}) {
   require(!allow_ace_safe(bit,false,false,false,false,true));
   require(!allow_ace_safe(0x10000u|bit,false,false,false,false,true));
 }
 for(auto bit : {0x2u,0x4u,0x10u,0x100u})
   require(!allow_ace_safe(bit,false,true,false,false,true));
 // Exhaust relevant rights and flag combinations against the original
 // predicate. The only permitted difference is DELETE on a verified root.
 const std::uint32_t bits[]={1,2,4,16,64,256,0x10000,0x40000,0x80000,0x10000000,0x40000000};
 for(unsigned selected=0;selected<(1u<<11);++selected) {
   std::uint32_t mask=0;for(unsigned i=0;i<11;++i)if(selected&(1u<<i))mask|=bits[i];
   for(bool inherit_only : {false,true})for(bool strict : {false,true})
   for(bool trusted : {false,true})for(bool creator : {false,true}) {
     auto original=[&](std::uint32_t m){
       if(inherit_only&&!strict)return true;
       if(trusted||(inherit_only&&creator))return true;
       return !(m&(0x000d0040u|(strict?0x116u:0u)|0x50000000u));
     };
     require(allow_ace_safe(mask,inherit_only,strict,trusted,creator)==original(mask));
     require(allow_ace_safe(mask,inherit_only,strict,trusted,creator,true)==original(strict?mask:(mask&~0x10000u)));
   }
 }

 auto root_acl=baseline();root_acl.nodes[L"c:\\"].mask=0x1301bf;
 root_acl.nodes[L"c:\\"].final_path=volume;require(pass(root_acl));
 root_acl.nodes[L"c:\\"].final_path=volume+L"ordinary-directory";require(!pass(root_acl)); // SUBST-like alias
 root_acl.nodes[L"c:\\"].final_path=L"";require(!pass(root_acl)); // failed/unknown identity
 root_acl.nodes[L"c:\\"].final_path=volume;root_acl.nodes[L"c:\\"].same_identity=false;require(!pass(root_acl));
 root_acl.nodes[L"c:\\"].same_identity=true;
 root_acl.nodes[L"c:\\"].final_path=volume;root_acl.nodes[L"c:\\"].reparse=true;require(!pass(root_acl));
 root_acl=baseline();root_acl.nodes[L"c:\\program files"].mask=0x10000;
 root_acl.nodes[L"c:\\program files"].final_path=volume;require(!pass(root_acl)); // ordinary ancestor remains strict

 auto f=baseline();require(pass(f));require(f.pinned.contains(L"c:\\"));require(!pass(f,false)); // fresh vs maintenance missing target
 try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",false);require(false);}
 catch(const failure_t &e){require(e.role==role_t::install_root && std::wstring(e.reason)==L"missing" && e.status==2 && std::wstring(e.api)==L"CreateFileW");}
 f=baseline();f.nodes[L"c:\\"].mutable_handle=true;
 try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",true);require(false);}
 catch(const failure_t &e){require(e.role==role_t::drive_ancestor && std::wstring(e.reason)==L"open" && std::wstring(e.api)==L"CreateFileW" && e.status==32);}
 f=baseline();f.nodes[L"c:\\program files"].reparse=true;
 try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",true);require(false);}
 catch(const failure_t &e){require(e.role==role_t::anchor && std::wstring(e.reason)==L"reparse");}
 f=baseline();f.nodes[L"c:\\program files\\vendor"].unsafe=true;
 try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",true);require(false);}
 catch(const failure_t &e){require(e.role==role_t::ancestor && e.access==0x10000);}
 f=baseline();f.nodes[L"c:\\"].mutable_handle=true;f.nodes[L"c:\\program files"].reparse=true;
 try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",true);require(false);}
 catch(const failure_t &e){require(e.role==role_t::drive_ancestor && std::wstring(e.reason)==L"open");} // first rejection
 f=baseline();f.nodes[std::wstring(L"c:")+wchar_t(92)].unsafe=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].unsafe=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].reparse=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].mutable_handle=true;require(!pass(f));
 f=baseline();const std::wstring root=L"c:\\program files\\vendor\\host",script=root+L"\\script.ps1";
 f.nodes[root]={};f.nodes[script]={false};f.listing[root]={script};require(pass(f,false));require(f.pinned.contains(script));
 f.nodes[script].mutable_handle=true;
 try{validate(f,root,L"c:\\program files",false);require(false);}
 catch(const failure_t &e){require(e.role==role_t::descendant && e.status==32 && std::wstring(e.api)==L"CreateFileW");}
 f.nodes[script].mutable_handle=false;
 f.nodes[script].unsafe=true;require(!pass(f));f.nodes[script].unsafe=false;f.nodes[script].reparse=true;require(!pass(f));
 f.nodes[script].reparse=false;f.nodes[script].mutable_handle=true;require(!pass(f));
 f=baseline();f.nodes[root]={};f.listing[root]={script};require(!pass(f)); // disappearing enumeration entry
 bool escaped=false;try{validate(f,L"d:\\host",L"c:\\program files",true);escaped=true;}catch(...){}require(!escaped);
 std::cout<<"MSI guard: fresh/maintenance, all ancestors, nested files, reparse, mutable handles, disappearing entries and bounds passed\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'guard.cpp';exe=Path(tmp)/'guard';cpp.write_text(code)
 subprocess.run([a.compiler,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-I',str(r),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
