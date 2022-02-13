#include "pch.h"

#include "base/utils.h"

#include "config/data.h"

#include "app/module.audio.h"
#include "app/module.frontend.h"
#include "app/module.frontend.fonts.h"

#include "app/ouro.h"

#include "ssp/wav.h"
#include "ssp/flac.h"

#include "discord/discord.bot.ui.h"

#include "effect/vst2/host.h"

#include "endlesss/all.h"

#include "beam.fx.vst.h"
#include "beam.fx.databus.h"

#include "gl/shader.h"

#define MIX_ISPC 1
#if MIX_ISPC
#include "ispc/.gen/mix_ispc.gen.h"
#endif 

#define OUROVEON_BEAM           "BEAM"
#define OUROVEON_BEAM_VERSION   "0.6.1-alpha"

using namespace std::chrono_literals;


// ---------------------------------------------------------------------------------------------------------------------
struct MixEngine : public app::module::MixerInterface, 
                   public rec::IRecordable
{
    using AudioBuffer = app::module::Audio::OutputBuffer;

    static constexpr uint32_t cSampleCountMax = std::numeric_limits<uint32_t>::max();

    // 
    struct ProgressionConfiguration
    {
        ProgressionConfiguration()
            : m_triggerPoint( TriggerPoint::AnyBarStart )
            , m_blendTime( BlendTime::TwoBars )
            , m_greedyMode( false )
        {}

        bool operator==( ProgressionConfiguration const& ) const = default;

        // choose when to begin blending to the next riff
        enum class TriggerPoint
        {
            Arbitrary,                      // start blending whenever a new riff arrives, yolo
            NextRiffStart,                  //             .. when a riff loops around to the beginning (ie. once per riff loop)
            AnyBarStart,                    //             .. when a bar segment is crossed (or near enough) (ie. usually 8 opportunities per riff loop)
            AnyEvenBarStart,                //             .. when an even-numbered bar is crossed
        }               m_triggerPoint;

        static constexpr size_t cTriggerPointCount = 4;
        static constexpr std::array< const char*, cTriggerPointCount > cTriggerPointNames {{
            "At Any Point",
            "At Riff Start",
            "At Any Bar Start",
            "At Even Bar Start"
        }};
        inline static const char* getTriggerPointName( const TriggerPoint tp )
        {
            return cTriggerPointNames[(size_t)tp];
        }
        inline static bool triggerPointGetter( void* data, int idx, const char** out_text )
        {
            *out_text = getTriggerPointName( (TriggerPoint)idx );
            return true;
        }

        // how long the blend should take
        enum class BlendTime
        {
            Zero,
            OneBar,
            TwoBars,
            FourBars,
            EightBars
        }               m_blendTime;

        static constexpr size_t cBlendTimeCount = 5;
        static constexpr std::array< const char*, cBlendTimeCount > cBlendTimeNames {{
            "Instant",
            "One Bar",
            "Two Bars",
            "Four Bars",
            "Eight Bars"
        }};
        inline static const char* getBlendTimeName( const BlendTime bt )
        {
            return cBlendTimeNames[(size_t)bt];
        }
        inline static bool blendTimeGetter( void* data, int idx, const char** out_text )
        {
            *out_text = getBlendTimeName( (BlendTime)idx );
            return true;
        }
        inline float getBlendTimeMultiplier() const
        {
            switch ( m_blendTime )
            {
            default:
            case ProgressionConfiguration::BlendTime::OneBar:    return 1.0f;
            case ProgressionConfiguration::BlendTime::TwoBars:   return 2.0f;
            case ProgressionConfiguration::BlendTime::FourBars:  return 4.0f;
            case ProgressionConfiguration::BlendTime::EightBars: return 8.0f;
            }
        }

        bool            m_greedyMode;       // if on and there are multiple 'next riffs' in the queue when it comes time to begin
                                            // blending, empty the list and only blend to the most recent one. if off, we will
                                            // work our way through each enqueued change in turn
    };

    struct RepComConfiguration
    {
        bool            m_enable = false;   // if multi-track is started, use repcom logic
    };

    enum class EngineCommand
    {
        Invalid,
        BeginRecording,
        StopRecording,
        UpdateProgressionConfiguration,     // pass ProgressionConfiguration*
        UpdateRepComConfiguration,          // pass RepComConfiguration*
    };
    struct EngineCommandData : public base::BasicCommandType<EngineCommand> { using BasicCommandType::BasicCommandType; };

    using CommandQueue  = mcc::ReaderWriterQueue<EngineCommandData>;
    using RiffQueue     = mcc::ReaderWriterQueue<endlesss::live::RiffPtr>;

    uint32_t                    m_audioBufferSize;
    uint32_t                    m_audioSampleRate;


    std::array< float*, 8 >     m_mixChannelLeft;
    std::array< float*, 8 >     m_mixChannelRight;

    app::AudioPlaybackTimeInfo  m_unifiedTimeInfo;

    uint64_t                    m_samplePosition;


    endlesss::live::RiffPtr     m_riffCurrent;
    uint32_t                    m_riffLengthInSamples;
    double                      m_riffPlaybackPercentage;
    int32_t                     m_riffPlaybackBar;
    int32_t                     m_riffPlaybackBarSegment;

    RiffQueue                   m_riffQueue;
    CommandQueue                m_commandQueue;

    endlesss::live::RiffPtr     m_riffNext;
    float                       m_transitionValue;

    float                       m_stemBeatRate;
    double                      m_transitionRate;

    ProgressionConfiguration    m_progression;


    MixEngine( const app::AudioModule& audioEngine )
        : m_samplePosition( 0 )
        , m_riffCurrent( nullptr )
        , m_riffNext( nullptr )
        , m_transitionValue( 0 )
        , m_stemBeatRate( 4.0f )
        , m_transitionRate( 0.25 )
        , m_multiTrackInFlux( false )
        , m_multiTrackWaitingToRecordOnRiffEdge( false )
        , m_multiTrackRecording( false )
        , m_repcomRepeatBar( 0 )
        , m_repcomPausedOnBar( -1 )
        , m_repcomRepeatLimit( 0 )
        , m_repcomSampleStart( 0 )
        , m_repcomSampleEnd( cSampleCountMax )
        , m_repcomState( RepComState::Unpaused )
    {
        memset( &m_unifiedTimeInfo, 0, sizeof( m_unifiedTimeInfo ) );

        m_audioBufferSize = audioEngine->getMaximumBufferSize();
        m_audioSampleRate = audioEngine->getSampleRate();

        for ( size_t mI = 0; mI < 8; mI++ )
        {
            m_mixChannelLeft[mI]  = mem::malloc16As<float>( m_audioBufferSize );
            m_mixChannelRight[mI] = mem::malloc16As<float>( m_audioBufferSize );
        }
    }

    virtual ~MixEngine()
    {
        for ( float* channel : m_mixChannelLeft )
            mem::free16( channel );
        m_mixChannelLeft.fill( nullptr );

        for ( float* channel : m_mixChannelRight )
            mem::free16( channel );
        m_mixChannelRight.fill( nullptr );
    }

    inline void addNextRiff( endlesss::live::RiffPtr& nextRiff )
    {
        m_riffQueue.emplace( nextRiff );
    }

    inline void updateProgressionConfiguration( const ProgressionConfiguration* pConfig )
    {
        m_commandQueue.emplace( EngineCommand::UpdateProgressionConfiguration, (void*) pConfig );
    }

    inline void updateRepComConfiguration( const RepComConfiguration* pConfig )
    {
        m_commandQueue.emplace( EngineCommand::UpdateRepComConfiguration, (void*)pConfig );
    }

    inline uint32_t getBarRepetitions() const { return m_repcomRepeatBar; }

    inline bool isRepComEnabled() const { return m_repcom.m_enable; }
    inline bool isRepComPaused() const { return isRepComEnabled() && ( m_repcomState != RepComState::Unpaused ); }
    inline int32_t getBarPausedOn() const { return m_repcomPausedOnBar; }


    void update(
        const AudioBuffer&  outputBuffer,
        const float         outputVolume,
        const uint32_t      samplesToWrite,
        const uint64_t      samplePosition ) override;

    void commit(
        const AudioBuffer&  outputBuffer,
        const float         outputVolume,
        const uint32_t      samplesToWrite )
    {
        ispc::downmix_8channel_stereo(
            outputVolume,
            samplesToWrite,
            m_mixChannelLeft[0],
            m_mixChannelLeft[1],
            m_mixChannelLeft[2],
            m_mixChannelLeft[3],
            m_mixChannelLeft[4],
            m_mixChannelLeft[5],
            m_mixChannelLeft[6],
            m_mixChannelLeft[7],
            m_mixChannelRight[0],
            m_mixChannelRight[1],
            m_mixChannelRight[2],
            m_mixChannelRight[3],
            m_mixChannelRight[4],
            m_mixChannelRight[5],
            m_mixChannelRight[6],
            m_mixChannelRight[7],
            outputBuffer.m_workingLR[0],
            outputBuffer.m_workingLR[1]);

        if ( m_multiTrackRecording )
        {
            const bool repComEnabled = isRepComEnabled();

            // repetition compression is being activated or suspended, meaning we need to take just a chunk of the 
            // presented samples rather than all of it
            if ( repComEnabled && ( m_repcomState == RepComState::SampleFragmentAndPause ||
                                    m_repcomState == RepComState::SampleFragmentAndResume ) )
            {
                const auto fragmentSampleCount = std::min( (uint32_t)m_repcomSampleEnd, samplesToWrite ) - m_repcomSampleStart;

                blog::mix( "[ REPCOM ] Fragmenting ({})  [ {} ] -> [ {} ]  ({} samples)",
                    ( m_repcomState == RepComState::SampleFragmentAndPause ) ? "Pausing" : "Resuming",
                    m_repcomSampleStart,
                    m_repcomSampleStart + fragmentSampleCount,
                    fragmentSampleCount );

                for ( auto i = 0; i < 8; i++ )
                {
                    m_multiTrackOutputs[i]->appendSamples(
                        m_mixChannelLeft[i] + m_repcomSampleStart,
                        m_mixChannelRight[i] + m_repcomSampleStart,
                        fragmentSampleCount );
                }
            }
            else if ( !repComEnabled || m_repcomState == RepComState::Unpaused )
            {
                for ( auto i = 0; i < 8; i++ )
                {
                    m_multiTrackOutputs[i]->appendSamples( m_mixChannelLeft[i], m_mixChannelRight[i], samplesToWrite );
                }
            }
        }
        {
            if ( m_repcomState == RepComState::SampleFragmentAndPause )
                 m_repcomState  = RepComState::Paused;
            if ( m_repcomState == RepComState::SampleFragmentAndResume )
            {
                m_repcomState       = RepComState::Unpaused;
                m_repcomPausedOnBar = -1;
            }

            m_repcomSampleStart = 0;
            m_repcomSampleEnd   = cSampleCountMax;
        }
    }

    void mainThreadUpdate( const float dT, endlesss::Exchange& beatEx )
    {
        for ( auto& sb : m_stemBeats )
            sb.update( dT, m_stemBeatRate );

        m_stemBeatConsensus += (-0.1f - m_stemBeatConsensus) * (dT * m_stemBeatRate);
        m_stemBeatConsensus = std::clamp( m_stemBeatConsensus, 0.0f, 1.0f );

        beatEx.m_consensusBeat = m_stemBeatConsensus;
        for ( auto stemI = 0U; stemI < 8; stemI++ )
        {
            beatEx.m_stemPulse[stemI]  = m_stemBeats[stemI].m_pulse;
            beatEx.m_stemEnergy[stemI] = m_stemEnergy[stemI];
        }
    }

    struct StemBeats
    {
        std::vector< float >    m_hits;
        float                   m_pulse;

        inline void update( float dT, const float beatRate )
        {
            std::vector< float > newHits;
            if ( m_pulse == 1.0f )
                newHits.push_back( 0.0f );

            for ( auto i = 0; i < m_hits.size(); i++ )
            {
                const float c = m_hits[i] + dT;
                if ( c < 1.0f )
                    newHits.push_back( c );
            }
            m_hits = std::move( newHits );

            m_pulse += ( -0.1f - m_pulse ) * ( dT * beatRate );
            m_pulse = std::clamp( m_pulse, 0.0f, 1.0f );
        }
    };

    std::array< StemBeats, 8 >  m_stemBeats;
    float                       m_stemBeatConsensus;
    std::array< float, 8 >      m_stemEnergy;


// ---------------------------------------------------------------------------------------------------------------------
// rec::IRecordable

public:

    inline bool beginRecording( const std::string& outputPath, const std::string& filePrefix ) override
    {
        // should not be calling this if we're already in the process of streaming out
        assert( !isRecording() );
        if ( isRecording() )
            return false;

        fs::path recordingsPath{ outputPath };

        // set up 8 WAV output streams, one for each Endlesss channel
        for ( auto i = 0; i < 8; i++ )
        {
            auto recordFile = recordingsPath / fmt::format( "{}beam_channel{}.flac", filePrefix, i );
            m_multiTrackOutputs[i] = ssp::FLACWriter::Create(
                recordFile.string(),
                m_audioSampleRate,
                i );
        }

        // tell the worker thread to begin writing to our streams
        m_commandQueue.enqueue( EngineCommand::BeginRecording );
        m_multiTrackInFlux = true;

        return true;
    }

    inline void stopRecording() override
    {
        // can't stop what hasn't started
        assert( isRecording() );
        if ( !isRecording() )
            return;

        m_commandQueue.enqueue( EngineCommand::StopRecording );
        m_multiTrackInFlux = true;
    }

    // either we're fully engaged with writing out the stream or the request to do (or to stop) is still in-flight
    inline bool isRecording() const override
    {
        return m_multiTrackRecording || 
               m_multiTrackInFlux;
    }

    inline uint64_t getRecordingDataUsage() const
    {
        if ( !isRecording() )
            return 0;

        uint64_t usage = 0;
        for ( auto i = 0; i < 8; i++ )
            usage += m_multiTrackOutputs[i]->getStorageUsageInBytes();

        return usage;
    }

    inline const char* getRecorderName() const override { return " Multitrack "; }
    inline const char* getFluxState() const override
    {
        if ( m_repcomState != RepComState::Unpaused )
            return "[PAUSED] ";
        if ( m_multiTrackInFlux )
            return " Awaiting Loop Start";

        return nullptr;
    }


private:

    using MultiTrackStreams = std::array < std::unique_ptr<ssp::FLACWriter>, 8 >;

    bool                m_multiTrackInFlux;
    bool                m_multiTrackWaitingToRecordOnRiffEdge;
    bool                m_multiTrackRecording;
    MultiTrackStreams   m_multiTrackOutputs;

    // multitrack "repetition compression" (RepCom) 

    // repcom config written to via engine command
    RepComConfiguration m_repcom;

    // repcom internal state
    int32_t             m_repcomRepeatBar;
    int32_t             m_repcomPausedOnBar;
    int32_t             m_repcomRepeatLimit;
    uint32_t            m_repcomSampleStart;
    uint32_t            m_repcomSampleEnd;
    enum class RepComState
    {
        Unpaused,
        SampleFragmentAndPause,
        Paused,
        SampleFragmentAndResume
    }                   m_repcomState;
};

