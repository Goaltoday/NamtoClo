#include "clo_refiner.hpp"
#include "common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <vector>

namespace ntc {
namespace {
constexpr std::uint32_t kSampleRate = 44100;
constexpr std::size_t kCoeffBase = 0x88;
constexpr std::size_t kPreferredFftSize = 32768;

// Tone Match SOURCE must reproduce the same external GP-200 SnapTone wrapper
// used by CloPlayer.  Gain is before the CLO core and therefore changes the
// nonlinear excitation; Volume is after the CLO core.  CloPlayer defaults to
// visible Gain=50 and Volume=50.
constexpr float kToneMatchGainControl = 50.0f;
constexpr float kToneMatchVolumeControl = 50.0f;

float cloPlayerGainControlToLinear(float visibleControl) {
    constexpr float uiToInternalSlope  = 0.69311597f;
    constexpr float uiToInternalOffset = 25.201331f;
    constexpr float firmwareOffset     = -3.986313819885254f;
    constexpr float firmwareSlope      =  0.07972627133131027f;
    const float internalGain = uiToInternalSlope * visibleControl + uiToInternalOffset;
    return std::exp(firmwareOffset + internalGain * firmwareSlope);
}

float cloPlayerVolumeControlToLinear(float control) {
    constexpr float offset = -3.986313819885254f;
    constexpr float slope  =  0.07972627133131027f;
    return std::exp(offset + control * slope);
}

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
float lef(const std::uint8_t* p) { auto u=le32(p); float v{}; std::memcpy(&v,&u,4); return v; }
double led(const std::uint8_t* p) { std::uint64_t u=0; for(int i=0;i<8;++i) u|=static_cast<std::uint64_t>(p[i])<<(8*i); double v{}; std::memcpy(&v,&u,8); return v; }

bool readMono44100(const fs::path& path, std::vector<float>& out, std::string& error) {
    std::ifstream f(path, std::ios::binary); if(!f){ error="Cannot read WAV: "+pathToUtf8(path); return false; }
    std::array<std::uint8_t,12> h{}; f.read(reinterpret_cast<char*>(h.data()),12);
    if(f.gcount()!=12 || std::memcmp(h.data(),"RIFF",4)!=0 || std::memcmp(h.data()+8,"WAVE",4)!=0){ error="Invalid WAV: "+pathToUtf8(path); return false; }
    std::uint16_t fmt=0,ch=0,bits=0,align=0; std::uint32_t sr=0; std::vector<std::uint8_t> data;
    while(f){ std::array<std::uint8_t,8> c{}; f.read(reinterpret_cast<char*>(c.data()),8); if(f.gcount()!=8) break; auto n=le32(c.data()+4); std::vector<std::uint8_t> b(n); if(n){ f.read(reinterpret_cast<char*>(b.data()),n); if(static_cast<std::uint32_t>(f.gcount())!=n){ error="Truncated WAV"; return false; }} if(n&1) f.seekg(1,std::ios::cur);
        if(std::memcmp(c.data(),"fmt ",4)==0 && n>=16){ fmt=le16(b.data()); ch=le16(b.data()+2); sr=le32(b.data()+4); align=le16(b.data()+12); bits=le16(b.data()+14); if(fmt==0xfffe && n>=40) fmt=le16(b.data()+24); }
        else if(std::memcmp(c.data(),"data",4)==0) data=std::move(b);
    }
    if(sr==0 || ch==0 || align==0 || data.empty()){
        error="Invalid/empty WAV for refinement: "+pathToUtf8(path);
        return false;
    }
    const std::size_t frames=data.size()/align; const int bps=(bits+7)/8;
    if(bps<=0 || static_cast<std::size_t>(bps)*ch>align){
        error="Unsupported WAV block alignment for refinement: "+pathToUtf8(path);
        return false;
    }
    std::vector<float> decoded(frames);
    for(std::size_t i=0;i<frames;++i){ const auto* p=data.data()+i*align; double sum=0; for(std::uint16_t cc=0;cc<ch;++cc){ const auto* q=p+cc*bps; double v=0;
            if(fmt==1 && bits==8) v=(static_cast<int>(q[0])-128)/128.0;
            else if(fmt==1 && bits==16) v=static_cast<std::int16_t>(le16(q))/32768.0;
            else if(fmt==1 && bits==24){ std::int32_t x=q[0]|(q[1]<<8)|(q[2]<<16); if(x&0x800000)x|=0xff000000; v=x/8388608.0; }
            else if(fmt==1 && bits==32){ auto x=static_cast<std::int32_t>(le32(q)); v=x/2147483648.0; }
            else if(fmt==3 && bits==32){ auto u=le32(q); float x{}; std::memcpy(&x,&u,4); v=std::isfinite(x)?x:0; }
            else {
                error="Unsupported WAV format for refinement ("+std::to_string(sr)+" Hz, "+std::to_string(ch)+" ch, "+std::to_string(bits)+" bit, fmt "+std::to_string(fmt)+"): "+pathToUtf8(path);
                return false;
            }
            sum+=v; }
        decoded[i]=static_cast<float>(sum/ch); }

    if(sr==kSampleRate){ out=std::move(decoded); return true; }

    const double ratio=static_cast<double>(sr)/static_cast<double>(kSampleRate);
    const std::size_t outFrames=static_cast<std::size_t>(std::llround(static_cast<double>(decoded.size())/ratio));
    out.resize(outFrames);
    for(std::size_t i=0;i<outFrames;++i){
        const double pos=static_cast<double>(i)*ratio;
        const std::size_t i0=std::min(static_cast<std::size_t>(pos),decoded.size()-1);
        const std::size_t i1=std::min(i0+1,decoded.size()-1);
        const double frac=pos-static_cast<double>(i0);
        out[i]=static_cast<float>(decoded[i0]+(decoded[i1]-decoded[i0])*frac);
    }
    return true;
}

struct Biquad { double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0; float process(float x){ double y=b0*x+z1; z1=b1*x-a1*y+z2; z2=b2*x-a2*y; return static_cast<float>(y);} };
struct AP { float a=0,s=0; float process(float x){ float y=s+a*x; s=x-a*y; return y; } };
struct Poly {
    std::vector<AP> a,b; float delay=0;
    Poly(std::initializer_list<float> aa,std::initializer_list<float> bb){ for(float x:aa)a.push_back({x,0}); for(float x:bb)b.push_back({x,0}); }
    float r(std::vector<AP>& v,float x){for(auto& s:v)x=s.process(x);return x;}
    void up(float x,float& e,float& o){e=r(a,x);o=r(b,x);} float down(float e,float o){float x=r(a,e), y=r(b,o); float z=.5f*(x+delay);delay=y;return z;}
};

struct Model { Biquad pre,post; std::vector<float>A,B; float pp=0,pn=0,kp=0,kn=0; };
bool parseModel(const std::vector<std::uint8_t>& d, Model& m, std::string& error){
    if(d.size()<0x88 || std::memcmp(d.data(),"VTSI",4)!=0){error="Invalid VTSI CLO.";return false;}
    m.pre={led(d.data()+0x18),led(d.data()+0x20),led(d.data()+0x28),led(d.data()+0x30),led(d.data()+0x38)};
    m.post={led(d.data()+0x40),led(d.data()+0x48),led(d.data()+0x50),led(d.data()+0x58),led(d.data()+0x60)};
    m.pp=lef(d.data()+0x68);m.pn=lef(d.data()+0x6c);m.kp=lef(d.data()+0x70);m.kn=lef(d.data()+0x74);
    auto sa=le32(d.data()+0x78),ca=le32(d.data()+0x7c),sb=le32(d.data()+0x80),cb=le32(d.data()+0x84);
    std::uint64_t need=kCoeffBase+4ull*std::max<std::uint64_t>(std::uint64_t(sa)+ca,std::uint64_t(sb)+cb); if(ca==0||cb==0||need>d.size()){error="Truncated CLO coefficients.";return false;}
    m.A.resize(ca);m.B.resize(cb);for(std::size_t i=0;i<ca;++i)m.A[i]=lef(d.data()+kCoeffBase+4ull*(sa+i));for(std::size_t i=0;i<cb;++i)m.B[i]=lef(d.data()+kCoeffBase+4ull*(sb+i));return true;
}

std::vector<float> precomputeA(const Model& src,const std::vector<float>& in,std::size_t n,float inputGain=1.0f){
    Model m=src; std::vector<float> hist(m.A.size(),0), out(n); std::size_t ix=0;
    for(std::size_t i=0;i<n;++i){ float x=m.pre.process(in[i]*inputGain); hist[ix]=x; double s=0;std::size_t h=ix;for(float t:m.A){s+=double(t)*hist[h];h=h? h-1:hist.size()-1;}ix=(ix+1)%hist.size();out[i]=float(s);}return out;
}


void fft(std::vector<std::complex<float>>& a, bool inverse){
    const std::size_t n=a.size();
    for(std::size_t i=1,j=0;i<n;++i){
        std::size_t bit=n>>1;
        for(;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if(i<j) std::swap(a[i],a[j]);
    }
    constexpr float pi=3.14159265358979323846f;
    for(std::size_t len=2;len<=n;len<<=1){
        const float ang=(inverse?2.0f:-2.0f)*pi/static_cast<float>(len);
        const std::complex<float> wlen(std::cos(ang),std::sin(ang));
        for(std::size_t i=0;i<n;i+=len){
            std::complex<float> w(1.0f,0.0f);
            for(std::size_t j=0;j<len/2;++j){
                const auto u=a[i+j];
                const auto v=a[i+j+len/2]*w;
                a[i+j]=u+v;
                a[i+j+len/2]=u-v;
                w*=wlen;
            }
        }
    }
    if(inverse){ const float inv=1.0f/static_cast<float>(n); for(auto& v:a)v*=inv; }
}

std::size_t nextPow2(std::size_t n){ std::size_t p=1; while(p<n)p<<=1; return p; }

struct FirFftPlan {
    std::size_t fftSize=0, filterLen=0, hop=0;
    std::vector<std::complex<float>> filterSpectrum;

    explicit FirFftPlan(const std::vector<float>& h){
        filterLen=h.size();
        fftSize=nextPow2(std::max(kPreferredFftSize,filterLen*2));
        hop=fftSize-filterLen+1;
        filterSpectrum.assign(fftSize,{});
        for(std::size_t i=0;i<h.size();++i) filterSpectrum[i]=std::complex<float>(h[i],0.0f);
        fft(filterSpectrum,false);
    }

    void process(const std::vector<float>& input,std::vector<float>& output) const {
        output.assign(input.size(),0.0f);
        std::vector<std::complex<float>> buf(fftSize);
        const std::size_t overlap=filterLen-1;
        for(std::size_t pos=0;pos<input.size();pos+=hop){
            std::fill(buf.begin(),buf.end(),std::complex<float>{});
            for(std::size_t j=0;j<overlap;++j){
                if(pos+j>=overlap) buf[j]=std::complex<float>(input[pos+j-overlap],0.0f);
            }
            const std::size_t count=std::min(hop,input.size()-pos);
            for(std::size_t j=0;j<count;++j) buf[overlap+j]=std::complex<float>(input[pos+j],0.0f);
            fft(buf,false);
            for(std::size_t k=0;k<fftSize;++k) buf[k]*=filterSpectrum[k];
            fft(buf,true);
            for(std::size_t j=0;j<count;++j) output[pos+j]=buf[overlap+j].real();
        }
    }
};

void renderPreB(const Model& base,const std::vector<float>& aout,float pp,float pn,float kp,float kn,std::vector<float>& out);




void renderPreB(const Model& base,const std::vector<float>& aout,float pp,float pn,float kp,float kn,std::vector<float>& out){
    Biquad post=base.post;
    Poly u1({.045728147029876709f,.3325011134147644f,.66320204734802246f,.93385583162307739f},{.16808754205703735f,.50448572635650635f,.80378085374832153f});
    Poly u2({.054230779409408569f,.39879697561264038f,.86291784048080444f},{.19969958066940308f,.62109684944152832f});
    Poly d1({.070765949785709381f,.51316756010055542f},{.25785309076309204f,.81731736660003662f});
    Poly d2({.054217524826526642f,.38308733701705933f,.74872094392776489f},{.19679796695709229f,.57313638925552368f,.91429370641708374f});
    out.resize(aout.size());
    auto shape=[&](float x){return x>0?pp*(1-std::exp(-kp*x)):pn*(std::exp(kn*x)-1);};
    for(std::size_t i=0;i<aout.size();++i){
        float a,b,c0,c1;
        u1.up(aout[i],a,b);
        u2.up(a,c0,c1); c0=shape(c0); c1=shape(c1); const float e0=d1.down(c0,c1);
        u2.up(b,c0,c1); c0=shape(c0); c1=shape(c1); const float e1=d1.down(c0,c1);
        out[i]=post.process(d2.down(e0,e1));
    }
}

std::vector<float> hannWindow(std::size_t n){
    std::vector<float> w(n);
    constexpr double pi=3.14159265358979323846;
    if(n<=1){ if(n==1)w[0]=1.0f; return w; }
    for(std::size_t i=0;i<n;++i) w[i]=static_cast<float>(0.5-0.5*std::cos(2.0*pi*double(i)/double(n-1)));
    return w;
}

}

namespace {
// Single direct Tone Match correction at the final destination size.
constexpr std::size_t kV26Fft=16384;
constexpr std::size_t kV26Hop=4096;
constexpr int kV26Groups=11;
constexpr double kV26SilenceDb=-55.0;
constexpr double kV26MinHz=30.0;
constexpr double kV26MaxHz=20000.0;
constexpr double kV26CmpMinHz=40.0;
constexpr double kV26CmpMaxHz=18000.0;
constexpr std::size_t kV26Points=512;
constexpr std::size_t kV26IrLength=2048;
constexpr double kV26NegInf=-160.0;

struct V26Profile{std::vector<double> f,db; std::size_t frames=0; bool valid()const{return f.size()>1&&db.size()==f.size();}};
struct V26Comp{std::vector<double> f,raw; bool valid()const{return f.size()>1&&raw.size()==f.size();}};
static double v26median(std::vector<double> v){if(v.empty())return 0; auto m=v.begin()+static_cast<std::ptrdiff_t>(v.size()/2);std::nth_element(v.begin(),m,v.end());return *m;}
static double v26interp(const std::vector<double>&f,const std::vector<double>&v,double hz){if(f.empty()||f.size()!=v.size())return 0;if(hz<=f.front())return v.front();if(hz>=f.back())return v.back();auto u=std::lower_bound(f.begin(),f.end(),hz);auto i1=static_cast<std::size_t>(std::distance(f.begin(),u));auto i0=i1-1;double a=(hz-f[i0])/(f[i1]-f[i0]);return v[i0]+std::clamp(a,0.0,1.0)*(v[i1]-v[i0]);}

static V26Profile v26analyse(const std::vector<float>& s,double scale,std::size_t start,std::size_t count,const std::vector<std::size_t>* fixedWindows=nullptr){
    V26Profile p;if(start>=s.size())return p;std::size_t end=std::min(s.size(),start+count);if(end-start<kV26Fft)return p;
    auto win=hannWindow(kV26Fft);std::array<std::vector<long double>,kV26Groups> sums;for(auto&g:sums)g.assign(kV26Fft/2+1,0);std::array<std::size_t,kV26Groups> counts{};std::vector<std::complex<float>> b(kV26Fft);double silence=std::pow(10.0,kV26SilenceDb/20.0);std::size_t accepted=0;
    // source/target are internal floating-point renders, not PCM files being
    // inspected for hard digital clipping.  A valid NAM or CLO render can
    // legitimately exceed +/-1.0.  Rejecting an entire FFT frame when just
    // one sample crosses 0.999 caused active DI material (especially bass)
    // to produce zero accepted frames and "Tone Match comparison invalid".
    // Keep only the silence guard here.
    for(std::size_t pos=start;pos+kV26Fft<=end;pos+=kV26Hop){if(fixedWindows && !std::binary_search(fixedWindows->begin(),fixedWindows->end(),pos))continue;long double ss=0;double mean=0;for(std::size_t i=0;i<kV26Fft;++i){double x=scale*s[pos+i];ss+=x*x;mean+=x;}double rms=std::sqrt(static_cast<double>(ss/kV26Fft));if(!std::isfinite(rms))return {};if(!fixedWindows && rms<silence)continue;mean/=kV26Fft;for(std::size_t i=0;i<kV26Fft;++i)b[i]={static_cast<float>((scale*s[pos+i]-mean)*win[i]),0};fft(b,false);auto gi=accepted%kV26Groups;for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;double mag=std::abs(b[k]);sums[gi][k]+=mag*mag;}++counts[gi];++accepted;}
    p.frames=accepted;if(!accepted)return p;std::vector<double> spec(kV26Fft/2+1,kV26NegInf);
    for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;std::vector<double> means;for(int g=0;g<kV26Groups;++g)if(counts[g]){double mp=static_cast<double>(sums[g][k]/counts[g]);means.push_back(10*std::log10(std::max(mp,1e-20)));}double med=v26median(means);spec[k]=med;}
    for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;p.f.push_back(hz);p.db.push_back(spec[k]);}return p;
}
static V26Comp v26compare(const V26Profile&s,const V26Profile&t){V26Comp c;if(!s.valid()||!t.valid())return c;double a=std::log(kV26CmpMinHz),b=std::log(kV26CmpMaxHz);for(std::size_t i=0;i<kV26Points;++i){double q=double(i)/(kV26Points-1),hz=std::exp(a+q*(b-a));c.f.push_back(hz);c.raw.push_back(v26interp(t.f,t.db,hz)-v26interp(s.f,s.db,hz));}return c;}
static std::vector<float> v26minPhaseIr(const V26Comp&c){const auto& curve=c.raw;const std::size_t N=4096;std::vector<std::complex<float>> logsp(N),cep(N),mc(N),cls(N),mps(N),imp(N);for(std::size_t k=0;k<=N/2;++k){double hz=double(k)*kSampleRate/N,db=v26interp(c.f,curve,hz),lm=db*0.11512925464970229;logsp[k]={float(lm),0};if(k>0&&k<N/2)logsp[N-k]={float(lm),0};}fft(logsp,true);cep=logsp;mc[0]=cep[0];for(std::size_t i=1;i<N/2;++i)mc[i]=cep[i]*2.0f;mc[N/2]=cep[N/2];fft(mc,false);cls=mc;for(std::size_t i=0;i<N;++i)mps[i]=std::exp(cls[i]);fft(mps,true);imp=mps;std::vector<float> ir(kV26IrLength);for(std::size_t i=0;i<ir.size();++i)ir[i]=imp[i].real();return ir;}
static void renderWithB(const std::vector<float>& preB,const std::vector<float>& B,std::vector<float>& out,float outputGain=1.0f){
    FirFftPlan plan(B);
    plan.process(preB,out);
    if(outputGain!=1.0f) for(auto& x:out) x*=outputGain;
}


