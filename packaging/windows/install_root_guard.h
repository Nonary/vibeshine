#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
namespace install_root_guard {
  enum class role_t { none, drive_ancestor, anchor, ancestor, install_root, descendant };
  // Stable reason codes plus the affected local path for actionable installer logs.
  struct failure_t : std::exception {
    std::wstring path;
    const wchar_t *reason;
    role_t role;
    const wchar_t *api;
    std::uint32_t status;
    std::uint32_t access;
    failure_t(const wchar_t *why, role_t object=role_t::none, const wchar_t *call=L"none", std::uint32_t result=0, std::uint32_t bits=0):
      reason(why), role(object), api(call), status(result), access(bits) {}
    const char *what() const noexcept override { return "Install root validation failed"; }
  };
  inline bool critical(const failure_t &failure) {
    const std::wstring reason=failure.reason;
    // Block observed unsafe paths/permissions, not an inability to prove every
    // compatibility detail on the host. MSI reports actual transaction errors.
    return reason==L"invalid_path" || reason==L"invalid_local_path" ||
      reason==L"outside_program_files" || reason==L"outside_anchor" ||
      reason==L"reparse" || reason==L"owner_or_dacl" || reason==L"untrusted_allow" ||
      reason==L"invalid_ancestor" || reason==L"unexpected" ||
      (reason==L"identity_unproven" && failure.status==0);
  }
  struct anchor_diagnostic_t {
    bool failed=false;
    std::uint32_t status=0;
    void record(bool success, std::uint32_t result) { if(!success) { failed=true;status=result; } }
    const wchar_t *rejection(bool matched, bool removing) const {
      return matched || removing ? nullptr : failed ? L"known_folder" : L"outside_program_files";
    }
  };
  // Only accept the exact root of a handle-resolved volume GUID path. A drive
  // letter alone is insufficient: it can be mapped to an ordinary directory.
  inline bool is_volume_guid_root(const std::wstring &path) {
    if (path.size()!=49 || path.compare(0,11,L"\\\\?\\Volume{")!=0 || path[47]!=L'}' || path[48]!=L'\\') return false;
    for (std::size_t i=11;i<47;++i) {
      if (i==19 || i==24 || i==29 || i==34) { if (path[i]!=L'-') return false; }
      else if (!((path[i]>=L'0' && path[i]<=L'9') || (path[i]>=L'a' && path[i]<=L'f') || (path[i]>=L'A' && path[i]<=L'F'))) return false;
    }
    return true;
  }
  inline bool same_file_identity(std::uint32_t volume_a, std::uint32_t high_a, std::uint32_t low_a,
                                 std::uint32_t volume_b, std::uint32_t high_b, std::uint32_t low_b) {
    // Unknown volume serials or file IDs cannot establish object identity.
    // The GUID path check is still required separately.
    return volume_a && (high_a || low_a) && volume_a==volume_b && high_a==high_b && low_a==low_b;
  }
  inline std::uint32_t unsafe_access_bits(std::uint32_t mask, bool strict, bool volume_root=false) {
    constexpr std::uint32_t rename_control = 0x00010000 | 0x00000040 | 0x00040000 | 0x00080000;
    constexpr std::uint32_t writes = 0x00000002 | 0x00000004 | 0x00000010 | 0x00000100;
    // DELETE on a true volume root cannot rename an ordinary parent entry.
    // This does not exempt child deletion, ownership/DACL control, or writes.
    const auto controls = rename_control & ((!strict && volume_root) ? ~std::uint32_t{0x00010000} : ~std::uint32_t{0});
    return mask & (controls | (strict ? writes : 0) | 0x10000000 | 0x40000000);
  }
  inline bool allow_ace_safe(std::uint32_t mask, bool inherit_only, bool strict, bool trusted, bool creator_owner, bool volume_root=false) {
    if (inherit_only && !strict) return true;
    if (trusted || (inherit_only && creator_owner)) return true;
    return !unsafe_access_bits(mask,strict,volume_root);
  }
  struct node_t { bool exists, directory; std::uint32_t status=0; };
  inline std::wstring parent(std::wstring path) {
    const auto end=path.find_last_of(L'\\');
    if(end==std::wstring::npos || end<2) throw failure_t(L"invalid_local_path");
    return path.substr(0,end==2 ? 3 : end);
  }
  // Backends pin every existing object against write/delete sharing until the
  // complete scan ends. Reparse points and untrusted owner/write ACLs fail.
  template<class Backend>
  void validate(Backend &fs, const std::wstring &root, const std::wstring &anchor, bool allow_missing) {
    std::vector<std::wstring> ancestors;
    bool anchored=false;
    for(auto path=root;;path=parent(path)) {
      ancestors.push_back(path);
      if(path==anchor) anchored=true;
      if(path.size()<=3) break;
    }
    if(!anchored) throw failure_t(L"outside_anchor");
    std::reverse(ancestors.begin(),ancestors.end());
    bool missing=false;
    for(const auto &path:ancestors) {
      const auto role=path==root ? role_t::install_root : path.size()<=3 ? role_t::drive_ancestor : path==anchor ? role_t::anchor : role_t::ancestor;
      const auto node=fs.pin(path,path==root || (anchor.size()>3 && path.size()>=anchor.size()),role);
      if(!node.exists) { if(path.size()<=anchor.size() || !allow_missing) throw failure_t(L"missing",role,node.status?L"CreateFileW":L"none",node.status);missing=true; }
      else if(missing || !node.directory) throw failure_t(L"invalid_ancestor",role);
    }
    if(missing) return;
    std::vector<std::wstring> pending{root};
    std::size_t count=ancestors.size();
    while(!pending.empty()) {
      auto directory=std::move(pending.back());pending.pop_back();
      for(const auto &child:fs.children(directory,directory==root ? role_t::install_root : role_t::descendant)) {
        if(++count>100000 || child.size()>32760) throw failure_t(L"bounds",role_t::descendant);
        auto node=fs.pin(child,true,role_t::descendant);
        if(!node.exists) throw failure_t(L"tree_changed",role_t::descendant,node.status?L"CreateFileW":L"none",node.status);
        if(node.directory) pending.push_back(child);
      }
    }
  }
}
