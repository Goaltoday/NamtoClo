#include "clo_refiner.hpp"
#include "common.hpp"
#include "corrective_ir.hpp"

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
    std::uint64_t need=kCoeffBase+4ull*std::max<std::uint64_t>(sa+ca,sb+cb); if(ca==0||cb==0||need>d.size()){error="Truncated CLO coefficients.";return false;}
    m.A.resize(ca);m.B.resize(cb);for(std::size_t i=0;i<ca;++i)m.A[i]=lef(d.data()+kCoeffBase+4ull*(sa+i));for(std::size_t i=0;i<cb;++i)m.B[i]=lef(d.data()+kCoeffBase+4ull*(sb+i));return true;
}

std::vector<float> precomputeA(const Model& src,const std::vector<float>& in,std::size_t n){
    Model m=src; std::vector<float> hist(m.A.size(),0), out(n); std::size_t ix=0;
    for(std::size_t i=0;i<n;++i){ float x=m.pre.process(in[i]); hist[ix]=x; double s=0;std::size_t h=ix;for(float t:m.A){s+=double(t)*hist[h];h=h? h-1:hist.size()-1;}ix=(ix+1)%hist.size();out[i]=float(s);}return out;
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

double fitScale(const std::vector<float>& candidate,const std::vector<float>& target){
    const std::size_t n=std::min(candidate.size(),target.size());
    long double cc=0.0L,ct=0.0L;
    for(std::size_t i=0;i<n;++i){ cc+=static_cast<long double>(candidate[i])*candidate[i]; ct+=static_cast<long double>(candidate[i])*target[i]; }
    return cc>1e-30L?static_cast<double>(ct/cc):1.0;
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
// Tone Match analysis on the final 20 s.
// No band guards, no confidence masking, no artificial HF freeze and no global-level removal.
// The generated 2048-sample minimum-phase IR is applied directly in memory to Block B.
// This avoids creating a temporary diagnostic WAV without changing the correction itself.
constexpr std::size_t kV26Fft=16384;
constexpr std::size_t kV26Hop=4096;
constexpr int kV26Groups=11;
constexpr double kV26SilenceDb=-55.0;
constexpr double kV26Clip=0.999;
constexpr double kV26MinHz=30.0;
constexpr double kV26MaxHz=20000.0;
constexpr double kV26CmpMinHz=40.0;
constexpr double kV26CmpMaxHz=18000.0;
constexpr std::size_t kV26Points=512;
constexpr std::size_t kV26IrLength=2048;
constexpr double kV26Smooth=0.05; // same slider setting requested by the user
constexpr double kV26MadToSigma=1.4826;
constexpr double kV26NegInf=-160.0;
constexpr double kV26ConfRefDb=3.0;

struct V26Profile{std::vector<double> f,db,conf; double coverage=0; std::size_t frames=0; bool valid()const{return f.size()>1&&db.size()==f.size()&&conf.size()==f.size();}};
struct V26Comp{std::vector<double> f,raw,conf; bool valid()const{return f.size()>1&&raw.size()==f.size()&&conf.size()==f.size();}};
static double v26clamp(double x){return std::clamp(x,0.0,1.0);} 
static double v26median(std::vector<double> v){if(v.empty())return 0; auto m=v.begin()+static_cast<std::ptrdiff_t>(v.size()/2);std::nth_element(v.begin(),m,v.end());return *m;}
static double v26mad(const std::vector<double>& v,double med){std::vector<double>d;d.reserve(v.size());for(double x:v)d.push_back(std::abs(x-med));return v26median(std::move(d));}
static double v26interp(const std::vector<double>&f,const std::vector<double>&v,double hz){if(f.empty()||f.size()!=v.size())return 0;if(hz<=f.front())return v.front();if(hz>=f.back())return v.back();auto u=std::lower_bound(f.begin(),f.end(),hz);auto i1=static_cast<std::size_t>(std::distance(f.begin(),u));auto i0=i1-1;double a=(hz-f[i0])/(f[i1]-f[i0]);return v[i0]+std::clamp(a,0.0,1.0)*(v[i1]-v[i0]);}

static V26Profile v26analyse(const std::vector<float>& s,double scale,std::size_t start,std::size_t count){
    V26Profile p;if(start>=s.size())return p;std::size_t end=std::min(s.size(),start+count);if(end-start<kV26Fft)return p;
    auto win=hannWindow(kV26Fft);std::array<std::vector<long double>,kV26Groups> sums;for(auto&g:sums)g.assign(kV26Fft/2+1,0);std::array<std::size_t,kV26Groups> counts{};std::vector<std::complex<float>> b(kV26Fft);double silence=std::pow(10.0,kV26SilenceDb/20.0);std::size_t accepted=0;
    for(std::size_t pos=start;pos+kV26Fft<=end;pos+=kV26Hop){long double ss=0;bool clip=false;double mean=0;for(std::size_t i=0;i<kV26Fft;++i){double x=scale*s[pos+i];ss+=x*x;mean+=x;if(std::abs(x)>=kV26Clip)clip=true;}double rms=std::sqrt(static_cast<double>(ss/kV26Fft));if(rms<silence||clip)continue;mean/=kV26Fft;for(std::size_t i=0;i<kV26Fft;++i)b[i]={static_cast<float>((scale*s[pos+i]-mean)*win[i]),0};fft(b,false);auto gi=accepted%kV26Groups;for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;double mag=std::abs(b[k]);sums[gi][k]+=mag*mag;}++counts[gi];++accepted;}
    p.frames=accepted;if(!accepted)return p;std::size_t active=0;for(auto c:counts)if(c)++active;std::vector<double> spec(kV26Fft/2+1,kV26NegInf),dev(kV26Fft/2+1,0);double strongest=kV26NegInf;
    for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;std::vector<double> means;for(int g=0;g<kV26Groups;++g)if(counts[g]){double mp=static_cast<double>(sums[g][k]/counts[g]);means.push_back(10*std::log10(std::max(mp,1e-20)));}double med=v26median(means);spec[k]=med;dev[k]=kV26MadToSigma*v26mad(means,med);strongest=std::max(strongest,med);}
    std::size_t incl=0,confident=0;for(std::size_t k=0;k<=kV26Fft/2;++k){double hz=double(k)*kSampleRate/kV26Fft;if(hz<kV26MinHz||hz>kV26MaxHz)continue;double frameC=v26clamp(double(accepted)/32.0),groupC=v26clamp(double(active)/kV26Groups),energyC=v26clamp((spec[k]-strongest+60.0)/60.0),stable=1.0/(1.0+dev[k]/kV26ConfRefDb),c=v26clamp(frameC*groupC*energyC*stable);p.f.push_back(hz);p.db.push_back(spec[k]);p.conf.push_back(c);++incl;if(c>=.25)++confident;}if(incl)p.coverage=double(confident)/incl;return p;
}
static V26Comp v26compare(const V26Profile&s,const V26Profile&t){V26Comp c;if(!s.valid()||!t.valid())return c;double a=std::log(kV26CmpMinHz),b=std::log(kV26CmpMaxHz);for(std::size_t i=0;i<kV26Points;++i){double q=double(i)/(kV26Points-1),hz=std::exp(a+q*(b-a));c.f.push_back(hz);c.raw.push_back(v26interp(t.f,t.db,hz)-v26interp(s.f,s.db,hz));c.conf.push_back(v26clamp(std::min(v26interp(s.f,s.conf,hz),v26interp(t.f,t.conf,hz))));}return c;}
static double v26SmoothWidth(double amount){struct P{double a,w;};static constexpr P p[]={{0,0},{.25,1.0/24},{.5,1.0/12},{.75,1.0/6},{1,1.0/3}};if(amount<=0)return 0;for(int i=1;i<5;++i)if(amount<=p[i].a){double q=(amount-p[i-1].a)/(p[i].a-p[i-1].a);return p[i-1].w+q*(p[i].w-p[i-1].w);}return p[4].w;}
static std::vector<double> v26smooth(const V26Comp&c,double amount){std::vector<double> out(c.raw.size());if(amount<=0)return c.raw;double width=v26SmoothWidth(amount),sigma=std::max(width/2.354820045,1e-6),maxd=3*sigma;for(std::size_t i=0;i<c.raw.size();++i){double sw=0,sx=0;for(std::size_t j=0;j<c.raw.size();++j){double d=std::log2(c.f[j]/c.f[i]);if(std::abs(d)>maxd)continue;double w=std::exp(-.5*d*d/(sigma*sigma));sx+=w*c.raw[j];sw+=w;}out[i]=sw>1e-12?sx/sw:c.raw[i];}return out;}
static std::vector<float> v26minPhaseIr(const V26Comp&c,double smooth){auto curve=v26smooth(c,smooth);const std::size_t N=4096;std::vector<std::complex<float>> logsp(N),cep(N),mc(N),cls(N),mps(N),imp(N);for(std::size_t k=0;k<=N/2;++k){double hz=double(k)*kSampleRate/N,db=v26interp(c.f,curve,hz),lm=db*0.11512925464970229;logsp[k]={float(lm),0};if(k>0&&k<N/2)logsp[N-k]={float(lm),0};}fft(logsp,true);cep=logsp;mc[0]=cep[0];for(std::size_t i=1;i<N/2;++i)mc[i]=cep[i]*2.0f;mc[N/2]=cep[N/2];fft(mc,false);cls=mc;for(std::size_t i=0;i<N;++i)mps[i]=std::exp(cls[i]);fft(mps,true);imp=mps;std::vector<float> ir(kV26IrLength);for(std::size_t i=0;i<ir.size();++i)ir[i]=imp[i].real();return ir;}
static void renderWithB(const std::vector<float>& preB,const std::vector<float>& B,std::vector<float>& out){ FirFftPlan plan(B); plan.process(preB,out);}

}

