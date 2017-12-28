#include <iostream>
#include <fstream>
#include <map>
#include <portaudio.h>
#include <math.h>

struct Sample {
    std::string name;
    uint64_t length;
    uint8_t finetune : 4;
    uint8_t volume;
    uint64_t loopstart;
    uint64_t looplength;
    uint8_t *data;
};

struct Note {
    uint16_t period : 12;
    uint8_t sample;
    uint8_t effect : 4;
    uint8_t argument;
};

struct Row {
    uint64_t nchannels;
    Note *notes;
};

struct Pattern {
    uint64_t nrows;
    Row *rows;
};

struct Module {
    std::string name;
    Sample *samples;
    uint64_t nsamples;
    uint64_t npatterns;
    uint64_t norders;
    uint8_t *orders;
    Pattern *patterns;
};

enum TrackerEffectQuirks {
    EFXISPANNING = 0x00000001
};

struct TrackerQuirks {
    TrackerQuirks() {}
    TrackerQuirks(uint64_t nchannels, uint32_t effectquirks) :
        nchannels(nchannels) , effectquirks(effectquirks) {}
    uint64_t nchannels = 4;
    uint32_t effectquirks = 0;
};

enum ModuleLoadState {
    LOAD_FAILED_HEADER,
    LOAD_FAILED_PATTERN,
    LOAD_FAILED_OTHER,
    LOAD_FAILED_SAMPLE,
    LOAD_OK
};

enum Verbosity {
    NONE,
    MESSAGE,
    DEBUG
};

struct ChannelState {
    ChannelState() {
        latchedperiod = 0;
        lasteffect = 0;
        liveperiod = 0;
        liveeffect = 0;
    }

    double samplepoint;
    unsigned short latchedperiod : 12;
    unsigned char  latchedsample = 0;
    unsigned char  latchedvolume = 0;
    unsigned char  lasteffectparam = 0;
    unsigned char  lasteffect : 4;
    unsigned short liveperiod : 12;
    unsigned char  livevolume = 0;
    unsigned char  liveeffect : 4;
    unsigned char  liveeffectparam = 0;
    unsigned int   offset = 0;
    bool inloop = false;
    unsigned int   loopcnt = 0;
};

struct TrackerState {
    ChannelState *cstate;
    uint64_t tpr = 6;
    uint64_t bpm = 125;
    uint64_t samplerate = 44100;
    uint64_t SamplesPerTick() {
        return samplerate * (2500.0/bpm)/1000.0;
    }
};

enum ReturnAction {
    INC,
    LOOP,
    JUMP
};

struct TickReturn {
    uint8_t *audio;
    uint64_t nsamples;
    ReturnAction action;
};

enum PlayReturn {
    PLAY_OK,
    PLAY_FAILED
};

class PeriodCorrector {
public:
    PeriodCorrector() {
        GeneratePTPeriodTable(periods);
    }

    unsigned short CorrectPeriod(unsigned short period, unsigned char finetune) {
        for(int i = 0; i < 36; i++) {
            if(periods[i][0] == period)
                return periods[i][finetune];
        }
        return 0;
    }

private:
    double pow2(double x) {
        return pow(2.0, x);
    }

    void GeneratePTPeriodTable(unsigned short (*periods)[36]) {
        const double NTSC_CLK        = 3579545.0;
        const double REF_PERIOD_PT   = 856.0;
        const double REF_PERIOD_UST  = NTSC_CLK / 523.3 / 8;
        const double UST_TO_PT_RATIO = REF_PERIOD_UST / REF_PERIOD_PT;
        const double semitone_step   = pow2(-1.0/12.0);
        const double tune_step       = pow2(-1.0/8.0 * 1.0/12.0);
        int n, t;
        // Initialize with starting period, i.e. 907
        double p1 = REF_PERIOD_PT / semitone_step;
        for(t = 0 ; t < 8 ; t++) {
            // Initialize with starting period for current tuning
            double p2 = p1;
            for(n = 0 ; n < 36 ; n++) {
                // Round and save current period, update period for next Semitone
                periods[t+8][n]   = (unsigned short)(p2 + 0.5);
                p2               *= semitone_step;
                periods[t][n]     = (unsigned short)(p2 + 0.5);
                // Save correct UST period for normal tuning
                if(t == 0) {
                    periods[0][n] = (unsigned short)(p2 * UST_TO_PT_RATIO + 0.5);
                }
            }
            // Starting period for next tuning
            p1 *= tune_step;
        }
        // Create correct values for the octave halved periods for normal tuning
        for(n = 0 ; n < 9 ; n++)   { periods[0][n] = periods[0][n+12] * 2; }
        // Copy UST periods to tuning -8
        for(n = 1 ; n < 36 ; n++)  { periods[8][n] = periods[0][n-1];      }
        // Correct those 9 #?!$?#!%!! entries that refuse
        periods[1][ 4]--;  periods[1][22]++;  periods[ 1][24]++;
        periods[2][23]++;  periods[4][ 9]++;  periods[ 7][24]++;
        periods[9][ 6]--;  periods[9][26]--;  periods[12][34]--;
    }

