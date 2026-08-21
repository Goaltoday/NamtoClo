#include "native_converter.hpp"
#include "common.hpp"
#include "stimulus.hpp"

#include <NAM/get_dsp.h>
#include <CDSPResampler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <vector>

// Takuya Ooura FFT4G double-precision RDFT. The implementation is compiled
// directly from the pinned Ooura-derived fft4g.c source selected in CMake.
extern "C" void lsx_rdft(int n, int isgn, double* a, int* ip, double* w);

namespace ntc {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kEps = 1.1920928955078125e-7;
constexpr std::size_t kA = 128;
constexpr std::size_t kB = 2048;
constexpr std::size_t kFft = 2048;
constexpr std::size_t kBins = kFft / 2 + 1;
constexpr std::size_t kCloBytes = 0x2288;

// HTUSBTools.dll imports the single-precision CRT entry points for these
// trainer operations (log10f/logf/expf/powf/sinf/cosf/sqrtf).  Keep the
// arguments and return values in float instead of promoting through double.
inline float preciseLog10F(float x){return ::log10f(x);}
inline float preciseLogF(float x){return ::logf(x);}
inline float preciseExpF(float x){return ::expf(x);}
inline float precisePowF(float a,float b){return ::powf(a,b);}
inline float preciseSinF(float x){return ::sinf(x);}
inline float preciseCosF(float x){return ::cosf(x);}
inline float preciseSqrtF(float x){return ::sqrtf(x);}
inline float flipFloatSignBit(float x){
    std::uint32_t u{};std::memcpy(&u,&x,sizeof(u));u^=0x80000000u;std::memcpy(&x,&u,sizeof(u));return x;
}

void report(const StatusCallback& cb, const std::wstring& s) { if (cb) cb(s); }

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
void put32(std::vector<std::uint8_t>& d, std::size_t o, std::uint32_t v) {
    d[o] = static_cast<std::uint8_t>(v); d[o+1] = static_cast<std::uint8_t>(v >> 8);
    d[o+2] = static_cast<std::uint8_t>(v >> 16); d[o+3] = static_cast<std::uint8_t>(v >> 24);
}
void putFloat(std::vector<std::uint8_t>& d, std::size_t o, float v) {
    std::uint32_t u{}; std::memcpy(&u, &v, 4); put32(d,o,u);
}
void putDouble(std::vector<std::uint8_t>& d, std::size_t o, double v) {
    std::uint64_t u{}; std::memcpy(&u, &v, 8);
    for (int i=0;i<8;++i) d[o+i] = static_cast<std::uint8_t>(u >> (8*i));
}
constexpr std::array<std::uint8_t,256> kCrcLoOfficial = {
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,
    0x00,0xc1,0x81,0x40,0x01,0xc0,0x80,0x41,0x01,0xc0,0x80,0x41,0x00,0xc1,0x81,0x40,
};
constexpr std::array<std::uint8_t,256> kCrcHiOfficial = {
    0x00,0xc0,0xc1,0x01,0xc3,0x03,0x02,0xc2,0xc6,0x06,0x07,0xc7,0x05,0xc5,0xc4,0x04,
    0xcc,0x0c,0x0d,0xcd,0x0f,0xcf,0xce,0x0e,0x0a,0xca,0xcb,0x0b,0xc9,0x09,0x08,0xc8,
    0xd8,0x18,0x19,0xd9,0x1b,0xdb,0xda,0x1a,0x1e,0xde,0xdf,0x1f,0xdd,0x1d,0x1c,0xdc,
    0x14,0xd4,0xd5,0x15,0xd7,0x17,0x16,0xd6,0xd2,0x12,0x13,0xd3,0x11,0xd1,0xd0,0x10,
    0xf0,0x30,0x31,0xf1,0x33,0xf3,0xf2,0x32,0x36,0xf6,0xf7,0x37,0xf5,0x35,0x34,0xf4,
    0x3c,0xfc,0xfd,0x3d,0xff,0x3f,0x3e,0xfe,0xfa,0x3a,0x3b,0xfb,0x39,0xf9,0xf8,0x38,
    0x28,0xe8,0xe9,0x29,0xeb,0x2b,0x2a,0xea,0xee,0x2e,0x2f,0xef,0x2d,0xed,0xec,0x2c,
    0xe4,0x24,0x25,0xe5,0x27,0xe7,0xe6,0x26,0x22,0xe2,0xe3,0x23,0xe1,0x21,0x20,0xe0,
    0xa0,0x60,0x61,0xa1,0x63,0xa3,0xa2,0x62,0x66,0xa6,0xa7,0x67,0xa5,0x65,0x64,0xa4,
    0x6c,0xac,0xad,0x6d,0xaf,0x6f,0x6e,0xae,0xaa,0x6a,0x6b,0xab,0x69,0xa9,0xa8,0x68,
    0x78,0xb8,0xb9,0x79,0xbb,0x7b,0x7a,0xba,0xbe,0x7e,0x7f,0xbf,0x7d,0xbd,0xbc,0x7c,
    0xb4,0x74,0x75,0xb5,0x77,0xb7,0xb6,0x76,0x72,0xb2,0xb3,0x73,0xb1,0x71,0x70,0xb0,
    0x50,0x90,0x91,0x51,0x93,0x53,0x52,0x92,0x96,0x56,0x57,0x97,0x55,0x95,0x94,0x54,
    0x9c,0x5c,0x5d,0x9d,0x5f,0x9f,0x9e,0x5e,0x5a,0x9a,0x9b,0x5b,0x99,0x59,0x58,0x98,
    0x88,0x48,0x49,0x89,0x4b,0x8b,0x8a,0x4a,0x4e,0x8e,0x8f,0x4f,0x8d,0x4d,0x4c,0x8c,
    0x44,0x84,0x85,0x45,0x87,0x47,0x46,0x86,0x82,0x42,0x43,0x83,0x41,0x81,0x80,0x40,
};
std::uint16_t crc16Official(const std::uint8_t* p, std::size_t n) {
    // GP-200.exe 0x553150/0x553190.  dl/bl both start at 0xff.  The two
    // 256-byte tables are copied verbatim from VA 0x7722d0/0x7723d0.
    std::uint8_t lo=0xff,hi=0xff;
    for(std::size_t i=0;i<n;++i){
        const std::uint8_t idx=static_cast<std::uint8_t>(p[i]^lo);
        lo=static_cast<std::uint8_t>(kCrcLoOfficial[idx]^hi);
        hi=kCrcHiOfficial[idx];
    }
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(lo)<<8)|hi);
}

bool readPcm16Mono(const fs::path& path, std::vector<float>& x, std::uint32_t& sr, std::string& error) {
    std::ifstream f(path, std::ios::binary); if(!f){error="Cannot open stimulus WAV: "+pathToUtf8(path);return false;}
    std::array<std::uint8_t,12> h{}; f.read(reinterpret_cast<char*>(h.data()),12);
    if(f.gcount()!=12 || std::memcmp(h.data(),"RIFF",4)||std::memcmp(h.data()+8,"WAVE",4)){error="Invalid stimulus WAV.";return false;}
    std::uint16_t fmt=0,ch=0,bits=0,align=0; std::vector<std::uint8_t> data;
    while(f){std::array<std::uint8_t,8> c{};f.read(reinterpret_cast<char*>(c.data()),8);if(f.gcount()!=8)break;const auto n=le32(c.data()+4);std::vector<std::uint8_t>b(n);if(n){f.read(reinterpret_cast<char*>(b.data()),n);if(static_cast<std::uint32_t>(f.gcount())!=n){error="Truncated stimulus WAV.";return false;}}if(n&1)f.seekg(1,std::ios::cur);
        if(!std::memcmp(c.data(),"fmt ",4)&&n>=16){fmt=le16(b.data());ch=le16(b.data()+2);sr=le32(b.data()+4);align=le16(b.data()+12);bits=le16(b.data()+14);}
        else if(!std::memcmp(c.data(),"data",4))data=std::move(b);
    }
    if(fmt!=1||ch!=1||bits!=16||align!=2||sr==0||data.empty()){error="Converter expects the generated mono PCM16 WAV.";return false;}
    x.resize(data.size()/2); for(std::size_t i=0;i<x.size();++i)x[i]=static_cast<std::int16_t>(le16(data.data()+2*i))/32768.0f;
    return true;
}

