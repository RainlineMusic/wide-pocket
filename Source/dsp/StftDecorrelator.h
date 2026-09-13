/*
    Wide Pocket - natural subband decorrelator.
    Copyright (c) 2026 Rainline Music. All Rights Reserved.
*/
#pragma once
#include "Fft.h"
#include "VocalAnalyzer.h"
#include "WidePocketTypes.h"
#include <array>
#include <complex>
#include <vector>
namespace wp {
class StftDecorrelator {
public:
 static constexpr int numBands=VocalAnalyzer::numMaskBands;
 static constexpr float maxBandGain=1.25f;
 void prepare(double sr,int=8){sampleRate=std::max(8000.0,sr);fftSize=256;hopSize=128;numBins=129;fft.setOrder(8);window.resize(fftSize);for(int i=0;i<fftSize;++i)window[i]=std::sin((float)kPi*((float)i+.5f)/(float)fftSize);inputFrame.assign(fftSize,0);workFrame.assign(fftSize,0);overlap.assign(fftSize,0);pending.assign(hopSize,0);directSpectrum.assign(numBins,{});processedSpectrum.assign(numBins,{});directEnergy.assign(numBins,0);processedEnergy.assign(numBins,0);binBand.assign(numBins,0);preDelay.assign(preDelayFrames*numBins,{});const int d[numStages]={1,2,3,5};for(int s=0;s<numStages;++s){stages[s].delay=d[s];stages[s].xDelay.assign(d[s]*numBins,{});stages[s].yDelay.assign(d[s]*numBins,{});}buildBandTable();double fr=sampleRate/hopSize;for(auto&x:bandGain)x.reset(fr,240,0);for(auto&x:energyMatchSmoother)x.reset(fr,90,1);reset();}
 void reset()noexcept{std::fill(inputFrame.begin(),inputFrame.end(),0);std::fill(overlap.begin(),overlap.end(),0);std::fill(pending.begin(),pending.end(),0);std::fill(preDelay.begin(),preDelay.end(),std::complex<float>{});std::fill(directEnergy.begin(),directEnergy.end(),0);std::fill(processedEnergy.begin(),processedEnergy.end(),0);for(auto&s:stages){std::fill(s.xDelay.begin(),s.xDelay.end(),std::complex<float>{});std::fill(s.yDelay.begin(),s.yDelay.end(),std::complex<float>{});s.write=0;}for(auto&x:energyMatchSmoother)x.snapTo(1);fill=0;pendingIndex=hopSize;preWrite=0;previousTransientEnergy=smoothedTransientEnergy=1e-9f;holdFrames=inhibitFrames=0;transientActive=false;}
 void setBandGains(const std::array<float,numBands>&g)noexcept{for(int b=0;b<numBands;++b)bandGain[b].setTarget(clampf(g[b],0,maxBandGain));}
 void setUniformGain(float g)noexcept{for(auto&x:bandGain)x.setTarget(clampf(g,0,maxBandGain));}
 void setGainSmoothingMs(float ms)noexcept{for(auto&x:bandGain)x.setTime(sampleRate/hopSize,clampf(ms,120,600));}
 float process(float x)noexcept{inputFrame[fill++]=sanitise(x);if(fill==fftSize)processFrame();return pendingIndex<hopSize?sanitise(pending[pendingIndex++]):0;}
 int getLatencySamples()const noexcept{return fftSize;}int getFrameSize()const noexcept{return fftSize;}int getHopSize()const noexcept{return hopSize;}bool isTransientActive()const noexcept{return transientActive;}
private:
 struct AllpassStage{int delay=1,write=0;std::vector<std::complex<float>>xDelay,yDelay;};
 void processFrame()noexcept{
  for(int i=0;i<fftSize;++i)workFrame[i]=inputFrame[i]*window[i];fft.forwardReal(workFrame.data(),directSpectrum.data());updateTransientState();
  for(int b=0;b<numBins;++b){auto d=preDelay[preWrite*numBins+b];preDelay[preWrite*numBins+b]=transientActive?std::complex<float>{}:directSpectrum[b];processedSpectrum[b]=d;}preWrite=(preWrite+1)%preDelayFrames;
  for(auto&s:stages){for(int b=0;b<numBins;++b){size_t i=(size_t)s.write*numBins+b;auto x=processedSpectrum[b];auto y=gamma*x+s.xDelay[i]-gamma*s.yDelay[i];s.xDelay[i]=x;s.yDelay[i]=y;processedSpectrum[b]=y;}s.write=(s.write+1)%s.delay;}
  std::array<float,numBands>g{};for(int b=0;b<numBands;++b)g[b]=bandGain[b].next();std::array<double,numBands>cross{},de{},pe{};
  for(int b=1;b<numBins-1;++b){auto x=directSpectrum[b];auto y=processedSpectrum[b];float dn=std::norm(x),pn=std::norm(y);auto&d=directEnergy[b];auto&p=processedEnergy[b];d=envelopeAlpha*dn+(1-envelopeAlpha)*d;p=envelopeAlpha*pn+(1-envelopeAlpha)*p;float eg=1;if(p>envelopeBeta*d)eg=std::sqrt(envelopeBeta*d/std::max(p,1e-12f));else if(d>envelopeBeta*p)eg=std::sqrt(d/std::max(envelopeBeta*p,1e-12f));y*=clampf(eg,.55f,1.8f);float hz=(float)b*(float)sampleRate/(float)fftSize;float q=quadratureFloor*clampf((hz-100)/1200,.15f,1);y+=std::complex<float>(-q*x.imag(),q*x.real());processedSpectrum[b]=y;int band=binBand[b];cross[band]+=(double)(x.real()*y.real()+x.imag()*y.imag());de[band]+=(double)std::norm(x);}
  for(int b=1;b<numBins-1;++b){int band=binBand[b];float proj=clampf((float)(cross[band]/std::max(1e-12,de[band])),-1.5f,1.5f);processedSpectrum[b]-=proj*directSpectrum[b];pe[band]+=(double)std::norm(processedSpectrum[b]);}
  std::array<float,numBands>em{};for(int b=0;b<numBands;++b){float target=clampf((float)std::sqrt(de[b]/std::max(1e-12,pe[b])),.55f,1.45f);energyMatchSmoother[b].setTarget(target);em[b]=energyMatchSmoother[b].next();}
  processedSpectrum[0]={};processedSpectrum[numBins-1]={};for(int b=1;b<numBins-1;++b){int band=binBand[b];processedSpectrum[b]*=(transientActive?0.f:1.f)*g[band]*em[band];}fft.inverseReal(processedSpectrum.data(),workFrame.data());for(int i=0;i<fftSize;++i)overlap[i]+=workFrame[i]*window[i];std::copy(overlap.begin(),overlap.begin()+hopSize,pending.begin());pendingIndex=0;std::copy(overlap.begin()+hopSize,overlap.end(),overlap.begin());std::fill(overlap.end()-hopSize,overlap.end(),0);std::copy(inputFrame.begin()+hopSize,inputFrame.end(),inputFrame.begin());fill=fftSize-hopSize;
 }
 void updateTransientState()noexcept{float e=0;for(int b=4;b<numBins;++b)e+=std::norm(directSpectrum[b]);previousTransientEnergy=smoothedTransientEnergy;smoothedTransientEnergy=.4f*e+.6f*smoothedTransientEnergy;bool onset=e>1e-8f&&smoothedTransientEnergy>2.8f*previousTransientEnergy;if(onset&&inhibitFrames==0){holdFrames=8;inhibitFrames=56;}if(holdFrames>0)--holdFrames;if(inhibitFrames>0)--inhibitFrames;transientActive=holdFrames>0;}
 void buildBandTable(){float bh=(float)sampleRate/fftSize;for(int b=0;b<numBins;++b){float hz=std::max(60.f,b*bh);int band=(int)std::floor(std::log2(hz/60)/.75f);binBand[b]=std::max(0,std::min(numBands-1,band));}}
 static constexpr int numStages=4,preDelayFrames=4;static constexpr float gamma=.7f,envelopeAlpha=.4f,envelopeBeta=1.5f,quadratureFloor=.055f;
 double sampleRate=48000;int fftSize=256,hopSize=128,numBins=129;Fft fft;std::vector<float>window,inputFrame,workFrame,overlap,pending;std::vector<std::complex<float>>directSpectrum,processedSpectrum,preDelay;std::vector<float>directEnergy,processedEnergy;std::vector<int>binBand;std::array<AllpassStage,numStages>stages{};std::array<Smoother,numBands>bandGain{},energyMatchSmoother{};int fill=0,pendingIndex=0,preWrite=0;float previousTransientEnergy=1e-9f,smoothedTransientEnergy=1e-9f;int holdFrames=0,inhibitFrames=0;bool transientActive=false;
};}
