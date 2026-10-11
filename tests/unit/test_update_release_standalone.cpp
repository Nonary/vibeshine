// Standalone regression harness: g++ -std=c++20 -I. tests/unit/test_update_release_standalone.cpp
#include "src/update_release.h"
#include <cassert>
#include <iostream>
using nlohmann::json;
json release(std::string tag, bool pre = false) { return {{"tag_name",tag},{"draft",false},{"prerelease",pre},{"assets",json::array({{{"name","VibeshineSetup.exe"},{"state","uploaded"},{"size",100}}})}}; }
json daily(std::string day, std::string base = "2.0.0") {
  auto result=release("daily-2026-10-"+day,true);
  for(auto name : {"nightly-metadata.json","SHA256SUMS"}) result["assets"].push_back({{"name",name},{"state","uploaded"},{"size",100}});
  result["nightly_metadata"]={{"schema_version",1},{"channel","nightly"},{"tag",result["tag_name"]},{"version",base+"-daily.d202610"+day+"+aaaaaaaaaaaa"},{"source_commit",std::string(40,'a')},{"nightly_sequence",std::stoi("202610"+day)}};
  return result;
}
int main() {
  using release_selection::select;
  auto releases=json::array({release("1.9.0-stable.2"),release("2.0.0-beta.10",true),release("2.0.0-beta.2",true)});
  assert(select(releases,"1.9.0-stable.1",false)["tag_name"]=="1.9.0-stable.2");
  assert(select(releases,"2.0.0-beta.2",false).is_null());
  assert(select(releases,"2.0.0-beta.2",true)["tag_name"]=="2.0.0-beta.10");
  auto days=json::array({daily("10"),daily("11"),daily("12","1.0.0")});
  assert(select(days,"2.0.0-daily.d20261010+aaaaaaaaaaaa",true)["tag_name"]=="daily-2026-10-11");
  assert(select(days,"2.0.0-daily.d20261011+aaaaaaaaaaaa",true).is_null());
  assert(select(days,"2.0.0",true).is_null());
  assert(select(days,"2.0.0-beta.1",true).is_null());
  assert(select(json::array({release("2.0.0-rc.1",true)}),"2.0.0-daily.d20261011+aaaaaaaaaaaa",true).is_null());
  days.push_back(release("2.0.0"));
  assert(select(days,"2.0.0-daily.d20261010+aaaaaaaaaaaa",true)["tag_name"]=="2.0.0");
  auto published=release("2.0.0"); published["assets"][0]["name"]="VibeshineSetup-v2.0.0.exe";
  assert(select(json::array({published}),"1.18.4-stable.4",false)["tag_name"]=="2.0.0");
  auto bad=daily("11"); bad["nightly_metadata"]["source_commit"]="bad";
  auto draft=release("9.0.0"); draft["draft"]=true;
  auto incomplete=release("9.0.0"); incomplete["assets"][0]["state"]="new";
  assert(select(json::array({bad,draft,incomplete,json("bad"),json::object()}),"1.0.0",true).is_null());
  assert(select(releases,"unknown",true).is_null());
  assert(select(releases,"99999999999999999999999.0.0",true).is_null());
  std::cout << "release selection regression checks passed\n";
}
