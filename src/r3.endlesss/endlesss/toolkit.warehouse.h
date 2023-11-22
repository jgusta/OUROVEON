//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  \\ harry denholm \\ ishani            ishani.org/shelf/ouroveon/
//
//  
//

#pragma once

#include "base/text.h"
#include "spacetime/chronicle.h"
#include "endlesss/ids.h"
#include "endlesss/api.h"

namespace app { struct StoragePaths; }

namespace endlesss {

namespace cache { struct Jams; }

namespace toolkit {

// ---------------------------------------------------------------------------------------------------------------------
//
struct Warehouse
{
    using Instance = std::unique_ptr< Warehouse >;

    struct _change_index {};
    using ChangeIndex = base::id::Simple<_change_index, uint32_t, 1, 0>;

    struct ContentsReport
    {
        std::vector< types::JamCouchID >    m_jamCouchIDs;
        std::vector< int64_t >              m_populatedRiffs;
        std::vector< int64_t >              m_unpopulatedRiffs;
        std::vector< int64_t >              m_populatedStems;
        std::vector< int64_t >              m_unpopulatedStems;
        std::vector< bool >                 m_awaitingInitialSync;  // initial data set has not yet arrived, consider in flux
    };
    using ContentsReportCallback = std::function<void( const ContentsReport& report )>;

    // when there is a failure to validate a stem during riff fetching, the stem is cut out of the resulting database
    // and an entry is logged in a ledger, stashing the stem ID with any notes / tags about why we ignore it
    enum class StemLedgerType
    {
        MISSING_OGG         = 1,            // ogg data vanished; this is mostly due to the broken beta that went out
        DAMAGED_REFERENCE   = 2,            // sometimes chat messages (?!) or other riffs (??) seem to have been stored as stem CouchIDs 
        REMOVED_ID          = 3             // just .. gone. couch ID not found. unrecoverable
    };

    static const std::string_view getStemLedgerTypeAsString( const StemLedgerType ledgerType )
    {
        switch ( ledgerType )
        {
            case StemLedgerType::MISSING_OGG:       return "MISSING_OGG";
            case StemLedgerType::DAMAGED_REFERENCE: return "DAMAGED_REFERENCE";
            case StemLedgerType::REMOVED_ID:        return "REMOVED_ID";
            default:
                return "UNKNOWN";
        }
    }


    // SoA extraction of a set of riff data
    struct JamSlice
    {
        using StemUserHashes = std::array< uint64_t, 8 >;

        DECLARE_NO_COPY_NO_MOVE( JamSlice );

        JamSlice() = delete;
        JamSlice( const types::JamCouchID& jamID, const size_t elementsToReserve )
        {
            reserve( elementsToReserve );
        }

        // per-riff information
        std::vector< types::RiffCouchID >           m_ids;
        std::vector< spacetime::InSeconds >         m_timestamps;
        std::vector< uint64_t >                     m_userhash;
        std::vector< uint8_t >                      m_roots;
        std::vector< uint8_t >                      m_scales;
        std::vector< float >                        m_bpms;

        std::vector< StemUserHashes >               m_stemUserHashes;

        // riff-adjacency information
        std::vector< int32_t >                      m_deltaSeconds;
        std::vector< int8_t >                       m_deltaStem;

    protected:

        inline void reserve( const size_t elements )
        {
            m_ids.reserve( elements );
            m_timestamps.reserve( elements );
            m_userhash.reserve( elements );
            m_roots.reserve( elements );
            m_scales.reserve( elements );
            m_bpms.reserve( elements );

            m_stemUserHashes.reserve( elements );

            m_deltaSeconds.reserve( elements );
            m_deltaStem.reserve( elements );

            std::size_t memoryUsageEstimation = 0;
            {
                memoryUsageEstimation += sizeof( types::RiffCouchID ) * elements;
                memoryUsageEstimation += sizeof( spacetime::InSeconds ) * elements;
                memoryUsageEstimation += sizeof( uint64_t ) * elements;
                memoryUsageEstimation += sizeof( uint8_t ) * elements;
                memoryUsageEstimation += sizeof( uint8_t ) * elements;
                memoryUsageEstimation += sizeof( float ) * elements;

                memoryUsageEstimation += sizeof( uint64_t ) * elements * 8;

                memoryUsageEstimation += sizeof( int32_t ) * elements;
                memoryUsageEstimation += sizeof( uint8_t ) * elements;
            }

            blog::app( FMTX( "JamSlice SOA array allocation {}" ), base::humaniseByteSize( "~", memoryUsageEstimation ) );
        }
    };
    using JamSlicePtr = std::unique_ptr<JamSlice>;
    using JamSliceCallback = std::function<void( const types::JamCouchID& jamCouchID, JamSlicePtr&& resultSlice )>;


