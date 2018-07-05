//
// Copyright 2010-2012,2014-2015 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//m
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/thread_priority.hpp>
#include <uhd/utils/safe_main.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/exception.hpp>
#include <boost/thread/thread.hpp>
#include <boost/program_options.hpp>
#include <boost/math/special_functions/round.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <rfid/api.h>
#include <rfid/reader.h>
#include <iostream>
#include <fstream>
#include <csignal>
#include <complex>
#include <stdio.h>
#include <vector>
#include <cmath>
#include <sys/time.h>
#include <mutex>
#include <string>

namespace po = boost::program_options;
using namespace std;
/***********************************************************************
 * Signal handlers
 **********************************************************************/
static bool stop_signal_called = false;
void sig_int_handler(int){stop_signal_called = true;}

// CONSTANTS (READER CONFIGURATION)
// Fixed number of slots (2^(FIXED_Q))  
const int FIXED_Q              = 0;
// Termination criteria
// const int MAX_INVENTORY_ROUND = 50;
const int MAX_NUM_QUERIES     = 1;     // Stop after MAX_NUM_QUERIES have been sent
// valid values for Q
const int Q_VALUE [16][4] =  
{
    {0,0,0,0}, {0,0,0,1}, {0,0,1,0}, {0,0,1,1}, 
    {0,1,0,0}, {0,1,0,1}, {0,1,1,0}, {0,1,1,1}, 
    {1,0,0,0}, {1,0,0,1}, {1,0,1,0}, {1,0,1,1},
    {1,1,0,0}, {1,1,0,1}, {1,1,1,0}, {1,1,1,1}
};  
const bool P_DOWN = false;
// Duration in us
const int CW_D         = 250;    // Carrier wave
const int P_DOWN_D     = 2000;    // power down
const int T1_D         = 240;    // Time from Interrogator transmission to Tag response (250 us)
const int T2_D         = 480;    // Time from Tag response to Interrogator transmission. Max value = 20.0 * T_tag = 500us 
const int PW_D         = 12;      // Half Tari 
const int DELIM_D       = 12;      // A preamble shall comprise a fixed-length start delimiter 12.5us +/-5%
const int TRCAL_D     = 200;    // BLF = DR/TRCAL => 40e3 = 8/TRCAL => TRCAL = 200us
const int RTCAL_D     = 72;      // 6*PW = 72us
const int NUM_PULSES_COMMAND = 5;       // Number of pulses to detect a reader command
const int NUMBER_UNIQUE_TAGS = 1;      // Stop after NUMBER_UNIQUE_TAGS have been read 
// Number of bits
const int PILOT_TONE          = 12;  // Optional
const int TAG_PREAMBLE_BITS  = 6;   // Number of preamble bits
const int RN16_BITS          = 17;  // Dummy bit at the end
const int EPC_BITS            = 129;  // PC + EPC + CRC16 + Dummy = 6 + 16 + 96 + 16 + 1 = 135
const int QUERY_LENGTH        = 22;  // Query length in bits
const int T_READER_FREQ = 40e3;     // BLF = 40kHz
const float TAG_BIT_D   = 1.0/T_READER_FREQ * pow(10,6); // Duration in us
const int RN16_D        = (RN16_BITS + TAG_PREAMBLE_BITS) * TAG_BIT_D;
const int EPC_D          = (EPC_BITS  + TAG_PREAMBLE_BITS) * TAG_BIT_D;
// Query command 
const int QUERY_CODE[4] = {1,0,0,0};
const int M[2]          = {0,0};
const int SEL[2]         = {0,0};
const int SESSION[2]     = {0,0};
const int TARGET         = 0;
const int TREXT         = 0;
const int DR            = 0;
const int NAK_CODE[8]   = {1,1,0,0,0,0,0,0};
// ACK command
const int ACK_CODE[2]   = {0,1};
// QueryAdjust command
const int QADJ_CODE[4]   = {1,0,0,1};
// 110 Increment by 1, 000 unchanged, 010 decrement by 1
const int Q_UPDN[3][3]  = { {1,1,0}, {0,0,0}, {0,1,0} };
// FM0 encoding preamble sequences
const int TAG_PREAMBLE[] = {1,1,0,1,0,0,1,0,0,0,1,1};
// Gate block parameters
const float THRESH_FRACTION = 0.75;     
const int WIN_SIZE_D         = 250; 
// Duration in which dc offset is estimated (T1_D is 250)
const int DC_SIZE_D         = 120;
int PREAMBLE[2][12]={ {1,1,-1,1,-1,-1,1,-1,-1,-1,1,1}, {-1,-1,1,-1,1,1,-1,1,1,1,-1,-1} };
std::vector<float> data_0, data_1, cw, cw_ack, cw_query, delim, frame_sync, preamble, rtcal, trcal, query_bits, ack_bits, query_rep,nak, query_adjust_bits, p_down;
const int dac_rate = 1e6;
const int adc_rate = 2e6;
const int decim = 5;
const int rx_size = 200;
int s_rate = adc_rate;
enum GEN2_LOGIC_STATUS  {SEND_QUERY, SEND_ACK, SEND_QUERY_REP, IDLE, SEND_CW, START, START2};
enum GATE_STATUS        {GATE_OPEN, GATE_CLOSED, GATE_SEEK_RN16, GATE_SEEK_EPC};  
enum DECODER_STATUS     {DECODER_DECODE_RN16, DECODER_DECODE_EPC};
//decoder
enum SIGNAL_STATE {NEG_EDGE, POS_EDGE};
GATE_STATUS gate_status = GATE_SEEK_RN16;
DECODER_STATUS decoder_status = DECODER_DECODE_RN16;
GEN2_LOGIC_STATUS gen2_logic_status = START;
vector<complex<float> > before_gate(rx_size/decim), after_gate, rx_buff(rx_size);
vector<complex<float> > filter_buff(25);
vector<int> RN16_bits;
vector<int> EPC_bits;
int flag=0; //receive before send
int flag2 = 0; //fucking ack bug
//used in gate
int gate_pre_count = 0;
int win_length = WIN_SIZE_D * (s_rate / decim / pow(10,6));
vector<float> win_samples(win_length);
SIGNAL_STATE signal_state = NEG_EDGE;
int win_index=0, n_samples=0, n_samples_to_ungate; 
float num_pulses=0, sample_thresh, sample_ampl=0, avg_ampl=0;
map<string, string> mapping;
/***********************************************************************
 * Utilities
 **********************************************************************/
