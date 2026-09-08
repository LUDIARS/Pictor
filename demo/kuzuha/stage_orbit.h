#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pictor_kuzuha {
// A reusable normalized reference curve; imported once, allocation-free sampling.
class StageOrbit {
public:
    /// @implements SPEC-PC-KUZUHA-STAGE
    explicit StageOrbit(const std::string& path) {
        if(path.empty()) return;
        std::ifstream input(path);
        if(!input) throw std::runtime_error("Cannot read stage orbit curve");
        float time,value;
        while(input>>time) {
            if(!(input>>value)) throw std::runtime_error("Incomplete stage orbit key");
            if(!std::isfinite(time)||!std::isfinite(value)||time<0||std::abs(value)>2||
               (!keys_.empty() && time<=keys_.back()[0]))
                throw std::runtime_error("Invalid stage orbit key");
            keys_.push_back({time,value});
        }
        if(!input.eof() || keys_.size()<2 || keys_.front()[0]!=0 || keys_.back()[0]<=0)
            throw std::runtime_error("Invalid stage orbit curve");
    }
    /// @implements SPEC-PC-KUZUHA-STAGE
    float sample(float seconds) const {
        if(keys_.empty()) return std::sin(seconds*.34f);
        const float time=std::clamp(seconds/18.f,0.f,1.f)*keys_.back()[0];
        const auto upper=std::upper_bound(keys_.begin(),keys_.end(),time,
            [](float t,const auto& key){return t<key[0];});
        if(upper==keys_.begin()) return upper->at(1);
        if(upper==keys_.end()) return keys_.back()[1];
        const auto& a=*(upper-1);const auto& b=*upper;
        return a[1]+(b[1]-a[1])*(time-a[0])/(b[0]-a[0]);
    }
private:
    std::vector<std::array<float,2>> keys_;
};
}