// Use the same informative windows for initial model, target and final render.
// A silent output window remains in the diagnostic measurement.
static std::vector<std::size_t> sharedWindows(const std::vector<float>& source,
                                             const std::vector<float>& target) {
    std::vector<std::size_t> positions;
    const double threshold = std::pow(10.0, kV26SilenceDb / 20.0);
    for (std::size_t pos=0; pos+kV26Fft<=std::min(source.size(),target.size()); pos+=kV26Hop) {
        long double a=0, b=0;
        for (std::size_t i=0;i<kV26Fft;++i) {
            const double x=source[pos+i], y=target[pos+i];
            a+=x*x; b+=y*y;
        }
        const double ar=std::sqrt(static_cast<double>(a/kV26Fft));
        const double br=std::sqrt(static_cast<double>(b/kV26Fft));
        if (std::isfinite(ar) && std::isfinite(br) && ar>=threshold && br>=threshold)
            positions.push_back(pos);
    }
    return positions;
}

static bool finiteSamples(const std::vector<float>& samples) {
    return std::all_of(samples.begin(),samples.end(),[](float v){return std::isfinite(v);});
}

// Same historical Tone Match normalization: RMS of B coefficients, 0 dB post gain.
// Apply/truncate/normalize at the destination size BEFORE measuring the final render.
static bool correctedFinalB(const std::vector<float>& base,const std::vector<float>& ir,
                             std::vector<float>& result) {
    if(base.empty() || ir.empty() || !finiteSamples(base) || !finiteSamples(ir)) return false;
    std::vector<double> conv(base.size());
    long double oldEnergy=0, newEnergy=0;
    for(std::size_t n=0;n<base.size();++n) {
        long double sum=0;
        for(std::size_t k=0;k<=std::min(n,ir.size()-1);++k)
            sum+=static_cast<long double>(base[n-k])*ir[k];
        conv[n]=static_cast<double>(sum);
        oldEnergy+=static_cast<long double>(base[n])*base[n];
        newEnergy+=static_cast<long double>(conv[n])*conv[n];
    }
    if(!(oldEnergy>1e-40L) || !(newEnergy>1e-40L) || !std::isfinite(newEnergy))return false;
    const double gain=std::sqrt(static_cast<double>(oldEnergy/newEnergy));
    if(!std::isfinite(gain))return false;
    result.resize(base.size());
    for(std::size_t i=0;i<result.size();++i) {
        const double v=conv[i]*gain;
        if(!std::isfinite(v) || std::abs(v)>std::numeric_limits<float>::max())return false;
        result[i]=static_cast<float>(v);
    }
    return true;
}