//! Change to filename, e.g. from usrp_samples.dat to usrp_samples.00.dat,
//  but only if multiple names are to be generated.
/*std::string generate_out_filename(const std::string &base_fn, size_t n_names, size_t this_name)
{
    if (n_names == 1) {
        return base_fn;
    }

    boost::filesystem::path base_fn_fp(base_fn);
    base_fn_fp.replace_extension(
        boost::filesystem::path(
            str(boost::format("%02d%s") % this_name % base_fn_fp.extension().string())
        )
    );
    return base_fn_fp.string();
}*/

/***********************************************************************
 * transmit_worker function
 * A function to be used as a boost::thread_group thread for transmitting
 **********************************************************************/
void crc_append(std::vector<float> & q){
    int crc[] = {1,0,0,1,0};
    for(int i = 0; i < 17; i++){
        int tmp[] = {0,0,0,0,0};
        tmp[4] = crc[3];
        if(crc[4] == 1){
            if (q[i] == 1){
                tmp[0] = 0;
                tmp[1] = crc[0];
                tmp[2] = crc[1];
                tmp[3] = crc[2];
            }
            else{
                tmp[0] = 1;
                tmp[1] = crc[0];
                tmp[2] = crc[1];
                if(crc[2] == 1)
                    tmp[3] = 0;
                else
                    tmp[3] = 1;
            }
        }
        else{
            if (q[i] == 1){
                tmp[0] = 1;
                tmp[1] = crc[0];
                tmp[2] = crc[1];
                if(crc[2] == 1)
                    tmp[3] = 0;
                else
                    tmp[3] = 1;
            }
            else{
                tmp[0] = 0;
                tmp[1] = crc[0];
                tmp[2] = crc[1];
                tmp[3] = crc[2];
            }
        }
        memcpy(crc, tmp, 5*sizeof(float));
    }
    for (int i = 4; i >= 0; i--)
        q.push_back(crc[i]);
    return;
}

void gen_query_bits(void){
    int num_ones = 0, num_zeros = 0;
    query_bits.resize(0);
    query_bits.insert(query_bits.end(), &QUERY_CODE[0], &QUERY_CODE[4]);
    query_bits.push_back(DR);
    query_bits.insert(query_bits.end(), &M[0], &M[2]);
    query_bits.push_back(TREXT);
    query_bits.insert(query_bits.end(), &SEL[0], &SEL[2]);
    query_bits.insert(query_bits.end(), &SESSION[0], &SESSION[2]);
    query_bits.push_back(TARGET);
    query_bits.insert(query_bits.end(), &Q_VALUE[FIXED_Q][0], &Q_VALUE[FIXED_Q][4]);
    crc_append(query_bits);
    return;
}

void gen_ack_bits(void){
    ack_bits.resize(0);
    ack_bits.insert(ack_bits.end(), &ACK_CODE[0], &ACK_CODE[2]);
    ack_bits.insert(ack_bits.end(), &RN16_bits[0], &RN16_bits[16]);
    return;
}

void gen_query_adjust_bits(void){
    query_adjust_bits.resize(0);
    query_adjust_bits.insert(query_adjust_bits.end(), &QADJ_CODE[0], &QADJ_CODE[4]);
    query_adjust_bits.insert(query_adjust_bits.end(), &SESSION[0], &SESSION[2]);
    query_adjust_bits.insert(query_adjust_bits.end(), &Q_UPDN[1][0], &Q_UPDN[1][3]);
    return;
}

void initMap(void){
    mapping["0000"] = "0";
    mapping["0001"] = "1";
    mapping["0010"] = "2";
    mapping["0011"] = "3";
    mapping["0100"] = "4";
    mapping["0101"] = "5";
    mapping["0110"] = "6";
    mapping["0111"] = "7";
    mapping["1000"] = "8";
    mapping["1001"] = "9";
    mapping["1010"] = "A";
    mapping["1011"] = "B";
    mapping["1100"] = "C";
    mapping["1101"] = "D";
    mapping["1110"] = "E";
    mapping["1111"] = "F";
}

