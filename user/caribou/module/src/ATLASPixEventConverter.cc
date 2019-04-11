#include "CaribouEvent2StdEventConverter.hh"
#include "log.hpp"

using namespace eudaq;

namespace{
  auto dummy0 = eudaq::Factory<eudaq::StdEventConverter>::
  Register<ATLASPixEvent2StdEventConverter>(ATLASPixEvent2StdEventConverter::m_id_factory);
}

uint32_t ATLASPixEvent2StdEventConverter::gray_decode(uint32_t gray) const {
  uint32_t bin = gray;
  while(gray >>= 1) {
    bin ^= gray;
  }
  return bin;
}

uint64_t ATLASPixEvent2StdEventConverter::readout_ts_(0);
uint64_t ATLASPixEvent2StdEventConverter::fpga_ts_(0);
uint64_t ATLASPixEvent2StdEventConverter::fpga_ts1_(0);
uint64_t ATLASPixEvent2StdEventConverter::fpga_ts2_(0);
uint64_t ATLASPixEvent2StdEventConverter::fpga_ts3_(0);
bool ATLASPixEvent2StdEventConverter::new_ts1_(false);
bool ATLASPixEvent2StdEventConverter::new_ts2_(false);
bool ATLASPixEvent2StdEventConverter::timestamps_cleared_(false);
uint32_t ATLASPixEvent2StdEventConverter::clkdivend2M_(7);
double ATLASPixEvent2StdEventConverter::clockcycle_(8);
bool ATLASPixEvent2StdEventConverter::Converting(eudaq::EventSPC d1, eudaq::StandardEventSP d2, eudaq::ConfigurationSPC conf) const{
  auto ev = std::dynamic_pointer_cast<const eudaq::RawEvent>(d1);

  // No event
  if(!ev || ev->NumBlocks() < 1) {
    std::cout << "no event" << std::endl;
    return false;
  }

  // Read file and load data
  auto datablock = ev->GetBlock(0);
  uint32_t datain;
  memcpy(&datain, &datablock[0], sizeof(uint32_t));

  // Check if current word is a pixel data:
  if(datain & 0x80000000) {
    // Do not return and decode pixel data before T0 arrived
    if(!timestamps_cleared_) {
      std::cout << "!timestamps_cleared_" << std::endl;
      return false;
    }
    std::cout << "Finally: pixel data AND timestamps_cleared_ = true" << std::endl;
    // Structure: {1'b1, column_addr[5:0], row_addr[8:0], rise_timestamp[9:0], fall_timestamp[5:0]}
    // Extract pixel data
    long ts2 = gray_decode((datain)&0x003F);
    // long ts2 = gray_decode((datain>>6)&0x003F);
    // TS1 counter is by default half speed of TS2. By multiplying with 2 we make it equal.
    long ts1 = (gray_decode((datain >> 6) & 0x03FF)) << 1;
    // long ts1 = (gray_decode(((datain << 4) & 0x3F0) | ((datain >> 12) & 0xF)))<<1;
    int row = ((datain >> (6 + 10)) & 0x01FF);
    int col = ((datain >> (6 + 10 + 9)) & 0x003F);
    // long tot = 0;

    long long ts_diff = ts1 - static_cast<long long>(readout_ts_ & 0x07FF);

    if(ts_diff > 0) {
      // Hit probably came before readout started and meanwhile an OVF of TS1 happened
      if(ts_diff > 0x01FF) {
        ts_diff -= 0x0800;
      }
    } else {
      // Hit probably came after readout started and after OVF of TS1.
      if(ts_diff < (0x01FF - 0x0800)) {
        ts_diff += 0x0800;
      }
    }

    long long hit_ts = static_cast<long long>(readout_ts_) + ts_diff;

    // Convert the timestamp to nanoseconds:
    double timestamp = clockcycle_ * static_cast<double>(hit_ts);

    // calculate ToT only when pixel is good for storing (division is time consuming)
    int tot = static_cast<int>(ts2 - ((hit_ts % static_cast<long long>(64 * clkdivend2M_)) / clkdivend2M_));
    if(tot < 0) {
      tot += 64;
    }
    // convert ToT to nanoseconds
    // double tot_ns = tot * m_clockCycle;

    LOG(DEBUG) << "HIT: TS1: " << ts1 << "\t0x" << std::hex << ts1 << "\tTS2: " << ts2 << "\t0x" << std::hex << ts2
    << "\tTS_FULL: " << hit_ts << "\t" << timestamp << "ns"
    << "\tTOT: " << tot;
    std::cout << "HIT: TS1: " << ts1 << "=0x" << std::hex << ts1 << "\tTS2: " << std::dec << ts2 << "=0x" << std::hex << ts2
    << "\tTS_FULL: " << std::dec << hit_ts << "\t" << timestamp << "ns"
    << "\tTOT: " << std::dec << tot << std::dec << "\trow: " << row << "\tcol: " << col << std::endl;

    // Create a StandardPlane representing one sensor plane
    eudaq::StandardPlane plane(0, "Caribou", "ATLASpix");
    plane.SetSizeZS(25, 400, 1);
    plane.SetPixel(0, col, row, tot, timestamp);

    // Add the plane to the StandardEvent
    d2->AddPlane(plane);

    // Store frame begin and end:
    d2->SetTimeBegin(timestamp);
    d2->SetTimeEnd(timestamp);

    std::cout << "--> fpga_ts_ = " << fpga_ts_ << std::endl;
    return true;
  } else {
    // data is not hit information
    // Decode the message content according to 8 MSBits
    unsigned int message_type = (datain >> 24);
    LOG(DEBUG) << "Message type " << std::hex << message_type << std::dec;
    //std::cout << "Message type " << std::hex << message_type << std::dec << std::endl;

    if(message_type == 0b01000000) {
      // Timestamp from ATLASpix [23:0]
      uint64_t atp_ts = (datain >> 7) & 0x1FFFE;
      long long ts_diff = static_cast<long long>(atp_ts) - static_cast<long long>(fpga_ts_ & 0x1FFFF);

      if(ts_diff > 0) {
        if(ts_diff > 0x10000) {
          ts_diff -= 0x20000;
        }
      } else {
        if(ts_diff < -0x1000) {
          ts_diff += 0x20000;
        }
      }
      readout_ts_ = static_cast<unsigned long long>(static_cast<long long>(fpga_ts_) + ts_diff);
      LOG(DEBUG) << "RO_ts " << std::hex << readout_ts_ << " atp_ts " << atp_ts << std::dec;
      std::cout << "RO_ts " << std::hex << readout_ts_ << " atp_ts " << atp_ts << std::dec << std::endl;
    } else if(message_type == 0b00010000) {
      // Trigger counter from FPGA [23:0] (1/4)
      // do nothing here?
      std::cout << "Do nothing!" << std::endl;
    } else if(message_type == 0b00110000) {
      // Trigger counter from FPGA [31:24] and timestamp from FPGA [63:48] (2/4)
      fpga_ts1_ = ((static_cast<unsigned long long>(datain) << 48) & 0xFFFF000000000000);
      new_ts1_ = true;
    } else if(message_type == 0b00100000) {

      // Timestamp from FPGA [47:24] (3/4)
      uint64_t fpga_tsx = ((static_cast<unsigned long long>(datain) << 24) & 0x0000FFFFFF000000);
      if((!new_ts1_) && (fpga_tsx < fpga_ts2_)) {
        fpga_ts1_ += 0x0001000000000000;
        LOG(DEBUG) << "Missing TS_FPGA_1, adding one";
        std::cout << "Missing TS_FPGA_1, adding one" << std::endl;
      }
      new_ts1_ = false;
      new_ts2_ = true;
      fpga_ts2_ = fpga_tsx;
    } else if(message_type == 0b01100000) {

      // Timestamp from FPGA [23:0] (4/4)
      uint64_t fpga_tsx = ((datain)&0xFFFFFF);
      if((!new_ts2_) && (fpga_tsx < fpga_ts3_)) {
        fpga_ts2_ += 0x0000000001000000;
        LOG(DEBUG) <<"Missing TS_FPGA_2, adding one";
        std::cout <<"Missing TS_FPGA_2, adding one" << std::endl;
      }
      new_ts2_ = false;
      fpga_ts3_ = fpga_tsx;
      fpga_ts_ = fpga_ts1_ | fpga_ts2_ | fpga_ts3_;
      std::cout << "fpga_ts_ = " << fpga_ts_ << std::endl;
    } else if(message_type == 0b00000010) {
      // BUSY was asserted due to FIFO_FULL + 24 LSBs of FPGA timestamp when it happened
    } else if(message_type == 0b01110000) {
      // T0 received
      new_ts2_ = new_ts1_ = true;
      fpga_ts1_ = fpga_ts2_ = fpga_ts3_ = 0;
      LOG(DEBUG) << "Another T0 event was found in the data";
      std::cout << "########Another T0 event was found in the data" << std::endl;
      timestamps_cleared_ = true;
    } else if(message_type == 0b00000000) {

      // Empty data - should not happen
      LOG(DEBUG) << "EMPTY_DATA";
      std::cout << "EMTPY DATA" << std::endl;
    } else {

      // Other options...
      // LOG(DEBUG) << "...Other";
      // Unknown message identifier
      if(message_type & 0b11110010) {
        LOG(DEBUG) << "UNKNOWN_MESSAGE";
        std::cout << "UNKNOWN_MESSAGE" << std::endl;
      } else {
        // Buffer for chip data overflow (data that came after this word were lost)
        if((message_type & 0b11110011) == 0b00000001) {
          LOG(DEBUG) << "BUFFER_OVERFLOW";
          std::cout << "BUFFER_OVERFLOW" << std::endl;
        }
        // SERDES lock established (after reset or after lock lost)
        if((message_type & 0b11111110) == 0b00001000) {
          LOG(DEBUG) << "SERDES_LOCK_ESTABLISHED";
          std::cout << "SERDES_LOCK_ESTABLISHED" << std::endl;
        }
        // SERDES lock lost (data might be nonsense, including up to 2 previous messages)
        else if((message_type & 0b11111110) == 0b00001100) {
          LOG(DEBUG) << "SERDES_LOCK_LOST";
          std::cout << "SERDES_LOCK_LOST" << std::endl;
        }
        // Unexpected data came from the chip or there was a checksum error.
        else if((message_type & 0b11111110) == 0b00000100) {
          LOG(DEBUG) << "WEIRD_DATA";
          std::cout << "WEIRD_DATA" << std::endl;
        }
        // Unknown message identifier
        else {
          LOG(DEBUG) << "UNKNOWN_MESSAGE";
          std::cout << "UNKNOWN_MESSAGE" << std::endl;
        }
      }
    } // else
  }
  // This way we always return false -> decoding failed when data != hit information
  // Tha's okay but "decoding failed" only means "no pixel data"

  //std::cout << "--> fpga_ts_ = " << fpga_ts_ << std::endl;

  return false;
}