static double spectralRmse(const V26Profile& source,const V26Profile& target) {
    const auto comparison=v26compare(source,target);
    if(!comparison.valid())return std::numeric_limits<double>::infinity();
    long double sum=0;
    for(double d:comparison.raw) {
        if(!std::isfinite(d))return std::numeric_limits<double>::infinity();
        sum+=d*d;
    }
    // Diagnostic only: direct spectral RMSE, no level fitting or smoothing.
    return std::sqrt(static_cast<double>(sum/comparison.raw.size()));
}

static bool writeFinalClo(const std::vector<std::uint8_t>& source,const std::vector<float>& b,
                          const fs::path& output,std::string& error) {
    const std::size_t declared=0x88+4*(128+b.size());
    // Preserve GP-200's historically accepted padded physical container.
    const std::size_t physical=b.size()==1024 ? 0x2288 : declared;
    std::vector<std::uint8_t> data(physical,0);
    std::copy_n(source.begin(),0x88+128*4,data.begin());
    auto put32=[&](std::size_t off,std::uint32_t v) {
        for(int i=0;i<4;++i)data[off+i]=static_cast<std::uint8_t>(v>>(8*i));
    };
    put32(4,static_cast<std::uint32_t>(declared));
    put32(0x14,static_cast<std::uint32_t>(4*(128+b.size())));
    put32(0x78,0);put32(0x7c,128);put32(0x80,128);put32(0x84,static_cast<std::uint32_t>(b.size()));
    for(std::size_t i=0;i<b.size();++i) {
        std::uint32_t bits;std::memcpy(&bits,&b[i],4);put32(0x288+4*i,bits);
    }
    std::uint16_t crc=0xffff;
    for(std::size_t i=12;i<declared;++i) {
        crc^=data[i];
        for(int bit=0;bit<8;++bit)crc=(crc&1)?static_cast<std::uint16_t>((crc>>1)^0xa001):static_cast<std::uint16_t>(crc>>1);
    }
    data[8]=static_cast<std::uint8_t>(crc>>8);data[9]=static_cast<std::uint8_t>(crc);
    return writeFileBytes(output,data.data(),data.size(),error);
}

} // namespace

