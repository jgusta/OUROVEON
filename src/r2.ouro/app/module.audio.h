//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  \\ harry denholm \\ ishani            ishani.org/shelf/ouroveon/
//
//  
//

#pragma once

#include "app/module.h"

#include "base/utils.h"
#include "base/id.simple.h"
#include "base/instrumentation.h"
#include "base/eventbus.h"

#include "spacetime/moment.h"

#include "rec/irecordable.h"
#include "ssp/isamplestreamprocessor.h"
#include "dsp/scope.h"
#include "effect/container.h"

// clap plugin support
#include "plug/stash.clap.h"
#include "plug/plug.clap.h"
#include "clap/clap.h"
#include "clap/version.h"
#include "clap/helpers/event-list.hh"


// portaudio
struct PaStreamCallbackTimeInfo;
struct PaStreamParameters;

namespace config { struct Audio; }
namespace nst { class Instance; }
namespace ssp { class WAVWriter; class FLACWriter; class OpusStream; }

namespace app {

struct StoragePaths;
struct CoreGUI;
struct AudioPlaybackTimeInfo;

namespace module {

struct MixerInterface;
struct Audio;

// ---------------------------------------------------------------------------------------------------------------------
struct CLAPEffect
{
    app::module::Audio*             m_audioModule = nullptr;        // pointer back to owning audio host
    std::atomic_bool                m_ready = false;
    std::string                     m_displayName;

    plug::runtime::CLAP::Instance   m_runtime;
    plug::online::CLAP::Instance    m_online;

    clap_host                       m_host;

