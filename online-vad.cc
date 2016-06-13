// SNT5/vad.cc
// Coice Activity detector using NNET3 framework

#include <math.h>
#include "online-vad.h"

namespace kaldi {


Vad::Vad(const VadOptions &opts) : post_processor(opts.seg_opts) {
  isrecognize_cont = true;
  frame_shift_ = opts.frame_shift;
  frame_overlap_ = opts.frame_overlap;
  chunk_time_ = opts.chunk_time;
  feat_config_ = new OnlineFeaturePipelineConfig(opts.vad_feature_config);
  feat_pipeline_ = new OnlineFeaturePipeline(*feat_config_);
  out_segfile.open(opts.vad_segments_filename.c_str(), std::ios_base::app);
    
  energy_threshold_ = opts.energy_threshold;
  speech_offset_ = opts.speech_offset;
  wave_memory.Resize(0);
  old_frames_decoded_ = 0;
  new_frames_decoded_ = 0;
//the assumption for num_frames_total is that the missing 6 frames is at the end of chunk instead 
// chunk start. So, chunk start is same as num_frames_total at start.
  num_frames_total = 6;
  num_chunks_lat = 0;
  prev_start = 0;
  prev_end = 0;
  curr_start = 0;
  curr_end = 0; 
  seg_no_ = 0;
  use_gpu_log_ = opts.use_gpu_log;

  decoding_graph_ = opts.decoding_graph;
  seg_opts_ = opts.seg_opts;
  speech_to_sil_ratio_ = opts.speech_to_sil_ratio;
  pad_length_ = opts.pad_length;
  post_pad_length_ = opts.post_pad_length;
  num_frames_skipped_ = opts.num_frames_skipped;
  segment_buffer_len_ = opts.segment_buffer_len;
  chunk_count_buf_ = int(ceil((segment_buffer_len_*frame_shift_+frame_overlap_)/chunk_time_));

  gmm_decoding_opts_ = opts.gmm_decoding_opts;
  decode_fst_ = fst::ReadFstKaldi(decoding_graph_);
  models_ = new OnlineGmmDecodingModels(gmm_decoding_opts_);
  decoding_model_ = opts.decoding_model;
  ReadKaldiObject(decoding_model_, &trans_model_);
  gmm_decoder_ = new SingleUtteranceGmmDecoder(gmm_decoding_opts_,
                                               *models_,
                                               *feat_pipeline_,
                                               *decode_fst_,
						adaptation_state_,
						speech_offset_,
						energy_threshold_);
}

void Vad::reinitiate()
{
  isrecognize_cont = true;
  num_frames_total = 0;
  num_chunks_lat = 0;
  prev_start = 0;
  prev_end = 0;
  curr_start = 0;
  curr_end = 0;
}

bool Vad::Compute_online(const VectorBase<BaseFloat> &waveform, std::vector<std::vector<BaseFloat> > *seg_times, std::string wav_id, bool isrecordingcontinue) {
  try{
    // wave_buffer_ is the wave sample accumulator so that it could be used for sample history once there is utterance end and ;
    // seg_buffer_length is to be greater than wave_buffer length;
    wave_buffer_.Resize(wave_buffer_.Dim() + waveform.Dim(), kCopyData);
    for (int i = 0; i < waveform.Dim(); i++)
      {wave_buffer_(wave_buffer_.Dim()-waveform.Dim()+i) = waveform(i);}

    num_chunks_lat++;
    feat_pipeline_->AcceptWaveform(SNT_KALDI_SMP_FREQ, waveform);
//    KALDI_LOG << "Num frames in feat pipe : " << feat_pipeline_->NumFramesReady();
    gmm_decoder_->AdvanceDecoding();
//    KALDI_LOG << chunk_count_buf;
    if ((int(floor(((num_chunks_lat)*chunk_time_)/frame_shift_)) - (num_frames_skipped_) > 0) && ((num_chunks_lat % chunk_count_buf_) == 0)) {
      
      VectorFst<LatticeArc> decoded;  // linear FST.
      gmm_decoder_->GetBestPath(true, &decoded);

      std::vector<int32> alignment;
      std::vector<int32> words;
      LatticeWeight weight;

      GetLinearSymbolSequence(decoded, &alignment, &words, &weight);
      //delete decode_fst;
     
      new_frames_decoded_ = int(alignment.size());
      std::vector<std::vector<int32> > split_phones;
      
      std::vector<int32> sub_align(&alignment[std::max(0, old_frames_decoded_ - num_frames_skipped_)], &alignment[new_frames_decoded_ - num_frames_skipped_]);
      SplitToPhones(trans_model_, sub_align, &split_phones);
      
      std::vector<int32> phones;
      for (size_t i = 0; i < split_phones.size(); i++) {
        KALDI_ASSERT(!split_phones[i].empty());
        int32 phone = trans_model_.TransitionIdToPhone(split_phones[i][0]);
        int32 num_repeats = split_phones[i].size();
        for(int32 j = 0; j < num_repeats; j++)
          phones.push_back(phone);
      }
    //  KALDI_LOG << "alignment size : " <<phones.size();
      int64 num_segments = 0;
      std::vector<int64> frame_counts_per_class;

      kaldi::segmenter::Segmentation speech_seg_online;
      num_segments = (speech_seg_online).InsertFromAlignment(phones, 0, &frame_counts_per_class);
  //    KALDI_LOG << "Segmentation initiated from alignment : " << num_segments;

   
      post_processor.RemoveSegments(&speech_seg_online);
      post_processor.MergeLabels(&speech_seg_online);
 
      if ((speech_seg_online).Begin()->Length() <= 1) {
//	if (prev_end > 0) { *getReco_flag = true;}
        isrecognize_cont = true;
//	KALDI_WARN << "sil segment";
	}
        
      if ((speech_seg_online).Begin()->Length() > 1) {
        for (kaldi::segmenter::SegmentList::const_iterator it = (speech_seg_online).Begin(); it != (speech_seg_online).End(); ++it)  {
    //      KALDI_LOG << "PRESENT SEGS : " << (it->start_frame) << " :: " << it->end_frame;
          curr_start = (num_frames_total  + std::max(0, old_frames_decoded_ - num_frames_skipped_))*frame_shift_ + (it->start_frame)*frame_shift_;
          curr_end = (num_frames_total + std::max(0, old_frames_decoded_ - num_frames_skipped_))*frame_shift_ + (it->end_frame + 1)*frame_shift_ + frame_overlap_;
          //	    KALDI_LOG << "yes";
          
	  curr_start -= (pad_length_)*frame_shift_;
          if (prev_end > 0) prev_end += pad_length_*frame_shift_;
          //	    if (curr_start <= prev_end) { curr_start = prev_start; }
          if (((curr_start - prev_end) < (seg_opts_.max_intersegment_length*frame_shift_))&&(prev_end>0)) {
            curr_start = prev_start;
          }
          curr_start -= post_pad_length_*frame_shift_;	    
          if (prev_end > 0) prev_end += post_pad_length_*frame_shift_;
          if ((curr_start <= prev_end)&&(prev_end>0)) { curr_start = prev_start; }

          if (((curr_start - prev_end) > 0) && ((prev_end - prev_start)>0)) {
            //   return true;
            isrecognize_cont = false;
//	    KALDI_LOG << "Prev_seg : " << prev_start << " : " << prev_end;
	    seg_no_ = seg_no_ + 1;
	    out_segfile << wav_id << "_" << seg_no_ << " " << wav_id << " " << prev_start << " " << prev_end << "\n";
          } else {
            isrecognize_cont = true;}
	  std::vector<BaseFloat> row;
          row.push_back(prev_start);
          row.push_back(prev_end);
          (*seg_times).push_back(row);

          prev_start = curr_start;
          prev_end = curr_end;
	}
  //	  KALDI_LOG << "Prev_seg : " << prev_start << " : " << prev_end;              
	      if ((prev_end - prev_start)>0) { 	    
            std::vector<BaseFloat> row;
            row.push_back(prev_start);
            row.push_back(prev_end);
            (*seg_times).push_back(row);
        }
    }
         if (!isrecordingcontinue) {
            seg_no_ = seg_no_ + 1;
            out_segfile << wav_id << "_" << seg_no_ << " " << wav_id << " " << prev_start << " " << prev_end << "\n";
	}

//    KALDI_LOG << "WAVE_BUFFER_SIZE : " << wave_buffer_.Dim();
    if (isrecognize_cont == false) {
           
            feat_pipeline_->InputFinished();
            feat_pipeline_ = new OnlineFeaturePipeline(*feat_config_);
            num_chunks_lat = chunk_count_buf_; //-1 because it increments later in the function
	    num_frames_total = num_frames_total + old_frames_decoded_ - std::max(0.0f, (wave_memory.Dim()/(SNT_KALDI_SMP_FREQ) - chunk_time_)/(frame_shift_)) + 7;
	    wave_memory.Resize(chunk_count_buf_*SNT_KALDI_SMP_FREQ*chunk_time_, kSetZero);
	    for (int i = 0; i < chunk_count_buf_*SNT_KALDI_SMP_FREQ*chunk_time_; i++) {
	      wave_memory(i) = wave_buffer_(wave_buffer_.Dim() - wave_memory.Dim() + i);}
            feat_pipeline_->AcceptWaveform(SNT_KALDI_SMP_FREQ, wave_memory);
                delete gmm_decoder_;
            gmm_decoder_ = new SingleUtteranceGmmDecoder(gmm_decoding_opts_,
                                                         *models_,
                                                         *feat_pipeline_,
                                                         *decode_fst_,
                                                        adaptation_state_,
							speech_offset_,
							energy_threshold_);
	    //            KALDI_LOG << "Prev_seg : " << prev_start << " : " << prev_end;

            gmm_decoder_->AdvanceDecoding();
            new_frames_decoded_ = feat_pipeline_->NumFramesReady();}
    	    old_frames_decoded_ = new_frames_decoded_;
    	    wave_buffer_ = wave_memory;
    }
    //num_chunks++;
    return isrecognize_cont;
  } catch(const std::exception &e) {
    std::cerr << e.what();
    return true;
  }


}
}
