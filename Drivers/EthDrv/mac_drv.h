#ifndef ETH_MAC_DRV_H_
#define ETH_MAC_DRV_H_

#include <stdbool.h>
#include <stm32h5xx.h>

#define MODEINIT_HALF_DUPLEX (0)
#define MODEINIT_FULL_DUPLEX (1 << 8)
#define MODEINIT_SPEED_10MBPS (0)
#define MODEINIT_SPEED_100MBPS (1)

// MUST BE 4-BYTE ALIGNED!
typedef struct {
    uint16_t nextTxDescIdx; // index of next available TX descriptor
    uint16_t txCntSent;     // sequence number of last transmitted packet
    uint16_t txCntAcked;    // last transmission acknowledged by interrupt
    uint16_t pad0;
} ETHHW_State;

typedef struct {
    uint16_t rxRingLen, txRingLen;  // RX and TX descriptor ring length
    uint8_t *rxRingPtr, *txRingPtr; // pointer to RX and TX descriptor buffers
    uint8_t *bufPtr;                // pointer to RX and TX buffer area
    uint16_t blockSize;             // size of a single buffer
    uint8_t mac[6];                 // MAC-address
    ETHHW_State *statePtr;          // area where ETHHW state is stored, MUST immediately precede rxRingPtr!
} ETHHW_InitOpts;

/* volatile, because the Ethernet DMA writes these behind the compiler's back.
 *
 * Without it the ownership retry loop in ETHHW_Transmit() is loop-invariant: the body has no
 * barrier, no call and no side effect, so above -Og the compiler hoists the DES3 load and the
 * 100 000-iteration wait collapses to a single test. Measured in the disassembly -- at -O2 and
 * -O3 the retry is simply absent, and a descriptor still held by the DMA means the frame is
 * dropped on the spot. That matters because CMakeLists.txt offers -O2 and -Os as tested
 * alternatives and wires -O2 into RelWithDebInfo (finding N2).
 *
 * Same lesson as F62, which is why that one says a workaround for a hardware access
 * constraint must be expressed with `volatile` or it is not a constraint at all. The
 * extension below is CPU-only and stays plain. */
typedef struct {
    volatile uint32_t DES0, DES1, DES2, DES3; // descriptor DWORDS
} ETHHW_Desc;

// descriptor extension
// SIZE OF THIS MUST BE DIVISIBLE BY 4!!!
typedef struct {
    uint32_t bufAddr; // buffer address to restore
    uint16_t txCntr;  // transmit counter
    // RX: descriptor has been handed to the receive thread and not yet returned. The OWN bit
    // alone cannot express this -- it stays clear from the moment the MAC writes the frame
    // until ETHHW_RestoreRXDesc() runs in thread context, so without this flag an RX
    // interrupt arriving in that window re-delivers frames already queued (F61).
    uint16_t inFlight;
    uint32_t tsCbPtr; // pointer to timestamp callback function
    uint32_t tsCbArg; // user-defined timestamp parameter
} ETHHW_DescExt;

// full descriptor (DMA descriptor + extension)
// SIZE OF THIS MUST BE DIVISIBLE BY 4!!!
typedef struct {
    ETHHW_Desc desc;   // descriptor
    ETHHW_DescExt ext; // extension
} ETHHW_DescFull;

typedef enum {
    ETHHW_EVT_RX_NOTFY,
    ETHHW_EVT_RX_READ,
    ETHHW_EVT_TX_DONE
} ETHHW_EventType; // TODO shouldn't start with zero

typedef struct {
    uint8_t type; // event type
    union {
        struct {
            uint16_t size;  // packet size
            void *payload;  // pointer to received data
            uint32_t ts_s;  // timestamp seconds
            uint32_t ts_ns; // timestamp nanoseconds
        } rx;
        struct {
            uint32_t tag;   // some arbitrary tag
            uint32_t ts_s;  // timestamp seconds
            uint32_t ts_ns; // timestamp nanoseconds
        } tx;
    } data;
    ETHHW_DescFull * bd; // associated normal descriptor
    ETHHW_DescFull * bd_ctx; // associated context descriptor
} ETHHW_EventDesc;

#define ETHHW_RET_RX_PROCESSED (1)

typedef enum {
    ETHHW_TXOPT_NONE = 0b00,
    ETHHW_TXOPT_INTERRUPT_ON_COMPLETION = 0b01,
    ETHHW_TXOPT_CAPTURE_TS = 0b11
} ETHHW_TxOpt;

