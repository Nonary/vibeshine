#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace usb_short {
constexpr std::uint64_t maximum_bytes=900ull*1024*1024;
template<class T> void touch(std::vector<T>& v){
    auto* p=reinterpret_cast<volatile std::uint8_t*>(v.data());
    const auto bytes=v.size()*sizeof(T);
    for(std::size_t i=0;i<bytes;i+=4096)p[i]=0;
    if(bytes)p[bytes-1]=0;
}
// Evidence history only. It is never used to deliver audio to the encoder,
// packetizer or receiver. Admission and receiver queue sizes stay unchanged.
template<class Trace> class history {
public:
    struct entry {Trace trace{};std::uint64_t dequeue=0,control_prefix=0;double age_ms=0;};
private:
    std::vector<entry> rows_;
    std::size_t used_=0;
    std::uint64_t wire_bytes_=0;
public:
    static std::uint64_t budget(std::size_t capacity,unsigned channels){
        if(!capacity||capacity>28000||!channels||channels>12)throw std::runtime_error("Invalid short evidence duration or format");
        const auto bytes=std::uint64_t(capacity)*(sizeof(entry)+std::uint64_t(240)*channels*sizeof(float)+8*sizeof(double));
        if(bytes>maximum_bytes)throw std::runtime_error("Short evidence memory budget exceeded");
        return bytes;
    }
    history(std::size_t capacity,unsigned channels){
        budget(capacity,channels);rows_.resize(capacity);touch(rows_);
    }
    void append(const Trace& e,std::uint64_t dequeue,std::uint64_t prefix,double age){
        if(e.output_block!=used_||used_>=rows_.size()||e.datagram_count>e.datagrams.size()||
           (used_&&prefix<rows_[used_-1].control_prefix))throw std::runtime_error("Short evidence capacity or order mismatch");
        std::uint64_t wire=0;
        for(unsigned i=0;i<e.datagram_count;++i){
            if(!e.datagram_bytes[i]||e.datagram_bytes[i]>e.datagrams[i].size())throw std::runtime_error("Invalid observed datagram size");
            wire+=sizeof(std::uint32_t)+e.datagram_bytes[i];
        }
        auto& dst=rows_[used_];dst.trace=e;dst.dequeue=dequeue;dst.control_prefix=prefix;dst.age_ms=age;
        wire_bytes_+=wire;++used_;
    }
    const entry* begin()const noexcept{return rows_.data();}
    const entry* end()const noexcept{return rows_.data()+used_;}
    std::size_t count()const noexcept{return used_;}
    std::uint64_t wire_bytes()const noexcept{return wire_bytes_;}
};
}