    unsigned short periods[36][36];
};

class ModulePlayer {
public:
    ModulePlayer(std::fstream &moduledata, Verbosity verbosity = MESSAGE) : verbosity(verbosity) {
        if(!LoadModuleHeader(moduledata)) {
            loadstate = LOAD_FAILED_HEADER;
            return;
        }
        if(!LoadSampleHeaders(moduledata)) {
            loadstate = LOAD_FAILED_SAMPLE;
            return;
        }
        if(!LoadPatternsAndOrders(moduledata)) {
            loadstate = LOAD_FAILED_PATTERN;
            return;
        }
        if(!LoadSampleData(moduledata)){
            loadstate = LOAD_FAILED_SAMPLE;
            return;
        }
        state.cstate = new ChannelState[mod.patterns[0].rows[0].nchannels];
        loadstate = LOAD_OK;
    }

    PlayReturn playModule() {
        if(verbosity > NONE) {
            switch(loadstate) {
                case LOAD_OK:
                    std::cout << "Module load was successful, starting playback!"
                              << std::endl;
                case LOAD_FAILED_HEADER:
                    std::cout << "Module Load failed at header, is this a MOD file?"
                              << std::endl;
                    return PLAY_FAILED;
                case LOAD_FAILED_PATTERN:
                    std::cout << "Module load failed at pattern loading, module may be corrupted."
                              << std::endl;
                    return PLAY_FAILED;
                case LOAD_FAILED_SAMPLE:
                    std::cout << "Module load failed at sample loading, module may be corrupted."
                              << std::endl;
                    return PLAY_FAILED;
                case LOAD_FAILED_OTHER:
                    std::cout << "Module load failed in an unknown way. Oh no."
                              << std::endl;
                    return PLAY_FAILED;
            }
        }
        for(int i = 0;)
        return PLAY_OK;
    }

    TickReturn PlayOneTick(uint64_t order, uint64_t row, uint8_t tick) {
        TickReturn ret;
        ret.action = INC;
        return ret;
    }

private:
    int LoadSampleData(std::fstream &moduledata) {
        for(uint64_t i = 0; i < mod.nsamples; i++) {
            if(!mod.samples[i].length)
                continue;
            mod.samples[i].data = new uint8_t[mod.samples[i].length];
            moduledata.read((char*)mod.samples[i].data, mod.samples[i].length);
            if(!moduledata.good()) {
                return 0;
            }
        }
        return 1;
    }

    int LoadModuleHeader(std::fstream &moduledata) {
        bool soundtracker = false;
        moduledata.seekg(1080);
        std::string signature;
        for(int i = 0; i < 4; i++) {
            signature += moduledata.get();
        }
        if(!moduledata.good()) {
            return 0;
        }
        for(char c : signature) {
            if((c < 32) && (c > 126)) {
                soundtracker = true;
                break;
            }
        }
        moduledata.seekg(0);
        for(int i = 0; i < 20; i++) {
            mod.name = moduledata.get();
        }
        if(!moduledata.good()) {
            return 0;
        }
        mod.nsamples = soundtracker ? 15 : 31;
        return 1;
    }

