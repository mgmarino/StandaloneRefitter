//___________________________________________________
//
// Fit for scintillation and u-wire magnitudes in a noise-tolerant fashion.
// For more information, please see the slides and derivation from the Energy Meeting
// on July 22, 2013:
// https://confluence.slac.stanford.edu/display/exo/Energy+Meeting+22+July+2013
// (A full note will be written up in the near future.)
//
// Limitations:
// * Although this algorithm can be extended to handle events with more than one scintillation cluster,
//   for simplicity it does not currently do so.
// * The APD gains may not be right, which results in a suboptimal energy resolution.
// * In principle we could extract cluster-by-cluster light yield down the road.
// * It should be fairly cheap to also extract the estimated fit error due to electronic and Poisson noise.
//

#include "EXOAnalysisManager/EXORefitSignals.hh"
#include "EXOAnalysisManager/EXOGridCorrectionModule.hh"
#include "EXOCalibUtilities/EXOChannelMapManager.hh"
#include "EXOCalibUtilities/EXOElectronicsShapers.hh"
#include "EXOCalibUtilities/EXOUWireGains.hh"
#include "EXOCalibUtilities/EXOLifetimeCalib.hh"
#include "EXOCalibUtilities/EXOGridCorrectionCalib.hh"
#include "EXOUtilities/EXOTalkToManager.hh"
#include "EXOUtilities/EXONoiseCorrelations.hh"
#include "EXOUtilities/EXOEventData.hh"
#include "EXOUtilities/EXODigitizeWires.hh"
#include "EXOUtilities/EXOWaveform.hh"
#include "EXOUtilities/EXOWaveformFT.hh"
#include "EXOUtilities/EXOFastFourierTransformFFTW.hh"
#include "EXOUtilities/EXOTransferFunction.hh"
#include "EXOUtilities/EXOMiscUtil.hh"
#include "EXOUtilities/EXOUWireSignal.hh"
#include "TFile.h"
#include "TArrayI.h"
#include "TH3D.h"
#include "TGraph.h"
#include "TMatrixD.h"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cassert>

// Declarations necessary to use dgemm from a BLAS library.
enum CBLAS_ORDER {CblasRowMajor=101, CblasColMajor=102};
enum CBLAS_TRANSPOSE {CblasNoTrans=111, CblasTrans=112, CblasConjTrans=113};
#ifdef HAVE_BLAS
extern "C"
#endif
void cblas_dgemm(const CBLAS_ORDER Order, const CBLAS_TRANSPOSE TransA,
                 const CBLAS_TRANSPOSE TransB, const int M, const int N,
                 const int K, const double alpha, const double *A,
                 const int lda, const double *B, const int ldb,
                 const double beta, double *C, const int ldc)
#ifdef HAVE_BLAS
; // Just a declaration -- use the optimized BLAS library provided.
#else
{
  // Provide my own implementation when no optimized BLAS is available.
  // This might be inefficient, depending on how clever the compiler is.
  // Note also that I don't do any checking of the arguments.

  // Loop through entries in C.
  for(int row = 0; row < M; row++) {
    for(int col = 0; col < N; col++) {
      size_t C_index;
      if(Order == CblasRowMajor) C_index = row*ldc + col;
      else C_index = col*ldc + row;

      size_t A_index, A_stride;
      if((Order == CblasRowMajor and TransA == CblasNoTrans) or
         (Order == CblasColMajor and TransA != CblasNoTrans)) {
        A_index = row*lda;
        A_stride = 1;
      }
      else /* (Order == CblasRowMajor and TransA != CblasNoTrans) or
              (Order == CblasColMajor and TransA == CblasNoTrans)*/ {
        A_index = row;
        A_stride = lda;
      }

      size_t B_index, B_stride;
      if((Order == CblasRowMajor and TransB == CblasNoTrans) or
         (Order == CblasColMajor and TransB != CblasNoTrans)) {
        B_index = col;
        B_stride = ldb;
      }
      else /* (Order == CblasRowMajor and TransB != CblasNoTrans) or
              (Order == CblasColMajor and TransB == CblasNoTrans)*/ {
        B_index = col*ldb;
        B_stride = 1;
      }

      double acc = 0;
      for(int i = 0; i < K; i++) {
        acc += A[A_index] * B[B_index];
        A_index += A_stride;
        B_index += B_stride;
      }
      C[C_index] *= beta;
      C[C_index] += alpha * acc;
    } // End loop over columns of C
  } // End loop over rows of C
}
#endif

IMPLEMENT_EXO_ANALYSIS_MODULE(EXORefitSignals, "refit-signals")

int EXORefitSignals::TalkTo(EXOTalkToManager* tm)
{
  tm->CreateCommand("/refit-signals/lightfile",
                    "The root file containing the lightmap and gainmap.",
                    this,
                    fLightmapFilename,
                    &EXORefitSignals::SetLightmapFilename);
  tm->CreateCommand("/refit-signals/noisefile",
                    "The root file containing the relevant noise correlations.",
                    this,
                    fNoiseFilename,
                    &EXORefitSignals::SetNoiseFilename);
  tm->CreateCommand("/refit-signals/termination_thresh",
                    "Terminate when |r| < threshold",
                    this,
                    fRThreshold,
                    &EXORefitSignals::SetRThreshold);
  return 0;
}

