"""Compile the production server accept loop against deterministic pipe edges."""
from pathlib import Path
import subprocess, tempfile, argparse
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');a=p.parse_args()
r=Path(__file__).resolve().parents[2]
s=(r/'src/platform/windows/ipc/pipes.cpp').read_text()
start=s.index('  std::unique_ptr<INamedPipe> accept_server_pipe_client(')
end=s.index('\n  std::unique_ptr<INamedPipe> NamedPipeFactory::create_client',start)
code=r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <iostream>
using HANDLE=void*;using DWORD=std::uint32_t;
void require(bool b){if(!b)throw std::runtime_error("pipe authority regression");}
struct Edge{bool connected;std::optional<DWORD> peer;};
std::vector<Edge> edges;int opened=0,disconnected=0;bool duplicate_failure=false;
struct INamedPipe{Edge edge;void wait_for_client_connection(int n){require(n>0 && n<=100);}
 bool is_connected(){return edge.connected;}auto peer_process_id(){return edge.peer;}
 void disconnect(){++disconnected;}};
std::unique_ptr<INamedPipe> duplicate_server_pipe(HANDLE){
 ++opened;if(duplicate_failure)return {};
 require(!edges.empty());auto e=edges.front();edges.erase(edges.begin());return std::make_unique<INamedPipe>(e);
}
''' + s[start:end] + r'''
int main(){
 auto deadline=[] {return std::chrono::steady_clock::now()+std::chrono::seconds(1);};
 edges={{true,9},{true,42}};
 require(accept_server_pipe_client(nullptr,42,{},deadline())!=nullptr);
 require(opened==2 && disconnected==1); // hostile client drained before real child
 edges={{false,{}},{true,{}},{true,42}};opened=disconnected=0;
 require(accept_server_pipe_client(nullptr,42,{},deadline())!=nullptr);
 require(opened==3 && disconnected==2); // unconnected and unknown peer are never framed
 opened=0;require(!accept_server_pipe_client(nullptr,0,{},deadline()) && opened==0);
 require(!accept_server_pipe_client(nullptr,42,[]{return true;},deadline()) && opened==0);
 require(!accept_server_pipe_client(nullptr,42,{},std::chrono::steady_clock::now()) && opened==0);
 duplicate_failure=true;require(!accept_server_pipe_client(nullptr,42,{},deadline()) && opened==1);
 duplicate_failure=false;edges={{true,9}};opened=0;
 auto stale=[] {return opened!=0;};
 require(!accept_server_pipe_client(nullptr,42,stale,deadline()) && opened==1); // rotation stops retry
 std::cout<<"production host pipe accept: exact peer, wrong/missing peer, cancellation, deadline, duplicate failure and rotation passed\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'authority.cpp';exe=Path(tmp)/'authority';cpp.write_text(code)
 subprocess.run([a.compiler,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=5)
