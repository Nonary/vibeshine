#pragma once
#include <cstddef>
#include <cstdint>

// Independent little-endian local IPC. USB provenance is never a kernel-ring record.
namespace vibe_input {
constexpr std::uint32_t Magic=0x42535556u; // VUSB
constexpr std::uint16_t Version=1;
constexpr std::uint32_t SampleRate=48000, Frames=240, MaximumChannels=12;
constexpr std::uint32_t QueueBlocks=4, MaximumAgeMs=20;
constexpr std::uint32_t EndpointCharacters=128, InstanceCharacters=200;
enum SourceKind : std::uint32_t { UserModeUsb=1 };
enum MessageType : std::uint16_t { Hello=1, Welcome=2, Audio=3, Error=4, Goodbye=5 };
enum ErrorCode : std::uint32_t {
 NoError=0, InvalidHandshake=1, SourceUnavailable=2, BindingMismatch=3,
 InvalidRecord=4, SequenceGap=5, EpochChanged=6, InputLoss=7, QueueOverflow=8,
 StaleBlock=9, PeerDisconnected=10, ControlledStop=11, FormatMismatch=12,
 EndpointInactive=13, PipeTimeout=14, InternalFailure=15
};
constexpr bool ValidLayout(std::uint32_t c,std::uint32_t m) {
 return (c==2&&m==3)||(c==6&&(m==0x3f||m==0x60f))||(c==8&&m==0x63f)||(c==12&&m==0x2d63f);
}
#pragma pack(push,1)
struct Header {
 std::uint32_t magic; std::uint16_t version,type; std::uint32_t bytes,flags;
 std::uint64_t nonce[2]; // Client challenge in Hello, fresh server session nonce thereafter.
};
struct Profile {
 std::uint32_t sampleRate; std::uint16_t channels,frames; std::uint32_t channelMask;
 std::uint16_t nativeContainerBits,nativeValidBits;
};
struct UsbBinding {
 char16_t endpointId[EndpointCharacters];
 char16_t usbInstance[InstanceCharacters]; // Exact physical USB parent, not the composite audio function.
 char16_t controllerInstance[InstanceCharacters];
 char serial[16]; char service[6]; std::uint16_t reserved;
 std::uint32_t port; std::uint16_t vendor,product;
 // Loopback host is fixed 127.0.0.1. Secret USBIP busid is deliberately absent.
};
struct HelloMessage {
 Header header; std::uint32_t sourceKind,clientPid,expectedServerPid,windowsSessionId;
 std::uint64_t clientCreationTime,serverCreationTime; // GetProcessTimes FILETIME, no PID-only trust.
 Profile profile; UsbBinding binding;
};
struct WelcomeMessage {
 Header header; std::uint64_t clientNonce[2],qpcFrequency;
 std::uint32_t sourceKind,serverPid,clientPid,windowsSessionId;
 std::uint64_t serverCreationTime,clientCreationTime;
 Profile profile; std::uint32_t queueBlocks,maximumAgeMs; UsbBinding binding;
};
struct AudioHeader {
 Header header; std::uint32_t sourceKind,reserved;
 std::uint64_t blockSequence; // Starts at zero for every new authenticated pipe session.
 std::uint64_t epoch; // Real USB SET_INTERFACE/reset epoch, stable until disconnect.
 std::uint64_t firstFrame; // Native source frame position; contiguous +240 within this session.
 std::uint64_t firstUrbSequence,lastUrbSequence; // Control/feedback URBs may occur between these.
 std::uint64_t firstReceiveQpc,lastReceiveQpc; // Complete USB OUT request admission, first/last contributor.
 std::uint64_t blockReadyQpc; // AFTER all 240 float frames have been copied into this block.
 std::uint64_t enqueueQpc; // Immediately before release-publication to the bounded bridge queue.
 std::uint64_t sendQpc; // Immediately before this complete named-pipe WriteFile operation.
 std::uint64_t qpcFrequency,queueOverruns,inputDroppedFrames,inputRejectedUrbs;
 Profile profile;
 std::uint32_t queueDepthBlocks,queueCapacityBlocks,payloadBytes,reserved2;
 std::uint32_t firstUrbFrameOffset,lastUrbFrameOffset; // Last is inclusive; both refer to native packet-concatenated URB PCM.
};
struct AudioMessage { AudioHeader info; float samples[Frames*MaximumChannels]; };
struct ErrorMessage { Header header; std::uint32_t code,systemError; std::uint64_t lastBlockSequence; };
#pragma pack(pop)
static_assert(sizeof(Header)==32&&sizeof(Profile)==16&&sizeof(UsbBinding)==1088,"wire scalars");
static_assert(sizeof(HelloMessage)==1168&&sizeof(WelcomeMessage)==1200,"handshake ABI");
static_assert(sizeof(AudioHeader)==192&&sizeof(AudioMessage)==11712&&sizeof(ErrorMessage)==48,"audio ABI");
static_assert(sizeof(float)==4&&sizeof(char16_t)==2&&offsetof(AudioMessage,samples)==192,"PCM ABI");
constexpr bool ValidProfile(const Profile& p) {
 return p.sampleRate==SampleRate&&p.frames==Frames&&ValidLayout(p.channels,p.channelMask)&&
 ((p.nativeContainerBits==16&&p.nativeValidBits==16)||(p.nativeContainerBits==32&&p.nativeValidBits==24));
}
constexpr bool SameProfile(const Profile& a,const Profile& b) {
 return a.sampleRate==b.sampleRate&&a.channels==b.channels&&a.frames==b.frames&&a.channelMask==b.channelMask&&
 a.nativeContainerBits==b.nativeContainerBits&&a.nativeValidBits==b.nativeValidBits;
}
constexpr std::uint32_t AudioPayloadBytes(std::uint32_t channels){return Frames*channels*4;}
constexpr std::uint32_t AudioWireBytes(std::uint32_t channels){return sizeof(AudioHeader)+AudioPayloadBytes(channels);}
inline Header MakeHeader(MessageType type,std::uint32_t bytes,const std::uint64_t(&nonce)[2]){return {Magic,Version,type,bytes,0,{nonce[0],nonce[1]}};}
inline bool SameNonce(const std::uint64_t(&a)[2],const std::uint64_t(&b)[2]){return a[0]==b[0]&&a[1]==b[1]&&(a[0]||a[1]);}
inline bool ValidHeader(const Header& h,MessageType type,std::uint32_t bytes){return h.magic==Magic&&h.version==Version&&h.type==type&&h.bytes==bytes&&!h.flags&&(h.nonce[0]||h.nonce[1]);}
} // namespace vibe_input