// ---------------------------------------------------------------------------------------------------------------------
void MixEngine::update(
    const AudioBuffer&  outputBuffer,
    const float         outputVolume,
    const uint32_t      samplesToWrite,
    const uint64_t      samplePosition )
{
    m_samplePosition = samplePosition;
    m_unifiedTimeInfo.samplePos = (double)samplePosition;

    const double linearTimeStep = (double)samplesToWrite / (double)m_audioSampleRate;

    const auto notifyRepComOfActivity = [&]( uint32_t newStartSample )
    {
        assert( m_repcomState != RepComState::SampleFragmentAndResume );
        if ( m_repcomState == RepComState::Paused )
        {
            blog::mix( "[ REPCOM ] UNPAUSE on new activity @ {} -> {}", newStartSample, samplesToWrite );

            m_repcomSampleStart = newStartSample;
            m_repcomSampleEnd   = cSampleCountMax;
            m_repcomState       = RepComState::SampleFragmentAndResume;
        }
    };

    // flag to indicate that the local data buffers need updating with m_riffCurrent data
    // .. this is reset mid-cycle in the case we have to handle a transition event where the
    // riff might change underneith us (via exchangeLiveRiff)
    bool riffUnpackRequired = true;

    // swap in the Next riff, mark transition/unpack flags as appropriate
    const auto exchangeLiveRiff = [&]
    {
        m_riffCurrent           = m_riffNext;
        m_riffNext              = nullptr;
        m_transitionValue       = 0.0f;
        m_repcomRepeatBar       = 0;

        riffUnpackRequired      = true;
    };

    // process commands from the main thread
    {
        EngineCommandData engineCmd;
        if ( m_commandQueue.try_dequeue( engineCmd ) )
        {
            switch ( engineCmd.getCommand() )
            {
                // start multitrack on the edge of a riff; the actual recording begins 
                // when that is detected during the sample loop below
                case EngineCommand::BeginRecording:
                {
                    m_multiTrackWaitingToRecordOnRiffEdge = true;
                }
                break;

                // cleanly disengage recording from the mix thread
                case EngineCommand::StopRecording:
                {
                    m_multiTrackWaitingToRecordOnRiffEdge = false;
                    m_multiTrackRecording                 = false;
                    m_multiTrackInFlux                    = false;

                    // on deletion, files will commit and release
                    for ( auto i = 0; i < 8; i++ )
                        m_multiTrackOutputs[i].reset();

                    blog::mix( "[ Multitrack ] ... Stopped" );
                }
                break;

                case EngineCommand::UpdateProgressionConfiguration:
                {
                    assert( engineCmd.getPtr() != nullptr );
                    m_progression = *engineCmd.getPtrAs< ProgressionConfiguration >();
                }
                break;

                case EngineCommand::UpdateRepComConfiguration:
                {
                    assert( !isRecording() );   // should not be allowed to update if recording is already underway
                    assert( engineCmd.getPtr() != nullptr );
                    m_repcom = *engineCmd.getPtrAs< RepComConfiguration >();
                }
                break;

                default:
                case EngineCommand::Invalid:
                    blog::error::mix( "Unknown or invalid command received" );
                    break;
            }
        }
    }

    const auto checkForAndDequeueNextRiff = [&]
    {
        if ( m_riffNext == nullptr )
        {
            if ( m_riffQueue.peek() != nullptr &&
                 m_transitionValue == 0 )
            {
                if ( !m_riffQueue.try_dequeue( m_riffNext ) )
                {
                    assert( false );
                    return false;
                }

                blog::mix( "[ BLEND ] DEQUEUED new riff" );

                if ( m_riffNext->getSyncState() != endlesss::live::Riff::SyncState::Success )
                {
                    blog::error::mix( "[ BLEND ] .. new riff invalid, ignoring it" );
                    m_riffNext = nullptr;
                }

                // hard cut
                if ( m_progression.m_blendTime == ProgressionConfiguration::BlendTime::Zero )
                {
                    blog::mix( "[ BLEND ] hard cut" );
                    exchangeLiveRiff();
                }
                // pick blend rate based on how many bars to take
                else
                {
                    m_transitionRate = 1.0 / (m_riffNext->m_timingDetails.m_lengthInSecPerBar * m_progression.getBlendTimeMultiplier());
                }
            }
        }
        return (m_riffNext != nullptr);
    };

    // in Arbitrary mode, check all the time to see if we could be switching
    if ( m_progression.m_triggerPoint == ProgressionConfiguration::TriggerPoint::Arbitrary )
    {
        checkForAndDequeueNextRiff();
    }

    // update the transition, if one is active; and swap in the next riff if one has completed
    if ( m_riffNext != nullptr )
    {
        m_transitionValue += (float)(linearTimeStep * m_transitionRate);

        if ( m_transitionValue >= 1.0f )
        {
            blog::mix( "[ BLEND ] completed" );
            exchangeLiveRiff();
        }
    }

    // early out if we have nothing to play
    if ( m_riffCurrent == nullptr ||
         m_riffCurrent->m_timingDetails.m_lengthInSamples == 0 )
    {
        outputBuffer.applySilence();

        // reset computed riff playback variables
        m_riffPlaybackPercentage  = 0;
        m_riffPlaybackBar         = 0;
        m_riffPlaybackBarSegment  = 0;

        // check if there's something on the horizon
        if ( m_riffQueue.peek() != nullptr )
        {
            if ( checkForAndDequeueNextRiff() )
            {
                blog::mix( "[ BLEND ] hard cut to first riff" );
                exchangeLiveRiff();
                notifyRepComOfActivity( 0 );
            }
        }

        return;
    }



    const endlesss::live::Riff* currentRiff = m_riffCurrent.get();

    // compute where we are (roughly) for the UI
    currentRiff->getTimingDetails().ComputeProgressionAtSample( m_samplePosition, m_riffPlaybackPercentage, m_riffPlaybackBar, m_riffPlaybackBarSegment );


    // update vst time structure with latest state
    m_unifiedTimeInfo.tempo              = currentRiff->m_timingDetails.m_bpm;
    m_unifiedTimeInfo.timeSigNumerator   = currentRiff->m_timingDetails.m_quarterBeats;
    m_unifiedTimeInfo.timeSigDenominator = 4;


    std::array< bool,  16 >         stemHasBeat;
    std::array< float, 16 >         stemEnergy;
    std::array< float, 16 >         stemTimeStretch;
    std::array< float, 16 >         stemGains;
    std::array< endlesss::live::Stem*, 16 >   stemPtr;

    // keep note of where we are mixing in terms of the 0..N sample count of the current riff
    std::array< uint32_t, 2 >       riffLengthInSamples;
    std::array< uint32_t, 2 >       riffWrappedSampleStart;
    
    stemHasBeat.fill( false );
    stemEnergy.fill( 0.0f );
    stemTimeStretch.fill( 0.0f );
    stemGains.fill( 0.0f );
    stemPtr.fill( nullptr );

    riffLengthInSamples.fill( 0 );
    riffWrappedSampleStart.fill( 0 );

    // unpack our primary foreground riff into the local data buffers used during sample filling
    const auto decodeForegroundRiffData = [&]
    {
        riffLengthInSamples[0]      = currentRiff->m_timingDetails.m_lengthInSamples;
        riffWrappedSampleStart[0]   = samplePosition % riffLengthInSamples[0];

        for (auto stemI = 0U; stemI < 8; stemI++)
        {
            stemTimeStretch[stemI]  = currentRiff->m_stemTimeScales[stemI];
            stemGains[stemI]        = currentRiff->m_stemGains[stemI];
            stemPtr[stemI]          = currentRiff->m_stemPtrs[stemI];
        }
    };
    decodeForegroundRiffData();

    // unpack transitional riff? can be called mid-loop if we pull a new riff off the pile
    const auto decodeTransitionalRiffData = [&]
    {
        if ( m_transitionValue > 0 )
        {
            const auto* nextRiff = m_riffNext.get();

            riffLengthInSamples[1]      = nextRiff->m_timingDetails.m_lengthInSamples;
            riffWrappedSampleStart[1]   = samplePosition % riffLengthInSamples[1];

            for ( auto stemI = 0U; stemI < 8; stemI++ )
            {
                stemTimeStretch[ 8 + stemI ]  = nextRiff->m_stemTimeScales[stemI];
                stemGains[ 8 + stemI ]        = nextRiff->m_stemGains[stemI];
                stemPtr[ 8 + stemI ]          = nextRiff->m_stemPtrs[stemI];
            }
        }
    };
    decodeTransitionalRiffData();


    const auto segmentLengthInSamples   = currentRiff->m_timingDetails.m_lengthInSamplesPerBar;
          auto segmentSampleStart       = samplePosition % segmentLengthInSamples;

    for ( auto sI = 0U; sI < samplesToWrite; sI++ )
    {
        const uint64_t sample = samplePosition + sI;

        // get sample position in context of the riff
        uint64_t riffSample   = riffWrappedSampleStart[0] + sI;
        if ( riffSample >= riffLengthInSamples[0] )
            riffSample -= riffLengthInSamples[0];

        while ( segmentSampleStart >= segmentLengthInSamples )
        {
            segmentSampleStart -= segmentLengthInSamples;

            m_riffPlaybackBar++;
            if ( m_riffPlaybackBar >= currentRiff->m_timingDetails.m_barCount )
                m_riffPlaybackBar = 0;
        }

        const uint64_t segmentSample = segmentSampleStart++;


        if ( segmentSample == 0 )
        {
//            blog::mix( "[ Edge ] Bar {}", m_riffPlaybackBar + 1 );

            const bool isEvenBarNumber = ( m_riffPlaybackBar & 1 ) == 0;
            const bool shouldTriggerTransition =
                // any bar
                ( m_progression.m_triggerPoint == ProgressionConfiguration::TriggerPoint::AnyBarStart ) ||
                // 0, 2, 4, .. 
                ( m_progression.m_triggerPoint == ProgressionConfiguration::TriggerPoint::AnyEvenBarStart && isEvenBarNumber ) ||
                // next riff is bar 0
                ( m_progression.m_triggerPoint == ProgressionConfiguration::TriggerPoint::NextRiffStart && m_riffPlaybackBar == 0 );

            if ( shouldTriggerTransition )
            {
                bool allowedToTrigger = true;

                if ( isRepComPaused() && m_repcomPausedOnBar != m_riffPlaybackBar )
                    allowedToTrigger = false;

                if ( allowedToTrigger && checkForAndDequeueNextRiff() )
                {
                    if ( riffUnpackRequired )
                        decodeForegroundRiffData();
                    decodeTransitionalRiffData();
                    notifyRepComOfActivity( sI );
                }
            }

            if ( m_multiTrackRecording )
            {
                m_repcomRepeatBar++;

                const bool repComTriggerPause = isRepComEnabled() &&
                                                m_repcomRepeatBar >= currentRiff->m_timingDetails.m_longestStemInBars;

                if (    repComTriggerPause
                     && m_repcomState       == RepComState::Unpaused
                     && m_riffNext          == nullptr
                     && m_transitionValue   == 0 )
                {
                    blog::mix( "[ REPCOM ] Pausing @ bar {}, sample offset {}", m_riffPlaybackBar, sI );

                    m_repcomPausedOnBar = m_riffPlaybackBar;

                    m_repcomSampleStart = 0;
                    m_repcomSampleEnd   = sI;
                    m_repcomState       = RepComState::SampleFragmentAndPause;
                }
            }
        }
        if ( riffSample == 0 )
        {
            blog::mix( "[ Edge ] Riff" );

            // multitrack recording waits till the start of a riff to begin
            if ( m_multiTrackWaitingToRecordOnRiffEdge )
            {
                // begin writing out data
                m_multiTrackRecording                 = true;
                m_multiTrackWaitingToRecordOnRiffEdge = false;
                m_multiTrackInFlux                    = false;

                // reset repcom stats
                m_repcomRepeatBar                     = 0;

                blog::mix( "[ Multitrack ] Recording ..." );
            }
        }


        for ( auto stemI = 0U; stemI < 8; stemI++ )
        {
            const endlesss::live::Stem* stemInst = stemPtr[stemI];
            const float       stemGain = stemGains[stemI];

            if ( stemInst == nullptr || stemInst->hasFailed() )
            {
                m_mixChannelLeft[stemI][sI] = 0;
                m_mixChannelRight[stemI][sI] = 0;
                continue;
            }

            const auto sampleCount = stemInst->m_sampleCount;
            uint64_t finalSampleIdx = riffSample;

            if (stemTimeStretch[stemI] != 1.0f)
            {
                double scaleSample = (double)finalSampleIdx * stemTimeStretch[stemI];
                finalSampleIdx = (uint64_t)scaleSample;
            }
            finalSampleIdx %= sampleCount;


            const uint64_t bitBlock = finalSampleIdx >> 6;
            const uint64_t bitBit   = finalSampleIdx - (bitBlock << 6);

            stemHasBeat[stemI] |= ( stemInst->m_sampleBeat[bitBlock] >> bitBit ) != 0;
            stemEnergy[stemI]   = std::max( stemEnergy[stemI], stemInst->m_sampleEnergy[finalSampleIdx] );

            m_mixChannelLeft[stemI][sI]  = stemInst->m_channel[0][finalSampleIdx] * stemGain;
            m_mixChannelRight[stemI][sI] = stemInst->m_channel[1][finalSampleIdx] * stemGain;
        }

        if ( m_transitionValue > 0 )
        {
            riffSample = riffWrappedSampleStart[1] + sI;
            if ( riffSample >= riffLengthInSamples[1] )
                riffSample -= riffLengthInSamples[1];

            for ( auto stemI = 0U; stemI < 8; stemI++ )
            {
                const endlesss::live::Stem* stemInst = stemPtr[ 8 + stemI ];
                const float       stemGain = stemGains[ 8 + stemI ];
        
                // when transitioning, a missing/muted stem means we need to transition down to silence, not just skip entirely
                if ( stemInst == nullptr || stemInst->hasFailed() )
                {
                    m_mixChannelLeft[stemI][sI]     = base::lerp( m_mixChannelLeft[stemI][sI], 0.0f, m_transitionValue );
                    m_mixChannelRight[stemI][sI]    = base::lerp( m_mixChannelRight[stemI][sI], 0.0f, m_transitionValue );

                    continue;
                }
        
                const auto sampleCount = stemInst->m_sampleCount;
                uint64_t finalSampleIdx = riffSample;
        
                if ( stemTimeStretch[ 8 + stemI ] != 1.0f )
                {
                    double scaleSample = (double)finalSampleIdx * stemTimeStretch[ 8 + stemI ];
                    finalSampleIdx = (uint64_t)scaleSample;
                }
                finalSampleIdx %= sampleCount;

                m_mixChannelLeft[stemI][sI]         = base::lerp( m_mixChannelLeft[stemI][sI], stemInst->m_channel[0][finalSampleIdx] * stemGain, m_transitionValue );
                m_mixChannelRight[stemI][sI]        = base::lerp( m_mixChannelRight[stemI][sI], stemInst->m_channel[1][finalSampleIdx] * stemGain, m_transitionValue );
            }
        }
    }

    int32_t simultaneousBeats = 0;
    for ( auto stemI = 0U; stemI < 8; stemI++ )
    {
        if ( stemHasBeat[stemI] )
        {
            m_stemBeats[stemI].m_pulse = 1.0f;
            simultaneousBeats++;
        }

        m_stemEnergy[stemI] = stemEnergy[stemI];
    }
    if ( simultaneousBeats >= 3 )
        m_stemBeatConsensus = 1.0f;


    commit( outputBuffer, outputVolume, samplesToWrite );
}