void EXORefitSignals::FillNoiseCorrelations(const EXOEventData& ED)
{
  // Reorganize the noise matrix entries for fast use in matrix-vector multiplication.
  // This depends on the set of available waveforms; but we assume that this set doesn't
  // change much.  Compare to the static set in this function.
  // For the future, might consider whether we can rearrange EXONoiseCorrelations to be
  // more convenient for this.

  // Get the channel map.
  const EXOChannelMap& ChannelMap = GetChanMapForHeader(ED.fEventHeader);

  // Construct the set of channels to keep.
  std::vector<unsigned char> ChannelsToUse;
  for(unsigned char i = 0; i < NUMBER_READOUT_CHANNELS; i++) {
    if(EXOMiscUtil::TypeOfChannel(i) == EXOMiscUtil::kVWire) continue; // No v wires for now.
    if(ChannelMap.channel_suppressed_by_daq(i) or not ChannelMap.good_channel(i)) continue;
    if(ED.GetWaveformData()->GetWaveformWithChannel(i) == NULL) continue;
    ChannelsToUse.push_back(i);
  }

  // If the channel mapping is unchanged, do nothing.
  if(ChannelsToUse == fChannels) return;

  // Else, we'll need to extract the noise information to match the new ordering.
  // Note that we store the same-frequency blocks in column-major order,
  // to simplify GEMM calls (if BLAS is provided).
  fChannels = ChannelsToUse;
  fNoiseCorrelations.resize(fMaxF - fMinF + 1);
  TFile* NoiseFile = TFile::Open(fNoiseFilename.c_str());
  EXONoiseCorrelations* NoiseCorr =
    dynamic_cast<EXONoiseCorrelations*>(NoiseFile->Get("EXONoiseCorrelations"));
  for(size_t f = fMinF; f <= fMaxF; f++) {
    bool IsFullBlock = (f != fMaxF);
    std::vector<double>& block = fNoiseCorrelations[f-fMinF];
    block.resize(fChannels.size()*fChannels.size() * (IsFullBlock ? 4 : 1));

    // Iterate through column pairs.
    for(size_t index1 = 0; index1 < fChannels.size(); index1++) {
      unsigned char noiseIndex1 = NoiseCorr->GetIndexOfChannel(fChannels[index1]);
      size_t ColPos = index1*fChannels.size()*(IsFullBlock ? 4 : 1);

      // Start with the real column.
      for(size_t index2 = 0; index2 < fChannels.size(); index2++) {
        unsigned char noiseIndex2 = NoiseCorr->GetIndexOfChannel(fChannels[index2]);
        size_t RowPos = ColPos + index2*(IsFullBlock ? 2 : 1);

        // real row.
        block[RowPos] = NoiseCorr->GetRR(f)[noiseIndex2][noiseIndex1];

        // imag row.
        if(IsFullBlock) block[RowPos+1] = NoiseCorr->GetRI(f)[noiseIndex1][noiseIndex2];
      }

      // Now the imag column.
      if(IsFullBlock) {
        ColPos += fChannels.size()*(IsFullBlock ? 2 : 1);
        for(size_t index2 = 0; index2 < fChannels.size(); index2++) {
          unsigned char noiseIndex2 = NoiseCorr->GetIndexOfChannel(fChannels[index2]);
          size_t RowPos = ColPos + index2*(IsFullBlock ? 2 : 1);

          // real row.
          block[RowPos] = NoiseCorr->GetRI(f)[noiseIndex2][noiseIndex1];

          // imag row.
          if(IsFullBlock) block[RowPos+1] = NoiseCorr->GetII(f)[noiseIndex2][noiseIndex1];
        }
      }
    } // End loop over column pairs (index1).
  } // End loop over frequencies.  fNoiseCorrelations is initialized.

  // Cleanup -- this should do it.
  delete NoiseFile;

  // For convenience, pre-store the index where APDs start in fChannels.
  for(size_t i = 0; i < fChannels.size(); i++) {
    if(EXOMiscUtil::TypeOfChannel(fChannels[i]) == EXOMiscUtil::kAPDGang) {
      fFirstAPDChannelIndex = i;
      break;
    }
  }
}

int EXORefitSignals::Initialize()
{
  // Open the lightmap file, and extract their information.
  // Create unshaped wire drift waveforms.
  // Also initialize our various timers.

#ifndef HAVE_BLAS
  LogEXOMsg("You are not using an optimized BLAS -- performance may suffer.", EEWarning);
#endif

  std::string FullLightmapFilename = EXOMiscUtil::SearchForFile(fLightmapFilename);
  if(FullLightmapFilename == "") LogEXOMsg("Failed to find lightmap file.", EEAlert);
  TFile* LightmapFile = TFile::Open(FullLightmapFilename.c_str());

  // Get the list of active APDs.
  TArrayI* APDs = (TArrayI*)LightmapFile->GetObjectUnchecked("APDs");
  for(Int_t i = 0; i < APDs->GetSize(); i++) {
    Int_t gang = APDs->At(i);
    // Get the lightmaps.
    std::ostringstream lightmapname;
    lightmapname << "lightmap_" << std::setw(3) << std::setfill('0') << gang;
    std::string old_lightmap = lightmapname.str();
    std::string new_lightmap = old_lightmap + "_clone";
    fLightMaps[gang] = (TH3D*)LightmapFile->Get(old_lightmap.c_str())->Clone(new_lightmap.c_str());

    // Get the gainmaps.
    std::ostringstream gainmapname;
    gainmapname << "gainmap_" << std::setw(3) << std::setfill('0') << gang;
    std::string old_gainmap = gainmapname.str();
    std::string new_gainmap = old_gainmap + "_clone";
    fGainMaps[gang] = (TGraph*)LightmapFile->Get(old_gainmap.c_str())->Clone(new_gainmap.c_str());
  }
  delete LightmapFile;

  // Create wire drift waveforms.
  // The results will be high-bandpass waveforms, with the deposit occurring at 256us.
  // The whole waveform has a length of 512us.
  EXODigitizeWires dig;
  dig.set_drift_velocity(0.171*CLHEP::cm/CLHEP::microsecond);
  dig.set_collection_drift_velocity(0.2041*CLHEP::cm/CLHEP::microsecond);
  dig.set_trigger_time(256*CLHEP::microsecond); // Shift later so deposit is at 256us.
  dig.set_nsample(512);
  EXOCoordinates coord(EXOMiscUtil::kUVCoordinates, 0.25*CHANNEL_WIDTH, 0, 100*CLHEP::mm, 0);
  EXOMCPixelatedChargeDeposit pixel;
  pixel.SetCoordinates(coord);
  pixel.fTotalEnergy = pixel.fTotalIonizationEnergy = 1; // 1 MeV -- we'll rescale this.
  dig.GenerateUnshapedSignals(&pixel);
  fWireDeposit = dig.GetDdata(NCHANNEL_PER_WIREPLANE/2);
  fWireInduction = dig.GetDdata(NCHANNEL_PER_WIREPLANE/2-1);
  fWireInduction += dig.GetDdata(NCHANNEL_PER_WIREPLANE/2+1);
  fWireInduction /= 2;

  // Shift to move deposit time to 256us.
  size_t ShiftNeeded = size_t(pixel.fWireHitTime/SAMPLE_TIME_HIGH_BANDWIDTH) - 256*BANDWIDTH_FACTOR;
  for(size_t i = 0; i < fWireDeposit.GetLength() - ShiftNeeded; i++) {
    fWireDeposit[i] = fWireDeposit[i+ShiftNeeded];
    fWireInduction[i] = fWireInduction[i+ShiftNeeded];
  }

  // Normalize the model waveforms -- not strictly necessary since they're unshaped, but
  // it makes it easier to modularize code later.
  double MaxVal = fWireDeposit.GetMaxValue();
  fWireDeposit /= MaxVal;
  fWireInduction /= MaxVal;

  // Initialize stopwatches too.
  fWatch_GetNoise.Reset();
  fWatch_ProcessEvent.Reset();
  fWatch_InitialGuess.Reset();
  fWatch_Solve.Reset();
  fWatch_MatrixMul.Reset();
  fWatch_MatrixMul_NoiseTerms.Reset();
  return 0;
}