    // depending if plugin is activated or not we may be fetching the runtime instance from the 
    // original object or from inside the activated online one; this just wraps that and there
    // should be no way for it not to return something (unless something has gone badly wrong)
    plug::runtime::CLAP& getRuntimeInstance()
    { 
        if ( m_runtime )
            return *( m_runtime.get() );
        else if ( m_online )
            return m_online->getRuntimeInstance();

        ABSL_UNREACHABLE();
    }
};

// ---------------------------------------------------------------------------------------------------------------------
struct Audio final
             : public Module
             , public rec::IRecordable
             , public effect::IContainer
{
    DECLARE_NO_COPY_NO_MOVE( Audio );

    Audio( const char* appName );
    ~Audio();

    using SampleProcessorInstances  = std::vector< ssp::SampleStreamProcessorInstance >;
    using SampleProcessorQueue      = mcc::ReaderWriterQueue< ssp::SampleStreamProcessorInstance >;


    // -----------------------------------------------------------------------------------------------------------------
    // fp32 working buffers for the mix process, covering somewhere to build results, flip buffers for plugin processing, 
    // spares for padding plugin inputs, etc. 
    //
    struct OutputBuffer
    {
        OutputBuffer( const uint32_t maxSamples )
            : m_maxSamples( maxSamples )
        {
            m_workingLR[0]      = mem::alloc16To<float>( m_maxSamples, 0.0f );
            m_workingLR[1]      = mem::alloc16To<float>( m_maxSamples, 0.0f );
            m_finalOutputLR[0]  = mem::alloc16To<float>( m_maxSamples, 0.0f );
            m_finalOutputLR[1]  = mem::alloc16To<float>( m_maxSamples, 0.0f );
            m_silence           = mem::alloc16To<float>( m_maxSamples, 0.0f );
            m_runoff            = mem::alloc16To<float>( m_maxSamples, 0.0f );
        }

        ~OutputBuffer()
        {
            mem::free16( m_runoff );
            mem::free16( m_silence );
            mem::free16( m_finalOutputLR[1] );
            mem::free16( m_finalOutputLR[0] );
            mem::free16( m_workingLR[1] );
            mem::free16( m_workingLR[0] );
        }

        // blit silence into all output channels
        inline void applySilence() const
        {
            memcpy( m_workingLR[0],   m_silence, sizeof( float ) * m_maxSamples );
            memcpy( m_workingLR[1],   m_silence, sizeof( float ) * m_maxSamples );
        }

        // .. apply silence from a sample offset to end of buffer
        inline void applySilenceFrom( const uint32_t sampleOffset ) const
        {
            if ( sampleOffset >= m_maxSamples )
                return;

            memcpy( m_workingLR[0] + sampleOffset, m_silence, sizeof( float ) * (m_maxSamples - sampleOffset) );
            memcpy( m_workingLR[1] + sampleOffset, m_silence, sizeof( float ) * (m_maxSamples - sampleOffset) );
        }


        uint32_t                m_maxSamples;
        std::array<float*, 2>   m_workingLR;        // data is written here by the mixer function
        std::array<float*, 2>   m_finalOutputLR;    // we use these as flip-flop working buffers for plugin processing
        float*                  m_silence;          // empty input channel available for use as plugin input (for example)
        float*                  m_runoff;           // empty output channel to pad out for plugins using >2 outputs
    };

    // standard controls for how the final signal is processed before it is handed off to plugin / etc
    struct OutputSignal
    {
        float   m_linearGain    = 1.0f;             // multiply result of 8x stem combination at end of mixers
    };

    // an arbitrary choice, as some things need an estimate upfront - and even once a portaudio stream is open the sample count
    // can potentially change. Choose something big to give us breathing room. 
    ouro_nodiscard constexpr int32_t getMaximumBufferSize() const { return 1024 * 16; }


    // Module
    absl::Status create( app::Core* appCore ) override;
    void destroy() override;
    virtual std::string getModuleName() const override { return "Audio"; };
    virtual bool isPreflightSetupComplete() const override;


    // initialise chosen audio device, configure streaming scope processing
    ouro_nodiscard absl::Status initOutput( const config::Audio& outputDevice, const config::Spectrum& scopeSpectrumConfig );
    void termOutput();

    ouro_nodiscard constexpr int32_t getSampleRate() const { ABSL_ASSERT( m_outSampleRate > 0 ); return m_outSampleRate; }

    ouro_nodiscard std::chrono::microseconds getOutputLatencyMs() const { return m_outLatencyMs; }


    struct ExposedState
    {
        // we profile different chunks of the mixer execution to track what specific stages might be sluggish
        // no timing or averaging is done in here, we leave that to the UI thread
        enum class ExecutionStage
        {
            Start,
            Mixer,                  // mixer logic
            Plugins,                // external audio plugin processing
            Scope,                  // streaming frequency analysis, fed back to visualisation / exchange
            Interleave,             // move results into PA buffers
            SampleProcessing,       // pushing new samples out to any attached processors, like record-to-disk or discord-transmit
            Count
        };
        static constexpr std::array< const char*, (size_t)ExecutionStage::Count > ExecutionStageEventName =
        {
            "Start",
            "Mixer",
            "Plugins",
            "Scope",
            "Interleave",
            "SampleProcessing"
            "Invalid"
        };

        static constexpr size_t     cNumExecutionStages         = (size_t)ExecutionStage::Count;

        using StageAverage      = base::RollingAverage<60>;
        using StageCounters     = std::array< StageAverage, cNumExecutionStages >;


        ExposedState()
        {
            m_samplePos             = 0;

            m_maxBufferFillSize     = 0;
            m_minBufferFillSize     = std::numeric_limits<uint32_t>::max();
        }

        // called after a block of work has been done to log performance metrics for that stage
        inline void mark( const ExecutionStage es )
        {
            const size_t stageIndex = (size_t)es;

            // [Start] is an empty block, so there will be no event running; cap off the last running event from last time round
            if ( stageIndex > 0 )
                base::instr::eventEnd();

            m_perfCounters[stageIndex].update( (double)m_timingMoment.delta< std::chrono::microseconds >().count() );
            m_timingMoment.setToNow();

            // on everything but the final stage, kick an instrumentation event out
            if ( stageIndex != cNumExecutionStages - 1 )
            {
                base::instr::eventBegin( "AUDIOPROC", ExecutionStageEventName[stageIndex + 1] );
            }
        }

        spacetime::Moment   m_timingMoment;

        uint64_t            m_samplePos;                // incremented by audio callback, tracks total sample count
        uint32_t            m_maxBufferFillSize;        // tracking range of min/max audio buffer fill request sizes
        uint32_t            m_minBufferFillSize;        //

        // perf counter snapshots at start/mid/end of mixer process
        StageCounters       m_perfCounters;
    };

    ouro_nodiscard constexpr const ExposedState& getState() const { return m_state; }
    ouro_nodiscard double getAudioEngineCPULoadPercent() const;

    ouro_nodiscard inline float getOutputSignalGain() const { return m_outputSignalGain; }
    inline void setOutputSignalGain( const float gain ) { m_outputSignalGain = gain; }



    AsyncCommandCounter toggleMute();
    ouro_nodiscard constexpr bool isMuted() const { return m_mute; }

    // get copy of the rolling FFT analysis of audio output
    ouro_nodiscard inline dsp::Scope8::Result getCurrentScopeResult() const
    {
        ABSL_ASSERT( m_scope != nullptr );
        return m_scope->getCurrentResult();
    }

    ouro_nodiscard bool isMainThreadID( const std::thread::id& idToCheck ) const { return m_mainThreadID == idToCheck; }
    ouro_nodiscard bool isAudioThreadID( const std::thread::id& idToCheck ) const { return m_audioThreadID == idToCheck; }

private:

    enum class MixThreadCommand
    {
        Invalid,
        SetMixerFunction,
        InstallPlugin,
        ClearAllPlugins,
        TogglePluginBypass,
        ToggleMute,
        AttachSampleProcessor,
        DetatchSampleProcessor
    };
    struct MixThreadCommandData : public base::BasicCommandType<MixThreadCommand> { using BasicCommandType::BasicCommandType; };
    using MixThreadCommandQueue = mcc::ReaderWriterQueue<MixThreadCommandData>;

    void ProcessMixCommandsOnMixThread();
#if OURO_HAS_CLAP
    void ProcessClapEventsOnMixThread();
#endif // OURO_HAS_CLAP

    static int PortAudioCallback(
        const void* inputBuffer,
        void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo,
        unsigned long /*PaStreamCallbackFlags*/ statusFlags,
        void* userData );

    int PortAudioCallbackInternal(
        void* outputBuffer,
        unsigned long framesPerBuffer,
        const PaStreamCallbackTimeInfo* timeInfo );



    MixThreadCommandQueue               m_mixThreadCommandQueue;
    std::atomic_uint32_t                m_mixThreadCommandsIssued   = 0;
    std::atomic_uint32_t                m_mixThreadCommandsComplete = 0;

    std::thread::id                     m_audioThreadID;
    std::thread::id                     m_mainThreadID;

    // active output 
    void*                               m_paStream          = nullptr;
    uint32_t                            m_outSampleRate     = 0;
    uint32_t                            m_outMaxBufferSize  = 0;
    std::chrono::microseconds           m_outLatencyMs;
    OutputBuffer*                       m_mixerBuffers      = nullptr;      // the aligned intermediate buffer, filled by the configurable mixer process
    bool                                m_threadInitOnce    = false;        // as PA controls the actual mix thread work, this is checked to let us do any once-on-init code inside the callback code
    bool                                m_mute              = false;

    ExposedState                        m_state;

    std::unique_ptr<dsp::Scope8>        m_scope;


#if OURO_FEATURE_NST24
    using PluginEffectSlots = std::vector< nst::Instance* >;

    PluginEffectSlots                   m_pluginStack;                      // plugin slots, executed in order
    bool                                m_pluginBypass = false;
#endif // OURO_FEATURE_NST24

    std::atomic<float>                  m_outputSignalGain  = cycfi::q::lin_float( { -6.0f } );        // gain control applied after merging stems, pre-plugin processing

    MixerInterface*                     m_mixerInterface    = nullptr;


    SampleProcessorQueue                m_sampleProcessorsToInstall;
    SampleProcessorInstances            m_sampleProcessorsInstalled;

    ssp::SampleStreamProcessorInstance  m_currentRecorderProcessor;

public:

    // rec::IRecordable
    bool beginRecording( const fs::path& outputPath, const std::string& filePrefix ) override;
    void stopRecording() override;
    bool isRecording() const override;
    uint64_t getRecordingDataUsage() const override;
    std::string_view getRecorderName() const override { return " Final Mix "; }


public: 

    // effect::IContainer
    int32_t getEffectSampleRate() const override { return getSampleRate(); }
    int32_t getEffectMaximumBufferSize() const override { return getMaximumBufferSize(); }

    AsyncCommandCounter toggleEffectBypass() override;
    bool isEffectBypassEnabled() const override;

    AsyncCommandCounter effectAppend( nst::Instance* nst ) override;
    AsyncCommandCounter effectClearAll() override;


public:

    AsyncCommandCounter installMixer( MixerInterface* mixIf );

    AsyncCommandCounter attachSampleProcessor( ssp::SampleStreamProcessorInstance sspInstance );
    AsyncCommandCounter detachSampleProcessor( ssp::StreamProcessorInstanceID sspID );

    void blockUntil( AsyncCommandCounter counter );


private:

#if OURO_HAS_CLAP

    // CLAP support
    clap_host                           m_clapHost;
    clap_host_log                       m_clapHostLog;
    clap_host_latency                   m_clapHostLatency;
    clap_host_audio_ports               m_clapAudioPorts;
    clap_host_params                    m_clapHostParams;
    clap_host_state                     m_clapHostState;
    clap_host_gui                       m_clapHostGui;
    clap_host_thread_check              m_clapHostThreadCheck;
    

    CLAPEffect*                         m_clapEffectTest = nullptr;


    clap::helpers::EventList            m_clapProcessEventsIn;
    clap::helpers::EventList            m_clapProcessEventsOut;
    clap_event_transport                m_clapProcessTransport;
    clap_process                        m_clapProcess;

    plug::stash::CLAP::Instance         m_pluginStashClap;


public:
    // routed calls from clap_host function table
    const void* clapGetExtension( CLAPEffect& clapEffect, const char* extension_id ) noexcept;
    void clapRequestRestart( CLAPEffect& clapEffect ) noexcept;
    void clapRequestProcess( CLAPEffect& clapEffect ) noexcept;
    void clapRequestCallback( CLAPEffect& clapEffect ) noexcept;

#endif // OURO_HAS_CLAP

public:

    void imgui( app::CoreGUI& coreGUI );
};

// interface class used to plug sound output generators into the Audio instance; this is how a client app interacts with the audio engine
struct MixerInterface
{
    virtual ~MixerInterface() {}