bool refineCloBOnly(const fs::path& inputClo2048,
                    const fs::path& stimulusWav,
                    const fs::path& targetWav,
                    const fs::path& outputClo2048,
                    const CloRefineConfig& config,
                    std::string& error,
                    const RefineStatusCallback& status) {
    (void)config;

    std::vector<std::uint8_t> bytes;
    if (!readFileBytes(inputClo2048, bytes, error)) return false;

    Model m;
    if (!parseModel(bytes, m, error)) return false;

    std::vector<float> in, target;
    if (!readMono44100(stimulusWav, in, error) || !readMono44100(targetWav, target, error)) return false;

    const std::size_t tailFrames = 20u * kSampleRate;
    if (in.size() < tailFrames + kV26Fft) {
        error = "The conversion stimulus is too short for the final-20-s Tone Match";
        return false;
    }
    if (target.size() < tailFrames) {
        error = "The refinement target WAV must contain at least 20.000 seconds of audio";
        return false;
    }

    if (status) status(L"Rendering CLO for Tone Match...");
    auto aout = precomputeA(m, in, in.size());
    std::vector<float> preB, orig;
    renderPreB(m, aout, m.pp, m.pn, m.kp, m.kn, preB);
    renderWithB(preB, m.B, orig);

    const std::size_t sourceStart = orig.size() - tailFrames;
    const std::size_t targetStart = target.size() - tailFrames;
    std::vector<float> sourceTail(orig.begin() + static_cast<std::ptrdiff_t>(sourceStart), orig.end());
    std::vector<float> targetTail(target.begin() + static_cast<std::ptrdiff_t>(targetStart), target.end());

    const double fixedScale = fitScale(sourceTail, targetTail);
    const auto sourceProfile = v26analyse(sourceTail, fixedScale, 0, tailFrames);
    const auto targetProfile = v26analyse(targetTail, 1.0, 0, tailFrames);
    const auto comparison = v26compare(sourceProfile, targetProfile);
    if (!comparison.valid()) {
        error = "Tone Match comparison invalid";
        return false;
    }

    // Keep the established refinement DSP unchanged: 5% smoothing and a
    // 2048-sample minimum-phase IR.  The IR is now passed in memory, so no
    // diagnostic/intermediate WAV is written to disk.
    const auto ir = v26minPhaseIr(comparison, kV26Smooth);

    if (status) status(L"Applying Tone Match correction to Block B...");
    CorrectiveIrStats correctionStats;
    // Refinement intentionally uses 0 dB post gain; the historical manual
    // Corrective IR path keeps its own -6 dB default.
    if (!applyCorrectiveIrToClo(inputClo2048, ir, outputClo2048, correctionStats, error, 0.0)) return false;

    if (status) status(L"CLO refinement complete. Tone Match correction applied.");
    return true;
}

} // namespace ntc
