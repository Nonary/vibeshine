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
assert seq.find('.//Custom[@Action="ValidateInstallRoot"]').attrib['After']=='StopSvcQuietImmediate'
assert seq.find('.//Custom[@Action="RemoveConflictingProducts"]').attrib['After']=='ValidateInstallRoot'
assert custom.find('.//w:CustomAction[@Id="BlockLegacySunshine"]',ns).get('Error')
for old in ('AskUninstallLegacySunshine','WaitForLegacyUninstall'):
 assert custom.find('.//w:CustomAction[@Id="'+old+'"]',ns) is None
 assert seq.find('.//Custom[@Action="'+old+'"]') is None
source=(r/'packaging/windows/install_root_guard.cpp').read_text()
assert 'component_path(code.c_str()' in source
assert '"MsiGetComponentPathExW"' in source
assert 'MSIINSTALLCONTEXT_MACHINE,path.data(),&length)==INSTALLSTATE_LOCAL' in source
bootstrap=(r/'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()
preflight=bootstrap.split('private static InstallerResult UninstallCompetingProducts(',1)[1]
assert preflight.index('product => product.IsPerUser') < preflight.index('TryPreserveKnownMsiSourceBeforeReplacement')

for contract in ('FILE_FLAG_OPEN_REPARSE_POINT','FILE_SHARE_READ | (preview ? FILE_SHARE_WRITE : 0),nullptr,OPEN_EXISTING','GetFileInformationByHandle','FILE_ATTRIBUTE_REPARSE_POINT','GetSecurityInfo','INHERIT_ONLY_ACE','READ_CONTROL','MsiGetPropertyW','SHGetKnownFolderPath','pins.push_back'):
 assert contract in source,contract
code=r'''
#include "packaging/windows/install_root_guard.h"
#include <map>
#include <set>
#include <iostream>
using namespace install_root_guard;
struct Node{bool directory=true,unsafe=false,reparse=false,mutable_handle=false;};
struct FS{
 std::map<std::wstring,Node> nodes;std::map<std::wstring,std::vector<std::wstring>> listing;
 std::set<std::wstring> pinned;
 node_t pin(const std::wstring&p,bool strict){
  auto it=nodes.find(p);if(it==nodes.end())return {false,false};
  if(it->second.unsafe||it->second.reparse||it->second.mutable_handle)throw std::runtime_error("unsafe");
  if(p==L"c:\\program files\\vendor")if(!strict)throw std::runtime_error("higher Program Files ancestor must be strict");
  pinned.insert(p);return {true,it->second.directory};
 }
 std::vector<std::wstring> children(const std::wstring&p){if(!pinned.contains(p))throw std::runtime_error("un-pinned enumeration");return listing[p];}
};
void require(bool b){if(!b)throw std::runtime_error("guard regression");}
FS baseline(){FS f;f.nodes[L"c:\\"]={};f.nodes[L"c:\\program files"]={};f.nodes[L"c:\\program files\\vendor"]={};return f;}
bool pass(FS &f,bool missing=true){try{validate(f,L"c:\\program files\\vendor\\host",L"c:\\program files",missing);return true;}catch(const std::exception&){return false;}}
int main(){
 require(allow_ace_safe(0x1200a9,false,true,false,false));
 require(!allow_ace_safe(0x1301bf,true,true,false,false));
 require(allow_ace_safe(0x1f01ff,true,true,false,true));
 require(!allow_ace_safe(0x1f01ff,false,true,false,true));
 require(!allow_ace_safe(0x40,false,false,false,false));
 require(allow_ace_safe(0x4,false,false,false,false));

 auto f=baseline();require(pass(f));require(f.pinned.contains(L"c:\\"));require(!pass(f,false)); // fresh vs maintenance missing target
 f=baseline();f.nodes[std::wstring(L"c:")+wchar_t(92)].unsafe=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].unsafe=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].reparse=true;require(!pass(f));
 f=baseline();f.nodes[L"c:\\program files\\vendor"].mutable_handle=true;require(!pass(f));
 f=baseline();const std::wstring root=L"c:\\program files\\vendor\\host",script=root+L"\\script.ps1";
 f.nodes[root]={};f.nodes[script]={false};f.listing[root]={script};require(pass(f,false));require(f.pinned.contains(script));
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
