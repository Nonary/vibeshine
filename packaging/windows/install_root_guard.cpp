#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msiquery.h>
#include <aclapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <cwctype>
#include <memory>
#include "install_root_guard.h"
namespace {
  struct handle_t {
    HANDLE value;
    explicit handle_t(HANDLE h):value(h) {}
    ~handle_t(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    handle_t(const handle_t&)=delete;
  };
  std::wstring property(MSIHANDLE session,const wchar_t *name) {
    DWORD size=0;wchar_t empty=0;
    UINT result=MsiGetPropertyW(session,name,&empty,&size);
    if(result!=ERROR_MORE_DATA && result!=ERROR_SUCCESS)throw std::runtime_error("Unable to read MSI property");
    std::wstring value(size+1,L'\0');++size;
    if(MsiGetPropertyW(session,name,value.data(),&size)!=ERROR_SUCCESS)throw std::runtime_error("Unable to read MSI property");
    value.resize(size);return value;
  }
  std::wstring lower(std::wstring s){for(auto &ch:s)ch=static_cast<wchar_t>(std::towlower(ch));return s;}
  bool trusted(PSID sid) {
    if(!sid || !IsValidSid(sid))return false;
    if(IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
    PSID installer=nullptr;
    if(!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&installer))return false;
    const bool ok=EqualSid(sid,installer)!=FALSE;LocalFree(installer);return ok;
  }
  struct filesystem_t {
    std::vector<std::unique_ptr<handle_t>> pins;
    bool preview;
    install_root_guard::node_t pin(const std::wstring &path,bool strict) {
      auto file=std::make_unique<handle_t>(CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | (preview ? FILE_SHARE_WRITE : 0),nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
      if(file->value==INVALID_HANDLE_VALUE){
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return {false,false};
        throw std::runtime_error("Install object is inaccessible or open for mutation");
      }
      BY_HANDLE_FILE_INFORMATION info{};
      if(!GetFileInformationByHandle(file->value,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Reparse or unreadable install object");
      PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      if(GetSecurityInfo(file->value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
          &owner,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)throw std::runtime_error("Install security is unreadable");
      auto free_descriptor=std::unique_ptr<void,decltype(&LocalFree)>(descriptor,LocalFree);
      if(!trusted(owner)||!dacl)throw std::runtime_error("Install object has untrusted ownership or unrestricted access");
      for(DWORD i=0;i<dacl->AceCount;++i){
        void *raw=nullptr;if(!GetAce(dacl,i,&raw))throw std::runtime_error("Install ACL is unreadable");
        const auto header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){
          const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
          if(!install_root_guard::allow_ace_safe(ace->Mask,(header->AceFlags&INHERIT_ONLY_ACE)!=0,strict,
                trusted(&ace->SidStart),IsWellKnownSid(&ace->SidStart,WinCreatorOwnerSid)!=FALSE))
            throw std::runtime_error("Install object permits untrusted modification");
        }else if(header->AceType!=ACCESS_DENIED_ACE_TYPE && header->AceType!=SYSTEM_AUDIT_ACE_TYPE)
          throw std::runtime_error("Install object has unsupported access rules");
      }
      pins.push_back(std::move(file));return {true,(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0};
    }
    std::vector<std::wstring> children(const std::wstring &path) {
      std::vector<std::wstring> result;WIN32_FIND_DATAW data{};
      HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&data);
      if(search==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_NOT_FOUND)return result;throw std::runtime_error("Unable to enumerate install directory");}
      DWORD error=ERROR_SUCCESS;
      do{const std::wstring name=data.cFileName;if(name!=L"."&&name!=L"..")result.push_back(path+L"\\"+name);}while(FindNextFileW(search,&data));
      error=GetLastError();FindClose(search);
      if(error!=ERROR_NO_MORE_FILES)throw std::runtime_error("Install enumeration changed");return result;
    }
  };
}
static void validate_one(std::wstring requested,bool removing,bool preview) {
    while(requested.size()>3 && (requested.back()==L'\\'||requested.back()==L'/'))requested.pop_back();
    if(requested.size()<4 || requested[1]!=L':' || requested[2]!=L'\\' ||
       requested.find_first_of(L"\"'`$%;&|<>^\r\n/")!=std::wstring::npos)
      throw std::runtime_error("Install root must be a direct local path without command metacharacters");
    std::vector<wchar_t> normalized(32768);
    const DWORD length=GetFullPathNameW(requested.c_str(),static_cast<DWORD>(normalized.size()),normalized.data(),nullptr);
    if(!length || length>=normalized.size())throw std::runtime_error("Install root cannot be normalized");
    auto root=lower(std::wstring(normalized.data(),length));
    std::wstring anchor;
    for(const KNOWNFOLDERID *id : {&FOLDERID_ProgramFiles,&FOLDERID_ProgramFilesX86}) {
      PWSTR raw=nullptr;
      if(SUCCEEDED(SHGetKnownFolderPath(*id,0,nullptr,&raw))){
        auto candidate=lower(raw);CoTaskMemFree(raw);
        if(root.size()>candidate.size() && root.compare(0,candidate.size(),candidate)==0 && root[candidate.size()]==L'\\')anchor=candidate;
      }
    }
    if(anchor.empty()) {
      if(!removing)throw std::runtime_error("Install root must be beneath Program Files");
      // Retain removal of trusted legacy installations outside Program Files.
      // Ancestors must still deny untrusted rename/delete/control operations.
      anchor=root.substr(0,3);
    }
    filesystem_t fs{{},preview};install_root_guard::validate(fs,root,anchor,!removing || anchor.size()>3);

}
static std::wstring registry_string(const std::wstring &key,const wchar_t *name) {
  DWORD bytes=0;
  constexpr DWORD flags=RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY;
  const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,key.c_str(),name,flags,nullptr,nullptr,&bytes);
  if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND)return {};
  if(status!=ERROR_SUCCESS || bytes<sizeof(wchar_t) || bytes>65536)throw std::runtime_error("Invalid registered installation path");
  std::wstring value(bytes/sizeof(wchar_t),L'\0');
  if(RegGetValueW(HKEY_LOCAL_MACHINE,key.c_str(),name,flags,nullptr,value.data(),&bytes)!=ERROR_SUCCESS)
    throw std::runtime_error("Registered installation path changed");
  while(!value.empty() && value.back()==L'\0')value.pop_back();
  return value;
}
static const std::wstring uninstall_key=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
static std::wstring registered_root(const std::wstring &code) {
  DWORD size=0;wchar_t empty=0;
  auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",&empty,&size);
  if(status==ERROR_MORE_DATA || (status==ERROR_SUCCESS && size)) {
    std::wstring value(size+1,L'\0');++size;
    if(MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",value.data(),&size)!=ERROR_SUCCESS)
      throw std::runtime_error("Installed product location changed");
    value.resize(size);if(!value.empty())return value;
  }
  auto location=registry_string(uninstall_key+code,L"InstallLocation");
  if(!location.empty())return location;
  for(const auto name : {L"Sunshine",L"Vibeshine",L"Apollo",L"Vibepollo"}) {
    const auto key=uninstall_key+name;
    if(lower(registry_string(key,L"ProductCode"))==lower(code)) {
      location=registry_string(key,L"InstallLocation");
      if(!location.empty())return location;
    }
  }
  // Historical MSIs did not publish ARPINSTALLLOCATION. Their stable
  // directory-key-path component binds INSTALL_ROOT to this exact machine
  // product; a named ARP entry alone is not an identity proof.
  std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
  // Some MinGW SDK headers omit this Windows 7+ API. Resolve its exact
  // signature from the already loaded system MSI module, with no PATH lookup.
  using component_path_fn=INSTALLSTATE (WINAPI *)(LPCWSTR,LPCWSTR,LPCWSTR,MSIINSTALLCONTEXT,LPWSTR,LPDWORD);
  const auto component_path=reinterpret_cast<component_path_fn>(GetProcAddress(GetModuleHandleW(L"msi.dll"),"MsiGetComponentPathExW"));
  if(component_path && component_path(code.c_str(),L"{B6D8A6A3-63B7-4C3F-8A2C-2C8F2B2F3B61}",nullptr,
      MSIINSTALLCONTEXT_MACHINE,path.data(),&length)==INSTALLSTATE_LOCAL && length>3 && length<path.size())
    return std::wstring(path.data(),length);
  throw std::runtime_error("Cannot resolve the installed product's actual directory");
}
static UINT validate_root(MSIHANDLE session,bool preview) {
 try {
  const auto code=property(session,L"ProductCode");DWORD product_size=0;wchar_t product=0;
  const auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"ProductName",&product,&product_size);
  const bool installed=status==ERROR_SUCCESS || status==ERROR_MORE_DATA;
  const bool removing=installed && lower(property(session,L"REMOVE"))==L"all";
  validate_one(property(session,L"INSTALL_ROOT"),removing,preview);
  if(installed)validate_one(registered_root(code),true,preview);
  // MajorUpgrade removes related products using their recorded directories,
  // even when the incoming product is installed in a different directory.
  const auto upgrade=property(session,L"UpgradeCode");
  if(upgrade.empty())throw std::runtime_error("Missing MSI upgrade identity");
  for(DWORD index=0;;++index) {
    wchar_t related[39]={};
    const auto result=MsiEnumRelatedProductsW(upgrade.c_str(),0,index,related);
    if(result==ERROR_NO_MORE_ITEMS)break;
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot enumerate related installations");
    if(lower(related)!=lower(code))validate_one(registered_root(related),true,preview);
  }
  // Match the pre-transaction competing-product action's machine registration
  // roots; do not touch or validate those products when the caller skips it.
  if(!installed && !removing && property(session,L"SKIP_REMOVE_CONFLICTING_PRODUCTS")!=L"1") {
    for(const auto name : {L"Sunshine",L"Vibeshine",L"Apollo",L"Vibepollo"}) {
      auto location=registry_string(uninstall_key+name,L"InstallLocation");
      if(!location.empty())validate_one(location,true,preview);
    }
  }
  return ERROR_SUCCESS;
 }catch(...) {
  MSIHANDLE record=MsiCreateRecord(0);
  if(record){MsiRecordSetStringW(record,0,L"The install folder or an existing product directory is unsafe, writable by an untrusted account, or contains a reparse point. Close the host and run the bootstrapper from an elevated account.");MsiProcessMessage(session,INSTALLMESSAGE_ERROR,record);MsiCloseHandle(record);}
  return ERROR_INSTALL_FAILURE;
 }
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRootPreview(MSIHANDLE session) {
 return validate_root(session,true);
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRoot(MSIHANDLE session) {
 return validate_root(session,false);
}