typedef struct {
    uint32_t txTsCbPtr; // pointer to callback function invoked on transmission completion
    uint32_t tag;       // arbitrary tagging
} ETHHW_OptArg_TxTsCap;

void ETHHW_Init(ETH_TypeDef *eth, ETHHW_InitOpts *init);
void ETHHW_Start(ETH_TypeDef *eth);
void ETHHW_Transmit(ETH_TypeDef *eth, const uint8_t *buf, uint16_t len, uint8_t txOpts, void *txOptArgs);

void ETHHW_ProcessRx(ETH_TypeDef *eth);
void ETHHW_RestoreRXDesc(ETHHW_DescFull *bd);

void ETHHW_SetLinkProperties(ETH_TypeDef *eth, bool fastEthernet, bool fullDuplex);

void ETHHW_ISR(ETH_TypeDef *eth);

uint32_t ETHHW_ReadPHYRegister(ETH_TypeDef *eth, uint32_t PHYAddr, uint32_t PHYReg, uint32_t *pRegValue);
uint32_t ETHHW_WritePHYRegister(ETH_TypeDef *eth, uint32_t PHYAddr, uint32_t PHYReg, uint32_t RegValue);

/* ---- PTP CAPABILITIES ---- */

typedef enum {
    ETHHW_PTP_PPS_OFF = 0,
    ETHHW_PTP_PPS_1Hz = 1,
    ETHHW_PTP_PPS_2Hz,
    ETHHW_PTP_PPS_4Hz,
    ETHHW_PTP_PPS_8Hz,
    ETHHW_PTP_PPS_16Hz,
    ETHHW_PTP_PPS_32Hz,
    ETHHW_PTP_PPS_64Hz,
    ETHHW_PTP_PPS_128Hz,
    ETHHW_PTP_PPS_256Hz,
    ETHHW_PTP_PPS_512Hz,
    ETHHW_PTP_PPS_1024Hz,
    ETHHW_PTP_PPS_2048Hz,
    ETHHW_PTP_PPS_4096Hz,
    ETHHW_PTP_PPS_8192Hz,
    ETHHW_PTP_PPS_16384Hz
} ETHHW_PPS_FreqEnum;

void ETHHW_EnablePTPTimeStamping(ETH_TypeDef *eth);                                          // Enable PTP timestamping. NOT every frame: TSENALL is deliberately clear, so only PTP messages selected by SNAPTYPSEL are stamped -- see the note at the definition
void ETHHW_DisablePTPTimeStamping(ETH_TypeDef *eth);                                         // Disable PTP timestamping
void ETHHW_InitPTPTime(ETH_TypeDef *eth, uint32_t sec, uint32_t nsec);                       // Initialize PTP clock time
void ETHHW_EnablePTPFineCorr(ETH_TypeDef *eth, bool enFineCorr);                             // Enable fine correction method
uint32_t ETHHW_GetPTPAddend(ETH_TypeDef *eth);                                               // Get PTP addend
bool ETHHW_SetPTPAddend(ETH_TypeDef *eth, uint32_t addend);                                  // Set PTP addend; false if the hardware did not accept it
void ETHHW_SetPTPPPSFreq(ETH_TypeDef *eth, uint32_t freqCode);                               // Set PPS output frequency
uint32_t ETHHW_GetPTPSubsecondIncrement(ETH_TypeDef *eth);                                   // Get PTP clock subsecond increment
void ETHHW_SetPTPSubsecondIncrement(ETH_TypeDef *eth, uint8_t increment);                    // Set PTP clock subsecond increment, in whole nanoseconds. (0.467 ns is the BINARY-rollover quantum; TSCTRLSSR is set here, so rollover is digital and SSINC counts ns.)

void ETHHW_AuxTimestampCh(ETH_TypeDef *eth, uint8_t ch, bool en);                       // Enable auxiliary timestamping channel
void ETHHW_ReadLastAuxTimestamp(ETH_TypeDef *eth, uint32_t *ps, uint32_t *pns);         // Read lastly captured auxiliary timestamp
void ETHHW_ClearAuxTimestampFIFO(ETH_TypeDef *eth);                                     // Clear auxiliary timestamp FIFO
uint8_t ETHHW_GetAuxTimestampCnt(ETH_TypeDef *eth);                                     // Get number of available auxiliary snapshots
void ETHHW_StartPTPPPSPulseTrain(ETH_TypeDef *eth, uint32_t high_len, uint32_t period); // Generate PPS signal using the pulse train feature
void ETHHW_StopPTPPPSPulseTrain(ETH_TypeDef *eth);                                      // Stop generating a pulse train

#endif /* ETH_MAC_DRV_H_ */
