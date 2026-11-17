// ============================================================================
//  PdkTool (Windows) - Porsche 970 PDK 3000 (TC1766) bench/OBD tool
//  Standalone Win32 app, no Qt. Talks KWP2000-on-CAN (ISO 14230 over ISO 15765)
//  through a Tactrix OpenPort 2.0 J2534 driver (op20pt32.dll).
//
//  Build (from macOS with mingw-w64):
//    i686-w64-mingw32-g++ -O2 -static -std=c++17 -mwindows pdktool.cpp -o PdkTool.exe
//        (+ -lcomdlg32 -lgdi32 -luser32 -lkernel32)
//
//  Own-vehicle repair tool. SecurityAccess seed->key = ASAM_seedkey_PDK
//  (verified bit-for-bit vs PIWIS over all 65536 seeds).
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#ifdef PDK_EMBED_MITTEN
#include "mittenbin.h"          // private build only: Porsche data, not distributed
#else
static const uint32_t MITTEN_97000_SIZE = 15872, MITTEN_97000_CRC32 = 0x811C0A13;  // 970.618.970.00
static const uint32_t MITTEN_97100_SIZE = 15872, MITTEN_97100_CRC32 = 0xF6C74A85;  // 970.618.971.00
#endif
static uint32_t crc32buf(const uint8_t *p, size_t n) {
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++){ c^=p[i]; for(int k=0;k<8;k++) c=(c>>1)^(0xEDB88320u&(0u-(c&1))); }
    return ~c;
}
#include "dtc_table.h"
#include "dtc_ff.h"
#include "teachin_conditions.h"
#include "platforms.h"
#include "eeprom_decode.h"
#include "clutch_decode.h"
#include "usage_decode.h"
#include "bits3063.h"
static std::string exeDir();   // fwd (defined with the high-level tasks)
static std::string stamp();    // fwd

// ---------------------------------------------------------------------------
// J2534 minimal definitions
// ---------------------------------------------------------------------------
#define PASSTHRU_MSG_DATA_SIZE 4128
typedef struct {
    unsigned long ProtocolID, RxStatus, TxFlags, Timestamp, DataSize, ExtraDataIndex;
    unsigned char Data[PASSTHRU_MSG_DATA_SIZE];
} PASSTHRU_MSG;

#define J_CAN                5
#define J_ISO15765           6
#define J_STATUS_NOERROR     0
#define J_FLOW_CONTROL_FILTER 3
#define J_ISO15765_FRAME_PAD 0x40
#define J_TX_MSG_TYPE        0x01
#define J_START_OF_MESSAGE   0x02
#define J_CLEAR_RX_BUFFER    0x08

// J2534 PassThru* functions are declared WINAPI (__stdcall). On 32-bit Windows
// the calling convention MUST match or the stack is corrupted (crash). This was
// the cause of the crash the moment PassThruOpen/Close actually ran (CONNECTED).
typedef long (__stdcall *PF_Open)(const void*, unsigned long*);
typedef long (__stdcall *PF_Close)(unsigned long);
typedef long (__stdcall *PF_Connect)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long*);
typedef long (__stdcall *PF_Disconnect)(unsigned long);
typedef long (__stdcall *PF_ReadMsgs)(unsigned long, PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *PF_WriteMsgs)(unsigned long, const PASSTHRU_MSG*, unsigned long*, unsigned long);
typedef long (__stdcall *PF_StartMsgFilter)(unsigned long, unsigned long, const PASSTHRU_MSG*,
                                  const PASSTHRU_MSG*, const PASSTHRU_MSG*, unsigned long*);
typedef long (__stdcall *PF_Ioctl)(unsigned long, unsigned long, const void*, void*);

static HMODULE       g_dll = nullptr;
static PF_Open       pOpen = nullptr;
static PF_Close      pClose = nullptr;
static PF_Connect    pConnect = nullptr;
static PF_Disconnect pDisconnect = nullptr;
static PF_ReadMsgs   pReadMsgs = nullptr;
static PF_WriteMsgs  pWriteMsgs = nullptr;
static PF_StartMsgFilter pStartMsgFilter = nullptr;
static PF_Ioctl      pIoctl = nullptr;
static std::string   g_dllPath;      // resolved J2534 library path

// Find the installed J2534 driver DLL via the standard registry entry
//   HKLM\SOFTWARE\PassThruSupport.04.04\<vendor>\FunctionLibrary
// (read from the 32-bit view, where op20pt32.dll registers). Prefers Tactrix.
static std::string findJ2534Library() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\PassThruSupport.04.04",0,
                      KEY_READ|KEY_WOW64_32KEY,&hk)!=ERROR_SUCCESS) return "";
    char name[256]; DWORD idx=0; std::string best, firstAny;
    for(;;){ DWORD nlen=sizeof(name);
        if (RegEnumKeyExA(hk,idx++,name,&nlen,0,0,0,0)!=ERROR_SUCCESS) break;
        HKEY sub;
        if (RegOpenKeyExA(hk,name,0,KEY_READ|KEY_WOW64_32KEY,&sub)!=ERROR_SUCCESS) continue;
        char lib[512]; DWORD lsz=sizeof(lib),type=0;
        if (RegQueryValueExA(sub,"FunctionLibrary",0,&type,(LPBYTE)lib,&lsz)==ERROR_SUCCESS){
            std::string path(lib); if(firstAny.empty()) firstAny=path;
            std::string low=path; for(char&ch:low) ch=(char)tolower((unsigned char)ch);
            if (low.find("op20")!=std::string::npos||low.find("tactrix")!=std::string::npos||
                low.find("openport")!=std::string::npos) best=path;
        }
        RegCloseKey(sub);
    }
    RegCloseKey(hk);
    return best.empty()?firstAny:best;
}
static HMODULE loadJ2534Dll() {
    if (g_dll) return g_dll;
    std::string p = findJ2534Library();
    if (!p.empty()){ g_dll = LoadLibraryA(p.c_str()); if(g_dll) g_dllPath=p; }
    if (!g_dll){ g_dll = LoadLibraryA("op20pt32.dll"); if(g_dll) g_dllPath="op20pt32.dll"; }
    return g_dll;
}

// ---------------------------------------------------------------------------
// GUI plumbing
// ---------------------------------------------------------------------------
enum {
    IDC_TX = 1001, IDC_RX, IDC_LOG, IDC_BACKUP, IDC_RESTORE,
    ID_IDENTIFY, ID_GETSEED, ID_UNLOCK, ID_READEEP, ID_READ13, ID_READ14,
    ID_TEACH_S, ID_TEACH_C, ID_TEACH_H, ID_FF1872, ID_RESTOREWR, ID_STOP,
    ID_BROWSE_B, ID_BROWSE_R, ID_SELFTEST, ID_DETECT,
    ID_DIAGNOSE, ID_READDONOR, ID_TEACHALL, ID_LIVEROD, ID_MARK, ID_CLEARDTC, ID_ANALYZEEE, ID_VALVETEST, IDC_VALVESEL, ID_FULLBACKUP, IDC_PLATFORM
};
#define TIMER_DEVCHECK 1
// Vehicle / TCU family. 0 = Panamera 970 PDK 3000 (full support, firmware RE),
// 1 = 911 991 / Boxster-Cayman 981 / 718, 2 = 918 Spyder (level 1: PIWIS database only).
static int g_platform = 0;
static const char *PLAT_NAME[3] = {"Panamera 970 (2010-2016) - full support",
                                   "911 (991) / Boxster, Cayman (981, 718) - basic",
                                   "918 Spyder - basic"};
static HWND g_hPlat = nullptr;
static HWND g_hLog = nullptr, g_hTx = nullptr, g_hRx = nullptr,
           g_hBackup = nullptr, g_hRestore = nullptr, g_hMain = nullptr,
           g_hStatus = nullptr;
static volatile bool g_stop = false;
static volatile bool g_busy = false;
static volatile bool g_detecting = false;
static volatile bool g_detVerbose = false;
static volatile long g_mark = 0;     // MARK button counter (live rod logging)
static HWND g_hValve = nullptr;      // valve selector (actuator test)
static int g_valveSel = 0;
static int g_devState = -1;   // -1 unknown, 0 not found, 1 present
#define WM_APP_LOG      (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_DEVSTATE (WM_APP + 3)

static void logLine(const std::string &s) {
    char *buf = (char*)malloc(s.size() + 1);
    memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g_hMain, WM_APP_LOG, 0, (LPARAM)buf);
}
static void logBlock(const std::string &t) {   // log a multi-line text, one line per '\n'
    size_t a = 0, e; while ((e = t.find('\n', a)) != std::string::npos) { logLine(t.substr(a, e - a)); a = e + 1; }
    if (a < t.size()) logLine(t.substr(a));
}
static std::string hex(const unsigned char *d, int n) {
    static const char *H = "0123456789ABCDEF";
    std::string s; for (int i = 0; i < n; i++) { s += H[d[i]>>4]; s += H[d[i]&0xF]; s += ' '; }
    return s;
}

// ---------------------------------------------------------------------------
// J2534 ISO-TP (KWP2000 over CAN)
// ---------------------------------------------------------------------------
struct Bus {
    unsigned long dev = 0, chan = 0, filt = 0;
    unsigned long txId = 0x7E1, rxId = 0x7E9;
    bool connected = false;
    uint8_t lastNrc = 0;     // last negative response code (0 = none)