EXOAnalysisModule::EventStatus EXORefitSignals::ProcessEvent(EXOEventData *ED)
{
  fWatch_ProcessEvent.Start(false);

  // Start by making sure to reset any old values to zero.
  for(size_t i = 0; i < ED->GetNumScintillationClusters(); i++) {
    ED->GetScintillationCluster(i)->fDenoisedEnergy = 0;
  }

  // If we don't have previously-established scintillation times, we can't do anything -- skip.
  if(ED->GetNumScintillationClusters() == 0) {
    fWatch_ProcessEvent.Stop();
    return kOk;
  }

  // If the waveforms aren't full-length, skip for now (although we should be able to handle them later).
  if(ED->fEventHeader.fSampleCount != 2047) {
    fWatch_ProcessEvent.Stop();
    return kDrop;
  }

  // For now, we also only deal with events containing *exactly* one scintillation cluster.
  // There's nothing theoretical that warrants this; it's just easier to code up.
  if(ED->GetNumScintillationClusters() != 1) {
    fWatch_ProcessEvent.Stop();
    return kDrop;
  }
  EXOScintillationCluster* scint = ED->GetScintillationCluster(0);

  // If necessary, extract the noise correlations object with a proper ordering.
  fWatch_GetNoise.Start(false);
  FillNoiseCorrelations(*ED);
  fWatch_GetNoise.Stop();

  // If there are no fully-reconstructed clusters, then we can't do anything -- so, skip them too.
  // Otherwise, extract a list of clusters for future convenience.
  std::vector<EXOChargeCluster*> FullClusters;
  for(size_t i = 0; i < scint->GetNumChargeClusters(); i++) {
    EXOChargeCluster* clu = scint->GetChargeClusterAt(i);
    if(std::abs(clu->fX) > 200 or std::abs(clu->fY) > 200 or std::abs(clu->fZ) > 200) continue;
    if(clu->fPurityCorrectedEnergy < 1) continue;
    FullClusters.push_back(clu);
  }
  if(FullClusters.empty()) {
    fWatch_ProcessEvent.Stop();
    return kDrop;
  }

  // Save the unix time of the event (as a double, since ROOT will convert it anyway).
  fUnixTimeOfEvent = double(ED->fEventHeader.fTriggerSeconds);
  fUnixTimeOfEvent += double(ED->fEventHeader.fTriggerMicroSeconds)/1e6;

  // Given the positions of the clusters, estimate how the light should be distributed among gangs.
  // ExpectedYieldPerGang will be the expected peak-baseline (ADC counts) of a 2615 keV event.
  fExpectedYieldPerGang.clear();
  fExpectedEnergy_keV = 0;
  //double TotalPurityCorrectedEnergy = 0;
  for(size_t i = fFirstAPDChannelIndex; i < fChannels.size(); i++) fExpectedYieldPerGang[fChannels[i]] = 0;
  for(size_t i = 0; i < FullClusters.size(); i++) {
    EXOChargeCluster* clu = FullClusters[i];
    //TotalPurityCorrectedEnergy += clu->fPurityCorrectedEnergy;
    fExpectedEnergy_keV += clu->fPurityCorrectedEnergy;
    for(size_t j = fFirstAPDChannelIndex; j < fChannels.size(); j++) {
      unsigned char gang = fChannels[j];
      Double_t GainFuncVal = fGainMaps[gang]->Eval(fUnixTimeOfEvent);

      // Make sure cluster is in the proper range for interpolation -- else return 0.
      Double_t LightMapVal;
      TAxis* Xaxis = fLightMaps[gang]->GetXaxis();
      TAxis* Yaxis = fLightMaps[gang]->GetYaxis();
      TAxis* Zaxis = fLightMaps[gang]->GetZaxis();
      if(Xaxis->GetBinCenter(1) <= clu->fX and clu->fX < Xaxis->GetBinCenter(Xaxis->GetNbins()) and
         Yaxis->GetBinCenter(1) <= clu->fY and clu->fY < Yaxis->GetBinCenter(Yaxis->GetNbins()) and
         Zaxis->GetBinCenter(1) <= clu->fZ and clu->fZ < Zaxis->GetBinCenter(Zaxis->GetNbins())) {
        LightMapVal = fLightMaps[gang]->Interpolate(clu->fX, clu->fY, clu->fZ);
      }
      else {
        // Interpolate would return 0, and I'm actually OK with that -- but I still want to kill the warning.
        LightMapVal = 0;
      }

      fExpectedYieldPerGang[gang] += LightMapVal*GainFuncVal*clu->fPurityCorrectedEnergy;
    }
  }
  // We just want to weight the clusters appropriately when we guess where light should be collected.
  // Divide out to ensure that at the end, a result of 1 corresponds to a 2615 keV event (roughly).
  for(size_t i = fFirstAPDChannelIndex; i < fChannels.size(); i++) {
    fExpectedYieldPerGang[fChannels[i]] /= fExpectedEnergy_keV;
  }

  // If we don't expect any yield, then clearly there will be a degenerate matrix.
  // So, instead drop such events.
  // (Specifically, if a 2615 keV event would produce less than 1ADC on every gang, drop it.)
  bool HasYield = false;
  for(size_t i = fFirstAPDChannelIndex; i < fChannels.size(); i++) {
    if(fExpectedYieldPerGang[fChannels[i]] > 1) HasYield = true;
  }
  if(not HasYield) {
    fWatch_ProcessEvent.Stop();
    return kDrop;
  }

  // Generate the expected light signal shape (normalized), given the time of the scintillation.
  // Alternate between real and imaginary parts, mimicking the variable ordering we use throughout.
  // Also drop the zero-frequency component (which isn't used)
  // and the last imaginary component (which is identically zero).
  EXOWaveformFT modelFT = GetModelForTime(scint->fTime);
  fmodel_realimag.resize(2*modelFT.GetLength() - 3);
  for(size_t i = 1; i < modelFT.GetLength(); i++) {
    fmodel_realimag[2*i - 2] = modelFT[i].real();
    if(i != modelFT.GetLength()-1) fmodel_realimag[2*i - 1] = modelFT[i].imag();
  }

  // Now produce the expected wire signals.
  fWireModel.clear();
  std::set<EXOUWireSignal*> UWireSignals;
  EXOElectronicsShapers* electronicsShapers = GetCalibrationFor(EXOElectronicsShapers, 
                                                                EXOElectronicsShapersHandler, 
                                                                "timevartau", 
                                                                ED->fEventHeader);
  const EXOUWireGains* GainsFromDatabase = GetCalibrationFor(EXOUWireGains,
                                                             EXOUWireGainsHandler,
                                                             "source_calibration",
                                                             ED->fEventHeader);
  if(not electronicsShapers or not GainsFromDatabase) {
    LogEXOMsg("Unable to get necessary information from DB", EEError);
    return kDrop;
  }
  for(size_t i = 0; i < FullClusters.size(); i++) {
    EXOChargeCluster* clu = FullClusters[i];
    for(size_t j = 0; j < clu->GetNumUWireSignals(); j++) {
      EXOUWireSignal* sig = clu->GetUWireSignalAt(j);
      if(sig->fIsInduction) continue;
      UWireSignals.insert(sig);
    }
  }
  for(std::set<EXOUWireSignal*>::iterator sigIt = UWireSignals.begin();
      sigIt != UWireSignals.end();
      sigIt++) {

    std::map<unsigned char, std::vector<double> > ModelForThisSignal;

    // Deposit channel.
    const EXOTransferFunction& transferDep =
      electronicsShapers->GetTransferFunctionForChannel((*sigIt)->fChannel);
    double Gain = transferDep.GetGain();
    double DepChanGain = GainsFromDatabase->GetGainOnChannel((*sigIt)->fChannel);
    ModelForThisSignal[(*sigIt)->fChannel] = MakeWireModel(fWireDeposit,
                                                           transferDep,
                                                           Gain,
                                                           (*sigIt)->fTime);

    if(EXOMiscUtil::TypeOfChannel((*sigIt)->fChannel-1) == EXOMiscUtil::kUWire) {
      const EXOTransferFunction& transferInd =
        electronicsShapers->GetTransferFunctionForChannel((*sigIt)->fChannel-1);
      double ThisChanGain = Gain * GainsFromDatabase->GetGainOnChannel((*sigIt)->fChannel-1)/DepChanGain;
      ModelForThisSignal[(*sigIt)->fChannel-1] = MakeWireModel(fWireInduction,
                                                               transferInd,
                                                               ThisChanGain,
                                                               (*sigIt)->fTime);
    }

    if(EXOMiscUtil::TypeOfChannel((*sigIt)->fChannel+1) == EXOMiscUtil::kUWire) {
      const EXOTransferFunction& transferInd =
        electronicsShapers->GetTransferFunctionForChannel((*sigIt)->fChannel+1);
      double ThisChanGain = Gain * GainsFromDatabase->GetGainOnChannel((*sigIt)->fChannel+1)/DepChanGain;
      ModelForThisSignal[(*sigIt)->fChannel+1] = MakeWireModel(fWireInduction,
                                                               transferInd,
                                                               ThisChanGain,
                                                               (*sigIt)->fTime);
    }

    fWireModel.push_back(std::make_pair(*sigIt, ModelForThisSignal));
  } // End loop over u-wire signals.

  // For convenience, store the column length we'll be dealing with.
  fColumnLength = 2*fChannels.size()*(fMaxF-fMinF) + fChannels.size() + fWireModel.size() + 1;

  // We're going to deal with solving for all estimators simultaneously.
  // This should let us exploit GEMM from an optimized BLAS, giving faster performance.
  // X is organized in column-major form as one contiguous block of memory.
  // The constraints are ordered with wire signals first, and the (one) light signal last.
  // Note that it is all filled with zeroes initially.
  fWatch_InitialGuess.Start(false);
  std::vector<double> X(fColumnLength * (fWireModel.size() + 1), 0);

  // Do a simple, but not quite crazy, initial guess for x.
  // Start with the wires.
  for(size_t i = 0; i < fWireModel.size(); i++) {
    size_t ColIndex = i*fColumnLength;
    Int_t channel = fWireModel[i].first->fChannel;
    std::vector<double>& model = fWireModel[i].second[channel];
    size_t channel_index = 0;
    while(fChannels[channel_index] != channel) {
      channel_index++;
      if(channel_index >= fChannels.size()) LogEXOMsg("Index exceeded -- why can this happen?", EEAlert);
    }

    double Normalization = std::inner_product(model.begin(), model.end(), model.begin(), double(0));
    double SumSqNoise = 0;
    for(size_t f = fMinF; f <= fMaxF; f++) {
      size_t step = (f < fMaxF ? 2 : 1);
      size_t ColIndex = step*step*fChannels.size()*channel_index;
      double RNoiseVal = fNoiseCorrelations[f-fMinF][ColIndex + step*channel_index];
      SumSqNoise += 1./(RNoiseVal*RNoiseVal);
      if(step == 2) {
        ColIndex += step*fChannels.size();
        double INoiseVal = fNoiseCorrelations[f-fMinF][ColIndex + step*channel_index + 1];
        SumSqNoise += 1./(INoiseVal*INoiseVal);
      }
    }
    Normalization *= SumSqNoise;
    for(size_t f = fMinF; f <= fMaxF; f++) {
      size_t step = (f < fMaxF ? 2 : 1);
      size_t RowIndex = ColIndex + 2*fChannels.size()*(f-fMinF);
      RowIndex += channel_index*step;
      size_t NoiseColIndex = step*step*fChannels.size()*channel_index;

      double RNoise = fNoiseCorrelations[f-fMinF][NoiseColIndex + step*channel_index];
      X[RowIndex] = model[2*(f-fMinF)]/(RNoise*RNoise*Normalization);
      if(f != fMaxF) {
        NoiseColIndex += step*fChannels.size();
        double INoise = fNoiseCorrelations[f-fMinF][NoiseColIndex + step*channel_index + 1];
        X[RowIndex+1] = model[2*(f-fMinF) + 1]/(INoise*INoise*Normalization);
      }
    }
  }
  // And then do the one light signal.
  double norm_APDmodel = std::inner_product(fmodel_realimag.begin(), fmodel_realimag.end(),
                                            fmodel_realimag.begin(), double(0));
  double SumSqYieldExpected = 0;
  for(size_t i = fFirstAPDChannelIndex; i < fChannels.size(); i++) {
    SumSqYieldExpected += std::pow(fExpectedYieldPerGang[fChannels[i]], 2);
  }
  for(size_t i = fFirstAPDChannelIndex; i < fChannels.size(); i++) {
    double ExpectedYieldOnChannel = fExpectedYieldPerGang[fChannels[i]];
    double LeadingFactor = ExpectedYieldOnChannel/(SumSqYieldExpected*norm_APDmodel);

    size_t ColIndex = fWireModel.size()*fColumnLength;
    for(size_t f = fMinF; f <= fMaxF; f++) {
      size_t RowIndex = ColIndex + 2*fChannels.size()*(f-fMinF);
      RowIndex += i*(f != fMaxF ? 2 : 1);

      X[RowIndex] = LeadingFactor*fmodel_realimag[2*(f-fMinF)];
      if(f != fMaxF) X[RowIndex+1] = LeadingFactor*fmodel_realimag[2*(f-fMinF)+1];
    }
  }
  // Note that I don't have good guesses for the lagrange multipliers.
  fWatch_InitialGuess.Stop();

  // Solve the system.
  fWatch_Solve.Start(false);
  for(size_t i = 0; i < 3; i++) {
    if(DoBiCGSTAB(X, fRThreshold)) break; // successfully converged.
    if(i == 2) LogEXOMsg("Solver failed to converge", EEWarning);
  }
  fNumEntriesSolved++;
  fWatch_Solve.Stop();

  // Collect the fourier-transformed waveforms.  Save them split into real and complex parts.
  // Skip channels which aren't included in our noise or lightmap models, but warn.
  std::vector<EXODoubleWaveform> WF_real, WF_imag;
  for(size_t i = 0; i < fChannels.size(); i++) {
    const EXOWaveform* wf = ED->GetWaveformData()->GetWaveformWithChannel(fChannels[i]);
    if(not wf) LogEXOMsg("A waveform disappeared!", EEAlert);

    // Take the Fourier transform.
    EXODoubleWaveform dwf = wf->Convert<Double_t>();
    EXOWaveformFT fwf;
    EXOFastFourierTransformFFTW::GetFFT(dwf.GetLength()).PerformFFT(dwf, fwf);

    // Extract the real part.
    EXODoubleWaveform rwf;
    rwf.SetLength(fwf.GetLength());
    for(size_t f = 0; f < fwf.GetLength(); f++) rwf[f] = fwf[f].real();
    WF_real.push_back(rwf);

    // Extract the imaginary part.
    // Note that the first and last components are strictly real (though we get them anyway).
    EXODoubleWaveform iwf;
    iwf.SetLength(fwf.GetLength());
    for(size_t f = 0; f < fwf.GetLength(); f++) iwf[f] = fwf[f].imag();
    WF_imag.push_back(iwf);
  }

  // Produce estimates of the signals.
  std::vector<double> Results(fWireModel.size()+1, 0);
  for(size_t i = 0; i < Results.size(); i++) {
    for(size_t f = 0; f <= fMaxF-fMinF; f++) {
      for(size_t chan_index = 0; chan_index < fChannels.size(); chan_index++) {
        size_t XIndex = fColumnLength*i + 2*fChannels.size()*f + chan_index*(f < fMaxF-fMinF ? 2 : 1);
        Results[i] += X[XIndex]*WF_real[chan_index][f+fMinF];
        if(f < fMaxF-fMinF) Results[i] += X[XIndex+1]*WF_imag[chan_index][f+fMinF];
      }
    }
  }

  // Translate signal magnitudes into corresponding objects.
  // Start with wire signals.
  for(size_t i = 0; i < fWireModel.size(); i++) {
    EXOUWireSignal* sig = fWireModel[i].first;
    double UWireScalingFactor = ADC_FULL_SCALE_ELECTRONS_WIRE * W_VALUE_LXE_EV_PER_ELECTRON /
                                (CLHEP::keV * ADC_BITS);
    double GainCorrection = GainsFromDatabase->GetGainOnChannel(sig->fChannel)/300.0;
    sig->fDenoisedEnergy = Results[i]*GainCorrection*UWireScalingFactor;
  }
  // Propagate the wire signal energies to clusters.
  for(size_t i = 0; i < ED->GetNumChargeClusters(); i++) {
    EXOChargeCluster* clu = ED->GetChargeCluster(i);
    // I don't currently handle properly u-wire signals which are split between multiple charge clusters.
    // Probably will need Wolfi's help on that.
    // Or just figure out how to run clustering twice in one processing chain,
    // once before and once after this module.
    clu->fDenoisedEnergy = 0;
    for(size_t j = 0; j < clu->GetNumUWireSignals(); j++) {
      clu->fDenoisedEnergy += clu->GetUWireSignalAt(j)->fDenoisedEnergy;
    }
    // Apply the lifetime and grid corrections.
    // Just mimic the standard settings from processing as it exists now.
    EXOLifetimeCalib* lifeCalib = GetCalibrationFor(EXOLifetimeCalib,
                                                    EXOLifetimeCalibHandler, 
                                                    (clu->fZ > 0 ? "TPC1" : "TPC2"),
                                                    ED->fEventHeader);
    double lifetime = lifeCalib->lifetime(ED->fEventHeader.fTriggerSeconds);
    clu->fDenoisedEnergy *= std::exp(clu->fDriftTime / lifetime);
    EXOGridCorrectionCalib* gridCalib = GetCalibrationFor(EXOGridCorrectionCalib,
                                                       EXOGridCorrectionCalibHandler,
                                                       "linear_expcorrections",
                                                       ED->fEventHeader);
    clu->fDenoisedEnergy *= EXOGridCorrectionModule::GetGridCorrection(gridCalib, clu);
  }
  // Now translate the APD signal into the EXOScintillationCluster.
  scint->fDenoisedEnergy = Results.back()*fThoriumEnergy_keV;

  // No errors on results yet, but this is ToDo -- in principle it's easy to pull out.

  fWatch_ProcessEvent.Stop();
  return kOk;
}