// The official GP-200.exe RTTI contains CDSPResampler24 together with the
// pre-v4 templated r8brain type CDSPResampler<CDSPFracInterpolator<24,673>>.
// The project pins r8brain version-3.7.  Do not use oneshot() here: the
// audited converter path constructs CDSPResampler24 with MaxInLen equal to the
// complete input length, calls process() on the whole double buffer once, then
// feeds equal-size zero buffers until the requested output count is produced.
// GP-200.exe 0x5a70a0.  The converter uses the same whole-buffer
// CDSPResampler24 path for the 70-second stimulus and for FIR serialization.
// Source/destination rates and the output length are first rounded to float;
// MaxInLen is the COMPLETE input length.  The whole input is converted to
// double and passed to process() once, then equal-size zero blocks are passed
// until the requested (float-ratio, truncated) sample count has been produced.
std::vector<float> resampleR8Brain24(const std::vector<float>& in, double inRate, double outRate) {
    if(in.empty()) return {};
    const float srcF=static_cast<float>(inRate),dstF=static_cast<float>(outRate);
    if(srcF==dstF) return in;
    if(in.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int inCount=static_cast<int>(in.size());
    const int targetCount=std::max(0,static_cast<int>(static_cast<float>(inCount)*dstF/srcF));
    std::vector<float> out(static_cast<std::size_t>(targetCount),0.0f);
    if(targetCount==0) return out;
    std::vector<double> block(in.size());
    for(std::size_t i=0;i<in.size();++i) block[i]=static_cast<double>(in[i]);
    // r8brain 3.7 CDSPResampler24's 4-argument source constructor has
    // ReqTransBand=2.0 by default and internally uses the 180.15 dB design
    // used by the converter.  HTUSBTools' wrapper treats process()'s returned
    // count as a cumulative count: it copies current-previous samples from the
    // beginning of the returned buffer to destination[previous].
    r8b::CDSPResampler24 rs(static_cast<double>(srcF),static_cast<double>(dstF),inCount,2.0);
    std::size_t previous=0; bool first=true;
    while(previous<out.size()){
        if(!first) std::fill(block.begin(),block.end(),0.0);
        first=false;
        double* produced=nullptr;
        const int count=rs.process(block.data(),inCount,produced);
        if(count<0 || produced==nullptr) break;
        const std::size_t current=static_cast<std::size_t>(count);
        if(current>previous){
            const std::size_t take=std::min<std::size_t>(current-previous,out.size()-previous);
            for(std::size_t i=0;i<take;++i) out[previous+i]=static_cast<float>(produced[i]);
        }
        previous=current;
    }
    rs.clear();
    return out;
}

// Exact 50-float tables reconstructed from GP-200.exe.  The branch tests in
// the official converter are against 44100.0f, 48000.0f and 96000.0f and the
// selected table is passed to the 50-tap conditioning FIR used by the initial
// 23-28 s identification stage.  These tables are trainer-only; they are not
// serialized into Block A or Block B.
constexpr std::array<float,50> kInitialFir44100 = {
 2.368210077f, 3.574280024f, 3.095710039f, -0.392399013f, -3.150949955f,
 -3.830709934f, -2.215640068f, -0.8829950094f, -0.212944001f, -0.1931400001f,
 0.2239619941f, 0.4040279984f, 0.3783220053f, 0.08940640092f, 0.1935559958f,
 0.283547014f, 0.3439449966f, 0.05293060094f, -0.03797249869f, -0.06761389971f,
 0.07335829735f, -0.02648900077f, -0.02291630022f, 0.02360440046f, 0.08525899798f,
 -0.009279790334f, 0.04381980002f, 0.0233258009f, 0.1348949969f, 0.006286839955f,
 -0.02591219917f, -0.01955270022f, 0.08531299978f, -0.01847779937f, -0.07664210349f,
 -0.0785638988f, 0.03350910172f, 0.02729599923f, -0.09931690246f, -0.09534750134f,
 0.05328249931f, 0.03427400067f, -0.09364210069f, -0.06697729975f, -0.01135309972f,
 0.03991980106f, -0.0771979019f, -0.09798060358f, 0.002032200107f, 0.04304929823f
};
constexpr std::array<float,50> kInitialFir48000 = {
 2.369808912f, 3.532199383f, 3.386006355f, 0.5711528063f, -2.494512081f,
 -3.850381851f, -3.163845062f, -1.511753678f, -0.5816931129f, -0.1761655957f,
 -0.1537622064f, 0.2662706077f, 0.4051620066f, 0.3895485103f, 0.1137828976f,
 0.1536727995f, 0.2604695857f, 0.3561989963f, 0.202282995f, -0.01788109913f,
 -0.05925950035f, -0.03052599914f, 0.07328040153f, -0.0387939997f, -0.0205725003f,
 0.02094990015f, 0.08917230368f, -0.0006698999787f, 0.03298040107f, 0.01907679997f,
 0.095199503f, 0.09397300333f, -0.02691840008f, -0.02818600088f, 0.003533599898f,
 0.08518820256f, -0.02848079987f, -0.07661850005f, -0.08217039704f, 0.0129757002f,
 0.05085289851f, -0.06227429956f, -0.1223426983f, -0.02208109945f, 0.08036129922f,
 -0.02014300041f, -0.09813290089f, -0.05815440044f, -0.003250899957f, 0.03926600143f
};
constexpr std::array<float,50> kInitialFir96000 = {
 2.369808912f, 3.144882202f, 3.532199383f, 3.684798717f, 3.386006594f,
 2.297138929f, 0.5711528063f, -1.175742626f, -2.494512081f, -3.369890928f,
 -3.85038209f, -3.795108557f, -3.163845062f, -2.270354748f, -1.511753798f,
 -0.987195015f, -0.5816931725f, -0.2824920118f, -0.1761655957f, -0.2019508034f,
 -0.1537622064f, 0.04795689881f, 0.2662706077f, 0.3740029931f, 0.4051620066f,
 0.4213505089f, 0.3895485103f, 0.2569816113f, 0.1137828976f, 0.08147999644f,
 0.1536727995f, 0.2281782031f, 0.2604695857f, 0.3036418855f, 0.3561989963f,
 0.3322631121f, 0.202282995f, 0.05376290157f, -0.01788109913f, -0.0349936001f,
 -0.05925950035f, -0.07576920092f, -0.03052599914f, 0.04923079908f, 0.07328040153f,
 0.01946049929f, -0.0387939997f, -0.04402400181f, -0.0205725003f, -0.004320300184f
};

const std::array<float,50>& initialFirForRate(double sr) {
    if (std::abs(sr - 44100.0) < 1.0) return kInitialFir44100;
    if (std::abs(sr - 48000.0) < 1.0) return kInitialFir48000;
    if (std::abs(sr - 96000.0) < 1.0) return kInitialFir96000;
    // Official tables exist only for the three reconstructed paths.  The NAM
    // path normally resolves to one of them; 48 kHz is the official fallback.
    return kInitialFir48000;
}

// Implemented after FirPlan: the official trainer routes this 50-tap
// conditioning filter through the same 64/128 partitioned FIR engine used for
// A/B (0x553aa0 -> 0x55b2e0/0x55b460), not a separate direct convolution.
std::vector<float> applyInitialConditioningFir(const std::vector<float>& in, double sr);

std::optional<std::size_t> objectEnd(const std::string& s,std::size_t start){
    if(start>=s.size()||s[start]!='{')return std::nullopt;int depth=0;bool str=false,esc=false;
    for(std::size_t i=start;i<s.size();++i){char c=s[i];if(str){if(esc)esc=false;else if(c=='\\')esc=true;else if(c=='"')str=false;continue;}if(c=='"'){str=true;continue;}if(c=='{')++depth;else if(c=='}'&&--depth==0)return i+1;}return std::nullopt;
}
bool prepareFullA2(const fs::path& namPath,const fs::path& work,fs::path& result,std::string& error){
    result=namPath; std::ifstream f(namPath,std::ios::binary);if(!f)return true;std::string s((std::istreambuf_iterator<char>(f)),{});
    if(s.find("\"SlimmableContainer\"")==std::string::npos)return true;
    const auto sk=s.find("\"submodels\""); if(sk==std::string::npos)return true; const auto ao=s.find('[',sk);if(ao==std::string::npos)return true;
    double best=-std::numeric_limits<double>::infinity();std::string bestModel;std::size_t p=ao+1;
    while(p<s.size()){while(p<s.size()&&(std::isspace(static_cast<unsigned char>(s[p]))||s[p]==','))++p;if(p>=s.size()||s[p]==']')break;if(s[p]!='{')break;auto e=objectEnd(s,p);if(!e)break;
        const auto mk=s.find("\"max_value\"",p),mod=s.find("\"model\"",p); if(mk<*e&&mod<*e){const auto col=s.find(':',mk);if(col==std::string::npos||col>=*e){p=*e;continue;}char* ep=nullptr;const double v=std::strtod(s.c_str()+col+1,&ep);const auto mc=s.find(':',mod);if(mc==std::string::npos||mc>=*e){p=*e;continue;}const auto mo=s.find('{',mc);auto me=mo==std::string::npos?std::nullopt:objectEnd(s,mo);if(me&&*me<=*e&&v>best){best=v;bestModel=s.substr(mo,*me-mo);}}
        p=*e;
    }
    if(bestModel.empty()){error="Could not extract the Full submodel from A2 SlimmableContainer.";return false;}
    result=work/L"independent_a2_full.nam";std::ofstream o(result,std::ios::binary|std::ios::trunc);if(!o){error="Cannot create temporary A2 Full NAM.";return false;}o.write(bestModel.data(),static_cast<std::streamsize>(bestModel.size()));return o.good();
}

bool renderNam(const fs::path& path,const std::vector<float>& stimulus44100,int blockSize,double targetScale,
               std::vector<float>& input,std::vector<float>& target,double& rate,std::string& error,const StatusCallback& status){
    try{
        auto dsp=nam::get_dsp(path); if(!dsp){error="NeuralAmpModelerCore could not load the NAM.";return false;}
        rate=dsp->GetExpectedSampleRate();if(!(rate>1000.0&&rate<384000.0))rate=48000.0;
        report(status,L"Independent: rendering NAM at "+std::to_wstring(static_cast<int>(std::llround(rate)))+L" Hz...");

        // GP-200.exe does NOT feed the trainer's 600-sample guard area through
        // the NAM or through the sample-rate converter.  The source WAV is
        // exactly 70 seconds.  When the trainer loads input/output it allocates
        // Fs*70 + 600 floats, copies only the first Fs*70 samples and leaves the
        // final 600 floats zero.  StimulusBuilder also appends 600 samples for
        // the legacy HTUSBTools path, so strip them here before the independent
        // render and recreate the trainer buffers after rendering.
        constexpr std::size_t kSourceRate=44100;
        constexpr std::size_t kSource70=70*kSourceRate;
        std::vector<float> source70(kSource70,0.0f);
        const std::size_t sourceCopy=std::min(source70.size(),stimulus44100.size());
        std::copy_n(stimulus44100.begin(),sourceCopy,source70.begin());

        std::vector<float> renderedInput=resampleR8Brain24(source70,44100.0,rate);
        const float fs=static_cast<float>(rate);
        const float n70f=fs*70.0f; // same single-precision product used by the loader
        const std::size_t n70=static_cast<std::size_t>(std::max(0.0f,n70f));
        renderedInput.resize(n70,0.0f);
        std::vector<float> renderedTarget(n70,0.0f);

        (void)blockSize; // official NAM Reset max block is fixed at 1024 samples.
        constexpr int kOfficialNamBlock=1024;
        dsp->Reset(rate,kOfficialNamBlock);
        std::vector<NAM_SAMPLE> ib(kOfficialNamBlock,NAM_SAMPLE{}),ob(kOfficialNamBlock,NAM_SAMPLE{});
        NAM_SAMPLE* ip[1]={ib.data()}; NAM_SAMPLE* op[1]={ob.data()};
        (void)targetScale; // retained in the public config only for ABI/source compatibility.
        constexpr float targetScaleF=0.31f; // GP-200.exe getConvertNormalWav literal scale.
        for(std::size_t pos=0;pos<n70;pos+=kOfficialNamBlock){
            const int n=static_cast<int>(std::min<std::size_t>(kOfficialNamBlock,n70-pos));
            for(int i=0;i<n;++i) ib[static_cast<std::size_t>(i)]=static_cast<NAM_SAMPLE>(renderedInput[pos+static_cast<std::size_t>(i)]);
            // HTUSBTools/GP-200 wrapper resets the NAM with max block 0x400,
            // but each process call receives min(1024, remaining).  Therefore
            // the final partial block is processed at its true length; it is
            // not padded and processed as a full 1024-frame block.
            dsp->process(ip,op,n);
            for(int i=0;i<n;++i){
                const float y=static_cast<float>(ob[static_cast<std::size_t>(i)]);
                renderedTarget[pos+static_cast<std::size_t>(i)]=y*targetScaleF;
            }
        }

        input=std::move(renderedInput);
        target=std::move(renderedTarget);
        input.resize(n70+600,0.0f);
        target.resize(n70+600,0.0f);
        return true;
    }catch(const std::exception& e){error=std::string("NAM renderer: ")+e.what();return false;}
}

// Exact preprocessing reconstructed at GP-200.exe 0x559d80.
// The target buffer length is Fs*70+600.  All accumulation and arithmetic is
// float32.  First remove the mean, then fit/subtract a line using x=1..N.
void detrend(std::vector<float>& y){
    if(y.empty())return;
    const float n=static_cast<float>(y.size());

    float sum=0.0f;
    for(float v:y)sum+=v;
    const float mean=sum/n;
    for(float&v:y)v-=mean;

    float sumX=0.0f,sumY=0.0f,sumXY=0.0f,sumXX=0.0f;
    for(std::size_t i=0;i<y.size();++i){
        const float x=static_cast<float>(i+1);
        const float v=y[i];
        sumX+=x;
        sumY+=v;
        sumXY+=v*x;
        sumXX+=x*x;
    }

    const float numerator=n*sumXY-sumY*sumX;
    const float denominator=n*sumXX-sumX*sumX;
    const float slope=(denominator!=0.0f)?numerator/denominator:0.0f;
    const float intercept=(sumY-slope*sumX)/n;
    for(std::size_t i=0;i<y.size();++i){
        const float x=static_cast<float>(i+1);
        y[i]-=slope*x+intercept;
    }
}

std::size_t detectLatency(const std::vector<float>& y,double sr){
    // Official code truncates the trainer sample-rate float to int, searches
    // exactly 600 samples from 6*Fs and returns 600 if no threshold crossing is
    // found.  fabs(sample) is then compared as double against 0.01.
    const int fs=static_cast<int>(static_cast<float>(sr));
    if(fs<=0)return 600;
    const std::size_t base=static_cast<std::size_t>(6*fs);
    const std::size_t lim=std::min(y.size(),base+600);
    for(std::size_t i=base;i<lim;++i){
        const float av=std::fabs(y[i]);
        if(static_cast<double>(av)>0.01)return i-base;
    }
    return 600;
}
std::vector<float> alignLeft(const std::vector<float>& x,std::size_t n){std::vector<float> y(x.size(),0);if(n>=x.size())return y;std::copy(x.begin()+static_cast<std::ptrdiff_t>(n),x.end(),y.begin());return y;}

struct PK {float pp=.1f,pn=.1f,kp=1,kn=1;};

// Exact reconstruction of 0x558c30: 100 ms extrema over the first 5 s,
// P fixed by branch extrema, K seeded from the small-signal slope up to
// 0.5*P, then the seed is searched with the official multipliers
// 0.80, 0.85, ... 1.20. Positive K is selected first, then negative K.
PK fitPk(const std::vector<float>& in,const std::vector<float>& out,double sr){
    // Literal port of GP-200.exe 0x558c30.  The caller passes the first
    // 5*Fs samples.  The routine builds three N-window vectors:
    //   xAbs  = max(abs(input))
    //   yPos  = max(output, 0)
    //   yNeg  = min(output, 0)
    // and then a 2*N signed (x,y) dataset ordered as negative/reversed first,
    // positive/forward second.  That ordering is observable because the K
    // search error accumulator is float32.
    const float fs=static_cast<float>(sr);
    const float winF=static_cast<float>(static_cast<double>(fs)*0.1);
    if(!(winF>0.0f)) return PK{};

    // Caller at 0x55a0xx computes cvttss2si(float(Fs) * 5.0f) and passes
    // that exact length to 0x558c30; the trainer vectors themselves are longer.
    const int literalLength=static_cast<int>(fs*5.0f); // MULSS + CVTTSS2SI
    const int passedLength=std::max(0,std::min({static_cast<int>(in.size()),
                                                static_cast<int>(out.size()),
                                                literalLength}));
    const float lengthF=static_cast<float>(passedLength);
    const float quotient=lengthF/winF;
    const int windows=static_cast<int>(static_cast<float>(std::floor(static_cast<double>(quotient))));
    if(windows<=0) return PK{};

    std::vector<float> xAbs(static_cast<std::size_t>(windows),0.0f);
    std::vector<float> yPos(static_cast<std::size_t>(windows),0.0f);
    std::vector<float> yNeg(static_cast<std::size_t>(windows),0.0f);
    float pp=0.0f,pn=0.0f;

    for(int wi=0;wi<windows;++wi){
        const float fEnd=static_cast<float>(wi+1)*winF;
        const float fBegin=static_cast<float>(wi)*winF;
        const int endI=static_cast<int>(fEnd);      // cvttss2si
        const int beginI=static_cast<int>(fBegin);  // cvttss2si
        const int b=std::max(0,std::min(beginI,passedLength));
        const int e=std::max(0,std::min(endI,passedLength));
        float xa=0.0f,yp=0.0f,yn=0.0f;
        for(int i=b;i<e;++i){
            const float ax=std::fabs(in[static_cast<std::size_t>(i)]); // 0x4223a0
            if(ax>xa) xa=ax;
            const float y=out[static_cast<std::size_t>(i)];
            if(y>yp) yp=y;
            if(yn>y) yn=y;
        }
        const std::size_t w=static_cast<std::size_t>(wi);
        xAbs[w]=xa; yPos[w]=yp; yNeg[w]=yn;
        pp=std::max(pp,yp);
        const float negMag=flipFloatSignBit(yn); // XOR sign bit exactly as 0x558d9c
        pn=std::max(pn,negMag);
    }

    // 0x558de6..0x558e96: explicit signed point arrays.  Do not collapse this
    // into two branch errors per window: the official SSE walks these 2*N
    // points in this exact order and accumulates in float32.
    const std::size_t N=static_cast<std::size_t>(windows);
    std::vector<float> signedX(2*N),signedY(2*N);
    for(std::size_t i=0;i<N;++i){
        const std::size_t rev=N-1-i;
        signedX[i]=flipFloatSignBit(xAbs[rev]);
        signedY[i]=yNeg[rev];
        signedX[N+i]=xAbs[i];
        signedY[N+i]=yPos[i];
    }

    // First sample crossing +/-P/2, expressed exactly as the EXE's double
    // comparisons.  The count is one-past the crossing and is used by the
    // through-origin double regression.
    int posCount=0;
    const double posHalf=static_cast<double>(pp)*0.5;
    for(int i=0;i<windows;++i){
        if(static_cast<double>(yPos[static_cast<std::size_t>(i)])>=posHalf){posCount=i+1;break;}
    }
    int negCount=0;
    const double negHalf=static_cast<double>(pn)*-0.5;
    for(int i=0;i<windows;++i){
        if(negHalf>=static_cast<double>(yNeg[static_cast<std::size_t>(i)])){negCount=i+1;break;}
    }

    // 0x558f24..0x559181.  The compiler unrolls four samples, but the double
    // dependency chain remains sample-order sequential, so these scalar loops
    // preserve the exact product/add order.
    double posXY=0.0,posXX=0.0;
    for(int i=0;i<posCount;++i){
        const double x=static_cast<double>(xAbs[static_cast<std::size_t>(i)]);
        const double y=static_cast<double>(yPos[static_cast<std::size_t>(i)]);
        posXY += y*x;
        posXX += x*x;
    }
    double negXY=0.0,negXX=0.0;
    for(int i=0;i<negCount;++i){
        const double x=static_cast<double>(xAbs[static_cast<std::size_t>(i)]);
        const double y=static_cast<double>(yNeg[static_cast<std::size_t>(i)]);
        negXY -= y*x;
        negXX += x*x;
    }
    const float posSlope=static_cast<float>(posXY/posXX);
    const float negSlope=static_cast<float>(negXY/negXX);
    const float kp0=posSlope/pp;
    const float kn0=negSlope/pn;

    auto signedSse=[&](float kp,float kn)->float{
        float err=0.0f;
        for(std::size_t i=0;i<signedX.size();++i){
            const float x=signedX[i];
            float pred;
            if(x>0.0f){
                const float z=x*kp;
                const float nz=flipFloatSignBit(z);
                const float ex=preciseExpF(nz);
                const float oneMinus=1.0f-ex;
                pred=oneMinus*pp;
            }else{
                const float z=x*kn;
                const float ex=preciseExpF(z);
                const float exMinus=ex-1.0f;
                pred=exMinus*pn;
            }
            const float d=signedY[i]-pred;
            const float sq=d*d;
            err+=sq;
        }
        return err;
    };

    // Search positive K first.  Candidate multiplier is generated by
    // float<-double(float+0.05), compared as double against 1.2, and ties
    // select the later candidate (bestErr >= err).
    float mul=0.800000011920929f;
    float bestMulP=1.0f;
    float bestErr=std::numeric_limits<float>::max();
    while(static_cast<double>(mul)<=1.2){
        const float cand=mul*kp0;
        const float e=signedSse(cand,kn0);
        if(!(bestErr<e)){bestErr=e;bestMulP=mul;}
        mul=static_cast<float>(static_cast<double>(mul)+0.05);
    }
    const float bestKp=bestMulP*kp0;

    // Then search negative K with the selected positive K held fixed.
    mul=0.800000011920929f;
    float bestMulN=1.0f;
    bestErr=std::numeric_limits<float>::max();
    while(static_cast<double>(mul)<=1.2){
        const float cand=mul*kn0;
        const float e=signedSse(bestKp,cand);
        if(!(bestErr<e)){bestErr=e;bestMulN=mul;}
        mul=static_cast<float>(static_cast<double>(mul)+0.05);
    }

    PK r{};
    r.pp=pp;
    r.pn=pn;
    r.kp=bestKp;
    r.kn=bestMulN*kn0;
    return r;
}
// Trainer biquad numerical path from GP-200.exe (0x557900 et al.).
// The official trainer uses direct-form II state in double, scales the input
// by 1000, rounds the internal output to float, then applies 0.001 and rounds
// to float again. The reciprocal scales cancel mathematically but the
// intermediate float conversion is observable and is therefore retained.
struct Biquad{
    double b0=1,b1=0,b2=0,a1=0,a2=0;
    double w1=0,w2=0;
    float p(float x){
        const double w0=static_cast<double>(x)*1000.0-a1*w1-a2*w2;
        const double yd=b0*w0+b1*w1+b2*w2;
        w2=w1; w1=w0;
        const float rounded=static_cast<float>(yd);
        return static_cast<float>(static_cast<double>(rounded)*0.001);
    }
};
Biquad postForRate(double fs){
    // GP-200.exe computes this section in float, then stores/promotes the five
    // float32 results into the double CLO fields.  Keeping the arithmetic in
    // float reproduces the exact 48 kHz coefficients seen in official CLOs.
    constexpr float c=177.7158051f,w2=15791.45215f;
    const float f=static_cast<float>(fs),f2=f*f,D=f2+c*f+w2;
    const float b0=f2/D,b1=-2.0f*b0,b2=b0;
    const float a1=-(2.0f*f2-2.0f*w2)/D;
    const float a2=(f2-c*f+w2)/D;
    Biquad q;q.b0=static_cast<double>(b0);q.b1=static_cast<double>(b1);q.b2=static_cast<double>(b2);q.a1=static_cast<double>(a1);q.a2=static_cast<double>(a2);return q;
}
struct AP{float a=0,s=0;float p(float x){const float y=s+a*x;s=x-a*y;return y;}};
struct Poly{std::vector<AP>a,b;float d=0;Poly(std::initializer_list<float>x,std::initializer_list<float>y){for(float v:x)a.push_back({v,0});for(float v:y)b.push_back({v,0});}float r(std::vector<AP>&v,float x){for(auto&s:v)x=s.p(x);return x;}void up(float x,float&e,float&o){e=r(a,x);o=r(b,x);}float down(float e,float o){const float x=r(a,e),y=r(b,o),z=.5f*(x+d);d=y;return z;}};

bool powerOfTwo(std::size_t n){return n && !(n&(n-1));}
void fft(std::vector<std::complex<double>>& a,bool inv){
    const std::size_t n=a.size();
    for(std::size_t i=1,j=0;i<n;++i){std::size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
    for(std::size_t len=2;len<=n;len<<=1){const double ang=(inv?2:-2)*kPi/static_cast<double>(len);const std::complex<double> wl(std::cos(ang),std::sin(ang));for(std::size_t i=0;i<n;i+=len){std::complex<double>w(1,0);for(std::size_t j=0;j<len/2;++j){auto u=a[i+j],v=a[i+j+len/2]*w;a[i+j]=u+v;a[i+j+len/2]=u-v;w*=wl;}}}
    if(inv)for(auto&v:a)v/=static_cast<double>(n);
}
void transformAny(std::vector<std::complex<double>>& a,bool inv){
    if(powerOfTwo(a.size())){fft(a,inv);return;}
    const std::size_t n=a.size(); std::vector<std::complex<double>> o(n);
    const double sign=inv?1.0:-1.0;
    for(std::size_t k=0;k<n;++k){std::complex<long double> sum(0,0);for(std::size_t j=0;j<n;++j){const long double ph=sign*2.0L*static_cast<long double>(kPi)*static_cast<long double>(j)*static_cast<long double>(k)/static_cast<long double>(n);const std::complex<long double>w(std::cos(ph),std::sin(ph));sum+=std::complex<long double>(a[j].real(),a[j].imag())*w;}if(inv)sum/=static_cast<long double>(n);o[k]={static_cast<double>(sum.real()),static_cast<double>(sum.imag())};}
    a.swap(o);
}

// 0x553aa0 / 0x55b2e0 / 0x55b460 / 0x55b9a0: exact trainer FIR engine.
// GP-200.exe partitions FIRs into 64-sample blocks and uses a 128-point
// complex float transform.  The transform's twiddles are not generated with
// libm: 0x55b830 expands this exact int16 sine table using 1/32768.
struct ComplexF { float re=0.0f, im=0.0f; };

constexpr std::array<std::int16_t,128> kOfficialFft128SinQ15 = {
       0,  1608,  3212,  4808,  6393,  7962,  9512, 11039,
   12540, 14010, 15447, 16846, 18205, 19520, 20788, 22006,
   23170, 24279, 25330, 26320, 27246, 28106, 28899, 29622,
   30274, 30853, 31357, 31786, 32138, 32413, 32610, 32729,
   32767, 32729, 32610, 32413, 32138, 31786, 31357, 30853,
   30274, 29622, 28899, 28106, 27246, 26320, 25330, 24279,
   23170, 22006, 20788, 19520, 18205, 16846, 15447, 14010,
   12540, 11039,  9512,  7962,  6393,  4808,  3212,  1608,
       0, -1608, -3212, -4808, -6393, -7962, -9512,-11039,
  -12540,-14010,-15447,-16846,-18205,-19520,-20788,-22006,
  -23170,-24279,-25330,-26320,-27246,-28106,-28899,-29622,
  -30274,-30853,-31357,-31786,-32138,-32413,-32610,-32729,
  -32768,-32729,-32610,-32413,-32138,-31786,-31357,-30853,
  -30274,-29622,-28899,-28106,-27246,-26320,-25330,-24279,
  -23170,-22006,-20788,-19520,-18205,-16846,-15447,-14010,
  -12540,-11039, -9512, -7962, -6393, -4808, -3212, -1608
};

inline float officialSin128(std::size_t idx) {
    return static_cast<float>(kOfficialFft128SinQ15[idx & 127u]) * 3.0517578125e-05f;
}
inline float officialCos128(std::size_t idx) { return officialSin128((idx + 32u) & 127u); }

// Literal radix-2 data flow corresponding to 0x55b9a0 after its bit-reversal
// permutation. Direction 1 is inverse in the EXE; inverse divides every
// component by exactly 128 (0.0078125f).
void fft128Official(std::array<ComplexF,128>& a, bool inverse) {
    constexpr int N=128;
    // 0x55b9ed..0x55ba31: use the constructor-generated permutation table.
    // For N=128 it is exactly the conventional 7-bit reversal permutation;
    // swaps are performed only when i < perm[i].
    for(int i=0;i<N-1;++i){
        unsigned x=static_cast<unsigned>(i), r=0;
        for(int b=0;b<7;++b){r=(r<<1)|(x&1u);x>>=1;}
        const int j=static_cast<int>(r);
        if(i<j)std::swap(a[static_cast<std::size_t>(i)],a[static_cast<std::size_t>(j)]);
    }

    // Object field +0x08 is the number of stages (7).  The EXE starts
    // groupLength=2 / half=1 and doubles both after every stage.
    int groupLength=2;
    int half=1;
    for(int stage=0;stage<7;++stage){
        const int m=N/groupLength;

        // 0x55ba60..0x55bac4: j=0 has its own no-twiddle loop.  Preserve
        // subtraction-before-addition and the stores used by the scalar SSE.
        for(int base=0;base<N;base+=groupLength){
            const int hi=base+half;
            const float highRe=a[static_cast<std::size_t>(hi)].re;
            const float lowRe =a[static_cast<std::size_t>(base)].re;
            const float highIm=a[static_cast<std::size_t>(hi)].im;
            const float lowIm =a[static_cast<std::size_t>(base)].im;
            const float outHighRe=lowRe-highRe;
            const float outHighIm=lowIm-highIm;
            const float outLowRe=highRe+lowRe;
            const float outLowIm=lowIm+highIm;
            a[static_cast<std::size_t>(hi)].re=outHighRe;
            a[static_cast<std::size_t>(hi)].im=outHighIm;
            a[static_cast<std::size_t>(base)].re=outLowRe;
            a[static_cast<std::size_t>(base)].im=outLowIm;
        }

        // 0x55bad3..0x55bc27.  Twiddle real is read from the sine table at
        // (N/4 - j*m) mod N.  Twiddle imag is sine(j*m), and its sign is
        // flipped unless direction==1.  direction==1 is the inverse path.
        for(int j=1;j<half;++j){
            const int ti=j*m;
            const int wrIndex=(N/4-ti)%N < 0 ? ((N/4-ti)%N+N) : ((N/4-ti)%N);
            const float wr=officialSin128(static_cast<std::size_t>(wrIndex));
            float wi=officialSin128(static_cast<std::size_t>(ti%N));
            if(!inverse) wi=flipFloatSignBit(wi);

            for(int base=j;base<N;base+=groupLength){
                const int hi=base+half;
                const float highRe=a[static_cast<std::size_t>(hi)].re;
                const float highIm=a[static_cast<std::size_t>(hi)].im;

                // 0x55bb80..0x55bbe5 exact scalar operation order.
                float vr=highRe*wr;
                const float t0=highIm*wi;
                vr=vr-t0;
                float vi=highRe*wi;
                const float t1=highIm*wr;
                vi=vi+t1;

                const float lowRe=a[static_cast<std::size_t>(base)].re;
                const float lowIm=a[static_cast<std::size_t>(base)].im;
                const float outHighRe=lowRe-vr;
                const float outLowRe =lowRe+vr;
                const float outHighIm=lowIm-vi;
                const float outLowIm =lowIm+vi;
                a[static_cast<std::size_t>(hi)].re=outHighRe;
                a[static_cast<std::size_t>(base)].re=outLowRe;
                a[static_cast<std::size_t>(hi)].im=outHighIm;
                a[static_cast<std::size_t>(base)].im=outLowIm;
            }
        }

        half+=half;
        groupLength+=groupLength;
    }

    // 0x55bc4b..0x55be23: direction==1 divides every real and imaginary
    // component by float(N), using DIVSS/DIVPS.  Do not replace with *1/128.
    if(inverse){
        const float divisor=static_cast<float>(N);
        for(auto&v:a){v.re/=divisor;v.im/=divisor;}
    }
}
inline ComplexF mulOfficial(ComplexF a, ComplexF b){
    const float p0=a.re*b.re;
    const float p1=a.im*b.im;
    const float re=p0-p1;
    const float p2=a.re*b.im;
    const float p3=a.im*b.re;
    return {re,p2+p3};
}

struct FirPlan{
    static constexpr std::size_t kPart=64,kN=128,kPos=65;
    using PositiveSpectrum=std::array<ComplexF,kPos>;
    std::size_t len=0,partitions=0;
    std::vector<PositiveSpectrum> H;
    FirPlan()=default;
    explicit FirPlan(const std::vector<float>&h){
        len=h.size();if(!len)return;
        partitions=(len+kPart-1)/kPart;H.resize(partitions);
        for(std::size_t p=0;p<partitions;++p){
            std::array<ComplexF,kN> z{};
            const std::size_t base=p*kPart;
            for(std::size_t i=0;i<kPart&&base+i<len;++i)z[i]={h[base+i],0.0f};
            fft128Official(z,false);
            // 0x55b120 stores only the 65 non-redundant bins of the real
            // 128-point transform.  0x55b460 subsequently accumulates only
            // bins 0..64 and reconstructs bins 65..127 by conjugation.
            for(std::size_t k=0;k<kPos;++k)H[p][k]=z[k];
        }
    }
    void run(const std::vector<float>&x,std::vector<float>&y)const{
        if(len==0||partitions==0){y=x;return;}
        y.assign(x.size(),0.0f);
        std::vector<PositiveSpectrum> ring(partitions);
        for(auto&r:ring)for(auto&v:r)v={0.0f,0.0f};
        std::array<float,kPart> overlap{};
        std::size_t current=0;
        for(std::size_t pos=0;pos<x.size();pos+=kPart){
            std::array<ComplexF,kN> Xfull{};
            const std::size_t count=std::min(kPart,x.size()-pos);
            for(std::size_t i=0;i<count;++i)Xfull[i]={x[pos+i],0.0f};
            fft128Official(Xfull,false);
            for(std::size_t k=0;k<kPos;++k)ring[current][k]=Xfull[k];

            PositiveSpectrum Ypos{};
            // 0x55b584..0x55b68c: partition-major accumulation; for each
            // partition the EXE walks bins 0..64 in order with scalar float
            // mul/sub/add operations.  Do not accumulate the redundant half.
            for(std::size_t p=0;p<partitions;++p){
                const std::size_t ri=(current+partitions-p)%partitions;
                for(std::size_t k=0;k<kPos;++k){
                    const ComplexF a=ring[ri][k], b=H[p][k];
                    const float p0=a.re*b.re;
                    const float p1=a.im*b.im;
                    const float re=p0-p1;
                    Ypos[k].re+=re;
                    const float p2=a.re*b.im;
                    const float p3=a.im*b.re;
                    const float im=p2+p3;
                    Ypos[k].im+=im;
                }
            }

            // 0x55b63a..0x55b64e / 0x55b6e2..0x55b7ad: recreate the
            // conjugate half of the real spectrum before the inverse FFT.
            std::array<ComplexF,kN> Yfull{};
            Yfull[0]=Ypos[0];
            Yfull[64]=Ypos[64];
            for(std::size_t k=1;k<64;++k){
                Yfull[k]=Ypos[k];
                Yfull[kN-k]={Ypos[k].re,-Ypos[k].im};
            }
            fft128Official(Yfull,true);

            // 0x55b7d0..0x55b80f: first half + saved overlap, save the
            // second half as next overlap, then expose exactly 64 samples.
            for(std::size_t i=0;i<count;++i)y[pos+i]=Yfull[i].re+overlap[i];
            for(std::size_t i=0;i<kPart;++i)overlap[i]=Yfull[i+kPart].re;
            current=(current+1)%partitions;
        }
    }
};

std::vector<float> applyInitialConditioningFir(const std::vector<float>& in, double sr) {
    const auto& table = initialFirForRate(sr);
    const std::vector<float> h(table.begin(), table.end());
    FirPlan fp(h);
    std::vector<float> out;
    fp.run(in, out);
    return out;
}

struct Model{std::vector<float>A,B;PK pk;Biquad pre,post;};

// 0x5589d0 / 0x5580e0: PRE is executed even though its coefficients are
// identity on the NAM path.  Because the official biquad has observable
// float-rounding boundaries, identity PRE must not be optimized away.
void renderCoreNoFir(const Model& m,const std::vector<float>& in,std::vector<float>& out){
    Biquad pre=m.pre,post=m.post;
    Poly u1({.045728147029876709f,.3325011134147644f,.66320204734802246f,.93385583162307739f},{.16808754205703735f,.50448572635650635f,.80378085374832153f});
    Poly u2({.054230779409408569f,.39879697561264038f,.86291784048080444f},{.19969958066940308f,.62109684944152832f});
    Poly d1({.070765949785709381f,.51316756010055542f},{.25785309076309204f,.81731736660003662f});
    Poly d2({.054217524826526642f,.38308733701705933f,.74872094392776489f},{.19679796695709229f,.57313638925552368f,.91429370641708374f});
    out.resize(in.size());
    auto shape=[&](float x){if(x>0.0f){const float e=preciseExpF(-(m.pk.kp*x));return m.pk.pp*(1.0f-e);}const float e=preciseExpF(m.pk.kn*x);return m.pk.pn*(e-1.0f);};
    for(std::size_t i=0;i<in.size();++i){
        const float xin=pre.p(in[i]);
        float e,o,q0,q1;u1.up(xin,e,o);u2.up(e,q0,q1);q0=shape(q0);q1=shape(q1);const float z0=d1.down(q0,q1);u2.up(o,q0,q1);q0=shape(q0);q1=shape(q1);const float z1=d1.down(q0,q1);out[i]=post.p(d2.down(z0,z1));
    }
}

void renderModel(const Model& m,const std::vector<float>& in,std::vector<float>& out,bool includeB=true){
    Biquad preBq=m.pre;
    std::vector<float> preIn(in.size());
    for(std::size_t i=0;i<in.size();++i)preIn[i]=preBq.p(in[i]);
    FirPlan ap(m.A);std::vector<float>a;ap.run(preIn,a);Biquad post=m.post;
    Poly u1({.045728147029876709f,.3325011134147644f,.66320204734802246f,.93385583162307739f},{.16808754205703735f,.50448572635650635f,.80378085374832153f});
    Poly u2({.054230779409408569f,.39879697561264038f,.86291784048080444f},{.19969958066940308f,.62109684944152832f});
    Poly d1({.070765949785709381f,.51316756010055542f},{.25785309076309204f,.81731736660003662f});
    Poly d2({.054217524826526642f,.38308733701705933f,.74872094392776489f},{.19679796695709229f,.57313638925552368f,.91429370641708374f});
    std::vector<float>postOut(a.size());auto shape=[&](float x){if(x>0.0f){const float e=preciseExpF(-(m.pk.kp*x));return m.pk.pp*(1.0f-e);}const float e=preciseExpF(m.pk.kn*x);return m.pk.pn*(e-1.0f);};
    for(std::size_t i=0;i<a.size();++i){float e,o,q0,q1;u1.up(a[i],e,o);u2.up(e,q0,q1);q0=shape(q0);q1=shape(q1);const float z0=d1.down(q0,q1);u2.up(o,q0,q1);q0=shape(q0);q1=shape(q1);const float z1=d1.down(q0,q1);postOut[i]=post.p(d2.down(z0,z1));}
    if(includeB){FirPlan bp(m.B);bp.run(postOut,out);}else out=std::move(postOut);
}

std::vector<float> sliceSignal(const std::vector<float>&x,std::size_t b,std::size_t e){b=std::min(b,x.size());e=std::min(e,x.size());if(e<=b)return {};return std::vector<float>(x.begin()+static_cast<std::ptrdiff_t>(b),x.begin()+static_cast<std::ptrdiff_t>(e));}

// GP-200.exe uses float Fs * float seconds and cvttss2si (truncate toward zero)
// for all trainer segment boundaries.
std::size_t officialTimeIndex(double sr,float seconds){
    const float fs=static_cast<float>(sr);
    const float x=fs*seconds;
    if(!(x>0.0f)) return 0;
    return static_cast<std::size_t>(static_cast<int>(x));
}

std::vector<float> hammingF(std::size_t n){
    std::vector<float>w(n,1.0f);if(n<=1)return w;
    // GP-200.exe: float phase arithmetic, precise cosine helper, then the
    // Hamming multiply/subtract remain single-precision (mulss/subss).
    const float twoPi=6.2831854820251465f;
    const float denom=static_cast<float>(n-1);
    for(std::size_t i=0;i<n;++i){
        const float ph=(static_cast<float>(i)*twoPi)/denom;
        const float c=preciseCosF(ph);
        // 0x55586e..0x555885: both constants and both arithmetic
        // operations are single precision (mulss/subss).
        w[i]=0.5400000214576721f-c*0.46000000834465027f;
    }
    return w;
}

// The final Block-B routine has a different arithmetic boundary for its
// Hamming weights: phase/cosine remain float, then cos is promoted and the
// 0.54/0.46 multiply/subtract is performed in double before the final float
// conversion (HTUSBTools.dll 0x18009ad86..0x18009aee0).
std::vector<float> hammingFinalBOfficial(std::size_t n){
    std::vector<float>w(n,1.0f);if(n<=1)return w;
    const float twoPi=6.2831854820251465f;
    const float denom=static_cast<float>(n-1);
    for(std::size_t i=0;i<n;++i){
        const float ph=(static_cast<float>(i)*twoPi)/denom;
        const float c=preciseCosF(ph);
        w[i]=static_cast<float>(0.54-static_cast<double>(c)*0.46);
    }
    return w;
}

float sumFloatFinalBOfficial(const std::vector<float>&x){
    float sum=0.0f;std::size_t i=0;
    for(;i+4<=x.size();i+=4){sum+=x[i];sum+=x[i+1];sum+=x[i+2];sum+=x[i+3];}
    for(;i<x.size();++i)sum+=x[i];
    return sum;
}

void energiesFinalBOfficial(const std::vector<float>&target,const std::vector<float>&model,float&et,float&em){
    et=0.0f;em=0.0f;const std::size_t n=std::min(target.size(),model.size());std::size_t i=0;
    for(;i+4<=n;i+=4){
        float v=target[i];et+=v*v;v=model[i];em+=v*v;
        v=target[i+1];et+=v*v;v=model[i+1];em+=v*v;
        v=target[i+2];et+=v*v;v=model[i+2];em+=v*v;
        v=target[i+3];et+=v*v;v=model[i+3];em+=v*v;
    }
    for(;i<n;++i){float v=target[i];et+=v*v;v=model[i];em+=v*v;}
}
std::vector<double> fftFrequencyGrid(double sr){std::vector<double>f(kBins);for(std::size_t k=0;k<kBins;++k)f[k]=static_cast<double>(k)*(sr*0.5)/static_cast<double>(kBins-1);return f;}
std::vector<float> fftFrequencyGridF(double sr){
    // Trainer ctor 0x5539a8 calls 0x4225b0 for the N/2+1 frequency grid.
    const float fs=static_cast<float>(sr);
    const float ny=fs*0.5f;
    std::vector<float> f(kBins);
    if(kBins==1){f[0]=ny;return f;}
    f[0]=0.0f;
    const float delta=ny/static_cast<float>(kBins-1);
    for(std::size_t i=1;i<kBins;++i)f[i]=f[i-1]+delta;
    f[kBins-1]=ny;
    return f;
}
std::vector<double> linspace(double a,double b,std::size_t n){std::vector<double>v(n);if(!n)return v;if(n==1){v[0]=a;return v;}for(std::size_t i=0;i<n;++i)v[i]=a+(b-a)*static_cast<double>(i)/static_cast<double>(n-1);v.front()=a;v.back()=b;return v;}

void fftF(std::vector<std::complex<float>>& a,bool inv);

// 0x423180 wraps Takuya Ooura's double FFT4G rdft().  GP-200.exe first
// converts the folded float frame to double, calls rdft(N,+1), then rounds the
// packed positive-frequency result back to separate float real/imag arrays.
struct OouraRfft2048Official {
    // GP-200.exe 0x423180 owns one FFT4G work object and reuses its ip/w
    // tables for every frame (and for target/model transforms).  Preserve
    // that lifetime exactly instead of rebuilding the tables per call.
    std::array<int,64> ip{};
    std::array<double,kFft/2> w{};
    OouraRfft2048Official(){ ip.fill(0); w.fill(0.0); ip[0]=0; }

    void run(const std::vector<float>& in,std::array<float,kBins>& re,std::array<float,kBins>& im){
        std::array<double,kFft> a{};
        for(std::size_t i=0;i<kFft;++i)a[i]=static_cast<double>(i<in.size()?in[i]:0.0f);
        lsx_rdft(static_cast<int>(kFft),+1,a.data(),ip.data(),w.data());
        re.fill(0.0f);im.fill(0.0f);
        re[0]=static_cast<float>(a[0]);
        im[0]=0.0f;
        for(std::size_t k=1;k<kFft/2;++k){
            re[k]=static_cast<float>(a[2*k]);
            im[k]=static_cast<float>(-a[2*k+1]);
        }
        re[kFft/2]=static_cast<float>(a[1]);
        im[kFft/2]=0.0f;
    }
};

// 0x5557c0: ceil(0.125*Fs) Hamming frames, 50% overlap, mean removal,
// modulo-2048 folding, Ooura RDFT, and float32 Sxx/Sxy accumulation.
std::vector<float> ratioSpectrumF(const std::vector<float>& model,const std::vector<float>& target,double sr){
    const float fs=static_cast<float>(sr);
    // 0x5538xx: float Fs * 0.125f -> double + 0.5 -> cvttsd2si.
    const int Li=static_cast<int>(static_cast<double>(fs*0.125f)+0.5);
    const std::size_t L=static_cast<std::size_t>(std::max(1,Li));
    const std::size_t hop=std::max<std::size_t>(1,L-L/2);
    const auto wv=hammingF(L);
    const std::size_t total=std::min(model.size(),target.size());
    if(total<L)return std::vector<float>(kBins,1.0f);

    std::vector<float>sxx(kBins,0.0f),sxyRe(kBins,0.0f),sxyIm(kBins,0.0f);
    OouraRfft2048Official rdft;
    std::size_t frames=0,startPos=0;
    const std::size_t finalStart=total-L;
    for(;;){
        // 0x5557c0 clamps every scheduled start to total-L, ensuring the
        // final window is always anchored exactly at the end of the segment.
        const std::size_t p=std::min(startPos,finalStart);
        std::vector<float> xframe(L),yframe(L);
        float mx=0.0f,my=0.0f;
        for(std::size_t i=0;i<L;++i){
            // Literal scale boundary: cvtss2sd -> mulsd 1000.0 -> cvtsd2ss.
            const float xv=static_cast<float>(static_cast<double>(model[p+i])*1000.0);
            const float yv=static_cast<float>(static_cast<double>(target[p+i])*1000.0);
            xframe[i]=xv; yframe[i]=yv;
            mx+=xv; my+=yv;
        }
        mx/=static_cast<float>(L); my/=static_cast<float>(L);
        std::vector<float> xf(kFft,0.0f),yf(kFft,0.0f);
        for(std::size_t i=0;i<L;++i){
            const std::size_t j=i&(kFft-1);
            const float x0=xframe[i]-mx;
            const float y0=yframe[i]-my;
            xf[j]+=x0*wv[i];
            yf[j]+=y0*wv[i];
        }
        std::array<float,kBins> xr{},xi{},yr{},yi{};
        rdft.run(xf,xr,xi); rdft.run(yf,yr,yi); ++frames;
        for(std::size_t k=0;k<kBins;++k){
            const float p0=xr[k]*xr[k],p1=xi[k]*xi[k]; sxx[k]+=p0+p1;
            const float p2=xr[k]*yr[k],p3=xi[k]*yi[k]; sxyRe[k]+=p2+p3;
            const float p4=xr[k]*yi[k],p5=xi[k]*yr[k]; sxyIm[k]+=p4-p5;
        }
        if(p==finalStart)break;
        startPos+=hop;
    }
    if(!frames)return std::vector<float>(kBins,1.0f);
    std::vector<float>r(kBins,1.0f);
    for(std::size_t k=0;k<kBins;++k){
        const float q0=sxyRe[k]*sxyRe[k],q1=sxyIm[k]*sxyIm[k];
        const float mag=preciseSqrtF(q0+q1);
        r[k]=mag/(sxx[k]+static_cast<float>(kEps));
    }
    return r;
}

std::vector<double> ratioSpectrum(const std::vector<float>& model,const std::vector<float>& target,double sr){const auto r=ratioSpectrumF(model,target,sr);return std::vector<double>(r.begin(),r.end());}

float hzToMelF(float hz){
    // 0x554f00 helpers: all scalar arithmetic is float up to the precise
    // double libm wrapper, whose result is rounded back to float.
    float x=hz/700.0f;
    x+=1.0f;
    const float l=preciseLog10F(x);
    return l*2595.0f;
}
float melToHzF(float mel){
    const float e=mel/2595.0f;
    const float p=precisePowF(10.0f,e);
    return (p-1.0f)*700.0f;
}
double hzToMel(double hz){return static_cast<double>(hzToMelF(static_cast<float>(hz)));}
double melToHz(double mel){return static_cast<double>(melToHzF(static_cast<float>(mel)));}

// 0x4225b0.  This is intentionally incremental rather than evaluating the
// closed-form expression for every point: every addss rounds before the next
// point, and the final endpoint is overwritten with b exactly.
std::vector<float> linspaceF(float a,float b,std::size_t n){
    std::vector<float> v(n);
    if(!n)return v;
    if(n==1){v[0]=b;return v;} // The official helper overwrites the sole slot at exit.
    v[0]=a;
    const float delta=(b-a)/static_cast<float>(n-1);
    for(std::size_t i=1;i<n;++i){
        const float prev=v[i-1];
        v[i]=prev+delta;
    }
    v[n-1]=b;
    return v;
}

// 0x553c60.  The EXE precomputes a slope/intercept for every source point
// (the final pair is copied from N-2), then scans the complete source-X array
// for every query and chooses the closest non-negative (q-x) distance.
// This is deliberately not std::lower_bound nor y0+(y1-y0)*t: the float
// operation order is observable inside the ten optimization iterations.
std::vector<float> interpolateOfficialF(const std::vector<float>&srcX,
                                        const std::vector<float>&srcY,
                                        const std::vector<float>&queryX){
    const std::size_t n=std::min(srcX.size(),srcY.size());
    std::vector<float> out(queryX.size(),std::numeric_limits<float>::max());
    if(n<2)return out;
    std::vector<float> slope(n),intercept(n);
    for(std::size_t i=0;i+1<n;++i){
        const float dy=srcY[i+1]-srcY[i];
        const float dx=srcX[i+1]-srcX[i];
        slope[i]=dy/dx;
        const float prod=slope[i]*srcX[i];
        intercept[i]=srcY[i]-prod;
    }
    slope[n-1]=slope[n-2];
    intercept[n-1]=intercept[n-2];
    for(std::size_t qn=0;qn<queryX.size();++qn){
        const float q=queryX[qn];
        float best=std::numeric_limits<float>::max();
        int bestIndex=-1;
        for(std::size_t i=0;i<n;++i){
            const float diff=q-srcX[i];
            if(diff>=0.0f && diff<best){best=diff;bestIndex=static_cast<int>(i);}
        }
        if(bestIndex>=0){
            const std::size_t i=static_cast<std::size_t>(bestIndex);
            const float prod=slope[i]*q;
            out[qn]=prod+intercept[i];
        }
    }
    return out;
}
float interpOfficialF(const std::vector<float>&srcX,const std::vector<float>&srcY,float q){
    const std::vector<float> queries{q};
    return interpolateOfficialF(srcX,srcY,queries)[0];
}

// Retained only for non-trainer utility code.  Trainer reconstruction below
// uses 0x553c60 exactly via interpolateOfficialF().
float interpLinearF(const std::vector<float>&x,const std::vector<float>&y,float q){
    if(x.empty()||y.empty())return 0.0f;
    if(q<=x.front())return y.front();if(q>=x.back())return y.back();
    const auto it=std::lower_bound(x.begin(),x.end(),q);
    const std::size_t b=static_cast<std::size_t>(it-x.begin()),a=b-1;
    const float dx=x[b]-x[a];if(std::abs(dx)<1e-30f)return y[a];
    const float t=(q-x[a])/dx;return y[a]+(y[b]-y[a])*t;
}
double interpLinear(const std::vector<double>&x,const std::vector<double>&y,double q){if(x.empty()||y.empty())return 0.0;if(q<=x.front())return y.front();if(q>=x.back())return y.back();const auto it=std::lower_bound(x.begin(),x.end(),q);const std::size_t b=static_cast<std::size_t>(it-x.begin()),a=b-1;const double dx=x[b]-x[a];if(std::abs(dx)<1e-30)return y[a];const double t=(q-x[a])/dx;return y[a]+(y[b]-y[a])*t;}

// 0x555460, reproduced literally.  Kernel construction and accumulation are
// double.  The raw kernel is normalized to a sum of 1,000,000; an even input
// kernel length allocates one additional zero coefficient.  Interior points
// are multiplied by 1e-6 after convolution, whereas edge points divide by
// the sum of only the coefficients that were actually used.
std::vector<double> gaussianKernelOfficialD(std::size_t kernelN){
    if(kernelN==0)return {};
    std::vector<double> raw(kernelN);
    const double step=5.0/static_cast<double>(kernelN);
    const double center=std::ceil(static_cast<double>(kernelN)*0.5);
    double rawSum=0.0;
    for(std::size_t i=0;i<kernelN;++i){
        const double x=(static_cast<double>(i+1)-center)*step;
        const double a=-0.5*x;
        const double exponent=a*x;
        raw[i]=std::exp(exponent);
        rawSum+=raw[i];
    }
    const double scale=1000000.0/rawSum;
    const std::size_t storage=(kernelN&1u)?kernelN:kernelN+1;
    std::vector<double> kernel(storage,0.0);
    for(std::size_t i=0;i<kernelN;++i)
        kernel[i]=raw[kernelN-1-i]*scale;
    return kernel;
}
std::vector<float> gaussianSmoothExactF(const std::vector<float>&v,std::size_t kernelN){
    if(v.empty())return {};
    if(kernelN==0)return v; // No production call in the reconstructed trainer reaches N=0.
    const auto kernel=gaussianKernelOfficialD(kernelN);
    const int storage=static_cast<int>(kernel.size());
    const int center=static_cast<int>(std::ceil(static_cast<double>(storage)*0.5));
    const int left=center-1;
    const int right=storage-center;
    std::vector<float> out(v.size());
    for(int b=0;b<static_cast<int>(v.size());++b){
        const bool interior=(b>=left) && (b+right<static_cast<int>(v.size()));
        double sum=0.0;
        if(interior){
            for(int off=-left;off<=right;++off){
                const float sample=v[static_cast<std::size_t>(b+off)];
                const double weight=kernel[static_cast<std::size_t>(left+off)];
                sum+=static_cast<double>(sample)*weight;
            }
            sum*=1.0e-6;
        }else{
            double usedWeight=0.0;
            const int lo=std::max(-left,-b);
            const int hi=std::min(right,static_cast<int>(v.size())-b-1);
            for(int off=lo;off<=hi;++off){
                const double weight=kernel[static_cast<std::size_t>(left+off)];
                usedWeight+=weight;
                sum+=static_cast<double>(v[static_cast<std::size_t>(b+off)])*weight;
            }
            sum/=usedWeight;
        }
        out[static_cast<std::size_t>(b)]=static_cast<float>(sum);
    }
    return out;
}
std::vector<double> gaussianSmoothExact(const std::vector<double>&v,std::size_t n){
    std::vector<float>x(v.size());for(std::size_t i=0;i<v.size();++i)x[i]=static_cast<float>(v[i]);
    const auto y=gaussianSmoothExactF(x,n);return std::vector<double>(y.begin(),y.end());
}

struct ConditionedMagnitude{std::vector<double>freq,mag;};
struct ConditionedMagnitudeF{std::vector<float>freq,mag;};
// 0x554f00, literal data flow and helper sequence.
ConditionedMagnitudeF conditionMagnitudeF(const std::vector<float>&srcFreq,const std::vector<float>&srcMag,std::size_t destCount){
    const std::size_t n=std::min(srcFreq.size(),srcMag.size());
    ConditionedMagnitudeF out;if(!n||!destCount)return out;
    const float f0=srcFreq[0],f1=srcFreq[n-1];
    std::vector<float> sf(srcFreq.begin(),srcFreq.begin()+static_cast<std::ptrdiff_t>(n));

    // Source and destination Mel grids are made by 0x4225b0, then converted
    // back to Hz. Endpoints of the destination-Mel Hz grid are overwritten by
    // the exact source endpoints in the EXE.
    const float mel0=hzToMelF(f0),mel1=hzToMelF(f1);
    const auto srcMel=linspaceF(mel0,mel1,n);
    std::vector<float> srcMelHz(n);
    for(std::size_t i=0;i<n;++i)srcMelHz[i]=melToHzF(srcMel[i]);
    srcMelHz.front()=f0;srcMelHz.back()=f1;

    const auto dstMel=linspaceF(mel0,mel1,destCount);
    std::vector<float> dstMelHz(destCount);
    for(std::size_t i=0;i<destCount;++i)dstMelHz[i]=melToHzF(dstMel[i]);
    dstMelHz.front()=f0;dstMelHz.back()=f1;

    out.freq=linspaceF(f0,f1,destCount);

    std::vector<float> db(n);
    for(std::size_t i=0;i<n;++i){
        const float l=preciseLog10F(srcMag[i]);
        db[i]=20.0f*l;
    }

    const std::size_t k1=static_cast<std::size_t>(static_cast<int>(static_cast<double>(n)*0.002));
    db=gaussianSmoothExactF(db,k1);
    auto melDb1=interpolateOfficialF(sf,db,srcMelHz);

    const std::size_t k2=static_cast<std::size_t>(2*static_cast<int>(n/destCount));
    melDb1=gaussianSmoothExactF(melDb1,k2);

    std::vector<float> melDb2;
    if(n==destCount)melDb2=melDb1;
    else melDb2=interpolateOfficialF(srcMelHz,melDb1,dstMelHz);

    const auto linearDb=interpolateOfficialF(dstMelHz,melDb2,out.freq);
    out.mag.resize(destCount);
    for(std::size_t i=0;i<destCount;++i){
        const float exponent=linearDb[i]*0.05f;
        out.mag[i]=precisePowF(10.0f,exponent);
    }
    return out;
}
ConditionedMagnitude conditionMagnitude(const std::vector<double>&srcFreq,const std::vector<double>&srcMag,std::size_t destCount){const std::size_t n=std::min(srcFreq.size(),srcMag.size());std::vector<float>f(n),m(n);for(std::size_t i=0;i<n;++i){f[i]=static_cast<float>(srcFreq[i]);m[i]=static_cast<float>(srcMag[i]);}auto cf=conditionMagnitudeF(f,m,destCount);ConditionedMagnitude o;o.freq.assign(cf.freq.begin(),cf.freq.end());o.mag.assign(cf.mag.begin(),cf.mag.end());return o;}

void lowSmoothASequentialF(std::vector<float>&m,double sr){
    if(m.size()<2)return;
    const std::size_t n=m.size();const float fs=static_cast<float>(sr);
    const float invNy=2.0f/fs;
    const float raw=(invNy*60.0f)*static_cast<float>(n);
    // 0x4223c0 is the imported CRT floor() wrapper (IAT 0x76f750), followed
    // by cvttss2si in 0x553f10/0x557842.
    const int limI=static_cast<int>(std::floor(static_cast<double>(raw)));
    if(limI<=1)return;
    const std::size_t lim=std::min<std::size_t>(n-1,static_cast<std::size_t>(limI));
    m[0]=preciseSqrtF(m[0]*preciseSqrtF(m[0]*m[1]));
    for(std::size_t i=1;i<lim&&i+1<n;++i)
        m[i]=preciseSqrtF(m[i]*preciseSqrtF(m[i-1]*m[i+1]));
}
void lowSmoothASequential(std::vector<double>&m,double sr){std::vector<float>x(m.begin(),m.end());lowSmoothASequentialF(x,sr);m.assign(x.begin(),x.end());}

struct TrigTableF { std::vector<float> c,s; };
TrigTableF makeDirectTrigOfficial(std::size_t n){
    TrigTableF t;t.c.resize(n);t.s.resize(n);if(!n)return t;
    // 0x553419..0x553575: delta is calculated in float.  Only the first
    // trunc(float(N)*0.5f) entries call the precise libm sine/cosine helpers.
    // 0x553583..0x5536a8 fills the second half by XORing the IEEE-754 sign
    // bit of the corresponding first-half values (theta + pi), preserving
    // signed zero exactly.
    const float delta=6.2831854820251465f/static_cast<float>(n);
    const std::size_t half=static_cast<std::size_t>(static_cast<int>(static_cast<float>(n)*0.5f));
    for(std::size_t i=0;i<half;++i){
        const float phase=static_cast<float>(i)*delta;
        t.s[i]=preciseSinF(phase);
        t.c[i]=preciseCosF(phase);
    }
    for(std::size_t i=half;i<n;++i){
        const std::size_t j=i-half;
        t.s[i]=flipFloatSignBit(t.s[j]);
        t.c[i]=flipFloatSignBit(t.c[j]);
    }
    return t;
}

std::vector<ComplexF> directDftOfficial(const std::vector<ComplexF>& in,bool inverse,const TrigTableF& trig){
    const std::size_t n=in.size();std::vector<ComplexF> out(n);if(!n)return out;
    for(std::size_t k=0;k<n;++k){
        float sr=0.0f,si=0.0f;
        for(std::size_t j=0;j<n;++j){
            const std::size_t q=(j*k)%n;
            const float c=trig.c[q];const float ss=inverse?trig.s[q]:-trig.s[q];
            const float p0=in[j].re*c,p1=in[j].im*ss;sr+=p0-p1;
            const float p2=in[j].re*ss,p3=in[j].im*c;si+=p2+p3;
        }
        if(inverse){const float invN=1.0f/static_cast<float>(n);sr*=invN;si*=invN;}
        out[k]={sr,si};
    }
    return out;
}

// 0x5548f0: analytic/causal cepstrum construction.  The EXE combines each
// positive quefrency with its mirrored complex sample instead of replacing it
// by a generic '*2' shortcut; even-N Nyquist is handled separately.
std::vector<ComplexF> lifterCepstrumOfficial(const std::vector<ComplexF>& in){
    const std::size_t n=in.size();std::vector<ComplexF> out(n);if(!n)return out;
    out[0]=in[0];
    if(n&1u){
        const std::size_t stop=(n+1u)/2u;
        for(std::size_t i=1;i<stop;++i){out[i].re=in[i].re+in[n-i].re;out[i].im=in[i].im-in[n-i].im;}
        for(std::size_t i=stop;i<n;++i)out[i]={0.0f,0.0f};
    }else{
        const std::size_t mid=n/2u;
        for(std::size_t i=1;i<mid;++i){out[i].re=in[i].re+in[n-i].re;out[i].im=in[i].im-in[n-i].im;}
        out[mid].re=in[mid].re;out[mid].im=flipFloatSignBit(in[mid].im);
        for(std::size_t i=mid+1;i<n;++i)out[i]={0.0f,0.0f};
    }
    return out;
}

// 0x554420 -> 0x554af0 -> 0x553400 -> 0x5548f0.  The official path uses
// direct O(N^2) float DFTs for minimum-phase reconstruction even when N is a
// power of two; no radix-2 shortcut is used here.
std::vector<float> minimumPhaseF(const std::vector<float>&positive,std::size_t taps){
    if(positive.size()<2||!taps)return std::vector<float>(taps,0.0f);
    const std::size_t posN=positive.size(),fullN=2*posN-2;
    std::vector<float> m(fullN,0.0f);
    for(std::size_t i=0;i<posN;++i)m[i]=positive[i];
    for(std::size_t i=1;i+1<posN;++i)m[fullN-i]=m[i];

    // 0x554b24..0x554bb0: maximum is computed from abs(input).  The floor
    // is maxAbs * pow(10.0,-5.0), with the pow/multiply in double and one
    // final conversion to float.  Values whose ABS is below the floor are
    // replaced by +floor; otherwise the ORIGINAL signed value is preserved.
    float mx=0.0f;for(float v:m)mx=std::max(mx,std::fabs(v));
    const float floor=static_cast<float>(static_cast<double>(mx)*std::pow(10.0,-5.0));
    std::vector<float> logMag(fullN);
    for(std::size_t i=0;i<fullN;++i){
        float v=m[i];
        if(std::fabs(v)<floor)v=floor;
        logMag[i]=preciseLogF(v+std::numeric_limits<float>::epsilon());
    }

    // HTUSBTools.dll 0x180097a65: when inputCount == outputCount (the
    // normal A/B trainer path), execution jumps directly to 0x180097b64 and
    // copies the log-magnitude buffer.  The half-buffer reversal at
    // 0x180097ae7..0x180097b62 belongs only to the unequal-length/interpolated
    // branch and must NOT run for A/B reconstruction.

    std::vector<ComplexF> logSpec(fullN);
    for(std::size_t i=0;i<fullN;++i)logSpec[i]={logMag[i],0.0f};
    const auto trig=makeDirectTrigOfficial(fullN);
    auto cep=directDftOfficial(logSpec,true,trig);
    auto causal=lifterCepstrumOfficial(cep);
    auto clog=directDftOfficial(causal,false,trig);
    std::vector<ComplexF> minSpec(fullN);
    for(std::size_t i=0;i<fullN;++i){const float amp=preciseExpF(clog[i].re);minSpec[i]={amp*preciseCosF(clog[i].im),amp*preciseSinF(clog[i].im)};}
    auto impulse=directDftOfficial(minSpec,true,trig);

    float fullNorm2=0.0f;for(const auto&v:impulse)fullNorm2+=v.re*v.re;
    std::vector<float>h(taps,0.0f);for(std::size_t i=0;i<std::min(taps,impulse.size());++i)h[i]=impulse[i].re;
    float sum=0.0f;for(float v:h)sum+=v;const float mean=sum/static_cast<float>(h.size());for(auto&v:h)v-=mean;
    float shortNorm2=0.0f;for(float v:h)shortNorm2+=v*v;
    if(shortNorm2>1.0e-30f&&fullNorm2>0.0f){const float g=preciseSqrtF(fullNorm2)/preciseSqrtF(shortNorm2);for(auto&v:h)v*=g;}
    return h;
}

std::vector<float> minimumPhase(const std::vector<double>&positive,std::size_t taps){std::vector<float>x(positive.size());for(std::size_t i=0;i<positive.size();++i)x[i]=static_cast<float>(positive[i]);return minimumPhaseF(x,taps);}


// Main trainer setup (0x55a261..0x55a311) initializes the exponent
// vector to 1.0f, then overwrites it from floor((2/Fs)*80*N)-1 to the end
// with a float linspace from 1.0f to 0.5f.  This vector is multiplied by
// `step` at 0x5574e0 before pow().
std::vector<float> frequencyWeightsF(double sr){
    std::vector<float>w(kBins,1.0f);
    const float fs=static_cast<float>(sr);
    const float invNy=2.0f/fs;
    const float raw=(invNy*80.0f)*static_cast<float>(kBins);
    // 0x4223c0 == floor(), then the result is used as a one-based boundary.
    const int boundary=static_cast<int>(std::floor(static_cast<double>(raw)));
    const std::size_t start=boundary>0?std::min<std::size_t>(kBins-1,static_cast<std::size_t>(boundary-1)):0;
    const std::size_t count=kBins-start;
    const auto tail=linspaceF(1.0f,0.5f,count); // literal 0x4225b0
    std::copy(tail.begin(),tail.end(),w.begin()+static_cast<std::ptrdiff_t>(start));
    return w;
}

float lossFromRatioF(const std::vector<float>&r,double sr){
    const auto srcF=fftFrequencyGridF(sr);
    // Constructor 0x5596d3..0x559790: 512-point incremental Mel linspace,
    // interior Mel->Hz conversion, exact endpoint overwrites 80 and 10000.
    const float mel80=preciseLog10F(1.0f+80.0f/700.0f)*2595.0f;
    const float mel10k=preciseLog10F(1.0f+10000.0f/700.0f)*2595.0f;
    const auto melGrid=linspaceF(mel80,mel10k,512);
    std::vector<float> query(512);
    query[0]=80.0f;
    for(std::size_t i=1;i+1<query.size();++i)query[i]=melToHzF(melGrid[i]);
    query.back()=10000.0f;
    const auto values=interpolateOfficialF(srcF,r,query); // 0x553c60
    float sum=0.0f;
    for(float v:values){
        const float withEps=v+static_cast<float>(kEps);
        const float l=preciseLogF(withEps);
        sum+=std::fabs(l);
    }
    return sum*0.001953125f; // exact 1/512 constant at 0x21fe604
}

double lossFromRatio(const std::vector<double>&r,double sr){std::vector<float>x(r.begin(),r.end());return static_cast<double>(lossFromRatioF(x,sr));}

void regularizeInitialCurveF(std::vector<float>&v,const std::vector<float>&reference){
    if(v.empty()||reference.empty())return;
    // 0x5585a5..0x5586xx: maxss reduction starts at +0.0f; the 0.001
    // multiply is performed in double and rounded once to float.
    float mx=0.0f;
    for(float x:reference)mx=std::max(mx,x);
    const float f=static_cast<float>(static_cast<double>(mx)*0.001);
    const float twice=f+f;
    const float twiceSq=twice*twice;
    const float numerator=twice-f; // == f, but reproduces the EXE operation order.
    const float inv4f=numerator/twiceSq;
    for(auto&x:v){
        if(x<twice){
            const float xx=x*x;
            const float q=xx*inv4f;
            x=q+f;
        }
    }
}
void regularizeInitialCurve(std::vector<double>&v,const std::vector<double>&reference){std::vector<float>x(v.begin(),v.end()),r(reference.begin(),reference.end());regularizeInitialCurveF(x,r);v.assign(x.begin(),x.end());}

struct FactorState{std::vector<float>a,b;};
FactorState initialFactorState(const Model&m,const std::vector<float>&input,const std::vector<float>&target,double sr){
    const std::size_t lb=officialTimeIndex(sr,6.0f),le=officialTimeIndex(sr,21.0f);
    const std::size_t sb=officialTimeIndex(sr,23.0f),se=officialTimeIndex(sr,28.0f);
    auto lowIn=sliceSignal(input,lb,le),lowTarget=sliceSignal(target,lb,le);std::vector<float>lowPred;renderCoreNoFir(m,lowIn,lowPred);auto lowSpec=ratioSpectrumF(lowPred,lowTarget,sr);
    auto sweepIn=sliceSignal(input,sb,se),sweepTarget=sliceSignal(target,sb,se);sweepIn=applyInitialConditioningFir(sweepIn,sr);std::vector<float>sweepPred;renderCoreNoFir(m,sweepIn,sweepPred);auto sweepSpec=ratioSpectrumF(sweepPred,sweepTarget,sr);
    const std::size_t n=sweepSpec.size();
    const std::size_t k001=static_cast<std::size_t>(static_cast<int>(static_cast<double>(n)*0.001));
    const std::size_t k005=static_cast<std::size_t>(static_cast<int>(static_cast<double>(n)*0.005));
    sweepSpec=gaussianSmoothExactF(sweepSpec,k001);
    sweepSpec=gaussianSmoothExactF(sweepSpec,k005);
    regularizeInitialCurveF(sweepSpec,lowSpec);
    FactorState st;st.a.resize(kBins,1.0f);st.b=sweepSpec;for(std::size_t k=0;k<kBins;++k){const double num=static_cast<double>(lowSpec[k])*1000000.0;const double den=static_cast<double>(sweepSpec[k])*1000000.0+kEps;st.a[k]=static_cast<float>(num/den);}return st;
}

struct Phase{double t0,t1;int iterations;const wchar_t*name;};
void optimizePhase(Model&m,FactorState&state,const std::vector<float>&input,const std::vector<float>&target,double sr,const Phase&ph,int&globalIter,const StatusCallback&status){
    const auto freq=fftFrequencyGridF(sr),weights=frequencyWeightsF(sr);
    const std::size_t b=officialTimeIndex(sr,static_cast<float>(ph.t0)),e=officialTimeIndex(sr,static_cast<float>(ph.t1));
    const auto phaseIn=sliceSignal(input,b,e),phaseTarget=sliceSignal(target,b,e);

    // HTUSBTools.dll 0x18009b890.  S0/corr starts at all-ones for each phase.
    // state.a is the persistent A magnitude state and state.b is the persistent
    // accumulated B factor (initialized by 0x18009cd30 to sweepSpec).
    float step=1.0f,bestLoss=100.0f;
    Model bestM=m;
    FactorState bestState=state;
    std::vector<float>corr(kBins,1.0f),bestCorr=corr;

    for(int it=0;it<ph.iterations;++it){
        ++globalIter;
        report(status,L"Independent: A/B fit "+std::to_wstring(globalIter)+L"/10 ("+ph.name+L")...");

        // 0x18009bae0..0x18009bb1c: S0 = clamp(powf(S0,weight*step),0.2,5).
        std::vector<float>stepped(kBins);
        for(std::size_t k=0;k<kBins;++k){
            const float exponent=weights[k]*step;
            stepped[k]=std::clamp(precisePowF(corr[k],exponent),0.2f,5.0f);
        }
        // 0x18009bb3e: decay before conditioning; rollback may halve this
        // already-decayed value later.
        step*=0.8999999761581421f;

        // 0x18009bb59 / 0x18009bb78: two consecutive conditionMagnitude passes.
        const auto conditioned1=conditionMagnitudeF(freq,stepped,kBins);
        const auto conditioned2=conditionMagnitudeF(conditioned1.freq,conditioned1.mag,kBins);
        const auto& conditioned=conditioned2.mag;

        // 0x18009bbe0..0x18009be50:
        //   accumulated Bfactor *= conditioned
        //   Astate              /= conditioned
        FactorState trial=state;
        for(std::size_t k=0;k<kBins;++k){
            trial.b[k]*=conditioned[k];
            trial.a[k]/=conditioned[k];
        }
        lowSmoothASequentialF(trial.a,sr);

        // A state -> condition to A length -> minimum phase -> physical FIR A.
        Model candidate=m;
        const auto amag=conditionMagnitudeF(freq,trial.a,kA);
        candidate.A=minimumPhaseF(amag.mag,kA);

        // Render A only and estimate the still-required transfer (fresh/S5).
        std::vector<float>pre;
        renderModel(candidate,phaseIn,pre,false);
        const auto fresh=ratioSpectrumF(pre,phaseTarget,sr);

        // 0x18009c773..0x18009c996: S2 = fresh / accumulated Bfactor.
        std::vector<float>rb(kBins);
        for(std::size_t k=0;k<kBins;++k){
            const double num=static_cast<double>(fresh[k])*1000000.0;
            const double den=static_cast<double>(trial.b[k])*1000000.0+kEps;
            rb[k]=static_cast<float>(num/den);
        }

        // 0x18009c9a5..0x18009c9bd: conditioned(fresh/Bfactor) becomes S0,
        // i.e. the correction curve carried into the NEXT iteration.
        const auto nextConditioned=conditionMagnitudeF(freq,rb,kBins);
        const std::vector<float>nextCorr=nextConditioned.mag;

        // Critical distinction closed from the complete register trace:
        // 0x18009c9c2..0x18009c9dc passes rdi=S5 (the RAW fresh estimator)
        // directly to minimumPhase.  The conditioned rb/S0 is NOT the current
        // B FIR; it is only the next iteration's correction curve.
        candidate.B=minimumPhaseF(fresh,kB);

        // Full A+nonlinearity+B render.  The second estimator is used only for
        // the loss calculation; its residual is not fed back as S0.
        std::vector<float>final;
        renderModel(candidate,phaseIn,final,true);
        const auto residual=ratioSpectrumF(final,phaseTarget,sr);
        const float loss=lossFromRatioF(residual,sr);

        if(loss<bestLoss){
            // 0x18009caef..0x18009cb74 snapshots S0, FIR A/B, Astate and
            // Bfactor.  Since the live DLL buffers have already been updated,
            // keep the corresponding candidate/trial snapshots here.
            bestLoss=loss;
            bestM=candidate;
            bestState=trial;
            bestCorr=nextCorr;
            m=candidate;
            state=trial;
            corr=nextCorr;
        }else if(loss>1.2f*bestLoss){
            // 0x18009cb7e..0x18009cc24: restore best S0/FIR A/FIR B/Astate/
            // Bfactor and halve the already-decayed step.
            m=bestM;
            state=bestState;
            corr=bestCorr;
            step*=0.5f;
        }else{
            // Worse but within 1.2x: retain the live state and continue with
            // the newly conditioned fresh/Bfactor correction.
            m=candidate;
            state=trial;
            corr=nextCorr;
        }
    }

    // 0x18009cca1..0x18009ccff: every phase exits on its best snapshot.
    m=bestM;
    state=bestState;
}

void fitAB(Model&m,const std::vector<float>&input,const std::vector<float>&target,double sr,const StatusCallback&status){
    report(status,L"Independent: initial low-level / conditioned-sweep factorization...");FactorState state=initialFactorState(m,input,target,sr);int globalIter=0;
    const Phase phases[]={{23,28,3,L"sweep"},{6,21,2,L"low-level"},{30,50,5,L"multi-level"}};for(const auto&ph:phases)optimizePhase(m,state,input,target,sr,ph,globalIter,status);
}

std::vector<float> convolveTruncate(const std::vector<float>&a,const std::vector<float>&b,std::size_t n){std::vector<float>o(n,0.0f);for(std::size_t i=0;i<a.size();++i)for(std::size_t j=0;j<b.size()&&i+j<n;++j)o[i+j]+=a[i]*b[j];return o;}

std::vector<float> finalTailCorrection(const std::vector<float>&model,const std::vector<float>&target,double sr){
    const float fs0=static_cast<float>(sr);
    // 0x5566xx: cvtss2sd(float(Fs)) * 0.1(double), ceil(), then int.
    const std::size_t L=std::max<std::size_t>(1,static_cast<std::size_t>(std::ceil(static_cast<double>(fs0)*0.1)));const std::size_t end=std::min(model.size(),target.size());if(end<L)return std::vector<float>(256,1.0f);
    std::vector<float>xm(L,0.0f),yt(L,0.0f);
    // 0x553f90: fold only complete L-sample blocks. Any remainder is ignored.
    const std::size_t blocks=end/L;
    for(std::size_t b=0;b<blocks;++b)for(std::size_t i=0;i<L;++i){const std::size_t n=b*L+i;xm[i]+=model[n];yt[i]+=target[n];}
    const float mm=sumFloatFinalBOfficial(xm)/static_cast<float>(L);
    const float tm=sumFloatFinalBOfficial(yt)/static_cast<float>(L);
    const auto win=hammingFinalBOfficial(L);
    for(std::size_t i=0;i<L;++i){xm[i]=(xm[i]-mm)*win[i];yt[i]=(yt[i]-tm)*win[i];}
    const std::size_t posN=L/2+1;std::vector<float>freq(posN),modelMag(posN),targetMag(posN);
    // GP-200.exe final-B path builds the complete length-L DFT for each
    // folded signal with 0x553400's float trig table and float accumulators.
    // Only after both complete DFTs exist are bins 0..L/2 converted to
    // magnitudes.  Keep that evaluation order exactly.
    const auto trig=makeDirectTrigOfficial(L);
    std::vector<ComplexF> xmC(L),ytC(L);
    for(std::size_t i=0;i<L;++i){xmC[i]={xm[i],0.0f};ytC[i]={yt[i],0.0f};}
    const auto modelDft=directDftOfficial(xmC,false,trig);
    const auto targetDft=directDftOfficial(ytC,false,trig);
    const float fs=static_cast<float>(sr),Lf=static_cast<float>(L);
    for(std::size_t k=0;k<posN;++k){
        const float mr=modelDft[k].re,mi=modelDft[k].im;
        const float tr=targetDft[k].re,ti=targetDft[k].im;
        modelMag[k]=preciseSqrtF(mr*mr+mi*mi);
        targetMag[k]=preciseSqrtF(tr*tr+ti*ti);
    }
    // 0x5571cd: fresh 0..Nyquist frequency grid is produced by 0x4225b0
    // (incremental float linspace), not by k*Fs/L.
    freq=linspaceF(0.0f,fs*0.5f,posN);

    // Critical ordering from 0x556670: condition target magnitude and model
    // magnitude independently with 0x554f00 BEFORE computing their ratio.
    const auto ct=conditionMagnitudeF(freq,targetMag,posN);
    const auto cm=conditionMagnitudeF(freq,modelMag,posN);
    std::vector<float>ratio(posN,1.0f);for(std::size_t k=0;k<posN;++k){const double num=static_cast<double>(ct.mag[k])*1000000.0;const double den=static_cast<double>(cm.mag[k])*1000000.0+kEps;ratio[k]=std::clamp(static_cast<float>(num/den),0.1f,10.0f);}
    const std::size_t smoothN=std::max<std::size_t>(1,static_cast<std::size_t>(static_cast<int>(static_cast<double>(posN)*0.1)));
    ratio=gaussianSmoothExactF(ratio,smoothN);for(auto&v:ratio)v=std::clamp(v,0.1f,10.0f);
    const auto final=conditionMagnitudeF(freq,ratio,256);return final.mag;
}

void refineB(Model&m,const std::vector<float>&input,const std::vector<float>&target,double sr,const StatusCallback&status){
    report(status,L"Independent: final Block B tail refinement...");
    const std::size_t b=officialTimeIndex(sr,50.0f),e=officialTimeIndex(sr,70.0f);
    const auto tailIn=sliceSignal(input,b,e),tailTarget=sliceSignal(target,b,e);

    // 0x18009a673..0x18009a9ff: compute PRE -> A -> NL4x -> POST once and
    // preserve that pre-B signal.  After B is corrected, the DLL reruns only
    // FIR B over this same preserved signal for the energy normalization.
    std::vector<float>preB;
    renderModel(m,tailIn,preB,false);
    std::vector<float>pred;
    {FirPlan bp(m.B);bp.run(preB,pred);}

    auto corrMag=finalTailCorrection(pred,tailTarget,sr);
    auto corr=minimumPhaseF(corrMag,256);
    m.B=convolveTruncate(m.B,corr,kB);

    const float mean=sumFloatFinalBOfficial(m.B)/static_cast<float>(m.B.size());
    for(auto&v:m.B)v-=mean;

    {FirPlan bp(m.B);bp.run(preB,pred);}
    float et=0.0f,ep=0.0f;
    energiesFinalBOfficial(tailTarget,pred,et,ep);
    if(ep>1.0e-30f){
        const float g=preciseSqrtF(et)/preciseSqrtF(ep);
        for(auto&v:m.B)v*=g;
    }
}

// Exact FIR serialization SRC reconstructed from GP-200.exe 0x5a70a0.
// This path is intentionally separate from the long-stimulus SRC.  The
// official converter converts the whole FIR float vector to double, constructs
// CDSPResampler24 with MaxInLen == FIR input length, calls process() once with
// the whole FIR, then repeatedly feeds an equally long all-zero double block
// until the truncated target count is available.
std::vector<float> resampleFirOfficial(const std::vector<float>& h,double sr,std::size_t outLen){
    if(h.empty())return std::vector<float>(outLen,0.0f);
    if(std::abs(sr-44100.0)<1e-9){auto r=h;r.resize(outLen,0.0f);return r;}
    if(h.size()>static_cast<std::size_t>(std::numeric_limits<int>::max()))return std::vector<float>(outLen,0.0f);

    // 0x5a728c..0x5a72a3: conversion uses float arithmetic and truncation.
    const float inCountF=static_cast<float>(static_cast<int>(h.size()));
    const float srcF=static_cast<float>(sr), dstF=44100.0f;
    const int targetCount=std::max(0,static_cast<int>(inCountF*dstF/srcF));
    const std::size_t wanted=std::min<std::size_t>(outLen,static_cast<std::size_t>(targetCount));
    std::vector<float> out(outLen,0.0f);
    if(wanted==0)return out;

    const int blockLen=static_cast<int>(h.size());
    std::vector<double> block(h.size());
    for(std::size_t i=0;i<h.size();++i)block[i]=static_cast<double>(h[i]);
    r8b::CDSPResampler24 rs(static_cast<double>(srcF),static_cast<double>(dstF),blockLen,2.0);

    std::size_t previous=0;
    bool first=true;
    while(previous<wanted){
        if(!first)std::fill(block.begin(),block.end(),0.0);
        first=false;
        double* produced=nullptr;
        const int count=rs.process(block.data(),blockLen,produced);
        if(count<0||produced==nullptr)break;
        const std::size_t current=static_cast<std::size_t>(count);
        if(current>previous){
            const std::size_t take=std::min<std::size_t>(current-previous,wanted-previous);
            for(std::size_t i=0;i<take;++i)out[previous+i]=static_cast<float>(produced[i]);
        }
        previous=current;
    }
    rs.clear();
    return out;
}
bool serialize2048(const fs::path&path,const Model&m,double trainerRate,std::string&error){std::vector<std::uint8_t>d(kCloBytes,0);std::memcpy(d.data(),"VTSI",4);put32(d,0x04,0x2288);put32(d,0x14,0x2200);putDouble(d,0x18,m.pre.b0);putDouble(d,0x20,m.pre.b1);putDouble(d,0x28,m.pre.b2);putDouble(d,0x30,m.pre.a1);putDouble(d,0x38,m.pre.a2);
    putDouble(d,0x40,m.post.b0);putDouble(d,0x48,m.post.b1);putDouble(d,0x50,m.post.b2);putDouble(d,0x58,m.post.a1);putDouble(d,0x60,m.post.a2);putFloat(d,0x68,m.pk.pp);putFloat(d,0x6c,m.pk.pn);putFloat(d,0x70,m.pk.kp);putFloat(d,0x74,m.pk.kn);put32(d,0x78,0);put32(d,0x7c,128);put32(d,0x80,128);put32(d,0x84,2048);
    auto A44=resampleFirOfficial(m.A,trainerRate,128);auto Bscaled=m.B;for(auto&v:Bscaled)v*=4.0f;auto B44=resampleFirOfficial(Bscaled,trainerRate,2048);for(std::size_t i=0;i<A44.size();++i)putFloat(d,0x88+4*i,A44[i]);for(std::size_t i=0;i<B44.size();++i)putFloat(d,0x88+4*(128+i),B44[i]);const auto crc=crc16Official(d.data()+0x0c,d.size()-0x0c);d[8]=static_cast<std::uint8_t>(crc);d[9]=static_cast<std::uint8_t>(crc>>8);return writeFileBytes(path,d.data(),d.size(),error);}


fs::path resolveOriginalStimulusPath(){
    const fs::path exe=executablePath();
    return exe.empty()?fs::path{}:exe.parent_path()/L"nam_input_wav.wav";
}

bool writeMonoFloat32Wav(const fs::path&path,const std::vector<float>&samples,std::uint32_t sampleRate,std::string&error){
    if(sampleRate==0){error="Cannot write WAV with sample rate 0.";return false;}
    std::error_code ec;if(path.has_parent_path())fs::create_directories(path.parent_path(),ec);if(ec){error="Cannot create WAV directory: "+ec.message();return false;}
    std::ofstream o(path,std::ios::binary|std::ios::trunc);if(!o){error="Cannot create WAV: "+pathToUtf8(path);return false;}
    const std::uint16_t channels=1,bits=32,align=4;const std::uint32_t byteRate=sampleRate*align;
    if(samples.size()>((std::numeric_limits<std::uint32_t>::max()-36u)/4u)){error="WAV is too large.";return false;}
    const std::uint32_t dataBytes=static_cast<std::uint32_t>(samples.size()*4u),riffSize=36u+dataBytes;
    auto w16=[&](std::uint16_t v){char b[2]={static_cast<char>(v&255u),static_cast<char>((v>>8)&255u)};o.write(b,2);};
    auto w32=[&](std::uint32_t v){char b[4]={static_cast<char>(v&255u),static_cast<char>((v>>8)&255u),static_cast<char>((v>>16)&255u),static_cast<char>((v>>24)&255u)};o.write(b,4);};
    o.write("RIFF",4);w32(riffSize);o.write("WAVEfmt ",8);w32(16);w16(3);w16(channels);w32(sampleRate);w32(byteRate);w16(align);w16(bits);o.write("data",4);w32(dataBytes);
    for(float x:samples){if(!std::isfinite(x))x=0.0f;std::uint32_t u=0;std::memcpy(&u,&x,sizeof(u));w32(u);}
    if(!o){error="Failed while writing WAV: "+pathToUtf8(path);return false;}return true;
}


std::vector<float> prepareToneTarget44100(const std::vector<float>&renderedWithGuard,double sourceRate){
    const std::size_t n70=static_cast<std::size_t>(static_cast<float>(sourceRate)*70.0f);
    std::vector<float> source70(n70,0.0f);
    std::copy_n(renderedWithGuard.begin(),std::min(n70,renderedWithGuard.size()),source70.begin());
    auto out=resampleR8Brain24(source70,sourceRate,44100.0);
    out.resize(70u*44100u,0.0f);
    out.resize(70u*44100u+600u,0.0f);
    return out;
}

fs::path uniqueOutput(const fs::path&dir,const std::wstring&stem,const wchar_t*suffix){fs::path p=dir/(stem+suffix);int i=2;while(fs::exists(p))p=dir/(stem+L"_"+std::to_wstring(i++)+suffix);return p;}

} // namespace

ConversionResult convertNamToClo(const fs::path& inputNam,const fs::path& outputDirectory,
                                             StimulusConfig stimulus,CorrectiveIrConfig correction,CloRefineConfig refine,
                                             NativeConverterConfig trainer,const StatusCallback& status){
    ConversionResult r;r.inputNam=inputNam;std::string error;std::error_code ec;
    if(!fs::exists(inputNam,ec)||ec){r.error="Input NAM does not exist.";return r;}
    fs::create_directories(outputDirectory,ec);if(ec){r.error="Cannot create output directory: "+ec.message();return r;}

    const fs::path originalStimulus=resolveOriginalStimulusPath();
    if(originalStimulus.empty()||!fs::exists(originalStimulus,ec)||ec){
        r.error="Missing nam_input_wav.wav next to the executable.";return r;
    }

    const fs::path work=outputDirectory/(L".native_work_"+inputNam.stem().wstring());fs::remove_all(work,ec);fs::create_directories(work,ec);if(ec){r.error="Cannot create conversion work directory.";return r;}
    const fs::path stim=work/L"stimulus_70s.wav";report(status,L"Building original stimulus + selected Tail/Reamp...");if(!buildStimulus(originalStimulus,stimulus,stim,error)){r.error=error;fs::remove_all(work,ec);return r;}
    std::vector<float>s44;std::uint32_t ssr=0;if(!readPcm16Mono(stim,s44,ssr,error)){r.error=error;fs::remove_all(work,ec);return r;}
    fs::path modelPath;if(!prepareFullA2(inputNam,work,modelPath,error)){r.error=error;fs::remove_all(work,ec);return r;}

    std::vector<float>input,target;double sr=48000;if(!renderNam(modelPath,s44,trainer.blockSize,0.31f,input,target,sr,error,status)){r.error=error;fs::remove_all(work,ec);return r;}
    // Keep the raw NAM render for Tone Match before trainer detrend/latency alignment.
    const std::vector<float> toneTarget44100=prepareToneTarget44100(target,sr);
    detrend(target);const auto latency=detectLatency(target,sr);target=alignLeft(target,latency);report(status,L"Detected NAM latency "+std::to_wstring(latency)+L" samples.");
    Model m;m.A.assign(kA,0);m.A[0]=1;m.B.assign(kB,0);m.B[0]=1;m.pk=fitPk(input,target,sr);m.pre=Biquad{};m.post=postForRate(sr);{std::wostringstream os;os<<L"P/K = "<<m.pk.pp<<L" / "<<m.pk.pn<<L" / "<<m.pk.kp<<L" / "<<m.pk.kn;report(status,os.str());}
    fitAB(m,input,target,sr,status);refineB(m,input,target,sr,status);

    const fs::path original2048=work/L"native_original_2048.clo";if(!serialize2048(original2048,m,sr,error)){r.error=error;fs::remove_all(work,ec);return r;}

    fs::path toneMatched2048;
    if(refine.enabled){
        report(status,L"Tone Match: preparing matched NAM target...");
        fs::path refineStimulus=stim;
        std::vector<float> refineTarget44100=toneTarget44100;
        if(!refine.referenceWav.empty()){
            StimulusConfig refineStimulusConfig=stimulus;refineStimulusConfig.tailMode=TailMode::RecordedAudio;refineStimulusConfig.recordedAudio=refine.referenceWav;
            refineStimulus=work/L"refine_input_wav.wav";
            if(!buildStimulus(originalStimulus,refineStimulusConfig,refineStimulus,error)){r.error="Could not prepare refinement test WAV: "+error;fs::remove_all(work,ec);return r;}
            std::vector<float> refineS44;std::uint32_t refineSr44=0;if(!readPcm16Mono(refineStimulus,refineS44,refineSr44,error)){r.error=error;fs::remove_all(work,ec);return r;}
            std::vector<float> unusedInput,refineRendered;double refineRate=sr;if(!renderNam(modelPath,refineS44,trainer.blockSize,0.31f,unusedInput,refineRendered,refineRate,error,status)){r.error="Could not render refinement stimulus through NAM: "+error;fs::remove_all(work,ec);return r;}
            refineTarget44100=prepareToneTarget44100(refineRendered,refineRate);
            report(status,L"Tone Match: same refinement stimulus through NAM Full vs original native CLO.");
        }else report(status,L"Tone Match: original conversion stimulus through NAM Full vs original native CLO.");
        const fs::path targetWav=work/L"refine_nam_output.wav";if(!writeMonoFloat32Wav(targetWav,refineTarget44100,44100,error)){r.error=error;fs::remove_all(work,ec);return r;}
        toneMatched2048=work/L"native_2048_TONEMATCH.clo";
        if(!refineCloBOnly(original2048,refineStimulus,targetWav,toneMatched2048,refine,error,status)){r.error=error.empty()?"CLO refinement failed.":error;fs::remove_all(work,ec);return r;}
    }

    fs::path sourceForOutput=original2048;
    if(correction.enabled){
        if(correction.wav.empty()){r.error="Select a Corrective IR WAV file.";fs::remove_all(work,ec);return r;}
        report(status,L"Applying Corrective IR...");
        const fs::path corrected=work/L"native_2048_corrected.clo";CorrectiveIrStats stats;
        if(!applyCorrectiveIrToClo(original2048,correction.wav,corrected,stats,error)){r.error=error.empty()?"Corrective IR failed.":error;fs::remove_all(work,ec);return r;}
        report(status,L"Corrective IR applied: linear convolution, RMS match, -6 dB post gain. RMS gain "+std::to_wstring(stats.rmsGainDb)+L" dB; total "+std::to_wstring(stats.totalGainDb)+L" dB.");
        sourceForOutput=corrected;
    }

    if(refine.enabled){
        report(status,L"Generating Tone Match GP-200 1024 CLO...");
        r.gp2001024=uniqueOutput(outputDirectory,inputNam.stem().wstring(),L"_NATIVE_GP200_1024_TONEMATCH.clo");
        if(!makeGp200CompactClo(toneMatched2048,r.gp2001024,error)){r.error=error;fs::remove_all(work,ec);return r;}
    }else{
        r.gp2001024=uniqueOutput(outputDirectory,inputNam.stem().wstring(),L"_NATIVE_GP200_1024.clo");
        if(!makeGp200CompactClo(sourceForOutput,r.gp2001024,error)){r.error=error;fs::remove_all(work,ec);return r;}
    }
    fs::remove_all(work,ec);r.ok=true;report(status,L"Conversion complete.");return r;
}

BatchConversionResult convertNamFolderToClo(const fs::path& inputDirectory,const fs::path& outputDirectory,
                                                   StimulusConfig stimulus,CorrectiveIrConfig correction,CloRefineConfig refine,
                                                   NativeConverterConfig trainer,const StatusCallback& status){BatchConversionResult b;std::error_code ec;std::vector<fs::path>files;for(const auto&e:fs::directory_iterator(inputDirectory,ec)){if(ec)break;if(!e.is_regular_file(ec)||ec)continue;auto ext=e.path().extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});if(ext==L".nam")files.push_back(e.path());}std::sort(files.begin(),files.end());b.total=files.size();for(std::size_t i=0;i<files.size();++i){report(status,L"Batch "+std::to_wstring(i+1)+L"/"+std::to_wstring(files.size())+L": "+files[i].filename().wstring());auto r=convertNamToClo(files[i],outputDirectory,stimulus,correction,refine,trainer,status);if(r.ok)++b.succeeded;else ++b.failed;b.items.push_back(std::move(r));}b.ok=b.total>0&&b.failed==0;return b;}

} // namespace ntc