    // called from the realtime audio processing thread
    virtual void update(
        const Audio::OutputBuffer& outputBuffer,
        const Audio::OutputSignal& outputSignal,
        const uint32_t             samplesToWrite,
        const uint64_t             samplePosition ) = 0;

    // called from the main UI thread
    virtual void imgui() {}

    // optionally cast / return a rec::IRecordable interface to expose disk output functionality
    virtual rec::IRecordable* getRecordable() { return nullptr; }

    virtual const app::AudioPlaybackTimeInfo* getPlaybackTimeInfo() const { return nullptr; }
};

} // namespace module

// ---------------------------------------------------------------------------------------------------------------------
struct AudioDeviceQuery
{
    void findSuitable( const config::Audio& audioConfig );

    static int32_t generateStreamParametersFromDeviceConfig( const config::Audio& audioConfig, PaStreamParameters& streamParams );

    std::vector< std::string >      m_deviceNames;
    std::vector< const char* >      m_deviceNamesUnpacked;
    std::vector< double >           m_deviceLatencies;
    std::vector< int32_t >          m_devicePaIndex;
};

using AudioModule = std::unique_ptr<module::Audio>;


// ---------------------------------------------------------------------------------------------------------------------
// a shared memory block based on the timing data required to be fed into plugins. I'm not really sure this is a good idea!
// mixers can be handed a pointer and update it to sync data direct to all live effects. REFACTOR THIS LATER
//
struct AudioPlaybackTimeInfo
{
    double samplePos;           // current Position in audio samples (always valid)
    double sampleRate;          // current Sample Rate in Herz (always valid)
    double nanoSeconds;         // System Time in nanoseconds (10^-9 second)
    double ppqPos;              // Musical Position, in Quarter Note (1.0 equals 1 Quarter Note)
    double tempo;               // current Tempo in BPM (Beats Per Minute)
    double barStartPos;         // last Bar Start Position, in Quarter Note
    double cycleStartPos;       // Cycle Start (left locator), in Quarter Note
    double cycleEndPos;         // Cycle End (right locator), in Quarter Note
    int32_t timeSigNumerator;   // Time Signature Numerator (e.g. 3 for 3/4)
    int32_t timeSigDenominator; // Time Signature Denominator (e.g. 4 for 3/4)
};

} // namespace app

CREATE_EVENT_BEGIN( PanicStop )
CREATE_EVENT_END()