    int LoadSampleHeaders(std::fstream &moduledata) {
        mod.samples = new Sample[mod.nsamples];
        for(uint64_t i = 0; i < mod.nsamples; i++) {
            for(int z = 0; z < 22; z++) {
                mod.samples[i].name = moduledata.get();
            }
            mod.samples[i].length |= (unsigned long)moduledata.get() << 8;
            mod.samples[i].length |= moduledata.get();
            mod.samples[i].length *= 2;
            mod.samples[i].finetune = moduledata.get();
            mod.samples[i].volume = moduledata.get();
            mod.samples[i].loopstart |= (unsigned long)moduledata.get() << 8;
            mod.samples[i].loopstart |= moduledata.get();
            mod.samples[i].loopstart *= 2;
            mod.samples[i].looplength |= (unsigned long)moduledata.get() << 8;
            mod.samples[i].looplength |= moduledata.get();
            mod.samples[i].looplength *= 2;
            if(!moduledata.good()) {
                return 0;
            }
        }
        return 1;
    }

    void GenerateFastAndTakeTrackerChannelDefinitions(std::map<std::string, TrackerQuirks> &quirks) {
        for(int i = 10; i < 33; i++) {
            if(i % 2 == 0) {
                quirks[std::to_string(i) + "CN"] = TrackerQuirks(i, 0);
                quirks[std::to_string(i) + "CH"] = TrackerQuirks(i, 0);
            }
        }
    }

    int LoadPatternsAndOrders(std::fstream &moduledata) {
        mod.norders = moduledata.get();
        mod.orders = new uint8_t[mod.norders];
        moduledata.get();
        //Restart point, unsure of what to do with it.
        for(uint64_t i = 0; i < 128; i++){
            if(i < mod.norders) {
                mod.orders[i] = moduledata.get();
            } else {
                uint8_t item = moduledata.get();
                mod.npatterns = mod.npatterns > (item + 1) ? (mod.npatterns) : (item + 1);
            }
        }
        if(!moduledata.good()) {
            return 0;
        }
        std::string sampletag = "";
        for(int i = 0; i < 4; i++) {
            sampletag += moduledata.get();
        }
        if(!moduledata.good()) {
            return 0;
        }
        uint64_t nchannels = 0;
        std::map<std::string, TrackerQuirks> tag;
        tag["6CHN"] = TrackerQuirks(6, 0);
        tag["8CHN"] = TrackerQuirks(8, 0);
        tag["OCTA"] = TrackerQuirks(8, 0);
        tag["OKTA"] = TrackerQuirks(8 ,0);
        tag["CD81"] = TrackerQuirks(8, 0);
        tag["TDZ1"] = TrackerQuirks(1, 0);
        tag["TDZ2"] = TrackerQuirks(2, 0);
        tag["TDZ3"] = TrackerQuirks(3, 0);
        tag["5CHN"] = TrackerQuirks(5, 0);
        tag["7CHN"] = TrackerQuirks(7, 0);
        tag["9CHN"] = TrackerQuirks(9, 0);
        GenerateFastAndTakeTrackerChannelDefinitions(tag);
        nchannels = tag[sampletag].nchannels;
        mod.patterns = new Pattern[mod.npatterns];
        for(uint8_t i = 0; i < mod.npatterns; i++) {
           mod.patterns[i].nrows = 64;
           mod.patterns[i].rows = new Row[64];
           for(uint64_t row = 0; i < mod.patterns[i].nrows; row++){
               mod.patterns[i].rows[row].nchannels = nchannels;
               mod.patterns[i].rows[row].notes = new Note[nchannels];
               for(uint64_t channel = 0; channel < nchannels; channel++) {
                   uint32_t note = 0;
                   note |= (unsigned long)moduledata.get() << 24;
                   note |= (unsigned long)moduledata.get() << 16;
                   note |= (unsigned long)moduledata.get() << 8;
                   note |= (unsigned long)moduledata.get();
                   Note stnote;
                   stnote.period = (note & 0x0F000000 >> 12) | (note & 0x00FF0000) >> 16;
                   stnote.effect = (note & 0x00000F00 >> 8);
                   stnote.argument = note & 0x000000FF;
                   stnote.sample = ((note & 0xF0000000) >> 24) & (note & 0x0000F000) >> 12;
                   mod.patterns[i].rows[row].notes[channel] = stnote;
               }
           }
        }
        if(!moduledata.good()) {
            return 0;
        }
        return 1;
    }

    PeriodCorrector corrector;
    Verbosity verbosity = NONE;
    TrackerState state;
    ModuleLoadState loadstate = LOAD_FAILED_OTHER;
    Module mod;
};

int main(int argc, char *argv[])
{
    if(argc > 1) {
        std::fstream f(argv[1], std::ios_base::in | std::ios_base::binary);
        ModulePlayer player(f);
    }
    return 0;
}