bool refineCloBOnly(const fs::path& inputClo2048,
                    const fs::path& stimulusWav,
                    const fs::path& targetWav,
                    const fs::path& outputClo,
                    const CloRefineConfig& config,
                    std::string& error,
                    const RefineStatusCallback& status,
                    CloRefineStats* stats) {
    error.clear();
    CloRefineStats result;
    if(stats)*stats=result;
    if(config.destination!=CloDestination::Gp200 && config.destination!=CloDestination::Gp5) {
        error="Invalid CLO destination.";return false;
    }
    std::vector<std::uint8_t> bytes;
    if(!readFileBytes(inputClo2048,bytes,error))return false;
    // The initial NAM conversion and optional manual Corrective IR remain B2048.
    if(bytes.size()!=0x2288 || le32(bytes.data()+4)!=0x2288 || le32(bytes.data()+0x14)!=0x2200 ||
       le32(bytes.data()+0x78)!=0 || le32(bytes.data()+0x7c)!=128 ||
       le32(bytes.data()+0x80)!=128 || le32(bytes.data()+0x84)!=2048) {
        error="Final Tone Match requires the original A128/B2048 CLO.";return false;
    }
    Model model;
    if(!parseModel(bytes,model,error))return false;
    if(!finiteSamples(model.A) || !finiteSamples(model.B)) {
        error="Non-finite CLO coefficients.";return false;
    }
    const std::size_t taps=destinationBTaps(config.destination);
    model.B.resize(taps);
    std::vector<float> input,target;
    if(!readMono44100(stimulusWav,input,error) || !readMono44100(targetWav,target,error))return false;
    constexpr std::size_t usefulFrames=70u*kSampleRate, tailFrames=20u*kSampleRate;
    if(input.size()<usefulFrames || target.size()<usefulFrames) {
        error="Tone Match requires the complete 70-second stimulus and NAM render.";return false;
    }
    // The trainer appends 600 guard samples: compare exactly 50-70 seconds.
    input.resize(usefulFrames);target.resize(usefulFrames);
    if(!finiteSamples(input) || !finiteSamples(target)) {
        error="Non-finite Tone Match audio.";return false;
    }
    if(status)status(L"Tone Match: rendering destination B"+std::to_wstring(taps)+L" baseline...");
    const float inputGain=cloPlayerGainControlToLinear(kToneMatchGainControl);
    const float outputGain=cloPlayerVolumeControlToLinear(kToneMatchVolumeControl);
    auto aout=precomputeA(model,input,input.size(),inputGain);
    std::vector<float> preB,original;
    renderPreB(model,aout,model.pp,model.pn,model.kp,model.kn,preB);
    renderWithB(preB,model.B,original,outputGain);
    if(!finiteSamples(original) || !finiteSamples(preB)) {
        error="CLO render contains non-finite samples.";return false;
    }
    const std::size_t tailStart=usefulFrames-tailFrames;
    std::vector<float> sourceTail(original.begin()+tailStart,original.end());
    std::vector<float> targetTail(target.begin()+tailStart,target.end());
    const auto windows=sharedWindows(sourceTail,targetTail);
    if(windows.empty()) {
        error="Tone Match has no shared non-silent analysis windows.";return false;
    }
    result.analysisFrames=windows.size();
    const auto sourceProfile=v26analyse(sourceTail,1.0,0,tailFrames,&windows);
    const auto targetProfile=v26analyse(targetTail,1.0,0,tailFrames,&windows);
    const auto comparison=v26compare(sourceProfile,targetProfile);
    if(!comparison.valid()) {error="Tone Match comparison invalid.";return false;}

    if(status)status(L"Tone Match: applying direct correction on final B"+std::to_wstring(taps)+L"...");
    const auto ir=v26minPhaseIr(comparison);
    std::vector<float> finalB;
    if(!correctedFinalB(model.B,ir,finalB)) {
        error="Direct Tone Match correction is not numerically valid.";return false;
    }
    std::vector<float> rendered;
    renderWithB(preB,finalB,rendered,outputGain);
    if(!finiteSamples(rendered)) {
        error="Final Tone Match render contains non-finite samples.";return false;
    }
    std::vector<float> tail(rendered.begin()+tailStart,rendered.end());
    result.finalRmseDb=spectralRmse(v26analyse(tail,1.0,0,tailFrames,&windows),targetProfile);
    if(!std::isfinite(result.finalRmseDb)) {
        error="Final Tone Match measurement is invalid.";return false;
    }
    if(status)status(L"Tone Match complete. Final spectral RMSE (dB): "+std::to_wstring(result.finalRmseDb));
    if(!writeFinalClo(bytes,finalB,outputClo,error))return false;
    if(stats)*stats=result;
    return true;
}

} // namespace ntc
