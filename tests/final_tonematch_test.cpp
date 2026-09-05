// Portable synthetic test. This does not validate NAMCore, Windows UI or hardware.
#include "../src/clo_refiner.cpp"
#include <iostream>
#include <random>
#include <stdexcept>

namespace ntc {
std::string pathToUtf8(const fs::path& p) { return p.string(); }
bool readFileBytes(const fs::path& p,std::vector<std::uint8_t>& out,std::string& error) {
    std::ifstream f(p,std::ios::binary|std::ios::ate);
    if(!f){error="read failed";return false;}auto n=f.tellg();if(n<0)return false;
    out.resize(static_cast<std::size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(out.data()),n);return bool(f);
}
bool writeFileBytes(const fs::path& p,const std::uint8_t* b,std::size_t n,std::string& error) {
    std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b),static_cast<std::streamsize>(n));
    if(!f){error="write failed";return false;}return true;
}
}
void require(bool v,const char* text){if(!v)throw std::runtime_error(text);}
void put32(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t v){for(int i=0;i<4;++i)b[p+i]=static_cast<std::uint8_t>(v>>(8*i));}
void putf(std::vector<std::uint8_t>& b,std::size_t p,float v){std::uint32_t u;std::memcpy(&u,&v,4);put32(b,p,u);}
void putd(std::vector<std::uint8_t>& b,std::size_t p,double v){std::uint64_t u;std::memcpy(&u,&v,8);for(int i=0;i<8;++i)b[p+i]=static_cast<std::uint8_t>(u>>(8*i));}
std::uint16_t crc(const std::vector<std::uint8_t>& b,std::size_t end){std::uint16_t c=65535;for(std::size_t i=12;i<end;++i){c^=b[i];for(int k=0;k<8;++k)c=(c&1)?(c>>1)^0xa001:c>>1;}return c;}
void wav(const std::filesystem::path& path,const std::vector<float>& samples){
    std::vector<std::uint8_t>b(44+4*samples.size());std::memcpy(b.data(),"RIFF",4);put32(b,4,static_cast<std::uint32_t>(b.size()-8));
    std::memcpy(b.data()+8,"WAVEfmt ",8);put32(b,16,16);b[20]=3;b[22]=1;put32(b,24,44100);put32(b,28,176400);b[32]=4;b[34]=32;
    std::memcpy(b.data()+36,"data",4);put32(b,40,static_cast<std::uint32_t>(samples.size()*4));for(std::size_t i=0;i<samples.size();++i)putf(b,44+4*i,samples[i]);
    std::string e;require(ntc::writeFileBytes(path,b.data(),b.size(),e),"write WAV");
}
int main(int argc,char** argv)try {
    namespace fs=std::filesystem;
    const fs::path dir=argc>1?argv[1]:"test-output";fs::create_directories(dir);
    std::string error;
    // Confidence affects the curve itself. Confidence=0 gives an identity IR.
    ntc::V26Comp comp;comp.f={40,18000};comp.raw={6,6};comp.conf={0,0};
    auto raw=ntc::v26minPhaseIr(comp,false),weighted=ntc::v26minPhaseIr(comp,true);
    require(std::abs(raw[0]-std::pow(10.,6./20.))<1e-4,"raw curve changed");
    require(std::abs(weighted[0]-1)<1e-5,"confidence not applied");
    std::vector<float> base(512,0),candidate;base[0]=1;
    require(ntc::correctedFinalB(base,{2.f},candidate),"gain correction invalid");
    require(candidate==base,"historical B normalization changed");
    require(!ntc::correctedFinalB(base,{std::numeric_limits<float>::infinity()},candidate),"non-finite accepted");
    std::vector<float> s(32768,.1f),t(32768,.1f);
    auto windows=ntc::sharedWindows(s,t);require(!windows.empty(),"window selection");
    std::fill(s.begin(),s.end(),0.f);
    require(ntc::v26analyse(s,1,0,s.size(),&windows).frames==windows.size(),"silent candidate escaped scoring");

    std::vector<std::uint8_t> inputClo(0x2288);std::memcpy(inputClo.data(),"VTSI",4);
    put32(inputClo,4,0x2288);put32(inputClo,0x14,0x2200);put32(inputClo,0x7c,128);put32(inputClo,0x80,128);put32(inputClo,0x84,2048);
    putd(inputClo,0x18,1);putd(inputClo,0x40,1);for(auto p:{0x68,0x6c,0x70,0x74,0x88,0x288})putf(inputClo,p,1);
    // Strong tap outside BOTH destination budgets must not survive the export.
    putf(inputClo,0x288+1500*4,.7f);
    auto c=crc(inputClo,inputClo.size());inputClo[8]=c>>8;inputClo[9]=c&255;
    require(ntc::writeFileBytes(dir/"source2048.clo",inputClo.data(),inputClo.size(),error),"source write");
    ntc::Model model;require(ntc::parseModel(inputClo,model,error),"parse model");
    constexpr std::size_t n=70u*44100u;
    std::vector<float> input(n);std::mt19937 rng(341);
    for(std::size_t i=0;i<n;++i)input[i]=(static_cast<double>(rng())/rng.max()-.5)*.25;
    auto aout=ntc::precomputeA(model,input,n,ntc::cloPlayerGainControlToLinear(50));
    std::vector<float> preB,target;ntc::renderPreB(model,aout,1,1,1,1,preB);
    std::vector<float> targetB(1600,0);
    if(argc>2){targetB[0]=.2f;targetB[900]=std::sqrt(.96f);}
    else{targetB[0]=std::sqrt(.96f);targetB[5]=.2f;}
    ntc::renderWithB(preB,targetB,target,ntc::cloPlayerVolumeControlToLinear(50));
    // Deliberately extreme guard samples: they must never influence 50-70s analysis.
    input.resize(n+600,123.f);target.resize(n+600,-123.f);wav(dir/"stimulus.wav",input);wav(dir/"target.wav",target);
    for(auto dest:{ntc::CloDestination::Gp200,ntc::CloDestination::Gp5}) {
        ntc::CloRefineConfig cfg;cfg.destination=dest;ntc::CloRefineStats stats;
        const auto taps=ntc::destinationBTaps(dest);auto output=dir/("final"+std::to_string(taps)+".clo");
        require(ntc::refineCloBOnly(dir/"source2048.clo",dir/"stimulus.wav",dir/"target.wav",output,cfg,error,{},&stats),error.c_str());
        require(std::isfinite(stats.withoutConfidenceRmseDb)&&std::isfinite(stats.withConfidenceRmseDb),"candidate evaluation invalid");
        require(stats.selectedConfidence==(stats.withConfidenceRmseDb<stats.withoutConfidenceRmseDb),"wrong winner");
        std::vector<std::uint8_t> bytes;require(ntc::readFileBytes(output,bytes,error),"read output");
        const auto declared=0x88+4*(128+taps);
        require(bytes.size()==(taps==1024?0x2288:declared),"physical size");
        require(ntc::le32(bytes.data()+4)==declared&&ntc::le32(bytes.data()+0x14)==4*(128+taps),"header size");
        require(ntc::le32(bytes.data()+0x84)==taps&&ntc::le32(bytes.data()+0x7c)==128,"tap counts");
        require(std::equal(bytes.begin()+0x18,bytes.begin()+0x78,inputClo.begin()+0x18),"pre/post/PK changed");
        require(std::equal(bytes.begin()+0x88,bytes.begin()+0x288,inputClo.begin()+0x88),"A changed");
        require(std::all_of(bytes.begin()+declared,bytes.end(),[](auto v){return v==0;}),"padding not zero");
        const auto cc=crc(bytes,declared);require(bytes[8]==cc>>8&&bytes[9]==(cc&255),"CRC byte order");
        ntc::Model finalModel;require(ntc::parseModel(bytes,finalModel,error),"final parse");
        // Re-evaluate the serialized file, independently of the chosen candidate buffer.
        std::vector<float> outputAudio;ntc::renderWithB(preB,finalModel.B,outputAudio,ntc::cloPlayerVolumeControlToLinear(50));
        std::vector<float> actualTail(outputAudio.begin()+50u*44100u,outputAudio.end());
        std::vector<float> expectedTail(target.begin()+50u*44100u,target.begin()+n);
        std::vector<float> baseAudio;ntc::renderWithB(preB,base,baseAudio,ntc::cloPlayerVolumeControlToLinear(50));
        std::vector<float> baseTail(baseAudio.begin()+50u*44100u,baseAudio.end());auto fixed=ntc::sharedWindows(baseTail,expectedTail);
        const double score=ntc::spectralRmse(ntc::v26analyse(actualTail,1,0,actualTail.size(),&fixed),ntc::v26analyse(expectedTail,1,0,expectedTail.size(),&fixed));
        require(std::abs(score-std::min(stats.withoutConfidenceRmseDb,stats.withConfidenceRmseDb))<1e-5,"export differs from evaluated winner");
        std::cout<<"B"<<taps<<" baseline="<<stats.baselineRmseDb<<" raw="<<stats.withoutConfidenceRmseDb<<" confidence="<<stats.withConfidenceRmseDb<<" selected="<<(stats.selectedConfidence?"confidence":"raw")<<" serialized="<<score<<" frames="<<stats.analysisFrames<<"\n";
    }
    ntc::CloRefineConfig cfg;ntc::CloRefineStats stats;
    wav(dir/"short.wav",std::vector<float>(100,0));
    require(!ntc::refineCloBOnly(dir/"source2048.clo",dir/"short.wav",dir/"target.wav",dir/"must_not_exist.clo",cfg,error),"short input accepted");
    require(!fs::exists(dir/"must_not_exist.clo"),"failed job wrote output");
    std::cout<<"PASS: confidence, scoring, final sizes, CRC, preserved A/PK/biquads, guards, invalid input.\n";
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