int EXORefitSignals::ShutDown()
{
  // Print statistics and timing information.
  std::cout<<"Timer information for refit-apds module."<<std::endl;
  std::cout<<"Whole process:"<<std::endl;
  fWatch_ProcessEvent.Print();
  std::cout<<"Extracting the noise terms:"<<std::endl;
  fWatch_GetNoise.Print();
  std::cout<<"Producing an initial guess:"<<std::endl;
  fWatch_InitialGuess.Print();
  std::cout<<"Solving the matrix:"<<std::endl;
  fWatch_Solve.Print();
  std::cout<<"Multiplying the matrix by vectors (excluding allocation of return vector):"<<std::endl;
  fWatch_MatrixMul.Print();
  std::cout<<"Handling noise correlation part of matrix (the bottleneck):"<<std::endl;
  fWatch_MatrixMul_NoiseTerms.Print();
  std::cout<<std::endl;
  std::cout<<"Average number of iterations to solve: "
           <<double(fTotalNumberOfIterationsDone)/fNumEntriesSolved<<std::endl;
  std::cout<<"Alone, the wires would have required "
           <<double(fTotalIterationsForWires)/fNumEntriesSolved<<" iterations."<<std::endl;
  std::cout<<"Alone, the APDs would have required "
           <<double(fTotalIterationsForAPDs)/fNumEntriesSolved<<" iterations."<<std::endl;
  return 0;
}

