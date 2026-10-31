#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
namespace install_root_guard {
  inline bool allow_ace_safe(std::uint32_t mask, bool inherit_only, bool strict, bool trusted, bool creator_owner) {
    if (inherit_only && !strict) return true;
    if (trusted || (inherit_only && creator_owner)) return true;
    constexpr std::uint32_t rename_control = 0x00010000 | 0x00000040 | 0x00040000 | 0x00080000;
    constexpr std::uint32_t writes = 0x00000002 | 0x00000004 | 0x00000010 | 0x00000100;
    return !(mask & (rename_control | (strict ? writes : 0) | 0x10000000 | 0x40000000));
  }
  struct node_t { bool exists, directory; };
  inline std::wstring parent(std::wstring path) {
    const auto end=path.find_last_of(L'\\');
    if(end==std::wstring::npos || end<2) throw std::runtime_error("Invalid local install path");
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
    if(!anchored) throw std::runtime_error("Install path is outside the trusted anchor");
    std::reverse(ancestors.begin(),ancestors.end());
    bool missing=false;
    for(const auto &path:ancestors) {
      const auto node=fs.pin(path,path==root || (anchor.size()>3 && path.size()>=anchor.size()));
      if(!node.exists) { if(path.size()<=anchor.size() || !allow_missing) throw std::runtime_error("Installed directory is missing");missing=true; }
      else if(missing || !node.directory) throw std::runtime_error("Invalid install ancestor");
    }
    if(missing) return;
    std::vector<std::wstring> pending{root};
    std::size_t count=ancestors.size();
    while(!pending.empty()) {
      auto directory=std::move(pending.back());pending.pop_back();
      for(const auto &child:fs.children(directory)) {
        if(++count>100000 || child.size()>32760) throw std::runtime_error("Install tree exceeds validation bounds");
        auto node=fs.pin(child,true);
        if(!node.exists) throw std::runtime_error("Install tree changed during validation");
        if(node.directory) pending.push_back(child);
      }
    }
  }
}