namespace Providers {

// ---------------------------------------------------------------------------------------------------------------------
struct RiffPercentage : public Fx::Provider
{
    static constexpr size_t UniqueID         = PROVIDER_ID( 'R', 'I', 'F', '%' );
    static constexpr const char* VisibleName = "Riff %";

    const MixEngine*    m_mixer;

    RiffPercentage( const MixEngine* mixerInst )
        : m_mixer( mixerInst )
    {}

    virtual AbilityFlags flags() const { return kUsesRemapping; }

    virtual float generate( const Input& input ) override
    {
        return (float)m_mixer->m_riffPlaybackPercentage;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
struct MixTransition : public Fx::Provider
{
    static constexpr size_t UniqueID         = PROVIDER_ID( 'M', 'X', 'T', 'R' );
    static constexpr const char* VisibleName = "Transit";

    const MixEngine*    m_mixer;

    MixTransition( const MixEngine* mixerInst )
        : m_mixer( mixerInst )
    {}

    virtual AbilityFlags flags() const { return kUsesRemapping; }

    virtual float generate( const Input& input ) override
    {
        return (float)m_mixer->m_transitionValue;
    }
};


} // namespace Providers

// ---------------------------------------------------------------------------------------------------------------------
struct BeamApp : public app::OuroApp
{