EXOWaveformFT EXORefitSignals::GetModelForTime(double time) const
{
  // Return an EXOWaveformFT corresponding to a scintillation signal at time T.
  // The magnitude should be normalized so peak-baseline = 1.
  // The baseline itself is zero.
  // Note that at the moment, this assumes a waveform of length 2048 is required.
  // time is in ns.
  // No accounting for APD-by-APD shaping time variations is currently made.
  //
  // It might seem reasonable to do this just once, and apply a time shift in fourier space.
  // However, generating it in real space allows us to deal with signals near the end of
  // the trace, where periodicity is violated.
  // There is still some potential for caching the time-domain waveform, though, if needed.

  EXODoubleWaveform timeModel_fine;
  int refinedFactor = 5;
  timeModel_fine.SetLength(2048*refinedFactor);
  timeModel_fine.SetSamplingFreq(refinedFactor*CLHEP::megahertz);
  timeModel_fine.Zero();
  size_t NonzeroIndex = size_t(time/(CLHEP::microsecond/refinedFactor));
  for(size_t i = NonzeroIndex; i < timeModel_fine.GetLength(); i++) timeModel_fine[i] = 1;

  EXOTransferFunction tf;
  tf.AddIntegStageWithTime(3.*CLHEP::microsecond);
  tf.AddIntegStageWithTime(3.*CLHEP::microsecond);
  tf.AddDiffStageWithTime(10.*CLHEP::microsecond);
  tf.AddDiffStageWithTime(10.*CLHEP::microsecond);
  tf.AddDiffStageWithTime(300.*CLHEP::microsecond);

  tf.Transform(&timeModel_fine);
  timeModel_fine /= tf.GetGain();

  EXODoubleWaveform timeModel;
  timeModel.SetLength(2048);
  for(size_t i = 0; i < timeModel.GetLength(); i++) timeModel[i] = timeModel_fine[i*refinedFactor];

  EXOWaveformFT fwf;
  EXOFastFourierTransformFFTW::GetFFT(timeModel.GetLength()).PerformFFT(timeModel, fwf);
  assert(fwf.GetLength() == 1025); // Just to make sure I'm reasoning properly.
  return fwf;
}