    bool load() {
        if (pOpen) return true;
        if (!loadJ2534Dll()) { logLine("[ERROR] J2534 driver (op20pt32.dll) not found. Install Tactrix drivers."); return false; }
        pOpen=(PF_Open)GetProcAddress(g_dll,"PassThruOpen");
        pClose=(PF_Close)GetProcAddress(g_dll,"PassThruClose");
        pConnect=(PF_Connect)GetProcAddress(g_dll,"PassThruConnect");
        pDisconnect=(PF_Disconnect)GetProcAddress(g_dll,"PassThruDisconnect");
        pReadMsgs=(PF_ReadMsgs)GetProcAddress(g_dll,"PassThruReadMsgs");
        pWriteMsgs=(PF_WriteMsgs)GetProcAddress(g_dll,"PassThruWriteMsgs");
        pStartMsgFilter=(PF_StartMsgFilter)GetProcAddress(g_dll,"PassThruStartMsgFilter");
        pIoctl=(PF_Ioctl)GetProcAddress(g_dll,"PassThruIoctl");
        if (!pOpen||!pConnect||!pReadMsgs||!pWriteMsgs||!pStartMsgFilter)
        { logLine("[ERROR] J2534 entry points missing in DLL."); return false; }
        return true;
    }
    static void putId(PASSTHRU_MSG &m, unsigned long id) {
        m.Data[0]=(id>>24)&0xFF; m.Data[1]=(id>>16)&0xFF; m.Data[2]=(id>>8)&0xFF; m.Data[3]=id&0xFF;
    }
    bool open(unsigned long tx, unsigned long rx) {
        if (!load()) return false;
        txId=tx; rxId=rx;
        if (pOpen(nullptr,&dev)!=J_STATUS_NOERROR){ logLine("[ERROR] PassThruOpen failed."); return false; }
        if (pConnect(dev,J_ISO15765,0,500000,&chan)!=J_STATUS_NOERROR){ logLine("[ERROR] PassThruConnect failed."); return false; }
        PASSTHRU_MSG mask={0},patt={0},flow={0};
        mask.ProtocolID=patt.ProtocolID=flow.ProtocolID=J_ISO15765;
        mask.TxFlags=patt.TxFlags=flow.TxFlags=J_ISO15765_FRAME_PAD;
        mask.DataSize=patt.DataSize=flow.DataSize=4;
        putId(mask,rxId); putId(patt,rxId); putId(flow,txId);
        if (pStartMsgFilter(chan,J_FLOW_CONTROL_FILTER,&mask,&patt,&flow,&filt)!=J_STATUS_NOERROR)
        { logLine("[ERROR] StartMsgFilter failed."); return false; }
        connected=true; logLine("[OK] Bus connected."); return true;
    }
    void close() {
        if (connected && pDisconnect) pDisconnect(chan);
        if (dev && pClose) pClose(dev);
        connected=false; dev=0; chan=0;
    }
    bool send(const std::vector<uint8_t> &p) {
        PASSTHRU_MSG m={0}; m.ProtocolID=J_ISO15765; m.TxFlags=J_ISO15765_FRAME_PAD;
        putId(m,txId); for (size_t i=0;i<p.size();i++) m.Data[4+i]=p[i];
        m.DataSize=4+(unsigned long)p.size();
        unsigned long num=1;
        return pWriteMsgs(chan,&m,&num,1000)==J_STATUS_NOERROR;
    }
    // read one response payload (strips 4-byte CAN id). returns payload or empty.
    bool recv(std::vector<uint8_t> &out, unsigned long timeoutMs) {
        DWORD t0=GetTickCount();
        while (GetTickCount()-t0 < timeoutMs) {
            PASSTHRU_MSG m={0}; unsigned long num=1;
            long st=pReadMsgs(chan,&m,&num,200);
            if (st!=J_STATUS_NOERROR || num==0) continue;
            if (m.RxStatus & J_TX_MSG_TYPE) continue;          // tx echo
            if (m.DataSize<=4) continue;                        // indication only
            unsigned long id=(m.Data[0]<<24)|(m.Data[1]<<16)|(m.Data[2]<<8)|m.Data[3];
            if (id!=rxId) continue;
            out.assign(m.Data+4, m.Data+m.DataSize);
            return true;
        }
        return false;
    }
    // send + wait for a positive response whose first byte == want (e.g. SID+0x40).
    bool xfer(const std::vector<uint8_t> &p, uint8_t want, std::vector<uint8_t> &out, unsigned long tmo) {
        if (!send(p)) return false;
        DWORD t0=GetTickCount();
        while (GetTickCount()-t0 < tmo) {
            if (!recv(out, tmo)) return false;
            if (!out.empty() && out[0]==want) return true;
            if (out.size()>=3 && out[0]==0x7F) {                 // negative response
                logLine("[NRC] "+hex(out.data(),(int)out.size()));
                lastNrc = out[2];
                // 0x78 = responsePending -> keep waiting
                if (out[2]==0x78) continue;
                return false;
            }
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// PDK 3000 seed->key  (ASAM_seedkey_PDK, verified over all 65536 seeds)
// ---------------------------------------------------------------------------
static uint16_t pdkKey16(uint16_t s16) {
    int rot=0,op=0;
    if (s16&0x0040) rot+=1; if (s16&0x0400) rot+=2; if (s16&0x0004) rot+=4; if (s16&0x2000) rot+=8;
    if (s16&0x0200) op+=1;  if (s16&0x4000) op+=2;
    uint32_t w=s16;
    if (s16&0x0010) { for(int i=0;i<rot;i++){uint32_t b=w&0x8000; w=(w<<1)&0xFFFE; if(b)w|=1;} }
    else            { for(int i=0;i<rot;i++){uint32_t b=w&1;      w=(w>>1)&0x7FFF; if(b)w|=0x8000;} }
    switch(op){case 0:w=s16&w;break;case 1:w=s16|w;break;case 2:w=s16^w;break;case 3:w=~s16;break;}
    return (uint16_t)(w&0xFFFF);
}

// ---------------------------------------------------------------------------
// KWP services
// ---------------------------------------------------------------------------
static bool startSession(Bus &b, uint8_t ss) { std::vector<uint8_t> r; return b.xfer({0x10,ss},0x50,r,3000); }
static void testerPresent(Bus &b){ b.send({0x3E,0x01}); }

static bool securityAccess(Bus &b, uint8_t level) {
    std::vector<uint8_t> r;
    if (!b.xfer({0x27,level},0x67,r,2000)) { logLine("[SEC] no seed response."); return false; }
    if (r.size()<4) { logLine("[SEC] seed too short: "+hex(r.data(),(int)r.size())); return false; }
    uint16_t seed=(r[2]<<8)|r[3];
    uint16_t key=pdkKey16(seed);
    char buf[64]; sprintf(buf,"[SEC] seed=%04X key=%04X",seed,key); logLine(buf);
    std::vector<uint8_t> snd={(uint8_t)0x27,(uint8_t)(level+1),(uint8_t)(key>>8),(uint8_t)(key&0xFF)};
    if (!b.xfer(snd,0x67,r,2000)) { logLine("[SEC] key rejected."); return false; }
    logLine("[SEC] unlocked."); return true;
}

// ---------------------------------------------------------------------------
// Memory access (firmware RE, re_findings/kwp_services.md):
//  Application: 23 MT AH AL LEN, RAM only (0xD0000000 + 16-bit offset; MT 1 up to
//  0xD000D18B, MT 2 above), session 0x86 after SecurityAccess (10 89 -> 27 -> 10 86).
//  Flasher (entered with 10 85 after SecurityAccess): 23 A2 A1 A0 LEN, address 0xE00000 /
//  0xE10000 = DFlash (EEPROM) sector 0/1; 35 A2 A1 A0 00 S2 S1 S0 + repeated "36" + "37"
//  reads flash (block 0x13 = 0x01C000, 0x3E00 bytes). After the flasher the ECU needs
//  an ignition cycle.
// ---------------------------------------------------------------------------
static bool ramRead(Bus &b, uint32_t addr, uint8_t len, std::vector<uint8_t> &out) {
    if (addr<0xD0000000u || addr+len>0xD000E000u || len==0) return false;
    uint32_t off=addr-0xD0000000u; uint8_t mt=(addr<=0xD000D18Bu)?1:2;
    std::vector<uint8_t> r;
    if (!b.xfer({0x23,mt,(uint8_t)(off>>8),(uint8_t)off,len},0x63,r,1500)) return false;
    if ((int)r.size()<1+len) return false;
    out.assign(r.begin()+1,r.begin()+1+len); return true;
}
static bool enterRamSession(Bus &b) {          // 10 89 -> unlock -> 10 86
    startSession(b,0x89); Sleep(150);
    if (!securityAccess(b,0x01)) { logLine("[RAM] unlock failed."); return false; }
    if (!startSession(b,0x86)) { logLine("[RAM] session 86 refused."); return false; }
    Sleep(100); return true;
}
static bool g_inFlasher=false;
static bool enterFlasher(Bus &b) {             // 10 89 -> unlock -> 10 85 (flasher)
    startSession(b,0x89); Sleep(150);
    if (!securityAccess(b,0x01)) { logLine("[FLASH] unlock failed."); return false; }
    b.lastNrc=0;
    if (!startSession(b,0x85)) {
        logLine("[FLASH] programming session refused (car must stand still, engine off/ignition on).");
        return false; }
    g_inFlasher=true; Sleep(800);
    securityAccess(b,0x01);                     // flasher may ask again; harmless if not
    logLine("[FLASH] TCU is now in the flasher. Ignition OFF for 10 s afterwards to return to normal.");
    return true;
}
static bool flashRead(Bus &b, uint32_t a24, uint8_t len, std::vector<uint8_t> &out) {
    std::vector<uint8_t> r;
    if (!b.xfer({0x23,(uint8_t)(a24>>16),(uint8_t)(a24>>8),(uint8_t)a24,len},0x63,r,2000)) return false;
    if ((int)r.size()<1+len) return false;
    out.assign(r.begin()+1,r.begin()+1+len); return true;
}

static void saveFile(const std::string &path, const std::vector<uint8_t> &d) {
    FILE *f=fopen(path.c_str(),"wb"); if(!f){logLine("[SAVE] cannot open "+path);return;}
    fwrite(d.data(),1,d.size(),f); fclose(f); logLine("[SAVE] "+path+" ("+std::to_string(d.size())+" B)");
}

// ---------------------------------------------------------------------------
// EEPROM analysis (offline): record checksums, identification, learned data.
// Format and CRCs from firmware RE (re_findings/eeprom_map.md), verified on 2 dumps.
// ---------------------------------------------------------------------------
static void analyzeEeprom(const std::vector<uint8_t> &d, const std::string &src) {
    logLine("=== EEPROM ANALYSIS: "+src+" ===");
    pdkee::Parsed p;
    if (!pdkee::parse(d.data(),d.size(),p)) { logLine("[EE] cannot parse: "+p.error); return; }
    int okb=0, badb=0; std::string badl;
    for (auto &kv : p.blocks) { if (kv.second.innerOk) okb++; else { badb++; badl+=" "+std::to_string(kv.first); } }
    char m[200];
    sprintf(m,"[EE] records %d (bad %d), image slots valid %d, blocks %d OK / %d bad%s",
            p.records,p.badRecords,p.imageSlotsValid,okb,badb,badb?(" (block"+badl+")").c_str():"");
    logLine(m);
    logLine(badb||p.badRecords ? "[EE] CHECKSUM PROBLEM in the EEPROM - the TCU may reset these blocks (P174E possible)."
                               : "[EE] All checksums OK.");
    pdkee::Ident id;
    if (pdkee::identOf(p,id)) {
        logLine("[EE] VIN (positions 6,7,9 stored as _): "+id.vin);
        logLine("[EE] SW "+id.sw+"  variant "+id.swVariant+"  market "+id.market+"  (RW01 = Rest of World, US01 = USA)");
        logLine("[EE] part no "+id.partNo+"  HW "+id.hwString+"  last programmed "+id.progDate);
    }
    pdkee::RodData rd;
    if (pdkee::rodsOf(p,rd)) {
        logLine(rd.taught ? "[EE] Shift rods: TAUGHT (marker 0xA55A present)"
                          : "[EE] Shift rods: NOT TAUGHT - default values (7.5 / 2.0 mm). A full TEACH-IN is needed.");
        logLine("         rod   neutral    end+   synchro+    end-   synchro-   [mm]");
        for (int r=0;r<4;r++){ sprintf(m,"          %d   %+7.2f  %+7.2f  %+7.2f  %+7.2f  %+7.2f",r+1,
                                       rd.v[r][0],rd.v[r][1],rd.v[r][2],rd.v[r][3],rd.v[r][4]); logLine(m); }
        sprintf(m,"[EE] teach-in flags 0x%04X 0x%04X",rd.flagsA,rd.flagsB); logLine(m);
    }
    // TCU history / usage (blocks 5, 2, 9, 11; see re_findings/usage_stats.md)
    { auto pl=[&](int id,size_t skip){ std::vector<uint8_t> v; auto it=p.blocks.find(id);
          if(it!=p.blocks.end() && it->second.payload.size()>skip) v.assign(it->second.payload.begin()+skip,it->second.payload.end());
          return v; };
      logBlock(pdkuse::usageReport(pl(5,6),pl(2,2),pl(9,2),pl(11,2))); }
    { auto it4=p.blocks.find(4);
      if (it4!=p.blocks.end() && it4->second.payload.size()>=6+0x7B0) {
        const uint8_t *b=it4->second.payload.data()+6;
        sprintf(m,"[EE] launch-control (race start) counters: %u / %u",b[0x494]|(b[0x495]<<8),b[0x496]|(b[0x497]<<8)); logLine(m);
        logBlock(pdkclu::clutchReport(std::vector<uint8_t>(b,b+(it4->second.payload.size()-6)))); } }
    std::string rep; int bad=pdkee::checkSubBlocks(p,rep);
    logLine(std::string("[EE] adaptation sub-blocks")+(bad>0?" (some BAD):":":"));
    size_t a=0,b2; while((b2=rep.find('\n',a))!=std::string::npos){ logLine(rep.substr(a,b2-a)); a=b2+1; }
    logLine("[EE] Summary: "+pdkee::teachSummary(p));
    logLine("[EE] Donor swap: the VIN is not checked by the TCU. Same SW + same market as the old unit ->");
    logLine("     the old EEPROM can be cloned; otherwise (or if not taught) run TEACH-IN after installing.");
}

// ---------------------------------------------------------------------------
// Actions (run on worker thread)
// ---------------------------------------------------------------------------
static unsigned long readIds(Bus &b) {
    char t[32]; GetWindowTextA(g_hTx,t,sizeof(t)); b.txId=strtoul(t,0,16);
    GetWindowTextA(g_hRx,t,sizeof(t)); b.rxId=strtoul(t,0,16);
    return 0;
}
static void getPath(HWND h, std::string &out){ char t[512]; GetWindowTextA(h,t,sizeof(t)); out=t; }

static void actIdentify(Bus &b) {
    startSession(b,0x81); Sleep(150);
    std::vector<uint8_t> r;
    if (b.xfer({0x1A,0x9B},0x5A,r,2000)) logLine("[IDENT] "+hex(r.data()+2,(int)r.size()-2));
    if (b.xfer({0x18,0x00,0xFF,0x00},0x58,r,3000)) {
        logLine("[DTC] "+hex(r.data(),(int)r.size()));
        // 58 <count> [DTC_hi DTC_lo status] ... -> DTC records start at index 2
        for (size_t i=2;i+1<r.size();i+=3){ uint16_t d=(r[i]<<8)|r[i+1]; const char*n=0;
            if(d==0x186F)n="elektr. Abgleichdaten ungueltig"; else if(d==0x1870)n="Abgleich Schaltstangen ungueltig";
            else if(d==0x1871)n="Abgleich Kupplungen ungueltig"; else if(d==0x1872)n="Abgleich Hydraulik ungueltig / Hyti-TeachIn";
            if(n){char buf[96];sprintf(buf,"[DTC] P%04X  %s",d,n);logLine(buf);} }
    }
}
static void actGetSeed(Bus &b) {
    startSession(b,0x89); Sleep(150); std::vector<uint8_t> r;
    for (uint8_t lvl : {(uint8_t)0x01,(uint8_t)0x03}) {
        if (b.xfer({0x27,lvl},0x67,r,2000)) {
            uint16_t seed=(r.size()>=4)?((r[2]<<8)|r[3]):0;
            char buf[80]; sprintf(buf,"[SEED] level %02X  seed=%s -> key=%04X",lvl,hex(r.data()+2,(int)r.size()-2).c_str(),pdkKey16(seed));
            logLine(buf);
        }
        Sleep(200);
    }
}
// Learned data from the RAM mirror of the EEPROM (works in the normal application):
// block 4 adaptation buffer 0xD000C790 (0xA00 B; rods record at +0x208, marker 0xA55A at
// +0x238), block 8 identification 0xD000D91C (0xA4 B).
static std::vector<uint8_t> g_b5hist;   // RAM image of EEPROM block 5 from 0xD000D190 (0x108 B)
static void analyzeRamMirror(const std::vector<uint8_t> &b4, const std::vector<uint8_t> &b8) {
    logLine("=== LEARNED DATA (EEPROM mirror in RAM) ===");
    char m[200];
    if (b8.size()>=0xA4) {
        auto f=[&](uint32_t ram,int len){ std::string s; for(int i=0;i<len;i++){char c=(char)b8[ram-0xD000D91C+i]; s+=(c>=32&&c<127)?c:'.';} return s; };
        logLine("[EE] VIN (positions 6,7,9 stored as _): "+f(0xD000D950,17));
        logLine("[EE] SW "+f(0xD000D962,8)+"  variant "+f(0xD000D96E,8)+"  market "+f(0xD000D976,4)+"  part no "+f(0xD000D9A6,11));
    }
    if (b4.size()>=0x23A) {
        bool taught = (b4[0x238] | (b4[0x239]<<8))==0xA55A;
        logLine(taught? "[EE] Shift rods: TAUGHT" : "[EE] Shift rods: NOT TAUGHT - default values. A full TEACH-IN is needed.");
        logLine("         rod   neutral    end+   synchro+    end-   synchro-   [mm]");
        for(int r=0;r<4;r++){ double v[5]; for(int k=0;k<5;k++) v[k]=(int16_t)(b4[0x208+12*r+2*k]|(b4[0x209+12*r+2*k]<<8))/256.0;
            sprintf(m,"          %d   %+7.2f  %+7.2f  %+7.2f  %+7.2f  %+7.2f",r+1,v[0],v[1],v[2],v[3],v[4]); logLine(m); }
        sprintf(m,"[EE] teach-in flags 0x%04X 0x%04X",b4[0x1E6]|(b4[0x1E7]<<8),b4[0x1E8]|(b4[0x1E9]<<8)); logLine(m);
    }
    if (b4.size()>=0x7B0) {
        sprintf(m,"[EE] launch-control (race start) counters: %u / %u",b4[0x494]|(b4[0x495]<<8),b4[0x496]|(b4[0x497]<<8)); logLine(m);
        logBlock(pdkclu::clutchReport(b4));
    }
    if (g_b5hist.size()>=0x108) logBlock(pdkuse::usageReport(g_b5hist,{},{},{}));
}
static bool readRamMirror(Bus &b, std::vector<uint8_t> &b4, std::vector<uint8_t> &b8) {
    if (!enterRamSession(b)) return false;
    b4.clear(); b8.clear(); std::vector<uint8_t> part;
    for (uint32_t off=0; off<0xA00; off+=0xF0) {
        uint8_t n=(uint8_t)((0xA00-off<0xF0)?(0xA00-off):0xF0);
        if (!ramRead(b,0xD000C790+off,n,part)) { logLine("[EE] RAM read refused - see NRC above."); return false; }
        b4.insert(b4.end(),part.begin(),part.end()); testerPresent(b);
    }
    if (ramRead(b,0xD000D91C,0xA4,part)) b8=part;
    g_b5hist.clear();
    { std::vector<uint8_t> p1,p2; if (ramRead(b,0xD000D190,0x84,p1) && ramRead(b,0xD000D214,0x84,p2)) {
          g_b5hist=p1; g_b5hist.insert(g_b5hist.end(),p2.begin(),p2.end()); } }
    return true;
}
static void actReadEeprom(Bus &b) {
    std::string path; getPath(g_hBackup,path);
    if (path.empty()) path=exeDir()+"\\PDK_"+stamp();
    std::vector<uint8_t> b4,b8;
    if (readRamMirror(b,b4,b8)) {
        std::vector<uint8_t> all=b4; all.insert(all.end(),b8.begin(),b8.end());
        saveFile(path+"_eeprom_ram.bin",all); analyzeRamMirror(b4,b8);
    }
}
// Full DFlash (EEPROM) dump via the flasher: sectors 0xE00000 and 0xE10000, 16 KB each.
// VERIFIED: the flasher read-range table (0x8002D620, 7 entries) allows 0x8FE00000-0x8FE03FFF
// and 0x8FE10000-0x8FE13FFF (plus PFlash, calibration, RAM, PSPR).
static bool readDflash(Bus &b, std::vector<uint8_t> &all) {
    all.clear(); std::vector<uint8_t> part;
    for (uint32_t sec : {0xE00000u,0xE10000u}) {
        for (uint32_t off=0; off<0x4000; off+=0x40) {   // LEN must stay below the flasher's max block
            if (g_stop) return false;
            if (!flashRead(b,sec+off,0x40,part)) {
                char e[96]; sprintf(e,"[EE] flasher refused DFlash read at 0x%06X (see NRC).",sec+off); logLine(e); return false; }
            all.insert(all.end(),part.begin(),part.end());
            if ((off&0xFFF)==0){ char pr[48]; sprintf(pr,"[EE] DFlash %u %%",(unsigned)((all.size()*100)/0x8000)); logLine(pr); }
        }
    }
    return true;
}
// FULL BACKUP via OBD (read-only): flasher 0x23 reads the whole PFlash image (0x000000-0x177FFF;
// 0x040000-0x07FFFF is mapped to the calibration sector 0xA0040000, the rest to 0x80000000+A)
// and both DFlash (EEPROM) sectors. Same file layout as a bench (boot-mode) backup.
static void actFullBackup(Bus &b) {
    logLine("=== FULL BACKUP (flash + EEPROM) via OBD ===");
    std::string pre=exeDir()+"\\PDK_full_"+stamp();
    if (!enterFlasher(b)) return;
    std::vector<uint8_t> img, part; img.reserve(0x178000);
    uint8_t L=0x40; DWORD t0=GetTickCount();
    for (uint32_t a=0; a<0x178000; a+=L) {
        if (g_stop) { logLine("[BACKUP] stopped."); return; }
        if (!flashRead(b,a,L,part)) {
            if (L==0x40 && a==0) { L=0x20; a-=L; continue; }        // smaller block if 0x40 is too long
            char e[80]; sprintf(e,"[BACKUP] read refused at 0x%06X - stopping.",a); logLine(e); break; }
        img.insert(img.end(),part.begin(),part.end());
        if ((a & 0x7FFF)==0) { char pr[96]; DWORD el=(GetTickCount()-t0)/1000;
            sprintf(pr,"[BACKUP] flash %u %%  (%lu s)",(unsigned)(100ull*a/0x178000),(unsigned long)el); logLine(pr); testerPresent(b); }
    }
    if (img.size()==0x178000) {
        saveFile(pre+"_IntFl.bin",img);
        auto has=[&](const char*t){ return std::search(img.begin(),img.end(),t,t+strlen(t))!=img.end(); };
        logLine(std::string("[BACKUP] software: ")+(has("ZGXD8800")?"G088":has("ZGXD7000")?"G070":"unknown")+
                (crc32buf(img.data()+0x1C000,0x3E00)==MITTEN_97000_CRC32?", block 0x13 = Mitten Bin 970.618.970.00":
                 crc32buf(img.data()+0x1C000,0x3E00)==MITTEN_97100_CRC32?", block 0x13 = Mitten Bin 970.618.971.00":""));
    }
    std::vector<uint8_t> ee;
    if (readDflash(b,ee)) { saveFile(pre+"_EEprom.bin",ee); analyzeEeprom(ee,pre+"_EEprom.bin"); }
    logLine("[BACKUP] done. Ignition OFF for 10 s now (the TCU is still in the flasher).");
}

// Block 0x13 upload in the flasher: 35 01 C0 00 00 00 3E 00, then "36" until done, then "37".
static const uint32_t BLK13_SIZE = 0x3E00;
static std::vector<uint8_t> buildUpload13() { return {0x35,0x01,0xC0,0x00,0x00,0x00,0x3E,0x00}; }
static bool uploadBlock13(Bus &b, std::vector<uint8_t> &all) {
    all.clear(); std::vector<uint8_t> r;
    if (!b.xfer(buildUpload13(),0x75,r,5000)) { logLine("[UPLOAD] RequestUpload refused (see NRC)."); return false; }
    for (int i=0;i<400 && all.size()<BLK13_SIZE && !g_stop;i++){
        std::vector<uint8_t> td;
        if (!b.xfer({0x36},0x76,td,3000)) break;
        if (td.size()<=1) break;
        all.insert(all.end(),td.begin()+1,td.end());
        if ((i&0x1F)==0){ char pr[48]; sprintf(pr,"[UPLOAD] %d bytes",(int)all.size()); logLine(pr); }
    }
    b.xfer({0x37},0x77,r,5000);
    if (all.size()>BLK13_SIZE) all.resize(BLK13_SIZE);
    return all.size()==BLK13_SIZE;
}

// ---------------------------------------------------------------------------
// Teach-in (firmware RE, SW G070/G088 - see re_findings/teachin_thresholds.md):
//   LID 0x20 shift rods, 0x21 clutches, 0x22 hydraulics; request data = exactly the LID.
//   33 <LID> -> 73 LID status cond[u32 BE] steps[u32 BE] progress v1..v4[u16 BE]
//   status 0 not started, 1 running, 2 finished, 3 start conditions not met, 7 aborted.
//   Condition bit = 1 means OK. Start refused -> NRC 0x22 + condition word in 33.
// ---------------------------------------------------------------------------
static void printTeachConds(uint8_t lid, const std::vector<uint8_t> &r, bool onlyBad) {
    if (r.size()<7) return;
    uint32_t c=((uint32_t)r[3]<<24)|((uint32_t)r[4]<<16)|((uint32_t)r[5]<<8)|r[6];
    if (g_platform!=0) {                       // ZF condition texts from the PIWIS database
        const PlatCond *t; int n; bool p1=(g_platform==1);
        if(lid==TEACH_LID_RODS){t=p1?COND_9X1_RODS:COND_918_RODS; n=p1?PLAT_N(COND_9X1_RODS):PLAT_N(COND_918_RODS);}
        else if(lid==TEACH_LID_CLUTCH){t=p1?COND_9X1_CLUTCH:COND_918_CLUTCH; n=p1?PLAT_N(COND_9X1_CLUTCH):PLAT_N(COND_918_CLUTCH);}
        else {t=p1?COND_9X1_HYDR:COND_918_HYDR; n=p1?PLAT_N(COND_9X1_HYDR):PLAT_N(COND_918_HYDR);}
        int bad=0;
        for(int i=0;i<n;i++){ bool ok=(c>>t[i].bit)&1; if(!ok) bad++;
            if(!onlyBad || !ok){ char b2[24]; sprintf(b2,"   [bit %2d = %d] ",t[i].bit,ok?1:0); logLine(b2+std::string(t[i].text)); } }
        char m[160]; sprintf(m,"   condition word 0x%08X, %d bit(s) at 0. On the 970, 1 = OK and 0 = this condition stops the routine;",c,bad); logLine(m);
        logLine("   the same is expected on this model but not yet confirmed.");
        return;
    }
    const TeachCond *t; int n;
    if(lid==TEACH_LID_RODS){t=TEACH_RODS;n=TEACH_COUNT(TEACH_RODS);}
    else if(lid==TEACH_LID_CLUTCH){t=TEACH_CLUTCH;n=TEACH_COUNT(TEACH_CLUTCH);}
    else {t=TEACH_HYDR;n=TEACH_COUNT(TEACH_HYDR);}
    int bad=0;
    for(int i=0;i<n;i++){ bool ok=(c>>t[i].bit)&1; if(!ok) bad++;
        if(!onlyBad || !ok) logLine(std::string("   [")+(ok?"OK ":"!! ")+"] "+t[i].text); }
    char m[64]; sprintf(m,"   condition word 0x%08X, %d not OK",c,bad); logLine(m);
}
static void printTeachValues(uint8_t lid, const std::vector<uint8_t> &r) {
    if (r.size()<20) return;
    int v[4]; for(int i=0;i<4;i++) v[i]=(int16_t)((r[12+2*i]<<8)|r[13+2*i]);
    char m[160];
    if(lid==TEACH_LID_RODS) sprintf(m,"   rods %+6.2f %+6.2f %+6.2f %+6.2f mm | progress %u",v[0]/256.0,v[1]/256.0,v[2]/256.0,v[3]/256.0,r[11]);
    else if(lid==TEACH_LID_CLUTCH) sprintf(m,"   input shaft 1/2 %d %d | pressure 1/2 raw %d %d | progress %u",v[0],v[1],v[2],v[3],r[11]);
    else sprintf(m,"   pressure-regulator currents raw %d %d %d %d | progress %u",v[0],v[1],v[2],v[3],r[11]);
    logLine(m);
}
static bool actTeach(Bus &b, uint8_t lid) {
    const char *nm = lid==TEACH_LID_RODS?"shift rods":lid==TEACH_LID_CLUTCH?"clutches":"hydraulics";
    logLine(std::string("[TEACHIN] start ")+nm);
    std::vector<uint8_t> r; bool started=false;
    for (uint8_t ss : {(uint8_t)0x89,(uint8_t)0x86}) {   // 31 works in 89/85/86; 85 = flasher, never used
        if (ss==0x86) { if(!enterRamSession(b)) break; } else { startSession(b,ss); Sleep(150); }
        b.lastNrc=0;
        if (b.xfer({0x31,lid},0x71,r,5000)) { started=true; break; }
        if (b.lastNrc==0x22) {                                     // conditionsNotCorrect
            logLine("[TEACHIN] start refused: start conditions not met:");
            if (b.xfer({0x33,lid},0x73,r,3000)) printTeachConds(lid,r,false);
            return false;
        }
        if (g_stop) return false;
    }
    if (!started) { logLine("[TEACHIN] TCU did not accept the routine in any session - send me the log."); return false; }
    int status=1; uint32_t lastSteps=0xFFFFFFFF;
    for (int t=0;t<600 && !g_stop;t++){
        Sleep(1000); testerPresent(b);
        if (!b.xfer({0x33,lid},0x73,r,3000) || r.size()<3) continue;
        status=r[2];
        uint32_t steps = r.size()>=11 ? (((uint32_t)r[7]<<24)|((uint32_t)r[8]<<16)|((uint32_t)r[9]<<8)|r[10]) : 0;
        if (steps!=lastSteps || (t%10)==0){
            char m[96]; sprintf(m,"[TEACHIN] %3ds  %s  steps 0x%08X",t,teachStatusText((unsigned char)status),steps); logLine(m);
            printTeachValues(lid,r); lastSteps=steps; }
        if (status!=1) break;
    }
    if (g_stop || status==1) { b.xfer({0x32,lid},0x72,r,3000); logLine(std::string("[TEACHIN] stopped: ")+nm); return false; }
    if (status==2) { logLine(std::string("[TEACHIN] FINISHED OK: ")+nm); printTeachValues(lid,r); return true; }
    logLine(std::string("[TEACHIN] FAILED (")+teachStatusText((unsigned char)status)+"): "+nm+" - conditions:");
    printTeachConds(lid,r,false);
    return false;
}
static bool actTeach(Bus &b, const char *which) {
    return actTeach(b, !strcmp(which,"s")?TEACH_LID_RODS:!strcmp(which,"c")?TEACH_LID_CLUTCH:TEACH_LID_HYDR);
}
// ---------------------------------------------------------------------------
// Block 0x13 CHECK ("KD Mitten Bin" / donor dataset).
// The flasher writes block 0x13 with 34 01 C0 00 00 00 3E 00 + "36 <data>" + "37", but only
// after an erase routine (0x31 option not identified yet - flag 0xD000B76D bit 3). So this
// tool does NOT write: it reads the block via the flasher, saves it and compares it with
// the Mitten Bin / a chosen file. Both known TCUs already hold Mitten Bin 970.618.970.00.
// ---------------------------------------------------------------------------
static std::string exeDir();
// Mitten Bin source: built in (private build) or a .bin file next to PdkTool.exe, recognised by CRC32.
static bool getMitten(int which, std::vector<uint8_t> &img) {
    const uint32_t want = which==1 ? MITTEN_97000_CRC32 : MITTEN_97100_CRC32;
#ifdef PDK_EMBED_MITTEN
    (void)want;
    if (which==1) img.assign(MITTEN_97000,MITTEN_97000+MITTEN_97000_SIZE);
    else          img.assign(MITTEN_97100,MITTEN_97100+MITTEN_97100_SIZE);
    return true;
#else
    WIN32_FIND_DATAA fd; HANDLE h=FindFirstFileA((exeDir()+"\\*.bin").c_str(),&fd);
    if (h!=INVALID_HANDLE_VALUE) { do {
        std::string path=exeDir()+"\\"+fd.cFileName; FILE*f=fopen(path.c_str(),"rb"); if(!f) continue;
        std::vector<uint8_t> d(0x4000); size_t n=fread(d.data(),1,d.size(),f); fclose(f);
        if ((n==0x3E00 || n==0x4000) && crc32buf(d.data(),0x3E00)==want) { d.resize(0x3E00); img=d; FindClose(h);
            logLine("[0x13] using Mitten Bin file "+path); return true; }
    } while (FindNextFileA(h,&fd)); FindClose(h); }
    logLine(std::string("[0x13] Mitten Bin ")+(which==1?"970.618.970.00":"970.618.971.00")+" not found. Put the .bin "
            "(15872 or 16384 bytes, from your own PIWIS / a TCU backup) next to PdkTool.exe.");
    return false;
#endif
}
static int g_restoreSrc = 0;   // 0 = file in "Write file", 1 = Mitten Bin 970.618.970.00, 2 = 970.618.971.00
static bool loadRestoreImage(std::vector<uint8_t> &img, std::string &what) {
    if (g_restoreSrc==1){ if(!getMitten(1,img)) return false;
        what="Porsche KD Mitten Bin 970.618.970.00 (MY 2010-2013)"; }
    else if (g_restoreSrc==2){ if(!getMitten(2,img)) return false;
        what="Porsche KD Mitten Bin 970.618.971.00 (MY 2014+)"; }
    else {
        std::string path; getPath(g_hRestore,path);
        FILE*f=fopen(path.c_str(),"rb"); if(!f){logLine("[0x13] cannot open file: "+path);return false;}
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        img.resize(sz>0?sz:0); if(sz>0) fread(img.data(),1,sz,f); fclose(f);
        what="file "+path;
        if (sz==0x4000) img.resize(BLK13_SIZE);
        else if (sz!=(long)BLK13_SIZE){ logLine("[0x13] wrong size "+std::to_string(sz)+" bytes (need 15872 or 16384)."); return false; }
    }
    return true;
}
bool writeSelfTest() {
    bool ok = MITTEN_97000_SIZE==BLK13_SIZE && MITTEN_97100_SIZE==BLK13_SIZE;
#ifdef PDK_EMBED_MITTEN
    ok = ok && crc32buf(MITTEN_97000,MITTEN_97000_SIZE)==MITTEN_97000_CRC32 &&
               crc32buf(MITTEN_97100,MITTEN_97100_SIZE)==MITTEN_97100_CRC32;
#endif
    std::vector<uint8_t> u=buildUpload13();
    const uint8_t want[8]={0x35,0x01,0xC0,0x00,0x00,0x00,0x3E,0x00};
    return ok && u.size()==8 && memcmp(u.data(),want,8)==0;
}
static void actRestore(Bus &b) {
    logLine("=== CHECK BLOCK 0x13 ===");
    std::vector<uint8_t> img; std::string what;
    if (!loadRestoreImage(img,what)) return;
    if (!enterFlasher(b)) return;
    std::vector<uint8_t> cur;
    if (!uploadBlock13(b,cur)) { logLine("[0x13] could not read block 0x13 (see log)."); return; }
    saveFile(exeDir()+"\\PDK_"+stamp()+"_block13.bin",cur);
    { uint32_t c=crc32buf(cur.data(),BLK13_SIZE);
      if (c==MITTEN_97000_CRC32) logLine("[0x13] TCU block 0x13 = Porsche Mitten Bin 970.618.970.00.");
      else if (c==MITTEN_97100_CRC32) logLine("[0x13] TCU block 0x13 = Porsche Mitten Bin 970.618.971.00."); }
    size_t diff=0; for(size_t i=0;i<BLK13_SIZE;i++) if(cur[i]!=img[i]) diff++;
    if (!diff) logLine("[0x13] Identical to "+what+". Nothing to do - next step: TEACH-IN.");
    else { logLine("[0x13] Differs from "+what+" in "+std::to_string(diff)+" bytes.");
           logLine("[0x13] Writing is not offered: the flasher's erase routine is not identified yet. Send me this log."); }
    logLine("[0x13] Ignition OFF for 10 s now (the TCU is still in the flasher).");
}

// ---------------------------------------------------------------------------
// Built-in Self-Test (no car): a mock PDK ECU answers KWP over a fake bus and
// exercises session + SecurityAccess(real key) + RequestUpload stream/terminate
// + locked-refusal + DTC parse. Lets you confirm the exe runs and the logic is
// correct on your Windows machine before going to the vehicle.
// ---------------------------------------------------------------------------
struct MockBus {
    uint16_t lastSeed=0; bool unlocked=false; bool uploading=false;
    uint8_t upBlock=0; uint32_t upRemain=0; int seedReq=0;
    std::vector<uint8_t> handle(const std::vector<uint8_t>&p){
        if(p.empty())return{};
        switch(p[0]){
        case 0x10:return{0x50,(uint8_t)(p.size()>1?p[1]:0)};
        case 0x3E:return{0x7E,0x01};
        case 0x27:
            if(p[1]&1){seedReq++;lastSeed=0x1234+seedReq*0x1111;
                return{0x67,p[1],(uint8_t)(lastSeed>>8),(uint8_t)(lastSeed&0xFF)};}
            else{uint16_t w=pdkKey16(lastSeed),g=(p.size()>=4)?((p[2]<<8)|p[3]):0;
                if(g==w){unlocked=true;return{0x67,p[1]};}return{0x7F,0x27,0x35};}
        case 0x18:return{0x58,0x02,0x18,0x72,0x20,0x18,0x70,0x20};
        case 0x1A:return{0x5A,0x9B,'P','D','K'};
        case 0x35:if(!unlocked)return{0x7F,0x35,0x33};
            if(p.size()==8){upRemain=(p[5]<<16)|(p[6]<<8)|p[7];uploading=true;return{0x75,0xFE};}
            return{0x7F,0x35,0x12};
        case 0x36:if(!uploading||p.size()!=1)return{0x7F,0x36,0x24};
            if(upRemain==0)return{0x7F,0x36,0x53};
            {uint8_t n=(uint8_t)(upRemain<254?upRemain:254);std::vector<uint8_t>r={0x76};
             for(uint8_t i=0;i<n;i++)r.push_back((uint8_t)(upBlock+i));upRemain-=n;return r;}
        case 0x37:uploading=false;return{0x77};
        }
        return{0x7F,p[0],0x11};
    }
    bool xfer(const std::vector<uint8_t>&p,uint8_t want,std::vector<uint8_t>&out,unsigned long){
        out=handle(p);return !out.empty()&&out[0]==want;}
    bool send(const std::vector<uint8_t>&p){handle(p);return true;}
};
template<class B> static bool tSession(B&b,uint8_t s){std::vector<uint8_t>r;return b.xfer({0x10,s},0x50,r,0);}
template<class B> static bool tSec(B&b,uint8_t lvl){std::vector<uint8_t>r;
    if(!b.xfer({0x27,lvl},0x67,r,0)||r.size()<4)return false;
    uint16_t seed=(r[2]<<8)|r[3],key=pdkKey16(seed);
    std::vector<uint8_t>snd={0x27,(uint8_t)(lvl+1),(uint8_t)(key>>8),(uint8_t)(key&0xFF)};
    return b.xfer(snd,0x67,r,0);}
static const DtcInfo *findDtc(uint16_t code);
static void actSelfTest() {
    int fails=0; auto chk=[&](const char*n,bool ok){logLine(std::string("[TEST] ")+n+": "+(ok?"OK":"FAIL"));if(!ok)fails++;};
    // seed/key spot checks
    struct{uint16_t s,k;}kv[]={{0,0},{4,0},{0x00ff,0x00e0},{0x0100,0x0100},{0xfffe,1},{0xffff,0}};
    bool kok=true; for(auto&e:kv) if(pdkKey16(e.s)!=e.k) kok=false; chk("seed/key vectors",kok);
    MockBus a; chk("SecurityAccess unlock",(tSession(a,0x89)&&tSec(a,0x01)&&a.unlocked));
    MockBus b2; tSession(b2,0x89); tSec(b2,0x01); tSession(b2,0x85);
    std::vector<uint8_t> r,all; bool up=b2.xfer(buildUpload13(),0x75,r,0);
    for(int i=0;i<400&&up;i++){std::vector<uint8_t>td;if(!b2.xfer({0x36},0x76,td,0))break;if(td.size()<=1)break;all.insert(all.end(),td.begin()+1,td.end());}
    chk("block 0x13 upload stream (15872 B)",all.size()==BLK13_SIZE);
    MockBus c; c.handle({0x10,0x89}); chk("upload refused when locked",!c.xfer(buildUpload13(),0x75,r,0));
    auto d=a.handle({0x18,0,0xFF,0}); bool f1872=false; for(size_t i=2;i+1<d.size();i+=3)if(((d[i]<<8)|d[i+1])==0x1872)f1872=true;
    chk("DTC parse P1872",f1872);
    extern bool rodDecodeSelfTest(); chk("live rod sensor decode",rodDecodeSelfTest());
    { int sv=g_platform; bool ok=true;
      g_platform=1; const DtcInfo*a=findDtc(0x0164); ok=ok&&a&&!strcmp(a->pcode,"P0164"); a=findDtc(0x1870); ok=ok&&a&&a->hint[0];
      g_platform=2; a=findDtc(0x1872); ok=ok&&a&&a->hint[0];
      g_platform=0; a=findDtc(0x1731); ok=ok&&a&&a->hint[0];
      g_platform=sv; chk("DTC catalogues 970 / 911-981-718 / 918",ok); }
    extern bool writeSelfTest(); chk("Mitten Bin data intact + RequestUpload format",writeSelfTest());
    logLine(fails?"[SELF-TEST] *** FAILED ***":"[SELF-TEST] ALL PASSED - exe runs and logic is correct on this PC.");
}

// ---------------------------------------------------------------------------
// High-level one-click tasks (chain the low-level steps so the user needs no
// knowledge of KWP). Each runs end-to-end and reports in plain language.
// ---------------------------------------------------------------------------
static std::string exeDir(){ char p[MAX_PATH]; GetModuleFileNameA(0,p,MAX_PATH);
    std::string s=p; auto q=s.find_last_of("\\/"); return q==std::string::npos?std::string("."):s.substr(0,q); }
static std::string stamp(){ SYSTEMTIME t; GetLocalTime(&t); char b[32];
    sprintf(b,"%04d%02d%02d_%02d%02d%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); return b; }

// ---------------------------------------------------------------------------
// DIAGNOSE: every stored DTC with PIWIS text (German), translation, status,
// repair hint, and its freeze frame (environment data). Everything is also
// written to PDK_diag_<time>.txt next to the exe.
// ---------------------------------------------------------------------------
static FILE *g_report = nullptr;
static void rep(const std::string &s){ logLine(s); if(g_report) fprintf(g_report,"%s\r\n",s.c_str()); }
static const DtcInfo *findDtc(uint16_t code){
    if (g_platform!=0) {                       // level-1 platforms: catalogue from the PIWIS database
        const PlatDtc *t = g_platform==1 ? DTC_9X1 : DTC_918;
        int n = g_platform==1 ? PLAT_N(DTC_9X1) : PLAT_N(DTC_918);
        static DtcInfo tmp;
        for(int i=0;i<n;i++) if(t[i].code==code){
            const char *h = (code>=0x1870&&code<=0x1872) ? "Run TEACH-IN: shift rods, then hydraulics, then clutches." : "";
            tmp = DtcInfo{t[i].code,t[i].pcode,t[i].en,t[i].de,h,1}; return &tmp; }
        return nullptr;
    }
    for(int i=0;i<DTC_COUNT;i++) if(DTC_TABLE[i].code==code) return &DTC_TABLE[i];
    return nullptr;
}
static std::string pcodeOf(uint16_t c){
    static const char L[4]={'P','C','B','U'}; char b[8]; sprintf(b,"%c%04X",L[c>>14],c&0x3FFF); return b;
}
static std::string dtcStatusText(uint8_t st){
    // ISO 14230-3 statusOfDTC: bit7 warning lamp, bits6..5 storage state
    std::string s;
    switch((st>>5)&3){ case 3: s="PRESENT now"; break; case 2: s="intermittent / maturing"; break;
                       case 1: s="stored (not present now)"; break; default: s="no fault stored"; }
    if(st&0x80) s+=", warning lamp";
    char b[16]; sprintf(b," (0x%02X)",st); return s+b;
}
// Freeze-frame signal catalogue (TCU-internal measurement IDs used in the
// environment-data lists at 0xA0059210). Names/units from firmware + ODX;
// entries marked ~ are inferred and shown with their raw value as well.
struct EnvSig { uint16_t id; uint8_t size; const char *name; double k; double off; const char *unit; };
// Names, exact sizes and scaling of the freeze-frame measurement IDs (firmware + PIWIS
// freeze-frame structures, re_findings/env_signals.md).
#include "env_signals.h"
static int envSize(uint16_t id){
    for(const EnvSig&e:ENV_SIGS) if(e.id==id) return e.size;
    static const uint16_t one[]={1,0xb,0x16,0x18,0x19,0x28,0x2a,0x2c,0x2e,0x33,0x40,0x41};
    if(id>1000){ static const uint16_t two[]={3002,3003,3012,3051};
        for(uint16_t t:two) if(t==id) return 2;
        if(id==3049||id==3073) return 3; if(id==3063||id==3074) return 4; return 1; }
    for(uint16_t o:one) if(o==id) return 1;
    return 2;
}
// The 24 environment-data lists, exactly as in the firmware (0xA0059210, 14 IDs each).
static const uint16_t ENV_LISTS[24][14] = {
 {3,1,2,24,13,14,23,3,3,3,3,3,3,3},
 {3,1,64,65,11,5,17,3,3,3,3,3,3,3},
 {3,2,5,6,7,8,9,10,3,3,3,3,3,3},
 {3,28,29,30,31,32,33,3,3,3,3,3,3,3},
 {3,9,34,35,36,37,40,42,3,3,3,3,3,3},
 {3,15,13,14,17,6,12,3,3,3,3,3,3,3},
 {3,17,2,1001,1002,1003,1004,3,3,3,3,3,3,3},
 {3,24,2,46,22,10,17,3,3,3,3,3,3,3},
 {3,5,12,23,2,17,1,3,3,3,3,3,3,3},
 {3,1,5,15,17,38,39,46,3,3,3,3,3,3},
 {3,7,26,27,15,17,1,3,3,3,3,3,3,3},
 {3,3001,3002,3003,3007,3009,3052,3054,3,3,3,3,3,3},
 {3,3063,3064,3068,3069,3070,3075,3,3,3,3,3,3,3},
 {3,3034,3035,3073,3074,3049,3,3,3,3,3,3,3,3},
 {3,3001,3003,3004,3005,3006,3007,3009,3019,3020,3051,3,3,3},
 {3,3001,3002,3007,3014,3020,3022,3024,3026,3050,3052,3053,3,3},
 {3,3001,3002,3007,3013,3019,3021,3023,3025,3050,3052,3053,3,3},
 {3,3001,3004,3005,3007,3014,3020,3022,3026,3033,3043,3045,3056,3},
 {3,3001,3004,3006,3007,3013,3019,3021,3025,3032,3042,3045,3056,3},
 {3,3001,3007,3014,3020,3022,3024,3026,3027,3029,3031,3033,3045,3},
 {3,3001,3007,3013,3019,3021,3023,3025,3027,3028,3030,3032,3045,3},
 {3,3010,3011,3012,3052,3057,3058,3059,3060,3061,3062,3,3,3},
 {3,3008,3017,3018,3054,3055,3057,3058,3059,3060,3065,3009,3,3},
 {3,3008,3015,3016,3017,3018,3019,3020,3054,3057,3058,3059,3060,3},
};
// Freeze frame (firmware, VERIFIED - re_findings/dtc_and_live.md + kwp_services.md):
// The TCU packs the 14 measurement IDs of the event's environment list into 14 bytes
// (FUN_a00c4b94). ReadStatusOfDTC "17 hi lo" returns 57 n + n x 24-byte records:
//   [0-1] DTC, [2] status, [3] counter, [4-5] DEM event id, [6-9] entry flags,
//   [10-23] last-occurrence snapshot, multi-byte values BIG-endian.
// Event id -> list: event config byte +6 (EVT_LIST in dtc_ff.h).
static int listFor(uint16_t code, int alt){
    for(int i=0;i<DTC_FF_COUNT;i++) if(DTC_FF[i].code==(code&0xFFFF)) return alt? DTC_FF[i].list2 : DTC_FF[i].list1;
    return -1;
}
static void decodeSnapshot(int k, const uint8_t *p, const char *tag){
    char h[96]; sprintf(h,"      freeze frame %s (list %d):",tag,k); rep(h);
    int pos=0;
    for(int j=0;j<14 && pos<14;j++){
        uint16_t id=ENV_LISTS[k][j];
        if(id==0){ pos++; continue; }
        int sz=envSize(id);
        if(pos+sz>14){ char t[96]; sprintf(t,"        (measurement %u cut off at the 14-byte limit)",id); rep(t); break; }
        uint32_t rawu=0; for(int q=0;q<sz;q++) rawu=(rawu<<8)|p[pos+q];   // big-endian in the 17 answer
        int raw=(int)rawu;
        const EnvSig *sg=nullptr; for(const EnvSig&e:ENV_SIGS) if(e.id==id) sg=&e;
        char l[160];
        if(sg && sz>2) {
            sprintf(l,"        %-34s 0x%0*X",sg->name,sz*2,rawu);
            if(id==3063){ rep(l); for(int bt=0;bt<32;bt++) if(((rawu>>bt)&1) && BITS3063[bt]){
                std::string t=std::string("          bit ")+std::to_string(bt)+": "+BITS3063[bt]; rep(t);} pos+=sz; continue; }
        }
        else if(sg) {
            double v = (sz==2 && id!=3) ? (double)(int16_t)raw : (double)raw;
            if(id==3 && raw==0xFFFF) sprintf(l,"        %-34s invalid",sg->name);
            else sprintf(l,"        %-34s %9.2f %s   (raw %d)",sg->name,v*sg->k+sg->off,sg->unit,raw);
        } else sprintf(l,"        measurement %-5u                 raw %d (0x%0*X)",id,raw,sz*2,raw);
        rep(l); pos+=sz;
    }
}
static void decodeRecord17(const uint8_t *rec){
    uint16_t code=(rec[0]<<8)|rec[1]; uint16_t ev=(rec[4]<<8)|rec[5];
    int k = (ev>=1 && ev<=sizeof(EVT_LIST)) ? EVT_LIST[ev-1] : 255;
    if(k>23) k=listFor(code,0);
    char m[120]; sprintf(m,"      record: status 0x%02X, counter %u, event %u",rec[2],rec[3],ev); rep(m);
    if(k<0||k>23){ rep("      freeze frame: list unknown - raw bytes kept above."); return; }
    decodeSnapshot(k,rec+10,"(last occurrence)");
}
static void actDiagnose(Bus &b){
    std::string path=exeDir()+"\\PDK_diag_"+stamp()+".txt";
    g_report=fopen(path.c_str(),"w");
    rep("=== DIAGNOSE ===");
    rep(std::string("Vehicle: ")+PLAT_NAME[g_platform]);
    startSession(b,0x81); Sleep(150);
    std::vector<uint8_t> r;
    if(b.xfer({0x1A,0x9B},0x5A,r,2000)) rep("[IDENT] "+hex(r.data()+2,(int)r.size()-2));
    if(!b.xfer({0x18,0x00,0xFF,0x00},0x58,r,3000)){
        rep("No response from TCU. Check Tactrix, OBD, and that this is a PDK.");
        if(g_report){fclose(g_report);g_report=nullptr;} return; }
    rep("[DTC raw] "+hex(r.data(),(int)r.size()));
    struct D { uint16_t c; uint8_t st; };
    std::vector<D> list;
    for(size_t i=2;i+2<r.size();i+=3) list.push_back({(uint16_t)((r[i]<<8)|r[i+1]),r[i+2]});
    if(list.empty()) rep("No faults stored. TCU is clean.");
    else { char m[48]; sprintf(m,"%d fault(s) stored:",(int)list.size()); rep(m); }
    for(const D&d:list){
        const DtcInfo *di=findDtc(d.c);
        rep("");
        rep("* "+pcodeOf(d.c)+"  "+(di?di->en:"(unknown code)")+"   ["+dtcStatusText(d.st)+"]");
        if(di && di->de[0]) rep(std::string("      PIWIS: ")+di->de);
        if(di && di->hint[0]) rep(std::string("      Check: ")+di->hint);
        // Freeze frame / environment data for this DTC (ReadStatusOfDTC by code).
        std::vector<uint8_t> f;
        if(b.xfer({0x17,(uint8_t)(d.c>>8),(uint8_t)d.c},0x57,f,3000)){
            rep("      [17 raw] "+hex(f.data(),(int)f.size()));
            int n = f.size()>1 ? f[1] : 0;
            if (g_platform==0) for(int i=0;i<n && 2+24*(i+1)<=(int)f.size();i++) decodeRecord17(f.data()+2+24*i);
            else rep("      (freeze-frame decoding is available for the Panamera 970 only - raw bytes above)");
            if(n==0 || f.size()<26) rep("      (no freeze-frame record in the answer)");
        } else rep("      (no freeze frame returned)");
        testerPresent(b);
    }
    bool cal=false; for(const D&d:list) if(d.c>=0x186F&&d.c<=0x1872) cal=true;
    rep("");
    rep(cal? "Calibration fault present: see the Check lines above (WRITE 0x13 / TEACH-IN)."
           : "No calibration faults (P186F-P1872).");
    rep("Report saved: "+path);
    if(g_report){fclose(g_report);g_report=nullptr;}
}
static void actClearDtc(Bus &b){
    logLine("=== CLEAR DTCs ===");
    startSession(b,0x89); Sleep(150);
    std::vector<uint8_t> r;
    if(b.xfer({0x14,0xFF,0x00},0x54,r,5000)) logLine("[OK] Fault memory cleared. Run DIAGNOSE again to see what comes back.");
    else logLine("[CLEAR] refused - see the NRC above.");
}
static void actReadEeprom(Bus&);               // fwd
static void actReadDonor(Bus &b){
    logLine("=== READ FROM DONOR (backup) ===");
    char cur[8]; GetWindowTextA(g_hBackup,cur,sizeof(cur));
    if(!cur[0]){ std::string pre=exeDir()+"\\PDK_donor_"+stamp(); SetWindowTextA(g_hBackup,pre.c_str());
                 logLine("Saving to: "+pre+"_*.bin"); }
    std::string path; getPath(g_hBackup,path);
    // 1) learned data from the RAM mirror (normal application, nothing changes)
    std::vector<uint8_t> b4,b8;
    if (readRamMirror(b,b4,b8)) { std::vector<uint8_t> all=b4; all.insert(all.end(),b8.begin(),b8.end());
        saveFile(path+"_eeprom_ram.bin",all); analyzeRamMirror(b4,b8); }
    if (g_stop) return;
    // 2) flasher: block 0x13 + full DFlash (EEPROM) image
    if (!enterFlasher(b)) { logLine("Flasher not entered - block 0x13 / full EEPROM image not read."); return; }
    std::vector<uint8_t> blk;
    if (uploadBlock13(b,blk)) saveFile(path+"_block13.bin",blk);
    if (g_stop) return;
    std::vector<uint8_t> ee;
    if (readDflash(b,ee)) { saveFile(path+"_EEprom.bin",ee); analyzeEeprom(ee,path+"_EEprom.bin"); }
    else logLine("Full EEPROM image not readable in the flasher - the RAM mirror file above holds the learned data.");
    logLine("Done. Switch the ignition OFF for 10 s (the TCU is still in the flasher), then ON.");
}
static void actReadBlockUpload(Bus &b, uint8_t block) {
    if (block!=0x13){ logLine("[UPLOAD] There is no flash block 0x14: the adaptation data lives in EEPROM block 4 - use EEPROM / READ FROM DONOR."); return; }
    std::string path; getPath(g_hBackup,path); if(path.empty()) path=exeDir()+"\\PDK_"+stamp();
    if (!enterFlasher(b)) return;
    std::vector<uint8_t> blk; if (uploadBlock13(b,blk)) saveFile(path+"_block13.bin",blk);
    logLine("Ignition OFF for 10 s now (the TCU is still in the flasher).");
}
// Order verified in the firmware: the clutch teach-in needs a valid hydraulic
// adaptation (clutch condition bit 12), so: shift rods -> hydraulics -> clutches.
static bool confirmPdk3000(Bus &b) {
    if (g_platform!=0) {
        std::vector<uint8_t> r; startSession(b,0x81); Sleep(150);
        if (b.xfer({0x1A,0x9B},0x5A,r,2000)) logLine("[TEACHIN] TCU ident: "+hex(r.data()+2,(int)r.size()-2));
        std::string q=std::string("Vehicle: ")+PLAT_NAME[g_platform]+"\n\nThe teach-in routines (31 20 / 31 22 / 31 21) are the same as on the "
            "Panamera 970 according to the Porsche database, but this tool has not been used on this model yet.\n\nContinue?";
        return MessageBoxA(g_hMain,q.c_str(),"Teach-in",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES;
    }
    startSession(b,0x81); Sleep(150);
    std::vector<uint8_t> r; std::string asc;
    for (uint8_t id : {(uint8_t)0x9B,(uint8_t)0x91,(uint8_t)0x92,(uint8_t)0x94}) {
        if (b.xfer({0x1A,id},0x5A,r,2000))
            for (size_t i=2;i<r.size();i++) asc += (r[i]>=0x20&&r[i]<0x7F)?(char)r[i]:'.';
        asc += ' ';
    }
    logLine("[TEACHIN] TCU ident: "+asc);
    if (asc.find("970618")!=std::string::npos || asc.find("5WP54023")!=std::string::npos) {
        logLine("[TEACHIN] recognised: PDK 3000."); return true; }
    std::string q="The TCU could not be confirmed as a PDK 3000 (5WP54023).\nIdent: "+asc.substr(0,120)+
        "\n\nThe teach-in routine numbers in this tool were taken from the PDK 3000 firmware.\nContinue anyway?";
    return MessageBoxA(g_hMain,q.c_str(),"Teach-in",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)==IDYES;
}
static void actTeachAll(Bus &b){
    logLine("=== TEACH-IN (guided) ===");
    logLine("Conditions: engine idling (400..900 rpm), selector P, gearbox oil 60..100 C, foot off the pedals, charger on.");
    if (!confirmPdk3000(b)) { logLine("[TEACHIN] cancelled."); return; }
    logLine("Order: shift rods -> hydraulics -> clutches.");
    const uint8_t seq3[3]={TEACH_LID_RODS,TEACH_LID_HYDR,TEACH_LID_CLUTCH};
    for (int i=0;i<3;i++){
        if (g_stop) return;
        if (!actTeach(b,seq3[i])) {
            logLine("[TEACHIN] sequence halted. Fix the !! items above and run TEACH-IN again.");
            return;
        }
    }
    logLine("Teach-in sequence finished OK. Ignition off/on, then DIAGNOSE (and Analyze EEPROM after a read).");
}
// ---------------------------------------------------------------------------
// LIVE ROD SENSORS - record the 4 shift-rod distance sensors while shifting.
// Reads TCU RAM directly with ReadMemoryByAddress. Addresses come from the
// firmware RE (identical in SW G070 and G088):
//   0xD000271C  u16[4]  averaged sensor duty   (32768 = 100 %)
//   0xD0002724  u16[4]  last duty sample       (32768 = 100 %)
//   0xD000272C  s16[4]  rod position           (1/256 mm, = curve(duty) - 13.75 mm)
//   0xD0002734  u16[4]  bitwise complement of the position (integrity copy)
//   0xD0009758  s16[4]  filtered position (what PIWIS shows as Wegsensorwert) - SW G070
//   0xD000980C  s16[4]  the same in SW G088 (application RAM 0xD0004000-0xD000BFFF is laid out
//                       differently in G088; driver RAM below 0xD0004000 and the EEPROM mirrors
//                       from 0xD000C000 are identical). Chosen automatically, see pickFiltAddr().
//   0xD0003212  u8[4]   channel status (bit 0x08 = duty outside 8..92 % -> fault)
// The TCU flags a channel faulty when duty < 8.00 % or > 92.00 %.
// ---------------------------------------------------------------------------
struct RodSample {
    double duty[4], dutyRaw[4], pos[4], filt[4];
    uint8_t stat[4];
    bool complOk;
};
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
// Decode the 32-byte block at 0xD000271C plus 8 bytes at 0xD0009758 and
// 4 status bytes at 0xD0003212. Pure function (also used by the self-test).
static RodSample decodeRods(const uint8_t *blk32, const uint8_t *filt8, const uint8_t *stat4) {
    RodSample s; s.complOk = true;
    for (int i=0;i<4;i++){
        s.duty[i]    = le16(blk32+0x00+2*i) * 100.0 / 32768.0;
        s.dutyRaw[i] = le16(blk32+0x08+2*i) * 100.0 / 32768.0;
        int16_t p    = (int16_t)le16(blk32+0x10+2*i);
        uint16_t c   = le16(blk32+0x18+2*i);
        if ((uint16_t)(p ^ c) != 0xFFFF) s.complOk = false;
        s.pos[i]  = p / 256.0;
        s.filt[i] = (filt8 ? (int16_t)le16(filt8+2*i) : 0) / 256.0;
        s.stat[i] = stat4 ? stat4[i] : 0;
    }
    return s;
}
// Pick the filtered-position address for this software: the right one tracks the raw rod
// positions (0xD000272C) within a few mm; the wrong one holds unrelated data.
static uint32_t pickFiltAddr(Bus &b) {
    static const uint32_t CAND[2]={0xD0009758,0xD000980C}; static const char *NAME[2]={"G070","G088"};
    std::vector<uint8_t> blk, c;
    if (!ramRead(b,0xD000271C,0x20,blk)) return CAND[0];
    double best=1e9; int bi=0;
    for (int k=0;k<2;k++) {
        if (!ramRead(b,CAND[k],0x08,c)) continue;
        double e=0; for(int i=0;i<4;i++) e+=fabs(((int16_t)le16(c.data()+2*i)-(int16_t)le16(blk.data()+0x10+2*i))/256.0);
        if (e<best){best=e;bi=k;}
    }
    char m[120]; sprintf(m,"[LIVE] filtered rod positions at 0x%08X (software %s layout, mean deviation %.2f mm)",CAND[bi],NAME[bi],best/4);
    logLine(m); return CAND[bi];
}
// Self-test vector: ch1 50 % / +0.10 mm, ch2 8 % / -12 mm, ch3 92 % / +12 mm,
// ch4 broken complement -> must be detected.
bool rodDecodeSelfTest() {
    uint8_t blk[32]={0}, fl[8]={0}, st[4]={0,8,8,0};
    const uint16_t duty[4]={16384,2621,30147,16384};
    const int16_t  pos[4]={26,-3072,3072,0};
    for(int i=0;i<4;i++){
        blk[2*i]=duty[i]&0xFF; blk[2*i+1]=duty[i]>>8;
        blk[0x10+2*i]=(uint8_t)(pos[i]&0xFF); blk[0x11+2*i]=(uint8_t)((uint16_t)pos[i]>>8);
        uint16_t c=(uint16_t)~(uint16_t)pos[i]; if(i==3) c^=1;          // corrupt ch4
        blk[0x18+2*i]=c&0xFF; blk[0x19+2*i]=c>>8;
        fl[2*i]=blk[0x10+2*i]; fl[2*i+1]=blk[0x11+2*i];
    }
    RodSample s=decodeRods(blk,fl,st);
    bool ok = s.duty[0]>49.99 && s.duty[0]<50.01 && s.duty[1]>7.99 && s.duty[1]<8.01 &&
              s.duty[2]>91.99 && s.duty[2]<92.01 && s.pos[1]==-12.0 && s.pos[2]==12.0 &&
              s.filt[2]==12.0 && s.stat[1]==8 && !s.complOk;
    blk[0x18+6]^=1; s=decodeRods(blk,fl,st);                      // repair ch4
    return ok && s.complOk;
}
// Extra live values (firmware RE, SW G070/G088):
//   0xD000277A s16[3]  pressure sensors 1..3 (lookup table of ADC; 1 = ~1 mbar, 24 bar at full scale)
//   0xD0002792 s16[2]  sensor supply 1/2      (measurement IDs 5/6; mV, inferred)
//   0xD00027C0 s16[4]  actuator supply HSS1..4 (ADC ch 7,6,5,9 scaled; mV, inferred)
//   0xD0002760 u16[7]  EDS1..7 measured current (calibrated ADC; 0.1 mA, inferred)
//   0xD0001698 + 0x38*i u16  EDS i driver value (diag IDs 0x1B..0x27; 0.1 mA, inferred)
struct LiveExtra { bool valid=false; int rpm=-1; double kmh=-1; int tqRaw=0; int press[3]={0,0,0}; int supply[2]={0,0}; int hss[4]={0,0,0,0};
                   unsigned edsMeas[7]={0,0,0,0,0,0,0}; unsigned edsDrv[7]={0,0,0,0,0,0,0}; };
static LiveExtra decodeExtra(const uint8_t *a20 /*@D0002760*/, const uint8_t *b38 /*@D0002790*/,
                             const uint8_t *c /*@D0001698,0xFC*/, const uint8_t *d /*@D00017B0,0x3A*/) {
    LiveExtra e; e.valid=true;
    for(int i=0;i<7;i++) e.edsMeas[i]=le16(a20+2*i);
    for(int i=0;i<3;i++) e.press[i]=(int16_t)le16(a20+0x1A+2*i);
    for(int i=0;i<2;i++) e.supply[i]=(int16_t)le16(b38+0x02+2*i);
    for(int i=0;i<4;i++) e.hss[i]=(int16_t)le16(b38+0x30+2*i);
    for(int i=0;i<5;i++) e.edsDrv[i]=le16(c+0x38*i);
    for(int i=5;i<7;i++) e.edsDrv[i]=le16(d+0x38*(i-5));
    return e;
}
static void actLiveRods(Bus &b) {
    logLine("=== LIVE DATA ===");
    logLine("Shift slowly P -> R -> N -> D (and +/- in manual). Press MARK at each position.");
    logLine("Press STOP to finish. Everything is saved to a CSV next to the exe.");
    // 10 89 -> unlock -> 10 86, then 23 01 27 1C 20 (firmware: RAM read only in session 0x86)
    if (!enterRamSession(b)) { logLine("[LIVE] could not open the RAM-read session (see NRC above)."); return; }
    { std::vector<uint8_t> t; if(!ramRead(b,0xD000271C,0x20,t)) { logLine("[LIVE] TCU refused the RAM read - send me the log."); return; }
      if(!decodeRods(t.data(),nullptr,nullptr).complOk) logLine("[LIVE] warning: position/complement check failed on first read."); }
    std::string path = exeDir()+"\\PDK_rods_"+stamp()+".csv";
    FILE *csv = fopen(path.c_str(),"w");
    if (!csv) { logLine("[LIVE] cannot create "+path); return; }
    fprintf(csv,"t_ms,mark,duty1_pct,duty2_pct,duty3_pct,duty4_pct,"
                "pos1_mm,pos2_mm,pos3_mm,pos4_mm,filt1_mm,filt2_mm,filt3_mm,filt4_mm,"
                "stat1,stat2,stat3,stat4,sample1_pct,sample2_pct,sample3_pct,sample4_pct,valid,"
                "press1_raw,press2_raw,press3_raw,supply1_raw,supply2_raw,hss1_raw,hss2_raw,hss3_raw,hss4_raw,"
                "eds1_meas_raw,eds2_meas_raw,eds3_meas_raw,eds4_meas_raw,eds5_meas_raw,eds6_meas_raw,eds7_meas_raw,"
                "eds1_drv_raw,eds2_drv_raw,eds3_drv_raw,eds4_drv_raw,eds5_drv_raw,eds6_drv_raw,eds7_drv_raw,"
                "engine_rpm,vehicle_kmh,clutch_torque_raw\n");
    logLine("[LIVE] recording -> "+path);
    logLine("        duty %  (ch1..ch4)            |  position mm (ch1..ch4)");

    double mn[4],mx[4],dmn[4],dmx[4]; for(int i=0;i<4;i++){mn[i]=dmn[i]=1e9;mx[i]=dmx[i]=-1e9;}
    long lastMark=g_mark; int bad=0, n=0;
    const uint32_t filtAddr = pickFiltAddr(b);
    DWORD t0=GetTickCount(), lastTp=t0, lastShow=0, lastExtra=0;
    while (!g_stop) {
        std::vector<uint8_t> blk, fl, st;
        bool r1 = ramRead(b,0xD000271C,0x20,blk);
        bool r2 = r1 && ramRead(b,filtAddr,0x08,fl);
        bool r3 = r2 && ramRead(b,0xD0003212,0x04,st);
        // Extra live values (read-only), see LiveExtra / README for addresses.
        std::vector<uint8_t> ra, rb, rc, rd;
        bool rx = r3 && ramRead(b,0xD0002760,0x20,ra)      // EDS measured currents + pressures
                     && ramRead(b,0xD0002790,0x38,rb)      // sensor supplies + HSS supplies
                     && ramRead(b,0xD0001698,0xFC,rc)      // EDS driver structs 1..5
                     && ramRead(b,0xD0001698+0x118,0x3A,rd); // EDS driver structs 6..7
        std::vector<uint8_t> rcan; bool rcanOk = r3 && ramRead(b,0xD0003DC8,0x18,rcan); // CAN inputs: torque, rpm, Vref
        DWORD now=GetTickCount();
        if (!r3) { if(++bad>20){ logLine("[LIVE] TCU stopped answering - stopping."); break; } Sleep(100); continue; }
        LiveExtra ex = rx ? decodeExtra(ra.data(),rb.data(),rc.data(),rd.data()) : LiveExtra{};
        if (rcanOk) { ex.tqRaw=(int16_t)le16(rcan.data()+2); ex.rpm=le16(rcan.data()+6)/4; ex.kmh=le16(rcan.data()+0x16)*0.01; }
        bad=0; n++;
        RodSample s = decodeRods(blk.data(),fl.data(),st.data());
        long mk=g_mark;
        fprintf(csv,"%lu,%ld",(unsigned long)(now-t0),mk);
        for(int i=0;i<4;i++) fprintf(csv,",%.2f",s.duty[i]);
        for(int i=0;i<4;i++) fprintf(csv,",%.3f",s.pos[i]);
        for(int i=0;i<4;i++) fprintf(csv,",%.3f",s.filt[i]);
        for(int i=0;i<4;i++) fprintf(csv,",%u",s.stat[i]);
        for(int i=0;i<4;i++) fprintf(csv,",%.2f",s.dutyRaw[i]);
        fprintf(csv,",%d",s.complOk?1:0);
        for(int i=0;i<3;i++) fprintf(csv,",%d",ex.press[i]);
        for(int i=0;i<2;i++) fprintf(csv,",%d",ex.supply[i]);
        for(int i=0;i<4;i++) fprintf(csv,",%d",ex.hss[i]);
        for(int i=0;i<7;i++) fprintf(csv,",%u",ex.edsMeas[i]);
        for(int i=0;i<7;i++) fprintf(csv,",%u",ex.edsDrv[i]);
        fprintf(csv,",%d,%.2f,%d\n",ex.rpm,ex.kmh,ex.tqRaw);
        if (s.complOk) for(int i=0;i<4;i++){
            if(s.pos[i]<mn[i])mn[i]=s.pos[i]; if(s.pos[i]>mx[i])mx[i]=s.pos[i];
            if(s.duty[i]<dmn[i])dmn[i]=s.duty[i]; if(s.duty[i]>dmx[i])dmx[i]=s.duty[i]; }
        if (mk!=lastMark || now-lastShow>=500) {
            char line[200];
            sprintf(line,"%s%5.1f %5.1f %5.1f %5.1f  | %+6.2f %+6.2f %+6.2f %+6.2f%s%s",
                mk!=lastMark?"[MARK] ":"       ",
                s.duty[0],s.duty[1],s.duty[2],s.duty[3],s.pos[0],s.pos[1],s.pos[2],s.pos[3],
                ((s.stat[0]|s.stat[1]|s.stat[2]|s.stat[3])&0x08)?"  !! SENSOR FAULT (duty out of 8..92%)":"",
                s.complOk?"":"  (data check failed)");
            logLine(line); lastShow=now; lastMark=mk;
            if (ex.valid && (now-lastExtra>=1000)) {
                char l2[220];
                sprintf(l2,"       press %5.2f %5.2f %5.2f bar | supply %5.2f %5.2f V | EDS mA %4.0f %4.0f %4.0f %4.0f %4.0f %4.0f %4.0f",
                    ex.press[0]/1000.0,ex.press[1]/1000.0,ex.press[2]/1000.0,ex.supply[0]/1000.0,ex.supply[1]/1000.0,
                    ex.edsMeas[0]/10.0,ex.edsMeas[1]/10.0,ex.edsMeas[2]/10.0,ex.edsMeas[3]/10.0,ex.edsMeas[4]/10.0,ex.edsMeas[5]/10.0,ex.edsMeas[6]/10.0);
                logLine(l2);
                if (ex.rpm>=0){ char l3[120]; sprintf(l3,"       CAN: engine %d rpm | vehicle %.1f km/h | clutch torque raw %d",ex.rpm,ex.kmh,ex.tqRaw); logLine(l3); }
                if (ex.supply[0]>0 && (ex.supply[0]<4500 || ex.supply[0]>5500))
                    logLine("       !! sensor supply 1 outside 4.5..5.5 V (P1706 range) - check for a shorted sensor");
                lastExtra=now;
            }
        }
        if (now-lastTp>=1000){ testerPresent(b); lastTp=now; }
        Sleep(50);
    }
    fclose(csv);
    char m[96]; sprintf(m,"[LIVE] stopped. %d samples saved.",n); logLine(m);
    if (n) {
        logLine("[LIVE] range seen per channel:");
        for(int i=0;i<4;i++){ sprintf(m,"   ch%d  position %+6.2f .. %+6.2f mm   duty %5.1f .. %5.1f %%",
                                      i+1,mn[i],mx[i],dmn[i],dmx[i]); logLine(m); }
    }
    logLine("[LIVE] file: "+path);
}

// ---------------------------------------------------------------------------
// Valve test = KWP routine 0xD0 ("Stellglieddiagnose", actuator diagnosis).
// Firmware (VERIFIED): 31 D0 <byte0> <byte1> -> handler FUN_a00d65e8 stores the
// two bytes at 0xD0007521/0xD0007522. While the test runs, the measurement getters
// for the valve setpoints return the test values: byte0 bit k = EDS(k+1) (k=0..6,
// getters FUN_a00b5820..a00b5c74), byte1 bit k = MV(k+1) (k=0..2, FUN_a00b5d8a..ed6).
// ---------------------------------------------------------------------------
static const char *VALVES[] = {"EDS1","EDS2","EDS3","EDS4","EDS5","EDS6","EDS7","MV1","MV2","MV3"};
static void actValveTest(Bus &b) {
    int v=g_valveSel; if(v<0||v>9) v=0;
    uint8_t b0=0,b1=0; if(v<7) b0=(uint8_t)(1<<v); else b1=(uint8_t)(1<<(v-7));
    logLine(std::string("=== VALVE TEST: ")+VALVES[v]+" ===");
    std::vector<uint8_t> r; bool started=false;
    for (uint8_t ss : {(uint8_t)0x89,(uint8_t)0x86}) {
        if (ss==0x86) { if(!enterRamSession(b)) break; } else { startSession(b,ss); Sleep(150); }
        b.lastNrc=0;
        char m[64]; sprintf(m,"[VALVE] 31 D0 %02X %02X (session %02X)",b0,b1,ss); logLine(m);
        if (b.xfer({0x31,0xD0,b0,b1},0x71,r,5000)) { started=true; break; }
        if (b.lastNrc==0x22) { logLine("[VALVE] refused: conditions not correct (engine off? selector P? ignition on?)"); return; }
    }
    if (!started) { logLine("[VALVE] TCU did not accept the actuator test - send me the log."); return; }
    int st=1;
    for (int t=0;t<60 && !g_stop;t++){
        Sleep(500); testerPresent(b);
        if (b.xfer({0x33,0xD0},0x73,r,3000) && r.size()>=3){ st=r[2];
            char m[80]; sprintf(m,"[VALVE] %4.1fs status %d (%s)  raw %s",t*0.5,st,teachStatusText((unsigned char)st),hex(r.data(),(int)r.size()).c_str()); logLine(m);
            if (st!=1) break; }
    }
    b.xfer({0x32,0xD0},0x72,r,3000);
    logLine(st==2?"[VALVE] finished. Listen/feel for the valve click; check DIAGNOSE for new EDS/MV faults."
                 :"[VALVE] ended (see status above).");
}

struct Job { int id; };
static DWORD WINAPI worker(LPVOID p) {
    Job *j=(Job*)p; g_busy=true; g_stop=false;
    Bus b; readIds(b);
    if (b.open(b.txId,b.rxId)) {
        switch(j->id){
        case ID_IDENTIFY: actIdentify(b); break;
        case ID_GETSEED:  actGetSeed(b); break;
        case ID_UNLOCK:   startSession(b,0x89); Sleep(150); securityAccess(b,0x01); break;
        case ID_READEEP:  actReadEeprom(b); break;
        case ID_READ13:   actReadBlockUpload(b,0x13); break;
        case ID_READ14:   actReadBlockUpload(b,0x14); break;
        case ID_TEACH_S:  actTeach(b,"s"); break;
        case ID_TEACH_C:  actTeach(b,"c"); break;
        case ID_TEACH_H:  actTeach(b,"h"); break;
        case ID_RESTOREWR:actRestore(b); break;
        case ID_DIAGNOSE: actDiagnose(b); break;
        case ID_READDONOR:actReadDonor(b); break;
        case ID_TEACHALL: actTeachAll(b); break;
        case ID_LIVEROD:  actLiveRods(b); break;
        case ID_CLEARDTC: actClearDtc(b); break;
        case ID_VALVETEST:actValveTest(b); break;
        case ID_FULLBACKUP:actFullBackup(b); break;
        }
    }
    b.close();
    free(j); g_busy=false;
    PostMessageA(g_hMain,WM_APP_DONE,0,0);
    return 0;
}
static void startJob(int id){
    if(g_busy){logLine("[BUSY] wait for current action.");return;}
    Job*j=(Job*)malloc(sizeof(Job)); j->id=id;
    CreateThread(0,0,worker,j,0,0);
}

// ---------------------------------------------------------------------------
// Device detection + status
// ---------------------------------------------------------------------------
//  returns: 1 = Tactrix present & opened, 0 = driver present but no device,
//          -1 = op20pt32.dll (J2534 driver) not found
static int detectDevice() {
    HMODULE dll = loadJ2534Dll();
    if (!dll) return -1;
    PF_Open  o = (PF_Open) GetProcAddress(dll,"PassThruOpen");
    PF_Close c = (PF_Close)GetProcAddress(dll,"PassThruClose");
    if (!o || !c) return -1;
    unsigned long d = 0;          // runs on a background thread, so a slow/blocking
    if (o(nullptr,&d) == J_STATUS_NOERROR) { c(d); return 1; } // open never freezes the UI
    return 0;
}
static void updateStatus(int st) {
    g_devState = st;
    const char *txt =
        st==1 ? "Tactrix: CONNECTED" :
        st==0 ? "Tactrix: driver OK, NO DEVICE (check USB)" :
                "Tactrix: J2534 driver NOT FOUND (install Tactrix drivers)";
    if (g_hStatus) { SetWindowTextA(g_hStatus, txt); InvalidateRect(g_hStatus,nullptr,TRUE); }
}
// Detection runs on its OWN thread so a slow/blocking PassThruOpen can never
// freeze or stop the window from opening. Result is posted back to the UI.
static DWORD WINAPI detectThread(LPVOID){
    int st = detectDevice();
    g_detecting = false;
    if (g_hMain) PostMessageA(g_hMain, WM_APP_DEVSTATE, (WPARAM)(st+10), 0);
    return 0;
}
static void kickDetect(){
    if (g_detecting || g_busy) return;
    g_detecting = true;
    HANDLE t = CreateThread(0,0,detectThread,0,0,0);
    if (t) CloseHandle(t); else g_detecting = false;
}

// ---------------------------------------------------------------------------
// GUI
// ---------------------------------------------------------------------------
static HWND mkButton(HWND p,const char*t,int id,int x,int y,int w,int h){
    return CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,w,h,p,(HMENU)(INT_PTR)id,0,0);
}
static void browse(HWND h,bool save){
    char file[512]=""; OPENFILENAMEA ofn={0}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=g_hMain;
    ofn.lpstrFilter="BIN\0*.bin;*.BIN\0All\0*.*\0"; ofn.lpstrFile=file; ofn.nMaxFile=sizeof(file);
    ofn.Flags=OFN_PATHMUSTEXIST; bool ok= save?GetSaveFileNameA(&ofn):GetOpenFileNameA(&ofn);
    if(ok) SetWindowTextA(h,file);
}
static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE: {
        int x=10,y=10;
        g_hStatus=CreateWindowA("STATIC","Tactrix: checking...",WS_CHILD|WS_VISIBLE,x,y,380,20,h,0,0,0);
        mkButton(h,"Detect",ID_DETECT,x+400,y-2,90,24);
        y+=26;
        CreateWindowA("STATIC","Vehicle:",WS_CHILD|WS_VISIBLE,x,y+4,52,16,h,0,0,0);
        g_hPlat=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,x+56,y,434,200,h,(HMENU)IDC_PLATFORM,0,0);
        for(const char*pn:PLAT_NAME) SendMessageA(g_hPlat,CB_ADDSTRING,0,(LPARAM)pn);
        SendMessageA(g_hPlat,CB_SETCURSEL,0,0);
        y+=30;
        // ---- BIG one-click tasks (what the user actually wants to do) ----
        HFONT big=CreateFontA(19,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,"Segoe UI");
        auto bigBtn=[&](const char*t,int id,int bx,int by){
            HWND w=CreateWindowA("BUTTON",t,WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_MULTILINE,bx,by,238,54,h,(HMENU)(INT_PTR)id,0,0);
            SendMessageA(w,WM_SETFONT,(WPARAM)big,1); return w; };
        bigBtn("1.  DIAGNOSE",             ID_DIAGNOSE, x,     y);
        bigBtn("2.  READ FROM DONOR",    ID_READDONOR,x+252, y);
        y+=60;
        bigBtn("3.  CHECK BLOCK 0x13",     ID_RESTOREWR,x,     y);
        bigBtn("4.  TEACH-IN (guided)",    ID_TEACHALL, x+252, y);
        y+=60;
        { HWND w5=CreateWindowA("BUTTON","5.  LIVE DATA (rods, pressures, valves) - record",
              WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,x,y,490,46,h,(HMENU)(INT_PTR)ID_LIVEROD,0,0);
          SendMessageA(w5,WM_SETFONT,(WPARAM)big,1); }
        y+=54;
        mkButton(h,"STOP",ID_STOP,x,y,150,28);
        mkButton(h,"MARK",ID_MARK,x+160,y,110,28);
        mkButton(h,"Self-Test (no car)",ID_SELFTEST,x+280,y,180,28);
        y+=36;
        // ---- Advanced / manual (small; most users ignore this) ----
        CreateWindowA("STATIC","--- Advanced (manual) ---",WS_CHILD|WS_VISIBLE,x,y,490,16,h,0,0,0); y+=20;
        CreateWindowA("STATIC","TX:",WS_CHILD|WS_VISIBLE,x,y+3,22,16,h,0,0,0);
        g_hTx=CreateWindowA("EDIT","70F",WS_CHILD|WS_VISIBLE|WS_BORDER,x+24,y,48,20,h,(HMENU)IDC_TX,0,0);
        CreateWindowA("STATIC","RX:",WS_CHILD|WS_VISIBLE,x+82,y+3,22,16,h,0,0,0);
        g_hRx=CreateWindowA("EDIT","779",WS_CHILD|WS_VISIBLE|WS_BORDER,x+106,y,48,20,h,(HMENU)IDC_RX,0,0);
        CreateWindowA("STATIC","Save to:",WS_CHILD|WS_VISIBLE,x+168,y+3,46,16,h,0,0,0);
        g_hBackup=CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,x+214,y,218,20,h,(HMENU)IDC_BACKUP,0,0);
        mkButton(h,"...",ID_BROWSE_B,x+436,y,24,20);
        y+=24;
        CreateWindowA("STATIC","Write file:",WS_CHILD|WS_VISIBLE,x,y+3,72,16,h,0,0,0);
        g_hRestore=CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER,x+74,y,358,20,h,(HMENU)IDC_RESTORE,0,0);
        mkButton(h,"...",ID_BROWSE_R,x+436,y,24,20);
        y+=26;
        mkButton(h,"Identify",ID_IDENTIFY,x,y,76,22);
        mkButton(h,"Seed/Key",ID_GETSEED,x+80,y,76,22);
        mkButton(h,"Unlock",ID_UNLOCK,x+160,y,66,22);
        mkButton(h,"EEPROM",ID_READEEP,x+230,y,76,22);
        mkButton(h,"Blk13",ID_READ13,x+310,y,54,22);
        
        y+=24;
        mkButton(h,"Teach rods",ID_TEACH_S,x,y,108,22);
        mkButton(h,"Teach clutch",ID_TEACH_C,x+112,y,108,22);
        mkButton(h,"Teach hydr.",ID_TEACH_H,x+224,y,98,22);
        mkButton(h,"Clear DTCs",ID_CLEARDTC,x+330,y,92,22);
        y+=24;
        mkButton(h,"Analyze EEPROM file",ID_ANALYZEEE,x,y,150,22);
        g_hValve=CreateWindowA("COMBOBOX","",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,x+170,y,80,200,h,(HMENU)IDC_VALVESEL,0,0);
        for(const char*vn:VALVES) SendMessageA(g_hValve,CB_ADDSTRING,0,(LPARAM)vn);
        SendMessageA(g_hValve,CB_SETCURSEL,0,0);
        mkButton(h,"Valve test",ID_VALVETEST,x+256,y,120,22);
        mkButton(h,"Full backup",ID_FULLBACKUP,x+382,y,100,22);
        y+=30;
        g_hLog=CreateWindowA("EDIT","",WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            x,y,500,320,h,(HMENU)IDC_LOG,0,0);
        HFONT f=CreateFontA(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,FIXED_PITCH,"Consolas");
        SendMessageA(g_hLog,WM_SETFONT,(WPARAM)f,1);
        logLine("PdkTool - Porsche 970 PDK 3000.  Always make a BACKUP before any write/teach-in.");
        logLine("1=Diagnose  2=Read from donor  3=Write to broken  4=Teach-in  5=Live data.");
        logLine("CAN 0x70F/0x779, seed/key = ASAM_seedkey_PDK (verified).");
        SetWindowTextA(g_hStatus,"Tactrix: checking...");
        SetTimer(h,TIMER_DEVCHECK,3000,nullptr);
        kickDetect();            // async, never blocks window creation
        return 0; }
    case WM_TIMER:
        if(w==TIMER_DEVCHECK) kickDetect();
        return 0;
    case WM_APP_DEVSTATE: {
        int st=(int)w-10; updateStatus(st);
        if(g_detVerbose){ g_detVerbose=false;
            if(st<0) logLine("[DEV] No J2534 driver found (registry PassThruSupport.04.04 or op20pt32.dll).");
            else logLine(std::string("[DEV] driver: ")+(g_dllPath.empty()?"(by name)":g_dllPath)+
                         (st==1?"  -> Tactrix CONNECTED":"  -> no device (check USB)")); }
        return 0; }
    case WM_CTLCOLORSTATIC: {
        if((HWND)l==g_hStatus){ HDC dc=(HDC)w; SetBkMode(dc,TRANSPARENT);
            SetTextColor(dc, g_devState==1?RGB(0,130,0):g_devState==0?RGB(190,120,0):RGB(200,0,0));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break; }
    case WM_COMMAND: {
        int id=LOWORD(w);
        if(id==ID_STOP){g_stop=true;logLine("[STOP] requested.");return 0;}
        if(id==ID_ANALYZEEE && g_platform!=0){ MessageBoxA(h,"EEPROM analysis is available for the Panamera 970 (PDK 3000) only.",
                "Panamera 970 only",MB_OK|MB_ICONINFORMATION); return 0; }
        if(id==ID_ANALYZEEE){
            char file[512]=""; OPENFILENAMEA ofn={0}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=h;
            ofn.lpstrFilter="EEPROM dump\0*.bin;*EEprom*\0All\0*.*\0"; ofn.lpstrFile=file; ofn.nMaxFile=sizeof(file);
            ofn.Flags=OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST;
            if(!GetOpenFileNameA(&ofn)) return 0;
            FILE*f=fopen(file,"rb"); if(!f){logLine("[EE] cannot open file");return 0;}
            fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
            std::vector<uint8_t> d(sz>0?sz:0); if(sz>0) fread(d.data(),1,sz,f); fclose(f);
            analyzeEeprom(d,file); return 0; }
        if(id==ID_MARK){ long n=InterlockedIncrement(&g_mark); char m[40]; sprintf(m,"[MARK %ld]",n); logLine(m); return 0; }
        if(id==ID_BROWSE_B){browse(g_hBackup,true);return 0;}
        if(id==ID_BROWSE_R){browse(g_hRestore,false);return 0;}
        if(id==ID_SELFTEST){actSelfTest();return 0;}
        if(id==ID_DETECT){ g_detVerbose=true; logLine("[DEV] checking..."); kickDetect(); return 0; }
        bool devAction = (id>=ID_IDENTIFY && id<=ID_RESTOREWR) ||
                         id==ID_DIAGNOSE || id==ID_READDONOR || id==ID_TEACHALL || id==ID_LIVEROD || id==ID_CLEARDTC || id==ID_VALVETEST || id==ID_FULLBACKUP;
        if(id==IDC_PLATFORM){ if(HIWORD(w)==CBN_SELCHANGE){ int sel=(int)SendMessageA(g_hPlat,CB_GETCURSEL,0,0);
                g_platform = sel<0?0:sel; logLine(std::string("[VEHICLE] ")+PLAT_NAME[g_platform]);
                if(g_platform) logLine("[VEHICLE] Available: DIAGNOSE, TEACH-IN, Teach buttons, Clear DTCs, Identify. "
                                       "The other functions need the firmware of this TCU and stay Panamera 970 only."); }
            return 0; }
        { bool only970 = id==ID_READDONOR || id==ID_RESTOREWR || id==ID_LIVEROD || id==ID_VALVETEST || id==ID_FULLBACKUP ||
                         id==ID_READEEP || id==ID_READ13 || id==ID_READ14 || id==ID_ANALYZEEE;
          if(only970 && g_platform!=0){ MessageBoxA(h,"This function is available for the Panamera 970 (PDK 3000) only.\n\n"
                "It depends on the TCU firmware (memory addresses, flasher, EEPROM layout), which has been analysed for the 970 only.\n"
                "On this model use DIAGNOSE, TEACH-IN and Clear DTCs.","Panamera 970 only",MB_OK|MB_ICONINFORMATION); return 0; } }
        if(devAction){
            if(g_devState!=1){ if(MessageBoxA(h,"Tactrix not detected. Connect it (USB) and OBD/CAN.\n\nTry anyway?",
                "No device",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return 0; }
            bool teach = (id==ID_TEACH_S||id==ID_TEACH_C||id==ID_TEACH_H||id==ID_TEACHALL);
            bool write = (id==ID_RESTOREWR);
            if(id==ID_FULLBACKUP && MessageBoxA(h,"Reads the complete TCU flash (1.5 MB) and EEPROM via OBD (read-only).\n"
                   "Takes several minutes. Car standing, ignition ON, battery charger on.\n"
                   "Afterwards switch the ignition OFF for 10 s.\n\nStart?","Full backup",MB_YESNO|MB_ICONQUESTION)!=IDYES) return 0;
            if(id==ID_VALVETEST){
                g_valveSel=(int)SendMessageA(g_hValve,CB_GETCURSEL,0,0);
                std::string q=std::string("Actuator test of ")+VALVES[g_valveSel<0?0:g_valveSel]+
                   ".\n\nThe TCU energises this valve. With the engine running it can move a shift rod.\n"
                   "Use: ENGINE OFF, ignition ON, selector in P, parking brake on.\n\nStart?";
                if(MessageBoxA(h,q.c_str(),"Valve test",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return 0;
            }
            if(id==ID_CLEARDTC && MessageBoxA(h,"Clear the TCU fault memory?\n\nRun DIAGNOSE first - it saves a report with all faults and freeze frames.",
                   "Clear DTCs",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return 0;
            if(teach){
                if(MessageBoxA(h,
                   "TEACH-IN rewrites the gearbox calibration/adaptation in the ECU.\n\n"
                   "Run ONLY with: engine idling, selector in P, gearbox oil 60-90 C,\n"
                   "battery charger connected, no load, and AFTER a backup.\n"
                   "A wrong/interrupted teach-in can put the TCU in limp mode.\n\n"
                   "Start the teach-in?","Confirm teach-in",
                   MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return 0;
            }
            if(write){
                // Source: a file chosen in "Write file", otherwise offer the built-in
                // official Porsche KD Mitten Bin.
                char rf[8]; GetWindowTextA(g_hRestore,rf,sizeof(rf));
                std::string src;
                if(rf[0]){ g_restoreSrc=0; src="the file in 'Write file'"; }
                else {
                    int a=MessageBoxA(h,
                       "Compare block 0x13 in the TCU with what?\n\n"
                       "YES = official Porsche KD Mitten Bin (built in)\n"
                       "        (you choose the model year next)\n"
                       "NO  = choose a file (e.g. donor _block13.bin)\n"
                       "CANCEL = nothing",
                       "Check block 0x13 - reference",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);
                    if(a==IDCANCEL) return 0;
                    if(a==IDNO){ browse(g_hRestore,false); GetWindowTextA(g_hRestore,rf,sizeof(rf));
                                 if(!rf[0]){ logLine("[WRITE] No file selected."); return 0; }
                                 g_restoreSrc=0; src="the chosen file"; }
                    else {
                        int y=MessageBoxA(h,
                           "Which Mitten Bin?\n\n"
                           "YES = 970.618.970.00  - Panamera 970 MY 2010-2013 (A-D)\n"
                           "        and 970 II GTS\n"
                           "NO  = 970.618.971.00  - Panamera 970 II MY 2014+ (E and later)\n"
                           "CANCEL = nothing",
                           "Mitten Bin - model year",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON3);
                        if(y==IDCANCEL) return 0;
                        g_restoreSrc = (y==IDYES)?1:2;
                        src = (y==IDYES)?"Mitten Bin 970.618.970.00 (MY 2010-2013)":"Mitten Bin 970.618.971.00 (MY 2014+)";
                    }
                }
                std::string msg="Reads block 0x13 from the TCU (via the flasher), saves it and compares it with:\n"+src+
                   "\n\nNothing is written. The car must stand still, ignition ON. Afterwards switch the\n"
                   "ignition OFF for 10 s (the TCU stays in the flasher until then).\n\nContinue?";
                if(MessageBoxA(h,msg.c_str(),"Check block 0x13",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES) return 0;
            }
            startJob(id); return 0;
        }
        return 0; }
    case WM_APP_LOG: {
        char*s=(char*)l; int len=GetWindowTextLengthA(g_hLog);
        SendMessageA(g_hLog,EM_SETSEL,len,len);
        std::string line=std::string(s)+"\r\n";
        SendMessageA(g_hLog,EM_REPLACESEL,0,(LPARAM)line.c_str());
        free(s); return 0; }
    case WM_APP_DONE: logLine("[DONE]"); return 0;
    case WM_SIZE: {
        if(g_hLog){RECT rc;GetClientRect(h,&rc); MoveWindow(g_hLog,10,400,rc.right-20,rc.bottom-410,TRUE);} return 0; }
    case WM_DESTROY: g_stop=true; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h,m,w,l);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int show){
    WNDCLASSA wc={0}; wc.lpfnWndProc=WndProc; wc.hInstance=hi; wc.lpszClassName="PdkToolWnd";
    wc.hCursor=LoadCursor(0,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassA(&wc);
    g_hMain=CreateWindowA("PdkToolWnd","PdkTool - Porsche PDK (Panamera 970, 911 991, Boxster/Cayman 981/718, 918)",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,540,854,0,0,hi,0);
    ShowWindow(g_hMain,show); UpdateWindow(g_hMain);
    MSG msg; while(GetMessage(&msg,0,0,0)){TranslateMessage(&msg);DispatchMessage(&msg);}
    return 0;
}