    BeamApp()
        : app::OuroApp()
        , m_jamChoiceIndex( 0 )
    {
        Fx::Providers::registerDefaults( m_dataBus.m_providerFactory, m_dataBus.m_providerNames );

        m_lastAutoMemoryReclaimationTime = std::chrono::high_resolution_clock::now();

        m_discordBotUI = std::make_unique<discord::BotWithUI>( *this, m_configDiscord );
    }

    const char* GetAppName() const override { return OUROVEON_BEAM; }
    const char* GetAppNameWithVersion() const override { return (OUROVEON_BEAM " " OUROVEON_BEAM_VERSION); }
    const char* GetAppCacheName() const override { return "beam"; }

    int EntrypointOuro() override;


    // return the Jam CID selected by the modalJamBrowser() UI
    inline const endlesss::types::JamCouchID& getChosenJamDbID() const { return m_jamLibrary.getDatabaseID( m_jamChoiceIndex ); }

    inline void runAutomaticMemoryReclaim()
    {
        auto currentTime        = std::chrono::high_resolution_clock::now();
        auto currentDuration    = std::chrono::duration_cast<std::chrono::milliseconds>( currentTime - m_lastAutoMemoryReclaimationTime );

        if ( currentDuration.count() > 120 * 1000 )
        {
            m_stemCache.prune();
            m_lastAutoMemoryReclaimationTime = currentTime;
        }
    }

protected:

    size_t                                  m_jamChoiceIndex;

    MixEngine::ProgressionConfiguration     m_mixProgressionConfig;
    MixEngine::ProgressionConfiguration     m_mixProgressionConfigCommitted;

    MixEngine::RepComConfiguration          m_repComConfig;


    Fx::DataBus                             m_dataBus;
    Fx::VstStack                            m_vstStack;


    std::chrono::time_point<std::chrono::steady_clock>   m_lastAutoMemoryReclaimationTime;

    std::unique_ptr< discord::BotWithUI >   m_discordBotUI;
};


// ---------------------------------------------------------------------------------------------------------------------
int BeamApp::EntrypointOuro()
{
    std::vector< const char* > unpackedJamNames;
    m_jamLibrary.unpackJamNames( unpackedJamNames );

    if ( !m_stemCache.initialise( m_storagePaths->cacheCommon, m_mdAudio->getSampleRate() ) )
        return -1;





    // create and install the mixer engine
    MixEngine mixEngine( m_mdAudio );
    m_mdAudio->blockUntil( m_mdAudio->installMixer( &mixEngine ) );


    // custom databus providers
    {
        Fx::Providers::registerProvider< Providers::RiffPercentage >(
            m_dataBus.m_providerFactory,
            m_dataBus.m_providerNames,
            [&]() { return new Providers::RiffPercentage( &mixEngine ); } );

        Fx::Providers::registerProvider< Providers::MixTransition >(
            m_dataBus.m_providerFactory,
            m_dataBus.m_providerNames,
            [&]() { return new Providers::MixTransition( &mixEngine ); } );
    }

    // load last databus setup
    m_dataBus.load( m_appConfigPath );

    // management of VSTs
    m_vstStack.load( m_appConfigPath, m_mdAudio, &mixEngine.m_unifiedTimeInfo );


    // == SNOOP ========================================================================================================

    endlesss::Sentinel jamSentinel( m_apiNetworkConfiguration.value() );

    bool        riffSyncInProgress = false;
    endlesss::types::RiffCouchID riffSyncLastSeenId;
    const auto  riffChangeCallback = [&]( const endlesss::types::Jam& trackedJam )
    {
        if ( riffSyncInProgress )
            return;

        riffSyncInProgress = true;

        std::this_thread::sleep_for( 1000ms );

        // get the current riff from the jam
        endlesss::api::pull::LatestRiffInJam latestRiff { m_apiNetworkConfiguration.value(), trackedJam.couchID, trackedJam.displayName };
        if ( !latestRiff.hasLoadedSuccessfully() )
        {
            blog::error::app( "[ SYNC ] failed to read latest riff in jam" );
            riffSyncInProgress = false;
            return;
        }

        endlesss::types::RiffComplete completeRiffData { latestRiff };


        auto riff = std::make_shared<endlesss::live::Riff>( completeRiffData, m_mdAudio->getSampleRate() );
        riff->fetch( m_apiNetworkConfiguration.value(), m_stemCache, m_taskExecutor );

        // check to see if the riff contents actually changed; it might have been other metadata (like chats arriving)
        const auto& syncRiffId          = completeRiffData.riff.couchID;
        const bool  isActualNewRiffData = ( riffSyncLastSeenId != syncRiffId );
        riffSyncLastSeenId = syncRiffId;

        blog::app( "[ SYNC ] last id = [{}], new id = [{}] | {}", riffSyncLastSeenId, syncRiffId, isActualNewRiffData ? "is-new" : "no-new-data" );

        // if new data has arrived, chuck it in the mix
        if ( isActualNewRiffData )
        {
            mixEngine.addNextRiff( riff );  // transfer ownership; otherwise the riff will fall out of scope and be dealloced
        }

        riffSyncInProgress = false;
    };


    // == MAIN LOOP ====================================================================================================

    while ( MainLoopBegin() )
    {
        // process and blank out Exchange data ready to re-write it
        emitAndClearExchangeData();

        mixEngine.mainThreadUpdate( GImGui->IO.DeltaTime, m_endlesssExchange );

        // run modal jam browser window if it's open
        bool modalDisplayJamBrowser = false;
        const char* modalJamBrowserTitle = "Jam Browser";
        modalJamBrowser(
            modalJamBrowserTitle,
            m_jamLibrary, 
            [this]() { return m_jamLibrary.getDatabaseID( m_jamChoiceIndex ); },
            [this]( const endlesss::types::JamCouchID& newJamCID ) { m_jamLibrary.getIndexForDatabaseID( newJamCID, m_jamChoiceIndex ); }
        );


        auto currentRiffPtr = mixEngine.m_riffCurrent;

        // update exchange buffers 
        {
            endlesss::Exchange::fillDetailsFromRiff( m_endlesssExchange, currentRiffPtr, unpackedJamNames[m_jamChoiceIndex] );

            m_endlesssExchange.m_riffBeatSegmentActive = mixEngine.m_riffPlaybackBarSegment;
            m_endlesssExchange.m_riffTransition        = mixEngine.m_transitionValue;
        }

        ImGuiPerformanceTracker();

        m_dataBus.update();
        m_dataBus.imgui();

        m_vstStack.syncToDataBus( m_dataBus );
        m_vstStack.imgui( m_mdAudio, &mixEngine.m_unifiedTimeInfo, *m_mdFrontEnd, m_dataBus );


        m_discordBotUI->imgui( *m_mdFrontEnd );

        {
            ImGui::Begin( "System" );

            const auto panelRegionAvailable = ImGui::GetContentRegionAvail();

            ImGui_AppHeader();

            // expose volume control
            {
                float volumeF = m_mdAudio->getMasterVolume() * 1000.0f;
                if ( ImGui::KnobFloat( "Volume", 24.0f, &volumeF, 0.0f, 1000.0f, 2000.0f ) )
                    m_mdAudio->setMasterVolume( volumeF * 0.001f );
            }
            ImGui::SameLine( 0, 8.0f );
            // button to toggle end-chain mute on audio engine (but leave processing, WAV output etc intact)
            const bool isMuted = m_mdAudio->isMuted();
            {
                const char* muteIcon = isMuted ? ICON_FA_VOLUME_MUTE : ICON_FA_VOLUME_UP;

                {
                    ImGui::Scoped::ToggleButton bypassOn( isMuted );
                    if ( ImGui::Button( muteIcon, ImVec2( 48.0f, 48.0f ) ) )
                        m_mdAudio->toggleMute();
                }
                ImGui::CompactTooltip( "Mute final audio output\nThis does not affect streaming or disk-recording" );
            }


            ImGui::SeparatorBreak();
            ImGui::TextUnformatted( "Disk Recorders" );
            {
                ImGui_DiskRecorder( *m_mdAudio );
                ImGui_DiskRecorder(  mixEngine );
            }

            ImGui::SeparatorBreak();
            {
                const bool isTracking           = jamSentinel.isTrackerRunning();
                const bool isTrackerBroken      = jamSentinel.isTrackerBroken();
                const float chunkyButtonHeight  = 40.0f;

                ImGui::TextUnformatted( "Selected Jam :" );

                ImGui::PushItemWidth( panelRegionAvailable.x );

                {
                    ImGui::TextColored( 
                        isTracking ? ImGui::GetStyleColorVec4( ImGuiCol_PlotHistogram ) : ImGui::GetStyleColorVec4( ImGuiCol_SliderGrabActive ),
                        unpackedJamNames[m_jamChoiceIndex] );

                    ImGui::Spacing();
                }
                {
                    ImGui::BeginDisabledControls( isTracking );

                    if ( ImGui::Button( ICON_FA_TH " Browse ...", ImVec2( panelRegionAvailable.x, chunkyButtonHeight ) ) )
                        modalDisplayJamBrowser = true;

                    ImGui::EndDisabledControls( isTracking );
                }

                if ( isTracking )
                {
                    if ( isTrackerBroken )
                    {
                        if ( ImGui::Button( ICON_FA_EXCLAMATION_TRIANGLE " Tracker Failed", ImVec2( panelRegionAvailable.x, chunkyButtonHeight ) ) )
                        {
                            jamSentinel.startTracking( riffChangeCallback, { getChosenJamDbID(), unpackedJamNames[m_jamChoiceIndex] } );
                        }
                    }
                    else
                    {
                        ImGui::Scoped::ToggleButton toggled( true, true );
                        if ( ImGui::Button( ICON_FA_STOP_CIRCLE " Tracking Changes", ImVec2( panelRegionAvailable.x, chunkyButtonHeight ) ) )
                        {
                            jamSentinel.stopTracking();
                        }
                    }
                }
                else
                {
                    if ( ImGui::Button( ICON_FA_PLAY_CIRCLE " Begin Tracking ", ImVec2( panelRegionAvailable.x, chunkyButtonHeight ) ) )
                    {
                        jamSentinel.startTracking( riffChangeCallback, { getChosenJamDbID(), unpackedJamNames[m_jamChoiceIndex] } );
                    }
                }
            }

            ImGui::SeparatorBreak();

            ImGui::TextUnformatted( "Beat Decay Rate" );
            ImGui::DragFloat( "##beat_decay", &mixEngine.m_stemBeatRate, 0.01f, 0.1f, 25.0f );


            ImGui::PopItemWidth();
            ImGui::End();
        }
        {
            ImGui::Begin( "Progression Settings" );
            const auto panelRegionAvailable = ImGui::GetContentRegionAvail();

            ImGui::Columns( 2, nullptr, true );
            ImGui::SetColumnWidth( 0, panelRegionAvailable.x * 0.5f );
            {
                ImGui::TextUnformatted( ICON_FA_RANDOM " Riff Blending" );
                ImGui::Spacing();
                ImGui::Spacing();

                ImGui::PushItemWidth( 180.0f );

                ImGui::Combo( " Trigger Point", (int32_t*)&m_mixProgressionConfig.m_triggerPoint, &MixEngine::ProgressionConfiguration::triggerPointGetter, nullptr, MixEngine::ProgressionConfiguration::cTriggerPointCount );
                ImGui::Combo( " Transition Time",  (int32_t*)&m_mixProgressionConfig.m_blendTime, &MixEngine::ProgressionConfiguration::blendTimeGetter,    nullptr, MixEngine::ProgressionConfiguration::cBlendTimeCount );
                //ImGui::Checkbox( "Empty Riff Queue On Transition", &m_mixProgressionConfig.m_greedyMode );

                const bool progressionIsUpToDate = ( m_mixProgressionConfigCommitted == m_mixProgressionConfig );
                ImGui::BeginDisabledControls( progressionIsUpToDate );
                {
                    ImGui::Scoped::ToggleButton highlightButton( !progressionIsUpToDate, true );
                    if ( ImGui::Button( " Activate " ) )
                    {
                        mixEngine.updateProgressionConfiguration( &m_mixProgressionConfig );
                        m_mixProgressionConfigCommitted = m_mixProgressionConfig;
                    }
                }
                ImGui::EndDisabledControls( progressionIsUpToDate );
            }
            ImGui::NextColumn();
            {
                ImGui::TextUnformatted( ICON_FA_STREAM " Repetition Compression" );
                ImGui::Spacing();
                ImGui::Spacing();

                const bool recordingFreezeRepCom = mixEngine.isRecording();
                ImGui::BeginDisabledControls( recordingFreezeRepCom );
                {
                    if ( ImGui::Checkbox( "Enabled", &m_repComConfig.m_enable ) )
                    {
                        mixEngine.updateRepComConfiguration( &m_repComConfig );
                    }
                }
                ImGui::EndDisabledControls( recordingFreezeRepCom );

                if ( mixEngine.isRepComEnabled() )
                {
                    ImGui::Spacing();
                    if ( recordingFreezeRepCom )
                        ImGui::TextWrapped( "Cannot activate or deactivate while recording is underway" );
                    else
                        ImGui::TextWrapped( "Multitrack recording will pause when bars repeat too many times, unpausing when new transitions occur" );
                }
            }
            ImGui::Columns( 1 );

            ImGui::End();
        }
        {
            ImGui::Begin( "Current Riff" );
            const auto panelRegionAvailable = ImGui::GetContentRegionAvail();

            ImGui::Columns( 2, nullptr, false );
            ImGui::SetColumnWidth( 0, panelRegionAvailable.x * 0.55f );
            ImGui::SetColumnWidth( 1, panelRegionAvailable.x * 0.45f );

            const auto* currentRiff = currentRiffPtr.get();

            ImGui::TextUnformatted( "Current Jam" );
            if ( currentRiff )
            {
                // jam title
                ImGui::PushStyleColor( ImGuiCol_Text, 0xffffffffU );
                ImGui::PushFont( m_mdFrontEnd->getFont( app::module::Frontend::FontChoice::MediumTitle ) );
                ImGui::TextUnformatted( currentRiff->m_uiJamUppercase.c_str() );
                ImGui::PopFont();
                ImGui::PopStyleColor();

                // jam state (BPM et al)
                ImGui::TextUnformatted( currentRiff->m_uiDetails.c_str() );

                // jam changes
                ImGui::Spacing();
                ImGui::Text( "Last Change : %s, %s", currentRiff->m_uiIdentity.c_str(), currentRiff->m_uiTimestamp.c_str() );
            }
            else
            {
                // empty title
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetColorU32( ImGuiCol_SeparatorHovered ) );
                ImGui::PushFont( m_mdFrontEnd->getFont( app::module::Frontend::FontChoice::MediumTitle ) );
                ImGui::TextUnformatted( "NO CONNECTION");
                ImGui::PopFont();
                ImGui::PopStyleColor();

                // empty state
                ImGui::TextUnformatted( "" );

                // empty changes
                ImGui::Spacing();
                ImGui::TextUnformatted( "" );
            }

            ImGui::NextColumn();
            ImGui::TextUnformatted( ICON_FA_RANDOM " Active Riff Blending" );
            ImGui::Spacing();
            ImGui::Spacing();
            {
                const auto currentLineHeight = ImGui::GetTextLineHeight();
                const auto progressBarHeight = ImVec2( -1.0f, currentLineHeight * 1.25f );

                {
                    const auto* progTrigger = MixEngine::ProgressionConfiguration::getTriggerPointName( mixEngine.m_progression.m_triggerPoint );
                    const auto* progBlend   = MixEngine::ProgressionConfiguration::getBlendTimeName( mixEngine.m_progression.m_blendTime );

                    ImGui::Text( "Trigger %s, %s %s",
                        progTrigger,
                        progBlend,
                        mixEngine.m_progression.m_greedyMode ? "(Leap)" : "(Sequential)" );
                }

                const bool itemsInRiffQueue = ( mixEngine.m_riffQueue.size_approx() > 0 );
                {
                    ImGui::PushStyleColor( ImGuiCol_PlotHistogram, ImGui::GetStyleColorVec4( ImGuiCol_NavHighlight ) );

                    if ( itemsInRiffQueue )
                        ImGui::ProgressBar( mixEngine.m_transitionValue, progressBarHeight, " Transition Scheduled ... " );
                    else if ( mixEngine.m_transitionValue > 0 )
                        ImGui::ProgressBar( mixEngine.m_transitionValue, progressBarHeight, " Transitioning ... " );
                    else
                        ImGui::ProgressBar( mixEngine.m_transitionValue, progressBarHeight, "" );

                    ImGui::PopStyleColor();
                }

                ImGui::Spinner( "##syncing", riffSyncInProgress, currentLineHeight * 0.4f, 3.0f, ImGui::GetColorU32( ImGuiCol_Text ) );
                ImGui::SameLine();
                if ( riffSyncInProgress )
                    ImGui::TextUnformatted( " Fetching Stems ..." );
                else
                    ImGui::TextUnformatted( "" );
            }
            ImGui::Columns( 1 );

            if ( currentRiffPtr != nullptr )
            {
                {
                    ImGui::Spacing();
                    ImGui::Spacing();
                    ImGui::Spacing();
                    ImGui::BeatSegments( "##beats", m_endlesssExchange.m_riffBeatSegmentCount, m_endlesssExchange.m_riffBeatSegmentActive );
                    ImGui::Spacing();
                }

                if ( ImGui::BeginTable( "##stem_stack", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg ) )
                {
                    ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_ResizeGripHovered ) );
                    ImGui::TableSetupColumn( "User",    ImGuiTableColumnFlags_WidthFixed, 125.0f );
                    ImGui::TableSetupColumn( "Gain",    ImGuiTableColumnFlags_WidthFixed, 45.0f );
                    ImGui::TableSetupColumn( "Stretch", ImGuiTableColumnFlags_WidthFixed, 60.0f );
                    ImGui::TableSetupColumn( "Rep",     ImGuiTableColumnFlags_WidthFixed, 30.0f );
                    ImGui::TableSetupColumn( "Rate",    ImGuiTableColumnFlags_WidthFixed, 60.0f );
                    ImGui::TableSetupColumn( "Instr",   ImGuiTableColumnFlags_WidthFixed, 60.0f );
                    ImGui::TableSetupColumn( "Preset",  ImGuiTableColumnFlags_WidthFixed, 125.0f );
                    ImGui::TableSetupColumn( "Beat",    ImGuiTableColumnFlags_WidthFixed, 125.0f );
                    ImGui::TableSetupColumn( "Energy",  ImGuiTableColumnFlags_None );
                    ImGui::TableHeadersRow();
                    ImGui::PopStyleColor();

                    for ( size_t sI=0; sI<8; sI++ )
                    {
                        const endlesss::live::Stem* stem = currentRiff->m_stemPtrs[sI];

                        if ( stem == nullptr )
                        {
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( "" );
                        }
                        else
                        {
                            const auto& riffDocument = currentRiff->m_riffData;
                            const auto  bpsDiff      = riffDocument.riff.BPS / stem->m_data.BPS;

                            ImGui::TableNextColumn(); ImGui::TextUnformatted( stem->m_data.user.c_str() );
                            ImGui::TableNextColumn(); ImGui::Text( "%.2f", m_endlesssExchange.m_stemGain[sI] );
                            ImGui::TableNextColumn(); ImGui::Text( "%.2f", bpsDiff );
                            ImGui::TableNextColumn(); ImGui::Text( "%ix", currentRiff->m_stemRepetitions[sI] );
                            ImGui::TableNextColumn(); ImGui::Text( "%i", stem->m_data.sampleRate );
                            ImGui::TableNextColumn(); ImGui::Text( stem->m_data.getInstrumentName() );
                            ImGui::TableNextColumn(); ImGui::TextUnformatted( stem->m_data.preset.c_str() );

                            ImGui::PushStyleColor( ImGuiCol_PlotHistogram, m_endlesssExchange.m_stemColour[sI] );
                            ImGui::TableNextColumn(); 
                                ImGui::ProgressBar( m_endlesssExchange.m_stemPulse[sI], ImVec2(-1, 8.0f), "");
                            ImGui::PopStyleColor();

                            ImGui::TableNextColumn();
                                ImGui::ProgressBar( m_endlesssExchange.m_stemEnergy[sI], ImVec2(-1, 8.0f), "");
                        }
                    }

                    ImGui::EndTable();

                    ImGui::Spacing();
                    ImGui::Spacing();
                    ImGui::ProgressBar( (float)mixEngine.m_riffPlaybackPercentage, ImVec2( -1, 3.0f ), "" );
                    ImGui::BeatSegments( "##bars_play", currentRiff->m_timingDetails.m_barCount, mixEngine.m_riffPlaybackBar, 3.0f, ImGui::GetColorU32( ImGuiCol_PlotHistogram ) );

                    if ( mixEngine.isRepComEnabled() && 
                         mixEngine.isRecording() )
                    {
                        ImGui::Spacing();
                        ImGui::BeatSegments( "##bars_sync", currentRiff->m_timingDetails.m_barCount, mixEngine.getBarPausedOn(), 3.0f, ImGui::GetColorU32( ImGuiCol_HeaderActive ) );
                        ImGui::Spacing();

                        ImGui::Text( "Repetition Counter : %u | Last Pause Bar : %i", mixEngine.getBarRepetitions(), mixEngine.getBarPausedOn() );
                    }
                }
            }
            ImGui::End();
        }

        if ( modalDisplayJamBrowser )
            ImGui::OpenPopup( modalJamBrowserTitle );

        runAutomaticMemoryReclaim();

        MainLoopEnd( nullptr, nullptr );
    }

    m_discordBotUI.reset();

    m_mdAudio->blockUntil( m_mdAudio->installMixer( nullptr ) );

    m_dataBus.save( m_appConfigPath );
    m_vstStack.save( m_appConfigPath );

    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
int main( int argc, char** argv )
{
    BeamApp beam;
    const int result = beam.Run();
    if ( result != 0 )
        app::Core::waitForConsoleKey();

    return result;
}