double EXORefitSignals::GetGain(unsigned char channel) const
{
  // Return the gain of an apd channel.  This is the conversion factor from number
  // of photons incident on the APD to number of ADC counts (peak-baseline) in the
  // digitized signal.
  // It's a rough estimate, since this number isn't well-known, but we only need it to
  // set the scale for how important Poisson noise is, relative to electronic noise.
  // We currently use laser data from run 4540, and extract time-dependence from the
  // gainmap (the time-dependence of the lightmap).  It would be interesting to
  // do a fit of laser and gainmap data for the times when they overlap,
  // and get a higher-quality set of values.

  double Gain = 1.9; // 1.9 electron-hole pairs per photon, on average.

  // APD gains from the laser run 4540.
  switch(channel) {
    case 152: Gain *= 201.230438146; break;
    case 153: Gain *= 178.750438779; break;
    case 154: Gain *= 194.228589338; break;
    case 155: Gain *= 183.33801615; break;
    case 156: Gain *= 218.485999976; break;
    case 157: Gain *= 222.139259152; break;
    case 158: Gain *= 169.982559736; break;
    case 159: Gain *= 140.385120552; break;
    case 160: Gain *= 137.602725389; break;
    case 161: Gain *= 197.78183714; break;
    case 162: Gain *= 155.478773762; break;
    // case 163: Gain *= 0; // Bad channel, omitted.
    case 164: Gain *= 175.875067527; break;
    case 165: Gain *= 160.014408865; break;
    case 166: Gain *= 183.408055613; break;
    case 167: Gain *= 189.600819126; break;
    case 168: Gain *= 160.339214431; break;
    case 169: Gain *= 168.547991045; break;
    case 170: Gain *= 182.670039836; break;
    case 171: Gain *= 205.567802982; break;
    case 172: Gain *= 195.87450621; break;
    case 173: Gain *= 224.956647122; break;
    case 174: Gain *= 232.062359991; break;
    case 175: Gain *= 241.822881767; break;
    case 176: Gain *= 194.740435753; break;
    case 177: Gain *= 189.867775084; break;
    // case 178: Gain *= 0; // Bad channel, omitted.
    case 179: Gain *= 206.755206938; break;
    case 180: Gain *= 207.822617603; break;
    case 181: Gain *= 207.501985741; break;
    case 182: Gain *= 218.213137769; break;
    case 183: Gain *= 234.369354843; break;
    case 184: Gain *= 99.908111992; break;
    case 185: Gain *= 238.381809313; break;
    case 186: Gain *= 225.118270743; break;
    case 187: Gain *= 199.078450518; break;
    case 188: Gain *= 221.863823239; break;
    case 189: Gain *= 177.032783679; break;
    case 190: Gain *= 196.787332164; break;
    // case 191: Gain *= 0; // Bad channel, omitted.
    case 192: Gain *= 194.923448865; break;
    case 193: Gain *= 197.027984846; break;
    case 194: Gain *= 202.757086104; break;
    case 195: Gain *= 194.432937658; break;
    case 196: Gain *= 208.992809367; break;
    case 197: Gain *= 224.762562055; break;
    case 198: Gain *= 217.696006443; break;
    case 199: Gain *= 222.380158829; break;
    case 200: Gain *= 218.358804472; break;
    case 201: Gain *= 209.573057132; break;
    case 202: Gain *= 194.684536629; break;
    case 203: Gain *= 182.543842783; break;
    case 204: Gain *= 193.469930111; break;
    // case 205: Gain *= 0; // Bad channel, omitted.
    case 206: Gain *= 193.627191472; break;
    case 207: Gain *= 196.073150574; break;
    case 208: Gain *= 189.597962521; break;
    case 209: Gain *= 198.824317108; break;
    case 210: Gain *= 222.747770671; break;
    case 211: Gain *= 216.928470825; break;
    case 212: Gain *= 223.437239807; break;
    case 213: Gain *= 224.316404923; break;
    case 214: Gain *= 216.26783603; break;
    case 215: Gain *= 209.612423384; break;
    case 216: Gain *= 223.041660884; break;
    case 217: Gain *= 202.642254512; break;
    case 218: Gain *= 213.904993632; break;
    case 219: Gain *= 221.988942321; break;
    case 220: Gain *= 201.427174798; break;
    case 221: Gain *= 196.689200146; break;
    case 222: Gain *= 191.457656123; break;
    case 223: Gain *= 186.183873541; break;
    case 224: Gain *= 217.033080346; break;
    case 225: Gain *= 205.858374653; break;
    default: Gain *= 0; // Bad or non-existent channel.
  }
  // Time-dependence from the gainmap.
  const TGraph* GainGraph = fGainMaps.at(channel);
  Gain *= GainGraph->Eval(fUnixTimeOfEvent)/GainGraph->Eval(1355409118.254096);

  Gain *= 32.e-9; // Convert from electrons to volts in the preamp. Roughly 1/(5 pF) gain.
  Gain *= 12.10; // Gain from shapers (amplification factor, and gain from transfer function.
  Gain *= 4096./2.5; // Conversion from volts to ADC counts -- full-scale is 2.5 volts.

  return Gain;
}

bool EXORefitSignals::DoBiCGSTAB(std::vector<double>& X,
                                 double Threshold)
{
  // For more information on the Block-BiCGSTAB algorithm, see:
  // Electronic Transactions on Numerical Analysis, vol 16, 129-142 (2003).
  // "A BLOCK VERSION OF BICGSTAB FOR LINEAR SYSTEMS WITH MULTIPLE RIGHT-HAND SIDES"
  // A. EL GUENNOUNI, K. JBILOU, AND H. SADOK.
  // Do BiCGSTAB iterations until Threshold is met for all columns of X.
  // Use the given strategy; it corresponds to the matrix-vector multiplication to use.
  // Higher numbers will generally be faster, but only solve an approximation.
  // Returns true if the iterations actually terminated.

  // r_0 = b - Ax_0.  So compute Ax_0 - b, and then negate.
  std::vector<double> R = MatrixTimesVector(X);
  for(size_t i = 0; i <= fWireModel.size(); i++) {
    size_t Index = (i+1)*fColumnLength; // Next column; then subtract.
    Index -= fWireModel.size() + 1; // Step backward.
    Index += i; // Go forward to the right entry.
    R[Index] -= 1; // All models are normalized to 1.
  }
  for(size_t i = 0; i < R.size(); i++) {
    R[i] = -R[i];
  }

  std::vector<double> P = R;
  const std::vector<double> r0hat = R;

  // Solve the system.  Do a maximum of 1000 iterations, but expect to terminate much sooner.
  for(size_t i = 0; i < 1000; i++) {
    BiCGSTAB_iteration(X, R, P, r0hat);
    fTotalNumberOfIterationsDone++;

    // R is the residual at this iteration.
    // So, we should use it to test if the result is good enough.
    // b is the same every time (not dependent on fExpectedEnergy_keV),
    // so the permissible value |r| should be something we can find with trial and error.
    // Use the worst column.
    // The loop is currently split so I can understand the effect of APDs and wires on iterations.
    double WorstNorm = 0;
    for(size_t col = 0; col < fWireModel.size(); col++) {
      size_t ColIndex = col*fColumnLength;
      size_t NextCol = ColIndex + fColumnLength;
      double Norm = std::inner_product(R.begin() + ColIndex,
                                       R.begin() + NextCol,
                                       R.begin() + ColIndex,
                                       double(0));
      if(Norm > WorstNorm) WorstNorm = Norm;
    }
    if(WorstNorm > Threshold*Threshold) fTotalIterationsForWires++;
    for(size_t col = fWireModel.size(); col <= fWireModel.size(); col++) {
      size_t ColIndex = col*fColumnLength;
      size_t NextCol = ColIndex + fColumnLength;
      double Norm = std::inner_product(R.begin() + ColIndex,
                                       R.begin() + NextCol,
                                       R.begin() + ColIndex,
                                       double(0));
      if(Norm > WorstNorm) WorstNorm = Norm;
      if(Norm > Threshold*Threshold) fTotalIterationsForAPDs++;
    }

    if(WorstNorm < Threshold*Threshold) return true;
  }

  return false;
}