    struct ITask;
    struct INetworkTask;

    using WorkUpdateCallback    = std::function<void( const bool tasksRunning, const std::string& currentTask ) >;

    using TagBatchingCallback   = std::function<void( bool bBatchUpdateBegun )>;
    using TagUpdateCallback     = std::function<void( const endlesss::types::RiffTag& tagData )>;
    using TagRemovedCallback    = std::function<void( const endlesss::types::RiffCouchID& tagRiffID )>;


    Warehouse( const app::StoragePaths& storagePaths, api::NetConfiguration::Shared& networkConfig, base::EventBusClient eventBus );
    ~Warehouse();

    static std::string  m_databaseFile;
    using SqlDB = sqlite::Database<m_databaseFile>;

    // -----------------------------------------------------------------------------------------------------------------


    // inject callbacks to client code that are used to report what the warehouse is currently doing, or to receive
    // larger data packets like the full contents report
    void setCallbackWorkReport( const WorkUpdateCallback& cb );
    void setCallbackContentsReport( const ContentsReportCallback& cb );

    void setCallbackTagUpdate( const TagUpdateCallback& cbUpdate, const TagBatchingCallback& cbBatch );
    void setCallbackTagRemoved( const TagRemovedCallback& cb );

    void clearAllCallbacks();


    // manually enqueue a contents report event, refreshing anything that responds to that callback
    void requestContentsReport();


    // -----------------------------------------------------------------------------------------------------------------
    // Jam Naming

    // upsert a jamID (band####) -> display name record
    void upsertSingleJamIDToName( const endlesss::types::JamCouchID& jamCID, const std::string& displayName );

    // upsert all jamID -> display name records from the given jam cache, keeping the warehouse name lookup fresh
    void upsertJamDictionaryFromCache( const cache::Jams& jamCache );

    // fetch a list of all the JamID -> display name rows we have as a lookup table
    void extractJamDictionary( types::JamIDToNameMap& jamDictionary ) const;


    // -----------------------------------------------------------------------------------------------------------------
    // Jam Synchronisation

    // insert a jam ID into the warehouse to be queried and filled
    void addOrUpdateJamSnapshot( const types::JamCouchID& jamCouchID );

    // fetch the full stack of data for a given jam
    void addJamSliceRequest( const types::JamCouchID& jamCouchID, const JamSliceCallback& callbackOnCompletion );

    // erase the given jam from the warehouse database entirely
    void requestJamPurge( const types::JamCouchID& jamCouchID );

    // erase all unfilled riffs, effectively cutting short any in-progress sync
    void requestJamSyncAbort( const types::JamCouchID& jamCouchID );


    // -----------------------------------------------------------------------------------------------------------------
    // Jam Export / Import

    void requestJamExport( const types::JamCouchID& jamCouchID, const fs::path exportFolder, std::string_view jamTitle );
    void requestJamImport( const fs::path pathToData );


    // -----------------------------------------------------------------------------------------------------------------
    // Riff Resolution

    // instead of hitting the Endlesss network, the warehouse may be able to fill in all the data required to 
    // bring a riff online; returns true if that was the case
    bool fetchSingleRiffByID( const endlesss::types::RiffCouchID& riffID, endlesss::types::RiffComplete& result ) const;

    // change a specific stem entry in a riff, also creating an empty stem record if one doesn't exist;
    // used to modify existing riff structures if we find invalid mismatches from the server data
    bool patchRiffStemRecord(
        const types::JamCouchID& jamCouchID,
        const endlesss::types::RiffCouchID& riffID,
        const int32_t stemIndex,    // 0-base stem index to modify
        const endlesss::types::StemCouchID& newStemID );


    // -----------------------------------------------------------------------------------------------------------------
    // Stem Operations

    // given N stems, return a matching vector of origin jam IDs for each; if no jam ID can be determined, an empty ID is stored
    bool batchFindJamIDForStem( const endlesss::types::StemCouchIDs& stems, endlesss::types::JamCouchIDs& result ) const;

