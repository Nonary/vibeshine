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
    if(result!=ERROR_MORE_DATA && result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"msi_property",install_root_guard::role_t::none,L"MsiGetPropertyW",result);
    std::wstring value(size+1,L'\0');++size;
    result=MsiGetPropertyW(session,name,value.data(),&size);
    if(result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"msi_property",install_root_guard::role_t::none,L"MsiGetPropertyW",result);
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
  struct root_check_t { bool valid; const wchar_t *api; DWORD status; };
  root_check_t is_volume_root(HANDLE file,const BY_HANDLE_FILE_INFORMATION &info) {
    // Resolve the object already opened without following its final reparse
    // point. Failure, truncation, an alias to a directory, or an unfamiliar
    // format must all leave the normal ACL rejection in force.
    wchar_t resolved[50]{};
    const DWORD length=GetFinalPathNameByHandleW(file,resolved,50,FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);
    if(!length)return {false,L"GetFinalPathNameByHandleW",GetLastError()};
    if(length!=49 || !install_root_guard::is_volume_guid_root(std::wstring(resolved,length)))return {false,L"none",0};
    // Confirm the canonical root is the same filesystem object as the pinned
    // handle, not merely a name that looks like a root. Unknown IDs fail closed.
    handle_t root(CreateFileW(resolved,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
      nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(root.value==INVALID_HANDLE_VALUE)return {false,L"CreateFileW",GetLastError()};
    BY_HANDLE_FILE_INFORMATION root_info{};
    if(!GetFileInformationByHandle(root.value,&root_info))return {false,L"GetFileInformationByHandle",GetLastError()};
    if(!(root_info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (root_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return {false,L"none",0};
    return {install_root_guard::same_file_identity(info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,
      root_info.dwVolumeSerialNumber,root_info.nFileIndexHigh,root_info.nFileIndexLow),L"none",0};
  }
  struct filesystem_t {
    std::vector<std::unique_ptr<handle_t>> pins;
    bool preview;
    install_root_guard::node_t pin(const std::wstring &path,bool strict,install_root_guard::role_t role) {
      using namespace install_root_guard;
      auto file=std::make_unique<handle_t>(CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | (preview ? FILE_SHARE_WRITE : 0),nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
      if(file->value==INVALID_HANDLE_VALUE){
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return {false,false,error};
        throw failure_t(L"open",role,L"CreateFileW",error);
      }
      BY_HANDLE_FILE_INFORMATION info{};
      if(!GetFileInformationByHandle(file->value,&info))throw failure_t(L"attributes",role,L"GetFileInformationByHandle",GetLastError());
      if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw failure_t(L"reparse",role);
      PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      const DWORD security_status=GetSecurityInfo(file->value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
          &owner,nullptr,&dacl,nullptr,&descriptor);
      if(security_status!=ERROR_SUCCESS)throw failure_t(L"security",role,L"GetSecurityInfo",security_status);
      auto free_descriptor=std::unique_ptr<void,decltype(&LocalFree)>(descriptor,LocalFree);
      if(!trusted(owner)||!dacl)throw failure_t(L"owner_or_dacl",role);
      for(DWORD i=0;i<dacl->AceCount;++i){
        void *raw=nullptr;if(!GetAce(dacl,i,&raw))throw failure_t(L"acl_read",role,L"GetAce",GetLastError());
        const auto header=static_cast<ACE_HEADER*>(raw);
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){
          const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
          const bool inherit_only=(header->AceFlags&INHERIT_ONLY_ACE)!=0;
          const bool trusted_sid=trusted(&ace->SidStart);
          const bool creator_owner=IsWellKnownSid(&ace->SidStart,WinCreatorOwnerSid)!=FALSE;
          if(!install_root_guard::allow_ace_safe(ace->Mask,inherit_only,strict,trusted_sid,creator_owner)) {
            // Apply the exception only when DELETE is the sole rejected bit,
            // and only to the already-opened lexical drive-root object after
            // resolving it to an actual volume root.
            root_check_t identity{false,L"none",0};
            const bool identity_attempted=!strict && path.size()==3 &&
               install_root_guard::allow_ace_safe(ace->Mask,inherit_only,strict,trusted_sid,creator_owner,true);
            if(identity_attempted)
              identity=is_volume_root(file->value,info);
            if(!identity.valid) {
              throw failure_t(identity_attempted?L"identity_unproven":L"untrusted_allow",role,identity.api,identity.status,unsafe_access_bits(ace->Mask,strict));
            }
          }
        }else if(header->AceType!=ACCESS_DENIED_ACE_TYPE && header->AceType!=SYSTEM_AUDIT_ACE_TYPE)
          throw failure_t(L"unsupported_ace",role);
      }
      pins.push_back(std::move(file));return {true,(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0};
    }
    std::vector<std::wstring> children(const std::wstring &path,install_root_guard::role_t role) {
      using namespace install_root_guard;
      std::vector<std::wstring> result;WIN32_FIND_DATAW data{};
      HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&data);
      if(search==INVALID_HANDLE_VALUE){const DWORD error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return result;throw failure_t(L"enumerate",role,L"FindFirstFileW",error);}
      DWORD error=ERROR_SUCCESS;
      do{const std::wstring name=data.cFileName;if(name!=L"."&&name!=L"..")result.push_back(path+L"\\"+name);}while(FindNextFileW(search,&data));
      error=GetLastError();FindClose(search);
      if(error!=ERROR_NO_MORE_FILES)throw failure_t(L"enumeration_changed",role,L"FindNextFileW",error);return result;
    }
  };
}
static void validate_one(std::wstring requested,bool removing,bool preview) {
    while(requested.size()>3 && (requested.back()==L'\\'||requested.back()==L'/'))requested.pop_back();
    if(requested.size()<4 || requested[1]!=L':' || requested[2]!=L'\\' ||
       requested.find_first_of(L"\"'`$%;&|<>^\r\n/")!=std::wstring::npos)
      throw install_root_guard::failure_t(L"invalid_path",install_root_guard::role_t::install_root);
    std::vector<wchar_t> normalized(32768);
    const DWORD length=GetFullPathNameW(requested.c_str(),static_cast<DWORD>(normalized.size()),normalized.data(),nullptr);
    if(!length)throw install_root_guard::failure_t(L"normalize",install_root_guard::role_t::install_root,L"GetFullPathNameW",GetLastError());
    if(length>=normalized.size())throw install_root_guard::failure_t(L"normalize_too_long",install_root_guard::role_t::install_root);
    auto root=lower(std::wstring(normalized.data(),length));
    std::wstring anchor;
    install_root_guard::anchor_diagnostic_t folder_diagnostic;
    for(const KNOWNFOLDERID *id : {&FOLDERID_ProgramFiles,&FOLDERID_ProgramFilesX86}) {
      PWSTR raw=nullptr;
      const HRESULT folder_status=SHGetKnownFolderPath(*id,0,nullptr,&raw);
      folder_diagnostic.record(SUCCEEDED(folder_status),static_cast<std::uint32_t>(folder_status));
      if(SUCCEEDED(folder_status)){
        auto candidate=lower(raw);CoTaskMemFree(raw);
        if(root.size()>candidate.size() && root.compare(0,candidate.size(),candidate)==0 && root[candidate.size()]==L'\\')anchor=candidate;
      }
    }
    if(const auto reason=folder_diagnostic.rejection(!anchor.empty(),removing)) {
      if(folder_diagnostic.failed)throw install_root_guard::failure_t(reason,install_root_guard::role_t::install_root,L"SHGetKnownFolderPath",folder_diagnostic.status);
      throw install_root_guard::failure_t(reason,install_root_guard::role_t::install_root);
    }
    if(anchor.empty()) {
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
  if(status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"registry",install_root_guard::role_t::none,L"RegGetValueW",status);
  if(bytes<sizeof(wchar_t) || bytes>65536)throw install_root_guard::failure_t(L"registry_size");
  std::wstring value(bytes/sizeof(wchar_t),L'\0');
  const LSTATUS read_status=RegGetValueW(HKEY_LOCAL_MACHINE,key.c_str(),name,flags,nullptr,value.data(),&bytes);
  if(read_status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"registry",install_root_guard::role_t::none,L"RegGetValueW",read_status);
  while(!value.empty() && value.back()==L'\0')value.pop_back();
  return value;
}
static const std::wstring uninstall_key=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
static std::wstring registered_root(const std::wstring &code) {
  DWORD size=0;wchar_t empty=0;
  auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",&empty,&size);
  if(status==ERROR_MORE_DATA || (status==ERROR_SUCCESS && size)) {
    std::wstring value(size+1,L'\0');++size;
    const auto info_status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",value.data(),&size);
    if(info_status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"product_location",install_root_guard::role_t::none,L"MsiGetProductInfoExW",info_status);
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
  if(!component_path)throw install_root_guard::failure_t(L"installed_location_unresolved",install_root_guard::role_t::none,L"GetProcAddress",GetLastError());
  const auto component_state=component_path(code.c_str(),L"{B6D8A6A3-63B7-4C3F-8A2C-2C8F2B2F3B61}",nullptr,
      MSIINSTALLCONTEXT_MACHINE,path.data(),&length);
  if(component_state==INSTALLSTATE_LOCAL && length>3 && length<path.size())
    return std::wstring(path.data(),length);
  throw install_root_guard::failure_t(L"installed_location_unresolved",install_root_guard::role_t::none,L"MsiGetComponentPathExW",static_cast<std::uint32_t>(component_state));
}
static const wchar_t *role_name(install_root_guard::role_t role) {
  using install_root_guard::role_t;
  switch(role) {
    case role_t::drive_ancestor:return L"drive_ancestor";
    case role_t::anchor:return L"anchor";
    case role_t::ancestor:return L"ancestor";
    case role_t::install_root:return L"install_root";
    case role_t::descendant:return L"descendant";
    default:return L"none";
  }
}
static void log_failure(MSIHANDLE session,bool preview,const wchar_t *category,const install_root_guard::failure_t &failure) noexcept {
  // Diagnostics must never reveal names or change the validation result.
  try {
    const std::wstring message=std::wstring(L"Install root guard: phase=")+(preview?L"preview":L"strict")+
      L" category="+category+L" role="+role_name(failure.role)+L" reason="+failure.reason+
      L" api="+failure.api+L" status="+std::to_wstring(failure.status)+L" access="+std::to_wstring(failure.access);
    MSIHANDLE record=MsiCreateRecord(0);
    if(record){MsiRecordSetStringW(record,0,message.c_str());MsiProcessMessage(session,INSTALLMESSAGE_INFO,record);MsiCloseHandle(record);}
  } catch(...) {}
}
static UINT validate_root(MSIHANDLE session,bool preview) {
 const wchar_t *category=L"none";
 try {
  const auto code=property(session,L"ProductCode");DWORD product_size=0;wchar_t product=0;
  const auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"ProductName",&product,&product_size);
  const bool installed=status==ERROR_SUCCESS || status==ERROR_MORE_DATA;
  const bool removing=installed && lower(property(session,L"REMOVE"))==L"all";
  category=L"selected";
  validate_one(property(session,L"INSTALL_ROOT"),removing,preview);
  category=L"installed";
  if(installed)validate_one(registered_root(code),true,preview);
  // MajorUpgrade removes related products using their recorded directories,
  // even when the incoming product is installed in a different directory.
  category=L"related";
  const auto upgrade=property(session,L"UpgradeCode");
  if(upgrade.empty())throw install_root_guard::failure_t(L"missing_upgrade_identity");
  for(DWORD index=0;;++index) {
    wchar_t related[39]={};
    const auto result=MsiEnumRelatedProductsW(upgrade.c_str(),0,index,related);
    if(result==ERROR_NO_MORE_ITEMS)break;
    if(result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"related_enumeration",install_root_guard::role_t::none,L"MsiEnumRelatedProductsW",result);
    if(lower(related)!=lower(code))validate_one(registered_root(related),true,preview);
  }
  // Match the pre-transaction competing-product action's machine registration
  // roots; do not touch or validate those products when the caller skips it.
  category=L"conflicting";
  if(!installed && !removing && property(session,L"SKIP_REMOVE_CONFLICTING_PRODUCTS")!=L"1") {
    for(const auto name : {L"Sunshine",L"Vibeshine",L"Apollo",L"Vibepollo"}) {
      auto location=registry_string(uninstall_key+name,L"InstallLocation");
      if(!location.empty())validate_one(location,true,preview);
    }
  }
  return ERROR_SUCCESS;
 }catch(const install_root_guard::failure_t &failure) {log_failure(session,preview,category,failure);}
  catch(...) {log_failure(session,preview,category,install_root_guard::failure_t(L"unexpected"));}
 MSIHANDLE record=MsiCreateRecord(0);
 if(record){MsiRecordSetStringW(record,0,L"The install folder or an existing product directory is unsafe, writable by an untrusted account, or contains a reparse point. Close the host and run the bootstrapper from an elevated account.");MsiProcessMessage(session,INSTALLMESSAGE_ERROR,record);MsiCloseHandle(record);}
 return ERROR_INSTALL_FAILURE;
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRootPreview(MSIHANDLE session) {
 return validate_root(session,true);
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRoot(MSIHANDLE session) {
 return validate_root(session,false);
}