void EXORefitSignals::BiCGSTAB_iteration(std::vector<double>& X,
                                         std::vector<double>& R,
                                         std::vector<double>& P,
                                         const std::vector<double>& r0hat) const
{
  // Accept input in; do another iteration, and return the next iteration.
  // Does not bother with recycling any memory -- to revisit.
  // No pre-conditioning.  I just wanted something simple to implement here.

  // In a single iteration, we have need of at most three SxS matrices at a time.
  // So, go ahead and create them now, to reuse as needed.
  std::vector<double> SmallMat1, SmallMat2, SmallMat3;
  SmallMat1.resize((fWireModel.size()+1)*(fWireModel.size()+1));
  SmallMat2.resize((fWireModel.size()+1)*(fWireModel.size()+1));
  SmallMat3.resize((fWireModel.size()+1)*(fWireModel.size()+1));

  // Compute V.
  std::vector<double> V = MatrixTimesVector(P);

  // Compute the SxS matrix Rhat_0^T V.
  cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans,
              fWireModel.size() + 1, fWireModel.size() + 1, fColumnLength,
              1, &r0hat[0], fColumnLength, &V[0], fColumnLength,
              0, &SmallMat1[0], fWireModel.size() + 1);

  // Compute its inverse (which we'll use twice).
  // Do it the easy (and inefficient) way, using a TMatrixD.
  // Overwrite SmallMat1 with its own inverse.
  TMatrixD SmallMat1_TMat(fWireModel.size()+1, fWireModel.size()+1);
  for(size_t i = 0; i < SmallMat1.size(); i++) {
    SmallMat1_TMat(i % (fWireModel.size()+1), i / (fWireModel.size()+1)) = SmallMat1[i];
  }
  SmallMat1_TMat.InvertFast();
  for(size_t i = 0; i < SmallMat1.size(); i++) {
    SmallMat1[i] = SmallMat1_TMat(i % (fWireModel.size()+1), i / (fWireModel.size()+1));
  }

  // Compute the SxS matrix Rhat_0^T R.
  cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans,
              fWireModel.size() + 1, fWireModel.size() + 1, fColumnLength,
              1, &r0hat[0], fColumnLength, &R[0], fColumnLength,
              0, &SmallMat2[0], fWireModel.size() + 1);

  // Solve for alpha = SmallMat3.
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
              fWireModel.size() + 1, fWireModel.size() + 1, fWireModel.size() + 1,
              1, &SmallMat1[0], fWireModel.size() + 1, &SmallMat2[0], fWireModel.size() + 1,
              0, &SmallMat3[0], fWireModel.size() + 1);


  // Modify R.
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
              fColumnLength, fWireModel.size() + 1, fWireModel.size() + 1,
              -1, &V[0], fColumnLength, &SmallMat3[0], fWireModel.size() + 1,
               1, &R[0], fColumnLength);

  // Compute T.
  std::vector<double> T = MatrixTimesVector(R);

  // Compute omega.
  double omega = std::inner_product(T.begin(), T.end(), R.begin(), double(0)) /
                 std::inner_product(T.begin(), T.end(), T.begin(), double(0));

  // Modify X.
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
              fColumnLength, fWireModel.size() + 1, fWireModel.size() + 1,
              1, &P[0], fColumnLength, &SmallMat3[0], fWireModel.size() + 1,
              1, &X[0], fColumnLength);
  for(size_t i = 0; i < X.size(); i++) X[i] += omega*R[i];

  // Modify R.
  for(size_t i = 0; i < R.size(); i++) R[i] -= omega*T[i];

  // What follows is wasted if this is the last iteration -- but that's probably small overhead.

  // Compute the SxS matrix -Rhat_0^T T.
  cblas_dgemm(CblasColMajor, CblasTrans, CblasNoTrans,
              fWireModel.size() + 1, fWireModel.size() + 1, fColumnLength,
              -1, &r0hat[0], fColumnLength, &T[0], fColumnLength,
              0, &SmallMat2[0], fWireModel.size() + 1);

  // Solve for beta = SmallMat3.
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
              fWireModel.size() + 1, fWireModel.size() + 1, fWireModel.size() + 1,
              1, &SmallMat1[0], fWireModel.size() + 1, &SmallMat2[0], fWireModel.size() + 1,
              0, &SmallMat3[0], fWireModel.size() + 1);

  // Finally, update P.  Overwrite T for temporary work.
  T = R;
  for(size_t i = 0; i < P.size(); i++) P[i] -= omega*V[i];
  cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
              fColumnLength, fWireModel.size() + 1, fWireModel.size() + 1,
              1, &P[0], fColumnLength, &SmallMat3[0], fWireModel.size() + 1,
              1, &T[0], fColumnLength);
  std::swap(T, P);
}