    // populate the output vector with every stem associated with a jam (for example, for precatching them all into the cache)
    // also returns the summed FileLength values, in case disk storage estimation or reporting is useful
    bool fetchAllStemsForJam( const types::JamCouchID& jamCouchID, endlesss::types::StemCouchIDs& result, std::size_t& estimatedTotalFileSize ) const;

    // resolve a single stem data block from the database, if we can find it; returns false if we didn't
    bool fetchSingleStemByID( const types::StemCouchID& stemCouchID, endlesss::types::Stem& result ) const;

    // do you want all the stems? all of them? damn son alright
    bool fetchAllStems( endlesss::types::StemCouchIDs& result, std::size_t& estimatedTotalFileSize ) const;


    // see if we have any notes for a stem ID (if it was removed from the database during a sync for some reason)
    // returns false if we have no record for this stem ID
    bool getNoteTypeForStem( const types::StemCouchID& stemCID, StemLedgerType& typeResult );


    // -----------------------------------------------------------------------------------------------------------------
    // Tags

    // add or update a jam:riff tag
    void upsertTag( const endlesss::types::RiffTag& tag );

    // delete the tag from the database
    void removeTag( const endlesss::types::RiffTag& tag );

    // returns true if the given riff has tag data, optionally also returning the tag data if a structure is passed in
    bool isRiffTagged( const endlesss::types::RiffCouchID& riffID, endlesss::types::RiffTag* tagOutput = nullptr ) const;

    // get the current set of tags for a jam; returns the size of outputTags on return
    std::size_t fetchTagsForJam( const endlesss::types::JamCouchID& jamCID, std::vector<endlesss::types::RiffTag>& outputTags ) const;

    // set all the specific tag data in a single transaction
    void batchUpdateTags( const std::vector<endlesss::types::RiffTag>& inputTags );

    // delete all tags for a jam as a batch operation
    void batchRemoveAllTags( const endlesss::types::JamCouchID& jamCID );


    // -----------------------------------------------------------------------------------------------------------------

    ouro_nodiscard ChangeIndex getChangeIndexForJam( const endlesss::types::JamCouchID& jamID ) const;

    // control background task processing; pausing will stop any new work from being generated
    void workerTogglePause();
    ouro_nodiscard bool workerIsPaused() const { return m_workerThreadPaused; }

    // passing in the NetConfiguration for API access to Endlesss is optional; users should not enqueue tasks
    // that require it if it isn't present (and tasks will check and bail in error)
    ouro_nodiscard bool hasFullEndlesssNetworkAccess() const { return m_networkConfiguration->hasAccess( api::NetConfiguration::Access::Authenticated ); }

protected:

    using ChangeIndexMap = absl::flat_hash_map< ::endlesss::types::JamCouchID, ChangeIndex >;

    friend ITask;
    struct TaskSchedule;

    void threadWorker();

    void incrementChangeIndexForJam( const ::endlesss::types::JamCouchID& jamID );


    // handle riff tag actions, do database operations to add/remove as requested
    void event_RiffTagAction( const events::RiffTagAction* eventData );
    base::EventListenerID                   m_eventLID_RiffTagAction = base::EventListenerID::invalid();


    api::NetConfiguration::Shared           m_networkConfiguration;
    base::EventBusClient                    m_eventBusClient;

    std::unique_ptr<TaskSchedule>           m_taskSchedule;

    ChangeIndexMap                          m_changeIndexMap;

    WorkUpdateCallback                      m_cbWorkUpdate              = nullptr;
    WorkUpdateCallback                      m_cbWorkUpdateToInstall     = nullptr;
    ContentsReportCallback                  m_cbContentsReport          = nullptr;
    ContentsReportCallback                  m_cbContentsReportToInstall = nullptr;
    TagUpdateCallback                       m_cbTagUpdate;
    TagBatchingCallback                     m_cbTagBatching;
    TagRemovedCallback                      m_cbTagRemoved;
    std::mutex                              m_cbMutex;

    std::unique_ptr<std::thread>            m_workerThread;
    std::atomic_bool                        m_workerThreadAlive;
    std::atomic_bool                        m_workerThreadPaused;
};

} // namespace toolkit
} // namespace endlesss
