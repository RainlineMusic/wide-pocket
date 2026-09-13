/*
    Wide Pocket - single Natural vocal widening engine.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.
*/
#pragma once
#include "DelayLine.h"
#include "Guards.h"
#include "MidSideRenderer.h"
#include "StftDecorrelator.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"
#include <array>
namespace wp {
class WidePocketEngine {
public:
 static constexpr int numBands=VocalAnalyzer::numMaskBands;
 void prepare(double sr,int){sampleRate=std::max(8000.0,sr);analyzer.prepare(sampleRate);decorrelator.prepare(sampleRate,8);latencySamples=decorrelator.getLatencySamples();midDelay.prepare(latencySamples+8);sideDelay.prepare(latencySamples+8);midDelay.setDelay(latencySamples);sideDelay.setDelay(latencySamples);renderer.prepare(sampleRate);correlationGuard.prepare(sampleRate);outputGainSmoother.reset(sampleRate,20,dbToGain(parameters.outputDb));guardSmoother.reset(sampleRate,90,1);reset();}
 void reset(){analyzer.reset();decorrelator.reset();midDelay.reset();sideDelay.reset();renderer.reset(1,1);correlationGuard.reset();guardSmoother.snapTo(1);snapshot=AnalyzerFrame{};bandWidths.fill(0);}
 void setParameters(const Parameters&p)noexcept{parameters=p;outputGainSmoother.setTarget(dbToGain(clampf(parameters.outputDb,-24,12)));float stability=clamp01(parameters.stability*.01f);decorrelator.setGainSmoothingMs(lerp(180.f,520.f,stability));guardSmoother.setTime(sampleRate,lerp(70.f,160.f,stability));}
 int getLatencySamples()const noexcept{return latencySamples;} AnalyzerFrame getAnalyzerSnapshot()const noexcept{return snapshot;} std::array<float,numBands>getBandWidths()const noexcept{return bandWidths;}
 void process(float*l,float*r,int n)noexcept{for(int i=0;i<n;++i){auto ms=encodeMidSide(sanitise(l[i]),sanitise(r[i]));if(analyzer.pushSample(ms.mid))updatePerHop();float generated=decorrelator.process(ms.mid);float mid=midDelay.process(ms.mid);float side=sideDelay.process(ms.side)+guardSmoother.next()*generated;renderer.setTargets(1,1);auto out=renderer.render(mid,side);correlationGuard.measure(out.left,out.right);float gain=outputGainSmoother.next();l[i]=sanitise(out.left*gain);r[i]=sanitise(out.right*gain);}snapshot.correlation=correlationGuard.getCorrelation();snapshot.appliedWidth=clamp01(averageBandGain*guardSmoother.value());}
private:
 void updatePerHop()noexcept{const auto&f=analyzer.getFrame();snapshot=f;float width=clamp01(parameters.width*.01f),focus=clamp01(parameters.focus*.01f),air=clamp01(parameters.air*.01f);float base=.95f*std::pow(width,.82f),sum=0,weights=0;for(int b=0;b<numBands;++b){float c=bandCentreHz(b);float presence=std::exp(-std::pow(std::log2(c/1100.f)/1.7f,2.f));float high=clamp01(std::log2(std::max(c,3500.f)/3500.f)/1.8f);float focusGain=1-.42f*focus*presence,airGain=1+.38f*air*high;/* Still no hard cutoff, but the ramp now starts at 120 Hz and reaches full width an octave later: the measured male take had only 3 dB less Side than dry between 80 and 160 Hz, which is what read as low end distortion. */float p=clamp01(std::log2(std::max(c,120.f)/120.f)/2.2f);float low=lerp(.02f,1.f,p*p*(3-2*p));float gain=clampf(base*focusGain*airGain*low,0,StftDecorrelator::maxBandGain);bandWidths[b]=gain;float w=c<6000?1:.5f;sum+=gain*w;weights+=w;}decorrelator.setBandGains(bandWidths);averageBandGain=sum/std::max(1.f,weights);float sg=1-.38f*clamp01(parameters.sibilanceGuard*.01f)*f.sibilance;float tg=1-.55f*clamp01(parameters.transientFocus*.01f)*f.transient;float pg=1-.45f*f.plosive;guardSmoother.setTarget(clampf(sg*tg*pg,.18f,1.f));}
 static float bandCentreHz(int b)noexcept{return VocalAnalyzer::bandLowEdgeHz(b)*std::pow(2.f,.375f);}
 double sampleRate=48000;int latencySamples=256;Parameters parameters;VocalAnalyzer analyzer;StftDecorrelator decorrelator;DelayLine midDelay,sideDelay;MidSideRenderer renderer;CorrelationGuard correlationGuard;Smoother guardSmoother,outputGainSmoother;float averageBandGain=0;AnalyzerFrame snapshot;std::array<float,numBands>bandWidths{};
};}