std::vector<double> EXORefitSignals::MatrixTimesVector(const std::vector<double>& in) const
{
  // Do A*in; return the result.
  // Notice that means we're doing some unnecessary heap allocations;
  // consider in the future reusing the memory of these vectors.
  // If Fast is set to a non-zero value, it indicates we should perform multiplication by
  // some approximation of the matrix which will speed things up.
  // In general the higher the number, the faster the multiplication, but this is for internal use
  // so I make no promises.

  assert(in.size() == fColumnLength * (fWireModel.size()+1));
  std::vector<double> out;
  out.assign(in.size(), 0);

  // Note that I want to exclude vector allocation in the time --
  // because in principle that could be eliminated if the computational part ever became sub-dominant.
  fWatch_MatrixMul.Start(false);

  // First, do the noise blocks.
  // This has, in the past, been the bottleneck -- but now I've designed everything to optimize it.
  // Should keep an eye on it to ensure it remains the bottleneck.
  fWatch_MatrixMul_NoiseTerms.Start(false);
  for(size_t f = 0; f <= fMaxF - fMinF; f++) {
    size_t StartIndex = 2*fChannels.size()*f;
    size_t BlockSize = fChannels.size() * (f < fMaxF - fMinF ? 2 : 1);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                BlockSize, fWireModel.size()+1, BlockSize,
                1, &fNoiseCorrelations[f][0], BlockSize, &in[StartIndex], fColumnLength,
                1, &out[StartIndex], fColumnLength);
  }
  fWatch_MatrixMul_NoiseTerms.Stop();

  // Next, do the Poisson terms for APD channels.
  for(size_t k = fFirstAPDChannelIndex; k < fChannels.size(); k++) { // APD gangs
    double ChannelFactors = fExpectedEnergy_keV/fThoriumEnergy_keV;
    ChannelFactors *= GetGain(fChannels[k]);
    ChannelFactors *= fExpectedYieldPerGang.at(fChannels[k]);

    for(size_t n = 0; n <= fWireModel.size(); n++) { // signals
      // Compute the factors common to all frequencies.
      double CommonFactor = 0;
      for(size_t g = 0; g <= fMaxF - fMinF; g++) {
        size_t InIndex = n*fColumnLength + 2*fChannels.size()*g + k*(g < fMaxF-fMinF ? 2 : 1);
        CommonFactor += fmodel_realimag[2*g] * in[InIndex];
        if(g < fMaxF-fMinF) CommonFactor += fmodel_realimag[2*g+1] * in[InIndex+1];
      }
      CommonFactor *= ChannelFactors;

      // Now actually transfer the changes to the out vector.
      for(size_t f = 0; f <= fMaxF - fMinF; f++) {
        size_t OutIndex = n*fColumnLength + 2*fChannels.size()*f + k*(f < fMaxF-fMinF ? 2 : 1);
        out[OutIndex] += CommonFactor * fmodel_realimag[2*f];
        if(f < fMaxF-fMinF) out[OutIndex+1] += CommonFactor * fmodel_realimag[2*f+1];
      }
    }
  } // End Poisson terms.

  // Lagrange and constraint terms.
  // First loop through wire signals.
  for(size_t m = 0; m < fWireModel.size(); m++) {
    const std::map<unsigned char, std::vector<double> >& models = fWireModel[m].second;
    for(std::map<unsigned char, std::vector<double> >::const_iterator it = models.begin();
        it != models.end();
        it++) {
      unsigned char ChannelWithWireSignal = it->first;
      size_t channel_index = 0;
      while(fChannels[channel_index] != ChannelWithWireSignal) {
        channel_index++;
        if(channel_index >= fChannels.size()) LogEXOMsg("Index exceeded -- why can this happen?", EEAlert);
      }
      const std::vector<double>& modelWF = it->second;
      for(size_t f = 0; f <= fMaxF - fMinF; f++) {
        size_t Index1 = fColumnLength - (fWireModel.size()+1) + m;
        size_t Index2 = 2*fChannels.size()*f + channel_index*(f < fMaxF-fMinF ? 2 : 1);
        for(size_t n = 0; n <= fWireModel.size(); n++) {
          out[Index2] += modelWF[2*f]*in[Index1];
          out[Index1] += modelWF[2*f]*in[Index2];
          if(f < fMaxF-fMinF) {
            out[Index2+1] += modelWF[2*f+1]*in[Index1];
            out[Index1] += modelWF[2*f+1]*in[Index2+1];
          }
          Index1 += fColumnLength;
          Index2 += fColumnLength;
        }
      }
    }
  } // Done with Lagrange and constraint terms for wires.
  // Now, Lagrange and constraint terms for APDs.
  for(size_t k = fFirstAPDChannelIndex; k < fChannels.size(); k++) {
    double ExpectedYieldOnGang = fExpectedYieldPerGang.at(fChannels[k]);
    for(size_t f = 0; f <= fMaxF - fMinF; f++) {
      size_t Index1 = 2*fChannels.size()*f + k*(f < fMaxF-fMinF ? 2 : 1);
      size_t Index2 = fColumnLength - 1;
      for(size_t n = 0; n <= fWireModel.size(); n++) {
        out[Index2] += fmodel_realimag[2*f]*in[Index1];
        out[Index1] += fmodel_realimag[2*f]*in[Index2];
        if(f < fMaxF-fMinF) {
          out[Index2] += fmodel_realimag[2*f+1]*in[Index1+1];
          out[Index1+1] += fmodel_realimag[2*f+1]*in[Index2];
        }
        Index1 += fColumnLength;
        Index2 += fColumnLength;
      }
    }
  }

  fWatch_MatrixMul.Stop();
  return out;
}

std::vector<double> EXORefitSignals::MakeWireModel(EXODoubleWaveform& in,
                                                   const EXOTransferFunction& transfer,
                                                   const double Gain,
                                                   const double Time) const
{
  // Helper function for dealing with shaping and FFT of wire models.
  EXODoubleWaveform shapedIn;
  transfer.Transform(&in, &shapedIn);
  shapedIn /= Gain;

  EXODoubleWaveform wf;
  wf.SetLength(2048);
  wf.Zero();
  for(size_t i = 0; i < 2048; i++) {
    double RelTime = SAMPLE_TIME*i - Time;
    int HighBandwidthIndex = int((256.*CLHEP::microsecond + RelTime)/SAMPLE_TIME_HIGH_BANDWIDTH);
    if(HighBandwidthIndex >= 0 and HighBandwidthIndex < int(shapedIn.GetLength())) {
      wf[i] = shapedIn[HighBandwidthIndex];
    }
  }

  EXOWaveformFT fwf;
  EXOFastFourierTransformFFTW::GetFFT(2048).PerformFFT(wf, fwf);

  std::vector<double> out;
  out.resize(2*1024-1);
  for(size_t f = 1; f <= 1024; f++) {
    out[2*(f-1)] = fwf[f].real();
    if(f != 1024) out[2*(f-1)+1] = fwf[f].imag();
  }

  return out;
}






































void EXORefitSignals::DoBlBiCGSTAB(EventHandler& event)
{
  // Pick up wherever we left off.
  // This function gets called when a noise matrix multiplication just happened.
  // So, we have to first identify where we are, then proceed as far as we can until:
  //   Another matrix multiplication needs to be done, or
  //   The solver has terminated.

  if(event.fR.size() == 0) {
    // We're still in the setup phase.
    size_t NoiseColLength = fChannels.size() * (2*(fMaxF-fMinF) + 1);
    fR.assign(event.fColLength * (event.fWireModel.size()+1), 0);
    for(size_t i = 0; i <= event.fWireModel.size(); i++) {
      size_t IndexToGrab = event.fResultIndex + i * NoiseColLength;
      for(size_t j = 0; j < NoiseColLength; j++) {
        event.fR[i*event.fColumnLength + j] = fNoiseMulResult[IndexToGrab + j];
      }
    }
    // Now need to finish multiplying by A, accounting for the other terms.



















void EXORefitSignals::DoNoiseMultiplication()
{
  // Multiply everything in fNoiseMulQueue by the noise.
  // Note that we expect columns in the input to contain only the noise portion, not the constraint rows;
  // otherwise, the vector lengths would not match.
  // The result is placed in fNoiseMulResult.
  size_t NoiseColLength = fChannels.size() * (2*(fMaxF-fMinF) + 1);
  assert(fNoiseMulQueue.size() == NoiseColLength * fNumVectorsInQueue;
  fNoiseMulResult.assign(fNoiseMulQueue.size(), 0); // Probably don't need to fill with 0.

  // Do the multiplication -- one call for every frequency.
  fWatch_MatrixMul_NoiseTerms.Start(false); // Don't count vector allocation.
  for(size_t f = 0; f <= fMaxF - fMinF; f++) {
    size_t StartIndex = 2*fChannels.size()*f;
    size_t BlockSize = fChannels.size() * (f < fMaxF - fMinF ? 2 : 1);
    cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
                BlockSize, fNumVectorsInQueue, BlockSize,
                1, &fNoiseCorrelations[f][0], BlockSize, &fNoiseMulQueue[StartIndex], NoiseColLength,
                0, &fNoiseMulResult[StartIndex], NoiseColLength);
  }
  fWatch_MatrixMul_NoiseTerms.Stop();

  // Clean up, to be ready for the next call.
  fNoiseMulQueue.clear(); // Hopefully doesn't free memory, since I'll need it again.
  fNumVectorsInQueue = 0;
}