void readerInit(void){
    double sample_d = 1.0/dac_rate * pow(10,6);
    // Number of samples for transmitting
    float n_data0_s = 2 * PW_D / sample_d;
    float n_data1_s = 4 * PW_D / sample_d;
    float n_pw_s    = PW_D    / sample_d;
    float n_cw_s    = CW_D    / sample_d;
    float n_delim_s = DELIM_D / sample_d;
    float n_trcal_s = TRCAL_D / sample_d;
    // CW waveforms of different sizes
    const int n_cwquery_s   = (T1_D+T2_D+RN16_D)/sample_d;     //RN16
    const int n_cwack_s     = (3*T1_D+T2_D+EPC_D)/sample_d;    //EPC   
    //if it is longer than nominal it wont cause tags to change inventoried flag
    const int n_p_down_s     = (P_DOWN_D)/sample_d;  
    p_down.resize(n_p_down_s);         // Power down samples
    cw_query.resize(n_cwquery_s);      // Sent after query/query rep
    cw_ack.resize(n_cwack_s);          // Sent after ack
    std::fill_n(cw_query.begin(), cw_query.size(), 1);
    std::fill_n(cw_ack.begin(), cw_ack.size(), 1);
    // Construct vectors (resize() default initialization is zero)
    data_0.resize(n_data0_s);
    data_1.resize(n_data1_s);
    cw.resize(n_cw_s);
    delim.resize(n_delim_s);
    rtcal.resize(n_data0_s + n_data1_s);
    trcal.resize(n_trcal_s);
    // Fill vectors with data
    std::fill_n(data_0.begin(), data_0.size()/2, 1);
    std::fill_n(data_1.begin(), 3*data_1.size()/4, 1);
    std::fill_n(cw.begin(), cw.size(), 1);
    std::fill_n(rtcal.begin(), rtcal.size() - n_pw_s, 1); // RTcal
    std::fill_n(trcal.begin(), trcal.size() - n_pw_s, 1); // TRcal
    // create preamble
    preamble.insert( preamble.end(), delim.begin(), delim.end() );
    preamble.insert( preamble.end(), data_0.begin(), data_0.end() );
    preamble.insert( preamble.end(), rtcal.begin(), rtcal.end() );
    preamble.insert( preamble.end(), trcal.begin(), trcal.end() );
    // create framesync
    frame_sync.insert( frame_sync.end(), delim.begin() , delim.end() );
    frame_sync.insert( frame_sync.end(), data_0.begin(), data_0.end() );
    frame_sync.insert( frame_sync.end(), rtcal.begin() , rtcal.end() );
    // create query rep
    query_rep.insert( query_rep.end(), frame_sync.begin(), frame_sync.end());
    query_rep.insert( query_rep.end(), data_0.begin(), data_0.end() );
    query_rep.insert( query_rep.end(), data_0.begin(), data_0.end() );
    query_rep.insert( query_rep.end(), data_0.begin(), data_0.end() );
    query_rep.insert( query_rep.end(), data_0.begin(), data_0.end() );
    // create nak
    nak.insert( nak.end(), frame_sync.begin(), frame_sync.end());
    nak.insert( nak.end(), data_1.begin(), data_1.end() );
    nak.insert( nak.end(), data_1.begin(), data_1.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    nak.insert( nak.end(), data_0.begin(), data_0.end() );
    gen_query_bits();
    gen_query_adjust_bits();
    initMap();
    return;
}

void transmit_worker(
    uhd::tx_streamer::sptr tx_streamer,
    uhd::tx_metadata_t metadata
){
    readerInit();
    int size;
    metadata.start_of_burst = false;
    metadata.has_time_spec = false;
    metadata.end_of_burst = false;
    //send data until the signal handler gets called
    while(not stop_signal_called){
        if(flag==0)
            continue;
        vector<complex<float> > buff;
        uhd::async_metadata_t async_md;
        switch (gen2_logic_status){
            case START:
                for(int i = 0; i < cw_ack.size(); i++)
                    buff.push_back(cw_ack[i]);
                size = tx_streamer->send(&buff.front(), buff.size(), metadata);
                fprintf(stderr, "start send size = %d\n",size);
                if (not tx_streamer->recv_async_msg(async_md)){
                    std::cout << boost::format("failed:\n    Async message recv timed out.\n") << std::endl;
                    continue;
                }
                gen2_logic_status = SEND_QUERY;
                break;

            case SEND_QUERY:      
                decoder_status = DECODER_DECODE_RN16;
                gate_status = GATE_SEEK_RN16;
                for(int i = 0; i < preamble.size(); i++)
                    buff.push_back(preamble[i]);
                for(int i = 0; i < query_bits.size(); i++){
                    if(query_bits[i] == 1)
                        for(int j = 0; j < data_1.size(); j++)
                            buff.push_back(data_1[j]);
                    else
                        for(int j = 0; j < data_0.size(); j++)
                            buff.push_back(data_0[j]);
                }
                for(int i = 0; i < cw_query.size(); i++)
                    buff.push_back(cw_query[i]);
                size = tx_streamer->send(&buff.front(), buff.size(), metadata);
                fprintf(stderr, "query send size = %d\n",size);
                if (not tx_streamer->recv_async_msg(async_md)){
                    std::cout << boost::format("failed:\n    Async message recv timed out.\n") << std::endl;
                    continue;
                }
                gen2_logic_status = IDLE;
                break;

            case SEND_ACK:
                decoder_status = DECODER_DECODE_EPC;
                gate_status = GATE_SEEK_EPC;
                //Send FrameSync
                buff.insert(buff.end(), frame_sync.begin(), frame_sync.end());
                //ack
                buff.insert(buff.end(), data_0.begin(), data_0.end());
                buff.insert(buff.end(), data_1.begin(), data_1.end());
                for(int i = 0; i < RN16_bits.size(); i++){
                    if(RN16_bits[i] == 1){
                        buff.insert(buff.end(), data_1.begin(), data_1.end());
                    }
                    else{
                        buff.insert(buff.end(), data_0.begin(), data_0.end());
                    }           
                }
                buff.insert(buff.end(), cw_ack.begin(), cw_ack.end());
                size = tx_streamer->send(&buff.front(), buff.size(), metadata);
                fprintf(stderr, "ack send size = %d\n",size);
                if (not tx_streamer->recv_async_msg(async_md)){
                    std::cout << boost::format("failed:\n    Async message recv timed out.\n") << std::endl;
                    continue;
                } 
                /*fprintf(stderr, "ack_bits =");
                for(int i=0;i<RN16_bits.size();i++){
                    fprintf(stderr, "%d ",RN16_bits[i]);
                    if(i%4==3)
                        fprintf(stderr,"  ");
                    if(i==RN16_bits.size()-1)
                        fprintf(stderr,"\n");
                }*/
                //}    
                gen2_logic_status = IDLE;
                flag2 = 0;
                break;

            default:
                //IDLEs
                if(flag2==1){
                    gen2_logic_status = SEND_ACK;
                }
                break;
        }
                  
    }
    //send a mini EOB packet
    metadata.end_of_burst = true;
    tx_streamer->send("", 0, metadata);
    return;
}

/***********************************************************************
 * recv_to_file function
 **********************************************************************/
void filter(void){
    int now = 0, size = rx_size/decim;
    //beforeGate.resize(0);
    for(int i=0;i<size;i++){
        complex<float> tmp(0,0);
        if(now<25){
            for(int j=now;j>=0;j--){
                tmp += rx_buff[j];
            }
            for(int j=0;j<24-now;j++){
                tmp += filter_buff[j];
            }
        }
        else{
            for(int j=now;j>=now-24;j--){
                tmp += rx_buff[j];
            }
        } 
        before_gate[i] = tmp;
        now += decim;
    }
    for(int i=0;i<25;i++){
        filter_buff[i] = rx_buff[rx_size-1-i];
    }
    return;
}

void gate_impl(float (&ampl)[2]){
    int n_items = rx_size/decim;
    int n_samples_T1 = T1_D * (s_rate / pow(10,6));
    int n_samples_PW = PW_D * (s_rate / pow(10,6));
    int n_samples_TAG_BIT = TAG_BIT_D * (s_rate / pow(10,6));
    if(gate_status == GATE_SEEK_EPC){
        gate_status = GATE_CLOSED;
        n_samples_to_ungate = (EPC_BITS + TAG_PREAMBLE_BITS) * n_samples_TAG_BIT + 2*n_samples_TAG_BIT;
        n_samples = 0;
    }
    else if (gate_status == GATE_SEEK_RN16){
        gate_status = GATE_CLOSED;
        n_samples_to_ungate = (RN16_BITS + TAG_PREAMBLE_BITS) * n_samples_TAG_BIT + 2*n_samples_TAG_BIT;
        n_samples = 0;
    }
    //afterGate.resize(0);
    for(int i = 0; i < n_items; i++){
        // Tracking average amplitude
        sample_ampl = abs(before_gate[i]);
        avg_ampl = avg_ampl + (sample_ampl - win_samples[win_index])/win_length;
        win_samples[win_index] = sample_ampl; 
        win_index = (win_index + 1) % win_length; 
        //Threshold for detecting negative/positive edges
        sample_thresh = avg_ampl * THRESH_FRACTION;  
        if( !(gate_status == GATE_OPEN) ){
            //Tracking DC offset (only during T1)
            n_samples++;
            // Potitive edge -> Negative edge
            if( sample_ampl < sample_thresh && signal_state == POS_EDGE){
                n_samples = 0;
                signal_state = NEG_EDGE;
            }
            // Negative edge -> Positive edge 
            else if (sample_ampl > sample_thresh && signal_state == NEG_EDGE){
                signal_state = POS_EDGE;
                if (n_samples > n_samples_PW/2)
                    num_pulses++; 
                else
                    num_pulses = 0;
                n_samples = 0;
            }
            if(n_samples > n_samples_T1 && signal_state == POS_EDGE && num_pulses > NUM_PULSES_COMMAND){
                gate_status = GATE_OPEN;
                after_gate.push_back(before_gate[i]); 
                ampl[1] += sample_ampl;
                gate_pre_count += 1;
                num_pulses = 0; 
                n_samples = 1;
                for(int j=i; j>i-n_samples_TAG_BIT; j--){
                    if(j<0){
                        ampl[0] = (ampl[0] / (i+1)) * n_samples_TAG_BIT;
                        break;
                    }
                    else{
                        ampl[0] += abs(before_gate[j]);
                    }
                }
            }
        }
        else{
            n_samples++;
            after_gate.push_back(before_gate[i]); 
            if(gate_pre_count<n_samples_TAG_BIT){
                ampl[1] += sample_ampl;
                gate_pre_count++;
            }
            if (n_samples >= n_samples_to_ungate){
                gate_status = GATE_CLOSED;    
                gate_pre_count = 0;
                break;
            }
        }
    }
    return;
}

int correlate(int n_samples_TAG_BIT, float ampl[2]){
    //choose proper preamble
    int type;
    fprintf(stderr, "cw = %f preamble = %f\n",ampl[0], ampl[1]);
    if(ampl[0]>ampl[1]) 
        type = 1;//negative preamble
    else 
        type = 0;//positive preamble
    //correlation
    int size=after_gate.size(), index=0;
    float max=-10000000;
    float sum = 0.0;
    for (int i=0;i<60;i++)
        sum += abs(after_gate[i]);
    for (int i=0; i<20; i++){
        if(i!=0){
            sum = sum - abs(after_gate[i-1]) + abs(after_gate[i+59]);
        }
        float tmp=0.0, aver=sum/60;
        for (int j=0;j<60;j++){
            tmp += float(PREAMBLE[type][j/5])*(abs(after_gate[i+j])-aver);
        }
        if(tmp>max){
            max = tmp;
            index = i;
        }
    }
    ampl[0] = 0;
    ampl[1] = 0;
    return index;
}

void rn16Decode(int rn16Index){
    vector<int> tmpBits;
    int windowSize=10, now=rn16Index;
    RN16_bits.resize(0);
    //detect +1 or 0 in data
    for (int i=0;i<RN16_BITS*2-2;i++){
        float sum = 0.0, aver;
        for (int j=now-windowSize; j<now+windowSize; j++){
            sum += abs(after_gate[j]);
        }
        aver = sum/(windowSize*2);
        int count = 0;
        for (int j=now; j<now+5; j++)
            if( abs(after_gate[j]) > aver )
                count++;
        if(count>2)
            tmpBits.push_back(1);
        else
            tmpBits.push_back(0);
        now += 5;
    }
    //decode by fm0 encoding
    for (int i=0;i<RN16_BITS*2-2;i+=2){
        if(tmpBits[i]!=tmpBits[i+1])
            RN16_bits.push_back(0);
        else
            RN16_bits.push_back(1);
    }
    after_gate.resize(0);
    return;
}

void epcDecode(int epcIndex){
    vector<int> tmpBits;
    int windowSize=10, now=epcIndex;
    EPC_bits.resize(0);
    //detect +1 or 0 in data
    for (int i=0;i<EPC_BITS*2-2;i++){
        float sum = 0.0, aver;
        for (int j=now-windowSize; j<now+windowSize; j++){
            sum += abs(after_gate[j]);
        }
        aver = sum/(windowSize*2);
        int count = 0;
        for (int j=now; j<now+5; j++)
            if( abs(after_gate[j]) > aver )
                count++;
        if(count>2)
            tmpBits.push_back(1);
        else
            tmpBits.push_back(0);
        now += 5;
    }
    //decode by fm0 encoding
    for (int i=0;i<EPC_BITS*2-2;i+=2){
        if(tmpBits[i]!=tmpBits[i+1])
            EPC_bits.push_back(0);
        else
            EPC_bits.push_back(1);
    }
    after_gate.resize(0);
    return;
}

int check_crc(char * bits, int num_bits){
    register unsigned short i, j;
    register unsigned short crc_16, rcvd_crc;
    unsigned char * data;
    int num_bytes = num_bits / 8;
    data = (unsigned char* )malloc(num_bytes );
    int mask;

    for(i = 0; i < num_bytes; i++){
        mask = 0x80;
        data[i] = 0;
        for(j = 0; j < 8; j++){
            if (bits[(i * 8) + j] == '1'){
                data[i] = data[i] | mask;
            }
            mask = mask >> 1;
        }
    }
    rcvd_crc = (data[num_bytes - 2] << 8) + data[num_bytes -1];

    crc_16 = 0xFFFF; 
    for (i=0; i < num_bytes - 2; i++){
        crc_16^=data[i] << 8;
        for (j=0;j<8;j++){
            if (crc_16&0x8000){
                crc_16 <<= 1;
                crc_16 ^= 0x1021;
            }
            else
                crc_16 <<= 1;
        }
    }
    crc_16 = ~crc_16;

    if(rcvd_crc != crc_16){
        fprintf(stderr, "crc check fail\n");
        return -1;
    }
    else{
        fprintf(stderr, "crc check pass\n");
        return 1;
    }
}

void recv_to_file(
    uhd::usrp::multi_usrp::sptr usrp,
    const std::string &cpu_format,
    const std::string &wire_format,
    const std::string &file,
    size_t samps_per_buff,
    int num_requested_samples,
    float settling_time
){
    struct timeval s;
    //create a receive streamer
    uhd::stream_args_t stream_args(cpu_format,wire_format);
    uhd::rx_streamer::sptr rx_stream = usrp->get_rx_stream(stream_args);

    // Prepare buffers for received samples and metadata
    uhd::rx_metadata_t md;
    
    // Create one ofstream object per channel
    // (use shared_ptr because ofstream is non-copyable)
    ofstream outfile;
    outfile.open("raw_samples.bin", std::ofstream::binary);
    ofstream outfile2;
    outfile2.open("filter_samples.bin", std::ofstream::binary);
    ofstream outfile3;
    outfile3.open("gate_samples.bin", std::ofstream::binary);
    bool overflow_message = true;
    float timeout = settling_time + 0.1f; //expected settling time + padding for first recv

    //setup streaming
    uhd::stream_cmd_t stream_cmd((num_requested_samples == 0)?
        uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS:
        uhd::stream_cmd_t::STREAM_MODE_NUM_SAMPS_AND_DONE
    );
    stream_cmd.num_samps = num_requested_samples;
    stream_cmd.stream_now = true;
    stream_cmd.time_spec = uhd::time_spec_t(settling_time);
    rx_stream->issue_stream_cmd(stream_cmd);
    s_rate = s_rate/decim;
    int n_samples_TAG_BIT = TAG_BIT_D * (s_rate / pow(10,6));
    //rxBuff.resize(2000);
    //0 for cw, 1 for preamble
    float ampl[2] = {0};
    char char_bits[128];

    while(not stop_signal_called and (num_requested_samples == 0)){
        size_t num_rx_samps = rx_stream->recv(&rx_buff.front(), rx_buff.size(), md, timeout);
        // fir_filter_ccc      
        filter();
        //gate
        gate_impl(ampl);
        flag=1;
        if(n_samples != n_samples_to_ungate){
            outfile.write((const char*)&rx_buff.front(), num_rx_samps*sizeof(complex<float>));  
            continue;   
        }
        //rn16
        if(decoder_status==DECODER_DECODE_RN16){
            int rn16Index = correlate(n_samples_TAG_BIT,ampl);
            fprintf(stderr, "rn16 index = %d\n", rn16Index+60);
            if( rn16Index==0 || rn16Index>10 ){
                fprintf(stderr, "rn16 detection failure\n");
                stop_signal_called = true;
                outfile.write((const char*)&rx_buff.front(), num_rx_samps*sizeof(complex<float>));  
                continue;   
            }
            rn16Decode(rn16Index+60);
            flag2 = 1;
        } else if (decoder_status==DECODER_DECODE_EPC){
            int epcIndex = correlate(n_samples_to_ungate,ampl);
            fprintf(stderr, "epc index = %d\n", epcIndex+60);
            if( epcIndex==0 || epcIndex>10 ){
                fprintf(stderr, "epc detection failure\n");
                stop_signal_called = true;
                outfile.write((const char*)&rx_buff.front(), num_rx_samps*sizeof(complex<float>));  
                //outfile2.write((const char*)&before_gate.front(), before_gate.size()*sizeof(complex<float>)); 
                //outfile3.write((const char*)&after_gate.front(), after_gate.size()*sizeof(complex<float>)); 
                continue;   
            }
            outfile3.write((const char*)&after_gate.front(), after_gate.size()*sizeof(complex<float>));
            epcDecode(epcIndex+60);
            fprintf(stderr, "EPC = ");
            for(int i=0;i<EPC_bits.size();i++){
                if (EPC_bits[i] == 0)
                    char_bits[i] = '0';
                else
                    char_bits[i] = '1';
                /*fprintf(stderr, "%d ",EPC_bits[i]);
                if(i%4==3)
                    fprintf(stderr,"  ");
                if(i==EPC_bits.size()-1)
                    fprintf(stderr,"\n");*/
            }
            int result = check_crc(char_bits, EPC_BITS-1);
            string epcCode = "";
            for(int i=16;i<112;i+=4){
                string tmp = "";
                tmp.append(1, char_bits[i]);
                tmp.append(1, char_bits[i+1]);
                tmp.append(1, char_bits[i+2]);
                tmp.append(1, char_bits[i+3]);
                epcCode += mapping[tmp];
            }
            fprintf(stderr, "epc = %s\n", epcCode.c_str());
            stop_signal_called = true;
        }
        outfile.write((const char*)&rx_buff.front(), num_rx_samps*sizeof(complex<float>));  
    }
    // Shut down receiver
    stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
    rx_stream->issue_stream_cmd(stream_cmd);

    // Close files
    if(outfile.is_open())
        outfile.close();
    if(outfile2.is_open())
        outfile2.close();
    if(outfile3.is_open())
        outfile3.close();
    return;
}

/***********************************************************************
 * Main function
 **********************************************************************/
int UHD_SAFE_MAIN(int argc, char *argv[]){
    uhd::set_thread_priority_safe();

    //transmit variables to be set by po
    std::string tx_args, wave_type, tx_ant, tx_subdev, ref, otw, tx_channels;
    double tx_rate, tx_freq, tx_gain, wave_freq, tx_bw;
    float ampl;

    //receive variables to be set by po
    std::string rx_args, file, type, rx_ant, rx_subdev, rx_channels;
    size_t total_num_samps, spb;
    double rx_rate, rx_freq, rx_gain, rx_bw, lo_off;
    float settling;

    //setup the program options
    po::options_description desc("Allowed options");
    desc.add_options()
        ("help", "help message")
        ("tx-args", po::value<std::string>(&tx_args)->default_value("addr=192.168.91.12"), "uhd transmit device address args")
        ("rx-args", po::value<std::string>(&rx_args)->default_value("addr=192.168.91.11"), "uhd receive device address args")
        ("file", po::value<std::string>(&file)->default_value("raw_samples.bin"), "name of the file to write binary samples to")
        //("type", po::value<std::string>(&type)->default_value("float"), "sample type in file: double, float, or short")
        ("nsamps", po::value<size_t>(&total_num_samps)->default_value(0), "total number of samples to receive")
        ("settling", po::value<float>(&settling)->default_value(float(0.5)), "settling time (seconds) before receiving")
        ("spb", po::value<size_t>(&spb)->default_value(4000), "samples per buffer, 0 for default")
        ("tx-rate", po::value<double>(&tx_rate)->default_value(1e6), "rate of transmit outgoing samples")
        ("rx-rate", po::value<double>(&rx_rate)->default_value(2e6), "rate of receive incoming samples")
        ("tx-freq", po::value<double>(&tx_freq)->default_value(910e6), "transmit RF center frequency in Hz")
        ("rx-freq", po::value<double>(&rx_freq)->default_value(910e6), "receive RF center frequency in Hz")
        ("tx-gain", po::value<double>(&tx_gain)->default_value(0), "gain for the transmit RF chain")
        ("rx-gain", po::value<double>(&rx_gain)->default_value(0), "gain for the receive RF chain")
        ("tx-ant", po::value<std::string>(&tx_ant)->default_value("TX/RX"), "transmit antenna selection")
        ("rx-ant", po::value<std::string>(&rx_ant)->default_value("RX2"), "receive antenna selection")
        ("tx-subdev", po::value<std::string>(&tx_subdev)->default_value("A:0"), "transmit subdevice specification")
        ("rx-subdev", po::value<std::string>(&rx_subdev)->default_value("A:0"), "receive subdevice specification")
        ("tx-bw", po::value<double>(&tx_bw), "analog transmit filter bandwidth in Hz")
        ("rx-bw", po::value<double>(&rx_bw), "analog receive filter bandwidth in Hz")
        ("ref", po::value<std::string>(&ref)->default_value("internal"), "clock reference (internal, external, mimo)")
        ("otw", po::value<std::string>(&otw)->default_value("sc16"), "specify the over-the-wire sample mode")
        ("lo_off", po::value<double>(&lo_off), "Offset for frontend LO in Hz (optional)")
        //("tx-channels", po::value<std::string>(&tx_channels)->default_value("0"), "which TX channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
        //("rx-channels", po::value<std::string>(&rx_channels)->default_value("0"), "which RX channel(s) to use (specify \"0\", \"1\", \"0,1\", etc)")
        ("tx-int-n", "tune USRP TX with integer-N tuning")
        ("rx-int-n", "tune USRP RX with integer-N tuning")
    ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    //print the help message
    if (vm.count("help")){
        std::cout << boost::format("UHD TXRX Loopback to File %s") % desc << std::endl;
        return ~0;
    }

    //create a usrp device
    std::cout << std::endl;
    std::cout << boost::format("Creating the transmit usrp device with: %s...") % tx_args << std::endl;
    uhd::usrp::multi_usrp::sptr tx_usrp = uhd::usrp::multi_usrp::make(tx_args);
    std::cout << std::endl;
    std::cout << boost::format("Creating the receive usrp device with: %s...") % rx_args << std::endl;
    uhd::usrp::multi_usrp::sptr rx_usrp = uhd::usrp::multi_usrp::make(rx_args);

    //detect which channels to use
    /*std::vector<std::string> tx_channel_strings;
    std::vector<size_t> tx_channel_nums;
    boost::split(tx_channel_strings, tx_channels, boost::is_any_of("\"',"));
    for(size_t ch = 0; ch < tx_channel_strings.size(); ch++){
        size_t chan = boost::lexical_cast<int>(tx_channel_strings[ch]);
        if(chan >= tx_usrp->get_tx_num_channels()){
            throw std::runtime_error("Invalid TX channel(s) specified.");
        }
        else tx_channel_nums.push_back(boost::lexical_cast<int>(tx_channel_strings[ch]));
    }
    std::vector<std::string> rx_channel_strings;
    std::vector<size_t> rx_channel_nums;
    boost::split(rx_channel_strings, rx_channels, boost::is_any_of("\"',"));
    for(size_t ch = 0; ch < rx_channel_strings.size(); ch++){
        size_t chan = boost::lexical_cast<int>(rx_channel_strings[ch]);
        if(chan >= rx_usrp->get_rx_num_channels()){
            throw std::runtime_error("Invalid RX channel(s) specified.");
        }
        else rx_channel_nums.push_back(boost::lexical_cast<int>(rx_channel_strings[ch]));
    }*/

    //Lock mboard clocks
    tx_usrp->set_clock_source(ref);
    rx_usrp->set_clock_source(ref);

    //always select the subdevice first, the channel mapping affects the other settings
    if (vm.count("tx-subdev")) tx_usrp->set_tx_subdev_spec(tx_subdev);
    if (vm.count("rx-subdev")) rx_usrp->set_rx_subdev_spec(rx_subdev);

    std::cout << boost::format("Using TX Device: %s") % tx_usrp->get_pp_string() << std::endl;
    std::cout << boost::format("Using RX Device: %s") % rx_usrp->get_pp_string() << std::endl;

    //set the transmit sample rate
    if (not vm.count("tx-rate")){
        std::cerr << "Please specify the transmit sample rate with --tx-rate" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting TX Rate: %f Msps...") % (tx_rate/1e6) << std::endl;
    tx_usrp->set_tx_rate(tx_rate);
    std::cout << boost::format("Actual TX Rate: %f Msps...") % (tx_usrp->get_tx_rate()/1e6) << std::endl << std::endl;

    //set the receive sample rate
    if (not vm.count("rx-rate")){
        std::cerr << "Please specify the sample rate with --rx-rate" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting RX Rate: %f Msps...") % (rx_rate/1e6) << std::endl;
    rx_usrp->set_rx_rate(rx_rate);
    std::cout << boost::format("Actual RX Rate: %f Msps...") % (rx_usrp->get_rx_rate()/1e6) << std::endl << std::endl;

    //set the transmit center frequency
    if (not vm.count("tx-freq")){
        std::cerr << "Please specify the transmit center frequency with --tx-freq" << std::endl;
        return ~0;
    }

    uhd::tune_request_t tx_tune_request;
    if(vm.count("lo_off")) tx_tune_request = uhd::tune_request_t(tx_freq, lo_off);
    else tx_tune_request = uhd::tune_request_t(tx_freq);
    if(vm.count("tx-int-n")) tx_tune_request.args = uhd::device_addr_t("mode_n=integer");
    tx_usrp->set_tx_freq(tx_tune_request);
    std::cout << boost::format("Actual TX Freq: %f MHz...") % (tx_usrp->get_tx_freq()/1e6) << std::endl << std::endl;

    //set the rf gain
    if (vm.count("tx-gain")){
        std::cout << boost::format("Setting TX Gain: %f dB...") % tx_gain << std::endl;
        tx_usrp->set_tx_gain(tx_gain);
        std::cout << boost::format("Actual TX Gain: %f dB...") % tx_usrp->get_tx_gain() << std::endl << std::endl;
    }

    //set the analog frontend filter bandwidth
    if (vm.count("tx-bw")){
        std::cout << boost::format("Setting TX Bandwidth: %f MHz...") % tx_bw << std::endl;
        tx_usrp->set_tx_bandwidth(tx_bw);
        std::cout << boost::format("Actual TX Bandwidth: %f MHz...") % tx_usrp->get_tx_bandwidth() << std::endl << std::endl;
    }

    //set the antenna
    if (vm.count("tx-ant")) tx_usrp->set_tx_antenna(tx_ant);

    //set the receive center frequency
    if (not vm.count("rx-freq")){
        std::cerr << "Please specify the center frequency with --rx-freq" << std::endl;
        return ~0;
    }
    std::cout << boost::format("Setting RX Freq: %f MHz...") % (rx_freq/1e6) << std::endl;
    uhd::tune_request_t rx_tune_request(rx_freq);
    if(vm.count("rx-int-n")) rx_tune_request.args = uhd::device_addr_t("mode_n=integer");
    rx_usrp->set_rx_freq(rx_tune_request);
    std::cout << boost::format("Actual RX Freq: %f MHz...") % (rx_usrp->get_rx_freq()/1e6) << std::endl << std::endl;

    //set the receive rf gain
    if (vm.count("rx-gain")){
        std::cout << boost::format("Setting RX Gain: %f dB...") % rx_gain << std::endl;
        rx_usrp->set_rx_gain(rx_gain);
        std::cout << boost::format("Actual RX Gain: %f dB...") % rx_usrp->get_rx_gain() << std::endl << std::endl;
    }

    //set the receive analog frontend filter bandwidth
    if (vm.count("rx-bw")){
        std::cout << boost::format("Setting RX Bandwidth: %f MHz...") % (rx_bw/1e6) << std::endl;
        rx_usrp->set_rx_bandwidth(rx_bw);
        std::cout << boost::format("Actual RX Bandwidth: %f MHz...") % (rx_usrp->get_rx_bandwidth()/1e6) << std::endl << std::endl;
    }
    //set the receive antenna
    if (vm.count("ant")) rx_usrp->set_rx_antenna(rx_ant);

    size_t index = 0;
    //std::cout << "step" << step << std::endl;
    
    //create a transmit streamer
    //linearly map channels (index0 = channel0, index1 = channel1, ...)
    uhd::stream_args_t stream_args("fc32", otw);
    uhd::tx_streamer::sptr tx_stream = tx_usrp->get_tx_stream(stream_args);

    //setup the metadata flags
    uhd::tx_metadata_t md;
    md.start_of_burst = false;
    md.end_of_burst   = false;
    md.has_time_spec  = true;
    md.time_spec = uhd::time_spec_t(0.1); //give us 0.1 seconds to fill the tx 

    //Check Ref and LO Lock detect
    std::vector<std::string> tx_sensor_names, rx_sensor_names;
    tx_sensor_names = tx_usrp->get_tx_sensor_names(0);
    if (std::find(tx_sensor_names.begin(), tx_sensor_names.end(), "lo_locked") != tx_sensor_names.end()) {
        uhd::sensor_value_t lo_locked = tx_usrp->get_tx_sensor("lo_locked",0);
        std::cout << boost::format("Checking TX: %s ...") % lo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(lo_locked.to_bool());
    }
    rx_sensor_names = rx_usrp->get_rx_sensor_names(0);
    if (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "lo_locked") != rx_sensor_names.end()) {
        uhd::sensor_value_t lo_locked = rx_usrp->get_rx_sensor("lo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % lo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(lo_locked.to_bool());
    }

    tx_sensor_names = tx_usrp->get_mboard_sensor_names(0);
    if ((ref == "mimo") and (std::find(tx_sensor_names.begin(), tx_sensor_names.end(), "mimo_locked") != tx_sensor_names.end())) {
        uhd::sensor_value_t mimo_locked = tx_usrp->get_mboard_sensor("mimo_locked",0);
        std::cout << boost::format("Checking TX: %s ...") % mimo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(mimo_locked.to_bool());
    }
    if ((ref == "external") and (std::find(tx_sensor_names.begin(), tx_sensor_names.end(), "ref_locked") != tx_sensor_names.end())) {
        uhd::sensor_value_t ref_locked = tx_usrp->get_mboard_sensor("ref_locked",0);
        std::cout << boost::format("Checking TX: %s ...") % ref_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(ref_locked.to_bool());
    }

    rx_sensor_names = rx_usrp->get_mboard_sensor_names(0);
    if ((ref == "mimo") and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "mimo_locked") != rx_sensor_names.end())) {
        uhd::sensor_value_t mimo_locked = rx_usrp->get_mboard_sensor("mimo_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % mimo_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(mimo_locked.to_bool());
    }
    if ((ref == "external") and (std::find(rx_sensor_names.begin(), rx_sensor_names.end(), "ref_locked") != rx_sensor_names.end())) {
        uhd::sensor_value_t ref_locked = rx_usrp->get_mboard_sensor("ref_locked",0);
        std::cout << boost::format("Checking RX: %s ...") % ref_locked.to_pp_string() << std::endl;
        UHD_ASSERT_THROW(ref_locked.to_bool());
    }

    if (total_num_samps == 0){
        signal(SIGINT, &sig_int_handler);
        std::cout << "Press Ctrl + C to stop streaming..." << std::endl;
    }

    //reset usrp time to prepare for transmit/receive
    std::cout << boost::format("Setting device timestamp to 0...") << std::endl;
    tx_usrp->set_time_now(uhd::time_spec_t(0.0));

    //start transmit worker thread
    boost::thread_group transmit_thread;
    transmit_thread.create_thread(boost::bind(&transmit_worker, tx_stream, md));

    //recv to file
    recv_to_file(rx_usrp, "fc32", otw, file, spb, total_num_samps, settling);

    //clean up transmit worker
    stop_signal_called = true;
    transmit_thread.join_all();

    //finished
    std::cout << std::endl << "Done!" << std::endl << std::endl;
    return EXIT_SUCCESS;
}
